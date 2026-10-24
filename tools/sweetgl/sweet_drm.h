/* tools/sweetgl: msm DRM emulation (sweet_drm.c) and its log. */
#ifndef SWEET_DRM_H
#define SWEET_DRM_H

#include <stdint.h>

/* A "DRM fd" for fd_device_new (dup of /dev/null), or -1 without SweetGPU. */
int sweet_drm_open(void);

/* The kernel's mapping of the bo behind an msm mmap offset (MAP_FAILED if none). */
void *sweet_drm_bo_map(int fd, uint64_t offset);

/* A GEM handle for an IOSurface's memory: 0 or -errno. */
int sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size);

/* GPU submits since the last call: count, mach time spent in the kernel call, bos listed */
void sweet_drm_submit_stats(uint64_t *n, uint64_t *ticks, uint64_t *bos);

/* printf-style line to the kernel log (writes to /dev/null are teed there). */
void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
