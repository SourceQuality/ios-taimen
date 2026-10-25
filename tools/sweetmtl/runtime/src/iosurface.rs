// SPDX-License-Identifier: MIT
//! Buffers over IOSurfaces (docs/taimen-metal-plan.md 4.3): the surface's own pages as a Metal
//! buffer, imported by the surface's ID through SweetGPU (sel 12, the shim's
//! `sg_buffer_import_iosurface` in gallium/sg_iosurface.c), with no copy and no shadow. A texture
//! over such a buffer (`Buffer::new_texture_with_iosurface_plane`) renders into and samples the
//! surface itself, so every process that imports the surface sees the same memory. The kernel
//! cleans the surface's CPU cache lines at each submit that lists its bo (xnu 0242).
//!
//! The other import, `new_buffer_with_bytes_no_copy` over the surface's mapping (resource.rs), stays
//! as it is: the frontend falls back to it where this one is refused (and on the PC, which has no
//! IOSurfaces).

use crate::device::{next_id, Device};
use crate::error::{gallium, invalid, Error};
use crate::ffi;
use crate::resource::{Buffer, BufferInner, ResourceOptions, SgBuffer, StorageMode};
use std::ffi::c_void;
use std::sync::Arc;

unsafe extern "C" {
    /// gallium/sg_iosurface.h: a PIPE_BUFFER over the surface's bo; `size` gets its length.
    fn sg_buffer_import_iosurface(dev: *mut ffi::sg_device, surface_id: u32, base: *mut c_void, size: *mut u64)
                                  -> *mut ffi::sg_buffer;
}

impl Device {
    /// A buffer over IOSurface `surface_id`'s pages (the module docs). Its length is the kernel's:
    /// the surface's pages from the one its base address is in.
    ///
    /// # Safety
    /// `base` is the surface's mapping in this process from that page, and the surface stays alive
    /// for as long as the buffer (and any texture over it) lives.
    pub unsafe fn new_buffer_with_iosurface(&self, surface_id: u32, base: *mut u8, options: ResourceOptions)
                                            -> Result<Buffer, Error> {
        if options.storage_mode() == StorageMode::Memoryless {
            return Err(invalid("a memoryless buffer"));
        }
        if surface_id == 0 {
            return Err(invalid("IOSurface ID 0"));
        }
        let mut size = 0u64;
        // SAFETY: valid device; the caller's contract for `base`.
        let sg = unsafe { sg_buffer_import_iosurface(self.gpu().sg, surface_id, base as *mut c_void, &mut size) };
        if sg.is_null() {
            return Err(gallium(&format!("IOSurface {surface_id}")));
        }
        Ok(Buffer(Arc::new(BufferInner {
            device: self.clone(),
            sg: SgBuffer { gpu: self.gpu().clone(), sg },
            length: size,
            options,
            host: None,
            id: next_id(),
        })))
    }
}
