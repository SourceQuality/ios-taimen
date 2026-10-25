// SPDX-License-Identifier: MIT
//! From Metal's argument tables to Gallium bindings, through the translator's binder.
//!
//! An encoder keeps Metal's tables per stage (`Table`). At each dispatch or draw the tables become
//! the binder's facts (`StageTable`), the pipeline's plan for their shape says which descriptor
//! (set, binding) gets which resource (`DescriptorWrite`), and instantiation the buffer ranges and
//! push bytes. `resolve` turns the writes into Gallium bindings at the slots `assign_slots` gave
//! each stage (`Bind`), holding what they reference until the command buffer completes.

use crate::device::Device;
use crate::error::{invalid, not_yet, Error};
use crate::ffi;
use crate::pipeline::{neutral_sampler, SamplerState, SgSampler, StageProgram};
use crate::resource::{Buffer, BufferInner, SgTexture, SgView, Texture};
use metal2vulkan::bind::{
    BoundBuffer, BoundSampler, BoundTexture, DescriptorRange, ImageSource, ImageViewDesc, NeutralResource,
    PushConstantWrite, SamplerSource, StageTable, TextureFacts, UseTarget, WriteSource,
};
use metal2vulkan::bind::BindingPlan;
use metal2vulkan::pipeline::{SlotRef, Stage};
use metal2vulkan::vk;
use std::collections::BTreeMap;
use std::sync::Arc;

/// What a Metal buffer slot holds.
#[derive(Clone)]
pub(crate) enum Slot {
    Buffer { buffer: Buffer, offset: u64 },
    /// `set*Bytes`: the bytes' copy in the command buffer's upload arena.
    Bytes { buffer: Buffer, offset: u64, len: u32 },
}

/// One stage's Metal argument table.
#[derive(Clone, Default)]
pub(crate) struct Table {
    pub(crate) buffers: BTreeMap<u32, Slot>,
    pub(crate) textures: BTreeMap<u32, Texture>,
    pub(crate) samplers: BTreeMap<u32, SamplerState>,
    pub(crate) threadgroup: BTreeMap<u32, u32>,
}

/// Metal's argument-table limits (indices below them).
pub(crate) const BUFFER_SLOTS: u32 = 31;
pub(crate) const TEXTURE_SLOTS: u32 = 128;
pub(crate) const SAMPLER_SLOTS: u32 = 16;

impl Table {
    pub(crate) fn set_buffer(&mut self, buffer: Option<&Buffer>, offset: u64, index: u32) -> Result<(), Error> {
        if index >= BUFFER_SLOTS {
            return Err(invalid(format!("buffer index {index}")));
        }
        match buffer {
            Some(b) => {
                if offset > b.length() {
                    return Err(invalid(format!("buffer offset {offset} past its length {}", b.length())));
                }
                self.buffers.insert(index, Slot::Buffer { buffer: b.clone(), offset });
            }
            None => {
                self.buffers.remove(&index);
            }
        }
        Ok(())
    }

    pub(crate) fn set_buffer_offset(&mut self, offset: u64, index: u32) -> Result<(), Error> {
        match self.buffers.get_mut(&index) {
            Some(Slot::Buffer { buffer, offset: o }) => {
                if offset > buffer.length() {
                    return Err(invalid("buffer offset past its length"));
                }
                *o = offset;
                Ok(())
            }
            _ => Err(invalid(format!("setBufferOffset at index {index} with no buffer bound"))),
        }
    }

    pub(crate) fn set_texture(&mut self, texture: Option<&Texture>, index: u32) -> Result<(), Error> {
        if index >= TEXTURE_SLOTS {
            return Err(invalid(format!("texture index {index}")));
        }
        match texture {
            Some(t) => self.textures.insert(index, t.clone()),
            None => self.textures.remove(&index),
        };
        Ok(())
    }

    pub(crate) fn set_sampler(&mut self, sampler: Option<&SamplerState>, index: u32) -> Result<(), Error> {
        if index >= SAMPLER_SLOTS {
            return Err(invalid(format!("sampler index {index}")));
        }
        match sampler {
            Some(s) => self.samplers.insert(index, s.clone()),
            None => self.samplers.remove(&index),
        };
        Ok(())
    }

    /// The binder's facts of this table.
    pub(crate) fn facts(&self) -> StageTable {
        let mut t = StageTable::default();
        for (&i, slot) in &self.buffers {
            let bound = match slot {
                Slot::Buffer { buffer, offset } => BoundBuffer::Buffer {
                    length: buffer.length(),
                    offset: *offset,
                    gpu_address: buffer.gpu_address().0,
                    device_address: None,
                    attribute_stride: None,
                },
                Slot::Bytes { len, .. } => BoundBuffer::Bytes { len: *len, attribute_stride: None },
            };
            t.buffers.insert(i, bound);
        }
        for (&i, tex) in &self.textures {
            let mut facts = TextureFacts::new(tex.texture_type(), tex.pixel_format());
            facts.sample_count = tex.sample_count() as u32;
            facts.usage = tex.usage();
            facts.array_length = tex.array_length() as u32;
            facts.mipmap_level_count = tex.mipmap_level_count() as u32;
            t.textures.insert(i, BoundTexture::Texture { facts, resource_id: None });
        }
        for (&i, s) in &self.samplers {
            t.samplers.insert(i, BoundSampler::new(s.0.desc.clone()));
        }
        for (&i, &len) in &self.threadgroup {
            t.threadgroup_lengths.insert(i, len);
        }
        t
    }

    /// The ids of what the table's resources own (for `used_resources`).
    pub(crate) fn used(&self, out: &mut Vec<u64>) {
        for slot in self.buffers.values() {
            if let Slot::Buffer { buffer, .. } = slot {
                out.push(buffer.0.id);
            }
        }
        for t in self.textures.values() {
            out.extend(t.ids());
        }
    }
}

/// What one Gallium binding holds.
pub(crate) enum BindRes {
    Buffer { buffer: Arc<BufferInner>, offset: u64, size: u32, writable: bool },
    View(Arc<SgView>),
    Sampler(Arc<SgSampler>),
    Combined(Arc<SgView>, Arc<SgSampler>),
    Image { image: Arc<SgTexture>, vk_format: u32, level: u32, first_layer: u32, last_layer: u32, writable: bool },
}

pub(crate) struct Bind {
    pub(crate) stage: u32,
    pub(crate) cls: u32,
    pub(crate) slot: u32,
    pub(crate) sampler_slot: u32,
    pub(crate) res: BindRes,
}

/// One stage of a command: its program (slots), Metal stage and table.
pub(crate) struct StageRef<'a> {
    pub(crate) program: &'a StageProgram,
    pub(crate) metal: Stage,
    pub(crate) table: &'a Table,
}

fn view_type(ty: vk::ImageViewType) -> Result<u32, Error> {
    // vk::ImageViewType raw: 1D 0, 2D 1, 3D 2, CUBE 3, 1D_ARRAY 4, 2D_ARRAY 5, CUBE_ARRAY 6 ->
    // MTLTextureType
    Ok(match ty.as_raw() {
        0 => 0,
        1 => 2,
        2 => 7,
        3 => 5,
        4 => 1,
        5 => 3,
        6 => 6,
        other => return Err(not_yet(format!("image view type {other}"))),
    })
}

fn swizzle(c: vk::ComponentSwizzle, channel: u8) -> u8 {
    // VkComponentSwizzle raw: IDENTITY 0, ZERO 1, ONE 2, R 3, G 4, B 5, A 6 -> MTLTextureSwizzle
    match c.as_raw() {
        1 => 0,
        2 => 1,
        3..=6 => (c.as_raw() - 1) as u8,
        _ => 2 + channel,
    }
}

/// The Gallium sampler view of `texture` the binder's view describes.
pub(crate) fn texture_view(texture: &Texture, view: &ImageViewDesc) -> Result<Arc<SgView>, Error> {
    let r = texture.0.view;
    let (first_level, last_level) = match view.level {
        Some(l) => (r.levels.0 + l, r.levels.0 + l),
        None => (r.levels.0, r.levels.1 - 1),
    };
    let m = &view.components;
    let desc = ffi::sg_view_desc {
        vk_format: view.format.as_raw() as u32,
        ty: view_type(view.view_type)?,
        first_level,
        last_level,
        first_layer: r.slices.0,
        last_layer: r.slices.1 - 1,
        swizzle: [swizzle(m.r, 0), swizzle(m.g, 1), swizzle(m.b, 2), swizzle(m.a, 3)],
    };
    texture.sampler_view(desc)
}

/// The attachment's whole view, for framebuffer fetch (a texel fetch at the fragment).
fn attachment_view(texture: &Texture, level: u32, slice: u32) -> Result<Arc<SgView>, Error> {
    let desc = ffi::sg_view_desc {
        vk_format: crate::resource::vk_format(texture.pixel_format())?,
        ty: 2,
        first_level: level,
        last_level: level,
        first_layer: slice,
        last_layer: slice,
        swizzle: [2, 3, 4, 5],
    };
    texture.sampler_view(desc)
}

fn range_of(ranges: &[DescriptorRange], set: u32, binding: u32) -> Option<(u64, u64)> {
    ranges.iter().find(|r| r.set == set && r.binding == binding).map(|r| (r.range.offset, r.range.range))
}

fn writes(plan: &BindingPlan, slot: &SlotRef) -> bool {
    plan.uses.iter().any(|u| matches!(&u.resource, UseTarget::Slot(s) if s == slot) && u.access.write)
}

fn table<'a>(stages: &'a [StageRef<'a>], slot: &SlotRef) -> Result<&'a Table, Error> {
    stages
        .iter()
        .find(|s| s.metal == slot.stage)
        .map(|s| s.table)
        .ok_or_else(|| not_yet(format!("a descriptor of stage {:?} this command does not run", slot.stage)))
}

fn sampler_of(device: &Device, stages: &[StageRef], source: &SamplerSource, program: &StageProgram, set: u32, binding: u32)
              -> Result<Arc<SgSampler>, Error> {
    match source {
        SamplerSource::Slot { slot, compare, integer, .. } => {
            let t = table(stages, slot)?;
            let s = t
                .samplers
                .get(&slot.index)
                .ok_or_else(|| invalid(format!("no sampler bound at index {} of the {:?} stage", slot.index, slot.stage)))?;
            s.sg(*compare, *integer)
        }
        SamplerSource::Neutral(_) => neutral_sampler(device),
        SamplerSource::Immutable => program
            .immutable_sampler(set, binding)
            .cloned()
            .ok_or_else(|| invalid(format!("an immutable sampler at ({set}, {binding}) the layout lacks"))),
    }
}

fn image_of(stages: &[StageRef], source: &ImageSource) -> Result<Arc<SgView>, Error> {
    match source {
        ImageSource::Texture { slot, view } => {
            let t = table(stages, slot)?;
            let tex = t
                .textures
                .get(&slot.index)
                .ok_or_else(|| invalid(format!("no texture bound at index {} of the {:?} stage", slot.index, slot.stage)))?;
            texture_view(tex, view)
        }
        ImageSource::Neutral(n) => Err(not_yet(format!("a neutral image ({n:?}) where nothing is bound"))),
    }
}

/// The pass's attachments a draw may read (framebuffer fetch): texture, level, slice.
pub(crate) type Attachments = [Option<(Texture, u32, u32)>; 8];

/// Gallium bindings for every descriptor write of `plan`, in the stages that see each binding.
pub(crate) fn resolve(device: &Device, stages: &[StageRef], plan: &BindingPlan, ranges: &[DescriptorRange],
                      attachments: Option<&Attachments>, used: &mut Vec<u64>) -> Result<Vec<Bind>, Error> {
    let mut out = Vec::new();
    for w in &plan.descriptors {
        for s in stages {
            let Some(b) = s.program.binding(w.set, w.binding) else { continue };
            let slot = b.slot + w.array_element;
            let sampler_slot = b.sampler_slot + w.array_element;
            let res = match &w.source {
                WriteSource::Buffer { slot: mslot } => {
                    let t = table(stages, mslot)?;
                    let (buffer, base) = match t.buffers.get(&mslot.index) {
                        Some(Slot::Buffer { buffer, .. }) => (buffer.clone(), 0),
                        Some(Slot::Bytes { buffer, offset, .. }) => (buffer.clone(), *offset),
                        None => {
                            return Err(invalid(format!("no buffer bound at index {} of the {:?} stage",
                                                       mslot.index, mslot.stage)))
                        }
                    };
                    let (offset, size) = range_of(ranges, w.set, w.binding)
                        .ok_or_else(|| not_yet(format!("buffer descriptor ({}, {}) without a range", w.set, w.binding)))?;
                    used.push(buffer.0.id);
                    let size = u32::try_from(size).map_err(|_| invalid("buffer range past 4 GiB"))?;
                    BindRes::Buffer { buffer: buffer.0.clone(), offset: base + offset, size, writable: writes(plan, mslot) }
                }
                WriteSource::InlineBytes { slot: mslot } => {
                    let t = table(stages, mslot)?;
                    let Some(Slot::Bytes { buffer, offset, len }) = t.buffers.get(&mslot.index) else {
                        return Err(invalid(format!("no bytes set at index {} of the {:?} stage", mslot.index, mslot.stage)));
                    };
                    let (roff, rsize) = range_of(ranges, w.set, w.binding).unwrap_or((0, *len as u64));
                    BindRes::Buffer { buffer: buffer.0.clone(), offset: offset + roff, size: rsize as u32, writable: false }
                }
                WriteSource::Neutral(NeutralResource::ZeroBuffer { min_bytes }) => {
                    let b = device.neutral_zero(*min_bytes as u64)?;
                    let size = range_of(ranges, w.set, w.binding).map_or(*min_bytes as u64, |r| r.1).max(16);
                    BindRes::Buffer { buffer: b.0.clone(), offset: 0, size: size as u32, writable: false }
                }
                WriteSource::Neutral(NeutralResource::Sink { min_bytes }) => {
                    let b = device.neutral_sink(*min_bytes as u64)?;
                    let size = range_of(ranges, w.set, w.binding).map_or(*min_bytes as u64, |r| r.1).max(16);
                    BindRes::Buffer { buffer: b.0.clone(), offset: 0, size: size as u32, writable: true }
                }
                WriteSource::Neutral(NeutralResource::Sampler { .. }) => BindRes::Sampler(neutral_sampler(device)?),
                WriteSource::Texture { slot: mslot, view } => {
                    let t = table(stages, mslot)?;
                    let tex = t.textures.get(&mslot.index).ok_or_else(|| {
                        invalid(format!("no texture bound at index {} of the {:?} stage", mslot.index, mslot.stage))
                    })?;
                    used.extend(tex.ids());
                    if b.cls == ffi::SG_CLASS_IMAGE {
                        let r = tex.0.view;
                        BindRes::Image {
                            image: tex.0.image.clone(),
                            vk_format: view.format.as_raw() as u32,
                            level: r.levels.0 + view.level.unwrap_or(0),
                            first_layer: r.slices.0,
                            last_layer: r.slices.1 - 1,
                            writable: writes(plan, mslot),
                        }
                    } else {
                        BindRes::View(texture_view(tex, view)?)
                    }
                }
                WriteSource::Sampler { slot: mslot, compare, integer, opaque_black } => {
                    let source = SamplerSource::Slot { slot: *mslot, compare: *compare, integer: *integer, opaque_black: *opaque_black };
                    BindRes::Sampler(sampler_of(device, stages, &source, s.program, w.set, w.binding)?)
                }
                WriteSource::CombinedImageSampler { image, sampler } => {
                    BindRes::Combined(image_of(stages, image)?, sampler_of(device, stages, sampler, s.program, w.set, w.binding)?)
                }
                WriteSource::Attachment { color_index } => {
                    let a = attachments
                        .and_then(|a| a.get(*color_index as usize))
                        .and_then(|a| a.as_ref())
                        .ok_or_else(|| invalid(format!("framebuffer fetch of color attachment {color_index}, which the pass lacks")))?;
                    BindRes::View(attachment_view(&a.0, a.1, a.2)?)
                }
                other => return Err(not_yet(format!("descriptor source {other:?}"))),
            };
            out.push(Bind { stage: s.program.stage, cls: b.cls, slot, sampler_slot, res });
        }
    }
    // Immutable samplers of sampler-only descriptors take no write (Vulkan's rule): bind them.
    for s in stages {
        for (set, binding, sampler) in &s.program.immutable {
            let Some(b) = s.program.binding(*set, *binding) else { continue };
            if b.cls != ffi::SG_CLASS_SAMPLER {
                continue;
            }
            let written = out.iter().any(|o| o.stage == s.program.stage && o.cls == b.cls && o.sampler_slot == b.sampler_slot);
            if !written {
                out.push(Bind { stage: s.program.stage, cls: b.cls, slot: 0, sampler_slot: b.sampler_slot,
                                res: BindRes::Sampler(sampler.clone()) });
            }
        }
    }
    Ok(out)
}

/// The push block (constant buffer 0 of every stage) the writes make.
pub(crate) fn push_block(writes: &[PushConstantWrite]) -> Vec<u8> {
    let end = writes.iter().map(|w| w.offset as usize + w.bytes.len()).max().unwrap_or(0);
    let mut block = vec![0u8; end.div_ceil(16) * 16];
    for w in writes {
        block[w.offset as usize..w.offset as usize + w.bytes.len()].copy_from_slice(&w.bytes);
    }
    block
}

/// Binds `binds` into the device's context (the lock is held: `sg`).
pub(crate) fn apply(sg: *mut ffi::sg_device, binds: &[Bind]) {
    for b in binds {
        // SAFETY: the device lock is held by the caller; every handle is kept alive by `binds`.
        unsafe {
            match &b.res {
                BindRes::Buffer { buffer, offset, size, writable } => match b.cls {
                    ffi::SG_CLASS_SSBO => ffi::sg_ctx_set_ssbo(sg, b.stage, b.slot, buffer.sg.sg, *offset, *size, *writable),
                    _ => ffi::sg_ctx_set_ubo(sg, b.stage, b.slot, buffer.sg.sg, *offset, *size),
                },
                BindRes::View(v) => ffi::sg_ctx_set_view(sg, b.stage, b.slot, v.sg),
                BindRes::Sampler(s) => ffi::sg_ctx_set_sampler(sg, b.stage, b.sampler_slot, s.sg),
                BindRes::Combined(v, s) => {
                    ffi::sg_ctx_set_view(sg, b.stage, b.slot, v.sg);
                    ffi::sg_ctx_set_sampler(sg, b.stage, b.sampler_slot, s.sg);
                }
                BindRes::Image { image, vk_format, level, first_layer, last_layer, writable } => ffi::sg_ctx_set_image(
                    sg, b.stage, b.slot, image.sg, std::ptr::null_mut(), *vk_format, *level, *first_layer,
                    *last_layer, 0, 0, *writable,
                ),
            }
        }
    }
}
