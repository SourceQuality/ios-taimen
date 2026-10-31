// SPDX-License-Identifier: MIT
//! Command encoders (`MTLComputeCommandEncoder`, `MTLRenderCommandEncoder`,
//! `MTLBlitCommandEncoder`) and the render pass descriptor.
//!
//! An encoder keeps Metal's argument tables and state; each dispatch, draw or blit becomes one
//! recorded command, with its Gallium bindings resolved through the translator's binder
//! (bind.rs) and its shaders compiled for the plan's specialization. Fence waits run before the
//! encoder's commands and fence updates after them (an encoder's work never interleaves with
//! another queue's).

use crate::bind::{push_block, resolve, Attachments, Slot, StageRef, Table};
use crate::command::{Cmd, CommandBuffer, DispatchRegion, DrawCmd, PassCmd, ResolveCmd};
use crate::device::{Device, Size};
use crate::error::{invalid, not_yet, Error};
use crate::ffi;
use crate::pipeline::{spec_merge, spec_of, ComputePipelineState, DepthStencilState, RenderPipelineState, SamplerState};
use crate::resource::{sg_box, vk_format, Buffer, Origin, SgTexture, Texture};
use crate::sync::Fence;
use metal2vulkan::bind::{BindState, Dispatch, DrawCall, IndexBufferRef};
use metal2vulkan::metal::{CullMode, IndexType, PrimitiveType, RenderStages, TextureType, TriangleFillMode, Winding};
use metal2vulkan::pipeline::Stage;
use metal2vulkan::vk;
use std::ops::Range;
use std::sync::Arc;

pub const MAX_COLOR_ATTACHMENTS: usize = 8;

// ---- pass descriptor ----

/// `MTLLoadAction`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum LoadAction {
    #[default]
    DontCare,
    Load,
    Clear,
}

impl LoadAction {
    pub fn from_raw(raw: u64) -> Result<LoadAction, Error> {
        match raw {
            0 => Ok(LoadAction::DontCare),
            1 => Ok(LoadAction::Load),
            2 => Ok(LoadAction::Clear),
            _ => Err(invalid(format!("MTLLoadAction {raw}"))),
        }
    }
    fn raw(self) -> u32 {
        self as u32
    }
}

/// `MTLStoreAction`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum StoreAction {
    #[default]
    DontCare,
    Store,
    MultisampleResolve,
    StoreAndMultisampleResolve,
    Unknown,
    CustomSampleDepthStore,
}

impl StoreAction {
    pub fn from_raw(raw: u64) -> Result<StoreAction, Error> {
        Ok(match raw {
            0 => StoreAction::DontCare,
            1 => StoreAction::Store,
            2 => StoreAction::MultisampleResolve,
            3 => StoreAction::StoreAndMultisampleResolve,
            4 => StoreAction::Unknown,
            5 => StoreAction::CustomSampleDepthStore,
            _ => return Err(invalid(format!("MTLStoreAction {raw}"))),
        })
    }

    /// Whether the action resolves the attachment into its resolve texture.
    pub fn resolves(self) -> bool {
        matches!(self, StoreAction::MultisampleResolve | StoreAction::StoreAndMultisampleResolve)
    }
}

/// `MTLMultisampleDepthResolveFilter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum MultisampleDepthResolveFilter {
    #[default]
    Sample0,
    Min,
    Max,
}

impl MultisampleDepthResolveFilter {
    pub fn from_raw(raw: u64) -> Result<MultisampleDepthResolveFilter, Error> {
        match raw {
            0 => Ok(MultisampleDepthResolveFilter::Sample0),
            1 => Ok(MultisampleDepthResolveFilter::Min),
            2 => Ok(MultisampleDepthResolveFilter::Max),
            _ => Err(invalid(format!("MTLMultisampleDepthResolveFilter {raw}"))),
        }
    }
}

/// `MTLMultisampleStencilResolveFilter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum MultisampleStencilResolveFilter {
    #[default]
    Sample0,
    DepthResolvedSample,
}

impl MultisampleStencilResolveFilter {
    pub fn from_raw(raw: u64) -> Result<MultisampleStencilResolveFilter, Error> {
        match raw {
            0 => Ok(MultisampleStencilResolveFilter::Sample0),
            1 => Ok(MultisampleStencilResolveFilter::DepthResolvedSample),
            _ => Err(invalid(format!("MTLMultisampleStencilResolveFilter {raw}"))),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct ClearColor {
    pub red: f64,
    pub green: f64,
    pub blue: f64,
    pub alpha: f64,
}

impl ClearColor {
    pub fn new(red: f64, green: f64, blue: f64, alpha: f64) -> ClearColor {
        ClearColor { red, green, blue, alpha }
    }
}

#[derive(Clone, Default)]
pub struct RenderPassColorAttachment {
    pub texture: Option<Texture>,
    pub level: u64,
    pub slice: u64,
    pub depth_plane: u64,
    pub load_action: LoadAction,
    pub store_action: StoreAction,
    pub clear_color: ClearColor,
    /// Where a resolve store action resolves the attachment: a single-sampled texture's level and
    /// slice (a 3D texture's depth plane).
    pub resolve_texture: Option<Texture>,
    pub resolve_level: u64,
    pub resolve_slice: u64,
    pub resolve_depth_plane: u64,
}

#[derive(Clone, Default)]
pub struct RenderPassDepthAttachment {
    pub texture: Option<Texture>,
    pub level: u64,
    pub slice: u64,
    pub depth_plane: u64,
    pub load_action: LoadAction,
    pub store_action: StoreAction,
    pub clear_depth: f64,
    pub resolve_texture: Option<Texture>,
    pub resolve_level: u64,
    pub resolve_slice: u64,
    pub resolve_depth_plane: u64,
    pub resolve_filter: MultisampleDepthResolveFilter,
}

#[derive(Clone, Default)]
pub struct RenderPassStencilAttachment {
    pub texture: Option<Texture>,
    pub level: u64,
    pub slice: u64,
    pub depth_plane: u64,
    pub load_action: LoadAction,
    pub store_action: StoreAction,
    pub clear_stencil: u32,
    pub resolve_texture: Option<Texture>,
    pub resolve_level: u64,
    pub resolve_slice: u64,
    pub resolve_depth_plane: u64,
    pub resolve_filter: MultisampleStencilResolveFilter,
}

/// `MTLRenderPassDescriptor`, a plain value.
#[derive(Clone, Default)]
pub struct RenderPassDescriptor {
    pub color_attachments: [RenderPassColorAttachment; MAX_COLOR_ATTACHMENTS],
    pub depth_attachment: RenderPassDepthAttachment,
    pub stencil_attachment: RenderPassStencilAttachment,
    pub render_target_width: u64,
    pub render_target_height: u64,
    pub render_target_array_length: u64,
    pub default_raster_sample_count: u64,
    pub visibility_result_buffer: Option<Buffer>,
}

impl RenderPassDescriptor {
    /// Metal's fresh descriptor: color attachments clear-free, depth clears to 1.
    pub fn new() -> RenderPassDescriptor {
        let mut d = RenderPassDescriptor::default();
        d.depth_attachment.clear_depth = 1.0;
        d
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Viewport {
    pub origin_x: f64,
    pub origin_y: f64,
    pub width: f64,
    pub height: f64,
    pub znear: f64,
    pub zfar: f64,
}

impl Viewport {
    pub fn new(origin_x: f64, origin_y: f64, width: f64, height: f64, znear: f64, zfar: f64) -> Viewport {
        Viewport { origin_x, origin_y, width, height, znear, zfar }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScissorRect {
    pub x: u64,
    pub y: u64,
    pub width: u64,
    pub height: u64,
}

impl ScissorRect {
    pub fn new(x: u64, y: u64, width: u64, height: u64) -> ScissorRect {
        ScissorRect { x, y, width, height }
    }
}

/// `MTLVisibilityResultMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VisibilityResultMode {
    Disabled,
    Boolean,
    Counting,
}

impl VisibilityResultMode {
    pub fn from_raw(raw: u64) -> Result<VisibilityResultMode, Error> {
        match raw {
            0 => Ok(VisibilityResultMode::Disabled),
            1 => Ok(VisibilityResultMode::Boolean),
            2 => Ok(VisibilityResultMode::Counting),
            _ => Err(invalid(format!("MTLVisibilityResultMode {raw}"))),
        }
    }
}

/// `MTLBlitOption`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlitOption {
    None,
    DepthFromDepthStencil,
    StencilFromDepthStencil,
    RowLinearPvrtc,
}

fn u32_of(v: u64, what: &str) -> Result<u32, Error> {
    u32::try_from(v).map_err(|_| invalid(format!("{what} {v}")))
}

fn size3(s: Size, what: &str) -> Result<[u32; 3], Error> {
    Ok([u32_of(s.width, what)?, u32_of(s.height, what)?, u32_of(s.depth, what)?])
}

/// The no-copy buffers a table's buffers and textures live in.
fn no_copy_of(table: &Table, out: &mut Vec<Buffer>) {
    for slot in table.buffers.values() {
        if let Slot::Buffer { buffer, .. } = slot {
            if buffer.0.host.is_some() && !out.contains(buffer) {
                out.push(buffer.clone());
            }
        }
    }
    for t in table.textures.values() {
        if let Some(b) = &t.0.buffer {
            if b.0.host.is_some() && !out.contains(b) {
                out.push(b.clone());
            }
        }
    }
}

fn no_copy_texture(t: &Texture, out: &mut Vec<Buffer>) {
    if let Some(b) = &t.0.buffer {
        if b.0.host.is_some() && !out.contains(b) {
            out.push(b.clone());
        }
    }
}

fn no_copy_buffer(b: &Buffer, out: &mut Vec<Buffer>) {
    if b.0.host.is_some() && !out.contains(b) {
        out.push(b.clone());
    }
}

/// What every encoder keeps: its command buffer and what it recorded.
struct Core {
    cb: CommandBuffer,
    pre: Vec<Cmd>,
    cmds: Vec<Cmd>,
    post: Vec<Cmd>,
    used: Vec<u64>,
    no_copy: Vec<Buffer>,
    ended: bool,
}

impl Core {
    fn new(cb: &CommandBuffer) -> Result<Core, Error> {
        cb.open_encoder()?;
        Ok(Core { cb: cb.clone(), pre: Vec::new(), cmds: Vec::new(), post: Vec::new(), used: Vec::new(), no_copy: Vec::new(),
                  ended: false })
    }

    fn device(&self) -> Device {
        self.cb.device()
    }

    fn update_fence(&mut self, fence: &Fence) {
        self.post.push(Cmd::UpdateFence(fence.0.clone(), 0));
    }

    fn wait_for_fence(&mut self, fence: &Fence) {
        self.pre.push(Cmd::WaitFence(fence.0.clone(), 0));
    }

    fn end(&mut self) {
        let mut all = std::mem::take(&mut self.pre);
        all.append(&mut self.cmds);
        all.append(&mut self.post);
        self.cb.close_encoder(all, &self.used, &self.no_copy);
        self.ended = true;
    }

    fn set_bytes(&mut self, table: &mut Table, bytes: &[u8], index: u32) -> Result<(), Error> {
        if index >= crate::bind::BUFFER_SLOTS {
            return Err(invalid(format!("buffer index {index}")));
        }
        if bytes.len() > 4096 {
            return Err(invalid("set*Bytes of more than 4 KiB"));
        }
        let (buffer, offset) = self.cb.upload(bytes)?;
        table.buffers.insert(index, Slot::Bytes { buffer, offset, len: bytes.len() as u32 });
        Ok(())
    }
}

impl Drop for Core {
    fn drop(&mut self) {
        if !self.ended {
            self.cb.discard_encoder();
        }
    }
}

// ---- compute ----

/// `MTLComputeCommandEncoder`.
pub struct ComputeCommandEncoder {
    core: Core,
    pipeline: Option<ComputePipelineState>,
    table: Table,
}

impl CommandBuffer {
    pub fn compute_command_encoder(&self) -> Result<ComputeCommandEncoder, Error> {
        Ok(ComputeCommandEncoder { core: Core::new(self)?, pipeline: None, table: Table::default() })
    }

    pub fn blit_command_encoder(&self) -> Result<BlitCommandEncoder, Error> {
        Ok(BlitCommandEncoder { core: Core::new(self)? })
    }
}

impl ComputeCommandEncoder {
    pub fn set_compute_pipeline_state(&mut self, state: &ComputePipelineState) -> Result<(), Error> {
        if state.device() != self.core.device() {
            return Err(invalid("a pipeline of another device"));
        }
        self.pipeline = Some(state.clone());
        Ok(())
    }
    pub fn set_buffer(&mut self, buffer: Option<&Buffer>, offset: u64, index: u32) -> Result<(), Error> {
        self.table.set_buffer(buffer, offset, index)
    }
    pub fn set_buffer_offset(&mut self, offset: u64, index: u32) -> Result<(), Error> {
        self.table.set_buffer_offset(offset, index)
    }
    pub fn set_bytes(&mut self, bytes: &[u8], index: u32) -> Result<(), Error> {
        self.core.set_bytes(&mut self.table, bytes, index)
    }
    pub fn set_texture(&mut self, texture: Option<&Texture>, index: u32) -> Result<(), Error> {
        self.table.set_texture(texture, index)
    }
    pub fn set_sampler_state(&mut self, sampler: Option<&SamplerState>, index: u32) -> Result<(), Error> {
        self.table.set_sampler(sampler, index)
    }
    pub fn set_threadgroup_memory_length(&mut self, length: u32, index: u32) -> Result<(), Error> {
        if index >= 31 {
            return Err(invalid(format!("threadgroup memory index {index}")));
        }
        self.table.threadgroup.insert(index, length.div_ceil(16) * 16);
        Ok(())
    }
    pub fn update_fence(&mut self, fence: &Fence) -> Result<(), Error> {
        self.core.update_fence(fence);
        Ok(())
    }
    pub fn wait_for_fence(&mut self, fence: &Fence) -> Result<(), Error> {
        self.core.wait_for_fence(fence);
        Ok(())
    }

    /// `dispatchThreads:threadsPerThreadgroup:`.
    pub fn dispatch_threads(&mut self, threads: Size, per_group: Size) -> Result<(), Error> {
        let d = Dispatch::Threads { threads: size3(threads, "grid")?, threads_per_group: size3(per_group, "threadgroup")? };
        self.dispatch(d)
    }

    /// `dispatchThreadgroups:threadsPerThreadgroup:`.
    pub fn dispatch_threadgroups(&mut self, groups: Size, per_group: Size) -> Result<(), Error> {
        let d = Dispatch::Threadgroups { groups: size3(groups, "grid")?, threads_per_group: size3(per_group, "threadgroup")? };
        self.dispatch(d)
    }

    fn dispatch(&mut self, dispatch: Dispatch) -> Result<(), Error> {
        let p = self.pipeline.clone().ok_or_else(|| invalid("a dispatch without a compute pipeline state"))?;
        let device = self.core.device();
        let mut state = BindState::default();
        state.stages.insert(Stage::Kernel, self.table.facts());
        // the pipeline's compile this binding runs: the base, or a variant compiled for it
        let (v, plan) = p.settle(&state.shape(), &dispatch)?;
        let c = plan.instantiate_dispatch(&state, &dispatch)?;
        if !c.descriptor_bytes.is_empty() || c.root_table.is_some() {
            return Err(not_yet("argument buffers and handle tables (descriptor bytes)"));
        }
        if !c.push_holes.is_empty() {
            return Err(not_yet(format!("push holes {:?} (device addresses the shader reads)", c.push_holes)));
        }
        let mut used = Vec::new();
        let stages = [StageRef { program: &v.program, metal: Stage::Kernel, table: &self.table }];
        let binds = resolve(&device, &stages, &plan, &c.descriptor_ranges, None, &mut used)?;
        let base = spec_of(&plan.specialization);
        let mut regions = Vec::new();
        for r in &c.regions {
            if r.indirect.is_some() {
                return Err(not_yet("indirect dispatches"));
            }
            let shader = v.program.shader(&device, &spec_merge(&base, &spec_of(&r.specialization)))?;
            regions.push(DispatchRegion { shader, push: push_block(&r.push_constants), groups: r.group_count });
        }
        self.table.used(&mut self.core.used);
        self.core.used.extend(used);
        no_copy_of(&self.table, &mut self.core.no_copy);
        self.core.cmds.push(Cmd::Dispatch { binds, regions, shared: 0 });
        Ok(())
    }

    pub fn end_encoding(mut self) -> Result<(), Error> {
        self.core.end();
        Ok(())
    }
}

// ---- render ----

/// `MTLRenderCommandEncoder`.
pub struct RenderCommandEncoder {
    core: Core,
    width: u32,
    height: u32,
    layers: u32,
    samples: u32,
    attachments: Attachments,
    has_depth: bool,
    has_stencil: bool,
    /// The store actions' resolves, recorded after the pass ends.
    resolves: Vec<ResolveCmd>,
    pipeline: Option<RenderPipelineState>,
    depth_stencil: Option<DepthStencilState>,
    vertex: Table,
    fragment: Table,
    viewport: [f64; 6],
    scissor: [u32; 4],
    cull: CullMode,
    winding: Winding,
    fill: TriangleFillMode,
    stencil_ref: [u32; 2],
    blend_color: [f32; 4],
    depth_bias: [f32; 3],
    /// setDepthClipMode: MTLDepthClipModeClamp (Gallium depth_clamp) instead of Metal's default clip
    depth_clamp: bool,
    /// the pass descriptor's visibilityResultBuffer
    visibility: Option<Buffer>,
}

fn attachment(texture: &Texture, level: u64, slice: u64, depth_plane: u64, load: LoadAction, clear: [f64; 4],
              keep: &mut Vec<Arc<SgTexture>>) -> Result<(ffi::sg_attachment, u32, u32, u32, u32), Error> {
    // A texture without MTLTextureUsageRenderTarget is still an attachment: only Metal's validation layer refuses it,
    // and Maps renders into such textures (boot 211: its Look Around thumbnail stayed black while 12 command buffers
    // were refused). The shim makes every uncompressed texture a render target or depth/stencil one (sg_resource.c,
    // for blits), and a format that cannot render is refused where its surface is made.
    let layer = if texture.texture_type() == TextureType::Type3D { depth_plane } else { slice };
    let (l, s) = texture.image_level_slice(level, if texture.texture_type() == TextureType::Type3D { 0 } else { layer })?;
    let s = if texture.texture_type() == TextureType::Type3D { u32_of(depth_plane, "depth plane")? } else { s };
    keep.push(texture.0.image.clone());
    let w = (texture.0.desc.width >> l).max(1) as u32;
    let h = (texture.0.desc.height >> l).max(1) as u32;
    Ok((
        ffi::sg_attachment {
            tex: texture.0.image.sg,
            vk_format: vk_format(texture.pixel_format())?,
            level: l,
            first_layer: s,
            last_layer: s,
            load: load.raw(),
            clear,
        },
        w,
        h,
        l,
        s,
    ))
}

/// Where a resolve store action sends an attachment.
struct ResolveTarget<'a> {
    store: StoreAction,
    texture: Option<&'a Texture>,
    level: u64,
    slice: u64,
    depth_plane: u64,
}

/// An attachment's resolve, checked: its texture's level and layer `(l, s)`, `w` x `h`, into the
/// resolve texture's level and slice (a 3D texture's depth plane), on the GPU after the pass
/// (sg_ctx_resolve). None where the store action does not resolve (Metal ignores the resolve
/// texture then).
#[allow(clippy::too_many_arguments)]
fn resolve_of(what: impl Fn() -> String, texture: &Texture, l: u32, s: u32, w: u32, h: u32, target: ResolveTarget,
              aspects: u32, used: &mut Vec<u64>, no_copy: &mut Vec<Buffer>) -> Result<Option<ResolveCmd>, Error> {
    if target.store == StoreAction::Unknown && target.texture.is_some() {
        return Err(not_yet(format!(
            "{}: MTLStoreActionUnknown with a resolve texture (store actions set on the encoder)",
            what()
        )));
    }
    if !target.store.resolves() {
        return Ok(None);
    }
    let r = target
        .texture
        .ok_or_else(|| invalid(format!("{}: store action {:?} without a resolve texture", what(), target.store)))?;
    if r.sample_count() > 1 {
        return Err(invalid(format!("{}: a multisampled resolve texture", what())));
    }
    if Arc::ptr_eq(&r.0.image, &texture.0.image) {
        return Err(invalid(format!("{}: the resolve texture is the attachment's own", what())));
    }
    let three_d = r.texture_type() == TextureType::Type3D;
    let (rl, rs) = r.image_level_slice(target.level, if three_d { 0 } else { target.slice })?;
    let rs = if three_d {
        if target.depth_plane >= (r.0.desc.depth >> rl).max(1) {
            return Err(invalid(format!("{}: resolve depth plane {} past the level", what(), target.depth_plane)));
        }
        u32_of(target.depth_plane, "resolve depth plane")?
    } else {
        rs
    };
    let rw = (r.0.desc.width >> rl).max(1) as u32;
    let rh = (r.0.desc.height >> rl).max(1) as u32;
    used.extend(r.ids());
    no_copy_texture(r, no_copy);
    Ok(Some(ResolveCmd {
        src: texture.0.image.clone(),
        src_format: vk_format(texture.pixel_format())?,
        src_level: l,
        src_layer: s,
        dst: r.0.image.clone(),
        dst_format: vk_format(r.pixel_format())?,
        dst_level: rl,
        dst_layer: rs,
        width: w.min(rw),
        height: h.min(rh),
        aspects,
    }))
}

impl CommandBuffer {
    /// `renderCommandEncoderWithDescriptor:`: begins the pass (its load actions). The resolve
    /// store actions (MultisampleResolve, StoreAndMultisampleResolve) run after the pass ends
    /// (sg_ctx_resolve: the GPU's resolve, averaging color; depth and stencil resolve with Metal's
    /// Sample0 filters, other filters are not yet). The multisampled texture keeps its samples
    /// either way (MultisampleResolve leaves them undefined).
    pub fn render_command_encoder(&self, pass: &RenderPassDescriptor) -> Result<RenderCommandEncoder, Error> {
        let mut keep = Vec::new();
        let mut sg = ffi::sg_pass::default();
        let mut size: Option<(u32, u32)> = None;
        let mut attachments: Attachments = Default::default();
        let mut used = Vec::new();
        let mut no_copy = Vec::new();
        let mut resolves = Vec::new();
        let mut samples = pass.default_raster_sample_count.max(1) as u32;
        for (i, a) in pass.color_attachments.iter().enumerate() {
            let Some(t) = &a.texture else { continue };
            let c = a.clear_color;
            let (att, w, h, l, s) =
                attachment(t, a.level, a.slice, a.depth_plane, a.load_action, [c.red, c.green, c.blue, c.alpha], &mut keep)?;
            let target = ResolveTarget {
                store: a.store_action,
                texture: a.resolve_texture.as_ref(),
                level: a.resolve_level,
                slice: a.resolve_slice,
                depth_plane: a.resolve_depth_plane,
            };
            resolves.extend(resolve_of(|| format!("color attachment {i}"), t, l, s, w, h, target, ffi::SG_ASPECT_COLOR,
                                       &mut used, &mut no_copy)?);
            sg.color[i] = att;
            size.get_or_insert((w, h));
            attachments[i] = Some((t.clone(), l, s));
            samples = samples.max(t.sample_count() as u32);
            used.extend(t.ids());
            no_copy_texture(t, &mut no_copy);
        }
        let mut has_depth = false;
        let mut has_stencil = false;
        let depth_filter = pass.depth_attachment.resolve_filter;
        if let Some(t) = &pass.depth_attachment.texture {
            let a = &pass.depth_attachment;
            let (att, w, h, l, s) =
                attachment(t, a.level, a.slice, a.depth_plane, a.load_action, [a.clear_depth, 0.0, 0.0, 0.0], &mut keep)?;
            if a.store_action.resolves() && depth_filter != MultisampleDepthResolveFilter::Sample0 {
                return Err(not_yet(format!("depth resolve filter {depth_filter:?} (the GPU's resolve takes sample 0)")));
            }
            let target = ResolveTarget {
                store: a.store_action,
                texture: a.resolve_texture.as_ref(),
                level: a.resolve_level,
                slice: a.resolve_slice,
                depth_plane: a.resolve_depth_plane,
            };
            resolves.extend(resolve_of(|| "depth attachment".into(), t, l, s, w, h, target, ffi::SG_ASPECT_DEPTH, &mut used,
                                       &mut no_copy)?);
            sg.depth = att;
            size.get_or_insert((w, h));
            samples = samples.max(t.sample_count() as u32);
            has_depth = true;
            used.extend(t.ids());
        }
        if let Some(t) = &pass.stencil_attachment.texture {
            let a = &pass.stencil_attachment;
            let (att, w, h, l, s) = attachment(t, a.level, a.slice, a.depth_plane, a.load_action,
                                               [a.clear_stencil as f64, 0.0, 0.0, 0.0], &mut keep)?;
            // DepthResolvedSample is the sample the depth filter picks: sample 0 under Sample0
            if a.store_action.resolves() && a.resolve_filter == MultisampleStencilResolveFilter::DepthResolvedSample
                && depth_filter != MultisampleDepthResolveFilter::Sample0
            {
                return Err(not_yet(format!("stencil resolve of the depth filter's sample ({depth_filter:?})")));
            }
            let target = ResolveTarget {
                store: a.store_action,
                texture: a.resolve_texture.as_ref(),
                level: a.resolve_level,
                slice: a.resolve_slice,
                depth_plane: a.resolve_depth_plane,
            };
            resolves.extend(resolve_of(|| "stencil attachment".into(), t, l, s, w, h, target, ffi::SG_ASPECT_STENCIL,
                                       &mut used, &mut no_copy)?);
            sg.stencil = att;
            size.get_or_insert((w, h));
            samples = samples.max(t.sample_count() as u32);
            has_stencil = true;
            used.extend(t.ids());
        }
        let (aw, ah) = size.unwrap_or((0, 0));
        let width = if pass.render_target_width != 0 { u32_of(pass.render_target_width, "render target width")? } else { aw };
        let height = if pass.render_target_height != 0 { u32_of(pass.render_target_height, "render target height")? } else { ah };
        if width == 0 || height == 0 {
            return Err(invalid("a render pass of no size"));
        }
        if size.is_none() {
            return Err(not_yet("render passes without attachments"));
        }
        // SWEETMTL_TRACE: which passes run, to tell the one a GPU stall came in (boots 226, 233: Look Around opening)
        if crate::device::trace_enabled() {
            let colors: Vec<String> = pass.color_attachments.iter().enumerate().filter_map(|(i, a)| {
                a.texture.as_ref().map(|t| {
                    let resolve = a.resolve_texture.as_ref()
                        .map_or(String::new(), |r| format!(" -> {}x{}", r.0.desc.width, r.0.desc.height));
                    format!("color {i} {:?} {}x{} x{} {:?}/{:?}{resolve}", t.pixel_format(), t.0.desc.width,
                            t.0.desc.height, t.sample_count(), a.load_action, a.store_action)
                })
            }).collect();
            crate::device::trace(format_args!("render pass {width}x{height}, {samples} samples: {}{}{}", colors.join(", "),
                                              if has_depth { ", depth" } else { "" }, if has_stencil { ", stencil" } else { "" }));
        }
        let layers = pass.render_target_array_length.max(1) as u32;
        sg.width = width;
        sg.height = height;
        sg.layers = layers;
        sg.samples = samples;
        let mut core = Core::new(self)?;
        core.cmds.push(Cmd::BeginPass(Box::new(PassCmd { pass: sg, textures: keep })));
        core.used = used;
        core.no_copy = no_copy;
        Ok(RenderCommandEncoder {
            core,
            width,
            height,
            layers,
            samples,
            attachments,
            has_depth,
            has_stencil,
            resolves,
            pipeline: None,
            depth_stencil: None,
            vertex: Table::default(),
            fragment: Table::default(),
            viewport: [0.0, 0.0, width as f64, height as f64, 0.0, 1.0],
            scissor: [0, 0, width, height],
            cull: CullMode::None,
            winding: Winding::Clockwise,
            fill: TriangleFillMode::Fill,
            stencil_ref: [0, 0],
            blend_color: [0.0; 4],
            depth_bias: [0.0; 3],
            depth_clamp: false,
            visibility: pass.visibility_result_buffer.clone(),
        })
    }
}

impl RenderCommandEncoder {
    pub fn set_render_pipeline_state(&mut self, state: &RenderPipelineState) -> Result<(), Error> {
        if state.device() != self.core.device() {
            return Err(invalid("a pipeline of another device"));
        }
        self.pipeline = Some(state.clone());
        Ok(())
    }
    pub fn set_depth_stencil_state(&mut self, state: Option<&DepthStencilState>) -> Result<(), Error> {
        self.depth_stencil = state.cloned();
        Ok(())
    }
    pub fn set_vertex_buffer(&mut self, buffer: Option<&Buffer>, offset: u64, index: u32) -> Result<(), Error> {
        self.vertex.set_buffer(buffer, offset, index)
    }
    pub fn set_fragment_buffer(&mut self, buffer: Option<&Buffer>, offset: u64, index: u32) -> Result<(), Error> {
        self.fragment.set_buffer(buffer, offset, index)
    }
    pub fn set_vertex_buffer_offset(&mut self, offset: u64, index: u32) -> Result<(), Error> {
        self.vertex.set_buffer_offset(offset, index)
    }
    pub fn set_fragment_buffer_offset(&mut self, offset: u64, index: u32) -> Result<(), Error> {
        self.fragment.set_buffer_offset(offset, index)
    }
    pub fn set_vertex_bytes(&mut self, bytes: &[u8], index: u32) -> Result<(), Error> {
        self.core.set_bytes(&mut self.vertex, bytes, index)
    }
    pub fn set_fragment_bytes(&mut self, bytes: &[u8], index: u32) -> Result<(), Error> {
        self.core.set_bytes(&mut self.fragment, bytes, index)
    }
    pub fn set_vertex_texture(&mut self, texture: Option<&Texture>, index: u32) -> Result<(), Error> {
        self.vertex.set_texture(texture, index)
    }
    pub fn set_fragment_texture(&mut self, texture: Option<&Texture>, index: u32) -> Result<(), Error> {
        self.fragment.set_texture(texture, index)
    }
    pub fn set_vertex_sampler_state(&mut self, sampler: Option<&SamplerState>, index: u32) -> Result<(), Error> {
        self.vertex.set_sampler(sampler, index)
    }
    pub fn set_fragment_sampler_state(&mut self, sampler: Option<&SamplerState>, index: u32) -> Result<(), Error> {
        self.fragment.set_sampler(sampler, index)
    }

    /// `setViewport:`. Refused: a non-finite field, and depths outside [0, 1] (no depth clip
    /// control on a5xx to take them).
    pub fn set_viewport(&mut self, v: Viewport) -> Result<(), Error> {
        let f = [v.origin_x, v.origin_y, v.width, v.height, v.znear, v.zfar];
        if f.iter().any(|x| !x.is_finite()) {
            return Err(invalid("a viewport with a non-finite field"));
        }
        if !(0.0..=1.0).contains(&v.znear) || !(0.0..=1.0).contains(&v.zfar) {
            return Err(not_yet("viewport depths outside [0, 1]"));
        }
        if v.width < 0.0 {
            return Err(not_yet("a viewport of negative width"));
        }
        self.viewport = f;
        Ok(())
    }

    /// `setScissorRect:`, clipped to the render area as Metal clips it.
    pub fn set_scissor_rect(&mut self, r: ScissorRect) -> Result<(), Error> {
        let x = r.x.min(self.width as u64) as u32;
        let y = r.y.min(self.height as u64) as u32;
        let w = (r.width.min(self.width as u64 - x as u64)) as u32;
        let h = (r.height.min(self.height as u64 - y as u64)) as u32;
        self.scissor = [x, y, w, h];
        Ok(())
    }
    pub fn set_cull_mode(&mut self, mode: CullMode) {
        self.cull = mode;
    }
    pub fn set_front_facing_winding(&mut self, winding: Winding) {
        self.winding = winding;
    }
    pub fn set_triangle_fill_mode(&mut self, mode: TriangleFillMode) -> Result<(), Error> {
        self.fill = mode;
        Ok(())
    }
    /// Metal's setDepthClipMode: false is MTLDepthClipModeClip (the default), true MTLDepthClipModeClamp.
    pub fn set_depth_clamp(&mut self, clamp: bool) {
        self.depth_clamp = clamp;
    }
    pub fn set_stencil_reference_values(&mut self, front: u32, back: u32) {
        self.stencil_ref = [front, back];
    }
    pub fn set_depth_bias(&mut self, bias: f32, slope_scale: f32, clamp: f32) -> Result<(), Error> {
        if !bias.is_finite() || !slope_scale.is_finite() || clamp.is_nan() {
            return Err(invalid("a non-finite depth bias"));
        }
        self.depth_bias = [bias, slope_scale, clamp];
        Ok(())
    }
    /// SPI `setLineWidth:`: 1 only (Metal's default; wider lines are not measured).
    pub fn set_line_width(&mut self, width: f32) -> Result<(), Error> {
        if width == 1.0 {
            Ok(())
        } else {
            Err(not_yet(format!("line width {width}")))
        }
    }
    /// Occlusion queries into the pass's visibility result buffer (the shim's sg_ctx_visibility): the
    /// words are written when the command buffer completes. VectorKit's Look Around uses them (boot 240:
    /// its command buffers were refused and its round overview map stayed black).
    pub fn set_visibility_result_mode(&mut self, mode: VisibilityResultMode, offset: u64) -> Result<(), Error> {
        let raw = match mode {
            VisibilityResultMode::Disabled => 0,
            VisibilityResultMode::Boolean => 1,
            VisibilityResultMode::Counting => 2,
        };
        let buffer = match (&self.visibility, raw) {
            (_, 0) => None,
            (None, _) => return Err(invalid("a visibility result mode without a visibility result buffer")),
            (Some(b), _) => {
                if offset % 8 != 0 || offset.checked_add(8).map_or(true, |end| end > b.length()) {
                    return Err(invalid(format!("visibility result offset {offset} (buffer of {})", b.length())));
                }
                if !self.core.used.contains(&b.0.id) {
                    self.core.used.push(b.0.id);
                    no_copy_buffer(b, &mut self.core.no_copy);
                }
                Some(b.0.clone())
            }
        };
        self.core.cmds.push(Cmd::Visibility { buffer, offset, mode: raw });
        Ok(())
    }
    pub fn set_blend_color(&mut self, red: f32, green: f32, blue: f32, alpha: f32) {
        self.blend_color = [red, green, blue, alpha];
    }
    pub fn update_fence(&mut self, fence: &Fence, _stages: RenderStages) -> Result<(), Error> {
        self.core.update_fence(fence);
        Ok(())
    }
    pub fn wait_for_fence(&mut self, fence: &Fence, _stages: RenderStages) -> Result<(), Error> {
        self.core.wait_for_fence(fence);
        Ok(())
    }

    /// `drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:`.
    pub fn draw_primitives(&mut self, primitive: PrimitiveType, start: u32, count: u32, instances: u32,
                           base_instance: u32) -> Result<(), Error> {
        let draw = DrawCall::Primitives { vertex_start: start, vertex_count: count, instance_count: instances, base_instance };
        self.draw(primitive, draw, None)
    }

    /// `drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:`.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_indexed_primitives(&mut self, primitive: PrimitiveType, count: u32, index_type: IndexType,
                                   indices: &Buffer, offset: u64, instances: u32, base_vertex: i32,
                                   base_instance: u32) -> Result<(), Error> {
        let vk_type = match index_type {
            IndexType::UInt16 => vk::IndexType::UINT16,
            _ => vk::IndexType::UINT32,
        };
        let draw = DrawCall::Indexed {
            index_type: vk_type,
            index_buffer: IndexBufferRef::new(1, indices.length()),
            index_count: count,
            index_offset: offset,
            instance_count: instances,
            base_vertex,
            base_instance,
        };
        self.draw(primitive, draw, Some(indices))
    }

    fn draw(&mut self, primitive: PrimitiveType, call: DrawCall, indices: Option<&Buffer>) -> Result<(), Error> {
        let p = self.pipeline.clone().ok_or_else(|| invalid("a draw without a render pipeline state"))?;
        let device = self.core.device();
        let mut state = BindState::default();
        state.stages.insert(Stage::Vertex, self.vertex.facts());
        state.stages.insert(Stage::Fragment, self.fragment.facts());
        state.render_target_array_length = self.layers;
        state.render_area = [self.width, self.height];
        if let Some(ds) = &self.depth_stencil {
            state.depth_stencil = ds.descriptor().clone();
        }
        state.primitive_type = Some(primitive);
        // the pipeline's compile this binding runs: the base, or a variant compiled for it
        let (v, plan) = p.settle(&state.shape())?;
        let c = plan.instantiate_draw(&state, &call)?;
        if !c.descriptor_bytes.is_empty() || !c.root_tables.is_empty() {
            return Err(not_yet("argument buffers and handle tables (descriptor bytes)"));
        }
        if !c.push_holes.is_empty() {
            return Err(not_yet(format!("push holes {:?} (device addresses the shader reads)", c.push_holes)));
        }
        if !c.amplifications.is_empty() || c.mesh_dispatch.is_some() || c.index_bounds_pass || c.index_overrun.is_some()
            || c.indirect_needs_arguments
        {
            return Err(not_yet("this draw's binder form (amplification, mesh, index bounds, indirect arguments)"));
        }
        let fs_program = v.fs.as_ref().ok_or_else(|| not_yet("render pipelines without a fragment function"))?;
        let mut spec = spec_of(&plan.specialization);
        if let Some(s) = &c.specialization {
            spec = spec_merge(&spec, &spec_of(s));
        }
        let vs = v.vs.shader(&device, &spec)?;
        let fs = fs_program.shader(&device, &spec)?;
        let mut used = Vec::new();
        let stages = [
            StageRef { program: &v.vs, metal: Stage::Vertex, table: &self.vertex },
            StageRef { program: fs_program, metal: Stage::Fragment, table: &self.fragment },
        ];
        let binds = resolve(&device, &stages, &plan, &c.descriptor_ranges, Some(&self.attachments), &mut used)?;
        let push = push_block(&c.push_constants);

        // vertex input: elements in the vertex stage's input order
        let vi = &v.pipeline.vertex_input;
        let stride_of = |binding: u32| -> Result<u32, Error> {
            let b = vi.bindings.iter().find(|b| b.binding == binding).ok_or_else(|| invalid("an attribute of no binding"))?;
            Ok(match b.stride {
                metal2vulkan::pipeline::VertexStride::Static(s) => s,
                metal2vulkan::pipeline::VertexStride::PerBind => {
                    c.vertex_strides.iter().find(|(k, _)| *k == binding).map(|(_, s)| *s).unwrap_or(0)
                }
            })
        };
        let mut elements = Vec::new();
        for i in 0..vs.info.inputs as usize {
            let location = vs.info.input_locations[i];
            let a = vi
                .attributes
                .iter()
                .find(|a| a.location == location)
                .ok_or_else(|| invalid(format!("the vertex function reads attribute {location}, which the descriptor lacks")))?;
            let b = vi.bindings.iter().find(|b| b.binding == a.binding).ok_or_else(|| invalid("an attribute of no binding"))?;
            let divisor = if b.input_rate == vk::VertexInputRate::INSTANCE { b.divisor.max(1) } else { 0 };
            if b.input_rate == vk::VertexInputRate::INSTANCE && b.divisor == 0 {
                return Err(not_yet("a constant vertex step function"));
            }
            elements.push(ffi::sg_vertex_element {
                vk_format: a.format.as_raw() as u32,
                offset: a.offset,
                buffer: a.binding,
                stride: stride_of(a.binding)?,
                divisor,
            });
        }
        let nvb = plan.vertex_buffers.iter().map(|v| v.binding + 1).max().unwrap_or(0) as usize;
        let mut vertex_buffers = vec![None; nvb];
        let mut vertex_offsets = vec![0u64; nvb];
        for v in &plan.vertex_buffers {
            let (buffer, base) = match self.vertex.buffers.get(&v.buffer_index) {
                Some(Slot::Buffer { buffer, offset }) => (buffer.clone(), *offset),
                Some(Slot::Bytes { buffer, offset, .. }) => (buffer.clone(), *offset),
                None => return Err(invalid(format!("no vertex buffer bound at index {}", v.buffer_index))),
            };
            let off = c.buffer_offsets.iter().find(|(k, _)| *k == v.binding).map_or(base, |(_, o)| *o);
            used.push(buffer.0.id);
            vertex_buffers[v.binding as usize] = Some(buffer.0.clone());
            vertex_offsets[v.binding as usize] = off;
        }

        // blend from the pipeline's graphics state
        let g = &v.pipeline.graphics;
        let blend: Vec<ffi::sg_blend_rt> = g
            .color_attachments
            .iter()
            .map(|a| match a {
                None => ffi::sg_blend_rt::default(),
                Some(a) => match &a.blend {
                    None => ffi::sg_blend_rt { write_mask: a.write_mask.as_raw(), ..Default::default() },
                    Some(b) => ffi::sg_blend_rt {
                        enable: 1,
                        src_color: b.src_color.as_raw() as u32,
                        dst_color: b.dst_color.as_raw() as u32,
                        color_op: b.color_op.as_raw() as u32,
                        src_alpha: b.src_alpha.as_raw() as u32,
                        dst_alpha: b.dst_alpha.as_raw() as u32,
                        alpha_op: b.alpha_op.as_raw() as u32,
                        write_mask: a.write_mask.as_raw(),
                    },
                },
            })
            .collect();

        let depth_stencil = match &self.depth_stencil {
            Some(ds) if self.has_depth || self.has_stencil => {
                let d = ds.descriptor();
                let face = |s: &metal2vulkan::metal::state::StencilDescriptor| ffi::sg_stencil_face {
                    compare: s.stencil_compare_function.as_raw(),
                    fail: s.stencil_failure_operation.as_raw(),
                    depth_fail: s.depth_failure_operation.as_raw(),
                    pass: s.depth_stencil_pass_operation.as_raw(),
                    read_mask: s.read_mask,
                    write_mask: s.write_mask,
                };
                Some(ffi::sg_depth_stencil {
                    compare: if self.has_depth { d.depth_compare_function.as_raw() } else { 7 },
                    write: (self.has_depth && d.depth_write_enabled) as u32,
                    stencil_enable: self.has_stencil as u32,
                    front: face(&d.front_face_stencil),
                    back: face(&d.back_face_stencil),
                })
            }
            _ => None,
        };

        let raster = ffi::sg_raster {
            cull: self.cull.as_raw(),
            winding: self.winding.as_raw(),
            fill: self.fill.as_raw(),
            depth_clip: if self.depth_clamp { 0 } else { 1 },
            samples: self.samples,
            depth_bias: self.depth_bias[0],
            depth_bias_slope: self.depth_bias[1],
            depth_bias_clamp: self.depth_bias[2],
        };

        let restart = matches!(primitive, PrimitiveType::LineStrip | PrimitiveType::TriangleStrip);
        let (first_vertex, count, first_instance, instances, index, base_vertex) = match c.draw {
            vk::DrawCommand::Draw { vertex_count, instance_count, first_vertex, first_instance } => {
                (first_vertex, vertex_count, first_instance, instance_count, None, 0)
            }
            vk::DrawCommand::DrawIndexed { index_type, index_buffer_offset, index_count, instance_count, first_index,
                                           vertex_offset, first_instance } => {
                let b = indices.ok_or_else(|| invalid("an indexed draw without indices"))?;
                let size = if index_type == vk::IndexType::UINT16 { 2 } else { 4 };
                used.push(b.0.id);
                no_copy_buffer(b, &mut self.core.no_copy);
                let offset = index_buffer_offset + first_index as u64 * size as u64;
                (0, index_count, first_instance, instance_count, Some((b.0.clone(), size, offset)), vertex_offset)
            }
            other => return Err(not_yet(format!("draw command {other:?}"))),
        };

        self.vertex.used(&mut self.core.used);
        self.fragment.used(&mut self.core.used);
        self.core.used.extend(used);
        no_copy_of(&self.vertex, &mut self.core.no_copy);
        no_copy_of(&self.fragment, &mut self.core.no_copy);
        self.core.cmds.push(Cmd::Draw(Box::new(DrawCmd {
            vs,
            fs,
            binds,
            push,
            blend,
            alpha_to_coverage: g.alpha_to_coverage,
            alpha_to_one: g.alpha_to_one,
            blend_color: self.blend_color,
            raster,
            depth_stencil,
            stencil_ref: self.stencil_ref,
            viewport: self.viewport,
            scissor: self.scissor,
            elements,
            vertex_buffers,
            vertex_offsets,
            primitive: primitive.as_raw(),
            primitive_restart: restart,
            first_vertex,
            count,
            first_instance,
            instances,
            index,
            base_vertex,
            texture_barrier: !g.input_attachments.is_empty(),
        })));
        Ok(())
    }

    pub fn end_encoding(mut self) -> Result<(), Error> {
        self.core.cmds.push(Cmd::EndPass);
        for r in std::mem::take(&mut self.resolves) {
            self.core.cmds.push(Cmd::Resolve(Box::new(r)));
        }
        self.core.end();
        Ok(())
    }
}

// ---- blit ----

/// `MTLBlitCommandEncoder`.
pub struct BlitCommandEncoder {
    core: Core,
}

impl BlitCommandEncoder {
    pub fn update_fence(&mut self, fence: &Fence) -> Result<(), Error> {
        self.core.update_fence(fence);
        Ok(())
    }
    pub fn wait_for_fence(&mut self, fence: &Fence) -> Result<(), Error> {
        self.core.wait_for_fence(fence);
        Ok(())
    }

    fn use_buffer(&mut self, b: &Buffer) {
        self.core.used.push(b.0.id);
        no_copy_buffer(b, &mut self.core.no_copy);
    }

    fn use_texture(&mut self, t: &Texture) {
        self.core.used.extend(t.ids());
        no_copy_texture(t, &mut self.core.no_copy);
    }

    /// `copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:`.
    pub fn copy_from_buffer_to_buffer(&mut self, src: &Buffer, src_offset: u64, dst: &Buffer, dst_offset: u64, size: u64)
                                      -> Result<(), Error> {
        if src_offset + size > src.length() || dst_offset + size > dst.length() {
            return Err(invalid("a buffer copy past a buffer's end"));
        }
        self.use_buffer(src);
        self.use_buffer(dst);
        self.core.cmds.push(Cmd::CopyBuffer { src: src.0.clone(), src_offset, dst: dst.0.clone(), dst_offset, size });
        Ok(())
    }

    /// `fillBuffer:range:value:`.
    pub fn fill_buffer(&mut self, buffer: &Buffer, range: Range<u64>, value: u8) -> Result<(), Error> {
        if range.start > range.end || range.end > buffer.length() {
            return Err(invalid("a fill past the buffer's end"));
        }
        self.use_buffer(buffer);
        self.core.cmds.push(Cmd::FillBuffer { buffer: buffer.0.clone(), offset: range.start, size: range.end - range.start, value });
        Ok(())
    }

    /// `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:...`.
    #[allow(clippy::too_many_arguments)]
    pub fn copy_from_texture_to_texture(&mut self, src: &Texture, src_slice: u64, src_level: u64, src_origin: Origin,
                                        size: Size, dst: &Texture, dst_slice: u64, dst_level: u64, dst_origin: Origin)
                                        -> Result<(), Error> {
        let (sl, ss) = src.image_level_slice(src_level, src_slice)?;
        let (dl, ds) = dst.image_level_slice(dst_level, dst_slice)?;
        let b = sg_box(src_origin, size)?;
        self.use_texture(src);
        self.use_texture(dst);
        self.core.cmds.push(Cmd::CopyTexture {
            src: src.0.image.clone(),
            src_level: sl,
            src_layer: ss,
            src_box: b,
            dst: dst.0.image.clone(),
            dst_level: dl,
            dst_layer: ds,
            dst_origin: [u32_of(dst_origin.x, "origin")?, u32_of(dst_origin.y, "origin")?, u32_of(dst_origin.z, "origin")?],
        });
        Ok(())
    }

    /// `copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:...`.
    #[allow(clippy::too_many_arguments)]
    pub fn copy_from_buffer_to_texture(&mut self, src: &Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64,
                                       size: Size, dst: &Texture, slice: u64, level: u64, origin: Origin,
                                       option: BlitOption) -> Result<(), Error> {
        if option != BlitOption::None {
            return Err(not_yet(format!("blit option {option:?}")));
        }
        let (l, s) = dst.image_level_slice(level, slice)?;
        let b = sg_box(origin, size)?;
        self.use_buffer(src);
        self.use_texture(dst);
        self.core.cmds.push(Cmd::BufferToTexture {
            src: src.0.clone(),
            offset,
            stride: u32_of(bytes_per_row, "bytesPerRow")?,
            layer_stride: u32_of(bytes_per_image, "bytesPerImage")?,
            dst: dst.0.image.clone(),
            level: l,
            layer: s,
            b,
        });
        Ok(())
    }

    /// `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:...`.
    #[allow(clippy::too_many_arguments)]
    pub fn copy_from_texture_to_buffer(&mut self, src: &Texture, slice: u64, level: u64, origin: Origin, size: Size,
                                       dst: &Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64,
                                       option: BlitOption) -> Result<(), Error> {
        if option != BlitOption::None {
            return Err(not_yet(format!("blit option {option:?}")));
        }
        let (l, s) = src.image_level_slice(level, slice)?;
        let b = sg_box(origin, size)?;
        self.use_texture(src);
        self.use_buffer(dst);
        self.core.cmds.push(Cmd::TextureToBuffer {
            src: src.0.image.clone(),
            level: l,
            layer: s,
            b,
            dst: dst.0.clone(),
            offset,
            stride: u32_of(bytes_per_row, "bytesPerRow")?,
            layer_stride: u32_of(bytes_per_image, "bytesPerImage")?,
        });
        Ok(())
    }

    /// `generateMipmapsForTexture:`.
    pub fn generate_mipmaps(&mut self, texture: &Texture) -> Result<(), Error> {
        self.use_texture(texture);
        self.core.cmds.push(Cmd::GenerateMipmaps(texture.0.image.clone()));
        Ok(())
    }

    pub fn end_encoding(mut self) -> Result<(), Error> {
        self.core.end();
        Ok(())
    }
}
