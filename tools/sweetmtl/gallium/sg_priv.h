/*
 * sweetmtl Gallium shim: private structures (sg.h is the interface).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SWEETMTL_SG_PRIV_H
#define SWEETMTL_SG_PRIV_H

#include "sg.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/simple_mtx.h"
#include "util/u_inlines.h"

struct cso_context;
struct fd_bo;

/* one occlusion query of a pass and the buffer word its samples count into */
struct sg_vis {
   struct pipe_query *query;
   struct sg_buffer *buf;
   uint64_t offset;
};

struct sg_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   struct cso_context *cso;
   int fd;
   /* freedreno (the a540, or drm-shim); false: softpipe, the PC's validation driver that runs
    * the commands on the CPU (SG_DRIVER=softpipe, hosts built with SG_WITH_SOFTPIPE) */
   bool freedreno;
   uint64_t next_fake_iova;
   uint32_t gpu_id;
   char name[64];
   simple_mtx_t lock;
   struct sg_stats stats;
   bool in_pass;
   /* Metal visibility results: the occlusion query counting now and where its count goes, and the
    * ended ones the next sg_ctx_flush hands to its fence (sg_fence_resolve writes them) */
   struct sg_vis vis;
   struct sg_vis *vis_done;
   unsigned vis_count, vis_cap;
   /* Copies of the push blocks: Gallium reads a user constant buffer when a command is
    * emitted, after the runtime's call returned. */
   uint8_t push[3][256];
};

struct sg_buffer {
   struct sg_device *dev;
   struct pipe_resource *res;
   struct pipe_transfer *xfer; /* softpipe: the mapping kept for the buffer's life */
   void *map;
   uint64_t size;
   uint64_t iova;
};

struct sg_texture {
   struct sg_device *dev;
   struct pipe_resource *res;
};

struct sg_view {
   struct sg_device *dev;
   struct pipe_sampler_view *view;
};

struct sg_sampler {
   struct sg_device *dev;
   void *cso;
};

struct sg_shader {
   struct sg_device *dev;
   uint32_t stage;
   void *cso;
   struct sg_shader_info info;
};

struct sg_fence {
   struct pipe_fence_handle *fence;
   struct sg_vis *vis; /* visibility queries to write out once it signals */
   unsigned vis_count;
};

/* sg_replay.c: ends and frees a visibility query left counting outside a pass */
void sg_vis_drop(struct sg_device *dev);

/* sg_device.c */
void sg_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
mesa_shader_stage sg_mesa_stage(uint32_t stage);

/* sg_resource.c */
struct fd_bo *sg_resource_bo(struct pipe_resource *res);
bool sg_read_locked(struct sg_device *dev, struct pipe_resource *res, uint32_t level,
                    uint32_t layer, const struct sg_box *box, void *data, uint32_t stride,
                    uint32_t layer_stride);

/* sg_format.c */
uint32_t sg_pipe_format(uint32_t vk_format);

/* sg_map.c: -1 where Gallium has no counterpart */
int sg_target_from_metal(uint32_t type);
unsigned sg_bind_from_usage(uint32_t vk_format, uint32_t usage);
int sg_swizzle_from_metal(uint8_t swizzle);
int sg_func_from_metal(uint32_t func);
int sg_stencil_op_from_metal(uint32_t op);
int sg_wrap_from_metal(uint32_t mode);
int sg_prim_from_metal(uint32_t prim);
int sg_blend_factor_from_vk(uint32_t factor);
int sg_blend_op_from_vk(uint32_t op);
unsigned sg_colormask_from_vk(uint32_t mask);

#endif
