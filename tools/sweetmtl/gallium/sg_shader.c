/*
 * sweetmtl Gallium shim: the metal2vulkan translator's SPIR-V as Gallium CSOs for freedreno a5xx.
 *
 * The NIR pipeline is the one M0 qualified on all 232 CoreAnimation stages
 * (tools/sweetmtl/m0/spv2ir3.c): vk_spirv_to_nir, tu_spirv_to_nir and a bindful subset of
 * tu_lower_nir, then the screen's finalize_nir. Two things differ:
 *
 * - Descriptors go to the Gallium slots the runtime assigned (struct sg_binding), not to slots
 *   in order of first use, so the runtime binds each descriptor write where the shader reads it.
 *   Push constants are constant buffer 0 (the stage's push block).
 * - Read-only storage buffers of the vertex stage are read as constant buffers
 *   (SG_CLASS_SSBO_AS_UBO): a5xx emits shader-buffer state for the fragment and compute stages
 *   only, and its constant-buffer loads are plain ldg from the buffer's address, unbounded in size.
 *
 * Input attachments (framebuffer fetch) become texel fetches at the fragment coordinate of the
 * view bound at their slot: the attachment itself, behind a texture barrier between draws.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sg_priv.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "cso_cache/cso_context.h"
#include "util/set.h"

#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "ir3/ir3_gallium.h"
#include "freedreno_screen.h"

struct lower_state {
   const struct sg_shader_desc *desc;
   const char *error;
};

static const struct sg_binding *
find_binding(const struct sg_shader_desc *d, unsigned set, unsigned binding)
{
   for (uint32_t i = 0; i < d->binding_count; i++)
      if (d->bindings[i].set == set && d->bindings[i].binding == binding)
         return &d->bindings[i];
   return NULL;
}

/* ---- vertex-stage storage buffers as constant buffers ---- */

static bool
ssbo_to_ubo(nir_shader *nir, const struct sg_shader_desc *d, const char **error)
{
   bool any = false;
   for (uint32_t i = 0; i < d->binding_count; i++)
      any |= d->bindings[i].cls == SG_CLASS_SSBO_AS_UBO;
   if (!any)
      return false;
   struct set *defs = _mesa_pointer_set_create(NULL);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_deref) {
               nir_deref_instr *deref = nir_instr_as_deref(instr);
               if (deref->deref_type != nir_deref_type_var &&
                   _mesa_set_search(defs, deref->parent.ssa)) {
                  deref->modes = nir_var_mem_ubo;
                  _mesa_set_add(defs, &deref->def);
               }
               continue;
            }
            if (instr->type == nir_instr_type_phi) {
               nir_phi_instr *phi = nir_instr_as_phi(instr);
               nir_foreach_phi_src(src, phi) {
                  if (_mesa_set_search(defs, src->src.ssa)) {
                     *error = "a pointer to a vertex-stage storage buffer through a phi";
                     _mesa_set_destroy(defs, NULL);
                     return false;
                  }
               }
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_vulkan_resource_index: {
               const struct sg_binding *b =
                  find_binding(d, nir_intrinsic_desc_set(intr), nir_intrinsic_binding(intr));
               if (b && b->cls == SG_CLASS_SSBO_AS_UBO &&
                   nir_intrinsic_desc_type(intr) == nir_descriptor_type_storage_buffer) {
                  nir_intrinsic_set_desc_type(intr, nir_descriptor_type_uniform_buffer);
                  _mesa_set_add(defs, &intr->def);
               }
               break;
            }
            case nir_intrinsic_vulkan_resource_reindex:
            case nir_intrinsic_load_vulkan_descriptor:
               if (_mesa_set_search(defs, intr->src[0].ssa)) {
                  nir_intrinsic_set_desc_type(intr, nir_descriptor_type_uniform_buffer);
                  _mesa_set_add(defs, &intr->def);
               }
               break;
            case nir_intrinsic_store_deref:
            case nir_intrinsic_deref_atomic:
            case nir_intrinsic_deref_atomic_swap:
            case nir_intrinsic_get_ssbo_size:
               if (_mesa_set_search(defs, intr->src[0].ssa)) {
                  *error = "a vertex-stage storage buffer is written or sized (a5xx has no "
                           "vertex-stage shader buffers)";
                  _mesa_set_destroy(defs, NULL);
                  return false;
               }
               break;
            case nir_intrinsic_deref_buffer_array_length:
               if (_mesa_set_search(defs, intr->src[0].ssa)) {
                  *error = "the length of a vertex-stage storage buffer array (not yet)";
                  _mesa_set_destroy(defs, NULL);
                  return false;
               }
               break;
            default:
               break;
            }
         }
      }
   }
   _mesa_set_destroy(defs, NULL);
   return true;
}

/* ---- descriptors to Gallium slots ---- */

static bool
lower_resource(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct lower_state *s = data;
   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      const struct sg_binding *bd =
         find_binding(s->desc, nir_intrinsic_desc_set(intr), nir_intrinsic_binding(intr));
      if (!bd) {
         s->error = "a buffer descriptor the layout does not name";
         return false;
      }
      bool ssbo = nir_intrinsic_desc_type(intr) == nir_descriptor_type_storage_buffer;
      if (ssbo != (bd->cls == SG_CLASS_SSBO)) {
         s->error = "a buffer descriptor of another class than its binding";
         return false;
      }
      nir_def *idx = nir_iadd_imm(b, intr->src[0].ssa, bd->slot);
      nir_def *comps[4] = {idx};
      for (unsigned i = 1; i < intr->def.num_components; i++)
         comps[i] = nir_imm_intN_t(b, 0, intr->def.bit_size);
      nir_def_replace(&intr->def, nir_vec(b, comps, intr->def.num_components));
      return true;
   }
   case nir_intrinsic_vulkan_resource_reindex: {
      nir_def *a = intr->src[0].ssa;
      nir_def *comps[4];
      comps[0] = nir_iadd(b, nir_channel(b, a, 0), intr->src[1].ssa);
      for (unsigned i = 1; i < intr->def.num_components; i++)
         comps[i] = nir_channel(b, a, i);
      nir_def_replace(&intr->def, nir_vec(b, comps, intr->def.num_components));
      return true;
   }
   case nir_intrinsic_load_vulkan_descriptor:
      nir_def_replace(&intr->def, intr->src[0].ssa);
      return true;
   case nir_intrinsic_load_push_constant: {
      nir_def *off = nir_iadd_imm(b, intr->src[0].ssa, nir_intrinsic_base(intr));
      nir_def *v = nir_load_ubo(b, intr->def.num_components, intr->def.bit_size,
                                nir_imm_int(b, 0), off, .align_mul = 4, .align_offset = 0,
                                .range_base = 0, .range = ~0);
      nir_def_replace(&intr->def, v);
      return true;
   }
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
   case nir_intrinsic_image_deref_sparse_load: {
      nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(d);
      const struct sg_binding *bd =
         find_binding(s->desc, var->data.descriptor_set, var->data.binding);
      if (!bd || bd->cls != SG_CLASS_IMAGE) {
         s->error = "a storage image the layout does not name";
         return false;
      }
      nir_def *idx = nir_imm_int(b, bd->slot);
      if (d->deref_type == nir_deref_type_array)
         idx = nir_iadd(b, idx, d->arr.index.ssa);
      nir_rewrite_image_intrinsic(intr, idx, nir_image_intrinsic_type_default);
      /* the index-only form has a RANGE_BASE the deref form lacks: its slot keeps whatever index
       * the deref form had there (image_deref_store's SRC_TYPE, 160 for float32), and backends
       * add it to the image index (nir_to_tgsi: STORE IMAGE[160]). The slot is in idx already. */
      if (nir_intrinsic_has_range_base(intr))
         nir_intrinsic_set_range_base(intr, 0);
      return true;
   }
   default:
      return false;
   }
}

/* Index sources of the buffer intrinsics: the vector the address format leaves, take its index. */
static bool
scalar_index(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   int src;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_get_ssbo_size:
      src = 0;
      break;
   case nir_intrinsic_store_ssbo:
      src = 1;
      break;
   default:
      return false;
   }
   if (intr->src[src].ssa->num_components == 1)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[src], nir_channel(b, intr->src[src].ssa, 0));
   return true;
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex, void *data)
{
   struct lower_state *s = data;
   bool progress = false;
   bool have_sampler = false;
   b->cursor = nir_before_instr(&tex->instr);
   for (int which = 0; which < 2; which++) {
      nir_tex_src_type t = which ? nir_tex_src_sampler_deref : nir_tex_src_texture_deref;
      nir_deref_instr *d = nir_steal_tex_deref(tex, t);
      if (!d)
         continue;
      nir_variable *var = nir_deref_instr_get_variable(d);
      const struct sg_binding *bd =
         find_binding(s->desc, var->data.descriptor_set, var->data.binding);
      if (!bd) {
         s->error = "a texture or sampler the layout does not name";
         return false;
      }
      unsigned base;
      if (which) {
         if (bd->cls != SG_CLASS_SAMPLER && bd->cls != SG_CLASS_COMBINED) {
            s->error = "a sampler descriptor of another class";
            return false;
         }
         base = bd->sampler_slot;
         have_sampler = true;
      } else {
         if (bd->cls != SG_CLASS_TEXTURE && bd->cls != SG_CLASS_COMBINED &&
             bd->cls != SG_CLASS_TEXEL_BUFFER && bd->cls != SG_CLASS_INPUT_ATTACHMENT) {
            s->error = "a texture descriptor of another class";
            return false;
         }
         base = bd->slot;
         if (bd->cls == SG_CLASS_COMBINED && nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref) < 0) {
            /* a combined descriptor read through its image deref alone: its own sampler */
            tex->sampler_index = bd->sampler_slot;
            have_sampler = true;
         }
      }
      if (d->deref_type == nir_deref_type_array) {
         if (nir_src_is_const(d->arr.index))
            base += nir_src_as_uint(d->arr.index);
         else
            nir_tex_instr_add_src(tex,
                                  which ? nir_tex_src_sampler_offset : nir_tex_src_texture_offset,
                                  d->arr.index.ssa);
      }
      if (which)
         tex->sampler_index = base;
      else
         tex->texture_index = base;
      progress = true;
   }
   /* A texture read without a sampler (txf, a subpass input) uses its texture's slot. */
   if (progress && !have_sampler)
      tex->sampler_index = tex->texture_index;
   return progress;
}

static void
count_slots(const struct sg_shader_desc *d, nir_shader *nir)
{
   unsigned ubos = 1, ssbos = 0, tex = 0, samplers = 0, images = 0;
   for (uint32_t i = 0; i < d->binding_count; i++) {
      const struct sg_binding *bd = &d->bindings[i];
      unsigned n = bd->count ? bd->count : 1;
      switch (bd->cls) {
      case SG_CLASS_UBO:
      case SG_CLASS_SSBO_AS_UBO:
         ubos = MAX2(ubos, bd->slot + n);
         break;
      case SG_CLASS_SSBO:
         ssbos = MAX2(ssbos, bd->slot + n);
         break;
      case SG_CLASS_TEXTURE:
      case SG_CLASS_TEXEL_BUFFER:
      case SG_CLASS_INPUT_ATTACHMENT:
         tex = MAX2(tex, bd->slot + n);
         break;
      case SG_CLASS_SAMPLER:
         samplers = MAX2(samplers, bd->sampler_slot + n);
         break;
      case SG_CLASS_COMBINED:
         tex = MAX2(tex, bd->slot + n);
         samplers = MAX2(samplers, bd->sampler_slot + n);
         break;
      case SG_CLASS_IMAGE:
         images = MAX2(images, bd->slot + n);
         break;
      }
   }
   nir->info.num_ubos = ubos;
   nir->info.num_ssbos = ssbos;
   nir->info.num_textures = MAX2(tex, samplers);
   nir->info.num_images = images;
}

/* The vertex stage's attribute locations, in driver-location order (the vertex elements'). */
static void
vertex_inputs(nir_shader *nir, struct sg_shader_info *info)
{
   int locations[32];
   unsigned n = 0;
   for (unsigned i = 0; i < 32; i++)
      locations[i] = -1;
   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.location < VERT_ATTRIB_GENERIC0 || var->data.driver_location >= 32)
         continue;
      locations[var->data.driver_location] = var->data.location - VERT_ATTRIB_GENERIC0;
      n = MAX2(n, var->data.driver_location + 1);
   }
   info->inputs = n;
   for (unsigned i = 0; i < n; i++)
      info->input_locations[i] = locations[i] < 0 ? 0xffffffffu : (uint32_t)locations[i];
}

/* Whether the shader reaches memory by 64-bit address (PhysicalStorageBuffer pointers): ir3 has
 * no load/store_global for a5xx (ir3_a4xx.c leaves them NULL), so such a shader is refused here
 * instead of crashing the compiler. */
static bool
uses_global_memory(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_load_global:
            case nir_intrinsic_load_global_constant:
            case nir_intrinsic_store_global:
            case nir_intrinsic_global_atomic:
            case nir_intrinsic_global_atomic_swap:
               return true;
            default:
               break;
            }
         }
      }
   }
   return false;
}

static const struct spirv_capabilities *
sg_spirv_caps(void)
{
   static struct spirv_capabilities caps;
   static bool init;
   if (!init) {
      memset(&caps, 0, sizeof(caps));
      caps.Shader = caps.Float16 = caps.Int16 = caps.Int8 = caps.ImageQuery = true;
      caps.InputAttachment = caps.StorageBuffer16BitAccess = caps.StorageBuffer8BitAccess = true;
      caps.UniformAndStorageBuffer16BitAccess = caps.UniformAndStorageBuffer8BitAccess = true;
      caps.StoragePushConstant16 = caps.StoragePushConstant8 = true;
      caps.StorageImageWriteWithoutFormat = caps.StorageImageReadWithoutFormat = true;
      caps.SampledBuffer = caps.ImageBuffer = caps.StorageImageExtendedFormats = true;
      caps.DerivativeControl = caps.SampleRateShading = caps.ImageGatherExtended = true;
      caps.ClipDistance = caps.Sampled1D = caps.Image1D = caps.SampledCubeArray = true;
      caps.ImageCubeArray = caps.MinLod = caps.Int64 = caps.GroupNonUniform = true;
      caps.GroupNonUniformBallot = caps.GroupNonUniformVote = true;
      caps.GroupNonUniformShuffle = caps.GroupNonUniformArithmetic = true;
      caps.GroupNonUniformQuad = caps.VariablePointers = caps.VariablePointersStorageBuffer = true;
      caps.PhysicalStorageBufferAddresses = true;
      init = true;
   }
   return &caps;
}

/* softpipe (the PC's validation driver): the generic optimization loop ir3_optimize_loop stands
 * for */
static void
generic_optimize(nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_if, 0);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_undef);
   } while (progress);
}

static nir_shader *
translate(struct sg_device *dev, const struct sg_shader_desc *d, struct sg_shader_info *info)
{
   struct fd_screen *fds = dev->freedreno ? fd_screen(dev->screen) : NULL;
   mesa_shader_stage stage = sg_mesa_stage(d->stage);
   const nir_shader_compiler_options *nopts =
      fds ? ir3_get_compiler_options(fds->compiler) : dev->screen->nir_options[stage];

   struct spirv_to_nir_options sopts = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = sg_spirv_caps(),
      .mediump_16bit_alu = fds != NULL,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 64,
      .min_ssbo_alignment = 4,
   };
   struct nir_spirv_specialization_entry entries[64];
   struct nir_spirv_specialization spec = {.num_entries = 0, .entries = entries};
   if (d->spec_count > ARRAY_SIZE(entries)) {
      sg_set_error("%u specialization constants (at most 64 here)", d->spec_count);
      return NULL;
   }
   for (uint32_t i = 0; i < d->spec_count; i++) {
      entries[i].id = d->spec[i].id;
      entries[i].size = d->spec[i].size;
      entries[i].data = (uint8_t *)d->spec[i].data;
      entries[i].defined_on_module = false;
   }
   spec.num_entries = d->spec_count;

   nir_shader *nir = spirv_to_nir(d->words, d->word_count, d->spec_count ? &spec : NULL, stage,
                                  d->entry ? d->entry : "main", &sopts, nopts);
   if (!nir) {
      sg_set_error("spirv_to_nir failed");
      return NULL;
   }

   /* vk_spirv_to_nir */
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);

   const char *error = NULL;
   ssbo_to_ubo(nir, d, &error);
   if (error) {
      sg_set_error("%s", error);
      ralloc_free(nir);
      return NULL;
   }

   /* tu_spirv_to_nir */
   nir->info.num_ubos = 0;
   nir->info.num_ssbos = 0;
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_opt_find_array_copies);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_dce);
   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_lower_sysvals_to_varyings_options sv = {.primitive_id = true};
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &sv);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_mediump_vars,
            nir_var_function_temp | nir_var_shader_temp | nir_var_mem_shared);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_combine_stores, nir_var_all);
   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_is_helper_invocation);
   if (fds) {
      struct ir3_optimize_options oo = {0};
      ir3_optimize_loop(fds->compiler, &oo, nir);
   } else {
      generic_optimize(nir);
   }

   /* tu_lower_nir, bindful */
   const nir_opt_access_options ao = {.is_vulkan = true};
   NIR_PASS(_, nir, nir_opt_access, &ao);
   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_input_attachment_options ia = {0};
      NIR_PASS(_, nir, nir_lower_input_attachments, &ia);
      const nir_lower_sysvals_to_varyings_options sv = {.point_coord = true};
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &sv);
   }
   if (fds)
      ir3_nir_lower_io_vars_to_temporaries(nir);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global, nir_address_format_64bit_global);
   if (uses_global_memory(nir)) {
      sg_set_error("buffer-device-address (pointer) loads or stores: ir3 has no global memory "
                   "ops for a5xx yet (plan blocker 1, an ldg/stg patch)");
      ralloc_free(nir);
      return NULL;
   }
   if (stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               glsl_get_natural_size_align_bytes);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
               nir_address_format_32bit_offset);
      const struct nir_lower_compute_system_values_options cs = {0};
      NIR_PASS(_, nir, nir_lower_compute_system_values, &cs);
   }

   struct lower_state ls = {.desc = d};
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_resource, nir_metadata_control_flow, &ls);
   if (!ls.error)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, scalar_index, nir_metadata_control_flow, NULL);
   if (!ls.error)
      NIR_PASS(_, nir, nir_shader_tex_pass, lower_tex, nir_metadata_control_flow, &ls);
   if (ls.error) {
      sg_set_error("%s", ls.error);
      ralloc_free(nir);
      return NULL;
   }
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_uniform | nir_var_image | nir_var_mem_ubo | nir_var_mem_ssbo |
               nir_var_mem_push_const,
            NULL);
   count_slots(d, nir);

   nir_assign_io_var_locations(nir, nir_var_shader_in);
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   if (stage == MESA_SHADER_VERTEX)
      vertex_inputs(nir, info);
   if (fds) {
      ir3_nir_lower_io(nir);
   } else {
      generic_optimize(nir);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   }
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (getenv("SG_PRINT_NIR"))
      nir_print_shader(nir, stderr);
   /* ir3_screen_finalize_nir: what the state tracker runs before making a CSO (softpipe has
    * none: nir_to_tgsi lowers what it needs) */
   if (dev->screen->finalize_nir)
      dev->screen->finalize_nir(dev->screen, nir, true);
   if (stage == MESA_SHADER_COMPUTE) {
      info->workgroup[0] = nir->info.workgroup_size[0];
      info->workgroup[1] = nir->info.workgroup_size[1];
      info->workgroup[2] = nir->info.workgroup_size[2];
      info->shared_bytes = nir->info.shared_size;
   }
   return nir;
}

struct sg_shader *
sg_shader_new(struct sg_device *dev, const struct sg_shader_desc *d)
{
   struct sg_shader_info info;
   memset(&info, 0, sizeof(info));
   info.stage = d->stage;
   nir_shader *nir = translate(dev, d, &info);
   if (!nir)
      return NULL;
   if (d->stage == SG_COMPUTE && nir->info.workgroup_size_variable) {
      sg_set_error("a kernel without a specialized local size");
      ralloc_free(nir);
      return NULL;
   }

   void *cso;
   sg_set_error("ir3 compile failed");
   if (d->stage == SG_COMPUTE) {
      struct pipe_compute_state pcs = {
         .ir_type = PIPE_SHADER_IR_NIR,
         .prog = nir,
         .static_shared_mem = nir->info.shared_size,
      };
      cso = dev->pipe->create_compute_state(dev->pipe, &pcs);
   } else {
      struct pipe_shader_state pss;
      memset(&pss, 0, sizeof(pss));
      pss.type = PIPE_SHADER_IR_NIR;
      pss.ir.nir = nir;
      cso = d->stage == SG_VERTEX ? dev->pipe->create_vs_state(dev->pipe, &pss)
                                  : dev->pipe->create_fs_state(dev->pipe, &pss);
   }
   if (!cso)
      return NULL;
   if (!dev->freedreno) {
      struct sg_shader *s = calloc(1, sizeof(*s));
      s->dev = dev;
      s->stage = d->stage;
      s->cso = cso;
      s->info = info;
      dev->stats.shaders++;
      return s;
   }
   /* The default variant (compiled synchronously at creation: sg_device.c's debug callback). */
   struct ir3_shader *shader = ir3_get_shader(cso);
   struct ir3_shader_key key;
   memset(&key, 0, sizeof(key));
   struct ir3_shader_variant *v = shader ? ir3_shader_variant(shader, key, false, NULL) : NULL;
   if (!v || !v->bin) {
      if (d->stage == SG_COMPUTE)
         dev->pipe->delete_compute_state(dev->pipe, cso);
      else if (d->stage == SG_VERTEX)
         dev->pipe->delete_vs_state(dev->pipe, cso);
      else
         dev->pipe->delete_fs_state(dev->pipe, cso);
      return NULL;
   }
   info.instrs = v->info.instrs_count;
   info.full_regs = v->info.max_reg + 1;
   info.half_regs = v->info.max_half_reg + 1;
   info.constlen = v->constlen;
   if (d->stage == SG_COMPUTE && !info.workgroup[0]) {
      info.workgroup[0] = shader->nir->info.workgroup_size[0];
      info.workgroup[1] = shader->nir->info.workgroup_size[1];
      info.workgroup[2] = shader->nir->info.workgroup_size[2];
   }
   dev->stats.shaders++;
   dev->stats.shader_instrs += info.instrs;

   struct sg_shader *s = calloc(1, sizeof(*s));
   s->dev = dev;
   s->stage = d->stage;
   s->cso = cso;
   s->info = info;
   return s;
}

void
sg_shader_free(struct sg_shader *s)
{
   if (!s)
      return;
   struct sg_device *dev = s->dev;
   simple_mtx_lock(&dev->lock);
   /* unbind first: a CSO is not deleted while bound (the next command binds its own) */
   if (s->stage == SG_COMPUTE) {
      cso_set_compute_shader_handle(dev->cso, NULL);
      dev->pipe->delete_compute_state(dev->pipe, s->cso);
   } else if (s->stage == SG_VERTEX) {
      cso_set_vertex_shader_handle(dev->cso, NULL);
      dev->pipe->delete_vs_state(dev->pipe, s->cso);
   } else {
      cso_set_fragment_shader_handle(dev->cso, NULL);
      dev->pipe->delete_fs_state(dev->pipe, s->cso);
   }
   simple_mtx_unlock(&dev->lock);
   free(s);
}

void
sg_shader_info(struct sg_shader *shader, struct sg_shader_info *out)
{
   *out = shader->info;
}
