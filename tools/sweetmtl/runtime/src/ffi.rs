// SPDX-License-Identifier: MIT
//! The Gallium shim's C interface (tools/sweetmtl/gallium/sg.h), declared by hand.

#![allow(non_camel_case_types, dead_code)]

use std::ffi::{c_char, c_void};

#[repr(C)]
pub struct sg_device {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_buffer {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_texture {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_view {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_sampler {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_shader {
    _p: [u8; 0],
}
#[repr(C)]
pub struct sg_fence {
    _p: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_stats {
    pub flushes: u64,
    pub dispatches: u64,
    pub draws: u64,
    pub clears: u64,
    pub blits: u64,
    pub cpu_copies: u64,
    pub shaders: u64,
    pub shader_instrs: u64,
}

pub const SG_USAGE_SAMPLE: u32 = 1;
pub const SG_USAGE_STORAGE: u32 = 2;
pub const SG_USAGE_RENDER: u32 = 4;
pub const SG_USAGE_VERTEX: u32 = 8;
pub const SG_USAGE_BLEND: u32 = 16;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_texture_desc {
    pub ty: u32,
    pub vk_format: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub array_size: u32,
    pub levels: u32,
    pub samples: u32,
    pub usage: u32,
    pub linear: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_box {
    pub x: i32,
    pub y: i32,
    pub z: i32,
    pub width: i32,
    pub height: i32,
    pub depth: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_view_desc {
    pub vk_format: u32,
    pub ty: u32,
    pub first_level: u32,
    pub last_level: u32,
    pub first_layer: u32,
    pub last_layer: u32,
    pub swizzle: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_sampler_desc {
    pub min_filter: u32,
    pub mag_filter: u32,
    pub mip_filter: u32,
    pub s_address: u32,
    pub t_address: u32,
    pub r_address: u32,
    pub border: u32,
    pub normalized: u32,
    pub max_anisotropy: u32,
    pub compare: u32,
    pub compare_enable: u32,
    pub integer_border: u32,
    pub lod_min: f32,
    pub lod_max: f32,
}

pub const SG_VERTEX: u32 = 0;
pub const SG_FRAGMENT: u32 = 1;
pub const SG_COMPUTE: u32 = 2;

pub const SG_CLASS_UBO: u32 = 1;
pub const SG_CLASS_SSBO: u32 = 2;
pub const SG_CLASS_SSBO_AS_UBO: u32 = 3;
pub const SG_CLASS_TEXTURE: u32 = 4;
pub const SG_CLASS_SAMPLER: u32 = 5;
pub const SG_CLASS_COMBINED: u32 = 6;
pub const SG_CLASS_IMAGE: u32 = 7;
pub const SG_CLASS_TEXEL_BUFFER: u32 = 8;
pub const SG_CLASS_INPUT_ATTACHMENT: u32 = 9;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct sg_binding {
    pub set: u32,
    pub binding: u32,
    pub count: u32,
    pub cls: u32,
    pub slot: u32,
    pub sampler_slot: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sg_spec {
    pub id: u32,
    pub size: u32,
    pub data: *const c_void,
}

#[repr(C)]
pub struct sg_shader_desc {
    pub stage: u32,
    pub words: *const u32,
    pub word_count: usize,
    pub entry: *const c_char,
    pub spec: *const sg_spec,
    pub spec_count: u32,
    pub bindings: *const sg_binding,
    pub binding_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sg_shader_info {
    pub stage: u32,
    pub instrs: u32,
    pub full_regs: u32,
    pub half_regs: u32,
    pub constlen: u32,
    pub workgroup: [u32; 3],
    pub shared_bytes: u32,
    pub inputs: u32,
    pub input_locations: [u32; 32],
}

impl Default for sg_shader_info {
    fn default() -> Self {
        sg_shader_info {
            stage: 0,
            instrs: 0,
            full_regs: 0,
            half_regs: 0,
            constlen: 0,
            workgroup: [0; 3],
            shared_bytes: 0,
            inputs: 0,
            input_locations: [0; 32],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_blend_rt {
    pub enable: u32,
    pub src_color: u32,
    pub dst_color: u32,
    pub color_op: u32,
    pub src_alpha: u32,
    pub dst_alpha: u32,
    pub alpha_op: u32,
    pub write_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_raster {
    pub cull: u32,
    pub winding: u32,
    pub fill: u32,
    pub depth_clip: u32,
    pub samples: u32,
    pub depth_bias: f32,
    pub depth_bias_slope: f32,
    pub depth_bias_clamp: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_stencil_face {
    pub compare: u32,
    pub fail: u32,
    pub depth_fail: u32,
    pub pass: u32,
    pub read_mask: u32,
    pub write_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_depth_stencil {
    pub compare: u32,
    pub write: u32,
    pub stencil_enable: u32,
    pub front: sg_stencil_face,
    pub back: sg_stencil_face,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_vertex_element {
    pub vk_format: u32,
    pub offset: u32,
    pub buffer: u32,
    pub stride: u32,
    pub divisor: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sg_attachment {
    pub tex: *mut sg_texture,
    pub vk_format: u32,
    pub level: u32,
    pub first_layer: u32,
    pub last_layer: u32,
    pub load: u32,
    pub clear: [f64; 4],
}

impl Default for sg_attachment {
    fn default() -> Self {
        sg_attachment {
            tex: std::ptr::null_mut(),
            vk_format: 0,
            level: 0,
            first_layer: 0,
            last_layer: 0,
            load: 0,
            clear: [0.0; 4],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sg_pass {
    pub color: [sg_attachment; 8],
    pub depth: sg_attachment,
    pub stencil: sg_attachment,
    pub width: u32,
    pub height: u32,
    pub layers: u32,
    pub samples: u32,
}

pub const SG_ASPECT_COLOR: u32 = 1;
pub const SG_ASPECT_DEPTH: u32 = 2;
pub const SG_ASPECT_STENCIL: u32 = 4;

#[repr(C)]
pub struct sg_resolve {
    pub src: *mut sg_texture,
    pub dst: *mut sg_texture,
    pub src_format: u32,
    pub src_level: u32,
    pub src_layer: u32,
    pub dst_format: u32,
    pub dst_level: u32,
    pub dst_layer: u32,
    pub width: u32,
    pub height: u32,
    pub aspects: u32,
}

#[repr(C)]
pub struct sg_draw {
    pub vs: *mut sg_shader,
    pub fs: *mut sg_shader,
    pub blend: *const sg_blend_rt,
    pub blend_count: u32,
    pub alpha_to_coverage: u32,
    pub alpha_to_one: u32,
    pub blend_color: [f32; 4],
    pub raster: *const sg_raster,
    pub depth_stencil: *const sg_depth_stencil,
    pub stencil_ref: [u32; 2],
    pub viewport: [f64; 6],
    pub scissor: [u32; 4],
    pub elements: *const sg_vertex_element,
    pub element_count: u32,
    pub vertex_buffers: *const *mut sg_buffer,
    pub vertex_offsets: *const u64,
    pub vertex_buffer_count: u32,
    pub primitive: u32,
    pub primitive_restart: u32,
    pub first_vertex: u32,
    pub vertex_count: u32,
    pub first_instance: u32,
    pub instance_count: u32,
    pub index_size: u32,
    pub index_buffer: *mut sg_buffer,
    pub index_offset: u64,
    pub base_vertex: i32,
    pub texture_barrier: u32,
}

unsafe extern "C" {
    pub fn sg_last_error() -> *const c_char;

    pub fn sg_device_open() -> *mut sg_device;
    pub fn sg_device_open_fd(fd: i32) -> *mut sg_device;
    pub fn sg_device_close(dev: *mut sg_device);
    pub fn sg_device_name(dev: *mut sg_device) -> *const c_char;
    pub fn sg_device_gpu_id(dev: *mut sg_device) -> u32;
    pub fn sg_device_stats(dev: *mut sg_device, out: *mut sg_stats);
    pub fn sg_lock(dev: *mut sg_device);
    pub fn sg_unlock(dev: *mut sg_device);

    pub fn sg_format_supported(dev: *mut sg_device, vk_format: u32, usage: u32, samples: u32) -> bool;
    pub fn sg_format_block_bytes(vk_format: u32) -> u32;
    pub fn sg_format_block_width(vk_format: u32) -> u32;
    pub fn sg_format_block_height(vk_format: u32) -> u32;

    pub fn sg_buffer_new(dev: *mut sg_device, size: u64) -> *mut sg_buffer;
    pub fn sg_buffer_free(buf: *mut sg_buffer);
    pub fn sg_buffer_map(buf: *mut sg_buffer) -> *mut c_void;
    pub fn sg_buffer_iova(buf: *mut sg_buffer) -> u64;
    pub fn sg_buffer_size(buf: *mut sg_buffer) -> u64;

    pub fn sg_texture_new(dev: *mut sg_device, desc: *const sg_texture_desc) -> *mut sg_texture;
    pub fn sg_texture_new_on_buffer(dev: *mut sg_device, desc: *const sg_texture_desc, buf: *mut sg_buffer, offset: u64,
                                    stride: u32) -> *mut sg_texture;
    pub fn sg_texture_free(tex: *mut sg_texture);
    pub fn sg_texture_pitch(tex: *mut sg_texture, level: u32) -> u32;
    pub fn sg_texture_write(dev: *mut sg_device, tex: *mut sg_texture, level: u32, layer: u32, b: *const sg_box,
                            data: *const c_void, stride: u32, layer_stride: u32) -> bool;
    pub fn sg_texture_read(dev: *mut sg_device, tex: *mut sg_texture, level: u32, layer: u32, b: *const sg_box,
                           data: *mut c_void, stride: u32, layer_stride: u32) -> bool;

    pub fn sg_view_new(dev: *mut sg_device, tex: *mut sg_texture, desc: *const sg_view_desc) -> *mut sg_view;
    pub fn sg_view_new_buffer(dev: *mut sg_device, buf: *mut sg_buffer, vk_format: u32, offset: u64, size: u64)
                              -> *mut sg_view;
    pub fn sg_view_free(view: *mut sg_view);
    pub fn sg_sampler_new(dev: *mut sg_device, desc: *const sg_sampler_desc) -> *mut sg_sampler;
    pub fn sg_sampler_free(sampler: *mut sg_sampler);

    pub fn sg_shader_new(dev: *mut sg_device, desc: *const sg_shader_desc) -> *mut sg_shader;
    pub fn sg_shader_free(shader: *mut sg_shader);
    pub fn sg_shader_info(shader: *mut sg_shader, out: *mut sg_shader_info);

    pub fn sg_ctx_set_push(dev: *mut sg_device, stage: u32, bytes: *const c_void, size: u32);
    pub fn sg_ctx_set_ubo(dev: *mut sg_device, stage: u32, slot: u32, buf: *mut sg_buffer, offset: u64, size: u32);
    pub fn sg_ctx_set_ssbo(dev: *mut sg_device, stage: u32, slot: u32, buf: *mut sg_buffer, offset: u64, size: u32,
                           writable: bool);
    pub fn sg_ctx_set_view(dev: *mut sg_device, stage: u32, slot: u32, view: *mut sg_view);
    pub fn sg_ctx_set_sampler(dev: *mut sg_device, stage: u32, slot: u32, sampler: *mut sg_sampler);
    pub fn sg_ctx_set_image(dev: *mut sg_device, stage: u32, slot: u32, tex: *mut sg_texture, buf: *mut sg_buffer,
                            vk_format: u32, level: u32, first_layer: u32, last_layer: u32, offset: u64, size: u64,
                            writable: bool);
    pub fn sg_ctx_reset_bindings(dev: *mut sg_device);
    pub fn sg_ctx_dispatch(dev: *mut sg_device, cs: *mut sg_shader, grid: *const u32, shared_bytes: u32) -> bool;
    pub fn sg_ctx_begin_pass(dev: *mut sg_device, pass: *const sg_pass) -> bool;
    pub fn sg_ctx_end_pass(dev: *mut sg_device);
    pub fn sg_ctx_resolve(dev: *mut sg_device, resolve: *const sg_resolve) -> bool;
    pub fn sg_ctx_draw(dev: *mut sg_device, draw: *const sg_draw) -> bool;
    pub fn sg_ctx_copy_buffer(dev: *mut sg_device, dst: *mut sg_buffer, dst_offset: u64, src: *mut sg_buffer,
                              src_offset: u64, size: u64) -> bool;
    pub fn sg_ctx_fill_buffer(dev: *mut sg_device, buf: *mut sg_buffer, offset: u64, size: u64, value: u8) -> bool;
    pub fn sg_ctx_copy_texture(dev: *mut sg_device, dst: *mut sg_texture, dst_level: u32, dst_layer: u32,
                               dst_origin: *const u32, src: *mut sg_texture, src_level: u32, src_layer: u32,
                               src_box: *const sg_box) -> bool;
    pub fn sg_ctx_texture_to_buffer(dev: *mut sg_device, src: *mut sg_texture, level: u32, layer: u32,
                                    b: *const sg_box, dst: *mut sg_buffer, offset: u64, stride: u32,
                                    layer_stride: u32) -> bool;
    pub fn sg_ctx_buffer_to_texture(dev: *mut sg_device, src: *mut sg_buffer, offset: u64, stride: u32,
                                    layer_stride: u32, dst: *mut sg_texture, level: u32, layer: u32,
                                    b: *const sg_box) -> bool;
    pub fn sg_ctx_generate_mipmaps(dev: *mut sg_device, tex: *mut sg_texture) -> bool;
    pub fn sg_ctx_sync_host(dev: *mut sg_device, buf: *mut sg_buffer, host: *mut c_void, size: u64, to_host: bool)
                            -> bool;
    pub fn sg_ctx_memory_barrier(dev: *mut sg_device);
    pub fn sg_ctx_flush(dev: *mut sg_device) -> *mut sg_fence;

    pub fn sg_fence_wait(dev: *mut sg_device, fence: *mut sg_fence, timeout_ns: u64) -> i32;
    pub fn sg_fence_free(dev: *mut sg_device, fence: *mut sg_fence);
}

/// The shim's per-thread reason for its last failure.
pub fn last_error() -> String {
    // SAFETY: the shim returns a NUL-terminated per-thread buffer.
    let p = unsafe { sg_last_error() };
    if p.is_null() {
        return String::new();
    }
    // SAFETY: as above.
    unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned()
}
