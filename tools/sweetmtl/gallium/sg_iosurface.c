/*
 * sweetmtl Gallium shim: buffers over IOSurfaces (docs/taimen-metal-plan.md 2.3, 4.3).
 *
 * An IOSurface's pages become a Metal buffer without a copy. SweetGPU's sel 12
 * (tools/sweetgl/sweet_drm.c, sweet_drm_import_iosurface) makes a GEM handle for the surface, and
 * freedreno imports that handle as a PIPE_BUFFER (resource_from_handle, KMS handle), the way
 * sweetgl's EAGL layer imports surfaces as textures (eagl.c bind_surface). A texture over the
 * buffer (sg_texture_new_on_buffer) then shares the surface's bo: the GPU renders into and samples
 * the surface itself, and the kernel cleans the surface's CPU cache lines at each submit that lists
 * the bo (xnu 0242). Like sg_buffer_new's buffers, the bo is mapped once and never replaced; its CPU
 * view is the surface's own mapping in this process.
 *
 * On the PC (no SweetGPU) the import is refused, and the frontend imports the surface's mapping as
 * host pages instead (sg_buffer_new + the runtime's shadow copies).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "sg_priv.h"
#include "sg_iosurface.h"

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "util/u_range.h"

#include "freedreno_resource.h"
#include "freedreno/drm/freedreno_drmif.h"

#ifdef __APPLE__
#include "xf86drm.h"
#include "sweet_drm.h"
#endif

struct sg_buffer *
sg_buffer_import_iosurface(struct sg_device *dev, uint32_t surface_id, void *base, uint64_t *size_out)
{
   if (size_out)
      *size_out = 0;
#ifndef __APPLE__
   (void)dev;
   (void)base;
   sg_set_error("IOSurface %u: an import by ID needs SweetGPU (iOS); this build has none",
                surface_id);
   return NULL;
#else
   if (!dev->freedreno) {
      sg_set_error("IOSurface %u: an import by ID is freedreno's", surface_id);
      return NULL;
   }
   uint32_t handle = 0;
   uint64_t size = 0;
   int r = sweet_drm_import_iosurface(surface_id, (uint64_t)(uintptr_t)base, &handle, &size);
   if (r != 0) {
      sg_set_error("IOSurface %u: SweetGPU import (sel 12) failed (%d)", surface_id, r);
      return NULL;
   }
   if (size == 0 || size > UINT32_MAX) {
      struct drm_gem_close close = {.handle = handle};
      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &close);
      sg_set_error("IOSurface %u: %llu bytes: Gallium buffers hold 1 byte to 4 GiB", surface_id,
                   (unsigned long long)size);
      return NULL;
   }
   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_BUFFER;
   templ.format = PIPE_FORMAT_R8_UNORM;
   templ.width0 = (uint32_t)size;
   templ.height0 = templ.depth0 = templ.array_size = 1;
   /* as sg_buffer_new's buffers, plus shared (another process owns the surface) */
   templ.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
                PIPE_BIND_SHADER_BUFFER | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHADER_IMAGE |
                PIPE_BIND_COMMAND_ARGS_BUFFER | PIPE_BIND_SHARED;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.flags = PIPE_RESOURCE_FLAG_MAP_PERSISTENT | PIPE_RESOURCE_FLAG_MAP_COHERENT;
   struct winsys_handle wh;
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_KMS;
   wh.handle = handle;
   wh.stride = (unsigned)size; /* a buffer's layout: pitch = width (fdl_layout_buffer) */
   wh.offset = 0;
   wh.modifier = DRM_FORMAT_MOD_LINEAR;
   wh.format = templ.format;
   /* takes over the handle: closed with the resource, or now on failure (eagl.c says the same) */
   struct pipe_resource *res = dev->screen->resource_from_handle(
      dev->screen, &templ, &wh,
      PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE | PIPE_HANDLE_USAGE_SHADER_WRITE);
   if (!res) {
      sg_set_error("IOSurface %u (%llu bytes): freedreno refused the buffer import", surface_id,
                   (unsigned long long)size);
      return NULL;
   }
   struct fd_resource *rsc = fd_resource(res);
   void *map = fd_bo_map(rsc->bo);
   if (!map)
      map = base;
   /* The surface holds data the GPU did not write: all of it is valid. */
   util_range_add(&rsc->b.b, &rsc->valid_buffer_range, 0, (unsigned)size);
   struct sg_buffer *buf = calloc(1, sizeof(*buf));
   if (!buf) {
      pipe_resource_reference(&res, NULL);
      sg_set_error("IOSurface %u: out of memory", surface_id);
      return NULL;
   }
   buf->dev = dev;
   buf->res = res;
   buf->map = map;
   buf->size = size;
   buf->iova = fd_bo_get_iova(rsc->bo);
   if (size_out)
      *size_out = size;
   return buf;
#endif
}
