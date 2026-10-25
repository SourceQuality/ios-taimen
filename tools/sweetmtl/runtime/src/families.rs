// SPDX-License-Identifier: MIT
//! Metal's GPU families, Apple's private (SPI) device properties and per-format capabilities, as
//! this device answers them.
//!
//! The private properties are the selectors CoreAnimation and the other frameworks read off an
//! `MTLDevice` (the frontend installs each as a method). The answers here are the a540's through
//! Gallium (limits of the a540 profile and of freedreno a5xx); a property with no answer of the
//! device's is `None`, and the frontend then leaves the base class's 0/NO. They are best effort
//! for M1 and are checked against what clients do on the phone.

use crate::device::Device;
use crate::ffi;
use metal2vulkan::metal::PixelFormat;

/// `MTLGPUFamily`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum GpuFamily {
    Apple1,
    Apple2,
    Apple3,
    Apple4,
    Apple5,
    Apple6,
    Apple7,
    Apple8,
    Apple9,
    Mac1,
    Mac2,
    Common1,
    Common2,
    Common3,
    MacCatalyst1,
    MacCatalyst2,
    Metal3,
}

impl GpuFamily {
    pub fn raw(self) -> i64 {
        use GpuFamily::*;
        match self {
            Apple1 => 1001,
            Apple2 => 1002,
            Apple3 => 1003,
            Apple4 => 1004,
            Apple5 => 1005,
            Apple6 => 1006,
            Apple7 => 1007,
            Apple8 => 1008,
            Apple9 => 1009,
            Mac1 => 2001,
            Mac2 => 2002,
            Common1 => 3001,
            Common2 => 3002,
            Common3 => 3003,
            MacCatalyst1 => 4001,
            MacCatalyst2 => 4002,
            Metal3 => 5001,
        }
    }
}

/// A feature row of Apple's family tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FeatureRow {
    SimdBarrier,
    SimdPermute,
    FramebufferFetch,
    RasterOrderGroups,
    TileShaders,
    Imageblocks,
}

/// A format capability of Apple's format tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FormatCapability {
    Filter,
    Write,
    Color,
    Blend,
    Msaa,
    Resolve,
}

/// One row of Apple's family tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FamilyRow {
    Feature(FeatureRow),
    Format(PixelFormat, FormatCapability),
    Limit(&'static str),
}

/// How the device meets a row.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RowState {
    Exact,
    Approximate,
    Missing,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RowAudit {
    pub row: FamilyRow,
    pub state: RowState,
}

/// An answer to a private property: the device's own, or none.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PrivateAnswer<T>(Option<T>);

impl<T: Copy> PrivateAnswer<T> {
    /// The device's answer, where it has one.
    pub fn value(self) -> Option<T> {
        self.0
    }
}

macro_rules! private_list {
    ($name:ident, $ty:ty, { $($variant:ident = $sel:literal => $answer:expr,)* }) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum $name {
            $($variant,)*
        }
        impl $name {
            pub const ALL: [$name; [$($sel),*].len()] = [$($name::$variant),*];
            /// Metal's selector.
            pub fn selector(self) -> &'static str {
                match self {
                    $($name::$variant => $sel,)*
                }
            }
            fn answer(self, d: &Answers) -> Option<$ty> {
                let _ = d;
                match self {
                    $($name::$variant => $answer(d),)*
                }
            }
        }
    };
}

/// What the answers are computed from.
struct Answers {
    max_total_threads: u64,
    max_threadgroup_memory: u64,
    max_texture_2d: u64,
    max_layers: u64,
}

private_list!(PrivateCount, u64, {
    GpuCoreCount = "gpuCoreCount" => |_: &Answers| None,
    MaxVertexAmplificationCount = "maxVertexAmplificationCount" => |_: &Answers| Some(1),
    DeviceLinearTextureAlignmentBytes = "deviceLinearTextureAlignmentBytes" => |_: &Answers| Some(64),
    LinearTextureAlignmentBytes = "linearTextureAlignmentBytes" => |_: &Answers| Some(64),
    IosurfaceTextureAlignmentBytes = "iosurfaceTextureAlignmentBytes" => |_: &Answers| Some(64),
    IosurfaceReadOnlyTextureAlignmentBytes = "iosurfaceReadOnlyTextureAlignmentBytes" => |_: &Answers| Some(64),
    MaxVertexBuffers = "maxVertexBuffers" => |_: &Answers| Some(31),
    MaxFragmentBuffers = "maxFragmentBuffers" => |_: &Answers| Some(31),
    MaxComputeBuffers = "maxComputeBuffers" => |_: &Answers| Some(31),
    MaxTileBuffers = "maxTileBuffers" => |_: &Answers| Some(0),
    MaxVertexTextures = "maxVertexTextures" => |_: &Answers| Some(16),
    MaxFragmentTextures = "maxFragmentTextures" => |_: &Answers| Some(16),
    MaxComputeTextures = "maxComputeTextures" => |_: &Answers| Some(16),
    MaxTileTextures = "maxTileTextures" => |_: &Answers| Some(0),
    MaxVertexSamplers = "maxVertexSamplers" => |_: &Answers| Some(16),
    MaxFragmentSamplers = "maxFragmentSamplers" => |_: &Answers| Some(16),
    MaxComputeSamplers = "maxComputeSamplers" => |_: &Answers| Some(16),
    MaxTileSamplers = "maxTileSamplers" => |_: &Answers| Some(0),
    MaxComputeLocalMemorySizes = "maxComputeLocalMemorySizes" => |_: &Answers| Some(31),
    MaxComputeThreadgroupMemory = "maxComputeThreadgroupMemory" => |a: &Answers| Some(a.max_threadgroup_memory),
    MaxComputeThreadgroupMemoryAlignmentBytes = "maxComputeThreadgroupMemoryAlignmentBytes" => |_: &Answers| Some(16),
    MaxPredicatedNestingDepth = "maxPredicatedNestingDepth" => |_: &Answers| None,
    MaxRasterizationRateLayerCount = "maxRasterizationRateLayerCount" => |_: &Answers| Some(0),
    MaxTextureWidth2D = "maxTextureWidth2D" => |a: &Answers| Some(a.max_texture_2d),
    MaxTextureHeight2D = "maxTextureHeight2D" => |a: &Answers| Some(a.max_texture_2d),
    MaxTextureLayers = "maxTextureLayers" => |a: &Answers| Some(a.max_layers),
    MaxTotalComputeThreadsPerThreadgroup = "maxTotalComputeThreadsPerThreadgroup" => |a: &Answers| Some(a.max_total_threads),
    MaxViewportCount = "maxViewportCount" => |_: &Answers| Some(1),
});

private_list!(PrivateFlag, bool, {
    // framebuffer fetch is emulated: a texture read of the attachment behind a barrier per draw
    IsFramebufferReadSupported = "isFramebufferReadSupported" => |_: &Answers| Some(true),
    IsLargeMrtSupported = "isLargeMRTSupported" => |_: &Answers| Some(false),
    IsRgb10a2GammaSupported = "isRGB10A2GammaSupported" => |_: &Answers| Some(false),
    SupportsBufferWithAddressRanges = "supportsBufferWithAddressRanges" => |_: &Answers| Some(false),
    SupportsBufferWithIosurface = "supportsBufferWithIOSurface" => |_: &Answers| Some(false),
    SupportsCommandBufferJump = "supportsCommandBufferJump" => |_: &Answers| Some(false),
    SupportsComputeCompressedTextureWrite = "supportsComputeCompressedTextureWrite" => |_: &Answers| Some(false),
    SupportsConditionalLoadStore = "supportsConditionalLoadStore" => |_: &Answers| Some(false),
    SupportsDynamicAttributeStride = "supportsDynamicAttributeStride" => |_: &Answers| Some(false),
    SupportsExtendedXr10Formats = "supportsExtendedXR10Formats" => |_: &Answers| Some(false),
    SupportsFloat16BcubicFiltering = "supportsFloat16BCubicFiltering" => |_: &Answers| Some(false),
    SupportsHeapWithAddressRanges = "supportsHeapWithAddressRanges" => |_: &Answers| Some(false),
    SupportsImageBlocks = "supportsImageBlocks" => |_: &Answers| Some(false),
    SupportsMemoryOrderAtomics = "supportsMemoryOrderAtomics" => |_: &Answers| Some(false),
    SupportsNativeHardwareFp16 = "supportsNativeHardwareFP16" => |_: &Answers| Some(true),
    SupportsNonSquareTileShaders = "supportsNonSquareTileShaders" => |_: &Answers| Some(false),
    SupportsProgrammableSamplePositions = "supportsProgrammableSamplePositions" => |_: &Answers| Some(false),
    SupportsResourceDetachBacking = "supportsResourceDetachBacking" => |_: &Answers| Some(false),
    SupportsSharedTextureHandles = "supportsSharedTextureHandles" => |_: &Answers| Some(false),
    SupportsSimdReduction = "supportsSIMDReduction" => |_: &Answers| Some(false),
    SupportsSimdShuffleAndFill = "supportsSIMDShuffleAndFill" => |_: &Answers| Some(false),
    SupportsTexture2dMultisampleArray = "supportsTexture2DMultisampleArray" => |_: &Answers| Some(false),
    SupportsTileShaders = "supportsTileShaders" => |_: &Answers| Some(false),
});

/// The device's private properties.
pub struct PrivateProperties {
    counts: Vec<(PrivateCount, Option<u64>)>,
    flags: Vec<(PrivateFlag, Option<bool>)>,
}

impl PrivateProperties {
    pub(crate) fn of(device: &Device) -> PrivateProperties {
        let m = device.translator().metal_properties();
        let limits = &device.properties().description.limits;
        let a = Answers {
            max_total_threads: m.max_total_threads_per_threadgroup as u64,
            max_threadgroup_memory: m.max_threadgroup_memory_length as u64,
            max_texture_2d: m.max_texture_dimension_2d as u64,
            max_layers: limits.max_image_array_layers as u64,
        };
        PrivateProperties {
            counts: PrivateCount::ALL.iter().map(|&c| (c, c.answer(&a))).collect(),
            flags: PrivateFlag::ALL.iter().map(|&f| (f, f.answer(&a))).collect(),
        }
    }

    pub fn count(&self, c: PrivateCount) -> PrivateAnswer<u64> {
        PrivateAnswer(self.counts.iter().find(|(k, _)| *k == c).and_then(|(_, v)| *v))
    }

    pub fn flag(&self, f: PrivateFlag) -> PrivateAnswer<bool> {
        PrivateAnswer(self.flags.iter().find(|(k, _)| *k == f).and_then(|(_, v)| *v))
    }
}

/// What a pixel format can do on the device (Metal's format-table columns).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct FormatCapabilities(pub u32);

impl FormatCapabilities {
    pub const NONE: FormatCapabilities = FormatCapabilities(0);
    pub const SAMPLE: u32 = 1;
    pub const FILTER: u32 = 2;
    pub const COLOR: u32 = 4;
    pub const BLEND: u32 = 8;
    pub const WRITE: u32 = 16;
    pub const MSAA: u32 = 32;

    pub(crate) fn of(device: &Device, format: PixelFormat) -> FormatCapabilities {
        let Some(vk) = metal2vulkan::metal::format::direct_format(format) else {
            return FormatCapabilities::NONE;
        };
        let vk = vk.as_raw() as u32;
        let sg = device.gpu().sg;
        // SAFETY: plain queries on a valid device.
        let has = |usage: u32, samples: u32| unsafe { ffi::sg_format_supported(sg, vk, usage, samples) };
        let mut c = 0;
        if has(ffi::SG_USAGE_SAMPLE, 0) {
            c |= Self::SAMPLE | Self::FILTER;
        }
        if has(ffi::SG_USAGE_RENDER, 0) {
            c |= Self::COLOR;
        }
        if has(ffi::SG_USAGE_BLEND, 0) {
            c |= Self::BLEND;
        }
        if has(ffi::SG_USAGE_STORAGE, 0) {
            c |= Self::WRITE;
        }
        if has(ffi::SG_USAGE_RENDER, 4) {
            c |= Self::MSAA;
        }
        FormatCapabilities(c)
    }
}

/// A pixel format's block layout (the frontend's view of Metal's private format table).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PixelFormatInfo {
    /// 0x400: a block (compressed) format.
    pub flags: u64,
    /// [bytes per block, block width, block height, 0]
    pub words: [u64; 4],
}

impl PixelFormatInfo {
    pub(crate) fn of(raw: u64) -> PixelFormatInfo {
        let Some(pf) = u32::try_from(raw).ok().and_then(|r| PixelFormat::try_from(r).ok()) else {
            return PixelFormatInfo::default();
        };
        let Some(vk) = metal2vulkan::metal::format::direct_format(pf) else {
            return PixelFormatInfo::default();
        };
        let vk = vk.as_raw() as u32;
        // SAFETY: plain value queries.
        let (bytes, w, h) = unsafe {
            (ffi::sg_format_block_bytes(vk), ffi::sg_format_block_width(vk), ffi::sg_format_block_height(vk))
        };
        PixelFormatInfo {
            flags: if w > 1 || h > 1 { 0x400 } else { 0 },
            words: [bytes as u64, w as u64, h as u64, 0],
        }
    }
}
