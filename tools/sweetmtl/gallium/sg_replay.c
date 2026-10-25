/*
 * sweetmtl Gallium shim: command replay into the device's pipe_context.
 *
 * The runtime replays a committed command buffer under sg_lock(): bindings (sg_ctx_set_*),
 * then a dispatch, a pass with its draws, or a blit, command by command in Metal's order, then
 * sg_ctx_flush. State follows Vulkan's conventions as lavapipe maps them onto Gallium
 * (frontends/lavapipe/lvp_execute.c): clip_halfz, half-pixel centers, the first vertex
 * provoking, scissoring always on. Metal's viewport (y down, origin top-left) becomes the Vulkan
 * viewport of negative height that metal2vulkan-runtime makes of it (render_state.rs).
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sg_priv.h"

#include "cso_cache/cso_context.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/box.h"
#include "util/u_gen_mipmap.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"

#define SG_MAX_UBOS 16
#define SG_MAX_SSBOS 24
#define SG_MAX_VIEWS 16
#define SG_MAX_SAMPLERS 16
#define SG_MAX_IMAGES 24

void
sg_ctx_set_push(struct sg_device *dev, uint32_t stage, const void *bytes, uint32_t size)
{
   if (stage > SG_COMPUTE)
      return;
   if (size > sizeof(dev->push[stage]))
      size = sizeof(dev->push[stage]);
   if (size)
      memcpy(dev->push[stage], bytes, size);
   struct pipe_constant_buffer cb = {
      .buffer = NULL,
      .buffer_offset = 0,
      .buffer_size = size,
      .user_buffer = dev->push[stage],
   };
   dev->pipe->set_constant_buffer(dev->pipe, sg_mesa_stage(stage), 0, size ? &cb : NULL);
}

void
sg_ctx_set_ubo(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_buffer *buf,
               uint64_t offset, uint32_t size)
{
   struct pipe_constant_buffer cb = {
      .buffer = buf ? buf->res : NULL,
      .buffer_offset = (unsigned)offset,
      .buffer_size = size,
   };
   dev->pipe->set_constant_buffer(dev->pipe, sg_mesa_stage(stage), slot, buf ? &cb : NULL);
}

void
sg_ctx_set_ssbo(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_buffer *buf,
                uint64_t offset, uint32_t size, bool writable)
{
   struct pipe_shader_buffer sb = {
      .buffer = buf ? buf->res : NULL,
      .buffer_offset = (unsigned)offset,
      .buffer_size = size,
   };
   dev->pipe->set_shader_buffers(dev->pipe, sg_mesa_stage(stage), slot, 1, buf ? &sb : NULL,
                                 writable ? 1 : 0);
}

void
sg_ctx_set_view(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_view *view)
{
   struct pipe_sampler_view *v = view ? view->view : NULL;
   dev->pipe->set_sampler_views(dev->pipe, sg_mesa_stage(stage), slot, 1, 0, &v);
}

void
sg_ctx_set_sampler(struct sg_device *dev, uint32_t stage, uint32_t slot,
                   struct sg_sampler *sampler)
{
   void *s = sampler ? sampler->cso : NULL;
   dev->pipe->bind_sampler_states(dev->pipe, sg_mesa_stage(stage), slot, 1, &s);
}

void
sg_ctx_set_image(struct sg_device *dev, uint32_t stage, uint32_t slot, struct sg_texture *tex,
                 struct sg_buffer *buf, uint32_t vk_format, uint32_t level, uint32_t first_layer,
                 uint32_t last_layer, uint64_t offset, uint64_t size, bool writable)
{
   if (!tex && !buf) {
      dev->pipe->set_shader_images(dev->pipe, sg_mesa_stage(stage), slot, 0, 1, NULL);
      return;
   }
   struct pipe_image_view iv;
   memset(&iv, 0, sizeof(iv));
   iv.resource = tex ? tex->res : buf->res;
   iv.format = vk_format ? (enum pipe_format)sg_pipe_format(vk_format) : iv.resource->format;
   iv.access = iv.shader_access =
      PIPE_IMAGE_ACCESS_READ | (writable ? PIPE_IMAGE_ACCESS_WRITE : 0);
   if (tex) {
      iv.u.tex.level = level;
      iv.u.tex.first_layer = first_layer;
      iv.u.tex.last_layer = last_layer;
   } else {
      iv.u.buf.offset = (unsigned)offset;
      iv.u.buf.size = (unsigned)size;
   }
   dev->pipe->set_shader_images(dev->pipe, sg_mesa_stage(stage), slot, 1, 0, &iv);
}

void
sg_ctx_reset_bindings(struct sg_device *dev)
{
   struct pipe_context *p = dev->pipe;
   void *nulls[SG_MAX_SAMPLERS] = {0};
   for (uint32_t stage = SG_VERTEX; stage <= SG_COMPUTE; stage++) {
      mesa_shader_stage s = sg_mesa_stage(stage);
      for (unsigned i = 0; i < SG_MAX_UBOS; i++)
         p->set_constant_buffer(p, s, i, NULL);
      /* a5xx has no vertex-stage shader buffers or images to unbind */
      if (stage != SG_VERTEX) {
         p->set_shader_buffers(p, s, 0, SG_MAX_SSBOS, NULL, 0);
         p->set_shader_images(p, s, 0, 0, SG_MAX_IMAGES, NULL);
      }
      p->set_sampler_views(p, s, 0, 0, SG_MAX_VIEWS, NULL);
      p->bind_sampler_states(p, s, 0, SG_MAX_SAMPLERS, nulls);
   }
}

bool
sg_ctx_dispatch(struct sg_device *dev, struct sg_shader *cs, const uint32_t grid[3],
                uint32_t shared_bytes)
{
   if (!cs || cs->stage != SG_COMPUTE) {
      sg_set_error("dispatch without a compute shader");
      return false;
   }
   if (!grid[0] || !grid[1] || !grid[2])
      return true; /* nothing runs, as on Metal */
   cso_set_compute_shader_handle(dev->cso, cs->cso);
   struct pipe_grid_info info;
   memset(&info, 0, sizeof(info));
   info.work_dim = 3;
   for (int i = 0; i < 3; i++) {
      info.block[i] = cs->info.workgroup[i] ? cs->info.workgroup[i] : 1;
      info.grid[i] = grid[i];
   }
   info.variable_shared_mem = shared_bytes;
   dev->pipe->launch_grid(dev->pipe, &info);
   dev->stats.dispatches++;
   return true;
}

static bool
fill_surface(struct pipe_surface *s, const struct sg_attachment *a)
{
   memset(s, 0, sizeof(*s));
   s->texture = a->tex->res;
   s->format = a->vk_format ? (enum pipe_format)sg_pipe_format(a->vk_format) : a->tex->res->format;
   if (s->format == PIPE_FORMAT_NONE) {
      sg_set_error("attachment of VkFormat %u: no Gallium format", a->vk_format);
      return false;
   }
   s->level = a->level;
   s->first_layer = a->first_layer;
   s->last_layer = a->last_layer >= a->first_layer ? a->last_layer : a->first_layer;
   return true;
}

static union pipe_color_union
clear_color(enum pipe_format format, const double c[4])
{
   union pipe_color_union u;
   for (int i = 0; i < 4; i++) {
      if (util_format_is_pure_uint(format))
         u.ui[i] = c[i] <= 0 ? 0 : (c[i] >= 4294967295.0 ? 0xffffffffu : (uint32_t)c[i]);
      else if (util_format_is_pure_sint(format))
         u.i[i] = (int32_t)c[i];
      else
         u.f[i] = (float)c[i];
   }
   return u;
}

#define LOAD_CLEAR 2 /* MTLLoadActionClear */

bool
sg_ctx_begin_pass(struct sg_device *dev, const struct sg_pass *pass)
{
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width = pass->width;
   fb.height = pass->height;
   fb.layers = pass->layers ? pass->layers : 1;
   fb.samples = pass->samples > 1 ? pass->samples : 0;
   for (int i = 0; i < 8; i++) {
      if (!pass->color[i].tex)
         continue;
      if (!fill_surface(&fb.cbufs[i], &pass->color[i]))
         return false;
      fb.nr_cbufs = i + 1;
   }
   if (pass->depth.tex && pass->stencil.tex && pass->depth.tex != pass->stencil.tex) {
      sg_set_error("separate depth and stencil textures in one pass (not yet: a5xx binds one "
                   "depth/stencil surface)");
      return false;
   }
   const struct sg_attachment *zs =
      pass->depth.tex ? &pass->depth : pass->stencil.tex ? &pass->stencil : NULL;
   if (zs && !fill_surface(&fb.zsbuf, zs))
      return false;
   cso_set_framebuffer(dev->cso, &fb);
   dev->in_pass = true;

   for (int i = 0; i < 8; i++) {
      const struct sg_attachment *a = &pass->color[i];
      if (!a->tex || a->load != LOAD_CLEAR)
         continue;
      union pipe_color_union color = clear_color(fb.cbufs[i].format, a->clear);
      dev->pipe->clear(dev->pipe, PIPE_CLEAR_COLOR0 << i, 0xffffffffu, 0, NULL, &color, 0, 0);
      dev->stats.clears++;
   }
   unsigned zs_clear = 0;
   double depth = 0;
   unsigned stencil = 0;
   if (pass->depth.tex && pass->depth.load == LOAD_CLEAR) {
      zs_clear |= PIPE_CLEAR_DEPTH;
      depth = pass->depth.clear[0];
   }
   if (pass->stencil.tex && pass->stencil.load == LOAD_CLEAR) {
      zs_clear |= PIPE_CLEAR_STENCIL;
      stencil = (unsigned)pass->stencil.clear[0];
   }
   if (zs_clear) {
      union pipe_color_union none;
      memset(&none, 0, sizeof(none));
      dev->pipe->clear(dev->pipe, zs_clear, 0, 0xff, NULL, &none, depth, stencil);
      dev->stats.clears++;
   }
   return true;
}

void
sg_ctx_end_pass(struct sg_device *dev)
{
   dev->in_pass = false;
}

static bool
make_blend(const struct sg_draw *d, struct pipe_blend_state *blend)
{
   memset(blend, 0, sizeof(*blend));
   blend->independent_blend_enable = 1;
   blend->alpha_to_coverage = d->alpha_to_coverage;
   blend->alpha_to_one = d->alpha_to_one;
   blend->max_rt = d->blend_count ? d->blend_count - 1 : 0;
   for (uint32_t i = 0; i < d->blend_count && i < PIPE_MAX_COLOR_BUFS; i++) {
      const struct sg_blend_rt *b = &d->blend[i];
      struct pipe_rt_blend_state *rt = &blend->rt[i];
      rt->colormask = sg_colormask_from_vk(b->write_mask);
      if (!b->enable)
         continue;
      int sc = sg_blend_factor_from_vk(b->src_color), dc = sg_blend_factor_from_vk(b->dst_color),
          sa = sg_blend_factor_from_vk(b->src_alpha), da = sg_blend_factor_from_vk(b->dst_alpha),
          oc = sg_blend_op_from_vk(b->color_op), oa = sg_blend_op_from_vk(b->alpha_op);
      if (sc < 0 || dc < 0 || sa < 0 || da < 0 || oc < 0 || oa < 0) {
         sg_set_error("blend state of attachment %u without a Gallium counterpart", i);
         return false;
      }
      rt->blend_enable = 1;
      rt->rgb_src_factor = sc;
      rt->rgb_dst_factor = dc;
      rt->rgb_func = oc;
      rt->alpha_src_factor = sa;
      rt->alpha_dst_factor = da;
      rt->alpha_func = oa;
   }
   return true;
}

static bool
make_dsa(const struct sg_depth_stencil *ds, struct pipe_depth_stencil_alpha_state *dsa)
{
   memset(dsa, 0, sizeof(*dsa));
   if (!ds)
      return true; /* Metal's default: compare Always without writes, no stencil */
   int df = sg_func_from_metal(ds->compare);
   if (df < 0) {
      sg_set_error("depth compare function %u", ds->compare);
      return false;
   }
   if (df != PIPE_FUNC_ALWAYS || ds->write) {
      dsa->depth_enabled = 1;
      dsa->depth_writemask = ds->write ? 1 : 0;
      dsa->depth_func = df;
   }
   if (ds->stencil_enable) {
      const struct sg_stencil_face *f[2] = {&ds->front, &ds->back};
      for (int i = 0; i < 2; i++) {
         int fn = sg_func_from_metal(f[i]->compare), fo = sg_stencil_op_from_metal(f[i]->fail),
             zo = sg_stencil_op_from_metal(f[i]->depth_fail),
             po = sg_stencil_op_from_metal(f[i]->pass);
         if (fn < 0 || fo < 0 || zo < 0 || po < 0) {
            sg_set_error("stencil state without a Gallium counterpart");
            return false;
         }
         dsa->stencil[i].enabled = 1;
         dsa->stencil[i].func = fn;
         dsa->stencil[i].fail_op = fo;
         dsa->stencil[i].zfail_op = zo;
         dsa->stencil[i].zpass_op = po;
         dsa->stencil[i].valuemask = f[i]->read_mask & 0xff;
         dsa->stencil[i].writemask = f[i]->write_mask & 0xff;
      }
   }
   return true;
}

static void
make_raster(const struct sg_raster *r, struct pipe_rasterizer_state *rs)
{
   memset(rs, 0, sizeof(*rs));
   rs->flatshade_first = 1;
   rs->clip_halfz = 1;
   rs->half_pixel_center = 1;
   rs->scissor = 1;
   rs->point_size_per_vertex = 1;
   rs->point_quad_rasterization = 1;
   rs->point_line_tri_clip = 1;
   rs->no_ms_sample_mask_out = 1;
   rs->line_width = 1.0f;
   rs->point_size = 1.0f;
   rs->depth_clip_near = rs->depth_clip_far = 1;
   rs->fill_front = rs->fill_back = PIPE_POLYGON_MODE_FILL;
   /* MTLWinding: 0 clockwise (Metal's default), 1 counter-clockwise; facing is taken in the
    * framebuffer's y-down coordinates on both sides, as Vulkan's (lavapipe's front_ccw). */
   rs->front_ccw = 0;
   if (!r)
      return;
   rs->cull_face = r->cull == 1 ? PIPE_FACE_FRONT : r->cull == 2 ? PIPE_FACE_BACK : PIPE_FACE_NONE;
   rs->front_ccw = r->winding == 1;
   rs->fill_front = rs->fill_back = r->fill == 1 ? PIPE_POLYGON_MODE_LINE : PIPE_POLYGON_MODE_FILL;
   rs->depth_clip_near = rs->depth_clip_far = r->depth_clip ? 1 : 0;
   rs->depth_clamp = !r->depth_clip;
   rs->multisample = r->samples > 1;
   if (r->depth_bias != 0 || r->depth_bias_slope != 0) {
      rs->offset_tri = rs->offset_line = rs->offset_point = 1;
      rs->offset_units = r->depth_bias;
      rs->offset_scale = r->depth_bias_slope;
      rs->offset_clamp = r->depth_bias_clamp;
   }
}

bool
sg_ctx_draw(struct sg_device *dev, const struct sg_draw *d)
{
   if (!dev->in_pass) {
      sg_set_error("draw outside a render pass");
      return false;
   }
   if (!d->vs || d->vs->stage != SG_VERTEX) {
      sg_set_error("draw without a vertex shader");
      return false;
   }
   if (!d->fs || d->fs->stage != SG_FRAGMENT) {
      sg_set_error("draws without a fragment function (rasterization off) are not yet "
                   "implemented");
      return false;
   }
   if (d->first_instance) {
      sg_set_error("baseInstance %u: a5xx has no start instance (not yet emulated)",
                   d->first_instance);
      return false;
   }
   int prim = sg_prim_from_metal(d->primitive);
   if (prim < 0) {
      sg_set_error("primitive type %u", d->primitive);
      return false;
   }
   struct pipe_blend_state blend;
   struct pipe_depth_stencil_alpha_state dsa;
   struct pipe_rasterizer_state rs;
   if (!make_blend(d, &blend) || !make_dsa(d->depth_stencil, &dsa))
      return false;
   make_raster(d->raster, &rs);

   struct cso_velems_state ve;
   memset(&ve, 0, sizeof(ve));
   if (d->element_count > PIPE_MAX_ATTRIBS) {
      sg_set_error("%u vertex elements (a5xx takes %u)", d->element_count, PIPE_MAX_ATTRIBS);
      return false;
   }
   ve.count = d->element_count;
   for (uint32_t i = 0; i < ve.count; i++) {
      const struct sg_vertex_element *e = &d->elements[i];
      enum pipe_format f = (enum pipe_format)sg_pipe_format(e->vk_format);
      if (f == PIPE_FORMAT_NONE) {
         sg_set_error("vertex attribute of VkFormat %u: no Gallium format", e->vk_format);
         return false;
      }
      ve.velems[i].src_offset = (uint16_t)e->offset;
      ve.velems[i].vertex_buffer_index = e->buffer;
      ve.velems[i].src_format = f;
      ve.velems[i].src_stride = e->stride;
      ve.velems[i].instance_divisor = e->divisor;
   }

   if (d->texture_barrier && dev->pipe->texture_barrier)
      dev->pipe->texture_barrier(dev->pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

   cso_set_vertex_shader_handle(dev->cso, d->vs->cso);
   cso_set_fragment_shader_handle(dev->cso, d->fs->cso);
   cso_set_blend(dev->cso, &blend);
   struct pipe_blend_color bc;
   memcpy(bc.color, d->blend_color, sizeof(bc.color));
   dev->pipe->set_blend_color(dev->pipe, &bc);
   cso_set_rasterizer(dev->cso, &rs);
   cso_set_depth_stencil_alpha(dev->cso, &dsa);
   struct pipe_stencil_ref ref;
   ref.ref_value[0] = (uint8_t)d->stencil_ref[0];
   ref.ref_value[1] = (uint8_t)d->stencil_ref[1];
   cso_set_stencil_ref(dev->cso, ref);

   /* Metal's viewport as the negative-height Vulkan viewport (y' = y + h, h' = -h) in lavapipe's
    * scale and translate. */
   const double *v = d->viewport;
   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0] = (float)(v[2] * 0.5);
   vp.translate[0] = (float)(v[0] + v[2] * 0.5);
   vp.scale[1] = (float)(-v[3] * 0.5);
   vp.translate[1] = (float)(v[1] + v[3] * 0.5);
   vp.scale[2] = (float)(v[5] - v[4]);
   vp.translate[2] = (float)v[4];
   vp.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
   vp.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
   vp.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
   vp.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
   cso_set_viewport(dev->cso, &vp);
   struct pipe_scissor_state sc = {
      .minx = d->scissor[0],
      .miny = d->scissor[1],
      .maxx = d->scissor[0] + d->scissor[2],
      .maxy = d->scissor[1] + d->scissor[3],
   };
   dev->pipe->set_scissor_states(dev->pipe, 0, 1, &sc);

   cso_set_vertex_elements(dev->cso, &ve);
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS];
   memset(vbs, 0, sizeof(vbs));
   uint32_t nvb = MIN2(d->vertex_buffer_count, PIPE_MAX_ATTRIBS);
   for (uint32_t i = 0; i < nvb; i++) {
      vbs[i].is_user_buffer = false;
      vbs[i].buffer.resource = d->vertex_buffers[i] ? d->vertex_buffers[i]->res : NULL;
      vbs[i].buffer_offset = d->vertex_buffers[i] ? (unsigned)d->vertex_offsets[i] : 0;
   }
   cso_set_vertex_buffers(dev->cso, nvb, nvb ? vbs : NULL);

   struct pipe_draw_info info;
   memset(&info, 0, sizeof(info));
   info.mode = (enum mesa_prim)prim;
   info.instance_count = d->instance_count;
   info.start_instance = d->first_instance;
   struct pipe_draw_start_count_bias draw;
   memset(&draw, 0, sizeof(draw));
   if (d->index_size) {
      if (!d->index_buffer || (d->index_size != 2 && d->index_size != 4) ||
          d->index_offset % d->index_size) {
         sg_set_error("indexed draw: bad index buffer, size or offset");
         return false;
      }
      info.index_size = d->index_size;
      info.index.resource = d->index_buffer->res;
      info.primitive_restart = d->primitive_restart ? 1 : 0;
      info.restart_index = d->index_size == 2 ? 0xffff : 0xffffffffu;
      draw.start = (unsigned)(d->index_offset / d->index_size);
      draw.count = d->vertex_count;
      draw.index_bias = d->base_vertex;
   } else {
      draw.start = d->first_vertex;
      draw.count = d->vertex_count;
   }
   if (draw.count && info.instance_count) {
      cso_draw_vbo(dev->cso, &info, 0, NULL, &draw, 1);
      dev->stats.draws++;
   }
   return true;
}

/* Waits for every submitted and recorded command (before a CPU copy). */
static void
sync_all(struct sg_device *dev)
{
   struct pipe_fence_handle *fence = NULL;
   dev->pipe->flush(dev->pipe, &fence, 0);
   if (fence) {
      dev->screen->fence_finish(dev->screen, NULL, fence, OS_TIMEOUT_INFINITE);
      dev->screen->fence_reference(dev->screen, &fence, NULL);
   }
}

bool
sg_ctx_copy_buffer(struct sg_device *dev, struct sg_buffer *dst, uint64_t dst_offset,
                   struct sg_buffer *src, uint64_t src_offset, uint64_t size)
{
   if (!size)
      return true;
   if (dst_offset + size > dst->size || src_offset + size > src->size) {
      sg_set_error("buffer copy past a buffer's end");
      return false;
   }
   struct pipe_box box;
   u_box_1d((int)src_offset, (int)size, &box);
   dev->pipe->resource_copy_region(dev->pipe, dst->res, 0, (unsigned)dst_offset, 0, 0, src->res,
                                   0, &box);
   dev->stats.blits++;
   return true;
}

bool
sg_ctx_fill_buffer(struct sg_device *dev, struct sg_buffer *buf, uint64_t offset, uint64_t size,
                   uint8_t value)
{
   if (!size)
      return true;
   if (offset + size > buf->size) {
      sg_set_error("buffer fill past the buffer's end");
      return false;
   }
   if (dev->pipe->clear_buffer) {
      dev->pipe->clear_buffer(dev->pipe, buf->res, (unsigned)offset, (unsigned)size, &value, 1);
      dev->stats.blits++;
      return true;
   }
   /* a5xx has no clear_buffer: wait for the GPU, then fill through the mapping */
   sync_all(dev);
   memset((uint8_t *)buf->map + offset, value, size);
   dev->stats.cpu_copies++;
   return true;
}

bool
sg_ctx_copy_texture(struct sg_device *dev, struct sg_texture *dst, uint32_t dst_level,
                    uint32_t dst_layer, const uint32_t dst_origin[3], struct sg_texture *src,
                    uint32_t src_level, uint32_t src_layer, const struct sg_box *src_box)
{
   struct pipe_box box;
   if (src->res->target == PIPE_TEXTURE_3D)
      u_box_3d(src_box->x, src_box->y, src_box->z, src_box->width, src_box->height,
               src_box->depth ? src_box->depth : 1, &box);
   else
      u_box_3d(src_box->x, src_box->y, src_layer, src_box->width, src_box->height,
               src_box->depth ? src_box->depth : 1, &box);
   unsigned dz = dst->res->target == PIPE_TEXTURE_3D ? dst_origin[2] : dst_layer;
   dev->pipe->resource_copy_region(dev->pipe, dst->res, dst_level, dst_origin[0], dst_origin[1],
                                   dz, src->res, src_level, &box);
   dev->stats.blits++;
   return true;
}

bool
sg_ctx_texture_to_buffer(struct sg_device *dev, struct sg_texture *src, uint32_t level,
                         uint32_t layer, const struct sg_box *box, struct sg_buffer *dst,
                         uint64_t offset, uint32_t stride, uint32_t layer_stride)
{
   uint64_t rows = util_format_get_nblocksy(src->res->format, box->height);
   uint64_t need = offset + (uint64_t)layer_stride * ((box->depth ? box->depth : 1) - 1) +
                   (uint64_t)stride * (rows ? rows - 1 : 0) +
                   util_format_get_stride(src->res->format, box->width);
   if (need > dst->size) {
      sg_set_error("texture to buffer copy past the buffer's end");
      return false;
   }
   /* A CPU copy once the GPU is idle (the buffer may be in use by earlier work). The GPU path,
    * a blit into a linear texture over the buffer (rows aligned as a5xx lays them), is next. */
   sync_all(dev);
   bool ok = sg_read_locked(dev, src->res, level, layer, box, (uint8_t *)dst->map + offset,
                            stride, layer_stride);
   dev->stats.cpu_copies++;
   return ok;
}

bool
sg_ctx_buffer_to_texture(struct sg_device *dev, struct sg_buffer *src, uint64_t offset,
                         uint32_t stride, uint32_t layer_stride, struct sg_texture *dst,
                         uint32_t level, uint32_t layer, const struct sg_box *box)
{
   struct pipe_box b;
   if (dst->res->target == PIPE_TEXTURE_3D)
      u_box_3d(box->x, box->y, box->z, box->width, box->height, box->depth ? box->depth : 1, &b);
   else
      u_box_3d(box->x, box->y, layer, box->width, box->height, box->depth ? box->depth : 1, &b);
   sync_all(dev);
   dev->pipe->texture_subdata(dev->pipe, dst->res, level, PIPE_MAP_WRITE, &b,
                              (uint8_t *)src->map + offset, stride, layer_stride);
   dev->stats.cpu_copies++;
   return true;
}

bool
sg_ctx_generate_mipmaps(struct sg_device *dev, struct sg_texture *tex)
{
   struct pipe_resource *r = tex->res;
   if (!r->last_level)
      return true;
   unsigned last_layer = r->target == PIPE_TEXTURE_3D ? 0 : (unsigned)r->array_size - 1;
   if (!util_gen_mipmap(dev->pipe, r, r->format, 0, r->last_level, 0, last_layer,
                        PIPE_TEX_FILTER_LINEAR)) {
      sg_set_error("mipmap generation failed for %s", util_format_name(r->format));
      return false;
   }
   dev->stats.blits++;
   return true;
}

bool
sg_ctx_sync_host(struct sg_device *dev, struct sg_buffer *buf, void *host, uint64_t size,
                 bool to_host)
{
   if (size > buf->size)
      size = buf->size;
   sync_all(dev);
   if (to_host)
      memcpy(host, buf->map, size);
   else
      memcpy(buf->map, host, size);
   dev->stats.cpu_copies++;
   return true;
}

void
sg_ctx_memory_barrier(struct sg_device *dev)
{
   dev->pipe->memory_barrier(dev->pipe, PIPE_BARRIER_ALL);
}
