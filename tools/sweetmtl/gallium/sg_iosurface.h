/*
 * sweetmtl Gallium shim: buffers over IOSurfaces (sg_iosurface.c). Kept beside sg.h so that the
 * other paths stay as they are; the runtime declares it in runtime/src/iosurface.rs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SWEETMTL_SG_IOSURFACE_H
#define SWEETMTL_SG_IOSURFACE_H

#include <stdint.h>

struct sg_device;
struct sg_buffer;

/* A buffer over IOSurface surface_id's pages, without a copy: SweetGPU makes a bo of them (sel 12)
 * and freedreno imports it as a PIPE_BUFFER. base is the surface's mapping in this process from the
 * page its base address is in (the bo's CPU view); *size gets the buffer's length (the kernel's).
 * Free it with sg_buffer_free. NULL (sg_last_error says why) without SweetGPU: the PC. */
struct sg_buffer *sg_buffer_import_iosurface(struct sg_device *dev, uint32_t surface_id, void *base,
                                             uint64_t *size);

#endif
