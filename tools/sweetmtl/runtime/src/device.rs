// SPDX-License-Identifier: MIT
//! `Device` (`MTLDevice`): the Gallium shim's device (one pipe_screen and pipe_context per
//! process) beside the metal2vulkan translator's device for the a540 profile, and the answers to
//! Metal's device queries.

use crate::error::{gallium, Error, RuntimeGap};
use crate::ffi;
use crate::families::{FamilyRow, GpuFamily, RowAudit};
use metal2vulkan::device::{DeviceDescription, DevicePolicy};
use metal2vulkan::metal::PixelFormat;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

/// The device profile the translator targets: tools/sweetmtl/profile/a540-gallium.json, M0's profile (what Gallium a5xx
/// provides or emulates) with the format table freedreno states for the a540 (profile/make-profile.py).
/// `SWEETMTL_PROFILE=path` replaces it, for experiments.
static PROFILE: &str = include_str!("../../profile/a540-gallium.json");

/// The Metal release whose measured answers a level stands for (kept for the bridge's options;
/// this runtime answers from the device, not from a measured Apple GPU).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MetalLevel {
    MacOS26_7,
    MacOS27_0_1,
}

impl MetalLevel {
    pub const NEWEST: MetalLevel = MetalLevel::MacOS27_0_1;
}

/// Where compiled pipelines are kept between processes. Not used yet: every process compiles
/// its pipelines (the statistics stay zero).
#[derive(Clone, Debug, Default)]
pub struct ShaderCacheOptions {
    pub directory: Option<PathBuf>,
}

impl ShaderCacheOptions {
    pub fn in_directory(dir: impl Into<PathBuf>) -> ShaderCacheOptions {
        ShaderCacheOptions { directory: Some(dir.into()) }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShaderCacheStatistics {
    pub hits: u64,
    pub misses: u64,
    pub rejected: u64,
    pub read_failures: u64,
    pub writes: u64,
    pub write_failures: u64,
    pub evictions: u64,
    pub driver_loads: u64,
    pub driver_pipeline_hits: u64,
    pub driver_pipeline_misses: u64,
    pub driver_saves: u64,
    pub driver_data_truncated: u64,
    pub driver_data_mismatched: u64,
    pub driver_data_unchanged: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum MessageSeverity {
    Verbose,
    Info,
    Warning,
    Error,
}

/// One message of the runtime's to its consumer (the bridge counts pipeline creations by them).
#[derive(Clone, Debug)]
pub struct DebugMessage {
    pub severity: MessageSeverity,
    pub id_name: String,
    pub text: String,
}

/// Where the device's messages go.
#[derive(Clone)]
pub struct MessageSink(Arc<dyn Fn(&DebugMessage) + Send + Sync>);

impl MessageSink {
    pub fn new(f: impl Fn(&DebugMessage) + Send + Sync + 'static) -> MessageSink {
        MessageSink(Arc::new(f))
    }
}

impl std::fmt::Debug for MessageSink {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("MessageSink")
    }
}

#[derive(Clone, Debug, Default)]
pub struct DeviceOptions {
    /// flame's Vulkan loader path: there is no loader here (ignored).
    pub loader: Option<PathBuf>,
    pub metal_level: Option<MetalLevel>,
    pub shader_cache: ShaderCacheOptions,
    pub messages: Option<MessageSink>,
}

/// `MTLArgumentBuffersTier` / `MTLReadWriteTextureTier` as raw Metal values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tier(pub u32);

impl Tier {
    pub fn as_raw(self) -> u32 {
        self.0
    }
}

/// Metal's three-dimensional size.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Size {
    pub width: u64,
    pub height: u64,
    pub depth: u64,
}

impl Size {
    pub fn new(width: u64, height: u64, depth: u64) -> Size {
        Size { width, height, depth }
    }
}

/// Metal's public device properties, from the translator's answers for the a540 profile and the
/// Gallium device.
#[derive(Clone, Debug)]
pub struct DeviceProperties {
    pub max_buffer_length: u64,
    pub max_threads_per_threadgroup: Size,
    pub max_threadgroup_memory_length: u64,
    pub argument_buffers_support: Tier,
    pub read_write_texture_support: Tier,
    pub supports_32bit_float_filtering: bool,
    pub vendor_id: u32,
    pub description: DeviceDescription,
    pub(crate) supports_depth24_stencil8: bool,
    pub(crate) sample_counts: u32,
}

impl DeviceProperties {
    /// `areRasterOrderGroupsSupported`: no pixel interlock on a5xx (M0: the two raster-order
    /// shaders of QuartzCore are refused).
    pub fn are_raster_order_groups_supported(&self) -> Result<bool, RuntimeGap> {
        Ok(false)
    }
    pub fn is_depth24_stencil8_pixel_format_supported(&self) -> Result<bool, RuntimeGap> {
        Ok(self.supports_depth24_stencil8)
    }
    pub fn are_programmable_sample_positions_supported(&self) -> Result<bool, RuntimeGap> {
        Ok(false)
    }
    pub fn supports_sampler_reduction_mode(&self) -> Result<bool, RuntimeGap> {
        Ok(false)
    }
    /// `minimumLinearTextureAlignmentForPixelFormat:`: a texture over a buffer starts at a 64-byte
    /// aligned offset with rows 64-byte aligned (the a5xx layout the phone's Mesa patch admits).
    pub fn minimum_linear_texture_alignment_for_pixel_format(&self, format: PixelFormat) -> Result<u64, Error> {
        let vk = metal2vulkan::metal::format::direct_format(format)
            .ok_or_else(|| Error::Unsupported(format!("{format:?} has no linear layout here")))?;
        // SAFETY: plain value query.
        let (bytes, w) = unsafe {
            (ffi::sg_format_block_bytes(vk.as_raw() as u32), ffi::sg_format_block_width(vk.as_raw() as u32))
        };
        if bytes == 0 || w != 1 {
            return Err(Error::Invalid(format!("{format:?}: a block format has no linear texture")));
        }
        Ok(64)
    }
    pub fn supports_texture_sample_count(&self, count: u64) -> Result<bool, RuntimeGap> {
        Ok(count <= 32 && count > 0 && (self.sample_counts & count as u32) != 0)
    }
    pub fn supports_vertex_amplification_count(&self, count: u64) -> Result<bool, RuntimeGap> {
        Ok(count == 1)
    }
    pub fn supports_rasterization_rate_map_with_layer_count(&self, _layers: u64) -> Result<bool, RuntimeGap> {
        Ok(false)
    }
    /// `supportsPrimitiveType:` (MTLPrimitiveType: point, line, line strip, triangle, strip).
    pub fn supports_primitive_type(&self, raw: u64) -> bool {
        raw <= 4
    }
    /// `supportsTextureWriteRoundingMode:`: the native mode only.
    pub fn supports_texture_write_rounding_mode(&self, raw: u64) -> bool {
        raw == 0
    }
}

/// A shim handle shared by everything that uses the GPU.
pub(crate) struct Gpu {
    pub(crate) sg: *mut ffi::sg_device,
}

// SAFETY: the shim serializes context use behind sg_lock; the rest of its calls are thread-safe.
unsafe impl Send for Gpu {}
unsafe impl Sync for Gpu {}

impl Drop for Gpu {
    fn drop(&mut self) {
        // SAFETY: the device outlives every object (each holds an Arc of it).
        unsafe { ffi::sg_device_close(self.sg) }
    }
}

/// The device lock (sg_lock), held while the device's pipe_context is used.
pub(crate) struct GpuLock<'a>(&'a Gpu);

impl Gpu {
    pub(crate) fn lock(&self) -> GpuLock<'_> {
        // SAFETY: a valid device; released by the guard.
        unsafe { ffi::sg_lock(self.sg) };
        GpuLock(self)
    }
}

impl GpuLock<'_> {
    pub(crate) fn sg(&self) -> *mut ffi::sg_device {
        self.0.sg
    }
}

impl Drop for GpuLock<'_> {
    fn drop(&mut self) {
        // SAFETY: taken in Gpu::lock.
        unsafe { ffi::sg_unlock(self.0.sg) }
    }
}

pub(crate) struct DeviceInner {
    pub(crate) gpu: Arc<Gpu>,
    pub(crate) translator: metal2vulkan::device::Device,
    pub(crate) props: DeviceProperties,
    pub(crate) name: String,
    pub(crate) messages: Option<MessageSink>,
    pub(crate) completion: crate::command::Completion,
    pub(crate) neutral: Mutex<crate::resource::Neutral>,
}

/// `MTLDevice`. Cheap to clone; the GPU stays open while any clone or object of it lives.
#[derive(Clone)]
pub struct Device(pub(crate) Arc<DeviceInner>);

impl PartialEq for Device {
    fn eq(&self, other: &Device) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for Device {}

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

/// `SWEETMTL_TRACE=1`: a line per pipeline, command buffer and failure on stderr (on iOS also to
/// /dev/null, which taimen's kernel tees into its log).
pub(crate) fn trace_enabled() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("SWEETMTL_TRACE").is_some_and(|v| v != "0"))
}

pub(crate) fn trace(line: std::fmt::Arguments) {
    if !trace_enabled() {
        return;
    }
    let text = format!("sweetmtl: {line}\n");
    eprint!("{text}");
    #[cfg(target_os = "ios")]
    if !stderr_is_dev_null() {
        if let Ok(mut null) = std::fs::OpenOptions::new().write(true).open("/dev/null") {
            use std::io::Write;
            let _ = null.write_all(text.as_bytes());
        }
    }
}

/// Whether stderr is /dev/null already (a launchd job's), so the trace line reached the kernel's
/// tee once and needs no second write.
#[cfg(target_os = "ios")]
fn stderr_is_dev_null() -> bool {
    static NULL: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *NULL.get_or_init(|| {
        // SAFETY: plain stat calls into zeroed structs.
        unsafe {
            let mut err: libc::stat = std::mem::zeroed();
            let mut null: libc::stat = std::mem::zeroed();
            libc::fstat(2, &mut err) == 0
                && libc::stat(c"/dev/null".as_ptr(), &mut null) == 0
                && (err.st_mode & libc::S_IFMT) == libc::S_IFCHR
                && err.st_rdev == null.st_rdev
        }
    })
}

/// A process-unique id for a resource (never reused).
pub(crate) fn next_id() -> u64 {
    NEXT_ID.fetch_add(1, Ordering::Relaxed)
}

impl Device {
    /// The system's device: the Adreno 540 through the shim.
    pub fn system_default() -> Result<Device, Error> {
        Device::system_default_with(&DeviceOptions::default())
    }

    pub fn system_default_with(options: &DeviceOptions) -> Result<Device, Error> {
        let json = match std::env::var_os("SWEETMTL_PROFILE") {
            Some(path) => std::fs::read_to_string(&path)
                .map_err(|e| Error::Invalid(format!("SWEETMTL_PROFILE {}: {e}", PathBuf::from(&path).display())))?,
            None => PROFILE.to_string(),
        };
        let description = DeviceDescription::from_khronos_json(&json, None)?;
        let translator = metal2vulkan::device::Device::new(description.clone(), DevicePolicy::default())?;
        // SAFETY: opens the platform's GPU; null with a reason on failure.
        let sg = unsafe { ffi::sg_device_open() };
        if sg.is_null() {
            return Err(gallium("device"));
        }
        let gpu = Arc::new(Gpu { sg });
        // SAFETY: valid device.
        let gpu_id = unsafe { ffi::sg_device_gpu_id(sg) };
        let m = translator.metal_properties();
        let props = DeviceProperties {
            max_buffer_length: m.max_buffer_length.min(u32::MAX as u64),
            max_threads_per_threadgroup: Size::new(
                m.max_threads_per_threadgroup[0] as u64,
                m.max_threads_per_threadgroup[1] as u64,
                m.max_threads_per_threadgroup[2] as u64,
            ),
            max_threadgroup_memory_length: m.max_threadgroup_memory_length as u64,
            // MTLArgumentBuffersTier1 = 0; MTLReadWriteTextureTier1 = 1 (the profile's tiers)
            argument_buffers_support: Tier(0),
            read_write_texture_support: Tier(1),
            supports_32bit_float_filtering: m.supports_32bit_float_filtering,
            vendor_id: 0x5143,
            supports_depth24_stencil8: m.supports_depth24_stencil8,
            sample_counts: m.raster_sample_counts.as_raw() as u32,
            description,
        };
        // gpu id 0: not freedreno (the PC's softpipe validation mode, SG_DRIVER=softpipe)
        let name = if gpu_id != 0 {
            format!("Adreno (TM) {gpu_id} (sweetmtl)")
        } else {
            "softpipe (sweetmtl validation)".to_string()
        };
        let completion = crate::command::Completion::start(gpu.clone());
        Ok(Device(Arc::new(DeviceInner {
            gpu,
            translator,
            props,
            name,
            messages: options.messages.clone(),
            completion,
            neutral: Mutex::new(crate::resource::Neutral::default()),
        })))
    }

    pub fn name(&self) -> &str {
        &self.0.name
    }

    pub fn properties(&self) -> &DeviceProperties {
        &self.0.props
    }

    /// The translator's device for the a540 profile (pipelines compile against it).
    pub fn translator(&self) -> &metal2vulkan::device::Device {
        &self.0.translator
    }

    pub fn private_properties(&self) -> crate::families::PrivateProperties {
        crate::families::PrivateProperties::of(self)
    }

    pub fn shader_cache_statistics(&self) -> ShaderCacheStatistics {
        ShaderCacheStatistics::default()
    }

    /// The rows of Apple's family tables this device misses in `family`. Not audited yet: none
    /// are listed (supportsFamily: answers false for every family of the device's own).
    pub fn family_blockers(&self, _family: GpuFamily) -> Vec<RowAudit> {
        Vec::new()
    }

    /// The device's rows of `family`'s table. Not audited yet: none.
    pub fn family_rows(&self, _family: GpuFamily) -> Vec<RowAudit> {
        Vec::new()
    }

    /// `supportsFamily:` on the device's own: no Apple family is met exactly (the bridge adds the
    /// target hardware's families on top).
    pub fn supports_family(&self, _raw: i64) -> bool {
        false
    }

    /// What `format` can do here: sampled, filtered, rendered, blended, written, multisampled.
    pub fn pixel_format_capabilities(&self, format: PixelFormat) -> crate::families::FormatCapabilities {
        crate::families::FormatCapabilities::of(self, format)
    }

    /// The format's block layout: words[0] bytes per block, flags 0x400 for a block (compressed)
    /// format; zeroes for a format the device has no layout of.
    pub fn pixel_format_info(&self, raw: u64) -> crate::families::PixelFormatInfo {
        crate::families::PixelFormatInfo::of(raw)
    }

    /// The shim's counters (draws, dispatches, flushes, ...): what a drm-shim run can check.
    pub fn gallium_stats(&self) -> ffi::sg_stats {
        let mut s = ffi::sg_stats::default();
        // SAFETY: valid device, writable out.
        unsafe { ffi::sg_device_stats(self.0.gpu.sg, &mut s) };
        s
    }

    pub(crate) fn gpu(&self) -> &Arc<Gpu> {
        &self.0.gpu
    }

    pub(crate) fn message(&self, severity: MessageSeverity, id_name: &str, text: String) {
        if let Some(sink) = &self.0.messages {
            (sink.0)(&DebugMessage { severity, id_name: id_name.to_string(), text });
        }
    }
}

impl FamilyRow {
    /// The row's name in Apple's tables.
    pub fn label(&self) -> String {
        match self {
            FamilyRow::Feature(f) => format!("{f:?}"),
            FamilyRow::Format(pf, _) => format!("{pf:?}"),
            FamilyRow::Limit(l) => l.to_string(),
        }
    }
}
