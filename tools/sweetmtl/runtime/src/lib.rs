// SPDX-License-Identifier: MIT
//! sweetmtl-runtime: the Metal object model on Mesa's Gallium freedreno driver for the Adreno 540
//! (taimen), for flame's native Metal bridge (tools/sweetmtl/bridge).
//!
//! It stands where flame's bridge had `metal2vulkan-runtime` (the Metal API on Vulkan), with the
//! subset of that API the bridge calls, re-implemented here (docs/taimen-metal-plan.md section 2):
//! the metal2vulkan translator makes SPIR-V, layouts and binding plans; the C shim in
//! tools/sweetmtl/gallium turns them into Gallium CSOs, bindings and submissions. What is not
//! built yet fails with `Error::NotYet` naming it (tools/sweetmtl/README.md lists the coverage).
//!
//! Linking: this crate declares the shim's symbols and links nothing itself. The final link (the
//! bridge's static library into a test tool or the Metal bundle) adds libsweetgallium.a and
//! Mesa's static libraries (tools/sweetmtl/host-check.sh, plan section 4.4).

pub mod bind;
pub mod command;
pub mod device;
pub mod encoder;
pub mod error;
pub mod families;
pub mod ffi;
pub mod library;
pub mod pipeline;
pub mod resource;
pub mod sync;

pub use command::{CommandBuffer, CommandBufferStatus, CommandQueue, Handler, UsedResources};
pub use device::{
    DebugMessage, Device, DeviceOptions, DeviceProperties, MessageSeverity, MessageSink, MetalLevel, ShaderCacheOptions,
    ShaderCacheStatistics, Size, Tier,
};
pub use encoder::{
    BlitCommandEncoder, BlitOption, ClearColor, ComputeCommandEncoder, LoadAction, RenderCommandEncoder,
    RenderPassColorAttachment, RenderPassDepthAttachment, RenderPassDescriptor, RenderPassStencilAttachment,
    ScissorRect, StoreAction, Viewport, VisibilityResultMode, MAX_COLOR_ATTACHMENTS,
};
pub use error::{Error, RuntimeGap};
pub use families::{
    FamilyRow, FeatureRow, FormatCapabilities, FormatCapability, GpuFamily, PixelFormatInfo, PrivateAnswer,
    PrivateCount, PrivateFlag, PrivateProperties, RowAudit, RowState,
};
pub use library::{Function, Library};
pub use pipeline::{
    BinaryArchive, BinaryArchiveDescriptor, ComputePipelineState, DepthStencilState, PipelineOption, PipelineOptions,
    RenderPipelineState, SamplerState,
};
pub use resource::{
    Buffer, GpuAddress, Heap, HeapDescriptor, HeapType, HostMemory, IoSurfacePlane, Origin, PurgeableState, Region,
    ResourceOptions, ResourceRef, StorageMode, Texture, TextureDescriptor,
};
pub use sync::{Event, EventRef, Fence, SharedEvent, SharedEventListener};
