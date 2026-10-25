# Plan: a Metal device on taimen (Adreno 540) (2026-10-25)

Plan only. Nothing was built, booted or committed. Sources read:
- `ref/ios-flame`: `METAL-NEXT.md`, `HANDOFF.md`, `phone/metal2/*` (frontend, plugin, bridge, patches), `tools/build-m2v-bridge.sh`, `tools/flame-metal2-install.sh`, `xnu/patches/0096`, `0116`.
- `port/`: `tools/sweetgl/*`, `.github/workflows/sweetgl.yml`, `docs/taimen-gpu-plan.md`, `NOTES-taimen.md`.
- `scratch/xnu`: `iokit/Kernel/arm/SweetGPU.cpp`, `SweetFramebuffer.cpp`, `bsd/dev/sweet_gpu.c`.
- metal2vulkan on GitHub (README, `docs/API.md`, `docs/ARCHITECTURE.md`, `docs/REFUSALS.md`, runtime source tree).
- Mesa freedreno and ir3 sources, read from the fuchsia.googlesource.com mirror at **mesa-23.3.5**. gitlab.freedesktop.org refused the fetch, and no Mesa tree is unpacked on this PC. Statements about Mesa therefore need a re-check against our 26.3.0-rc2 pin in M0.

Marked **(unverified)**: inferences that no file in hand confirms.

## 0. Short version

- **Feasible, but large.** Flame's stack splits cleanly at a C ABI. The Objective-C Metal frontend talks only to `nmb_*` (`bridge/NativeMetalObjects.h`, 125 functions, 119 of them used). Behind that sits Rust that calls two crates:
  - **the metal2vulkan translator** (`metal2vulkan`): AIR → SPIR-V plus a binder that never calls Vulkan;
  - **metal2vulkan-runtime**: everything that touches Vulkan (ash). Only this part ties the stack to Turnip.
- **Recommendation: (a) "metal2gallium".** Keep three pieces as they are:
  - the frontend (`NativeBridgeMetalFrontend.mm`), with small taimen changes;
  - the C ABI and the bridge's Metal-value conversion code (`NativeMetalObjects.rs`);
  - the metal2vulkan translator.

  Replace metal2vulkan-runtime with our own small runtime crate. It implements the runtime API subset the bridge calls (about 100 methods), over a C shim on Gallium freedreno. The shim reuses sweetgl's screen and `sweet_drm.c`. Shaders go AIR → SPIR-V (translator) → NIR (`spirv_to_nir`, `libvtn.a`, already linked into sweetgl) → ir3 for a5xx.
- **No new kernel GPU ABI is needed.** We keep the SweetGPU user client that sweetgl already uses. The kernel only has to name the plugin on the existing `IOAcceleratorES`. Before arbitrary apps get a device, it also needs GPU hang recovery: today a hang turns the GPU service off until reboot (`sweet_gpu.c:1669-1677`).
- **Two hard blockers, both testable on the PC before the phone (M0):**
  1. **ir3 has no global memory ops on a4xx/a5xx.** `ir3_a4xx.c` sets `.emit_intrinsic_load_global_ir3 = NULL` and `.emit_intrinsic_store_global_ir3 = NULL`, and global atomics are `unreachable("Global atomic are unimplemented on A5xx")`. CA's shaders follow 64-bit `gpuAddress` pointers (flame `HANDOFF.md:697-700`). The translator emits those as buffer-device-address loads, and refuses `DereferenceLoadedPointer` without them (`REFUSALS.md`). a5xx already loads UBOs with `ldg` from 64-bit addresses, so a `load_global_ir3`/`store_global_ir3` for a5xx should be a small Mesa patch **(unverified until it compiles and runs)**.
  2. **No framebuffer fetch on a5xx** (`PIPE_CAP_FBFETCH` is a6xx-only). CA picks the Apple-family `_lph` shaders, which blend in the shader (`fc_framebuffer_fetch=1`, flame `HANDOFF.md:705-706`). We emulate it the way sweetgl already does for GL: sample the render target behind a texture barrier (`eagl.c:298-345, 961-1000` and `mesa.patch` `fd5_texture_barrier`). The translator's profile has to claim `rasterizationOrderColorAttachmentAccess`, which this emulation provides only between draws.
- **Order:**
  - **M0** (PC only): translate and compile CA's shaders for a540, and run the Gallium backend on drm-shim.
  - **M1**: a device in a test tool.
  - **M2**: clear, readback and IOSurface render.
  - **M3**: CA's pipelines on the phone.
  - **M4**: the first real Metal client (AegirPoster) while backboardd stays on GLES.
  - **M5**: backboardd on Metal (optional).

  This swaps the brief's M4 and M5. backboardd already composites at ~60 frames/s on GLES (`taimen-gpu-plan.md:3-21`); what fails today is the clients that need a device.
- **Size:** about 10-14k new lines (Rust 6-8k, C 2.5-4k, the rest small) and **5-8 weeks** of focused sessions to M4 with a few clients. WebKit's GPU process and Maps at good quality come after that. The biggest unknowns are the translator's demands on our "virtual Vulkan" profile, the a5xx `ldg` patch, and per-process memory.

## 1. What the frontend calls, and what is reusable (question 1)

### 1.1 The layers in flame's SweetMetal2.bundle

| Layer | Files (ref/ios-flame) | Vulkan? |
| --- | --- | --- |
| Plugin: class Metal instantiates, process allow-list, env, CA prefs | `phone/metal2/SweetMetal2.mm` (245 lines) | no (passes Turnip's path as "loader") |
| Frontend: ObjC Metal objects | `phone/metal2/NativeBridgeMetalFrontend.mm` (1758 lines, 285 methods), `.h` | **no** (no `vk*` calls; "VkPipeline" appears only in text) |
| C ABI | `phone/metal2/bridge/NativeMetalObjects.h` (125 fns), `NativeMetalBridge.h` (`nmb_selftest`) | no |
| Bridge (Rust): raw Metal values → translator types, handles, errors | `bridge/NativeMetalObjects.rs` (2047), `NativeMetalBridge.rs` (363, self-test) | uses runtime types (`NativeMetalObjects.rs:19-23`) |
| Translator: AIR parse, AIR→SPIR-V, layout, binder | crate `metal2vulkan` (+ `metal2vulkan-air`) | **no** ("returns plain data for creating Vulkan objects"; `bind/` "never calls Vulkan") |
| Runtime: Metal object model on ash | crate `metal2vulkan-runtime` (15 dirs, 100+ files) | **yes** |
| Vulkan driver | Turnip `libvulkan_freedreno.dylib` + darwin KMD + kernel 0115 NativeAdreno ABI (`phone/turnip/NativeAdrenoABI.h`) | yes, and a6xx+ only |

### 1.2 The C ABI the frontend uses (`NativeMetalObjects.h`)

The frontend uses 119 of the 125 functions. Unused: `nmb_device_release`, `nmb_fixture_air`, `nmb_function_constant_air`, `nmb_compute_pipeline_new`, `nmb_render_pipeline_new`, `nmb_binary_archive_add_compute`. Errors are per thread (`nmb_last_error`, `nmb_last_warning`), and every handle is released once.

- **Device (h:36-57):**
  - `nmb_device_open(loader, cache)`, `_name`, `_max_buffer_length`, `_work_stats`, `_shader_cache_stats`, `_max_threads_per_threadgroup`;
  - `_answer_selector`/`_answer` (Metal's count/flag queries by selector name, installed as device methods: frontend `installAnswers`, `NativeBridgeMetalFrontend.mm:1533`);
  - `_vendor_name`, `_family_waivers`, `_linear_bytes_per_row`.
- **Binary archives (h:59-62, 146-148):** `nmb_binary_archive_new/release/serialize/add_render/add_compute_descriptor`.
- **Buffers (h:64-70, 93):** `nmb_buffer_new`, `_new_no_copy` (page-aligned host memory, used for IOSurface bases), `_release`, `_contents`, `_length`, `_gpu_address`, `_set_purgeable_state`.
- **Heaps (h:76-84):** `nmb_heap_new/release/size/used_size/allocated_size/max_available_size/set_purgeable_state/new_buffer/new_texture`.
- **Textures (h:72-99):** `nmb_texture_new`, `_new_on_buffer` (linear over a buffer; IOSurface flag), `_new_linear`, `_new_view`, `_set_purgeable_state`, `_release`, `_describe`, `_replace`, `_get_bytes`.
- **Libraries and functions (h:101-113):** `nmb_library_new` (metallib or bare AIR), `_release`, `_function_count`, `_function_name`, `nmb_function_new`, `_new_specialized` (function constants), `_release`, `_type`.
- **Pipelines and states (h:115-161):**
  - compute: `nmb_compute_pipeline_new_with_options`, `_with_descriptor`, `_release`, `_max_threads`, `_execution_width`;
  - render: `nmb_render_pipeline_new_with_options`, `_release`;
  - `nmb_sampler_new/release`, `nmb_depth_stencil_new/release`.
- **Queue and command buffers (h:163-185):**
  - `nmb_queue_new/release`;
  - `nmb_command_buffer_new/release/commit/status/error/gpu_time`;
  - `_uses_buffer`/`_uses_texture` (the frontend's resource waits);
  - `_on_completed`, `_on_scheduled`, `_wait`, `_wait_scheduled`.
- **Sync (h:187-202):** `nmb_event_new/release`, `nmb_shared_event_value/set_value/wait/notify`, `nmb_command_buffer_signal_event/wait_event`, `nmb_fence_new/release`, `nmb_encoder_update_fence/wait_fence`.
- **Encoders (h:204-257):**
  - lifecycle: `nmb_compute_encoder_new`, `nmb_blit_encoder_new`, `nmb_render_encoder_new` (pass descriptor: 8 colours, depth, stencil, visibility buffer), `nmb_encoder_end`, `_discard`;
  - bindings (stage 0 compute, 1 vertex, 2 fragment): `nmb_encoder_set_buffer`, `_set_buffer_offset`, `_set_bytes`, `_set_texture`, `_set_sampler`;
  - compute: `nmb_compute_set_pipeline`, `_set_threadgroup_memory`, `_dispatch`;
  - render: `nmb_render_set_pipeline`, `_set_depth_stencil`, `_set_viewport`, `_set_scissor`, `_set_raster`, `_set_stencil_reference`, `_set_blend_color`, `_set_depth_bias`, `_set_line_width`, `_set_visibility_mode`, `_draw`, `_draw_indexed`;
  - blit: `nmb_blit_copy_buffer`, `_fill_buffer`, `_copy_texture`, `_buffer_to_texture`, `_texture_to_buffer`, `_generate_mipmaps`.

Frontend entry points used by the plugin (`NativeBridgeMetalFrontend.h`): `native_bridge_metal_open(loader, cache, registryID, missing)` (`.mm:1553`), plus status, telemetry and benchmark helpers. `native_bridge_metal_open` makes a runtime subclass of iOS's `_MTLDevice` ("NativeBridgeMetalDevice"). It copies `NBDeviceMethods` into it, installs the answers, adopts `MTLDeviceSPI`, and adopts `MTLTextureImplementation` on `NBTexture` (`.mm:1553-1590`). Every other object is a plain NSObject class (`METAL-NEXT.md:93-96`). The frontend states its own gaps: "heaps, argument buffers, indirect commands, drawables/presentation" (`.mm:8`).

### 1.3 Vulkan-specific or reusable

- **Reusable as is:**
  - the frontend and plugin, with taimen deltas (section 4.3);
  - the C ABI;
  - the bridge's conversions (`NativeMetalObjects.rs:7-18` imports only translator types: enums, `RenderPipelineDescriptor`, `FunctionConstantValues`, `SamplerDescriptor`, `DepthStencilDescriptor`);
  - the whole translator: AIR parsing (`metal2vulkan-air`), specialisation, AIR→SPIR-V 1.5, the pipeline layout, vertex input, fixed-function state, the binder (`BindState` → `BindingPlan` → `instantiate_draw/dispatch` → descriptor writes, push constants, "push holes" filled with device addresses), pipeline keys and artifact (de)serialisation (`API.md`);
  - translator patches 0001, 0004 and 0005 (crates `metal2vulkan-air`, `metal2vulkan`).
- **Vulkan-specific (to replace):**
  - every `metal2vulkan_runtime::*` use (`NativeMetalObjects.rs:19-23`: `Device`, `Buffer`, `Texture`, `Heap`, `CommandQueue/Buffer`, the encoders, `Event/SharedEvent/Fence`, `BinaryArchive`, `ShaderCacheOptions` (`:165`), `FamilyRow/FeatureRow/GpuFamily/MetalLevel/PrivateProperties`);
  - runtime patches 0002, 0003, 0006, 0100, 0101;
  - Turnip and the NativeAdreno kernel ABI.
- **Resource tracking:**
  - The frontend's part is ours to keep: `waitForResource` (`.mm:525`) and `flushSurfaces` (`.mm:259`), which on taimen becomes a no-op because the kernel cleans IOSurfaces at submit (xnu 0242).
  - The runtime's part (`used_resources.rs`, patch 0002) has to be redone: which buffers or textures a pending command buffer uses.

### 1.4 Where metal2vulkan comes from

- **Not on this PC.** `ref/ios-flame/work/` holds only `userland/bin` (`ios-cc`, `mac-cc`), and no `metal2vulkan` checkout exists anywhere in WSL.
- **Not fetched in any CI either.** Flame's only workflow is `.github/workflows/xnu.yml`. Flame built the bridge by hand on its Mac:
  - `tools/build-m2v-bridge.sh:5`: "work/metal2vulkan = github.com/steelbrain/metal2vulkan 720ef1ce (UniXNU's pin) + phone/metal2/bridge/patches";
  - cargo `+1.98.0`, staticlib, `aarch64-apple-ios`, deployment target 16.0;
  - fixtures assembled with Homebrew `llvm-as`.
- **Repo:** https://github.com/steelbrain/metal2vulkan, GPL-2.0-or-later OR LGPL-3.0-or-later, Rust >= 1.98, edition 2024.
  - Crates: `metal2vulkan` (translator, CLI feature), `metal2vulkan-runtime` (ash), `metal2vulkan-air`, plus validation and devtools.
  - The current HEAD is `a2c440b8`, which `METAL-NEXT.md:4-5` notes is 66 commits newer than the 720ef1ce pin.
- **Plan:** pin 720ef1ce plus flame's three translator patches (proven on iOS 16.7.16 AIR). Fetch it in CI and WSL with `tools/sweetmtl/m2v-fetch.sh` (git fetch of the pin, then patch).

## 2. Can the ABI sit on Gallium? (question 2)

### 2.1 Shader path

1. The translator, given **our device profile** (a Khronos profiles JSON: `DeviceDescription::from_khronos_json`), compiles a render or compute pipeline. It returns SPIR-V 1.5 per stage, one layout (set 0 dense in `ResourceSlot` order, set 1 heap arrays, push constants per command), vertex input and graphics state (`API.md`). It refuses anything the profile does not cover, with a typed reason.
2. `spirv_to_nir` (Mesa `src/compiler/spirv`, `libvtn.a`: already in sweetgl's link, `tools/sweetgl/build.sh:18`). The options:
   - push constants and UBO/SSBO use 32-bit index+offset;
   - physical storage buffers use `nir_address_format_64bit_global`;
   - subpass inputs are kept.
3. Our NIR lowering, written after lavapipe's and rusticl's frontends:
   - (set, binding) → Gallium slots from the translator's layout;
   - push constants → constant buffer 0;
   - input attachments → `txf` at the frag coord on an extra sampler view (`nir_lower_input_attachments`);
   - `nir_lower_explicit_io`;
   - IO locations;
   - optionally 16-bit float → 32-bit (sweetgl runs `nofp16` for CA's GL shaders: `sweetgl.c:102-108`);
   - then `screen->finalize_nir`.
4. `pipe->create_{vs,fs,compute}_state` with `PIPE_SHADER_IR_NIR` → freedreno and ir3 for gpu 540.

### 2.2 What CA's Metal path asks of the GPU, and a5xx

From flame's runs on the same iOS 16.7.16 (20H392) iPhone X userland that taimen runs (`ref/ios-flame/README.md:7`, `research/ipsw-x/meta.json`):
- **Pipelines:**
  - Translating the logged keys on the Mac, 230 of 242 CA pipeline keys translated (`HANDOFF.md:163-164`).
  - On the phone, about 270 render pipelines on a cold cache at 3-30 ms each on the SD855 (`HANDOFF.md:118-119`).
  - UniXNU measured 103 pipelines in 40 s on SM8550 (`METAL-NEXT.md:97`).
- **The compositor is one uber shader.** `fixed_vert_lph_spc` + `fixed_frag_lph_cph` (104 KB AIR), mostly unspecialised. Each draw is configured through buffers that hold **64-bit gpuAddress pointers** (`HANDOFF.md:686-700`). Each draw is an indexed quad of 6 u16 indices; a pass has about 3-10 draws.
- **Other features:**
  - framebuffer fetch on attachment 0 **and** a memoryless attachment 1;
  - RenderBox: an RG16F accumulator on attachment 1 with depth/stencil (`HANDOFF.md:307-311, 435-436`);
  - 1-px lines for edge AA, and points;
  - render formats BGRA8, RGBA8, R8, RGBA16F, BGR5A1, B5G6R5 (`HANDOFF.md:410-411`);
  - 1D textures (Siri chip);
  - blend factors beyond source-over (`HANDOFF.md:274-277`);
  - compute kernels in QuartzCore's library (counted by `nmb_selftest`'s QuartzCore step).

| Need | freedreno a5xx (mesa-23.3.5 source; re-check in 26.3) | Plan |
| --- | --- | --- |
| Pointer loads (BDA) | **missing**: `ir3_a4xx_funcs` has `load/store_global_ir3 = NULL`; global atomics unimplemented | Mesa patch: a5xx `ldg`/`stg` like the gen<6 UBO path (`emit_intrinsic_load_ubo` builds 64-bit addresses: `ir3_pointer_size`). No global atomics: refuse |
| Framebuffer fetch | `PIPE_CAP_FBFETCH`: a6xx only; `TEXTURE_BARRIER`: yes | Emulate as sweetgl does (`eagl.c` fbfetch, `fd5_texture_barrier`); per draw, not within a draw |
| MRT, independent blend | `max_rts = A5XX_MAX_RENDER_TARGETS` (8); `INDEP_BLEND_ENABLE`: a5xx yes | memoryless attachments become real textures (sysmem rendering) |
| Dual-source blend | a3xx/a6xx only | profile: off (translator refuses) |
| Render formats | BGRA8/RGBA8 (+sRGB), R8, RG8, R16F, RG16F, RGBA16F, R32F, RGB10A2, B5G6R5, B5G5R5A1, A8, R11G11B10F: all have RB formats (`fd5_format.c`) | XR10 ('w30r', BGR10_XR): no direct match, so withhold for backboardd as flame does |
| Depth/stencil | Z16, Z24S8, Z32F, Z32F_S8X24, S8 | ok |
| Compressed sampling | ASTC/ETC2/BC: sample only; **no PVRTC** | answers must not claim PVRTC; CPU-decode or refuse |
| MSAA | 1, 2, 4 samples (`fd5_screen.c` `valid_sample_count`) | **(unverified)** in sysmem (GMEM bypass), which taimen forces (`sweetgl.c:102`) |
| Compute | `fd5_compute.c`; 1024 threads; SSBO/images 24 in FS/CS, **0 in VS** | ok; vertex-stage buffers must go through `ldg` (BDA/UBO) |
| Depth clamp | `DEPTH_CLIP_DISABLE`: not a5xx | profile: off |
| Start instance | a5xx hidden (`START_INSTANCE` = a4xx/a6xx) | emulate (offset per-instance vertex buffers / push value) |
| Sample shading, geometry, tessellation | no | profile: off |
| Textures per stage | 16 sampler views, 16 samplers; 16 constant buffers; cube arrays yes; array layers 256; 2D 16384 | profile limits |
| Occlusion, timestamps | yes | visibility results, GPU time |

Verdict: yes, everything CA's compositor needs exists on a5xx, except **global loads** (a Mesa patch) and **framebuffer fetch** (emulated as in sweetgl). Apps that need global atomics, dual-source blending, depth clamp, sample shading or tessellation get the translator's typed refusals. That is the same failure mode flame has for unsupported features.

### 2.3 Gallium-side design notes

- **One pipe_screen per process** over `sweet_drm.c` (`fd_drm_screen_create_renderonly`, `sweetgl.c:112`). Use the same env as sweetgl: `FD_MESA_DEBUG=sysmem` (GMEM tile restore overruns unpadded IOSurfaces, `sweetgl.c:96-102`).
- **Deferred replay.** Metal encodes on many threads; a `pipe_context` is single-threaded. Our runtime records each `MTLCommandBuffer` as a Rust command list. It replays it at commit on the queue's thread into one `pipe_context`, then calls `flush` with a fence. A completion thread waits (`fence_finish`, i.e. SweetGPU sel 14) and runs the handlers.
- **Stable GPU addresses.** `gpuAddress` = bo iova (sel 10 returns it; `SweetGPU.cpp:21-24`). Avoid any freedreno path that reallocates a resource's bo: no `DISCARD_WHOLE_RESOURCE` transfers, no shadowing. Map bos directly. Check that freedreno batch reordering is off in 26.3 (pointer-chased buffers are invisible to its dependency tracking) **(unverified)**.
- **Bo lists.** Buffers reached only through pointers are not bound anywhere, so freedreno will not list them in the submit.
  - Heap bos are write-combined and need no flush (`SweetGPU.cpp` `taimengpuheap`).
  - Wired fallback bos need one: attach all live Metal buffers of the device to each submit, or allocate Metal buffers only from the heap.
- **IOSurfaces.** Import by ID with sel 12 and wrap with `resource_from_handle` (LINEAR, pitch % 64 == 0, else a copy). This is exactly sweetgl's code, `eagl.c:1755-1850`. Do not import through host pointers as Turnip did.

## 3. Alternatives (question 3)

| | What | For | Against | Verdict |
| --- | --- | --- | --- | --- |
| **(a)** | Our runtime subset on Gallium + metal2vulkan translator + flame's frontend | Reuses the frontend, ABI, conversions and translator; no new kernel ABI; Gallium is lean; sized to CA first | We write ~9-12k lines; app coverage grows piecemeal; needs the ir3 `ldg` patch | **Recommended** |
| (b) | Translator SPIR-V → SPIRV-Cross → GLSL ES on sweetgl's GL | GL path proven at 60 fps; fbfetch already emulated | **No pointer chasing in GLSL ES** (no buffer_reference; Mesa GL has no BDA), so CA's uber shader cannot run; GL contexts are thread-bound, Metal is not; two more compilers, slower compiles | Rejected |
| (c1) | Turnip on a5xx | Would run flame's stack unchanged | Turnip is a6xx+ by design (bindless descriptors, a6xx state); a new Vulkan driver | Rejected |
| (c2) | Lavapipe-style Vulkan-on-Gallium, then flame's runtime unchanged | Full Metal coverage of metal2vulkan-runtime for free | Lavapipe (Mesa ≥ 23.2) uses llvmpipe-specific descriptors ([MR 22828](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/22828), [MR 25398](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/25398)); VkDeviceMemory needs `allocate_memory/resource_bind_backing`, which freedreno lacks; more code than (a) | Plan B if app coverage in (a) becomes the bottleneck |
| (c3) | SweetMetal v1 style: host-made PM4 templates (flame `phone/metal`) | Fast to first pixels | Every pipeline must be seen on the host first; flame dropped it (`METAL-NEXT.md:9-14`) | Rejected |
| (c4) | Software Metal (metal2vulkan-runtime on lavapipe/llvmpipe, CPU) | No GPU work | LLVM JIT on iOS, very large, slow on Kryo 280 | Only as a last-resort fallback for tiny clients |
| (c5) | AIR → NIR directly (UniXNU `kits/AirNir`, flame `HANDOFF.md:326, 363`) | Skips SPIR-V | Not on this PC; maturity unknown; still needs `ldg` on a5xx | Keep in mind if `spirv_to_nir` chokes on the translator's SPIR-V |

## 4. Plan with milestones (question 4)

### 4.0 How iOS finds a Metal driver, and how flame registered theirs

- `MTLCreateSystemDefaultDevice` → `MTLRegisterDevices` iterates `IOServiceMatching("IOAcceleratorES")`. It loads `/System/Library/Extensions/<MetalPluginName>.bundle`, takes the class named `MetalPluginClassName` (a `_MTLDevice` subclass) and calls `-initWithAcceleratorPort:` with the service. A nil result is skipped, and that process has no device. Sources: flame `xnu/patches/0096-flame-metal-accelerator.patch` (comment lines 8-17, properties lines 55-57) and `0116` (switching plugin and class with `sweetmtl=2`).
- Flame's plugin (`SweetMetal2.mm`), section by section:
  - `:130-244`: `SweetMTL2Device -initWithAcceleratorPort:` calls `[super init]`, then gives the frontend's device once per process;
  - `:104-125`: only for backboardd and the processes listed in `/var/local/mtl2-clients`;
  - `:162-166`: CA prefs off: framebuffer compression, IOSurface compression, deep framebuffer;
  - `:65-70`: `MTLFailureTypeSetErrorModeType(t, 5)` so MPS validation does not abort;
  - `:96-101`: `newPipelineLibraryWithFilePath:` → nil;
  - `:56-62`: `sys_dcache_flush` overridden.

  `Info.plist` names `NSPrincipalClass` SweetMTL2Device. `tools/flame-metal2-install.sh` copies the bundle to `/System/Library/Extensions` and restarts backboardd.
- **taimen today:** `SweetFramebuffer.cpp:191-201, 566-572` (xnu 0166) publishes an `IOAcceleratorES` under AppleCLCD **without** `MetalPluginName`. Metal therefore skips it, and CA uses GLES (sweetgl). sweetgl reaches backboardd through `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=...` (`tools/taimen/boot-xnu.sh:59`); the GPU is the SweetGPU user client (`SweetGPU.cpp:1-35`).
- **taimen plan:**
  - A boot-arg `taimenmtl=SweetMetal-<hash8>` makes the kernel set `MetalPluginName` = that name and `MetalPluginClassName` = `SweetMTLDevice` on the same `IOAcceleratorES`. Without it, the system behaves as today.
  - Hash-named bundles fit ROOTFIX's write-if-absent rule, as `sweetgl-<hash>.dylib` does.
  - Leave out `IOGLBundleName` at first. OpenGLES might then try to load our bundle as a GL driver and disturb sweetgl's EAGL **(unverified)**; flame set it, so M1 tests both.

### 4.1 Milestones (each one testable on its own)

**M0: PC-only qualification (WSL, no phone).** Answers both blockers before any phone time.
- Build in WSL (Linux x86_64):
  - the metal2vulkan CLI (translator only, no ash) at 720ef1ce + the 3 translator patches;
  - Mesa 26.3.0-rc2 + `tools/sweetgl/mesa.patch` + a new `mesa-metal.patch` (a5xx `ldg`/`stg`), with `-Dtools=freedreno,drm-shim`.
- Write `tools/sweetmtl/profile/a540-gallium.json`: Vulkan 1.2, bufferDeviceAddress, shaderInt64, rasterizationOrderColorAttachmentAccess, independentBlend, imageCubeArray, fragmentStoresAndAtomics; no dualSrcBlend, depthClamp, geometry, tessellation or sampleRateShading; limits from section 2.2.
- Translate every vertex/fragment pair and kernel of the iPhone X `QuartzCore.framework/default.metallib`. Extract it from `research/ipsw-x/ios16-root.img` with the HFS+ reader `mkrootfix.py` uses; it is the path `m2vtest.c` uses on the phone.
- Compile each pair to ir3 for gpu 540 (`ir3_compiler`, if it reads `.spv` **(unverified)**; otherwise a 200-line `spv2ir3` driver on `spirv_to_nir`).
- Then run the Gallium shim (4.2) on the freedreno drm-shim with `FD_GPU_ID=540`: device, buffer, texture, pipeline, a recorded draw (no execution; check it with `cffdump`).
- **Pass:** a refusal list and an ir3 failure list for CA, with counts comparable to flame's 230/242; the `ldg` patch compiles CA's pointer loads.
- **Decision gate:** if the translator demands features we cannot fake, read its refusals and choose between profile tweaks, a translator patch, or plan B (c2).

**M0b: a dev loop without reboots** (recommended, small): `tools/phone/devd`, a root launchd daemon on the USB link (192.168.7.2).
- It takes `put PATH` (file over TCP), `run ARGV`, and `kill NAME`, and lives next to netsvcd. It is enabled by a boot-arg or file only: a dev phone over USB, never Wi-Fi.
- It turns M2-M5 iterations into minutes instead of a ROOTFIX boot each. It also sidesteps the ABL ~35 MiB payload limit for big bundles (`NOTES-taimen.md:628-633`).

**M1: `MTLCreateSystemDefaultDevice` returns our device in a test tool.**
- **Kernel:** `xnu/patches/NNNN-taimen-metal-plugin.patch` (`SweetFramebuffer.cpp`: properties when `taimenmtl=` is set).
- **Bundle:**
  - plugin (`SweetMetal.mm`, from `SweetMetal2.mm`), with a client list in `/private/var/sweetmtl/clients`. backboardd is **not** listed, so it returns nil there and CA stays on GLES.
  - frontend, unchanged except section 4.3;
  - runtime: device, answers and buffers only. The other `nmb_*` functions are stubs that fail with "not yet".
- **Test tool:** `mtltest` (ObjC, launchd job via `mkrootfix.py --daemon`). It logs through the /dev/null tee (`sweetnull=backboardd,mtltest`, xnu 0048): device name, registryID, `supportsFamily` answers, `newBuffer` + `contents` + `gpuAddress`.
- **Pass:** "mtltest: device ... registryID ..." in the XNU log; backboardd still draws with sweetgl at ~60 frames/s.

**M2: clear, readback, compute, IOSurface render.**
- **Runtime additions:** textures, command queue and buffer with commit and completion, render encoder (load Clear, store), blit (texture → buffer, buffer copy, fill), `getBytes`, compute pipelines and dispatch, sync (fences and events in-process).
- **`mtlbridgetest`:** the bridge self-test `nmb_selftest` (`NativeMetalBridge.rs:300-339`) recompiled against our runtime, like flame's `m2vtest.c`. Steps:
  - 0x6501 device;
  - 0x6502 the translator's fixture library;
  - 0x6503 compute (1000 outputs);
  - 0x6504 render 8x8;
  - 0x6505 render into imported memory, which here means an **IOSurface** imported with sel 12.
- **`mtltest`** does the same through `Metal.framework`: clear a BGRA8 texture backed by an IOSurface, then read it back with the CPU. That is "present" in the sense CA and CAMetalLayer drawables use: CA consumes IOSurfaces.
- **Pass:** compute_bad = render_bad = import_bad = 0; times per step logged.

**M3: CA's pipelines on the phone.**
- **Runtime additions:** render pipelines from the translator, the binder (per-draw descriptors and push constants → Gallium slots and cb0), vertex input, blend, depth/stencil, rasterizer, viewports, framebuffer-fetch emulation, the pipeline cache on disk (translator `to_bytes`/`from_bytes_for_key`, keyed by SHA-256), samplers.
- **Mesa:** the `ldg` patch from M0, on the phone now.
- **Test:** `mtlbridgetest` step 0x6506 builds every QuartzCore pair and kernel and draws each once into BGRA8. `mtltest` then draws the compositor (`fixed_vert_lph_spc` + `fixed_frag_lph_cph`) with CA-like buffers that contain a gpuAddress, and checks the pixels.
- **Pass:** pairs OK in the same range as flame (≥ 230 of the logged keys); cold and warm compile ms per pipeline; no GPU hang.

**M4: the first real Metal client while backboardd stays on GLES.**
- **Prerequisite (kernel):** GPU hang recovery for the A540. Today a stuck fence sets `g_ready = false` "until reboot" (`sweet_gpu.c:1669-1677`), which would also kill backboardd's GLES. The recovery is RBBM soft reset + CP and ME re-init + zap resume, as flame's 0108 did for the A640 (`HANDOFF.md:400-403`), plus per-client fault attribution.
- **Clients:** list AegirPoster (it crash-loops today: `NOTES-taimen.md:1036-1037`).
- **Frontend gaps flame met:**
  - XR10 answers for non-backboardd clients (`SweetMetal2.mm:211-217`);
  - `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`;
  - `newTextureWithBytesNoCopy`;
  - `MTLTextureImplementation` (`HANDOFF.md:71-73, 103-106`);
  - CAMetalLayer drawables. The frontend has no `presentDrawable:` (`.mm:8`); check what QuartzCore calls.
- **Next clients:** Maps (VectorKit: depth/stencil), then WebKit's GPU process. Sandboxed clients need the shader cache in `_CS_DARWIN_USER_CACHE_DIR` (`SweetMetal2.mm:154-160`) and an `iokit-open` for SweetGPU that our Sandbox policy allows **(unverified)**.
- **Pass:** AegirPoster stays up and the lock-screen poster draws (`phonectl.py shot`); no GLES regressions; GPU-hang test: `mtltest --hang` recovers.

**M5: backboardd on Metal (optional).**
- List backboardd **and SpringBoard**: flame's unlock swipe did nothing without SpringBoard (`HANDOFF.md:124-125`). Keep sweetgl injected as the fallback.
- Compare frame times with GLES (~60 frames/s, `taimen-gpu-plan.md:3-21`) and cold-boot compile time with the cache warm and cold.
- **Pass:** lock, unlock, home, Control Center and Settings correct (screenshot tour); frame rate not worse than GLES. Otherwise backboardd stays on GLES for good.

### 4.2 Files to create or change

| Path | What |
| --- | --- |
| `tools/sweetmtl/env.sh` | sources `tools/sweetgl/env.sh` (Mesa pin); `M2V_REPO`, `M2V_PIN=720ef1ce`; Rust toolchain 1.98.0 |
| `tools/sweetmtl/m2v-fetch.sh`, `patches/0001,0004,0005-*.patch` | fetch the pin and apply flame's translator patches (copied, MIT/LGPL headers kept) |
| `tools/sweetmtl/profile/a540-gallium.json` | the device profile the translator targets |
| `tools/sweetmtl/runtime/` (Rust crate `sweetmtl-runtime`) | the runtime API subset `NativeMetalObjects.rs` calls (about 60 types, about 100 methods): device+answers, memory, textures+IOSurface, library/function (translator), pipelines (translator + cache), encoders (recording, binder), queue/sync/completion, blits. Semantics read from metal2vulkan-runtime (license: GPL-2.0+/LGPL-3.0+ if code is copied) |
| `tools/sweetmtl/bridge/` | `NativeMetalObjects.{rs,h}`, `NativeMetalBridge.{rs,h}` from flame, with the `use metal2vulkan_runtime` lines pointed at our crate |
| `tools/sweetmtl/gallium/sg.h`, `sg_device.c`, `sg_shader.c`, `sg_resource.c`, `sg_replay.c` | C shim over Mesa: screen via `sweet_drm.c` (shared source with sweetgl), SPIR-V → NIR → lowering → CSOs, resources and IOSurface import (from `eagl.c`), command replay, fences |
| `tools/sweetmtl/frontend/NativeBridgeMetalFrontend.{mm,h}`, `SweetMetal.mm`, `Info.plist` | flame's files + taimen deltas (4.3) |
| `tools/sweetmtl/test/mtltest.m`, `mtlbridgetest.c` | M1-M3 tests (launchd jobs, log via the /dev/null tee) |
| `tools/sweetmtl/build.sh`, `host-check.sh` | macOS build of bundle + tests; WSL M0 run (translator CLI + ir3 + drm-shim) |
| `tools/sweetgl/mesa.patch` (or a second `tools/sweetmtl/mesa-metal.patch` applied by `mesa-fetch.sh`) | ir3 a5xx `load_global_ir3`/`store_global_ir3`; fixes M2/M3 turn up |
| `.github/workflows/sweetmtl.yml` | CI (4.4) |
| `tools/taimen/sweetmtl-install.sh` | like `sweetgl-install.sh`: wait for CI, download, ROOTFIX `--zfile` + `--mkdir` for `/System/Library/Extensions/SweetMetal-<hash>.bundle/...`, `--daemon` for tests, record `../out/sweetmtl/current.env` |
| `tools/taimen/boot-xnu.sh` | `MTL=1`: add `taimenmtl=SweetMetal-<hash>`, extend `sweetnull` with the clients |
| `xnu/patches/NNNN-taimen-metal-plugin.patch` | `MetalPluginName/ClassName` on `IOAcceleratorES` (`SweetFramebuffer.cpp:566-572`) |
| `xnu/patches/NNNN-taimen-gpu-recovery.patch` | A540 hang recovery in `bsd/dev/sweet_gpu.c` (replaces `gpu_hung`'s "off until reboot") |
| `xnu/patches/NNNN-taimen-gpu-bo-user.patch` (optional) | a SweetGPU selector that makes a bo from page-aligned user memory (`newBufferWithBytesNoCopy` on non-IOSurface memory); until then the runtime copies |
| `tools/phone/devd.c` (M0b) | put/run/kill over USB |

### 4.3 Frontend and plugin deltas for taimen

- `native_bridge_metal_open`: ignore `loader` (no Turnip).
- `sys_dcache_flush` (`SweetMetal2.mm:56-62`) and `flushSurfaces` (`.mm:259`): the kernel cleans listed bos and IOSurfaces at submit (xnu 0242). Keep `dc civac` only for no-copy host memory.
- `surfaceBuffer` (`.mm:167-189`) and `newTextureWithDescriptor:iosurface:plane:` (`.mm:1415-1422`):
  - import by **IOSurface ID** (a new `nmb_buffer_new_iosurface(id)`, sel 12) instead of a host pointer;
  - accept plane 0 of 'b3a8' as `eagl.c:1761-1769` does (the frontend refuses any surface with planes);
  - YUV planes later.
- The plugin waits for the GPU service like `eagl.c` `gpu_ready` (`:1876`) with a bounded wait, then returns nil. Logging goes through the /dev/null tee. The cache lives in `/private/var/sweetmtl/cache/<proc>`, or the sandbox cache dir. The env file is `/private/var/sweetmtl/env`.

### 4.4 How it is built (GitHub macOS runners; the PC is Windows)

`sweetmtl.yml`, on `macos-14`, with the same Xcode selection as `sweetgl.yml`:
- Python deps and bison as in `sweetgl.yml`.
- `rustup toolchain install 1.98.0 --target aarch64-apple-ios` (and `aarch64-apple-darwin` for host tests).
- `brew install llvm` for the fixtures' `llvm-as`, or commit the two fixture `.air` files.
- Caches:
  - the Mesa tarball (same key as sweetgl);
  - ccache, restored from sweetgl's key prefix;
  - `~/.cargo/registry` and the cargo target dir keyed by `M2V_PIN` + `Cargo.lock`.
- Steps:
  1. `tools/sweetgl/mesa-fetch.sh`, `mesa-setup.sh`, `ninja` the static libs. Metal needs no GL or GLSL; the list is `libgallium`, `libfreedreno*`, `libnir`, `libvtn`, `libcompiler`, `libmesa_util*`, `libblake3`, `libmesa_util_c11`.
  2. `tools/sweetmtl/m2v-fetch.sh` and `cargo +1.98.0 build --release --target aarch64-apple-ios`, giving `libsweetmtl.a` (bridge + runtime + translator), with `IPHONEOS_DEPLOYMENT_TARGET=16.0`.
  3. `xcrun -sdk iphoneos clang++ -bundle` with the frontend, plugin, shim and `sweet_drm.c`, plus the static libs; `-exported_symbols_list` empty, hidden visibility; frameworks Foundation, Metal, IOSurface, IOKit, CoreFoundation; `-lc++`.
  4. `codesign -f -s -`, then `nm -m | grep "weak external"` (the strchrnul lesson, `METAL-NEXT.md:129-131`).
  5. Upload an artifact with `SweetMetal.bundle/`, `mtltest`, `mtlbridgetest`, `BUILDINFO.txt` and SHA256SUMS.
- The same scripts run in WSL for M0 with `--target x86_64-unknown-linux-gnu` and a native Mesa build.

### 4.5 How it is installed

- **Boot path:**
  - `tools/taimen/sweetmtl-install.sh` → `mkrootfix.py ... --mkdir /System/Library/Extensions/SweetMetal-<hash>.bundle --zfile .../SweetMetal=... --zfile .../Info.plist=... --zfile .../_CodeSignature/CodeResources=...`, plus `--daemon` for the tests;
  - `boot-xnu.sh MTL=1` adds `taimenmtl=SweetMetal-<hash>`.
- **Size:** if the bundle pushes the payload past ABL's ~35 MiB, use the raw-partition route (op 8, `--apps-part system_b`, xnu 0275) or M0b's `devd put`.
- **Per process:** Metal loads the bundle in every process that asks for a device. The plugin's client list decides who gets one; there is no DYLD_INSERT.

## 5. Risks and unknowns (question 5)

1. **a5xx global memory** (blocker): no `ldg`/`stg` path in ir3 for gen 5, and no global atomics. The patch is small if a5xx `ldg` behaves like the UBO path. Tested in M0 (compile) and M3 (run).
2. **The translator against a "virtual" profile.** It was built for real Vulkan 1.2 drivers. It may demand features we only claim (rasterization-order fetch, 16-bit storage, descriptor arrays for argument buffers or heaps in set 1) or emit SPIR-V that `spirv_to_nir` and ir3 a5xx cannot handle. M0 measures it.
3. **Framebuffer-fetch emulation:** a texture barrier per draw (CPU and GPU cost; sysmem only), and no ordering inside one draw. CA draws one quad per draw (`HANDOFF.md:695-696`), so this is fine for CA; apps with overlapping primitives may show artifacts.
4. **Hangs:** until the recovery patch, one bad Metal submit kills GPU rendering for the whole boot, backboardd's GLES included.
5. **Memory:**
   - The Rust translator + Mesa in every Metal client: tens of MB of code and heap **(unverified)**. Extensions such as posters have tight jetsam limits.
   - The GPU heap is 256 MB shared (`taimengpuheap=256`); beyond it come wired bos with per-submit cache maintenance.
   - Flame saw 60-130 MB of GPU memory per process (`HANDOFF.md:130`).
6. **Compile time:** 270 pipelines at 3-30 ms on the SD855 (Turnip). Kryo 280 is slower, and our path adds `spirv_to_nir`. The persistent cache is mandatory; first boots will stutter.
7. **Shared GPU address space:** every process can read any GPU memory (`SweetGPU.cpp:31-34`). That is acceptable for this dev phone only, as the file says.
8. **Plugin discovery details:** whether a nil device in backboardd keeps CA on GLES cleanly, and whether `IOGLBundleName` matters. M1 settles both.
9. **Formats and features apps expect from an Apple-family device:** PVRTC, XR10, depth clamp, dual-source blend, MSAA in sysmem, global atomics. Refusals will be typed, but some frameworks (MPS, CoreImage) may abort instead of falling back. Flame's `MTLFailureTypeSetErrorModeType` helps.
10. **Mesa facts were read at 23.3.5.** Re-check caps, `ir3_a4xx.c` and freedreno batch reordering in 26.3.0-rc2 during M0.
11. **Licensing:** metal2vulkan is GPL-2.0+/LGPL-3.0+. Code we port from its runtime takes that license. Mesa and flame's frontend are MIT.

### Size (honest)

| Part | New lines | Time |
| --- | --- | --- |
| M0 host qualification (scripts, profile, spv2ir3, drm-shim harness, ldg patch draft) | 0.8-1.5k | 2-4 days |
| Runtime crate (Rust) | 6-8k | most of M1-M4 |
| Gallium shim (C) | 2.5-4k | M2-M3 |
| Mesa patches | 0.3-0.8k | M0, M3 |
| Frontend/plugin deltas | 0.3-0.6k | M1, M4 |
| Kernel: plugin name, hang recovery, optional bo-from-user-memory | 0.3-1k | M1, M4 |
| CI, install, devd | 0.5-0.8k | M0b, M1 |
| **Total** | **~10-14k** | **M1: ~1 week; M3: ~3 weeks; M4 with AegirPoster and a couple of apps: 5-8 weeks** |

WebKit's GPU process, Maps at full quality and third-party apps come on top of that, one feature at a time. Most of the uncertainty sits in M0's two gates. If either fails badly, the fallback is plan B (c2) at a higher cost, or keeping Metal-dependent clients off.
