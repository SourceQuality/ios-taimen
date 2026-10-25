/*
 * sweetmtl Gallium shim: Metal's and Vulkan's raw enumerations to Gallium's.
 *
 * Metal values are those of Apple's headers (MTLBlendFactor, MTLCompareFunction, ...); the
 * Vulkan values are the ones the translator states in its GraphicsState. A value without a
 * Gallium counterpart maps to -1 (the caller fails with a reason).
 *
 * SPDX-License-Identifier: MIT
 */
#include <vulkan/vulkan_core.h>

#include "sg_priv.h"

#include "util/format/u_format.h"

int
sg_target_from_metal(uint32_t type)
{
   switch (type) {
   case 0: return PIPE_TEXTURE_1D;         /* MTLTextureType1D */
   case 1: return PIPE_TEXTURE_1D_ARRAY;   /* 1DArray */
   case 2: return PIPE_TEXTURE_2D;         /* 2D */
   case 3: return PIPE_TEXTURE_2D_ARRAY;   /* 2DArray */
   case 4: return PIPE_TEXTURE_2D;         /* 2DMultisample */
   case 5: return PIPE_TEXTURE_CUBE;       /* Cube */
   case 6: return PIPE_TEXTURE_CUBE_ARRAY; /* CubeArray */
   case 7: return PIPE_TEXTURE_3D;         /* 3D */
   case 8: return PIPE_TEXTURE_2D_ARRAY;   /* 2DMultisampleArray */
   case 9: return PIPE_BUFFER;             /* TextureBuffer */
   default: return -1;
   }
}

unsigned
sg_bind_from_usage(uint32_t vk_format, uint32_t usage)
{
   enum pipe_format f = (enum pipe_format)sg_pipe_format(vk_format);
   unsigned bind = 0;
   if (usage & SG_USAGE_SAMPLE)
      bind |= PIPE_BIND_SAMPLER_VIEW;
   if (usage & SG_USAGE_STORAGE)
      bind |= PIPE_BIND_SHADER_IMAGE;
   if (usage & (SG_USAGE_RENDER | SG_USAGE_BLEND))
      bind |= util_format_is_depth_or_stencil(f) ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;
   if (usage & SG_USAGE_VERTEX)
      bind |= PIPE_BIND_VERTEX_BUFFER;
   return bind;
}

int
sg_swizzle_from_metal(uint8_t s)
{
   switch (s) {
   case 0: return PIPE_SWIZZLE_0;
   case 1: return PIPE_SWIZZLE_1;
   case 2: return PIPE_SWIZZLE_X;
   case 3: return PIPE_SWIZZLE_Y;
   case 4: return PIPE_SWIZZLE_Z;
   case 5: return PIPE_SWIZZLE_W;
   default: return -1;
   }
}

int
sg_func_from_metal(uint32_t f)
{
   /* MTLCompareFunction is PIPE_FUNC's order: never, less, equal, lequal, greater, notequal,
    * gequal, always */
   return f <= 7 ? (int)f : -1;
}

int
sg_stencil_op_from_metal(uint32_t op)
{
   switch (op) {
   case 0: return PIPE_STENCIL_OP_KEEP;
   case 1: return PIPE_STENCIL_OP_ZERO;
   case 2: return PIPE_STENCIL_OP_REPLACE;
   case 3: return PIPE_STENCIL_OP_INCR;      /* IncrementClamp */
   case 4: return PIPE_STENCIL_OP_DECR;      /* DecrementClamp */
   case 5: return PIPE_STENCIL_OP_INVERT;
   case 6: return PIPE_STENCIL_OP_INCR_WRAP;
   case 7: return PIPE_STENCIL_OP_DECR_WRAP;
   default: return -1;
   }
}

int
sg_wrap_from_metal(uint32_t mode)
{
   switch (mode) {
   case 0: return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   case 1: return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
   case 2: return PIPE_TEX_WRAP_REPEAT;
   case 3: return PIPE_TEX_WRAP_MIRROR_REPEAT;
   case 4: return PIPE_TEX_WRAP_CLAMP_TO_BORDER;  /* ClampToZero: a transparent black border */
   case 5: return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   default: return -1;
   }
}

int
sg_prim_from_metal(uint32_t prim)
{
   switch (prim) {
   case 0: return MESA_PRIM_POINTS;
   case 1: return MESA_PRIM_LINES;
   case 2: return MESA_PRIM_LINE_STRIP;
   case 3: return MESA_PRIM_TRIANGLES;
   case 4: return MESA_PRIM_TRIANGLE_STRIP;
   default: return -1;
   }
}

int
sg_blend_factor_from_vk(uint32_t f)
{
   switch ((VkBlendFactor)f) {
   case VK_BLEND_FACTOR_ZERO: return PIPE_BLENDFACTOR_ZERO;
   case VK_BLEND_FACTOR_ONE: return PIPE_BLENDFACTOR_ONE;
   case VK_BLEND_FACTOR_SRC_COLOR: return PIPE_BLENDFACTOR_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
   case VK_BLEND_FACTOR_DST_COLOR: return PIPE_BLENDFACTOR_DST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA: return PIPE_BLENDFACTOR_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA: return PIPE_BLENDFACTOR_DST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
   case VK_BLEND_FACTOR_CONSTANT_COLOR: return PIPE_BLENDFACTOR_CONST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA: return PIPE_BLENDFACTOR_CONST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
   case VK_BLEND_FACTOR_SRC1_COLOR: return PIPE_BLENDFACTOR_SRC1_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
   case VK_BLEND_FACTOR_SRC1_ALPHA: return PIPE_BLENDFACTOR_SRC1_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
   default: return -1;
   }
}

int
sg_blend_op_from_vk(uint32_t op)
{
   switch ((VkBlendOp)op) {
   case VK_BLEND_OP_ADD: return PIPE_BLEND_ADD;
   case VK_BLEND_OP_SUBTRACT: return PIPE_BLEND_SUBTRACT;
   case VK_BLEND_OP_REVERSE_SUBTRACT: return PIPE_BLEND_REVERSE_SUBTRACT;
   case VK_BLEND_OP_MIN: return PIPE_BLEND_MIN;
   case VK_BLEND_OP_MAX: return PIPE_BLEND_MAX;
   default: return -1;
   }
}

unsigned
sg_colormask_from_vk(uint32_t mask)
{
   unsigned m = 0;
   if (mask & VK_COLOR_COMPONENT_R_BIT) m |= PIPE_MASK_R;
   if (mask & VK_COLOR_COMPONENT_G_BIT) m |= PIPE_MASK_G;
   if (mask & VK_COLOR_COMPONENT_B_BIT) m |= PIPE_MASK_B;
   if (mask & VK_COLOR_COMPONENT_A_BIT) m |= PIPE_MASK_A;
   return m;
}
