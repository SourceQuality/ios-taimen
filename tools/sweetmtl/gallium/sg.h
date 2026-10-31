/*
 * sweetmtl Gallium shim (tools/sweetmtl/gallium): the C side of the Metal runtime
 * (tools/sweetmtl/runtime, Rust crate sweetmtl-runtime) on Mesa's freedreno Gallium driver
 * for the Adreno 540 (a5xx).
 *
 * The runtime records Metal command buffers as Rust command lists. At commit it replays each
 * list into the device's one pipe_context through the sg_ctx_* calls below, then flushes with a
 * fence (sg_ctx_flush) that a completion thread waits on (sg_fence_wait). Shaders come in as
 * the metal2vulkan translator's SPIR-V (Vulkan set/binding descriptors, push constants) and
 * leave as Gallium CSOs: spirv_to_nir, the M0 lowering of tools/sweetmtl/m0/spv2ir3.c with the
 * descriptor slots the runtime assigned (struct sg_binding), the screen's finalize_nir.
 *
 * Values cross this interface in stable ABIs only, never as Mesa enums: formats are VkFormat
 * (the translator's), blend state is Vulkan's (the translator's GraphicsState), the rest is
 * Metal's raw enumerations (MTLTextureType, MTLCompareFunction, MTLPrimitiveType, ...). The shim
 * maps them to Gallium.
 *
 * Threading: everything named sg_ctx_* and sg_shader_new use the device's context and must run
 * between sg_lock() and sg_unlock(). The other calls take the lock themselves where they need
 * the context and are safe from any thread otherwise.
 *
 * Errors: a call that fails returns NULL/false and leaves its reason in a per-thread string,
 * sg_last_error().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SWEETMTL_SG_H
#define SWEETMTL_SG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sg_device;
struct sg_buffer;
struct sg_texture;
struct sg_view;
struct sg_sampler;
struct sg_shader;
struct sg_fence;

const char *sg_last_error(void);

/* ---- device ---- */

/* Opens the platform's GPU: on Linux the render node (SG_RENDER_NODE, default
 * /dev/dri/renderD128: Mesa's freedreno drm-shim on the PC), on iOS SweetGPU through
 * sweet_drm.c. */
struct sg_device *sg_device_open(void);
/* The same over an msm DRM fd the caller opened (ownership passes to the device). */
struct sg_device *sg_device_open_fd(int fd);
void sg_device_close(struct sg_device *dev);
/* The Gallium screen's name ("FD540") and the GPU id (540). */
const char *sg_device_name(struct sg_device *dev);
uint32_t sg_device_gpu_id(struct sg_device *dev);

/* Counters, for checks on drm-shim where nothing executes. */
struct sg_stats {
   uint64_t flushes;        /* sg_ctx_flush calls */
   uint64_t dispatches;     /* launch_grid calls */
   uint64_t draws;          /* draw_vbo calls */
   uint64_t clears;         /* clears (load action Clear) */
   uint64_t blits;          /* copies, fills, mipmaps and resolves recorded on the GPU */
   uint64_t cpu_copies;     /* copies and fills done on the CPU after a wait */
   uint64_t shaders;        /* CSOs created */
   uint64_t shader_instrs;  /* ir3 instructions of their default variants */
};
void sg_device_stats(struct sg_device *dev, struct sg_stats *out);

void sg_lock(struct sg_device *dev);
void sg_unlock(struct sg_device *dev);

/* ---- formats (VkFormat in, facts out) ---- */

#define SG_USAGE_SAMPLE 1u   /* MTLTextureUsageShaderRead */
#define SG_USAGE_STORAGE 2u  /* MTLTextureUsageShaderWrite */
#define SG_USAGE_RENDER 4u   /* MTLTextureUsageRenderTarget */
#define SG_USAGE_VERTEX 8u   /* a vertex attribute format */
#define SG_USAGE_BLEND 16u   /* blending into it */

/* Whether the device takes `vk_format` for every use in `usage` at `samples` (0/1 single). */
bool sg_format_supported(struct sg_device *dev, uint32_t vk_format, uint32_t usage,
                         uint32_t samples);
/* What the device does with `vk_format` (the a540's answers, from freedreno's format tables):
 * SG_FEAT_* bits. The device profile's format table is made of them (profile/make-profile.py). */
#define SG_FEAT_SAMPLED 0x1u
#define SG_FEAT_FILTER 0x2u
#define SG_FEAT_STORAGE 0x4u
#define SG_FEAT_STORAGE_ATOMIC 0x8u
#define SG_FEAT_COLOR 0x10u
#define SG_FEAT_BLEND 0x20u
#define SG_FEAT_DEPTH_STENCIL 0x40u
#define SG_FEAT_LINEAR 0x80u
#define SG_FEAT_VERTEX 0x100u
#define SG_FEAT_UNIFORM_TEXEL 0x200u
#define SG_FEAT_STORAGE_TEXEL 0x400u
#define SG_FEAT_SAMPLES_2 0x800u
#define SG_FEAT_SAMPLES_4 0x1000u
#define SG_FEAT_SAMPLES_8 0x2000u
uint32_t sg_format_features(struct sg_device *dev, uint32_t vk_format);
/* Bytes, width and height of a block (a texel for uncompressed formats); 0 for unknown. */
uint32_t sg_format_block_bytes(uint32_t vk_format);
uint32_t sg_format_block_width(uint32_t vk_format);
uint32_t sg_format_block_height(uint32_t vk_format);

/* ---- buffers ---- */

/* A PIPE_BUFFER resource of `size` bytes, mapped for its whole life (sg_buffer_map), at a GPU
 * address (iova) that never changes. */
struct sg_buffer *sg_buffer_new(struct sg_device *dev, uint64_t size);
void sg_buffer_free(struct sg_buffer *buf);
void *sg_buffer_map(struct sg_buffer *buf);
uint64_t sg_buffer_iova(struct sg_buffer *buf);
uint64_t sg_buffer_size(struct sg_buffer *buf);

/* ---- textures ---- */

struct sg_texture_desc {
   uint32_t type;        /* MTLTextureType */
   uint32_t vk_format;
   uint32_t width, height, depth, array_size;
   uint32_t levels;      /* mipmap levels, >= 1 */
   uint32_t samples;     /* 0 or 1 for single-sampled; a540: 2 or 4 (softpipe fakes them: 1) */
   uint32_t usage;       /* SG_USAGE_* */
   uint32_t linear;      /* force a linear layout */
};
struct sg_texture *sg_texture_new(struct sg_device *dev, const struct sg_texture_desc *desc);
/* A linear 2D texture over `buf`'s memory at `offset`, rows `stride` bytes apart: the same GPU
 * memory, no copy. Fails where a5xx cannot lay a texture there (pitch or offset alignment). */
struct sg_texture *sg_texture_new_on_buffer(struct sg_device *dev,
                                            const struct sg_texture_desc *desc,
                                            struct sg_buffer *buf, uint64_t offset,
                                            uint32_t stride);
void sg_texture_free(struct sg_texture *tex);
/* The layout's row pitch of `level` in bytes (0 for a level past the last). */
uint32_t sg_texture_pitch(struct sg_texture *tex, uint32_t level);

struct sg_box {
   int32_t x, y, z;
   int32_t width, height, depth;
};
/* CPU writes and reads of a texture region (replaceRegion:, getBytes:); they wait for the GPU
 * work that uses the texture. `layer` is the array slice (3D textures take z from the box). */
bool sg_texture_write(struct sg_device *dev, struct sg_texture *tex, uint32_t level,
                      uint32_t layer, const struct sg_box *box, const void *data,
                      uint32_t stride, uint32_t layer_stride);
bool sg_texture_read(struct sg_device *dev, struct sg_texture *tex, uint32_t level,
                     uint32_t layer, const struct sg_box *box, void *data, uint32_t stride,
                     uint32_t layer_stride);

/* Sampler views: a texture as the shader samples it. */
struct sg_view_desc {
   uint32_t vk_format;   /* 0: the texture's */
   uint32_t type;        /* MTLTextureType of the view */
   uint32_t first_level, last_level;
   uint32_t first_layer, last_layer;
   uint8_t swizzle[4];   /* MTLTextureSwizzle: 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha */
};
struct sg_view *sg_view_new(struct sg_device *dev, struct sg_texture *tex,
                            const struct sg_view_desc *desc);
/* A texel-buffer view of `buf`. */
struct sg_view *sg_view_new_buffer(struct sg_device *dev, struct sg_buffer *buf,
                                   uint32_t vk_format, uint64_t offset, uint64_t size);
void sg_view_free(struct sg_view *view);

struct sg_sampler_desc {
   uint32_t min_filter, mag_filter;     /* MTLSamplerMinMagFilter */
   uint32_t mip_filter;                 /* MTLSamplerMipFilter */
   uint32_t s_address, t_address, r_address; /* MTLSamplerAddressMode */
   uint32_t border;                     /* MTLSamplerBorderColor */
   uint32_t normalized;
   uint32_t max_anisotropy;
   uint32_t compare;                    /* MTLCompareFunction; 0 (never) with compare_enable 0 */
   uint32_t compare_enable;
   uint32_t integer_border;
   float lod_min, lod_max;
};
struct sg_sampler *sg_sampler_new(struct sg_device *dev, const struct sg_sampler_desc *desc);
void sg_sampler_free(struct sg_sampler *sampler);

/* ---- shaders ---- */

enum sg_stage { SG_VERTEX = 0, SG_FRAGMENT = 1, SG_COMPUTE = 2 };

enum sg_class {
   SG_CLASS_UBO = 1,          /* uniform buffer -> constant buffer `slot` (0 is the push block) */
   SG_CLASS_SSBO,             /* storage buffer -> shader buffer `slot` */
   SG_CLASS_SSBO_AS_UBO,      /* read-only storage buffer read as constant buffer `slot`
                               * (a5xx has no shader buffers in the vertex stage) */
   SG_CLASS_TEXTURE,          /* sampled image -> sampler view `slot` */
   SG_CLASS_SAMPLER,          /* sampler -> sampler state `sampler_slot` */
   SG_CLASS_COMBINED,         /* combined image sampler -> view `slot`, sampler `sampler_slot` */
   SG_CLASS_IMAGE,            /* storage image / storage texel buffer -> shader image `slot` */
   SG_CLASS_TEXEL_BUFFER,     /* uniform texel buffer -> sampler view `slot` */
   SG_CLASS_INPUT_ATTACHMENT, /* subpass input -> texel fetch of sampler view `slot` */
};

/* Where descriptor (set, binding) lives among Gallium's per-stage slots; arrays take `count`
 * consecutive slots from `slot` (and from `sampler_slot`). */
struct sg_binding {
   uint32_t set, binding, count, cls;
   uint32_t slot, sampler_slot;
};

/* One specialization constant: `size` bytes at `data` (a SpecializationMapEntry). */
struct sg_spec {
   uint32_t id, size;
   const void *data;
};

struct sg_shader_desc {
   uint32_t stage;                  /* enum sg_stage */
   const uint32_t *words;           /* SPIR-V */
   size_t word_count;
   const char *entry;
   const struct sg_spec *spec;
   uint32_t spec_count;
   const struct sg_binding *bindings;
   uint32_t binding_count;
};
/* Translates and compiles the stage to a CSO; its default ir3 variant is compiled before this
 * returns, so a compile failure is this call's. Hold sg_lock. */
struct sg_shader *sg_shader_new(struct sg_device *dev, const struct sg_shader_desc *desc);
void sg_shader_free(struct sg_shader *shader);

struct sg_shader_info {
   uint32_t stage;
   uint32_t instrs;              /* ir3 instructions of the default variant */
   uint32_t full_regs, half_regs;
   uint32_t constlen;
   uint32_t workgroup[3];        /* compute: the local size after specialization */
   uint32_t shared_bytes;        /* compute: static shared memory */
   uint32_t inputs;              /* vertex: attribute inputs */
   uint32_t input_locations[32]; /* vertex: their locations, in vertex-element order */
};
void sg_shader_info(struct sg_shader *shader, struct sg_shader_info *out);

/* ---- fixed-function state as plain values (the shim makes and caches the CSOs) ---- */

struct sg_blend_rt {               /* Vulkan's VkPipelineColorBlendAttachmentState */
   uint32_t enable;
   uint32_t src_color, dst_color, color_op;   /* VkBlendFactor, VkBlendOp */
   uint32_t src_alpha, dst_alpha, alpha_op;
   uint32_t write_mask;                       /* VkColorComponentFlags: R 1, G 2, B 4, A 8 */
};
struct sg_raster {                 /* Metal's encoder state */
   uint32_t cull;                  /* MTLCullMode */
   uint32_t winding;               /* MTLWinding */
   uint32_t fill;                  /* MTLTriangleFillMode */
   uint32_t depth_clip;            /* 1 clip (Metal's default), 0 clamp */
   uint32_t samples;
   float depth_bias, depth_bias_slope, depth_bias_clamp;
};
struct sg_stencil_face {           /* MTLStencilDescriptor */
   uint32_t compare;               /* MTLCompareFunction */
   uint32_t fail, depth_fail, pass; /* MTLStencilOperation */
   uint32_t read_mask, write_mask;
};
struct sg_depth_stencil {          /* MTLDepthStencilDescriptor */
   uint32_t compare;               /* MTLCompareFunction; Always with write 0: no depth test */
   uint32_t write;
   uint32_t stencil_enable;
   struct sg_stencil_face front, back;
};
struct sg_vertex_element {
   uint32_t vk_format;
   uint32_t offset;
   uint32_t buffer;                /* vertex buffer index */
   uint32_t stride;
   uint32_t divisor;               /* 0 per vertex, n per n instances */
};

/* ---- replay (sg_ctx_*: hold sg_lock) ---- */

/* The push block of `stage` (constant buffer 0), copied now. */
void sg_ctx_set_push(struct sg_device *dev, uint32_t stage, const void *bytes, uint32_t size);
void sg_ctx_set_ubo(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_buffer *buf,
                    uint64_t offset, uint32_t size);
void sg_ctx_set_ssbo(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_buffer *buf,
                     uint64_t offset, uint32_t size, bool writable);
void sg_ctx_set_view(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_view *view);
void sg_ctx_set_sampler(struct sg_device *dev, uint32_t stage, uint32_t slot,
                        struct sg_sampler *sampler);
/* A storage image (one level, a layer range) or, with buf, a storage texel buffer. */
void sg_ctx_set_image(struct sg_device *dev, uint32_t stage, uint32_t slot,
                      struct sg_texture *tex, struct sg_buffer *buf, uint32_t vk_format,
                      uint32_t level, uint32_t first_layer, uint32_t last_layer,
                      uint64_t offset, uint64_t size, bool writable);
/* Unbinds every binding of every stage (before each command's own). */
void sg_ctx_reset_bindings(struct sg_device *dev);

/* Runs `grid` workgroups of the shader's specialized local size. */
bool sg_ctx_dispatch(struct sg_device *dev, struct sg_shader *cs, const uint32_t grid[3],
                     uint32_t shared_bytes);

struct sg_attachment {
   struct sg_texture *tex;   /* NULL: none */
   uint32_t vk_format;       /* view format, 0: the texture's */
   uint32_t level, first_layer, last_layer;
   uint32_t load;            /* MTLLoadAction: 0 don't care, 1 load, 2 clear */
   double clear[4];          /* color; depth in [0]; stencil in [0] of the stencil attachment */
};
struct sg_pass {
   struct sg_attachment color[8];
   struct sg_attachment depth, stencil;
   uint32_t width, height, layers, samples;
};
/* Sets the framebuffer and performs the load actions. */
bool sg_ctx_begin_pass(struct sg_device *dev, const struct sg_pass *pass);
void sg_ctx_end_pass(struct sg_device *dev);

/* The store action MTLStoreAction(StoreAnd)MultisampleResolve of one attachment, after its pass
 * ended (sg_ctx_end_pass): `src` (the multisampled attachment, one level and layer) resolved into
 * the single-sampled `dst` over the rectangle (0, 0, width, height), clipped to both levels. It
 * runs on the GPU as Gallium's resolve, pipe->blit from the multisampled resource with the nearest
 * filter: color averages the samples, depth and stencil take sample 0 (Metal's
 * MTLMultisampleDepthResolveFilterSample0 and MTLMultisampleStencilResolveFilterSample0; freedreno
 * a5xx does it with u_blitter's resolve shaders, stencil through its stencil fallback). Where the
 * driver fakes multisampling (softpipe: sg_texture_new made `src` single-sampled) it is a copy.
 * The source keeps its samples (Metal's MultisampleResolve leaves them undefined: kept is one
 * answer). */
#define SG_ASPECT_COLOR 1u
#define SG_ASPECT_DEPTH 2u
#define SG_ASPECT_STENCIL 4u
struct sg_resolve {
   struct sg_texture *src, *dst;
   uint32_t src_format, src_level, src_layer; /* format: VkFormat of the view, 0: the texture's */
   uint32_t dst_format, dst_level, dst_layer; /* layer: the array slice, or a 3D level's depth plane */
   uint32_t width, height;
   uint32_t aspects;                          /* SG_ASPECT_* */
};
bool sg_ctx_resolve(struct sg_device *dev, const struct sg_resolve *resolve);

struct sg_draw {
   struct sg_shader *vs, *fs;
   const struct sg_blend_rt *blend;     /* blend_count entries, one per color attachment */
   uint32_t blend_count;
   uint32_t alpha_to_coverage, alpha_to_one;
   float blend_color[4];
   const struct sg_raster *raster;
   const struct sg_depth_stencil *depth_stencil;  /* NULL: Metal's default (no test) */
   uint32_t stencil_ref[2];             /* front, back */
   double viewport[6];                  /* Metal: originX, originY, width, height, znear, zfar */
   uint32_t scissor[4];                 /* x, y, width, height */
   const struct sg_vertex_element *elements;
   uint32_t element_count;
   struct sg_buffer *const *vertex_buffers; /* vertex_buffer_count entries, NULL allowed */
   const uint64_t *vertex_offsets;
   uint32_t vertex_buffer_count;
   uint32_t primitive;                  /* MTLPrimitiveType */
   uint32_t primitive_restart;
   uint32_t first_vertex, vertex_count; /* vertex_count: indices for an indexed draw */
   uint32_t first_instance, instance_count;
   uint32_t index_size;                 /* 0 direct; 2 or 4 indexed */
   struct sg_buffer *index_buffer;
   uint64_t index_offset;               /* bytes */
   int32_t base_vertex;
   uint32_t texture_barrier;            /* framebuffer fetch: a texture barrier first */
};
bool sg_ctx_draw(struct sg_device *dev, const struct sg_draw *draw);

/* Blits (MTLBlitCommandEncoder): on the GPU where Gallium has the operation, else a CPU copy
 * after waiting for the GPU (counted in sg_stats.cpu_copies). */
bool sg_ctx_copy_buffer(struct sg_device *dev, struct sg_buffer *dst, uint64_t dst_offset,
                        struct sg_buffer *src, uint64_t src_offset, uint64_t size);
bool sg_ctx_fill_buffer(struct sg_device *dev, struct sg_buffer *buf, uint64_t offset,
                        uint64_t size, uint8_t value);
bool sg_ctx_copy_texture(struct sg_device *dev, struct sg_texture *dst, uint32_t dst_level,
                         uint32_t dst_layer, const uint32_t dst_origin[3],
                         struct sg_texture *src, uint32_t src_level, uint32_t src_layer,
                         const struct sg_box *src_box);
bool sg_ctx_texture_to_buffer(struct sg_device *dev, struct sg_texture *src, uint32_t level,
                              uint32_t layer, const struct sg_box *box, struct sg_buffer *dst,
                              uint64_t offset, uint32_t stride, uint32_t layer_stride);
bool sg_ctx_buffer_to_texture(struct sg_device *dev, struct sg_buffer *src, uint64_t offset,
                              uint32_t stride, uint32_t layer_stride, struct sg_texture *dst,
                              uint32_t level, uint32_t layer, const struct sg_box *box);
bool sg_ctx_generate_mipmaps(struct sg_device *dev, struct sg_texture *tex);
/* CPU copy between a buffer and host memory once the GPU is idle (the runtime's shadow of
 * no-copy host memory). */
bool sg_ctx_sync_host(struct sg_device *dev, struct sg_buffer *buf, void *host, uint64_t size,
                      bool to_host);
void sg_ctx_memory_barrier(struct sg_device *dev);

/* Metal's setVisibilityResultMode:offset: inside a pass: from here on the samples that pass the
 * depth and stencil tests are counted for the 64-bit word at `offset` of `buf` (mode 1 Boolean,
 * 2 Counting), or no longer (0). The pass's end stops counting. Each word is written once the
 * flush's fence signaled (sg_fence_resolve): the sum of the counts aimed at it. */
bool sg_ctx_visibility(struct sg_device *dev, struct sg_buffer *buf, uint64_t offset, uint32_t mode);

/* Submits what was replayed; the fence signals when the GPU finished it. */
struct sg_fence *sg_ctx_flush(struct sg_device *dev);

/* ---- fences (any thread) ---- */

/* 1 signaled, 0 timed out, -1 error. */
int sg_fence_wait(struct sg_device *dev, struct sg_fence *fence, uint64_t timeout_ns);
void sg_fence_free(struct sg_device *dev, struct sg_fence *fence);
/* After the fence signaled: writes its visibility results into their buffers (takes the device
 * lock; the buffers must still exist). Nothing to do: true at once. */
bool sg_fence_resolve(struct sg_device *dev, struct sg_fence *fence);

#ifdef __cplusplus
}
#endif

#endif
