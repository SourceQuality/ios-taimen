/*
 * sweetmtl Gallium shim: the device (one pipe_screen and one pipe_context per process),
 * errors, fences.
 *
 * The screen is made the way sweetgl makes its own (tools/sweetgl/sweetgl.c): the freedreno
 * DRM winsys over an msm fd, driconf defaults without xmlconfig, sysmem rendering. On the PC the
 * fd is Mesa's freedreno drm-shim (LD_PRELOAD, FD_GPU_ID=540), which records submits and
 * executes nothing; on the phone it is sweet_drm.c over the SweetGPU user client.
 *
 * Hosts built with SG_WITH_SOFTPIPE can run the same shim on softpipe instead (SG_DRIVER=softpipe):
 * Mesa's CPU driver executes the commands, so the PC checks what the GPU would compute (the
 * self-test's values), with the freedreno-only parts (bo iovas, textures over a buffer's bo, ir3)
 * left out.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sg_priv.h"

#include "compiler/glsl_types.h"
#include "cso_cache/cso_context.h"
#include "freedreno/drm/freedreno_drm_public.h"
#include "frontend/drm_driver.h"
#include "renderonly/renderonly.h"
#include "util/driconf.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/xmlconfig.h"

#include "freedreno_screen.h"

#ifdef SG_WITH_SOFTPIPE
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"
#endif

#ifdef __APPLE__
#include "sweet_drm.h"

/* sweet_drm.c's log (sweetgl.c has its own): writes to /dev/null, which taimen's kernel tees into
 * its log */
void
sweet_drm_log(const char *fmt, ...)
{
   static int fd = -1;
   if (fd < 0)
      fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
   char line[512];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if (n > (int)sizeof(line) - 2)
      n = sizeof(line) - 2;
   line[n++] = '\n';
   (void)write(fd, line, n);
}

/* freedreno calls these only with a renderonly device (KMS scanout); there is none, and Mesa's
 * renderonly.c is not built for iOS (sweetgl.c defines the same) */
void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
   (void)scanout;
   (void)ro;
}

struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
                                          struct winsys_handle *out_handle)
{
   (void)rsc;
   (void)ro;
   (void)out_handle;
   return NULL;
}
#endif

static const driOptionDescription sg_driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "freedreno/driinfo_freedreno.h"
};
static driOptionCache sg_opt_info, sg_opts;

static __thread char sg_error[512];

void
sg_set_error(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(sg_error, sizeof(sg_error), fmt, ap);
   va_end(ap);
   if (getenv("SG_DEBUG"))
      fprintf(stderr, "sg: %s\n", sg_error);
}

const char *
sg_last_error(void)
{
   return sg_error;
}

mesa_shader_stage
sg_mesa_stage(uint32_t stage)
{
   switch (stage) {
   case SG_VERTEX:
      return MESA_SHADER_VERTEX;
   case SG_FRAGMENT:
      return MESA_SHADER_FRAGMENT;
   default:
      return MESA_SHADER_COMPUTE;
   }
}

/* The driver's compile messages: errors become the failing call's reason. */
static void
sg_debug_message(void *data, unsigned *id, enum util_debug_type type, const char *fmt,
                 va_list args)
{
   (void)data;
   (void)id;
   if (type == UTIL_DEBUG_TYPE_ERROR || type == UTIL_DEBUG_TYPE_OUT_OF_MEMORY) {
      char line[400];
      vsnprintf(line, sizeof(line), fmt, args);
      sg_set_error("driver: %s", line);
   } else if (getenv("SG_DEBUG_DRIVER")) {
      vfprintf(stderr, fmt, args);
      fputc('\n', stderr);
   }
}

static struct util_debug_callback sg_debug = {
   .async = false,
   .debug_message = sg_debug_message,
};

/* FD_MESA_DEBUG for Metal: sysmem (no GMEM tiles: tile restore overruns unpadded IOSurfaces,
 * as sweetgl found) and inorder (no batch reordering: buffers reached through gpuAddress
 * pointers are invisible to freedreno's dependency tracking). An existing setting is kept and
 * extended. */
static void
sg_fd_env(void)
{
   const char *have = getenv("FD_MESA_DEBUG");
   char want[256];
   if (!have || !*have) {
      setenv("FD_MESA_DEBUG", "sysmem,inorder", 1);
      return;
   }
   snprintf(want, sizeof(want), "%s%s%s", have, strstr(have, "sysmem") ? "" : ",sysmem",
            strstr(have, "inorder") ? "" : ",inorder");
   setenv("FD_MESA_DEBUG", want, 1);
}

static struct sg_device *
sg_device_of_screen(struct pipe_screen *screen, int fd, bool freedreno, uint32_t gpu_id)
{
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      sg_set_error("Gallium context creation failed");
      screen->destroy(screen);
      glsl_type_singleton_decref();
      return NULL;
   }
   struct sg_device *dev = calloc(1, sizeof(*dev));
   dev->screen = screen;
   dev->pipe = pipe;
   dev->fd = fd;
   dev->freedreno = freedreno;
   dev->next_fake_iova = 0x100000000ull;
   dev->gpu_id = gpu_id;
   snprintf(dev->name, sizeof(dev->name), "%s", screen->get_name(screen));
   simple_mtx_init(&dev->lock, mtx_plain);
   /* Synchronous initial variants (freedreno compiles them in the creating call when a debug
    * callback is set), so a compile failure belongs to sg_shader_new. */
   if (pipe->set_debug_callback)
      pipe->set_debug_callback(pipe, &sg_debug);
   dev->cso = cso_create_context(pipe, 0);
   return dev;
}

struct sg_device *
sg_device_open_fd(int fd)
{
   if (fd < 0) {
      sg_set_error("no GPU fd");
      return NULL;
   }
   sg_fd_env();
   /* spirv_to_nir builds GLSL types (sg_shader.c) */
   glsl_type_singleton_init_or_ref();
   static bool parsed;
   if (!parsed) {
      driParseOptionInfo(&sg_opt_info, sg_driconf, ARRAY_SIZE(sg_driconf));
      parsed = true;
   }
   struct pipe_screen_config cfg = {.options = &sg_opts, .options_info = &sg_opt_info};
   struct pipe_screen *screen = fd_drm_screen_create_renderonly(fd, NULL, &cfg);
   if (!screen) {
      sg_set_error("freedreno screen creation failed");
      close(fd);
      glsl_type_singleton_decref();
      return NULL;
   }
   struct fd_screen *fds = fd_screen(screen);
   if (fds->gen != 5) {
      sg_set_error("GPU %u is not an a5xx (this runtime is built for the Adreno 540)",
                   fds->gpu_id);
      screen->destroy(screen);
      glsl_type_singleton_decref();
      return NULL;
   }
   return sg_device_of_screen(screen, fd, true, fds->gpu_id);
}

struct sg_device *
sg_device_open(void)
{
   const char *driver = getenv("SG_DRIVER");
   if (driver && !strcmp(driver, "softpipe")) {
#ifdef SG_WITH_SOFTPIPE
      glsl_type_singleton_init_or_ref();
      struct pipe_screen *screen = softpipe_create_screen(null_sw_create());
      if (!screen) {
         sg_set_error("softpipe screen creation failed");
         glsl_type_singleton_decref();
         return NULL;
      }
      return sg_device_of_screen(screen, -1, false, 0);
#else
      sg_set_error("SG_DRIVER=softpipe: this shim was built without softpipe");
      return NULL;
#endif
   }
#ifdef __APPLE__
   return sg_device_open_fd(sweet_drm_open());
#else
   const char *node = getenv("SG_RENDER_NODE");
   int fd = open(node ? node : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      sg_set_error("%s: %s (on the PC, run under Mesa's freedreno drm-shim: LD_PRELOAD and "
                   "FD_GPU_ID=540)",
                   node ? node : "/dev/dri/renderD128", strerror(errno));
      return NULL;
   }
   return sg_device_open_fd(fd);
#endif
}

void
sg_device_close(struct sg_device *dev)
{
   if (!dev)
      return;
   simple_mtx_lock(&dev->lock);
   cso_destroy_context(dev->cso);
   dev->pipe->destroy(dev->pipe);
   simple_mtx_unlock(&dev->lock);
   dev->screen->destroy(dev->screen);
   simple_mtx_destroy(&dev->lock);
   free(dev);
   glsl_type_singleton_decref();
}

const char *
sg_device_name(struct sg_device *dev)
{
   return dev->name;
}

uint32_t
sg_device_gpu_id(struct sg_device *dev)
{
   return dev->gpu_id;
}

void
sg_device_stats(struct sg_device *dev, struct sg_stats *out)
{
   simple_mtx_lock(&dev->lock);
   *out = dev->stats;
   simple_mtx_unlock(&dev->lock);
}

void
sg_lock(struct sg_device *dev)
{
   simple_mtx_lock(&dev->lock);
}

void
sg_unlock(struct sg_device *dev)
{
   simple_mtx_unlock(&dev->lock);
}

struct sg_fence *
sg_ctx_flush(struct sg_device *dev)
{
   struct pipe_fence_handle *fence = NULL;
   dev->pipe->flush(dev->pipe, &fence, 0);
   dev->stats.flushes++;
   if (!fence) {
      sg_set_error("flush gave no fence");
      return NULL;
   }
   struct sg_fence *f = calloc(1, sizeof(*f));
   f->fence = fence;
   return f;
}

int
sg_fence_wait(struct sg_device *dev, struct sg_fence *fence, uint64_t timeout_ns)
{
   if (!fence)
      return -1;
   /* No context: the fence was flushed already (sg_ctx_flush), so no flush is needed. */
   return dev->screen->fence_finish(dev->screen, NULL, fence->fence, timeout_ns) ? 1 : 0;
}

void
sg_fence_free(struct sg_device *dev, struct sg_fence *fence)
{
   if (!fence)
      return;
   dev->screen->fence_reference(dev->screen, &fence->fence, NULL);
   free(fence);
}
