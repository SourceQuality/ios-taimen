// SPDX-License-Identifier: MIT
//! Buffers, textures and heaps (`MTLBuffer`, `MTLTexture`, `MTLHeap`).
//!
//! A buffer is a Gallium PIPE_BUFFER resource the shim keeps mapped: `contents` is that mapping
//! and `gpuAddress` the freedreno bo's iova, both stable for the buffer's life. A buffer over the
//! caller's host pages (`newBufferWithBytesNoCopy:`) is shadowed: freedreno cannot make a bo of
//! user memory, so the runtime keeps a GPU buffer beside the pages and copies the pages in before a
//! command buffer that uses it runs and back out after it completed. A texture is a Gallium
//! texture; a texture over a buffer is a linear texture over the buffer's own bo (no copy).

use crate::device::{next_id, Device, Gpu, Size};
use crate::error::{gallium, invalid, not_yet, Error};
use crate::ffi;
use metal2vulkan::metal::{PixelFormat, TextureType, TextureUsage};
use std::ops::Range;
use std::ptr::NonNull;
use std::sync::{Arc, Mutex};

// ---- options ----

/// `MTLResourceOptions`: CPU cache mode (bits 0-3), storage mode (bits 4-7), hazard tracking
/// (bits 8-9).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct ResourceOptions(u64);

/// `MTLStorageMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StorageMode {
    Shared,
    Managed,
    Private,
    Memoryless,
}

impl ResourceOptions {
    pub fn from_raw(raw: u64) -> Result<ResourceOptions, Error> {
        let cache = raw & 0xf;
        let storage = (raw >> 4) & 0xf;
        let hazard = (raw >> 8) & 0x3;
        if cache > 1 || storage > 3 || hazard > 2 || raw & !0x3ff != 0 {
            return Err(invalid(format!("MTLResourceOptions 0x{raw:x}")));
        }
        Ok(ResourceOptions(raw))
    }
    pub fn shared() -> ResourceOptions {
        ResourceOptions(0)
    }
    pub fn private() -> ResourceOptions {
        ResourceOptions(2 << 4)
    }
    pub fn raw(self) -> u64 {
        self.0
    }
    pub fn storage_mode(self) -> StorageMode {
        match (self.0 >> 4) & 0xf {
            1 => StorageMode::Managed,
            2 => StorageMode::Private,
            3 => StorageMode::Memoryless,
            _ => StorageMode::Shared,
        }
    }
}

/// `MTLPurgeableState`: KeepCurrent 1, NonVolatile 2, Volatile 3, Empty 4.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PurgeableState(u64);

impl PurgeableState {
    pub fn from_raw(raw: u64) -> Result<PurgeableState, Error> {
        if (1..=4).contains(&raw) {
            Ok(PurgeableState(raw))
        } else {
            Err(invalid(format!("MTLPurgeableState {raw}")))
        }
    }
    pub fn raw(self) -> u64 {
        self.0
    }
}

/// Purgeability is not implemented: the memory always stays (Metal's answer for an object that
/// is never purged, NonVolatile, after any request).
fn purgeable(state: PurgeableState) -> Result<PurgeableState, Error> {
    let _ = state;
    Ok(PurgeableState(2))
}

// ---- geometry ----

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Origin {
    pub x: u64,
    pub y: u64,
    pub z: u64,
}

impl Origin {
    pub fn new(x: u64, y: u64, z: u64) -> Origin {
        Origin { x, y, z }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Region {
    pub origin: Origin,
    pub size: Size,
}

impl Region {
    pub fn new_3d(x: u64, y: u64, z: u64, width: u64, height: u64, depth: u64) -> Region {
        Region { origin: Origin::new(x, y, z), size: Size::new(width, height, depth) }
    }
}

pub(crate) fn sg_box(origin: Origin, size: Size) -> Result<ffi::sg_box, Error> {
    let c = |v: u64| i32::try_from(v).map_err(|_| invalid(format!("coordinate {v}")));
    Ok(ffi::sg_box {
        x: c(origin.x)?,
        y: c(origin.y)?,
        z: c(origin.z)?,
        width: c(size.width)?,
        height: c(size.height)?,
        depth: c(size.depth.max(1))?,
    })
}

/// What a command buffer uses, by the resource that owns the memory.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ResourceRef(pub(crate) u64);

/// The GPU address of a buffer's byte 0 (`gpuAddress`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct GpuAddress(pub u64);

// ---- buffers ----

/// Page-aligned host memory a buffer is made over (`newBufferWithBytesNoCopy:`).
#[derive(Clone, Copy, Debug)]
pub struct HostMemory(NonNull<u8>);

impl HostMemory {
    /// # Safety
    /// `pointer` is page aligned and stays mapped, readable and writable, for as long as any
    /// buffer over it lives.
    pub unsafe fn new(pointer: NonNull<u8>) -> HostMemory {
        HostMemory(pointer)
    }
}

/// A deallocator for no-copy memory (not taken: the caller keeps its memory).
pub type Deallocator = Box<dyn FnOnce() + Send>;

pub(crate) struct SgBuffer {
    #[allow(dead_code)] // keeps the device open while the buffer lives
    pub(crate) gpu: Arc<Gpu>,
    pub(crate) sg: *mut ffi::sg_buffer,
}

// SAFETY: the shim's buffers are plain GPU memory; access is the program's to order (Metal's rule).
unsafe impl Send for SgBuffer {}
unsafe impl Sync for SgBuffer {}

impl Drop for SgBuffer {
    fn drop(&mut self) {
        // SAFETY: from sg_buffer_new; the last reference.
        unsafe { ffi::sg_buffer_free(self.sg) }
    }
}

pub(crate) struct BufferInner {
    pub(crate) device: Device,
    pub(crate) sg: SgBuffer,
    pub(crate) length: u64,
    pub(crate) options: ResourceOptions,
    /// The caller's pages of a no-copy buffer (shadowed by `sg`).
    pub(crate) host: Option<NonNull<u8>>,
    pub(crate) id: u64,
}

// SAFETY: as SgBuffer; the host pointer is the caller's memory, used under Metal's rules.
unsafe impl Send for BufferInner {}
unsafe impl Sync for BufferInner {}

/// `MTLBuffer`.
#[derive(Clone)]
pub struct Buffer(pub(crate) Arc<BufferInner>);

impl PartialEq for Buffer {
    fn eq(&self, other: &Buffer) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for Buffer {}

fn new_sg_buffer(device: &Device, length: u64) -> Result<SgBuffer, Error> {
    if length > device.properties().max_buffer_length {
        return Err(invalid(format!("buffer of {length} bytes is past maxBufferLength")));
    }
    // SAFETY: valid device.
    let sg = unsafe { ffi::sg_buffer_new(device.gpu().sg, length.max(1)) };
    if sg.is_null() {
        return Err(gallium("buffer"));
    }
    Ok(SgBuffer { gpu: device.gpu().clone(), sg })
}

impl Device {
    /// `newBufferWithLength:options:`: zeroed memory.
    pub fn new_buffer(&self, length: u64, options: ResourceOptions) -> Result<Buffer, Error> {
        if options.storage_mode() == StorageMode::Memoryless {
            return Err(invalid("a memoryless buffer"));
        }
        let sg = new_sg_buffer(self, length)?;
        // SAFETY: a fresh mapping of at least `length` bytes.
        unsafe { std::ptr::write_bytes(ffi::sg_buffer_map(sg.sg) as *mut u8, 0, length as usize) };
        Ok(Buffer(Arc::new(BufferInner { device: self.clone(), sg, length, options, host: None, id: next_id() })))
    }

    /// `newBufferWithBytes:length:options:`.
    pub fn new_buffer_with_bytes(&self, bytes: &[u8], options: ResourceOptions) -> Result<Buffer, Error> {
        let buffer = self.new_buffer(bytes.len() as u64, options)?;
        // SAFETY: the mapping holds `bytes.len()` bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), ffi::sg_buffer_map(buffer.0.sg.sg) as *mut u8, bytes.len())
        };
        Ok(buffer)
    }

    /// `newBufferWithBytesNoCopy:length:options:deallocator:`, shadowed (the module docs): the
    /// pages are copied to the GPU before each command buffer that uses the buffer and back after
    /// it completes.
    ///
    /// # Safety
    /// `memory` holds `length` bytes and outlives the buffer.
    pub unsafe fn new_buffer_with_bytes_no_copy(&self, memory: HostMemory, length: u64, options: ResourceOptions,
                                                deallocator: Option<Deallocator>) -> Result<Buffer, Error> {
        if deallocator.is_some() {
            return Err(not_yet("a no-copy buffer with a deallocator"));
        }
        if (memory.0.as_ptr() as usize) % 4096 != 0 {
            return Err(invalid("no-copy memory that is not page aligned"));
        }
        let sg = new_sg_buffer(self, length)?;
        // SAFETY: the caller's contract; the mapping holds `length` bytes.
        unsafe { std::ptr::copy_nonoverlapping(memory.0.as_ptr(), ffi::sg_buffer_map(sg.sg) as *mut u8, length as usize) };
        Ok(Buffer(Arc::new(BufferInner {
            device: self.clone(),
            sg,
            length,
            options,
            host: Some(memory.0),
            id: next_id(),
        })))
    }
}

impl Buffer {
    /// `contents`: none for private storage.
    pub fn contents(&self) -> Option<NonNull<u8>> {
        if self.0.options.storage_mode() == StorageMode::Private {
            return None;
        }
        if let Some(host) = self.0.host {
            return Some(host);
        }
        // SAFETY: the shim's stable mapping.
        NonNull::new(unsafe { ffi::sg_buffer_map(self.0.sg.sg) } as *mut u8)
    }

    pub fn length(&self) -> u64 {
        self.0.length
    }

    pub fn gpu_address(&self) -> GpuAddress {
        // SAFETY: valid buffer.
        GpuAddress(unsafe { ffi::sg_buffer_iova(self.0.sg.sg) })
    }

    pub fn resource_options(&self) -> ResourceOptions {
        self.0.options
    }

    pub fn device(&self) -> Device {
        self.0.device.clone()
    }

    pub fn as_resource(&self) -> ResourceRef {
        ResourceRef(self.0.id)
    }

    pub fn set_purgeable_state(&self, state: PurgeableState) -> Result<PurgeableState, Error> {
        purgeable(state)
    }

    /// `newTextureWithDescriptor:offset:bytesPerRow:`: a linear texture over the buffer's memory.
    pub fn new_texture(&self, descriptor: &TextureDescriptor, offset: u64, bytes_per_row: u64) -> Result<Texture, Error> {
        Texture::over_buffer(self, descriptor, offset, bytes_per_row)
    }

    /// A texture over an IOSurface plane the buffer holds. On the PC (and until the SweetGPU
    /// import by IOSurface ID is wired, plan 4.3) the same as a texture over the buffer.
    pub fn new_texture_with_iosurface_plane(&self, descriptor: &TextureDescriptor, plane: IoSurfacePlane)
                                            -> Result<Texture, Error> {
        if plane.compressed {
            return Err(Error::Unsupported("a compressed IOSurface plane".into()));
        }
        Texture::over_buffer(self, descriptor, plane.offset, plane.bytes_per_row)
    }

    /// `newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:` (SPI).
    pub fn new_linear_texture(&self, descriptor: &TextureDescriptor, offset: u64, bytes_per_row: u64,
                              bytes_per_image: u64) -> Result<Texture, Error> {
        if descriptor.depth > 1 || descriptor.array_length > 1 {
            let _ = bytes_per_image;
            return Err(not_yet("a linear texture of several images"));
        }
        Texture::over_buffer(self, descriptor, offset, bytes_per_row)
    }
}

/// One plane of an IOSurface (offset and row pitch in the surface's memory).
#[derive(Clone, Copy, Debug)]
pub struct IoSurfacePlane {
    pub offset: u64,
    pub bytes_per_row: u64,
    pub compressed: bool,
}

impl IoSurfacePlane {
    pub fn new(offset: u64, bytes_per_row: u64, compressed: bool) -> IoSurfacePlane {
        IoSurfacePlane { offset, bytes_per_row, compressed }
    }
}

// ---- textures ----

/// `MTLTextureDescriptor`, a plain value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureDescriptor {
    pub texture_type: TextureType,
    pub pixel_format: PixelFormat,
    pub width: u64,
    pub height: u64,
    pub depth: u64,
    pub mipmap_level_count: u64,
    pub sample_count: u64,
    pub array_length: u64,
    pub usage: TextureUsage,
    pub resource_options: ResourceOptions,
}

impl Default for TextureDescriptor {
    /// Metal's defaults: a 1x1 2D RGBA8Unorm texture of one level, read by shaders.
    fn default() -> TextureDescriptor {
        TextureDescriptor {
            texture_type: TextureType::Type2D,
            pixel_format: PixelFormat::RGBA8Unorm,
            width: 1,
            height: 1,
            depth: 1,
            mipmap_level_count: 1,
            sample_count: 1,
            array_length: 1,
            usage: TextureUsage::SHADER_READ,
            resource_options: ResourceOptions::shared(),
        }
    }
}

impl TextureDescriptor {
    /// `texture2DDescriptorWithPixelFormat:width:height:mipmapped:`.
    pub fn texture_2d(format: PixelFormat, width: u64, height: u64, mipmapped: bool) -> TextureDescriptor {
        let levels = if mipmapped { 64 - width.max(height).max(1).leading_zeros() as u64 } else { 1 };
        TextureDescriptor { pixel_format: format, width, height, mipmap_level_count: levels, ..TextureDescriptor::default() }
    }
}

pub(crate) struct SgTexture {
    #[allow(dead_code)] // keeps the device open while the texture lives
    pub(crate) gpu: Arc<Gpu>,
    pub(crate) sg: *mut ffi::sg_texture,
}

// SAFETY: as SgBuffer.
unsafe impl Send for SgTexture {}
unsafe impl Sync for SgTexture {}

impl Drop for SgTexture {
    fn drop(&mut self) {
        // SAFETY: from sg_texture_new*, the last reference.
        unsafe { ffi::sg_texture_free(self.sg) }
    }
}

pub(crate) struct SgView {
    pub(crate) sg: *mut ffi::sg_view,
}

// SAFETY: as SgBuffer.
unsafe impl Send for SgView {}
unsafe impl Sync for SgView {}

impl Drop for SgView {
    fn drop(&mut self) {
        // SAFETY: from sg_view_new*, the last reference.
        unsafe { ffi::sg_view_free(self.sg) }
    }
}

/// The Metal view a texture object is of its image: format, type, levels and slices.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct ViewRange {
    pub(crate) format: PixelFormat,
    pub(crate) ty: TextureType,
    pub(crate) levels: (u32, u32),
    pub(crate) slices: (u32, u32),
}

pub(crate) struct TextureInner {
    pub(crate) device: Device,
    pub(crate) image: Arc<SgTexture>,
    /// The descriptor of the image (a view keeps its parent's).
    pub(crate) desc: TextureDescriptor,
    pub(crate) view: ViewRange,
    /// What owns the memory of a texture over a buffer.
    pub(crate) buffer: Option<Buffer>,
    pub(crate) id: u64,
    pub(crate) views: Mutex<Vec<(ffi::sg_view_desc, Arc<SgView>)>>,
}

/// `MTLTexture`.
#[derive(Clone)]
pub struct Texture(pub(crate) Arc<TextureInner>);

impl PartialEq for Texture {
    fn eq(&self, other: &Texture) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for Texture {}

pub(crate) fn vk_format(format: PixelFormat) -> Result<u32, Error> {
    metal2vulkan::metal::format::direct_format(format)
        .map(|f| f.as_raw() as u32)
        .ok_or_else(|| Error::Unsupported(format!("pixel format {format:?}")))
}

fn usage_bits(usage: TextureUsage) -> u32 {
    let mut u = 0;
    if usage.contains(TextureUsage::SHADER_READ) {
        u |= ffi::SG_USAGE_SAMPLE;
    }
    if usage.contains(TextureUsage::SHADER_WRITE) {
        u |= ffi::SG_USAGE_STORAGE;
    }
    if usage.contains(TextureUsage::RENDER_TARGET) {
        u |= ffi::SG_USAGE_RENDER;
    }
    u
}

fn sg_desc(d: &TextureDescriptor) -> Result<ffi::sg_texture_desc, Error> {
    let small = |v: u64, what: &str| u32::try_from(v).map_err(|_| invalid(format!("texture {what} {v}")));
    if d.width == 0 || d.height == 0 || d.depth == 0 || d.mipmap_level_count == 0 || d.array_length == 0 {
        return Err(invalid("a texture of no size"));
    }
    if d.resource_options.storage_mode() == StorageMode::Memoryless {
        // a memoryless attachment is a real texture here (sysmem rendering)
    }
    Ok(ffi::sg_texture_desc {
        ty: d.texture_type.as_raw(),
        vk_format: vk_format(d.pixel_format)?,
        width: small(d.width, "width")?,
        height: small(d.height, "height")?,
        depth: small(d.depth, "depth")?,
        array_size: small(d.array_length, "array length")?,
        levels: small(d.mipmap_level_count, "level count")?,
        samples: small(d.sample_count.max(1), "sample count")?,
        usage: usage_bits(d.usage),
        linear: 0,
    })
}

fn whole_view(d: &TextureDescriptor) -> ViewRange {
    let slices = match d.texture_type {
        TextureType::Cube | TextureType::CubeArray => d.array_length as u32 * 6,
        _ => d.array_length as u32,
    };
    ViewRange { format: d.pixel_format, ty: d.texture_type, levels: (0, d.mipmap_level_count as u32), slices: (0, slices) }
}

impl Device {
    /// `newTextureWithDescriptor:`.
    pub fn new_texture(&self, descriptor: &TextureDescriptor) -> Result<Texture, Error> {
        let desc = sg_desc(descriptor)?;
        // SAFETY: valid device and descriptor.
        let sg = unsafe { ffi::sg_texture_new(self.gpu().sg, &desc) };
        if sg.is_null() {
            return Err(gallium("texture"));
        }
        Ok(Texture(Arc::new(TextureInner {
            device: self.clone(),
            image: Arc::new(SgTexture { gpu: self.gpu().clone(), sg }),
            desc: *descriptor,
            view: whole_view(descriptor),
            buffer: None,
            id: next_id(),
            views: Mutex::new(Vec::new()),
        })))
    }
}

impl Texture {
    fn over_buffer(buffer: &Buffer, descriptor: &TextureDescriptor, offset: u64, bytes_per_row: u64) -> Result<Texture, Error> {
        if buffer.0.host.is_some() {
            // the shadow's bo is the GPU's memory: draws land there and the completion copies them
            // back to the pages (resource.rs module docs)
        }
        let mut desc = sg_desc(descriptor)?;
        desc.linear = 1;
        let row = u32::try_from(bytes_per_row).map_err(|_| invalid("bytesPerRow"))?;
        let device = &buffer.0.device;
        // SAFETY: valid device, buffer and descriptor.
        let sg = unsafe { ffi::sg_texture_new_on_buffer(device.gpu().sg, &desc, buffer.0.sg.sg, offset, row) };
        if sg.is_null() {
            return Err(gallium("texture over a buffer"));
        }
        Ok(Texture(Arc::new(TextureInner {
            device: device.clone(),
            image: Arc::new(SgTexture { gpu: device.gpu().clone(), sg }),
            desc: *descriptor,
            view: whole_view(descriptor),
            buffer: Some(buffer.clone()),
            id: next_id(),
            views: Mutex::new(Vec::new()),
        })))
    }

    pub fn texture_type(&self) -> TextureType {
        self.0.view.ty
    }
    pub fn pixel_format(&self) -> PixelFormat {
        self.0.view.format
    }
    pub fn width(&self) -> u64 {
        (self.0.desc.width >> self.0.view.levels.0).max(1)
    }
    pub fn height(&self) -> u64 {
        (self.0.desc.height >> self.0.view.levels.0).max(1)
    }
    pub fn depth(&self) -> u64 {
        (self.0.desc.depth >> self.0.view.levels.0).max(1)
    }
    pub fn mipmap_level_count(&self) -> u64 {
        (self.0.view.levels.1 - self.0.view.levels.0) as u64
    }
    pub fn sample_count(&self) -> u64 {
        self.0.desc.sample_count
    }
    pub fn array_length(&self) -> u64 {
        match self.0.view.ty {
            TextureType::Cube => 1,
            TextureType::CubeArray => ((self.0.view.slices.1 - self.0.view.slices.0) / 6) as u64,
            TextureType::Type2DArray | TextureType::Type1DArray | TextureType::Type2DMultisampleArray => {
                (self.0.view.slices.1 - self.0.view.slices.0) as u64
            }
            _ => 1,
        }
    }
    pub fn usage(&self) -> TextureUsage {
        self.0.desc.usage
    }
    pub fn resource_options(&self) -> ResourceOptions {
        self.0.desc.resource_options
    }
    pub fn device(&self) -> Device {
        self.0.device.clone()
    }
    /// What owns the texture's memory: its buffer for a texture over one.
    pub fn as_resource(&self) -> ResourceRef {
        match &self.0.buffer {
            Some(b) => b.as_resource(),
            None => ResourceRef(self.0.id),
        }
    }
    pub(crate) fn ids(&self) -> [u64; 2] {
        [self.0.id, self.0.buffer.as_ref().map_or(0, |b| b.0.id)]
    }

    pub fn set_purgeable_state(&self, state: PurgeableState) -> Result<PurgeableState, Error> {
        purgeable(state)
    }

    /// `newTextureViewWithPixelFormat:`.
    pub fn new_texture_view(&self, format: PixelFormat) -> Result<Texture, Error> {
        self.view(ViewRange { format, ..self.0.view })
    }

    /// `newTextureViewWithPixelFormat:textureType:levels:slices:`.
    pub fn new_texture_view_with_levels_slices(&self, format: PixelFormat, ty: TextureType, levels: Range<u64>,
                                               slices: Range<u64>) -> Result<Texture, Error> {
        let base = self.0.view;
        let (l0, l1) = (base.levels.0 as u64 + levels.start, base.levels.0 as u64 + levels.end);
        let (s0, s1) = (base.slices.0 as u64 + slices.start, base.slices.0 as u64 + slices.end);
        if l1 > base.levels.1 as u64 || s1 > base.slices.1 as u64 || l0 >= l1 || s0 >= s1 {
            return Err(invalid("texture view past its texture's levels or slices"));
        }
        self.view(ViewRange { format, ty, levels: (l0 as u32, l1 as u32), slices: (s0 as u32, s1 as u32) })
    }

    fn view(&self, view: ViewRange) -> Result<Texture, Error> {
        vk_format(view.format)?;
        Ok(Texture(Arc::new(TextureInner {
            device: self.0.device.clone(),
            image: self.0.image.clone(),
            desc: self.0.desc,
            view,
            buffer: self.0.buffer.clone(),
            id: self.0.id,
            views: Mutex::new(Vec::new()),
        })))
    }

    /// The level and slice of this object's view (`level`, `slice` relative to it).
    pub(crate) fn image_level_slice(&self, level: u64, slice: u64) -> Result<(u32, u32), Error> {
        let v = self.0.view;
        let l = v.levels.0 as u64 + level;
        let s = v.slices.0 as u64 + slice;
        if l >= v.levels.1 as u64 || s >= v.slices.1 as u64 {
            return Err(invalid(format!("level {level} slice {slice} past the texture")));
        }
        Ok((l as u32, s as u32))
    }

    /// `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:`.
    pub fn replace_region(&self, region: Region, level: u64, slice: u64, bytes: &[u8], bytes_per_row: u64,
                          bytes_per_image: u64) -> Result<(), Error> {
        let (l, s) = self.image_level_slice(level, slice)?;
        let b = sg_box(region.origin, region.size)?;
        let need = bytes_per_image.max(bytes_per_row * region.size.height) * region.size.depth.max(1);
        if (bytes.len() as u64) < need.min(bytes_per_row * region.size.height.max(1)) {
            return Err(invalid("replaceRegion: fewer bytes than the region"));
        }
        // SAFETY: valid texture; the bytes hold the region (checked above).
        let ok = unsafe {
            ffi::sg_texture_write(self.0.device.gpu().sg, self.0.image.sg, l, s, &b, bytes.as_ptr() as *const _,
                                  bytes_per_row as u32, bytes_per_image as u32)
        };
        if ok { Ok(()) } else { Err(gallium("replaceRegion")) }
    }

    /// `getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:`.
    pub fn get_bytes(&self, bytes: &mut [u8], bytes_per_row: u64, bytes_per_image: u64, region: Region, level: u64,
                     slice: u64) -> Result<(), Error> {
        let (l, s) = self.image_level_slice(level, slice)?;
        let b = sg_box(region.origin, region.size)?;
        let rows = region.size.height.max(1);
        if (bytes.len() as u64) < bytes_per_row * (rows - 1) + bytes_per_row.min(bytes.len() as u64) {
            return Err(invalid("getBytes: fewer bytes than the region"));
        }
        // SAFETY: valid texture; the bytes hold the region.
        let ok = unsafe {
            ffi::sg_texture_read(self.0.device.gpu().sg, self.0.image.sg, l, s, &b, bytes.as_mut_ptr() as *mut _,
                                 bytes_per_row as u32, bytes_per_image as u32)
        };
        if ok { Ok(()) } else { Err(gallium("getBytes")) }
    }

    /// A Gallium sampler view of this texture for a binder's `ImageViewDesc` (cached).
    pub(crate) fn sampler_view(&self, desc: ffi::sg_view_desc) -> Result<Arc<SgView>, Error> {
        let mut views = self.0.views.lock().unwrap_or_else(|p| p.into_inner());
        if let Some((_, v)) = views.iter().find(|(d, _)| {
            d.vk_format == desc.vk_format && d.ty == desc.ty && d.first_level == desc.first_level
                && d.last_level == desc.last_level && d.first_layer == desc.first_layer
                && d.last_layer == desc.last_layer && d.swizzle == desc.swizzle
        }) {
            return Ok(v.clone());
        }
        // SAFETY: valid texture and descriptor.
        let sg = unsafe { ffi::sg_view_new(self.0.device.gpu().sg, self.0.image.sg, &desc) };
        if sg.is_null() {
            return Err(gallium("sampler view"));
        }
        let view = Arc::new(SgView { sg });
        views.push((desc, view.clone()));
        Ok(view)
    }
}

// ---- heaps ----

/// `MTLHeapType`: Automatic 0, Placement 1, Sparse 2.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HeapType {
    Automatic,
    Placement,
    Sparse,
}

impl HeapType {
    pub fn from_raw(raw: u64) -> Result<HeapType, Error> {
        match raw {
            0 => Ok(HeapType::Automatic),
            1 => Ok(HeapType::Placement),
            2 => Ok(HeapType::Sparse),
            _ => Err(invalid(format!("MTLHeapType {raw}"))),
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct HeapDescriptor {
    pub heap_type: HeapType,
    pub size: u64,
    pub resource_options: ResourceOptions,
}

impl HeapDescriptor {
    pub fn new(heap_type: HeapType, size: u64, resource_options: ResourceOptions) -> HeapDescriptor {
        HeapDescriptor { heap_type, size, resource_options }
    }
}

/// `MTLHeap`: not yet (every constructor refuses, so no heap object exists).
pub struct Heap(());

const HEAPS: &str = "heaps (MTLHeap)";

impl Device {
    pub fn new_heap(&self, _descriptor: &HeapDescriptor) -> Result<Heap, Error> {
        Err(not_yet(HEAPS))
    }
}

impl Heap {
    pub fn size(&self) -> u64 {
        0
    }
    pub fn used_size(&self) -> u64 {
        0
    }
    pub fn current_allocated_size(&self) -> u64 {
        0
    }
    pub fn max_available_size(&self, _alignment: u64) -> Result<u64, Error> {
        Err(not_yet(HEAPS))
    }
    pub fn set_purgeable_state(&self, _state: PurgeableState) -> Result<PurgeableState, Error> {
        Err(not_yet(HEAPS))
    }
    pub fn new_buffer(&self, _length: u64, _options: ResourceOptions) -> Result<Buffer, Error> {
        Err(not_yet(HEAPS))
    }
    pub fn new_buffer_at_offset(&self, _length: u64, _options: ResourceOptions, _offset: u64) -> Result<Buffer, Error> {
        Err(not_yet(HEAPS))
    }
    pub fn new_texture(&self, _descriptor: &TextureDescriptor) -> Result<Texture, Error> {
        Err(not_yet(HEAPS))
    }
    pub fn new_texture_at_offset(&self, _descriptor: &TextureDescriptor, _offset: u64) -> Result<Texture, Error> {
        Err(not_yet(HEAPS))
    }
}

// ---- neutral resources (what a plan binds where nothing compatible is bound) ----

#[derive(Default)]
pub(crate) struct Neutral {
    zero: Option<Buffer>,
    sink: Option<Buffer>,
}

impl Device {
    /// A zeroed buffer of at least `min` bytes (reads of an unbound slot).
    pub(crate) fn neutral_zero(&self, min: u64) -> Result<Buffer, Error> {
        let mut n = self.0.neutral.lock().unwrap_or_else(|p| p.into_inner());
        if n.zero.as_ref().is_none_or(|b| b.length() < min) {
            n.zero = Some(self.new_buffer(min.max(65536), ResourceOptions::shared())?);
        }
        Ok(n.zero.clone().unwrap())
    }

    /// A buffer of at least `min` bytes that takes writes nobody reads.
    pub(crate) fn neutral_sink(&self, min: u64) -> Result<Buffer, Error> {
        let mut n = self.0.neutral.lock().unwrap_or_else(|p| p.into_inner());
        if n.sink.as_ref().is_none_or(|b| b.length() < min) {
            n.sink = Some(self.new_buffer(min.max(4096), ResourceOptions::shared())?);
        }
        Ok(n.sink.clone().unwrap())
    }
}
