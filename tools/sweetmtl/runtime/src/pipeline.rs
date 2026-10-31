// SPDX-License-Identifier: MIT
//! Pipelines and states: compute and render pipeline states from the translator, sampler and
//! depth/stencil states, binary archives.
//!
//! A pipeline is the translator's plain-data pipeline (SPIR-V per stage, one layout) plus, per
//! stage, the Gallium slots of its descriptors (`assign_slots`) and the stage's CSOs, one per
//! specialization the binder's plans ask for (compiled when first asked, under the device lock).
//! Creating the pipeline compiles each stage once with its default specialization, so a stage ir3
//! cannot compile fails the creation, as Metal fails it. A pipeline state holds that compile and
//! the variants its draws or dispatches ask for (see "variants" below).

use crate::device::{Device, Gpu, MessageSeverity};
use crate::error::{gallium, invalid, not_yet, Error};
use crate::ffi;
use crate::library::Function;
use crate::resource::SgView;
use metal2vulkan::bind::{BindShape, BindingPlan, Dispatch, SamplerDescriptor};
use metal2vulkan::cache::{SpecializationKey, VariantKey};
use metal2vulkan::metal::state::DepthStencilDescriptor;
use metal2vulkan::pipeline::{
    ComputePipeline, ComputePipelineDescriptor, PipelineLayout, RenderPipeline, RenderPipelineDescriptor, Stage,
    StageSource,
};
use metal2vulkan::pipeline::StorageImageClass;
use metal2vulkan::vk;
use metal2vulkan::Budget;
use std::ffi::CString;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Instant;

// ---- options and archives ----

/// `MTLPipelineOption` bits (reflection requests): taken and ignored, no reflection is returned.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PipelineOption(pub u64);

impl PipelineOption {
    pub fn from_raw(raw: u64) -> PipelineOption {
        PipelineOption(raw)
    }
}

#[derive(Clone, Default)]
pub struct PipelineOptions {
    pub options: PipelineOption,
    /// Archives to look pipelines up in: none is read yet (every pipeline compiles).
    pub binary_archives: Vec<BinaryArchive>,
}

#[derive(Clone, Debug, Default)]
pub struct BinaryArchiveDescriptor {
    pub url: Option<PathBuf>,
}

impl BinaryArchiveDescriptor {
    pub fn new(url: Option<PathBuf>) -> BinaryArchiveDescriptor {
        BinaryArchiveDescriptor { url }
    }
}

/// `MTLBinaryArchive`: a list of the pipelines added to it. It is a cache Metal may ignore: no
/// archive is read (pipelines compile), and serializing one is not yet implemented.
#[derive(Clone)]
pub struct BinaryArchive(Arc<Mutex<ArchiveInner>>);

struct ArchiveInner {
    _url: Option<PathBuf>,
    render: usize,
    compute: usize,
}

impl Device {
    pub fn new_binary_archive(&self, descriptor: &BinaryArchiveDescriptor) -> Result<BinaryArchive, Error> {
        Ok(BinaryArchive(Arc::new(Mutex::new(ArchiveInner { _url: descriptor.url.clone(), render: 0, compute: 0 }))))
    }
}

impl BinaryArchive {
    pub fn add_render_pipeline_functions(&self, _descriptor: &RenderPipelineDescriptor) -> Result<(), Error> {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).render += 1;
        Ok(())
    }
    pub fn add_compute_pipeline_functions(&self, _descriptor: &ComputePipelineDescriptor) -> Result<(), Error> {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).compute += 1;
        Ok(())
    }
    pub fn serialize_to_url(&self, _path: &std::path::Path) -> Result<(), Error> {
        Err(not_yet("serializing binary archives (the pipeline cache on disk, plan M3)"))
    }
}

// ---- shaders ----

/// One CSO of one stage under one specialization.
pub(crate) struct Shader {
    pub(crate) gpu: Arc<Gpu>,
    pub(crate) sg: *mut ffi::sg_shader,
    pub(crate) info: ffi::sg_shader_info,
}

// SAFETY: CSOs are used under the device lock only; freeing takes it.
unsafe impl Send for Shader {}
unsafe impl Sync for Shader {}

impl Drop for Shader {
    fn drop(&mut self) {
        // SAFETY: from sg_shader_new; never dropped under the device lock (recorded commands drop
        // on the completion thread, pipelines on their owner's).
        unsafe { ffi::sg_shader_free(self.sg) }
        let _ = &self.gpu;
    }
}

/// A specialization as (constant id, bytes) pairs, sorted by id.
pub(crate) type Spec = Vec<(u32, Vec<u8>)>;

pub(crate) fn spec_of(key: &SpecializationKey) -> Spec {
    let mut out: Spec = key
        .entries
        .iter()
        .map(|e| {
            let at = e.offset as usize;
            (e.constant_id, key.data.get(at..at + e.size as usize).map(<[u8]>::to_vec).unwrap_or_default())
        })
        .collect();
    out.sort_by_key(|(id, _)| *id);
    out
}

/// `key` over `base`: the later entries win.
pub(crate) fn spec_merge(base: &Spec, over: &Spec) -> Spec {
    let mut out = base.clone();
    for (id, bytes) in over {
        match out.iter_mut().find(|(i, _)| i == id) {
            Some(slot) => slot.1 = bytes.clone(),
            None => out.push((*id, bytes.clone())),
        }
    }
    out.sort_by_key(|(id, _)| *id);
    out
}

/// One stage of a pipeline: its SPIR-V, its Gallium slots and its CSOs by specialization.
pub(crate) struct StageProgram {
    pub(crate) stage: u32,
    spirv: Arc<[u32]>,
    entry: CString,
    pub(crate) bindings: Vec<ffi::sg_binding>,
    /// The layout's immutable samplers this stage sees (a function's constexpr samplers): set,
    /// binding, Gallium sampler. Bound whether or not the plan writes their descriptor.
    pub(crate) immutable: Vec<(u32, u32, Arc<SgSampler>)>,
    defaults: Spec,
    variants: Mutex<Vec<(Spec, Arc<Shader>)>>,
}

/// A Gallium sampler for a layout's immutable sampler (Vulkan's description of a constexpr one).
pub(crate) fn sampler_from_vk(device: &Device, s: &vk::SamplerDesc) -> Result<Arc<SgSampler>, Error> {
    // VkSamplerAddressMode -> MTLSamplerAddressMode
    let address = |m: vk::SamplerAddressMode| -> Result<u32, Error> {
        Ok(match m.as_raw() {
            0 => 2, // repeat
            1 => 3, // mirrored repeat
            2 => 0, // clamp to edge
            3 => 5, // clamp to border
            4 => 1, // mirror clamp to edge
            other => return Err(not_yet(format!("sampler address mode {other}"))),
        })
    };
    if s.mip_lod_bias != 0.0 {
        return Err(not_yet("a constexpr sampler with a LOD bias"));
    }
    // VkBorderColor: float/int transparent black 0/1, opaque black 2/3, opaque white 4/5
    let border = s.border_color.as_raw() as u32;
    let desc = ffi::sg_sampler_desc {
        min_filter: s.min_filter.as_raw() as u32,
        mag_filter: s.mag_filter.as_raw() as u32,
        mip_filter: s.mipmap_mode.as_raw() as u32 + 1,
        s_address: address(s.address_mode_u)?,
        t_address: address(s.address_mode_v)?,
        r_address: address(s.address_mode_w)?,
        border: border / 2,
        normalized: 1,
        max_anisotropy: s.max_anisotropy.map_or(1, |a| a as u32),
        compare: s.compare.map_or(0, |c| c.as_raw() as u32),
        compare_enable: s.compare.is_some() as u32,
        integer_border: border % 2,
        lod_min: s.min_lod,
        lod_max: s.max_lod,
    };
    // SAFETY: valid device and descriptor.
    let sg = unsafe { ffi::sg_sampler_new(device.gpu().sg, &desc) };
    if sg.is_null() {
        return Err(gallium("constexpr sampler"));
    }
    Ok(Arc::new(SgSampler { sg }))
}

impl StageProgram {
    fn new(device: &Device, stage: u32, module: &metal2vulkan::pipeline::StageModule, layout: &PipelineLayout)
           -> Result<StageProgram, Error> {
        let bit = match stage {
            ffi::SG_VERTEX => vk::ShaderStageFlags::VERTEX,
            ffi::SG_FRAGMENT => vk::ShaderStageFlags::FRAGMENT,
            _ => vk::ShaderStageFlags::COMPUTE,
        };
        let mut immutable = Vec::new();
        for set in &layout.sets {
            for b in &set.bindings {
                if let (true, Some(s)) = (b.stages.intersects(bit), &b.immutable_sampler) {
                    immutable.push((set.set, b.binding, sampler_from_vk(device, s)?));
                }
            }
        }
        let defaults = module
            .specialization
            .constants
            .iter()
            .map(|c| (c.id, c.default.to_le_bytes()[..(c.size as usize).min(8)].to_vec()))
            .collect::<Vec<_>>();
        let mut defaults = defaults;
        defaults.sort_by_key(|(id, _)| *id);
        Ok(StageProgram {
            stage,
            spirv: module.spirv.clone(),
            entry: CString::new(module.entry_point.clone()).map_err(|_| invalid("entry point name"))?,
            bindings: assign_slots(layout, bit, stage)?,
            immutable,
            defaults,
            variants: Mutex::new(Vec::new()),
        })
    }

    pub(crate) fn immutable_sampler(&self, set: u32, binding: u32) -> Option<&Arc<SgSampler>> {
        self.immutable.iter().find(|(s, b, _)| *s == set && *b == binding).map(|(_, _, s)| s)
    }

    /// The CSO for `spec` (over the stage's defaults), compiled now if this is its first use.
    pub(crate) fn shader(&self, device: &Device, spec: &Spec) -> Result<Arc<Shader>, Error> {
        let spec = spec_merge(&self.defaults, spec);
        let mut variants = self.variants.lock().unwrap_or_else(|p| p.into_inner());
        if let Some((_, s)) = variants.iter().find(|(k, _)| *k == spec) {
            return Ok(s.clone());
        }
        let entries: Vec<ffi::sg_spec> = spec
            .iter()
            .map(|(id, bytes)| ffi::sg_spec { id: *id, size: bytes.len() as u32, data: bytes.as_ptr() as *const _ })
            .collect();
        let desc = ffi::sg_shader_desc {
            stage: self.stage,
            words: self.spirv.as_ptr(),
            word_count: self.spirv.len(),
            entry: self.entry.as_ptr(),
            spec: entries.as_ptr(),
            spec_count: entries.len() as u32,
            bindings: self.bindings.as_ptr(),
            binding_count: self.bindings.len() as u32,
        };
        let gpu = device.gpu();
        let sg = {
            let lock = gpu.lock();
            // SAFETY: the descriptor's arrays outlive the call; the lock is held.
            unsafe { ffi::sg_shader_new(lock.sg(), &desc) }
        };
        if sg.is_null() {
            return Err(gallium(match self.stage {
                ffi::SG_VERTEX => "vertex stage",
                ffi::SG_FRAGMENT => "fragment stage",
                _ => "kernel",
            }));
        }
        let mut info = ffi::sg_shader_info::default();
        // SAFETY: valid shader.
        unsafe { ffi::sg_shader_info(sg, &mut info) };
        crate::device::trace(format_args!(
            "{} compiled: {} ir3 instructions, {} full registers, constlen {}{}",
            match self.stage {
                ffi::SG_VERTEX => "vertex stage",
                ffi::SG_FRAGMENT => "fragment stage",
                _ => "kernel",
            },
            info.instrs,
            info.full_regs,
            info.constlen,
            if self.stage == ffi::SG_COMPUTE {
                format!(", local size {}x{}x{}", info.workgroup[0], info.workgroup[1], info.workgroup[2])
            } else {
                String::new()
            }
        ));
        let shader = Arc::new(Shader { gpu: gpu.clone(), sg, info });
        variants.push((spec, shader.clone()));
        Ok(shader)
    }

    pub(crate) fn binding(&self, set: u32, binding: u32) -> Option<&ffi::sg_binding> {
        self.bindings.iter().find(|b| b.set == set && b.binding == binding)
    }
}

/// Gallium slots for the layout's descriptors that `stage` sees, in (set, binding) order: push
/// constants are constant buffer 0, uniform buffers 1.., storage buffers 0.. (the vertex stage's
/// read as constant buffers), sampled images and input attachments views 0.., samplers 0..,
/// storage images after the storage buffers' slots (a5xx shares one IBO space).
pub(crate) fn assign_slots(layout: &PipelineLayout, bit: vk::ShaderStageFlags, stage: u32)
                           -> Result<Vec<ffi::sg_binding>, Error> {
    let mut all = Vec::new();
    for set in &layout.sets {
        for b in &set.bindings {
            if b.stages.intersects(bit) {
                all.push((set.set, b));
            }
        }
    }
    all.sort_by_key(|(s, b)| (*s, b.binding));
    let (mut ubo, mut ssbo, mut tex, mut smp, mut img) = (1u32, 0u32, 0u32, 0u32, 0u32);
    let mut out = Vec::new();
    for (set, b) in &all {
        let n = b.count.max(1);
        let (cls, slot, sampler_slot) = match b.ty {
            vk::DescriptorType::UNIFORM_BUFFER | vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC => {
                ubo += n;
                (ffi::SG_CLASS_UBO, ubo - n, 0)
            }
            vk::DescriptorType::STORAGE_BUFFER | vk::DescriptorType::STORAGE_BUFFER_DYNAMIC
                if stage == ffi::SG_VERTEX =>
            {
                ubo += n;
                (ffi::SG_CLASS_SSBO_AS_UBO, ubo - n, 0)
            }
            vk::DescriptorType::STORAGE_BUFFER | vk::DescriptorType::STORAGE_BUFFER_DYNAMIC => {
                ssbo += n;
                (ffi::SG_CLASS_SSBO, ssbo - n, 0)
            }
            vk::DescriptorType::COMBINED_IMAGE_SAMPLER => {
                tex += n;
                smp += n;
                (ffi::SG_CLASS_COMBINED, tex - n, smp - n)
            }
            vk::DescriptorType::SAMPLED_IMAGE => {
                tex += n;
                (ffi::SG_CLASS_TEXTURE, tex - n, 0)
            }
            vk::DescriptorType::UNIFORM_TEXEL_BUFFER => {
                tex += n;
                (ffi::SG_CLASS_TEXEL_BUFFER, tex - n, 0)
            }
            vk::DescriptorType::INPUT_ATTACHMENT => {
                tex += n;
                (ffi::SG_CLASS_INPUT_ATTACHMENT, tex - n, 0)
            }
            vk::DescriptorType::SAMPLER => {
                smp += n;
                (ffi::SG_CLASS_SAMPLER, 0, smp - n)
            }
            vk::DescriptorType::STORAGE_IMAGE | vk::DescriptorType::STORAGE_TEXEL_BUFFER => {
                if stage == ffi::SG_VERTEX {
                    return Err(Error::Unsupported("a storage image in the vertex stage (a5xx has none)".into()));
                }
                img += n;
                (ffi::SG_CLASS_IMAGE, img - n, 0)
            }
            other => return Err(not_yet(format!("descriptor type {other:?}"))),
        };
        out.push(ffi::sg_binding { set: *set, binding: b.binding, count: n, cls, slot, sampler_slot });
    }
    // storage images take the IBO slots after the storage buffers'
    for b in out.iter_mut().filter(|b| b.cls == ffi::SG_CLASS_IMAGE) {
        b.slot += ssbo;
    }
    if ubo > 16 {
        return Err(Error::Unsupported(format!("{} uniform buffers in one stage (a5xx binds 15 beside the push block)", ubo - 1)));
    }
    if ssbo + img > 24 {
        return Err(Error::Unsupported(format!("{} storage buffers and images in one stage (a5xx: 24)", ssbo + img)));
    }
    if tex > 16 || smp > 16 {
        return Err(Error::Unsupported(format!("{tex} textures, {smp} samplers in one stage (a5xx: 16 each)")));
    }
    Ok(out)
}

/// A compiled pipeline's memo of plans by bind shape (a plan is a pure function of pipeline,
/// device and shape).
#[derive(Default)]
pub(crate) struct Plans(Mutex<Vec<(BindShape, Arc<BindingPlan>)>>);

impl Plans {
    pub(crate) fn get(&self, shape: &BindShape, make: impl FnOnce() -> Result<BindingPlan, Error>)
                      -> Result<Arc<BindingPlan>, Error> {
        {
            let plans = self.0.lock().unwrap_or_else(|p| p.into_inner());
            if let Some((_, p)) = plans.iter().find(|(s, _)| s == shape) {
                return Ok(p.clone());
            }
        }
        let plan = Arc::new(make()?);
        self.0.lock().unwrap_or_else(|p| p.into_inner()).push((shape.clone(), plan.clone()));
        Ok(plan)
    }
}

// ---- variants ----
//
// A pipeline state is its base compile (the empty `VariantKey`) plus the variants its draws and
// dispatches ask for. A plan of the base for a bind shape names the variant that shape needs
// (`BindingPlan::variant`, for a dispatch `dispatch_variant`): a texture bound in an 8-bit sRGB
// format whose raw codes the function reads, for one. The state compiles the pipeline again under
// that key when first asked (metal2vulkan-runtime's `variant`: the key's `lowering_inputs` per
// stage), keeps it by key, and the draw or dispatch binds the variant's own plan for the shape,
// which states the same key.

/// Clears in `key` the fields this runtime compiles and binds, leaving what it cannot. Each of
/// these changes only the functions' code and their layout's ordinary descriptors, which
/// `bind::resolve` binds:
/// - `srgb_textures`: an 8-bit sRGB slot read through a UNORM view of the same texture (its raw
///   codes, `WriteSource::Texture` with the view's format) and decoded with Metal's table;
/// - `address_maps`: a constexpr sampler's address modes mapped for the bound texture;
/// - `cross_class`: a texture read in its own numeric class;
/// - `storage_view_slots`: read-only buffers kept as storage views;
/// - `dropped_vertex_layer`: the vertex function's `Layer` output left out (a pass of one layer);
/// - `emulated_simdgroups`: a kernel's simdgroups emulated in its own threadgroup memory;
/// - `storage_images` whose choice only changes the function's code: the bound format itself (no
///   alias view, no RGBX twin), written as it is (`Identity`) or rounded to `half` as Metal stores
///   it by the shader (`HalfTowardZero`, `HalfQuietNan`); the plan's view of the bound texture is
///   bound as any storage image. Metal Performance Shaders' kernels write half-float images so
///   (PridePosterExtension, boot 251).
fn without_supported(key: &VariantKey) -> VariantKey {
    let mut rest = key.clone();
    rest.storage_images.retain(|_, c| {
        !(c.alias.is_none()
            && !c.x_channel
            && matches!(c.class, StorageImageClass::Identity | StorageImageClass::HalfTowardZero | StorageImageClass::HalfQuietNan))
    });
    rest.srgb_textures.clear();
    rest.address_maps.clear();
    rest.cross_class.clear();
    rest.storage_view_slots.clear();
    rest.dropped_vertex_layer = false;
    rest.emulated_simdgroups = false;
    rest
}

/// `key`'s non-empty fields, each as `name value`.
fn variant_fields(key: &VariantKey) -> Vec<String> {
    let mut out = Vec::new();
    let mut field = |set: bool, name: &str, value: &dyn std::fmt::Debug| {
        if set {
            out.push(format!("{name} {value:?}"));
        }
    };
    let k = key;
    field(!k.storage_images.is_empty(), "storage_images", &k.storage_images);
    field(!k.psb_slots.is_empty(), "psb_slots", &k.psb_slots);
    field(!k.pulled_vertex_buffers.is_empty(), "pulled_vertex_buffers", &k.pulled_vertex_buffers);
    field(k.emulated_simdgroups, "emulated_simdgroups", &true);
    field(k.dropped_vertex_layer, "dropped_vertex_layer", &true);
    field(!k.storage_view_slots.is_empty(), "storage_view_slots", &k.storage_view_slots);
    field(!k.srgb_textures.is_empty(), "srgb_textures", &k.srgb_textures);
    field(!k.ycbcr_textures.is_empty(), "ycbcr_textures", &k.ycbcr_textures);
    field(!k.address_maps.is_empty(), "address_maps", &k.address_maps);
    field(k.flat_grid, "flat_grid", &true);
    field(k.replay_bindings, "replay_bindings", &true);
    field(k.amplification_draws.is_some(), "amplification_draws", &k.amplification_draws);
    field(k.viewport_compensation.is_some(), "viewport_compensation", &k.viewport_compensation);
    field(k.folded_groups, "folded_groups", &true);
    field(!k.extended_range.is_empty(), "extended_range", &k.extended_range);
    field(!k.zeroed_colors.is_empty(), "zeroed_colors", &k.zeroed_colors);
    field(!k.dropped_colors.is_empty(), "dropped_colors", &k.dropped_colors);
    field(k.kept_imageblock_bytes.is_some(), "kept_imageblock_bytes", &k.kept_imageblock_bytes);
    field(k.depth_clamp_range, "depth_clamp_range", &true);
    field(!k.swizzled_storage.is_empty(), "swizzled_storage", &k.swizzled_storage);
    field(k.addressed_function_tables, "addressed_function_tables", &true);
    field(!k.interlocked_colors.is_empty(), "interlocked_colors", &k.interlocked_colors);
    field(k.read_only_tests, "read_only_tests", &true);
    field(k.shader_tests, "shader_tests", &true);
    field(k.shader_stencil, "shader_stencil", &true);
    field(k.window_depth_varying, "window_depth_varying", &true);
    field(!k.cubic_samplers.is_empty(), "cubic_samplers", &k.cubic_samplers);
    field(k.captured_positions.is_some(), "captured_positions", &k.captured_positions);
    field(!k.extended_shadows.is_empty(), "extended_shadows", &k.extended_shadows);
    field(!k.cross_class.is_empty(), "cross_class", &k.cross_class);
    field(!k.anisotropic_samplers.is_empty(), "anisotropic_samplers", &k.anisotropic_samplers);
    out
}

/// Refuses a variant key with a field this runtime cannot bind yet, naming each such field.
fn check_variant(key: &VariantKey) -> Result<(), Error> {
    let rest = without_supported(key);
    if rest.is_empty() {
        return Ok(());
    }
    let mut named = variant_fields(&rest);
    if named.is_empty() {
        named.push(format!("a field this runtime does not know ({rest:?})"));
    }
    Err(not_yet(format!("a pipeline variant with {}", named.join(", "))))
}

/// The key as its non-empty fields, for logs.
fn variant_label(key: &VariantKey) -> String {
    variant_fields(key).join(", ")
}

/// A state's variants by key, each compiled when first asked for.
pub(crate) struct Variants<V>(Mutex<Vec<(VariantKey, Arc<V>)>>);

impl<V> Default for Variants<V> {
    fn default() -> Self {
        Variants(Mutex::new(Vec::new()))
    }
}

impl<V> Variants<V> {
    /// The variant of `key`, compiled by `compile` if it is the first ask. Compiles hold the
    /// state's lock, so two draws of one state never compile one variant twice.
    fn get(&self, key: &VariantKey, compile: impl FnOnce() -> Result<V, Error>) -> Result<Arc<V>, Error> {
        let mut variants = self.0.lock().unwrap_or_else(|p| p.into_inner());
        if let Some((_, v)) = variants.iter().find(|(k, _)| k == key) {
            return Ok(v.clone());
        }
        check_variant(key)?;
        let began = Instant::now();
        let v = Arc::new(compile()?);
        crate::device::trace(format_args!(
            "pipeline variant compiled ({}) in {:.3} ms",
            variant_label(key),
            began.elapsed().as_secs_f64() * 1000.0
        ));
        variants.push((key.clone(), v.clone()));
        Ok(v)
    }
}

// ---- compute ----

/// One compiled form of a compute pipeline state: the translator's pipeline under `key`, its
/// kernel's program, and its plans.
pub(crate) struct ComputeVariant {
    pub(crate) key: VariantKey,
    pub(crate) pipeline: ComputePipeline,
    pub(crate) program: StageProgram,
    plans: Plans,
}

impl ComputeVariant {
    /// `descriptor`'s pipeline under `key`: the kernel compiled under the key's lowering inputs
    /// and the descriptor's stage input and call stack depth (as the translator compiles a
    /// `Function` kernel), then the pipeline around it.
    fn compile(device: &Device, descriptor: &ComputePipelineDescriptor, key: &VariantKey) -> Result<ComputeVariant, Error> {
        let t = device.translator();
        let budget = Budget::standard();
        let pipeline = if key.is_empty() {
            t.compile_compute_pipeline(descriptor, &budget)?
        } else {
            let StageSource::Function(function) = &descriptor.kernel else {
                return Err(not_yet("a variant of a compute pipeline whose kernel was compiled before"));
            };
            let mut inputs = key.lowering_inputs(Stage::Kernel);
            inputs.stage_input = descriptor.stage_input.clone();
            inputs.max_call_stack_depth =
                (descriptor.max_call_stack_depth > 1).then_some(descriptor.max_call_stack_depth);
            let compiled = t.compile_function(function, &inputs, &budget)?;
            let mut d = descriptor.clone();
            d.kernel = StageSource::Compiled(compiled);
            t.compile_compute_pipeline(&d, &budget)?
        };
        if pipeline.stage.stage != vk::ShaderStage::COMPUTE {
            return Err(not_yet(format!("a compute pipeline of stage {:?}", pipeline.stage.stage)));
        }
        let program = StageProgram::new(device, ffi::SG_COMPUTE, &pipeline.stage, &pipeline.layout)?;
        Ok(ComputeVariant { key: key.clone(), pipeline, program, plans: Plans::default() })
    }

    fn plan(&self, device: &Device, shape: &BindShape) -> Result<Arc<BindingPlan>, Error> {
        self.plans.get(shape, || Ok(self.pipeline.plan(device.translator(), shape)?))
    }
}

pub(crate) struct ComputeInner {
    pub(crate) device: Device,
    descriptor: ComputePipelineDescriptor,
    base: Arc<ComputeVariant>,
    variants: Variants<ComputeVariant>,
}

/// `MTLComputePipelineState`.
#[derive(Clone)]
pub struct ComputePipelineState(pub(crate) Arc<ComputeInner>);

impl Device {
    /// `newComputePipelineStateWithFunction:error:`.
    pub fn new_compute_pipeline_state_with_function(&self, function: &Function, options: &PipelineOptions)
                                                    -> Result<ComputePipelineState, Error> {
        let descriptor = ComputePipelineDescriptor::new(StageSource::Function(function.function_descriptor()));
        self.new_compute_pipeline_state(&descriptor, options)
    }

    /// `newComputePipelineStateWithDescriptor:options:reflection:error:`.
    pub fn new_compute_pipeline_state(&self, descriptor: &ComputePipelineDescriptor, _options: &PipelineOptions)
                                      -> Result<ComputePipelineState, Error> {
        let began = Instant::now();
        let base = ComputeVariant::compile(self, descriptor, &VariantKey::new())?;
        base.program.shader(self, &Vec::new())?;
        self.message(MessageSeverity::Info, "metal2vulkan-pipeline-creation",
                     format!("compute pipeline made in {:.3} ms", began.elapsed().as_secs_f64() * 1000.0));
        Ok(ComputePipelineState(Arc::new(ComputeInner {
            device: self.clone(),
            descriptor: descriptor.clone(),
            base: Arc::new(base),
            variants: Variants::default(),
        })))
    }
}

impl ComputePipelineState {
    pub fn max_total_threads_per_threadgroup(&self) -> u32 {
        self.0.base.pipeline.metal_info.max_total_threads_per_threadgroup
    }
    pub fn thread_execution_width(&self) -> u32 {
        self.0.base.pipeline.metal_info.thread_execution_width
    }
    pub fn static_threadgroup_memory_length(&self) -> u32 {
        self.0.base.pipeline.metal_info.static_threadgroup_memory_length
    }
    pub fn device(&self) -> Device {
        self.0.device.clone()
    }

    /// The variant a dispatch of `dispatch` bound as `shape` runs, and its plan for `shape`: the
    /// base and its plan, or the variant the base's plan names (`dispatch_variant`), compiled when
    /// first needed, with its own plan, which names the same variant.
    pub(crate) fn settle(&self, shape: &BindShape, dispatch: &Dispatch)
                         -> Result<(Arc<ComputeVariant>, Arc<BindingPlan>), Error> {
        let s = &self.0;
        let plan = s.base.plan(&s.device, shape)?;
        let wanted = plan.dispatch_variant(dispatch)?;
        if wanted == s.base.key {
            return Ok((s.base.clone(), plan));
        }
        let variant = s.variants.get(&wanted, || ComputeVariant::compile(&s.device, &s.descriptor, &wanted))?;
        let plan = variant.plan(&s.device, shape)?;
        let settled = plan.dispatch_variant(dispatch)?;
        if settled != variant.key {
            return Err(not_yet(format!(
                "a compute variant ({}) whose own plan asks for another ({})",
                variant_label(&variant.key),
                variant_label(&settled)
            )));
        }
        Ok((variant, plan))
    }
}

// ---- render ----

/// One compiled form of a render pipeline state: the translator's pipeline under `key`, its
/// stages' programs, and its plans.
pub(crate) struct RenderVariant {
    pub(crate) key: VariantKey,
    pub(crate) pipeline: RenderPipeline,
    pub(crate) vs: StageProgram,
    pub(crate) fs: Option<StageProgram>,
    plans: Plans,
}

impl RenderVariant {
    /// `descriptor`'s pipeline under `key` (`compile_render_variant`: each function stage under
    /// the key's lowering inputs of its stage), refused where it needs what the runtime lacks.
    fn compile(device: &Device, descriptor: &RenderPipelineDescriptor, key: &VariantKey) -> Result<RenderVariant, Error> {
        let pipeline = device.translator().compile_render_variant(descriptor, key, &Budget::standard())?;
        let g = &pipeline.graphics;
        if g.tessellation.is_some() {
            return Err(not_yet("tessellation pipelines"));
        }
        if g.mesh_records.is_some() {
            return Err(not_yet("mesh pipelines"));
        }
        if !g.storage_attachments.is_empty() {
            return Err(not_yet("attachments read as storage images"));
        }
        if g.fragment_imageblock.is_some() {
            return Err(not_yet("fragment imageblocks"));
        }
        if g.sample_shading {
            return Err(Error::Unsupported("sample-rate shading".into()));
        }
        let mut vs = None;
        let mut fs = None;
        for stage in &pipeline.stages {
            match stage.stage {
                vk::ShaderStage::VERTEX => vs = Some(StageProgram::new(device, ffi::SG_VERTEX, stage, &pipeline.layout)?),
                vk::ShaderStage::FRAGMENT => fs = Some(StageProgram::new(device, ffi::SG_FRAGMENT, stage, &pipeline.layout)?),
                other => return Err(not_yet(format!("a render stage {other:?}"))),
            }
        }
        let vs = vs.ok_or_else(|| invalid("a render pipeline without a vertex stage"))?;
        Ok(RenderVariant { key: key.clone(), pipeline, vs, fs, plans: Plans::default() })
    }

    fn plan(&self, device: &Device, shape: &BindShape) -> Result<Arc<BindingPlan>, Error> {
        self.plans.get(shape, || Ok(self.pipeline.plan(device.translator(), shape)?))
    }
}

pub(crate) struct RenderInner {
    pub(crate) device: Device,
    descriptor: RenderPipelineDescriptor,
    base: Arc<RenderVariant>,
    variants: Variants<RenderVariant>,
}

/// `MTLRenderPipelineState`.
#[derive(Clone)]
pub struct RenderPipelineState(pub(crate) Arc<RenderInner>);

impl Device {
    /// `newRenderPipelineStateWithDescriptor:options:reflection:error:`.
    pub fn new_render_pipeline_state(&self, descriptor: &RenderPipelineDescriptor, _options: &PipelineOptions)
                                     -> Result<RenderPipelineState, Error> {
        let began = Instant::now();
        let made = self.render_pipeline(descriptor);
        let ms = began.elapsed().as_secs_f64() * 1000.0;
        let verdict = if made.is_ok() { "made" } else { "failed" };
        self.message(MessageSeverity::Info, "metal2vulkan-pipeline-creation",
                     format!("render pipeline {verdict} in {ms:.3} ms"));
        made
    }

    fn render_pipeline(&self, descriptor: &RenderPipelineDescriptor) -> Result<RenderPipelineState, Error> {
        let base = RenderVariant::compile(self, descriptor, &VariantKey::new())?;
        base.vs.shader(self, &Vec::new())?;
        if let Some(fs) = &base.fs {
            fs.shader(self, &Vec::new())?;
        }
        Ok(RenderPipelineState(Arc::new(RenderInner {
            device: self.clone(),
            descriptor: descriptor.clone(),
            base: Arc::new(base),
            variants: Variants::default(),
        })))
    }
}

impl RenderPipelineState {
    pub fn device(&self) -> Device {
        self.0.device.clone()
    }

    /// The variant a draw bound as `shape` runs, and its plan for `shape`: the base and its plan,
    /// or the variant the base's plan names (`BindingPlan::variant`), compiled when first needed,
    /// with its own plan, which names the same variant.
    pub(crate) fn settle(&self, shape: &BindShape) -> Result<(Arc<RenderVariant>, Arc<BindingPlan>), Error> {
        let s = &self.0;
        let plan = s.base.plan(&s.device, shape)?;
        if plan.variant == s.base.key {
            return Ok((s.base.clone(), plan));
        }
        let wanted = plan.variant.clone();
        let variant = s.variants.get(&wanted, || RenderVariant::compile(&s.device, &s.descriptor, &wanted))?;
        let plan = variant.plan(&s.device, shape)?;
        if plan.variant != variant.key {
            return Err(not_yet(format!(
                "a render variant ({}) whose own plan asks for another ({})",
                variant_label(&variant.key),
                variant_label(&plan.variant)
            )));
        }
        Ok((variant, plan))
    }
}

// ---- samplers and depth/stencil ----

pub(crate) struct SgSampler {
    pub(crate) sg: *mut ffi::sg_sampler,
}

// SAFETY: as other shim objects.
unsafe impl Send for SgSampler {}
unsafe impl Sync for SgSampler {}

impl Drop for SgSampler {
    fn drop(&mut self) {
        // SAFETY: from sg_sampler_new, the last reference.
        unsafe { ffi::sg_sampler_free(self.sg) }
    }
}

pub(crate) struct SamplerInner {
    pub(crate) device: Device,
    pub(crate) desc: SamplerDescriptor,
    variants: Mutex<Vec<((bool, bool), Arc<SgSampler>)>>,
}

/// `MTLSamplerState`. Its Gallium sampler is made per use the binder states (depth compare,
/// integer border).
#[derive(Clone)]
pub struct SamplerState(pub(crate) Arc<SamplerInner>);

impl Device {
    pub fn new_sampler_state(&self, descriptor: &SamplerDescriptor) -> Result<SamplerState, Error> {
        let s = SamplerState(Arc::new(SamplerInner { device: self.clone(), desc: *descriptor, variants: Mutex::new(Vec::new()) }));
        s.sg(false, false)?;
        Ok(s)
    }
}

impl SamplerState {
    pub(crate) fn sg(&self, compare: bool, integer: bool) -> Result<Arc<SgSampler>, Error> {
        let mut v = self.0.variants.lock().unwrap_or_else(|p| p.into_inner());
        if let Some((_, s)) = v.iter().find(|(k, _)| *k == (compare, integer)) {
            return Ok(s.clone());
        }
        let d = &self.0.desc;
        let desc = ffi::sg_sampler_desc {
            min_filter: d.min_filter.as_raw(),
            mag_filter: d.mag_filter.as_raw(),
            mip_filter: d.mip_filter.as_raw(),
            s_address: d.s_address_mode.as_raw(),
            t_address: d.t_address_mode.as_raw(),
            r_address: d.r_address_mode.as_raw(),
            border: d.border_color.as_raw(),
            normalized: d.normalized_coordinates as u32,
            max_anisotropy: d.max_anisotropy,
            compare: if compare { d.compare_function.as_raw() } else { 0 },
            compare_enable: compare as u32,
            integer_border: integer as u32,
            lod_min: d.lod_min_clamp,
            lod_max: d.lod_max_clamp,
        };
        // SAFETY: valid device and descriptor.
        let sg = unsafe { ffi::sg_sampler_new(self.0.device.gpu().sg, &desc) };
        if sg.is_null() {
            return Err(gallium("sampler"));
        }
        let s = Arc::new(SgSampler { sg });
        v.push(((compare, integer), s.clone()));
        Ok(s)
    }
}

/// A device-wide sampler for neutral bindings.
pub(crate) fn neutral_sampler(device: &Device) -> Result<Arc<SgSampler>, Error> {
    static NEUTRAL: Mutex<Vec<(usize, SamplerState)>> = Mutex::new(Vec::new());
    let key = Arc::as_ptr(&device.0) as usize;
    let mut n = NEUTRAL.lock().unwrap_or_else(|p| p.into_inner());
    if let Some((_, s)) = n.iter().find(|(k, _)| *k == key) {
        return s.sg(false, false);
    }
    let s = device.new_sampler_state(&SamplerDescriptor::new())?;
    n.push((key, s.clone()));
    s.sg(false, false)
}

/// `MTLDepthStencilState`.
#[derive(Clone)]
pub struct DepthStencilState(pub(crate) Arc<(Device, DepthStencilDescriptor)>);

impl Device {
    pub fn new_depth_stencil_state(&self, descriptor: &DepthStencilDescriptor) -> Result<DepthStencilState, Error> {
        Ok(DepthStencilState(Arc::new((self.clone(), *descriptor))))
    }
}

impl DepthStencilState {
    pub(crate) fn descriptor(&self) -> &DepthStencilDescriptor {
        &self.0 .1
    }
}

#[allow(dead_code)]
pub(crate) fn keep(_: &SgView) {}
