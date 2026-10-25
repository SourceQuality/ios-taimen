# M0: CoreAnimation's Metal shaders on the Adreno 540, checked on the PC

M0 of `docs/taimen-metal-plan.md`. It runs in WSL (Linux x86_64) and needs no phone.
It answers whether every shader in iOS 16 QuartzCore's `default.metallib` survives both steps:

1. **AIR → SPIR-V** with the metal2vulkan translator, against `a540-gallium.json`.
2. **SPIR-V → ir3 for gpu 540** with Mesa's own compiler, set up the way the Gallium a5xx driver uses it.

## Files

| File | What it is |
|---|---|
| `build.sh` | Builds the host tools: Mesa (the sweetgl pin + `mesa.patch` + `mesa-a5xx-ir3.patch`) with `spv2ir3` and `ir3-disasm`, and `m0dump`. |
| `a540-gallium.json` | The Vulkan profile the translator targets. It is vk12-floor plus what Gallium a5xx can provide or emulate: shaderInt64, independentBlend, 8/16-bit storage, float16, bufferDeviceAddress, scalar layout, and rasterization-order attachment access (emulated). |
| `m0dump/` | Lists each function of a metallib: stage, vertex attributes and function constants. Output is `functions.jsonl`. |
| `m0drive.py` | Translates every pipeline. It builds vertex descriptors from the reflection (one interleaved buffer at index 30) and pairs each fragment function with a vertex function: by name first, then by precision (`_lpf`/`_lph`), until one links. It fills in the function constants and color formats the translator asks for. Output is `$M0/out/pipelines/*` and `m0-report.json`. |
| `spv2ir3.c` | SPIR-V → NIR → ir3 for a5xx, bindful, the way Gallium does it. Descriptors are flattened per shader in (set, binding) order: push constants become constbuf 0, then UBOs 1.., SSBOs 0.., textures 0.., images 0... Input attachments become texel fetches at the fragment coordinate, which is the texture-barrier emulation of framebuffer fetch. Output is the disassembly plus an `OK ... instrs= full= constlen=` line. |
| `m0ir3.py` | Runs `spv2ir3` on every stage. Output is `$M0/out/ir3-540/*.txt` and `m0-ir3-540.json`. It counts `raw` words, which are instructions the a5xx decoder rejects. |
| `mesa-a5xx-ir3.patch` | ir3 fixes for paths that GL ES on a5xx never takes (see below). |

## Run

`$M0` defaults to `../research/m0` next to the port checkout.

1. Put `default.metallib` from `System/Library/Frameworks/QuartzCore.framework` of the iOS 16 root into `$M0`. It is Apple's binary, so it never goes into git.
2. Put the metal2vulkan checkout, with its CLI built (`cargo build --release --features cli`), in `$M0/metal2vulkan`.
3. Then run:

```bash
tools/sweetmtl/m0/build.sh
```

```bash
tools/sweetmtl/m0/m0drive.py -j 12
```

```bash
tools/sweetmtl/m0/m0ir3.py -j 12
```

## Results (2026-10-25, Mesa 26.3.0-rc2, metal2vulkan 720ef1ce + flame patches 0001-0006)

**Translation:** 139 of 141 functions translate. That is 39 vertex, 93 of 95 fragment and 7 kernel functions; 4 of the kernels are tile functions, compiled as tile pipelines. The 2 refusals are `inplace_copy_lpf` and `inplace_copy_lph`, which use raster order groups and so need `fragmentShaderPixelInterlock`. a5xx has no pixel interlock, so these two need a translator option that drops the ordering, or a copy pass.

**ir3 for gpu 540:** 232 of 232 SPIR-V stages compile, with 0 undecodable words.
- Median 183 instructions; the largest is `asg_frag_lpf` at 2823.
- At most 32 full registers, no spills, constlen at most 68 vec4.

**The plan's blocker 1 does not arise for CoreAnimation:**
- No stage uses `PhysicalStorageBuffer`. The SPIR-V capabilities seen are Shader, Float16, ImageQuery, InputAttachment, Int16, Int8, StorageBuffer16BitAccess, StorageBuffer8BitAccess and StorageImageWriteWithoutFormat.
- So CoreAnimation needs no a5xx `ldg`/`stg` patch.
- The descriptors used are UBOs (357), combined image samplers (81), SSBOs (65), input attachments (18, framebuffer fetch), sampled images (4), plus small push-constant ranges.

**What `mesa-a5xx-ir3.patch` fixes.** GL ES on a5xx never reaches these paths:
1. **Image loads.** `ir3_nir_scan_driver_consts` reserves image dims only for coherent loads. The emitter takes the LDIB path, which needs those dims, for every load without `CAN_REORDER`.
2. **16-bit SSBO loads.** They produced `ldgb` with a half destination, which a5xx doesn't have. SSBO loads are now 32-bit, like UBO loads.
3. **UBO loads inside control flow.** A load without `CAN_SPECULATE` skipped the UBO tracking, so no `UBO_PTRS` consts were reserved, which only a5xx's ldg path needs.
4. **Bitwise triops.** `has_bitwise_triops` was on for gen 5 and emitted `shrm`/`andg`, which the ISA description gates at gen 600 ("TODO check on pre a6xx gens"). They are now off for gen 5.

**Not covered yet:**
- The fixed-function shaders (`fixed_vert_*`/`fixed_frag_*`) were compiled with only one set of function constants.
- The Gallium shim on drm-shim (the last M0 item in the plan) is folded into M1.
