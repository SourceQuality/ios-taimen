/*
 * spv2ir3: compile a metal2vulkan SPIR-V stage with ir3 for an a5xx GPU the way the
 * Gallium driver would (bindful slots, no bindless): the M0 check of the taimen Metal plan.
 *
 *   spv2ir3 [--gpu 540] [-q] vert|frag|comp file.spv
 *
 * Descriptors are flattened per shader in (set, binding) order: push constants become
 * constant buffer 0 (Gallium's user constants), uniform buffers 1.., storage buffers 0..,
 * sampled/combined images and input attachments texture slots 0.., storage images image
 * slots 0... Input attachments read the texel under the fragment (nir_lower_input_attachments),
 * the "texture barrier" emulation of framebuffer fetch. Prints the disassembly and a summary
 * line "OK stage instrs=N half=N full=N constlen=N" or "FAIL ...".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vulkan/vulkan_core.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "common/freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"

#define MAXB 256

struct slots {
   int n;
   struct { unsigned set, binding, cls, base, size; } b[MAXB];
   unsigned next[4];
};
enum { C_UBO, C_SSBO, C_TEX, C_IMG };

static unsigned
slot_of(struct slots *s, unsigned set, unsigned binding, unsigned cls, unsigned size)
{
   for (int i = 0; i < s->n; i++)
      if (s->b[i].set == set && s->b[i].binding == binding)
         return s->b[i].base;
   if (s->n == MAXB)
      abort();
   unsigned base = s->next[cls];
   s->next[cls] += size ? size : 1;
   s->b[s->n].set = set;
   s->b[s->n].binding = binding;
   s->b[s->n].cls = cls;
   s->b[s->n].base = base;
   s->b[s->n].size = size;
   s->n++;
   return base;
}

static bool
lower_resource(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct slots *s = data;
   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      unsigned dt = nir_intrinsic_desc_type(intr);
      bool ssbo = dt == nir_descriptor_type_storage_buffer;
      unsigned base = slot_of(s, nir_intrinsic_desc_set(intr), nir_intrinsic_binding(intr),
                              ssbo ? C_SSBO : C_UBO, 1);
      if (!ssbo)
         base += 1; /* constant buffer 0 holds the push constants */
      nir_def *idx = nir_iadd_imm(b, intr->src[0].ssa, base);
      nir_def *comps[4] = { idx };
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
      unsigned size = glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;
      unsigned base = slot_of(s, var->data.descriptor_set, var->data.binding, C_IMG, size);
      nir_def *idx = nir_imm_int(b, base);
      if (d->deref_type == nir_deref_type_array)
         idx = nir_iadd(b, idx, d->arr.index.ssa);
      nir_rewrite_image_intrinsic(intr, idx, nir_image_intrinsic_type_default);
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
   struct slots *s = data;
   bool progress = false;
   b->cursor = nir_before_instr(&tex->instr);
   for (int which = 0; which < 2; which++) {
      nir_tex_src_type t = which ? nir_tex_src_sampler_deref : nir_tex_src_texture_deref;
      nir_deref_instr *d = nir_steal_tex_deref(tex, t);
      if (!d)
         continue;
      nir_variable *var = nir_deref_instr_get_variable(d);
      unsigned size = glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;
      unsigned base = slot_of(s, var->data.descriptor_set, var->data.binding, C_TEX, size);
      if (d->deref_type == nir_deref_type_array) {
         if (nir_src_is_const(d->arr.index))
            base += nir_src_as_uint(d->arr.index);
         else
            nir_tex_instr_add_src(tex, which ? nir_tex_src_sampler_offset : nir_tex_src_texture_offset,
                                  d->arr.index.ssa);
      }
      if (which)
         tex->sampler_index = base;
      else
         tex->texture_index = base;
      progress = true;
   }
   /* A texture read without a sampler (txf, a subpass input) uses its texture's slot. */
   if (progress && nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref) < 0 &&
       tex->sampler_index == 0)
      tex->sampler_index = tex->texture_index;
   return progress;
}

static uint32_t *
slurp(const char *path, size_t *words)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc(n);
   if (fread(w, 1, n, f) != (size_t)n)
      exit(2);
   fclose(f);
   *words = n / 4;
   return w;
}

int
main(int argc, char **argv)
{
   unsigned gpu = 540;
   bool quiet = false;
   int i = 1;
   for (; i < argc && argv[i][0] == '-'; i++) {
      if (!strcmp(argv[i], "--gpu") && i + 1 < argc)
         gpu = atoi(argv[++i]);
      else if (!strcmp(argv[i], "-q"))
         quiet = true;
   }
   if (argc - i != 2) {
      fprintf(stderr, "usage: spv2ir3 [--gpu 540] [-q] vert|frag|comp file.spv\n");
      return 2;
   }
   mesa_shader_stage stage = !strcmp(argv[i], "vert")   ? MESA_SHADER_VERTEX
                             : !strcmp(argv[i], "frag") ? MESA_SHADER_FRAGMENT
                                                        : MESA_SHADER_COMPUTE;
   size_t nwords;
   uint32_t *words = slurp(argv[i + 1], &nwords);

   glsl_type_singleton_init_or_ref();
   struct fd_dev_id dev_id = { .gpu_id = gpu };
   const struct fd_dev_info *info = fd_dev_info_raw(&dev_id);
   if (!info) {
      fprintf(stderr, "no dev info for gpu %u\n", gpu);
      return 2;
   }
   struct ir3_compiler_options copts = { .disable_cache = true,
                                         .bindless_fb_read_descriptor = -1,
                                         .bindless_fb_read_slot = -1 };
   struct ir3_compiler *compiler = ir3_compiler_create(NULL, &dev_id, info, &copts);
   const nir_shader_compiler_options *nopts = ir3_get_compiler_options(compiler);

   struct spirv_capabilities caps;
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

   struct spirv_to_nir_options sopts = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &caps,
      .mediump_16bit_alu = true,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 64,
      .min_ssbo_alignment = 4,
   };
   nir_shader *nir = spirv_to_nir(words, nwords, NULL, stage, "main", &sopts, nopts);
   if (!nir) {
      printf("FAIL %s spirv_to_nir\n", argv[i]);
      return 1;
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

   /* tu_spirv_to_nir */
   nir->info.num_ubos = 0;
   nir->info.num_ssbos = 0;
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_opt_find_array_copies);
   NIR_PASS(_, nir, nir_opt_copy_prop_vars);
   NIR_PASS(_, nir, nir_opt_dce);
   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_lower_sysvals_to_varyings_options sv = { .primitive_id = true };
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
   struct ir3_optimize_options oo = {};
   ir3_optimize_loop(compiler, &oo, nir);

   /* tu_lower_nir, bindful */
   const nir_opt_access_options ao = { .is_vulkan = true };
   NIR_PASS(_, nir, nir_opt_access, &ao);
   if (stage == MESA_SHADER_FRAGMENT) {
      const nir_input_attachment_options ia = { 0 };
      NIR_PASS(_, nir, nir_lower_input_attachments, &ia);
      const nir_lower_sysvals_to_varyings_options sv = { .point_coord = true };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &sv);
   }
   ir3_nir_lower_io_vars_to_temporaries(nir);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   if (stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               glsl_get_natural_size_align_bytes);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
               nir_address_format_32bit_offset);
      const struct nir_lower_compute_system_values_options cs = { 0 };
      NIR_PASS(_, nir, nir_lower_compute_system_values, &cs);
   }

   struct slots s;
   memset(&s, 0, sizeof(s));
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_resource, nir_metadata_control_flow, &s);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, scalar_index, nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_shader_tex_pass, lower_tex, nir_metadata_control_flow, &s);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_uniform | nir_var_image | nir_var_mem_ubo | nir_var_mem_ssbo |
               nir_var_mem_push_const,
            NULL);
   nir->info.num_ubos = s.next[C_UBO] + 1;
   nir->info.num_ssbos = s.next[C_SSBO];
   nir->info.num_textures = s.next[C_TEX];
   nir->info.num_images = s.next[C_IMG];

   nir_assign_io_var_locations(nir, nir_var_shader_in);
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   ir3_nir_lower_io(nir);

   /* ir3_screen_finalize_nir, then the Gallium shader state */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (getenv("SPV2IR3_PRE"))
      nir_print_shader(nir, stderr);
   const struct ir3_shader_nir_options fopts = {};
   ir3_finalize_nir(compiler, &fopts, nir);
   if (!quiet && getenv("SPV2IR3_NIR"))
      nir_print_shader(nir, stdout);

   struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &(struct ir3_shader_options){
                                                      .api_wavesize = IR3_SINGLE_OR_DOUBLE,
                                                      .real_wavesize = IR3_SINGLE_OR_DOUBLE,
                                                   });
   struct ir3_shader_key key;
   memset(&key, 0, sizeof(key));
   struct ir3_shader_variant *v = ir3_shader_get_variant(shader, &key, false, !quiet, NULL, NULL);
   if (!v || !v->bin) {
      printf("FAIL %s compile\n", argv[i]);
      return 1;
   }
   if (!quiet && v->disasm_info.disasm)
      fputs(v->disasm_info.disasm, stdout);
   printf("OK %s instrs=%u half=%d full=%d constlen=%u ubos=%u ssbos=%u tex=%u img=%u\n",
          argv[i], v->info.instrs_count, v->info.max_half_reg + 1, v->info.max_reg + 1,
          v->constlen, s.next[C_UBO] + 1, s.next[C_SSBO], s.next[C_TEX], s.next[C_IMG]);
   return 0;
}
