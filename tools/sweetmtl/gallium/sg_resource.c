/*
 * sweetmtl Gallium shim: buffers, textures, sampler views and samplers.
 *
 * Metal buffers are PIPE_BUFFER resources whose freedreno bo is mapped once and never
 * replaced, so `contents` and `gpuAddress` (the bo's iova) stay valid for the buffer's life.
 * Nothing here may make freedreno reallocate a buffer's bo: no DISCARD_WHOLE_RESOURCE
 * transfers, no invalidate_resource. CPU access to buffers goes through that mapping; CPU access
 * to textures goes through texture_map, which waits for the GPU work that writes them.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "sg_priv.h"

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "util/format/u_format.h"
#include "util/box.h"
#include "util/u_math.h"
#include "util/u_range.h"
#include "util/u_sampler.h"
#include "util/u_surface.h"

#include "freedreno_resource.h"
#include "freedreno/drm/freedreno_drmif.h"

struct fd_bo *
sg_resource_bo(struct pipe_resource *res)
{
   return fd_resource(res)->bo;
}

/* ---- buffers ---- */

struct sg_buffer *
sg_buffer_new(struct sg_device *dev, uint64_t size)
{
   if (size > UINT32_MAX) {
      sg_set_error("buffer of %llu bytes: Gallium buffers stop at 4 GiB",
                   (unsigned long long)size);
      return NULL;
   }
   if (size == 0)
      size = 16;
   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_BUFFER;
   templ.format = PIPE_FORMAT_R8_UNORM;
   templ.width0 = (uint32_t)size;
   templ.height0 = templ.depth0 = templ.array_size = 1;
   templ.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
                PIPE_BIND_SHADER_BUFFER | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHADER_IMAGE |
                PIPE_BIND_COMMAND_ARGS_BUFFER;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.flags = PIPE_RESOURCE_FLAG_MAP_PERSISTENT | PIPE_RESOURCE_FLAG_MAP_COHERENT;
   struct pipe_resource *res = dev->screen->resource_create(dev->screen, &templ);
   if (!res) {
      sg_set_error("buffer of %llu bytes: resource_create failed", (unsigned long long)size);
      return NULL;
   }
   if (!dev->freedreno) {
      /* softpipe: the buffer's memory is the CPU's and never moves; one map gives the pointer,
       * a made-up address stands for its iova */
      struct pipe_box box;
      u_box_1d(0, (int)size, &box);
      struct pipe_transfer *xfer = NULL;
      simple_mtx_lock(&dev->lock);
      void *map = dev->pipe->buffer_map(dev->pipe, res, 0,
                                        PIPE_MAP_READ_WRITE | PIPE_MAP_UNSYNCHRONIZED |
                                           PIPE_MAP_PERSISTENT | PIPE_MAP_COHERENT,
                                        &box, &xfer);
      simple_mtx_unlock(&dev->lock);
      if (!map) {
         sg_set_error("buffer map failed");
         pipe_resource_reference(&res, NULL);
         return NULL;
      }
      struct sg_buffer *buf = calloc(1, sizeof(*buf));
      buf->dev = dev;
      buf->res = res;
      buf->map = map;
      buf->xfer = xfer;
      buf->size = size;
      simple_mtx_lock(&dev->lock);
      buf->iova = dev->next_fake_iova;
      dev->next_fake_iova += (size + 0xffff) & ~0xffffull;
      simple_mtx_unlock(&dev->lock);
      return buf;
   }
   struct fd_resource *rsc = fd_resource(res);
   void *map = fd_bo_map(rsc->bo);
   if (!map) {
      sg_set_error("buffer of %llu bytes: bo map failed", (unsigned long long)size);
      pipe_resource_reference(&res, NULL);
      return NULL;
   }
   /* The CPU writes through the mapping behind freedreno's back: all of it holds data. */
   util_range_add(&rsc->b.b, &rsc->valid_buffer_range, 0, (unsigned)size);
   struct sg_buffer *buf = calloc(1, sizeof(*buf));
   buf->dev = dev;
   buf->res = res;
   buf->map = map;
   buf->size = size;
   buf->iova = fd_bo_get_iova(rsc->bo);
   return buf;
}

void
sg_buffer_free(struct sg_buffer *buf)
{
   if (!buf)
      return;
   if (buf->xfer) {
      simple_mtx_lock(&buf->dev->lock);
      buf->dev->pipe->buffer_unmap(buf->dev->pipe, buf->xfer);
      simple_mtx_unlock(&buf->dev->lock);
   }
   pipe_resource_reference(&buf->res, NULL);
   free(buf);
}

void *
sg_buffer_map(struct sg_buffer *buf)
{
   return buf->map;
}

uint64_t
sg_buffer_iova(struct sg_buffer *buf)
{
   return buf->iova;
}

uint64_t
sg_buffer_size(struct sg_buffer *buf)
{
   return buf->size;
}

/* ---- textures ---- */

static bool
texture_template(const struct sg_texture_desc *d, struct pipe_resource *t)
{
   memset(t, 0, sizeof(*t));
   int target = sg_target_from_metal(d->type);
   if (target < 0 || target == PIPE_BUFFER) {
      sg_set_error("texture type %u is not a texture Gallium makes here", d->type);
      return false;
   }
   t->target = (enum pipe_texture_target)target;
   t->format = (enum pipe_format)sg_pipe_format(d->vk_format);
   if (t->format == PIPE_FORMAT_NONE) {
      sg_set_error("VkFormat %u has no Gallium format", d->vk_format);
      return false;
   }
   t->width0 = d->width ? d->width : 1;
   t->height0 = d->height ? d->height : 1;
   t->depth0 = d->depth ? d->depth : 1;
   t->array_size = d->array_size ? d->array_size : 1;
   if (t->target == PIPE_TEXTURE_CUBE || t->target == PIPE_TEXTURE_CUBE_ARRAY)
      t->array_size *= 6;
   t->last_level = d->levels ? d->levels - 1 : 0;
   t->nr_samples = d->samples > 1 ? d->samples : 0;
   t->nr_storage_samples = t->nr_samples;
   /* Every texture can be a blit's source and destination (Metal's blits take any), so it is
    * sampleable and, where the format renders, a render target. */
   t->bind = sg_bind_from_usage(d->vk_format, d->usage) | PIPE_BIND_SAMPLER_VIEW;
   if (!util_format_is_compressed(t->format))
      t->bind |= util_format_is_depth_or_stencil(t->format) ? PIPE_BIND_DEPTH_STENCIL
                                                             : PIPE_BIND_RENDER_TARGET;
   if (d->linear)
      t->bind |= PIPE_BIND_LINEAR;
   t->usage = PIPE_USAGE_DEFAULT;
   return true;
}

static bool
supported(struct sg_device *dev, const struct pipe_resource *t)
{
   return dev->screen->is_format_supported(dev->screen, t->format, t->target, t->nr_samples,
                                           t->nr_storage_samples, t->bind & ~PIPE_BIND_LINEAR);
}

struct sg_texture *
sg_texture_new(struct sg_device *dev, const struct sg_texture_desc *desc)
{
   struct pipe_resource templ;
   if (!texture_template(desc, &templ))
      return NULL;
   if (!supported(dev, &templ)) {
      /* not every format renders or samples: keep exactly what the usage asks for */
      templ.bind = sg_bind_from_usage(desc->vk_format, desc->usage) |
                   (desc->linear ? PIPE_BIND_LINEAR : 0);
      if (!supported(dev, &templ)) {
         sg_set_error("format %s is not supported as target %u with bind 0x%x, %u samples",
                      util_format_name(templ.format), templ.target, templ.bind,
                      templ.nr_samples);
         return NULL;
      }
   }
   struct pipe_resource *res = dev->screen->resource_create(dev->screen, &templ);
   if (!res) {
      sg_set_error("texture %ux%ux%u %s: resource_create failed", templ.width0, templ.height0,
                   templ.depth0, util_format_name(templ.format));
      return NULL;
   }
   struct sg_texture *tex = calloc(1, sizeof(*tex));
   tex->dev = dev;
   tex->res = res;
   return tex;
}

struct sg_texture *
sg_texture_new_on_buffer(struct sg_device *dev, const struct sg_texture_desc *desc,
                         struct sg_buffer *buf, uint64_t offset, uint32_t stride)
{
   struct pipe_resource templ;
   if (!dev->freedreno) {
      sg_set_error("a texture over a buffer's memory is freedreno's (a bo shared by two "
                   "resources); softpipe has none");
      return NULL;
   }
   if (!texture_template(desc, &templ))
      return NULL;
   if (templ.target != PIPE_TEXTURE_2D || templ.last_level || templ.array_size > 1 ||
       templ.nr_samples) {
      sg_set_error("a texture over a buffer is one level of a single-sample 2D texture here");
      return NULL;
   }
   if (offset % 64) {
      sg_set_error("texture over a buffer at offset %llu: a5xx needs 64-byte aligned bases",
                   (unsigned long long)offset);
      return NULL;
   }
   uint64_t need = offset + (uint64_t)stride * templ.height0;
   if (need > buf->size) {
      sg_set_error("texture over a buffer: %llu bytes past the buffer's %llu",
                   (unsigned long long)need, (unsigned long long)buf->size);
      return NULL;
   }
   struct winsys_handle wh;
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_KMS;
   wh.handle = fd_bo_handle(sg_resource_bo(buf->res));
   wh.stride = stride;
   wh.offset = (unsigned)offset;
   wh.modifier = DRM_FORMAT_MOD_LINEAR;
   wh.format = templ.format;
   templ.bind |= PIPE_BIND_LINEAR;
   struct pipe_resource *res = dev->screen->resource_from_handle(
      dev->screen, &templ, &wh,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE | PIPE_HANDLE_USAGE_SHADER_WRITE);
   if (!res) {
      sg_set_error("texture %ux%u %s over a buffer, rows %u bytes apart: freedreno refused "
                   "the layout (a5xx linear pitch alignment)",
                   templ.width0, templ.height0, util_format_name(templ.format), stride);
      return NULL;
   }
   struct sg_texture *tex = calloc(1, sizeof(*tex));
   tex->dev = dev;
   tex->res = res;
   return tex;
}

void
sg_texture_free(struct sg_texture *tex)
{
   if (!tex)
      return;
   pipe_resource_reference(&tex->res, NULL);
   free(tex);
}

uint32_t
sg_texture_pitch(struct sg_texture *tex, uint32_t level)
{
   if (level > tex->res->last_level || !tex->dev->freedreno)
      return 0;
   return fd_resource_pitch(fd_resource(tex->res), level);
}

static void
to_pipe_box(const struct pipe_resource *res, uint32_t layer, const struct sg_box *b,
            struct pipe_box *out)
{
   if (res->target == PIPE_TEXTURE_3D)
      u_box_3d(b->x, b->y, b->z, b->width, b->height, b->depth ? b->depth : 1, out);
   else
      u_box_3d(b->x, b->y, layer, b->width, b->height, b->depth ? b->depth : 1, out);
}

bool
sg_texture_write(struct sg_device *dev, struct sg_texture *tex, uint32_t level, uint32_t layer,
                 const struct sg_box *box, const void *data, uint32_t stride,
                 uint32_t layer_stride)
{
   struct pipe_box b;
   to_pipe_box(tex->res, layer, box, &b);
   simple_mtx_lock(&dev->lock);
   dev->pipe->texture_subdata(dev->pipe, tex->res, level, PIPE_MAP_WRITE, &b, data, stride,
                              layer_stride);
   simple_mtx_unlock(&dev->lock);
   return true;
}

static bool
read_locked(struct sg_device *dev, struct pipe_resource *res, uint32_t level, uint32_t layer,
            const struct sg_box *box, void *data, uint32_t stride, uint32_t layer_stride)
{
   struct pipe_box b;
   to_pipe_box(res, layer, box, &b);
   struct pipe_transfer *xfer = NULL;
   void *map = dev->pipe->texture_map(dev->pipe, res, level, PIPE_MAP_READ, &b, &xfer);
   if (!map) {
      sg_set_error("texture_map for reading failed");
      return false;
   }
   util_copy_box(data, res->format, stride, layer_stride, 0, 0, 0, b.width, b.height, b.depth,
                 map, xfer->stride, xfer->layer_stride, 0, 0, 0);
   dev->pipe->texture_unmap(dev->pipe, xfer);
   return true;
}

bool
sg_texture_read(struct sg_device *dev, struct sg_texture *tex, uint32_t level, uint32_t layer,
                const struct sg_box *box, void *data, uint32_t stride, uint32_t layer_stride)
{
   simple_mtx_lock(&dev->lock);
   bool ok = read_locked(dev, tex->res, level, layer, box, data, stride, layer_stride);
   simple_mtx_unlock(&dev->lock);
   return ok;
}

/* sg_replay.c's CPU copies (lock held) */
bool
sg_read_locked(struct sg_device *dev, struct pipe_resource *res, uint32_t level, uint32_t layer,
               const struct sg_box *box, void *data, uint32_t stride, uint32_t layer_stride)
{
   return read_locked(dev, res, level, layer, box, data, stride, layer_stride);
}

/* ---- views and samplers ---- */

struct sg_view *
sg_view_new(struct sg_device *dev, struct sg_texture *tex, const struct sg_view_desc *desc)
{
   struct pipe_sampler_view templ;
   enum pipe_format format =
      desc->vk_format ? (enum pipe_format)sg_pipe_format(desc->vk_format) : tex->res->format;
   int target = sg_target_from_metal(desc->type);
   if (format == PIPE_FORMAT_NONE || target < 0) {
      sg_set_error("view of VkFormat %u, texture type %u: no Gallium counterpart",
                   desc->vk_format, desc->type);
      return NULL;
   }
   u_sampler_view_default_template(&templ, tex->res, format);
   templ.target = (enum pipe_texture_target)target;
   templ.u.tex.first_level = desc->first_level;
   templ.u.tex.last_level = desc->last_level;
   templ.u.tex.first_layer = desc->first_layer;
   templ.u.tex.last_layer = desc->last_layer;
   int sw[4];
   for (int i = 0; i < 4; i++) {
      sw[i] = sg_swizzle_from_metal(desc->swizzle[i]);
      if (sw[i] < 0) {
         sg_set_error("texture swizzle %u", desc->swizzle[i]);
         return NULL;
      }
   }
   templ.swizzle_r = sw[0];
   templ.swizzle_g = sw[1];
   templ.swizzle_b = sw[2];
   templ.swizzle_a = sw[3];
   simple_mtx_lock(&dev->lock);
   struct pipe_sampler_view *view = dev->pipe->create_sampler_view(dev->pipe, tex->res, &templ);
   simple_mtx_unlock(&dev->lock);
   if (!view) {
      sg_set_error("create_sampler_view failed (%s)", util_format_name(format));
      return NULL;
   }
   struct sg_view *v = calloc(1, sizeof(*v));
   v->dev = dev;
   v->view = view;
   return v;
}

struct sg_view *
sg_view_new_buffer(struct sg_device *dev, struct sg_buffer *buf, uint32_t vk_format,
                   uint64_t offset, uint64_t size)
{
   struct pipe_sampler_view templ;
   enum pipe_format format = (enum pipe_format)sg_pipe_format(vk_format);
   if (format == PIPE_FORMAT_NONE) {
      sg_set_error("texel buffer of VkFormat %u: no Gallium format", vk_format);
      return NULL;
   }
   u_sampler_view_default_template(&templ, buf->res, format);
   templ.target = PIPE_BUFFER;
   templ.u.buf.offset = (unsigned)offset;
   templ.u.buf.size = (unsigned)size;
   simple_mtx_lock(&dev->lock);
   struct pipe_sampler_view *view = dev->pipe->create_sampler_view(dev->pipe, buf->res, &templ);
   simple_mtx_unlock(&dev->lock);
   if (!view) {
      sg_set_error("create_sampler_view of a buffer failed");
      return NULL;
   }
   struct sg_view *v = calloc(1, sizeof(*v));
   v->dev = dev;
   v->view = view;
   return v;
}

void
sg_view_free(struct sg_view *view)
{
   if (!view)
      return;
   struct sg_device *dev = view->dev;
   simple_mtx_lock(&dev->lock);
   dev->pipe->sampler_view_release(dev->pipe, view->view);
   simple_mtx_unlock(&dev->lock);
   free(view);
}

struct sg_sampler *
sg_sampler_new(struct sg_device *dev, const struct sg_sampler_desc *d)
{
   struct pipe_sampler_state s;
   memset(&s, 0, sizeof(s));
   int ws = sg_wrap_from_metal(d->s_address), wt = sg_wrap_from_metal(d->t_address),
       wr = sg_wrap_from_metal(d->r_address), cf = sg_func_from_metal(d->compare);
   if (ws < 0 || wt < 0 || wr < 0 || cf < 0 || d->min_filter > 1 || d->mag_filter > 1 ||
       d->mip_filter > 2 || d->border > 2) {
      sg_set_error("sampler state value without a Gallium counterpart");
      return NULL;
   }
   s.wrap_s = ws;
   s.wrap_t = wt;
   s.wrap_r = wr;
   s.min_img_filter = d->min_filter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   s.mag_img_filter = d->mag_filter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   s.min_mip_filter = d->mip_filter == 0   ? PIPE_TEX_MIPFILTER_NONE
                      : d->mip_filter == 1 ? PIPE_TEX_MIPFILTER_NEAREST
                                           : PIPE_TEX_MIPFILTER_LINEAR;
   s.compare_mode = d->compare_enable ? PIPE_TEX_COMPARE_R_TO_TEXTURE : PIPE_TEX_COMPARE_NONE;
   s.compare_func = cf;
   s.unnormalized_coords = !d->normalized;
   s.max_anisotropy = d->max_anisotropy > 1 ? MIN2(d->max_anisotropy, 16) : 0;
   s.seamless_cube_map = 1;
   s.border_color_is_integer = d->integer_border;
   s.min_lod = d->lod_min;
   s.max_lod = d->lod_max;
   /* MTLSamplerBorderColor: transparent black, opaque black, opaque white. ClampToZero
    * (address mode 4) is a transparent black border. */
   bool zero = d->s_address == 4 || d->t_address == 4 || d->r_address == 4;
   unsigned border = zero ? 0 : d->border;
   float a = border == 0 ? 0.0f : 1.0f, c = border == 2 ? 1.0f : 0.0f;
   if (d->integer_border) {
      s.border_color.ui[0] = s.border_color.ui[1] = s.border_color.ui[2] = (uint32_t)c;
      s.border_color.ui[3] = (uint32_t)a;
   } else {
      s.border_color.f[0] = s.border_color.f[1] = s.border_color.f[2] = c;
      s.border_color.f[3] = a;
   }
   simple_mtx_lock(&dev->lock);
   void *cso = dev->pipe->create_sampler_state(dev->pipe, &s);
   simple_mtx_unlock(&dev->lock);
   if (!cso) {
      sg_set_error("create_sampler_state failed");
      return NULL;
   }
   struct sg_sampler *sampler = calloc(1, sizeof(*sampler));
   sampler->dev = dev;
   sampler->cso = cso;
   return sampler;
}

void
sg_sampler_free(struct sg_sampler *sampler)
{
   if (!sampler)
      return;
   struct sg_device *dev = sampler->dev;
   simple_mtx_lock(&dev->lock);
   dev->pipe->delete_sampler_state(dev->pipe, sampler->cso);
   simple_mtx_unlock(&dev->lock);
   free(sampler);
}
