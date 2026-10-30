/*
 * mtlabitest: the bridge's object ABI (NativeMetalObjects.h, what the Objective-C frontend calls) on sweetmtl-runtime,
 * beyond the self-test: device answers, textures (CPU access, sampling, storage), samplers, blits, indexed draws with
 * depth, viewport, scissor and culling, multisample resolves (color, depth and stencil), function constants,
 * threadgroup memory, vertex attributes, events, shared events and listeners, fences, handlers and resource tracking.
 *
 *   mtlabitest [--daemon] [AIR_DIR]
 *
 * AIR_DIR holds extra fixtures assembled from metal2vulkan's validation fixtures (host-check.sh makes them):
 * stage_io.air, sampling.air, threadgroup_word.air. A build with MTLABITEST_EMBEDDED_AIR (build-ios.sh) carries them
 * in the binary (embedded_air[], from a header it generates) and needs no AIR_DIR. --daemon: as a launchd job on the
 * phone, run once per boot and then idle (test/oneshot.h). Each check prints one line:
 *   ok        it ran and what it computed is right
 *   ran       it ran (recorded, compiled, submitted); its GPU-written result is not checkable (SWEETMTL_DRM_SHIM=1:
 *             freedreno drm-shim executes nothing)
 *   not yet   the runtime refused it as not yet implemented (the reason follows)
 *   FAIL      anything else
 * On softpipe (SG_DRIVER=softpipe) and on the phone every result is checked. The exit status counts FAIL lines.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "NativeMetalObjects.h"
#include "oneshot.h"

/* The bridge's self-test links against it (mtlbridgetest.c says why); this tool never calls it. */
void sys_dcache_flush(void *start, size_t len)
{
	(void)start;
	(void)len;
}

static int fails, shim, softpipe;
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

/* a GPU-computed result: checked where something executes */
static void gpu_result(const char *what, int bad, const char *detail)
{
	if (shim)
		line("ran", what, "result not checkable on drm-shim");
	else
		line(bad ? "FAIL" : "ok", what, detail);
}

/* Metal's raw values used below */
enum {
	PF_RGBA8 = 70, PF_RGBA32U = 123, PF_D32F = 252, PF_D32F_S8 = 260,
	TT_2D = 2, TT_2D_MS = 4, TU_READ = 1, TU_WRITE = 2, TU_RT = 4,
	SM_SHARED = 0, SM_PRIVATE = 2 << 4,
	LOAD_LOAD = 1, LOAD_CLEAR = 2, STORE_STORE = 1, STORE_RESOLVE = 2, STORE_STORE_RESOLVE = 3,
	STENCIL_RESOLVE_DEPTH_SAMPLE = 1,
	PRIM_TRIANGLE = 3,
	IDX_U16 = 0,
	CMP_LESS = 1, CMP_ALWAYS = 7,
	CULL_BACK = 2, WIND_CW = 0, WIND_CCW = 1,
	VF_FLOAT3 = 30, VF_HALF2 = 25, VF_USHORT2 = 13, VF_UCHAR4 = 3,
	DT_INT = 29,
};

static NmbDevice *dev;
static NmbQueue *queue;

static NmbLibrary *library_file(const char *name)
{
#ifdef MTLABITEST_EMBEDDED_AIR
	for (size_t i = 0; i < sizeof(embedded_air) / sizeof(embedded_air[0]); i++)
		if (!strcmp(embedded_air[i].name, name))
			return nmb_library_new(dev, embedded_air[i].bytes, embedded_air[i].size, true);
#endif
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

/* every texel of a w x h RGBA8 level is `px` */
static bool fill_rgba8(NmbTexture *t, int w, int h, int level, const uint8_t px[4])
{
	uint8_t *bytes = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++)
		memcpy(bytes + 4 * i, px, 4);
	uint64_t region[6] = {0, 0, 0, (uint64_t)w, (uint64_t)h, 1};
	bool ok = nmb_texture_replace(t, region, level, 0, bytes, (uint64_t)w * 4, 0, (uint64_t)w * h * 4);
	free(bytes);
	return ok;
}

static bool read_rgba8(NmbTexture *t, int w, int h, int level, uint8_t *out)
{
	uint64_t region[6] = {0, 0, 0, (uint64_t)w, (uint64_t)h, 1};
	return nmb_texture_get_bytes(t, out, (uint64_t)w * 4, 0, (uint64_t)w * h * 4, region, level, 0);
}

static int off_by(int a, int b)
{
	return a > b ? a - b : b - a;
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

static NmbRenderPipeline *colored_pipeline(NmbLibrary *lib, const char *fragment, uint64_t depth, uint64_t samples)
{
	NmbFunction *v = nmb_function_new(lib, "colored_vertex"), *f = nmb_function_new(lib, fragment);
	struct NmbRenderPipelineDescriptor d;
	memset(&d, 0, sizeof(d));
	d.vertex = v;
	d.fragment = f;
	d.colors[0].format = PF_RGBA8;
	d.colors[0].write_mask = 0xf;
	d.depth_format = depth;
	d.samples = samples;
	d.topology = 3; /* MTLPrimitiveTopologyClassTriangle */
	d.rasterization = 1;
	NmbRenderPipeline *p = v && f ? nmb_render_pipeline_new(dev, &d) : NULL;
	if (v)
		nmb_function_release(v);
	if (f)
		nmb_function_release(f);
	return p;
}

static void sampled_render(NmbLibrary *lib)
{
	NmbRenderPipeline *p = colored_pipeline(lib, "sampled_fragment", 0, 1);
	if (!p) {
		refused("render with a sampled texture: pipeline");
		return;
	}
	static const uint8_t px[4] = {40, 80, 120, 255};
	NmbTexture *src = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	NmbTexture *dst = texture(PF_RGBA8, 8, 8, 1, TU_RT | TU_READ, SM_SHARED);
	fill_rgba8(src, 8, 8, 0, px);
	float pos[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
	float col[6][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
	NmbBuffer *pb = nmb_buffer_new(dev, sizeof(pos), SM_SHARED, pos), *cbuf = nmb_buffer_new(dev, sizeof(col), SM_SHARED, col);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	struct NmbRenderPassDescriptor rp;
	memset(&rp, 0, sizeof(rp));
	rp.colors[0].texture = dst;
	rp.colors[0].load = LOAD_CLEAR;
	rp.colors[0].store = STORE_STORE;
	NmbEncoder *e = nmb_render_encoder_new(cb, &rp);
	if (!e || !nmb_render_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 1, pb, 0, 0) ||
	    !nmb_encoder_set_buffer(e, 1, cbuf, 0, 1) || !nmb_encoder_set_texture(e, 2, src, 0) ||
	    !nmb_render_draw(e, PRIM_TRIANGLE, 0, 6, 1, 0)) {
		refused("render with a sampled texture");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "render with a sampled texture")) {
		uint8_t out[8 * 8 * 4];
		int bad = 0;
		if (!read_rgba8(dst, 8, 8, 0, out))
			bad = 64;
		for (int i = 0; i < 64 && !bad; i++)
			for (int c = 0; c < 4; c++)
				bad += off_by(out[4 * i + c], px[c]) > 1;
		char detail[64];
		snprintf(detail, sizeof(detail), "%d of 256 channels off", bad);
		gpu_result("render with a sampled texture (constexpr linear sampler)", bad, detail);
	}
	nmb_command_buffer_release(cb);
	nmb_buffer_release(pb);
	nmb_buffer_release(cbuf);
	nmb_texture_release(src);
	nmb_texture_release(dst);
	nmb_render_pipeline_release(p);
}

static void storage_compute(NmbLibrary *lib)
{
	NmbFunction *k = nmb_function_new(lib, "invert_texels");
	NmbComputePipeline *p = k ? nmb_compute_pipeline_new(dev, k) : NULL;
	if (k)
		nmb_function_release(k);
	if (!p) {
		refused("compute on storage textures: pipeline");
		return;
	}
	NmbTexture *src = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	NmbTexture *dst = texture(PF_RGBA8, 8, 8, 1, TU_WRITE | TU_READ, SM_SHARED);
	uint8_t in[8 * 8 * 4];
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			uint8_t *t = in + 4 * (y * 8 + x);
			t[0] = (uint8_t)(x * 30);
			t[1] = (uint8_t)(y * 30);
			t[2] = 100;
			t[3] = 200;
		}
	uint64_t region[6] = {0, 0, 0, 8, 8, 1};
	nmb_texture_replace(src, region, 0, 0, in, 32, 0, sizeof(in));
	uint8_t junk[8 * 8 * 4];
	memset(junk, 0x5a, sizeof(junk)); /* a texel the kernel misses stays 0x5a */
	nmb_texture_replace(dst, region, 0, 0, junk, 32, 0, sizeof(junk));
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	NmbEncoder *e = nmb_compute_encoder_new(cb);
	uint64_t grid[3] = {8, 8, 1}, group[3] = {4, 4, 1};
	if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_encoder_set_texture(e, 0, src, 0) ||
	    !nmb_encoder_set_texture(e, 0, dst, 1) || !nmb_compute_dispatch(e, grid, group, true)) {
		refused("compute on storage textures");
		if (e)
			nmb_encoder_discard(e);
	} else if (nmb_encoder_end(e) && finish(cb, "compute on storage textures")) {
		uint8_t out[8 * 8 * 4];
		int bad = read_rgba8(dst, 8, 8, 0, out) ? 0 : 256;
		for (int i = 0; i < 64 && !bad; i++)
			for (int c = 0; c < 4; c++)
				if (off_by(out[4 * i + c], c < 3 ? 255 - in[4 * i + c] : in[4 * i + c]) > 1) {
					if (bad++ < 4 && !shim)
						printf("  texel %d channel %d: %u, want %u\n", i, c, out[4 * i + c],
						       c < 3 ? 255 - in[4 * i + c] : in[4 * i + c]);
				}
		char detail[64];
		snprintf(detail, sizeof(detail), "%d of 256 channels off", bad);
		gpu_result("compute on storage textures (read + write images)", bad, detail);
	}
	nmb_command_buffer_release(cb);
	nmb_texture_release(src);
	nmb_texture_release(dst);
	nmb_compute_pipeline_release(p);
}

static void blits(void)
{
	enum { N = 4096 };
	uint8_t *pattern = malloc(N);
	for (int i = 0; i < N; i++)
		pattern[i] = (uint8_t)(i * 13 + 1);
	static const uint8_t base[4] = {10, 20, 30, 40};
	NmbBuffer *a = nmb_buffer_new(dev, N, SM_SHARED, pattern), *b = nmb_buffer_new(dev, N, SM_SHARED, NULL),
	          *c = nmb_buffer_new(dev, N, SM_SHARED, NULL), *fill = nmb_buffer_new(dev, N, SM_SHARED, NULL);
	NmbTexture *t = texture(PF_RGBA8, 32, 32, 1, TU_READ, SM_SHARED);
	NmbTexture *mip = texture(PF_RGBA8, 16, 16, 5, TU_READ | TU_RT, SM_SHARED);
	NmbTexture *t2 = texture(PF_RGBA8, 32, 32, 1, TU_READ, SM_SHARED);
	fill_rgba8(mip, 16, 16, 0, base);
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
		uint8_t *copy = malloc(N);
		gpu_result("texture copy (GPU blit)", !read_rgba8(t2, 32, 32, 0, copy) || memcmp(copy, pattern, N), NULL);
		uint8_t level1[8 * 8 * 4];
		int mbad = read_rgba8(mip, 8, 8, 1, level1) ? 0 : 256;
		for (int i = 0; i < 64 && !mbad; i++)
			for (int k = 0; k < 4; k++)
				mbad += off_by(level1[4 * i + k], base[k]) > 1;
		gpu_result("mipmap generation (level 1 of a uniform 16x16)", mbad, NULL);
		free(copy);
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

/* A quad of two clockwise triangles (in Metal's y-down window coordinates), indexed, culling back faces with depth,
 * a viewport and a scissor. Front faces counter-clockwise: both are back faces, nothing draws. Front faces clockwise:
 * green inside the scissor, the clear color outside. */
static void indexed_and_depth(NmbLibrary *lib)
{
	NmbRenderPipeline *p = colored_pipeline(lib, "colored_fragment", PF_D32F, 1);
	struct NmbDepthStencilDescriptor ds;
	memset(&ds, 0, sizeof(ds));
	ds.compare = CMP_LESS;
	ds.write = 1;
	ds.front.compare = ds.back.compare = CMP_ALWAYS;
	ds.front.read_mask = ds.front.write_mask = ds.back.read_mask = ds.back.write_mask = 0xff;
	NmbDepthStencil *dss = nmb_depth_stencil_new(dev, &ds);
	if (!p || !dss) {
		refused("indexed draw with depth: pipeline");
		if (p)
			nmb_render_pipeline_release(p);
		if (dss)
			nmb_depth_stencil_release(dss);
		return;
	}
	float pos[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
	float col[4][4] = {{0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
	uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
	NmbBuffer *pb = nmb_buffer_new(dev, sizeof(pos), SM_SHARED, pos), *cbuf = nmb_buffer_new(dev, sizeof(col), SM_SHARED, col),
	          *ib = nmb_buffer_new(dev, sizeof(idx), SM_SHARED, idx);
	for (int winding = WIND_CCW; winding >= WIND_CW; winding--) {
		NmbTexture *rt = texture(PF_RGBA8, 16, 16, 1, TU_RT | TU_READ, SM_SHARED),
		           *depth = texture(PF_D32F, 16, 16, 1, TU_RT, SM_PRIVATE);
		NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
		struct NmbRenderPassDescriptor rp;
		memset(&rp, 0, sizeof(rp));
		rp.colors[0].texture = rt;
		rp.colors[0].load = LOAD_CLEAR;
		rp.colors[0].store = STORE_STORE;
		rp.colors[0].clear[2] = 1.0; /* blue */
		rp.colors[0].clear[3] = 1.0;
		rp.depth.texture = depth;
		rp.depth.load = LOAD_CLEAR;
		rp.depth.clear[0] = 1.0;
		NmbEncoder *e = rt && depth ? nmb_render_encoder_new(cb, &rp) : NULL;
		double vp[6] = {0, 0, 16, 16, 0, 1};
		uint64_t sc[4] = {2, 2, 12, 12};
		const char *what = winding == WIND_CCW ? "indexed draw, back faces culled (counter-clockwise front): nothing drawn"
		                                       : "indexed draw with depth, viewport, scissor (clockwise front): drawn";
		if (!e || !nmb_render_set_pipeline(e, p) || !nmb_render_set_depth_stencil(e, dss) ||
		    !nmb_render_set_viewport(e, vp) || !nmb_render_set_scissor(e, sc) ||
		    !nmb_render_set_raster(e, CULL_BACK, (uint64_t)winding, 0) || !nmb_encoder_set_buffer(e, 1, pb, 0, 0) ||
		    !nmb_encoder_set_buffer(e, 1, cbuf, 0, 1) ||
		    !nmb_render_draw_indexed(e, PRIM_TRIANGLE, 6, IDX_U16, ib, 0, 1, 0, 0)) {
			refused(what);
			if (e)
				nmb_encoder_discard(e);
		} else if (nmb_encoder_end(e) && finish(cb, what)) {
			uint8_t out[16 * 16 * 4];
			int bad = read_rgba8(rt, 16, 16, 0, out) ? 0 : 256;
			for (int y = 0; y < 16 && !bad; y++)
				for (int x = 0; x < 16; x++) {
					bool in = winding == WIND_CW && x >= 2 && x < 14 && y >= 2 && y < 14;
					const uint8_t want[4] = {0, (uint8_t)(in ? 255 : 0), (uint8_t)(in ? 0 : 255), 255};
					bad += memcmp(out + 4 * (y * 16 + x), want, 4) != 0;
				}
			char detail[64];
			snprintf(detail, sizeof(detail), "%d of 256 texels off", bad);
			gpu_result(what, bad, detail);
		}
		nmb_command_buffer_release(cb);
		if (rt)
			nmb_texture_release(rt);
		if (depth)
			nmb_texture_release(depth);
	}
	nmb_buffer_release(pb);
	nmb_buffer_release(cbuf);
	nmb_buffer_release(ib);
	nmb_render_pipeline_release(p);
	nmb_depth_stencil_release(dss);
}

/* Multisample resolve of a color attachment: a 4x RGBA8 target (the a540 renders 2x and 4x; the device answers 1 and
 * 4) cleared blue with a green triangle over its lower-left half (y > x in Metal's y-down window coordinates), stored
 * and resolved into a single-sampled texture (MTLStoreActionStoreAndMultisampleResolve); a second pass loads the kept
 * samples, draws nothing and resolves them into another (MTLStoreActionMultisampleResolve). Texels off the diagonal
 * resolve to one color; the diagonal's, half covered, average their samples where the device multisamples (softpipe
 * fakes 4x as 1x: one color each). */
static void msaa_resolve(NmbLibrary *lib)
{
	const char *what = "multisample resolve (4x RGBA8: clear, draw, store and resolve; load, resolve)";
	uint64_t four = 0;
	if (!nmb_device_answer(dev, "supportsTextureSampleCount:", 4, &four) || !four) {
		line("FAIL", what, "supportsTextureSampleCount:4 is false");
		return;
	}
	NmbRenderPipeline *p = colored_pipeline(lib, "colored_fragment", 0, 4);
	struct NmbTextureDescriptor md = {TT_2D_MS, PF_RGBA8, 16, 16, 1, 1, 4, 1, TU_RT, SM_PRIVATE};
	NmbTexture *ms = p ? nmb_texture_new(dev, &md) : NULL;
	NmbTexture *r1 = texture(PF_RGBA8, 16, 16, 1, TU_RT | TU_READ, SM_SHARED),
	           *r2 = texture(PF_RGBA8, 16, 16, 1, TU_RT | TU_READ, SM_SHARED);
	float pos[3][2] = {{-1, 1}, {1, -1}, {-1, -1}};
	float col[3][4] = {{0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
	NmbBuffer *pb = nmb_buffer_new(dev, sizeof(pos), SM_SHARED, pos), *cbuf = nmb_buffer_new(dev, sizeof(col), SM_SHARED, col);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	static const uint8_t junk[4] = {0x5a, 0x5a, 0x5a, 0x5a}; /* a texel the resolve misses stays 0x5a */
	if (!p || !ms || !r1 || !r2) {
		refused(what);
		goto done;
	}
	fill_rgba8(r1, 16, 16, 0, junk);
	fill_rgba8(r2, 16, 16, 0, junk);
	struct NmbRenderPassDescriptor rp;
	memset(&rp, 0, sizeof(rp));
	rp.colors[0].texture = ms;
	rp.colors[0].load = LOAD_CLEAR;
	rp.colors[0].store = STORE_STORE_RESOLVE;
	rp.colors[0].clear[2] = 1.0; /* blue */
	rp.colors[0].clear[3] = 1.0;
	rp.colors[0].resolve_texture = r1;
	NmbEncoder *e = nmb_render_encoder_new(cb, &rp);
	if (!e || !nmb_render_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 1, pb, 0, 0) ||
	    !nmb_encoder_set_buffer(e, 1, cbuf, 0, 1) || !nmb_render_draw(e, PRIM_TRIANGLE, 0, 3, 1, 0)) {
		refused(what);
		if (e)
			nmb_encoder_discard(e);
		goto done;
	}
	if (!nmb_encoder_end(e)) {
		refused(what);
		goto done;
	}
	rp.colors[0].load = LOAD_LOAD;
	rp.colors[0].store = STORE_RESOLVE;
	rp.colors[0].resolve_texture = r2;
	e = nmb_render_encoder_new(cb, &rp);
	if (!e || !nmb_encoder_end(e)) {
		refused("multisample resolve: the second pass (load, resolve)");
		goto done;
	}
	if (finish(cb, what)) {
		uint8_t a[16 * 16 * 4], b[16 * 16 * 4];
		int bad = read_rgba8(r1, 16, 16, 0, a) && read_rgba8(r2, 16, 16, 0, b) ? 0 : 256, blended = 0, differ = 0;
		for (int y = 0; y < 16 && bad < 256; y++)
			for (int x = 0; x < 16; x++) {
				const uint8_t *t = a + 4 * (y * 16 + x), *u = b + 4 * (y * 16 + x);
				if (x != y) {
					const uint8_t want[4] = {0, (uint8_t)(y > x ? 255 : 0), (uint8_t)(y > x ? 0 : 255), 255};
					int off = 0;
					for (int c = 0; c < 4; c++)
						off |= off_by(t[c], want[c]) > 1;
					bad += off;
				} else {
					/* half green, half blue: green and blue add up to one */
					bad += t[0] > 1 || t[3] < 254 || off_by(t[1] + t[2], 255) > 2;
					blended += t[1] > 8 && t[2] > 8;
				}
				int d = 0;
				for (int c = 0; c < 4; c++)
					d |= off_by(t[c], u[c]) > 1;
				differ += d;
			}
		char detail[160];
		snprintf(detail, sizeof(detail), "%d of 256 texels off, %d of 16 diagonal texels blended%s, %d differ after load and resolve",
		         bad, blended, softpipe ? " (softpipe fakes 4x: none expected)" : "", differ);
		/* a device that multisamples blends the half-covered diagonal */
		gpu_result(what, bad || differ || (!softpipe && !blended), detail);
	}
done:
	nmb_command_buffer_release(cb);
	nmb_buffer_release(pb);
	nmb_buffer_release(cbuf);
	if (ms)
		nmb_texture_release(ms);
	if (r1)
		nmb_texture_release(r1);
	if (r2)
		nmb_texture_release(r2);
	if (p)
		nmb_render_pipeline_release(p);
}

/* Multisample resolve of depth and stencil: a 4x Depth32Float_Stencil8 attachment cleared to 0.25 and 0x5b, resolved
 * (MTLStoreActionMultisampleResolve; depth filter Sample0, stencil filter DepthResolvedSample) into a single-sampled
 * one, read back (Depth32Float_Stencil8: a float, then the stencil in the low byte of a word). */
static void msaa_depth_stencil_resolve(void)
{
	const char *what = "multisample resolve of depth and stencil (4x Depth32Float_Stencil8: clear, resolve)";
	struct NmbTextureDescriptor md = {TT_2D_MS, PF_D32F_S8, 16, 16, 1, 1, 4, 1, TU_RT, SM_PRIVATE};
	NmbTexture *ms = nmb_texture_new(dev, &md), *r = texture(PF_D32F_S8, 16, 16, 1, TU_RT | TU_READ, SM_SHARED);
	NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
	enum { N = 16 * 16 * 8 };
	uint8_t *texels = malloc(N);
	if (!ms || !r) {
		refused(what);
		goto done;
	}
	/* depth 0.75, stencil 0x11 where the resolve misses */
	for (int i = 0; i < 16 * 16; i++) {
		const float z = 0.75f;
		const uint32_t s = 0x11;
		memcpy(texels + 8 * i, &z, 4);
		memcpy(texels + 8 * i + 4, &s, 4);
	}
	uint64_t region[6] = {0, 0, 0, 16, 16, 1};
	nmb_texture_replace(r, region, 0, 0, texels, 16 * 8, 0, N);
	struct NmbRenderPassDescriptor rp;
	memset(&rp, 0, sizeof(rp));
	rp.depth.texture = rp.stencil.texture = ms;
	rp.depth.load = rp.stencil.load = LOAD_CLEAR;
	rp.depth.store = rp.stencil.store = STORE_RESOLVE;
	rp.depth.clear[0] = 0.25;
	rp.stencil.clear[0] = 0x5b;
	rp.depth.resolve_texture = rp.stencil.resolve_texture = r;
	rp.stencil.resolve_filter = STENCIL_RESOLVE_DEPTH_SAMPLE;
	NmbEncoder *e = nmb_render_encoder_new(cb, &rp);
	if (!e || !nmb_encoder_end(e)) {
		refused(what);
		goto done;
	}
	if (finish(cb, what)) {
		memset(texels, 0, N);
		int depth_bad = 0, stencil_bad = 0;
		if (!nmb_texture_get_bytes(r, texels, 16 * 8, 0, N, region, 0, 0)) {
			depth_bad = stencil_bad = 256;
		} else {
			for (int i = 0; i < 16 * 16; i++) {
				float z;
				memcpy(&z, texels + 8 * i, 4);
				depth_bad += z != 0.25f;
				stencil_bad += texels[8 * i + 4] != 0x5b;
			}
		}
		char detail[96];
		snprintf(detail, sizeof(detail), "depth %d, stencil %d of 256 texels off", depth_bad, stencil_bad);
		gpu_result(what, depth_bad || stencil_bad, detail);
	}
done:
	nmb_command_buffer_release(cb);
	free(texels);
	if (ms)
		nmb_texture_release(ms);
	if (r)
		nmb_texture_release(r);
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
		float in[64] = {0};
		for (int i = 0; i < 8; i++)
			in[i] = (float)i;
		NmbBuffer *ib = nmb_buffer_new(dev, sizeof(in), SM_SHARED, in), *ob = nmb_buffer_new(dev, sizeof(in), SM_SHARED, NULL);
		NmbCommandBuffer *cb = nmb_command_buffer_new(queue);
		NmbEncoder *e = nmb_compute_encoder_new(cb);
		uint64_t grid[3] = {8, 1, 1}, group[3] = {8, 1, 1};
		if (!e || !nmb_compute_set_pipeline(e, p) || !nmb_encoder_set_buffer(e, 0, ib, 0, 0) ||
		    !nmb_encoder_set_buffer(e, 0, ob, 0, 1) || !nmb_compute_dispatch(e, grid, group, true)) {
			refused("function constants: dispatch");
			if (e)
				nmb_encoder_discard(e);
		} else if (nmb_encoder_end(e) && finish(cb, "function constants")) {
			const float *o = nmb_buffer_contents(ob);
			int bad = 0;
			for (int i = 0; i < 8; i++)
				bad += o[i] != i + 1.0f || o[i + 8] != 0.0f || o[i + 16] != i - 1.0f || o[i + 24] != (float)i;
			gpu_result("function constants (fcfold, mode = 0)", bad, NULL);
		}
		nmb_command_buffer_release(cb);
		nmb_buffer_release(ib);
		nmb_buffer_release(ob);
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
		gpu_result("threadgroup memory (setThreadgroupMemoryLength 16)", *(uint32_t *)nmb_buffer_contents(out) != 42, NULL);
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
	static const uint8_t px[4] = {51, 102, 153, 255};
	NmbTexture *t = texture(PF_RGBA8, 8, 8, 1, TU_READ, SM_SHARED);
	fill_rgba8(t, 8, 8, 0, px);
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
		const float *o = nmb_buffer_contents(out);
		int bad = 0;
		for (int i = 0; i < 16; i++)
			bad += off_by((int)(o[i] * 255.0f + 0.5f), px[i % 4]) > 1;
		gpu_result("sampler argument (sample_level kernel)", bad, NULL);
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
			line("ran", "vertex attributes (stage_io: 4 formats, set bytes, 2 color attachments, depth)",
			     "result not checked (a degenerate triangle)");
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
	if (!ok) {
		refused("fence across queues");
	} else {
		bool done = nmb_command_buffer_wait(consumer, 10000) == 0 && nmb_command_buffer_wait(producer, 10000) == 0 &&
		            nmb_command_buffer_status(consumer) == 4;
		line(done ? "ok" : "FAIL", "fence across queues (compute updates, blit waits)", NULL);
		const float *o = nmb_buffer_contents(buf);
		int bad = 0;
		for (int i = 0; i < 64; i++)
			bad += o[i] != (float)i; /* in is zero: out[i] = 0 * 2 + i */
		gpu_result("fence across queues: the blit saw the kernel's output", bad, NULL);
	}
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
	oneshot_begin("mtlabitest", &argc, &argv);
	air_dir = argc > 1 ? argv[1] : NULL;
	shim = getenv("SWEETMTL_DRM_SHIM") != NULL;
	mkdir("/tmp/sweetmtl-cache", 0777);
	dev = nmb_device_open("", "/tmp/sweetmtl-cache");
	if (!dev) {
		line("FAIL", "device", nmb_last_error());
		return oneshot_end("mtlabitest", 1);
	}
	queue = nmb_queue_new(dev);
	char name[128] = "";
	nmb_device_name(dev, name, sizeof(name));
	softpipe = strstr(name, "softpipe") != NULL;
	size_t n = 0;
	const void *air = nmb_fixture_air(&n);
	NmbLibrary *lib = nmb_library_new(dev, air, n, true);
	if (!lib) {
		line("FAIL", "fixture library", nmb_last_error());
		return oneshot_end("mtlabitest", 1);
	}
	device_answers();
	texture_cpu();
	sampled_render(lib);
	storage_compute(lib);
	blits();
	indexed_and_depth(lib);
	msaa_resolve(lib);
	msaa_depth_stencil_resolve();
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
	return oneshot_end("mtlabitest", fails);
}
