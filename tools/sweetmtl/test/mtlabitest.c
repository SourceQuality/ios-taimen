/*
 * mtlabitest: the bridge's object ABI (NativeMetalObjects.h, what the Objective-C frontend calls) on sweetmtl-runtime,
 * beyond the self-test: device answers, textures (CPU access, sampling, storage), samplers, blits, indexed draws,
 * function constants, threadgroup memory, vertex attributes, events, shared events and listeners, fences, handlers and
 * resource tracking.
 *
 *   mtlabitest [AIR_DIR]
 *
 * AIR_DIR holds extra fixtures assembled from metal2vulkan's validation fixtures (host-check.sh makes them):
 * stage_io.air, sampling.air, threadgroup_word.air. Each check prints one line:
 *   ok        it ran and what it computed is right (CPU-visible results are checkable on drm-shim too)
 *   ran       it ran (recorded, compiled, submitted); its GPU-written result is not checkable here (drm-shim)
 *   not yet   the runtime refused it as not yet implemented (the reason follows)
 *   FAIL      anything else
 * The exit status counts FAIL lines.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "NativeMetalObjects.h"

/* The bridge's self-test links against it (mtlbridgetest.c says why); this tool never calls it. */
void sys_dcache_flush(void *start, size_t len)
{
	(void)start;
	(void)len;
}

static int fails, shim;
static const char *air_dir;

static void line(const char *verdict, const char *what, const char *detail)
{
	printf("%-8s %s%s%s\n", verdict, what, detail && *detail ? ": " : "", detail ? detail : "");
	if (!strcmp(verdict, "FAIL"))
		fails++;
	fflush(stdout);
}

/* a failed call: "not yet" where the runtime says so, else FAIL */
static void refused(const char *what)
{
	const char *why = nmb_last_error();
	line(strstr(why, "not yet") || strstr(why, "NotYet") ? "not yet" : "FAIL", what, why);
}

static void gpu_result(const char *what, int bad, const char *detail)
{
	if (shim)
		line("ran", what, "result not checkable on drm-shim");
	else
		line(bad ? "FAIL" : "ok", what, detail);
}

/* Metal's raw values used below */
enum {
	PF_RGBA8 = 70, PF_RGBA32U = 123, PF_D32F = 252,
	TT_2D = 2, TU_READ = 1, TU_WRITE = 2, TU_RT = 4,
	SM_SHARED = 0, SM_PRIVATE = 2 << 4,
	LOAD_CLEAR = 2, STORE_STORE = 1,
	PRIM_TRIANGLE = 3,
	IDX_U16 = 0,
	CMP_LESS = 1, CMP_ALWAYS = 7,
	VF_FLOAT3 = 30, VF_HALF2 = 25, VF_USHORT2 = 13, VF_UCHAR4 = 3,
	DT_INT = 29,
};

static NmbDevice *dev;
static NmbQueue *queue;

static NmbLibrary *library_file(const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", air_dir ? air_dir : ".", name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *bytes = malloc(n);
	if (fread(bytes, 1, n, f) != (size_t)n)
		n = 0;
	fclose(f);
	NmbLibrary *lib = n ? nmb_library_new(dev, bytes, n, true) : NULL;
	free(bytes);
	return lib;
}

static NmbTexture *texture(uint64_t format, uint64_t w, uint64_t h, uint64_t levels, uint64_t usage, uint64_t options)
{
	struct NmbTextureDescriptor d = {TT_2D, format, w, h, 1, levels, 1, 1, usage, options};
	return nmb_texture_new(dev, &d);
}

static bool finish(NmbCommandBuffer *cb, const char *what)
{
	if (!nmb_command_buffer_commit(cb)) {
		refused(what);
		return false;
	}
	if (nmb_command_buffer_wait(cb, 10000) != 0) {
		line("FAIL", what, "no completion in 10 s");
		return false;
	}
	if (nmb_command_buffer_status(cb) != 4) {
		char e[256] = "";
		nmb_command_buffer_error(cb, e, sizeof(e));
		line(strstr(e, "not yet") ? "not yet" : "FAIL", what, e);
		return false;
	}
	return true;
}

static void device_answers(void)
{
	char name[128] = "", vendor[64] = "";
	nmb_device_name(dev, name, sizeof(name));
	nmb_device_vendor_name(dev, vendor, sizeof(vendor));
	uint64_t t[3] = {0};
	nmb_device_max_threads_per_threadgroup(dev, t);
	unsigned answered = 0, none = 0;
	uint32_t kind;
	for (size_t i = 0; nmb_device_answer_selector(i, &kind); i++) {
		const char *sel = nmb_device_answer_selector(i, &kind);
		uint64_t v = 0, arg = 0;
		if (!strcmp(sel, "supportsFamily:"))
			arg = 1003;
		else if (!strcmp(sel, "supportsTextureSampleCount:"))
			arg = 4;
		else if (!strcmp(sel, "minimumLinearTextureAlignmentForPixelFormat:"))
			arg = PF_RGBA8;
		if (nmb_device_answer(dev, sel, arg, &v))
			answered++;
		else
			none++;
	}
	char detail[256];
	snprintf(detail, sizeof(detail), "%s (%s), max threads %llux%llux%llu, %u selectors answered, %u without an answer",
	         name, vendor, (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2], answered, none);
	line(answered ? "ok" : "FAIL", "device answers", detail);
	uint64_t row = nmb_device_linear_bytes_per_row(dev, PF_RGBA8, 61);
	snprintf(detail, sizeof(detail), "RGBA8 x61 -> %llu bytes", (unsigned long long)row);
	line(row == 256 ? "ok" : "FAIL", "linear bytes per row", detail);
}

static void texture_cpu(void)
{
	NmbTexture *t = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	if (!t) {
		refused("texture replace/get bytes");
		return;
	}
	uint8_t in[8 * 8 * 4], out[8 * 8 * 4];
	for (int i = 0; i < (int)sizeof(in); i++)
		in[i] = (uint8_t)(i * 7 + 3);
	uint64_t region[6] = {0, 0, 0, 8, 8, 1};
	memset(out, 0, sizeof(out));
	if (!nmb_texture_replace(t, region, 0, 0, in, 32, 0, sizeof(in)) ||
	    !nmb_texture_get_bytes(t, out, 32, 0, sizeof(out), region, 0, 0)) {
		refused("texture replace/get bytes");
	} else {
		line(memcmp(in, out, sizeof(in)) ? "FAIL" : "ok", "texture replace/get bytes (CPU, 8x8 RGBA8)", NULL);
	}
	nmb_texture_release(t);
}

static void sampled_render(NmbLibrary *lib)
{
	NmbFunction *v = nmb_function_new(lib, "colored_vertex"), *f = nmb_function_new(lib, "sampled_fragment");
	struct NmbRenderPipelineDescriptor d;
	memset(&d, 0, sizeof(d));
	d.vertex = v;
	d.fragment = f;
	d.colors[0].format = PF_RGBA8;
	d.colors[0].write_mask = 0xf;
	d.topology = 3; /* MTLPrimitiveTopologyClassTriangle */
	d.rasterization = 1;
	NmbRenderPipeline *p = v && f ? nmb_render_pipeline_new(dev, &d) : NULL;
	if (!p) {
		refused("render with a sampled texture: pipeline");
		goto out;
	}
	NmbTexture *src = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	NmbTexture *dst = texture(PF_RGBA8, 8, 8, 1, TU_RT | TU_READ, SM_PRIVATE);
	float pos[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
	float col[6][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
	NmbBuffer *pb = nmb_buffer_new(dev, sizeof(pos), SM_SHARED, pos), *cb_ = nmb_buffer_new(dev, sizeof(col), SM_SHARED, col);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	struct NmbRenderPassDescriptor rp;
	memset(&rp, 0, sizeof(rp));
	rp.colors[0].texture = dst;
	rp.colors[0].load = LOAD_CLEAR;
	rp.colors[0].store = STORE_STORE;
	NmbEncoder *e = nmb_render_encoder_new(cb, &rp);
	if (!e || !nmb_render_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 1, pb, 0, 0) ||
	    !nmb_encoder_set_buffer(e, 1, cb_, 0, 1) || !nmb_encoder_set_texture(e, 2, src, 0) ||
	    !nmb_render_draw(e, PRIM_TRIANGLE, 0, 6, 1, 0)) {
		refused("render with a sampled texture");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "render with a sampled texture")) {
		gpu_result("render with a sampled texture (constexpr linear sampler)", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(pb);
	nmb_buffer_release(cb_);
	nmb_texture_release(src);
	nmb_texture_release(dst);
	nmb_render_pipeline_release(p);
out:
	if (v)
		nmb_function_release(v);
	if (f)
		nmb_function_release(f);
}

static void storage_compute(NmbLibrary *lib)
{
	NmbFunction *k = nmb_function_new(lib, "invert_texels");
	NmbComputePipeline *p = k ? nmb_compute_pipeline_new(dev, k) : NULL;
	if (!p) {
		refused("compute on storage textures: pipeline");
		goto out;
	}
	NmbTexture *src = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	NmbTexture *dst = texture(PF_RGBA8, 8, 8, 1, TU_WRITE | TU_READ, SM_SHARED);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	NmbEncoder *e = nmb_compute_encoder_new(cb);
	uint64_t grid[3] = {8, 8, 1}, group[3] = {8, 8, 1};
	if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_encoder_set_texture(e, 0, src, 0) ||
	    !nmb_encoder_set_texture(e, 0, dst, 1) || !nmb_compute_dispatch(e, grid, group, true)) {
		refused("compute on storage textures");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "compute on storage textures")) {
		gpu_result("compute on storage textures (read + write images)", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_texture_release(src);
	nmb_texture_release(dst);
	nmb_compute_pipeline_release(p);
out:
	if (k)
		nmb_function_release(k);
}

static void blits(void)
{
	enum { N = 4096 };
	uint8_t *pattern = malloc(N);
	for (int i = 0; i < N; i++)
		pattern[i] = (uint8_t)(i * 13 + 1);
	NmbBuffer *a = nmb_buffer_new(dev, N, SM_SHARED, pattern), *b = nmb_buffer_new(dev, N, SM_SHARED, NULL),
	          *c = nmb_buffer_new(dev, N, SM_SHARED, NULL), *fill = nmb_buffer_new(dev, N, SM_SHARED, NULL);
	NmbTexture *t = texture(PF_RGBA8, 32, 32, 1, TU_READ, SM_SHARED);
	NmbTexture *mip = texture(PF_RGBA8, 16, 16, 5, TU_READ | TU_RT, SM_PRIVATE);
	NmbTexture *t2 = texture(PF_RGBA8, 32, 32, 1, TU_READ, SM_SHARED);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	NmbEncoder *e = nmb_blit_encoder_new(cb);
	uint64_t zero[3] = {0, 0, 0}, size[3] = {32, 32, 1};
	bool ok = e && nmb_blit_copy_buffer(e, a, 0, b, 0, N) && nmb_blit_fill_buffer(e, fill, 16, 1040, 0xab) &&
	          nmb_blit_buffer_to_texture(e, a, 0, 128, 4096, size, t, 0, 0, zero) &&
	          nmb_blit_copy_texture(e, t, 0, 0, zero, size, t2, 0, 0, zero) &&
	          nmb_blit_texture_to_buffer(e, t, 0, 0, zero, size, c, 0, 128, 4096) && nmb_blit_generate_mipmaps(e, mip);
	if (!ok) {
		refused("blits");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "blits")) {
		const uint8_t *f = nmb_buffer_contents(fill), *cc = nmb_buffer_contents(c);
		int bad = 0;
		for (int i = 0; i < N; i++)
			bad += f[i] != (i >= 16 && i < 1040 ? 0xab : 0);
		line(bad ? "FAIL" : "ok", "fill buffer (a5xx: on the CPU after a wait)", NULL);
		line(memcmp(cc, pattern, N) ? "FAIL" : "ok", "buffer -> texture -> buffer (CPU paths)", NULL);
		gpu_result("buffer copy (GPU blit)", memcmp(nmb_buffer_contents(b), pattern, N) != 0, NULL);
		gpu_result("texture copy and mipmap generation (GPU)", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(a);
	nmb_buffer_release(b);
	nmb_buffer_release(c);
	nmb_buffer_release(fill);
	nmb_texture_release(t);
	nmb_texture_release(t2);
	nmb_texture_release(mip);
	free(pattern);
}

static void indexed_and_depth(NmbLibrary *lib)
{
	NmbFunction *v = nmb_function_new(lib, "colored_vertex"), *f = nmb_function_new(lib, "colored_fragment");
	struct NmbRenderPipelineDescriptor d;
	memset(&d, 0, sizeof(d));
	d.vertex = v;
	d.fragment = f;
	d.colors[0].format = PF_RGBA8;
	d.colors[0].write_mask = 0xf;
	d.depth_format = PF_D32F;
	d.topology = 3;
	d.rasterization = 1;
	NmbRenderPipeline *p = nmb_render_pipeline_new(dev, &d);
	struct NmbDepthStencilDescriptor ds;
	memset(&ds, 0, sizeof(ds));
	ds.compare = CMP_LESS;
	ds.write = 1;
	ds.front.compare = ds.back.compare = CMP_ALWAYS;
	ds.front.read_mask = ds.front.write_mask = ds.back.read_mask = ds.back.write_mask = 0xff;
	NmbDepthStencil *dss = nmb_depth_stencil_new(dev, &ds);
	if (!p || !dss) {
		refused("indexed draw with depth: pipeline");
		goto out;
	}
	float pos[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
	float col[4][4] = {{0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
	uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
	NmbBuffer *pb = nmb_buffer_new(dev, sizeof(pos), SM_SHARED, pos), *cbuf = nmb_buffer_new(dev, sizeof(col), SM_SHARED, col),
	          *ib = nmb_buffer_new(dev, sizeof(idx), SM_SHARED, idx);
	NmbTexture *rt = texture(PF_RGBA8, 16, 16, 1, TU_RT, SM_PRIVATE), *depth = texture(PF_D32F, 16, 16, 1, TU_RT, SM_PRIVATE);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	struct NmbRenderPassDescriptor rp;
	memset(&rp, 0, sizeof(rp));
	rp.colors[0].texture = rt;
	rp.colors[0].load = LOAD_CLEAR;
	rp.colors[0].store = STORE_STORE;
	rp.depth.texture = depth;
	rp.depth.load = LOAD_CLEAR;
	rp.depth.clear[0] = 1.0;
	NmbEncoder *e = rt && depth ? nmb_render_encoder_new(cb, &rp) : NULL;
	double vp[6] = {0, 0, 16, 16, 0, 1};
	uint64_t sc[4] = {2, 2, 12, 12};
	if (!e || !nmb_render_set_pipeline(e, p) || !nmb_render_set_depth_stencil(e, dss) || !nmb_render_set_viewport(e, vp) ||
	    !nmb_render_set_scissor(e, sc) || !nmb_render_set_raster(e, 2, 1, 0) || !nmb_encoder_set_buffer(e, 1, pb, 0, 0) ||
	    !nmb_encoder_set_buffer(e, 1, cbuf, 0, 1) ||
	    !nmb_render_draw_indexed(e, PRIM_TRIANGLE, 6, IDX_U16, ib, 0, 1, 0, 0)) {
		refused("indexed draw with depth, viewport, scissor, back culling");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "indexed draw with depth")) {
		gpu_result("indexed draw with depth, viewport, scissor, back culling", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(pb);
	nmb_buffer_release(cbuf);
	nmb_buffer_release(ib);
	if (rt)
		nmb_texture_release(rt);
	if (depth)
		nmb_texture_release(depth);
out:
	if (p)
		nmb_render_pipeline_release(p);
	if (dss)
		nmb_depth_stencil_release(dss);
	nmb_function_release(v);
	nmb_function_release(f);
}

static void function_constants(void)
{
	size_t n = 0;
	const void *air = nmb_function_constant_air(&n);
	NmbLibrary *lib = nmb_library_new(dev, air, n, true);
	if (!lib) {
		refused("function constants: library");
		return;
	}
	int32_t mode = 0;
	struct NmbConstant c = {DT_INT, 0, NULL, &mode, 4};
	NmbFunction *k = nmb_function_new_specialized(lib, "fcfold", &c, 1);
	NmbComputePipeline *p = k ? nmb_compute_pipeline_new(dev, k) : NULL;
	if (!p) {
		refused("function constants");
	} else {
		NmbBuffer *in = nmb_buffer_new(dev, 64 * 4, SM_SHARED, NULL), *out = nmb_buffer_new(dev, 64 * 4, SM_SHARED, NULL);
		NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
		NmbEncoder *e = nmb_compute_encoder_new(cb);
		uint64_t grid[3] = {8, 1, 1}, group[3] = {8, 1, 1};
		if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 0, in, 0, 0) ||
		    !nmb_encoder_set_buffer(e, 0, out, 0, 1) || !nmb_compute_dispatch(e, grid, group, true)) {
			refused("function constants: dispatch");
			if (e)
				nmb_encoder_discard(e);
		} else if (nmb_encoder_end(e) && finish(cb, "function constants")) {
			gpu_result("function constants (fcfold, mode = 0)", 0, NULL);
		}
		nmb_command_buffer_release(cb);
		nmb_buffer_release(in);
		nmb_buffer_release(out);
		nmb_compute_pipeline_release(p);
	}
	if (k)
		nmb_function_release(k);
	nmb_library_release(lib);
}

static void threadgroup_memory(void)
{
	NmbLibrary *lib = library_file("threadgroup_word.air");
	if (!lib) {
		line("FAIL", "threadgroup memory", "no threadgroup_word.air");
		return;
	}
	NmbFunction *k = nmb_function_new(lib, "threadgroup_word");
	NmbComputePipeline *p = k ? nmb_compute_pipeline_new(dev, k) : NULL;
	NmbBuffer *out = nmb_buffer_new(dev, 64, SM_SHARED, NULL);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	NmbEncoder *e = p ? nmb_compute_encoder_new(cb) : NULL;
	uint64_t grid[3] = {1, 1, 1}, group[3] = {1, 1, 1};
	if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_compute_set_threadgroup_memory(e, 16, 0) ||
	    !nmb_encoder_set_buffer(e, 0, out, 0, 1) || !nmb_compute_dispatch(e, grid, group, false)) {
		refused("threadgroup memory");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "threadgroup memory")) {
		gpu_result("threadgroup memory (setThreadgroupMemoryLength 16)", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(out);
	if (p)
		nmb_compute_pipeline_release(p);
	if (k)
		nmb_function_release(k);
	nmb_library_release(lib);
}

static void sampler_kernel(void)
{
	NmbLibrary *lib = library_file("sampling.air");
	if (!lib) {
		line("FAIL", "sampler argument", "no sampling.air");
		return;
	}
	NmbFunction *k = nmb_function_new(lib, "sample_level");
	NmbComputePipeline *p = k ? nmb_compute_pipeline_new(dev, k) : NULL;
	struct NmbSamplerDescriptor sd;
	memset(&sd, 0, sizeof(sd));
	sd.min_filter = sd.mag_filter = 1;
	sd.normalized = 1;
	sd.max_anisotropy = 1;
	sd.lod_max = 1000;
	NmbSampler *s = nmb_sampler_new(dev, &sd);
	NmbTexture *t = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	NmbBuffer *out = nmb_buffer_new(dev, 256, SM_SHARED, NULL);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	NmbEncoder *e = p && s ? nmb_compute_encoder_new(cb) : NULL;
	uint64_t grid[3] = {4, 1, 1}, group[3] = {4, 1, 1};
	if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_encoder_set_texture(e, 0, t, 0) || !nmb_encoder_set_sampler(e, 0, s, 0) ||
	    !nmb_encoder_set_buffer(e, 0, out, 0, 0) || !nmb_compute_dispatch(e, grid, group, true)) {
		refused("sampler argument (sample_level)");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "sampler argument")) {
		gpu_result("sampler argument (sample_level kernel)", 0, NULL);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(out);
	nmb_texture_release(t);
	if (s)
		nmb_sampler_release(s);
	if (p)
		nmb_compute_pipeline_release(p);
	if (k)
		nmb_function_release(k);
	nmb_library_release(lib);
}

static void vertex_attributes(void)
{
	NmbLibrary *lib = library_file("stage_io.air");
	if (!lib) {
		line("FAIL", "vertex attributes", "no stage_io.air");
		return;
	}
	NmbFunction *v = nmb_function_new(lib, "stage_io_vertex"), *f = nmb_function_new(lib, "stage_io_fragment");
	struct NmbRenderPipelineDescriptor d;
	memset(&d, 0, sizeof(d));
	d.vertex = v;
	d.fragment = f;
	d.colors[0].format = PF_RGBA8;
	d.colors[0].write_mask = 0xf;
	d.colors[2].format = PF_RGBA32U;
	d.colors[2].write_mask = 0xf;
	d.depth_format = PF_D32F;
	d.topology = 3;
	d.rasterization = 1;
	d.vertex_inputs = 1;
	d.attributes[0] = (struct NmbVertexAttribute){VF_FLOAT3, 0, 0};
	d.attributes[1] = (struct NmbVertexAttribute){VF_HALF2, 12, 0};
	d.attributes[4] = (struct NmbVertexAttribute){VF_USHORT2, 16, 0};
	d.attributes[7] = (struct NmbVertexAttribute){VF_UCHAR4, 20, 0};
	d.layouts[0] = (struct NmbVertexLayout){24, 1, 1};
	NmbRenderPipeline *p = v && f ? nmb_render_pipeline_new(dev, &d) : NULL;
	if (!p) {
		refused("vertex attributes (stage_io: 4 formats, 2 color attachments, depth): pipeline");
	} else {
		NmbBuffer *vb = nmb_buffer_new(dev, 24 * 3, SM_SHARED, NULL);
		float scale[4] = {1, 1, 1, 1};
		NmbTexture *c0 = texture(PF_RGBA8, 8, 8, 1, TU_RT, SM_PRIVATE), *c2 = texture(PF_RGBA32U, 8, 8, 1, TU_RT, SM_PRIVATE),
		           *z = texture(PF_D32F, 8, 8, 1, TU_RT, SM_PRIVATE);
		NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
		struct NmbRenderPassDescriptor rp;
		memset(&rp, 0, sizeof(rp));
		rp.colors[0].texture = c0;
		rp.colors[0].load = LOAD_CLEAR;
		rp.colors[0].store = STORE_STORE;
		rp.colors[2].texture = c2;
		rp.colors[2].load = LOAD_CLEAR;
		rp.colors[2].store = STORE_STORE;
		rp.depth.texture = z;
		rp.depth.load = LOAD_CLEAR;
		rp.depth.clear[0] = 1.0;
		NmbEncoder *e = nmb_render_encoder_new(cb, &rp);
		if (!e || !nmb_render_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 1, vb, 0, 0) ||
		    !nmb_encoder_set_bytes(e, 1, scale, sizeof(scale), 1) || !nmb_render_draw(e, PRIM_TRIANGLE, 0, 3, 1, 0)) {
			refused("vertex attributes (stage_io)");
			if (e)
				nmb_encoder_discard(e);
		} else if (nmb_encoder_end(e) && finish(cb, "vertex attributes")) {
			gpu_result("vertex attributes (stage_io: 4 formats, set bytes, 2 color attachments, depth)", 0, NULL);
		}
		nmb_command_buffer_release(cb);
		nmb_buffer_release(vb);
		nmb_texture_release(c0);
		nmb_texture_release(c2);
		nmb_texture_release(z);
		nmb_render_pipeline_release(p);
	}
	if (v)
		nmb_function_release(v);
	if (f)
		nmb_function_release(f);
	nmb_library_release(lib);
}

static int scheduled, completed;
static uint64_t notified;
static void on_scheduled(void *c)
{
	(void)c;
	__atomic_add_fetch(&scheduled, 1, __ATOMIC_SEQ_CST);
}
static void on_completed(void *c)
{
	(void)c;
	__atomic_add_fetch(&completed, 1, __ATOMIC_SEQ_CST);
}
static void on_notify(void *c, uint64_t v)
{
	(void)c;
	__atomic_store_n(&notified, v, __ATOMIC_SEQ_CST);
}

static void sync_objects(NmbLibrary *lib)
{
	/* a wait on another queue's later signal: cb1 (queue 1) waits for E >= 1, cb2 (queue 2) signals it */
	NmbQueue *q2 = nmb_queue_new(dev);
	NmbEvent *ev = nmb_event_new(dev, true);
	NmbCommandBuffer *cb1 = nmb_command_buffer_new(queue), *cb2 = nmb_command_buffer_new(q2);
	NmbBuffer *buf = nmb_buffer_new(dev, 4096, SM_SHARED, NULL);
	nmb_command_buffer_on_scheduled(cb1, on_scheduled, NULL);
	nmb_command_buffer_on_completed(cb1, on_completed, NULL);
	nmb_command_buffer_on_completed(cb2, on_completed, NULL);
	nmb_shared_event_notify(ev, 2, on_notify, NULL);
	bool ok = nmb_command_buffer_wait_event(cb1, ev, 1);
	NmbEncoder *e = nmb_blit_encoder_new(cb1);
	ok = ok && e && nmb_blit_fill_buffer(e, buf, 0, 64, 1) && nmb_encoder_end(e);
	ok = ok && nmb_command_buffer_signal_event(cb1, ev, 2);
	ok = ok && nmb_command_buffer_signal_event(cb2, ev, 1);
	int uses_before = nmb_command_buffer_uses_buffer(cb1, buf);
	ok = ok && nmb_command_buffer_commit(cb1) && nmb_command_buffer_commit(cb2);
	if (!ok) {
		refused("events across queues");
	} else {
		int w1 = nmb_command_buffer_wait(cb1, 10000), w2 = nmb_command_buffer_wait(cb2, 10000);
		bool reached = nmb_shared_event_wait(ev, 2, 1000);
		usleep(50000);
		char detail[160];
		snprintf(detail, sizeof(detail), "waits %d %d, value %llu, notified at %llu, scheduled %d, completed %d, uses %d",
		         w1, w2, (unsigned long long)nmb_shared_event_value(ev), (unsigned long long)notified, scheduled, completed,
		         uses_before);
		bool good = w1 == 0 && w2 == 0 && reached && notified == 2 && scheduled == 1 && completed == 2 &&
		            nmb_command_buffer_status(cb1) == 4 && uses_before == 1 && nmb_command_buffer_uses_buffer(cb1, buf) == 0;
		line(good ? "ok" : "FAIL", "events across queues, shared event listener, handlers, resource tracking", detail);
		double t0 = nmb_command_buffer_gpu_time(cb1, false), t1 = nmb_command_buffer_gpu_time(cb1, true);
		line(t0 > 0 && t1 >= t0 ? "ok" : "FAIL", "GPU start/end times", NULL);
	}
	nmb_command_buffer_release(cb1);
	nmb_command_buffer_release(cb2);

	/* a fence from a compute encoder on queue 2 to a blit encoder on queue 1 */
	NmbFence *fence = nmb_fence_new(dev);
	NmbFunction *k = nmb_function_new(lib, "scale_and_offset");
	NmbComputePipeline *p = nmb_compute_pipeline_new(dev, k);
	NmbCommandBuffer *producer = nmb_command_buffer_new(q2), *consumer = nmb_command_buffer_new(queue);
	NmbBuffer *in = nmb_buffer_new(dev, 256, SM_SHARED, NULL), *out = nmb_buffer_new(dev, 256, SM_SHARED, NULL);
	float s = 2;
	NmbEncoder *ce = nmb_compute_encoder_new(producer);
	uint64_t grid[3] = {64, 1, 1}, group[3] = {64, 1, 1};
	ok = ce && nmb_compute_set_pipeline(ce, p) && nmb_encoder_set_buffer(ce, 0, in, 0, 0) &&
	     nmb_encoder_set_buffer(ce, 0, out, 0, 1) && nmb_encoder_set_bytes(ce, 0, &s, 4, 2) &&
	     nmb_compute_dispatch(ce, grid, group, true) && nmb_encoder_update_fence(ce, fence, 0) && nmb_encoder_end(ce);
	NmbEncoder *be = nmb_blit_encoder_new(consumer);
	ok = ok && be && nmb_encoder_wait_fence(be, fence, 0) && nmb_blit_copy_buffer(be, out, 0, buf, 0, 256) && nmb_encoder_end(be);
	ok = ok && nmb_command_buffer_commit(producer) && nmb_command_buffer_commit(consumer);
	if (!ok)
		refused("fence across queues");
	else
		line(nmb_command_buffer_wait(consumer, 10000) == 0 && nmb_command_buffer_wait(producer, 10000) == 0 &&
		             nmb_command_buffer_status(consumer) == 4
		         ? "ok"
		         : "FAIL",
		     "fence across queues (compute updates, blit waits)", NULL);
	nmb_command_buffer_release(producer);
	nmb_command_buffer_release(consumer);
	nmb_buffer_release(in);
	nmb_buffer_release(out);
	nmb_buffer_release(buf);
	nmb_compute_pipeline_release(p);
	nmb_function_release(k);
	nmb_fence_release(fence);
	nmb_event_release(ev);
	nmb_queue_release(q2);
}

static void not_yet_paths(void)
{
	NmbHeap *h = nmb_heap_new(dev, 0, 1 << 20, 0);
	if (h) {
		line("FAIL", "heaps", "made a heap (expected not yet)");
		nmb_heap_release(h);
	} else {
		refused("heaps");
	}
	NmbBinaryArchive *a = nmb_binary_archive_new(dev, NULL);
	if (a && !nmb_binary_archive_serialize(a, "/tmp/sweetmtl-archive"))
		refused("binary archive serialize");
	if (a)
		nmb_binary_archive_release(a);
}

int main(int argc, char **argv)
{
	air_dir = argc > 1 ? argv[1] : NULL;
	shim = getenv("SWEETMTL_DRM_SHIM") != NULL;
	mkdir("/tmp/sweetmtl-cache", 0777);
	dev = nmb_device_open("", "/tmp/sweetmtl-cache");
	if (!dev) {
		line("FAIL", "device", nmb_last_error());
		return 1;
	}
	queue = nmb_queue_new(dev);
	size_t n = 0;
	const void *air = nmb_fixture_air(&n);
	NmbLibrary *lib = nmb_library_new(dev, air, n, true);
	if (!lib) {
		line("FAIL", "fixture library", nmb_last_error());
		return 1;
	}
	device_answers();
	texture_cpu();
	sampled_render(lib);
	storage_compute(lib);
	blits();
	indexed_and_depth(lib);
	function_constants();
	threadgroup_memory();
	sampler_kernel();
	vertex_attributes();
	sync_objects(lib);
	not_yet_paths();
	nmb_library_release(lib);
	nmb_queue_release(queue);
	nmb_device_release(dev);
	usleep(100000);
	printf("FAIL lines: %d\n", fails);
	return fails;
}
