# sweetmtl: a Metal device on the Adreno 540 (Gallium freedreno)

The Metal driver of `docs/taimen-metal-plan.md` ("metal2gallium"). Flame's native Metal frontend
and bridge run on a runtime of our own. The runtime uses the metal2vulkan translator for shaders
and binding plans and Mesa's Gallium freedreno a5xx driver for the GPU.

**Status (2026-10-25): the PC checks pass, and the iOS test tools build in CI.** The runtime, the
Gallium shim and the bridge build for Linux x86_64 against a host Mesa. `host-check.sh` runs the
bridge's self-test and an ABI test of every bridge call the frontend makes:

- on freedreno's **drm-shim** as an Adreno 540: everything records, compiles with ir3 for gpu 540
  and submits, and the submits are decoded with cffdump. drm-shim executes nothing, so values the
  GPU writes are not checked there.
- on **softpipe**, Mesa's CPU driver, through the same runtime, translator and NIR lowering: there
  the values are checked.

The GitHub job `.github/workflows/sweetmtl.yml` builds the same two test tools for iOS 16 arm64
(`build-ios.sh`). On the phone, mtlbridgetest passes (compute, render and IOSurface import 0 bad)
and mtlabitest passes all but one storage-image texel.

**M1 (built, not yet run on the phone):** the job also builds the Metal plug-in
`SweetMetal.bundle` (`frontend/`) and `mtltest`, which gets its device from Metal.framework's
`MTLCreateSystemDefaultDevice()` through the plug-in. See "M1-M2: mtltest" below.

## Layers

```
Metal.framework (client process)
  frontend/            SweetMetal.bundle: SweetMetal.mm (the plug-in class) + NativeBridgeMetalFrontend.mm (flame's)
    bridge/            NativeMetalObjects.{h,rs}, NativeMetalBridge.{h,rs}: flame's C ABI (nmb_*), MIT
      runtime/         sweetmtl-runtime (Rust): Metal's object model, recording, binder, sync
        m2v/           metal2vulkan translator (720ef1ce + patches/): AIR -> SPIR-V, layouts, binding plans
      gallium/         sg (C): SPIR-V -> NIR -> Gallium CSOs, resources, command replay, fences
        Mesa           freedreno a5xx + ir3 (tools/sweetgl/mesa.patch + m0/mesa-a5xx-ir3.patch)
          phone: tools/sweetgl/sweet_drm.c -> SweetGPU     PC: drm-shim (records) or softpipe (executes)
```

- **Bridge.** Flame's files with `use metal2vulkan_runtime` pointed at `sweetmtl_runtime`. They
  are otherwise unchanged, which keeps their self-test (`nmb_selftest`, steps 0x6501-0x6506) and
  the frontend's ABI as they are.
- **Runtime.** It implements the subset of metal2vulkan-runtime's API that the bridge imports
  (`runtime/src/lib.rs` re-exports every name). It was written from that API; no code was copied,
  so the GPL/LGPL terms of metal2vulkan-runtime do not reach it. The translator crate is a
  dependency.
- **Shim.** One `pipe_screen`, `pipe_context` and `cso_context` per process, behind a device lock.
  Its C ABI is `gallium/sg.h`. Values cross it in Vulkan or Metal terms (VkFormat, Vulkan blend
  enums, Metal sampler and depth enums), and `sg_map.c`/`sg_format.c` map them to Gallium.

## How work flows

- **Shaders.** A library is parsed once by the translator. A pipeline is the translator's
  SPIR-V per stage plus one layout. `assign_slots` (`runtime/src/pipeline.rs`) gives each
  descriptor a Gallium slot, per stage, in (set, binding) order:

  | Descriptor | Gallium slot |
  | --- | --- |
  | push constants | constant buffer 0 |
  | uniform buffers | constant buffers 1.. |
  | storage buffers | SSBOs 0.. |
  | vertex-stage storage buffers (read-only) | constant buffers: a5xx has no VS SSBOs |
  | sampled images, input attachments | sampler views 0.. |
  | samplers | samplers 0.. |
  | storage images | after the SSBOs (a5xx shares one IBO space) |

  `sg_shader.c` takes it from there: `spirv_to_nir`, then the Vulkan resource intrinsics are
  lowered to those slots, and ir3's own NIR passes and `ir3_nir_lower_io` run. The CSO is created
  and its default ir3 variant compiled at once, so a stage ir3 cannot compile fails the pipeline,
  as Metal would.
- **Specialization.** Each specialization the binder asks for gets its own CSO, compiled under the
  lock when first used: workgroup size, threadgroup memory lengths (`SpecKey::TgLength/TgOffset`)
  and function constants.
- **Binding.** At each dispatch or draw, the encoder's Metal argument tables become the binder's
  facts. Then `pipeline.plan(device, shape)` and `instantiate_dispatch/draw` run, and
  `bind.rs::resolve` turns each `WriteSource` into an `sg_ctx_set_*` binding at the stage's slots
  (buffers, inline bytes, textures, samplers, combined image/samplers, immutable constexpr
  samplers, neutral zero/sink buffers). The bindings hold their resources until completion.
- **Recording and replay.** Encoders record `Cmd`s: Gallium-ready shaders, bindings, push bytes
  and state. At `commit`, the command buffer goes to its queue's thread, which replays it into the
  context under the device lock and flushes with a fence. A per-device completion thread waits on
  the fences in submission order, with a 10 s timeout that reports an error. It then copies no-copy
  shadows back, sets events, marks the buffer completed and runs its handlers.
- **Memory.**
  - Buffers are `PIPE_BUFFER` resources kept mapped. `contents` is that mapping (none for
    private storage), and `gpuAddress` is the bo's iova.
  - `newBufferWithBytesNoCopy` is shadowed, because freedreno cannot make a bo from user pages. The
    pages are copied in before a command buffer that uses them runs, and copied back after it
    completes.
  - A texture over a buffer is a linear texture imported over the buffer's own bo (KMS handle,
    `resource_from_handle`), with no copy. a5xx needs 64-byte bases and 64-byte row pitch.
- **Sync.**
  - A signaled event flushes at the signal and takes its value once that submission's fence has
    signaled. An event wait releases the device lock and holds only its own queue.
  - A fence update counts at commit. A fence wait holds the replay until the updates committed
    before it have flushed; there is one ring, so they run in submission order. Waits run before an
    encoder's commands and updates after them.
- **Rendering.** Metal's viewport becomes a negative-height viewport with lavapipe's conventions
  (half-z clip, half-pixel centers, first-vertex flat shading, scissor always on). Load-action
  clears are `pipe->clear`, one per color attachment and one for depth/stencil, right after the
  framebuffer is set. The shim forces
  `FD_MESA_DEBUG=sysmem,inorder`: no GMEM tiling and no batch reordering. That is the base for
  framebuffer fetch through a texture barrier (plan 2.3).

## Coverage of the bridge's calls

The bridge exports 126 `nmb_*` functions. "Not yet" means the runtime returns `Error::NotYet`
naming the missing feature. The bridge passes it to the frontend as a nil/NO result with that
reason, and never substitutes another behavior. "Unsupported" (`Error::Unsupported`) is a typed
refusal for something the a540 cannot do.

| Area | Calls | State |
| --- | --- | --- |
| Device | `device_open/release/name/vendor_name/max_buffer_length/max_threads_per_threadgroup/linear_bytes_per_row/shader_cache_stats/work_stats`, `last_error/warning` | done |
| Device answers | `device_answer`, `device_answer_selector`, `device_family_waivers` | done. 72 private selectors are answered from the a540 profile and freedreno's limits (`families.rs`), and 2 have no answer. `supportsFamily` is false for every Apple family; the waivers list is empty |
| Buffers | `buffer_new/new_no_copy/contents/gpu_address/length/release/set_purgeable_state` | done. No-copy buffers are shadowed; one with a deallocator is not yet. Purgeable state always answers NonVolatile |
| Textures | `texture_new/new_linear/new_on_buffer/new_view/describe/replace/get_bytes/release/set_purgeable_state` | done: 1D/2D/3D/cube/arrays, mip levels, views with swizzle. A texture over a buffer needs freedreno. A linear texture of several images is not yet |
| Libraries, functions | `library_new/function_count/function_name/release`, `function_new/new_specialized/type/release`, `fixture_air`, `function_constant_air` | done (function constants included) |
| Compute | `compute_pipeline_new/new_with_descriptor/new_with_options/execution_width/max_threads/release`, `compute_encoder_new`, `compute_set_pipeline/set_threadgroup_memory`, `compute_dispatch` | done. Indirect dispatch is not yet |
| Render | `render_pipeline_new/new_with_options/release`, `render_encoder_new`, `render_set_pipeline/viewport/scissor/raster/depth_stencil/stencil_reference/depth_bias/blend_color/line_width/visibility_mode`, `render_draw`, `render_draw_indexed` | done: vertex descriptors, blend per attachment, depth/stencil, culling and winding, scissor, primitive restart, 4x multisampling with the resolve store actions (`resolve_texture` in `NmbPassAttachment`; after the pass, Gallium's resolve blit; depth and stencil with the Sample0 filters). Not yet: visibility results, line widths other than 1, depth resolve filters Min/Max, store actions set on the encoder (`MTLStoreActionUnknown` with a resolve texture), passes without attachments, pipelines without a fragment function, base instance (a5xx has no start instance), tessellation, mesh and tile functions (imageblocks), attachments read as storage images. Unsupported: sample-rate shading |
| Encoder state | `encoder_set_buffer/set_buffer_offset/set_bytes/set_texture/set_sampler/end/discard/update_fence/wait_fence` | done. Argument buffers (descriptor bytes, handle tables) and push holes (device addresses a shader reads) are not yet |
| Blits | `blit_encoder_new`, `blit_copy_buffer/fill_buffer/copy_texture/texture_to_buffer/buffer_to_texture/generate_mipmaps` | done. Copies run on the GPU with `resource_copy_region`, and mipmaps with `util_gen_mipmap`. a5xx has no `clear_buffer`, so fills, texture-to-buffer and buffer-to-texture run on the CPU after a wait |
| Command buffers, queues | `queue_new/release`, `command_buffer_new/commit/status/error/gpu_time/on_completed/on_scheduled/wait/wait_scheduled/signal_event/wait_event/uses_buffer/uses_texture/release` | done |
| Sync | `event_new/release`, `shared_event_value/set_value/wait/notify`, `fence_new/release` | done, within one process |
| Heaps | `heap_new/new_buffer/new_texture/size/used_size/allocated_size/max_available_size/set_purgeable_state/release` | not yet (`heap_new` refuses) |
| Binary archives | `binary_archive_new/add_compute/add_compute_descriptor/add_render/release`, `binary_archive_serialize` | adds are accepted and change nothing. Serialize is not yet: the pipeline cache on disk is M3 |
| Self-test | `nmb_selftest` | runs (0x6501-0x6506) |

Shader-side limits are typed errors at pipeline creation:

- 15 uniform buffers besides the push block;
- 24 storage buffers plus images per stage;
- 16 textures and 16 samplers per stage;
- no storage images in the vertex stage;
- vertex-stage storage buffers that are written. They are refused: a5xx reads them as constant
  buffers.
- pointer (buffer-device-address) loads and stores. ir3 has no a5xx global memory ops yet (plan
  risk 1). CoreAnimation's shaders do not use them (M0).

## Build and run on the PC (WSL)

```bash
source ~/tools/m0env.sh                # gcc, meson, ninja, cargo, python3 with mako
tools/sweetmtl/host-check.sh           # [--qc PATH/TO/QuartzCore/default.metallib] [--no-build]
```

It builds:

1. Mesa (the M0 tree: Mesa 26.3.0-rc2 + `tools/sweetgl/mesa.patch` + `m0/mesa-a5xx-ir3.patch`)
   with freedreno, softpipe, drm-shim and cffdump;
2. the shim (`gallium/build-host.sh`, with the compiler flags Mesa's own build gives
   `freedreno_resource.c`);
3. the runtime and bridge (cargo, release);
4. the test tools `mtlbridgetest` (the bridge's self-test) and `mtlabitest` (one check per bridge
   call group, with values), and `profile/gen-formats`.

It then runs:

- the format table check: `profile/a540-gallium.json` against freedreno's answers. Regenerate it
  with `gen-formats` + `profile/make-profile.py`.
- both tests on drm-shim with every submit dumped (`FD_RD_DUMP=combine`). Per submit it prints
  the CP_EXEC_CS, draw, blit and load-state counts and the MRT0 address.
- both tests on softpipe, with values checked.

It exits 0 only if all of that passes. Inputs and outputs default to `../research/m1/`. The
header of `host-check.sh` lists `RESEARCH`, `MESA_SRC`, `MESA_BUILD`, `M2V_SRC`, `LLVM_AS` and
`OUT`. `m2v-fetch.sh` makes the translator checkout (pin + `patches/`), or links an existing one
with `M2V_SRC`.

Environment variables:

| Variable | Effect |
| --- | --- |
| `SWEETMTL_TRACE=1` | one line per compiled stage (ir3 instructions, registers, constlen, local size) and per completed command buffer (time, device counters) |
| `SWEETMTL_PROFILE=FILE` | a different device profile JSON for the translator |
| `SG_DRIVER=softpipe` | the CPU validation driver (PC builds with softpipe) |
| `SG_RENDER_NODE=PATH` | the DRM node on Linux (default `/dev/dri/renderD128`) |
| `SG_DEBUG=1` | print every shim error to stderr as it is set |
| `SG_DEBUG_DRIVER=1` | print the driver's debug messages (ir3, freedreno) |
| `SG_PRINT_NIR=1` | the NIR handed to the driver, per stage |
| `NMB_LIST=1` | the self-test's QuartzCore step lists each function and each refusal |
| `SWEETMTL_DRM_SHIM=1` | (the tests) report GPU-written values as not checkable |
| `FD_MESA_DEBUG`, `FD_RD_DUMP`, `SOFTPIPE_DEBUG=cs` | Mesa's own; the shim always adds `sysmem,inorder` |

### Results (2026-10-25)

drm-shim, FD_GPU_ID=540:

- **Self-test.** 0x6501-0x6505 all run (exit 0), with 3 submits:
  - compute: 2 CP_EXEC_CS, 15 groups of 64 and 1 of 40, each a variant of its own;
  - render: a clear (RB_BLIT) and one draw, MRT0 linear with a 256-byte pitch;
  - import: the same draw, with MRT0 at the no-copy buffer's shadow bo.
- **Shader compiles.** The kernels take 60-69 ir3 instructions. The render pair takes 39 (VS)
  and 222 (FS).
- **mtlabitest.** Every call group records and submits, with no FAIL.
- **QuartzCore** (`--qc`, the iPhone X `default.metallib`, 141 functions): kernels 3 ok, 4
  failed; pairs 13 ok, 8 failed. Every failure comes from the self-test's minimal descriptors, not
  from the runtime. The 4 `tile_downsample_*` kernels need tile attachments. 6 pairs need a
  vertex descriptor and 2 (`path_blit_*`) need 3 color attachments. M0's driver, which builds
  full descriptors, translated 139 of 141.

softpipe:

- **Self-test.** compute 0/1000 wrong, render 0/64 wrong. 0x6505 import stops with a clear error,
  by design: a texture over a buffer needs freedreno.
- **mtlabitest.** All value checks right: sampled render through a constexpr sampler, read and
  write images, the blits and mipmaps, culling, depth, viewport and scissor, multisample resolves
  (color, depth and stencil; 4x faked as 1x), function constants, threadgroup memory, sampler
  arguments, events, and fences across queues. Heaps and archive serialization report not yet.

iOS: see the iOS build section. The Rust half alone also builds in WSL
(`cargo build --release --target aarch64-apple-ios` in `bridge/`, with conda-forge's
`rust-std-aarch64-apple-ios` of rustc's version), which is a quick check before a CI run.

Fixed on the way (worth knowing for the phone): `nir_rewrite_image_intrinsic` leaves the deref
form's SRC_TYPE in the new RANGE_BASE slot (160 for float32), so `sg_shader.c` resets it.
nir_to_tgsi added it to the image index. ir3 does not read it.

## Files

| Path | What |
| --- | --- |
| `frontend/` | the Metal plug-in (M1): `SweetMetal.mm` (class `SweetMTLDevice`, from flame's `SweetMetal2.mm`), `NativeBridgeMetalFrontend.{mm,h}` (flame's frontend with the plan 4.3 changes listed at its top), `Info.plist`, `LICENSE` (MIT: NativeMetal contributors, dipa-studio) |
| `bridge/` | flame's bridge (MIT, `LICENSE`), `build.rs` assembles the two fixtures with `LLVM_AS` (or takes `NMB_FIXTURE_AIR`/`NMB_FC_AIR`). taimen addition: `nmb_buffer_new_iosurface` |
| `runtime/src/` | `device.rs` (device, answers, trace), `families.rs` (families, private selectors, format capabilities), `resource.rs` (buffers, textures, views, heaps), `library.rs`, `pipeline.rs` (pipelines, slots, variants, samplers, depth/stencil, archives), `encoder.rs`, `bind.rs` (binder), `command.rs` (queues, command buffers, replay, completion), `sync.rs`, `ffi.rs` (sg.h by hand), `error.rs`, `iosurface.rs` (buffers over IOSurfaces, by ID) |
| `gallium/` | `sg.h` (the ABI), `sg_device.c` (screen: sweet_drm on iOS, a render node or softpipe on Linux), `sg_resource.c`, `sg_shader.c`, `sg_replay.c`, `sg_format.c`, `sg_map.c`, `sg_iosurface.{c,h}` (IOSurface import by ID: SweetGPU sel 12, iOS only), `build-host.sh` (the library, for the host or, with the iPhoneOS clang, for iOS) |
| `profile/` | `a540-gallium.json` (the translator's device profile: M0's profile + freedreno's format table), `gen-formats.c`, `make-profile.py` |
| `test/` | `mtlbridgetest.c` (runs `nmb_selftest`, like flame's `m2vtest.c`), `mtlabitest.c` (the ABI test), `mtltest.m` (M1-M2 through Metal.framework), `mtltest.clients` (the plug-in's clients file for mtltest boots), `oneshot.h` (their `--daemon` mode), `mtltool-ents.plist` (their entitlements on iOS) |
| `m0/` | M0: the CA shader qualification and `mesa-a5xx-ir3.patch` (`m0/README.md`) |
| `m2v-fetch.sh`, `patches/` | the translator checkout at 720ef1ce with flame's patches 0001, 0004, 0005 (`m2v/`, not in git) |
| `host-check.sh` | the PC check above |
| `build-ios.sh` | the iOS build of the test tools, the plug-in bundle and mtltest (macOS; run by `.github/workflows/sweetmtl.yml`) |
| `../taimen/sweetmtl-install.sh` | a CI build into `../out/sweetmtl/run-ID/` and the ROOTFIX blob for one phone test |

## iOS build (plan 4.4)

The PC has no iOS SDK, so the iOS binaries come from the GitHub job `.github/workflows/sweetmtl.yml`
(macos-14, Xcode 15.4 with the iPhoneOS 17.5 SDK, deployment target 16.0). It runs on pushes to
`taimen` and `metal-*` that touch `tools/sweetmtl/`, the workflow or the sweetgl files the build
takes (the Mesa pin and patch, the cross setup, `sweet_drm.c`), and by hand (workflow_dispatch).
The first green run is
[40203190936](https://github.com/SourceQuality/ios-taimen/actions/runs/40203190936) (commit
dad0d40, under 6 minutes).

1. **Mesa.** `tools/sweetgl/mesa-fetch.sh` and `mesa-setup.sh`, as sweetgl's job runs them, with
   `MESA_SRC=research/mesa-mtl` and `MESA_BUILD=research/mesa-ios-mtl`, plus
   `m0/mesa-a5xx-ir3.patch`. The trees are separate from sweetgl's because the patch's triops
   change must not reach sweetgl before M0's open question is settled.
2. **Translator.** `m2v-fetch.sh` clones the pin and applies the three patches. host-check gives
   the same results with that checkout as with M0's.
3. **`build-ios.sh`.**
   - ninja builds Mesa's static libraries.
   - `gallium/build-host.sh`, given the iPhoneOS clang, compiles the shim and
     `tools/sweetgl/sweet_drm.c` with the flags of the iOS build's freedreno.
   - cargo (Rust 1.99.1, the PC's) builds the bridge's static library for `aarch64-apple-ios`.
   - llvm-as (brew `llvm@21`) assembles the fixtures. mtlabitest carries its three in the binary,
     so it needs no fixture directory on the phone.
   - clang links each tool with IOKit, CoreFoundation, IOSurface, libc++ and libz
     (`-dead_strip`). The result is stripped (`strip -x`) and signed ad hoc with
     `test/mtltool-ents.plist` (`platform-application`, as netsvcd has).
   - The build fails if `nm -m` shows a weak import (`(undefined) weak external`): a symbol
     newer than iOS 16.0 would be NULL on the phone (the strchrnul lesson).
4. **Artifacts.**
   - `sweetmtl`: `mtlbridgetest`, `mtlabitest`, `BUILDINFO.txt` and `SHA256SUMS`. BUILDINFO
     records the commit, the run, Mesa, the patches, the translator, rustc, LLVM, Xcode and the
     SDK, plus the mkrootfix arguments with this build's hashes.
   - `sweetmtl-symbols`: unstripped copies for symbolication.

Run 40203190936 made `mtlbridgetest` 23.4 MB and `mtlabitest` 23.6 MB. Compressed with zlib -9,
which is what `--zfile` stores, each is 6.5 MiB. Both are arm64 with minos 16.0 and sdk 17.5. They
link IOKit, CoreFoundation, IOSurface, libc++, libz and libSystem, carry an ad hoc signature with
`platform-application`, and have no weak imports. On the PC, the same tools peak at 34 MB resident
with the QuartzCore step included.

### M1: the plug-in bundle and mtltest

- **`SweetMetal.bundle`.** `xcrun -sdk iphoneos clang++ -bundle` links `frontend/SweetMetal.mm` and
  `NativeBridgeMetalFrontend.mm` (ObjC++, ARC, hidden visibility) with the libraries the tools
  link, plus Foundation and Metal.
  - Nothing is exported (`-exported_symbols_list` with no names), so `-dead_strip` drops the
    bridge calls the frontend does not make. Metal finds the class `SweetMTLDevice` through the
    Objective-C runtime.
  - `_MTLDevice`, the class's private superclass, is in the SDK's Metal.tbd. The build checks that
    the bundle binds it to Metal and looks nothing up in the flat namespace.
  - The bundle holds the executable `SweetMetal`, stripped, plus `Info.plist`
    (`NSPrincipalClass` `SweetMTLDevice`) and `_CodeSignature/CodeResources`. It is signed ad hoc
    as a bundle, and the same weak-import check runs on it.
- **`mtltest`.** `test/mtltest.m`, Objective-C. It links only Foundation, Metal, IOSurface, IOKit
  and CoreFoundation, and is signed like the other tools.
- **Artifacts.** `sweetmtl` also holds `SweetMetal.bundle/`, `mtltest` and `clients` (a copy of
  `test/mtltest.clients`). `sweetmtl-symbols` also holds `SweetMetal.unstripped` and
  `mtltest.unstripped`. BUILDINFO prints the mtltest install with this build's hashes.

What the plug-in does (`frontend/SweetMetal.mm`; plan 4.3):

- **Clients.** It gives a device only to processes named in `/private/var/sweetmtl/clients`:
  one name per line, `#` starts a comment. Any other process gets nil, and Metal skips the plug-in
  there. backboardd is never listed before M5, so it stays on GLES.
- **GPU wait.** It waits for SweetGPU for `SWEETMTL_GPU_WAIT` seconds (default 20). If the GPU does
  not come up, it returns nil.
- **Environment.** It applies the `KEY=VALUE` lines of `/private/var/sweetmtl/env`.
- **Shader cache.** The cache is `/private/var/sweetmtl/cache/<process>`. A sandboxed process uses
  its own caches directory instead.
- **Log.** It logs `SweetMetal[pid]: ...` lines to stderr.

What changed in the frontend compared with flame's:

- **No Turnip.** The loader argument is ignored.
- **IOSurfaces are imported by ID.** `nmb_buffer_new_iosurface` goes through
  `runtime/src/iosurface.rs` and `gallium/sg_iosurface.c`, SweetGPU sel 12, and freedreno's
  `resource_from_handle` makes a `PIPE_BUFFER` over the surface's bo. The texture over that buffer
  is the surface itself, so there are no copies.
  - If the import by ID is refused, the frontend falls back to the host-pointer import (the
    shadowed no-copy buffer).
  - `SWEETMTL_IOSURFACE=host` forces the host-pointer import.
- **Plane 0 of 'b3a8' surfaces** is accepted.
- **No cache maintenance of surfaces.** The kernel cleans surfaces at each submit (xnu 0242), and
  shadowed pages are never touched by the GPU. `SWEETMTL_SURFACE_FLUSH=1` brings flame's
  `dc civac` back for debugging.

## Phone tests (M1-M3)

The runtime already covers M2 and most of M3's list. The first phone runs are therefore the two
test tools. They need no Metal.framework, no plugin and no kernel patch: they open SweetGPU
through `sweet_drm.c`, as sweetgl does.

### `--daemon`: one run per boot as a launchd job

`mkrootfix.py --daemon` makes RunAtLoad + KeepAlive jobs (ThrottleInterval 10 s) with stdout and
stderr on /dev/null. A tool that exits would therefore start again every 10 s. With `--daemon` as
its first argument, a tool (`test/oneshot.h`):

1. line-buffers stdout, so the log keeps every line written before a crash;
2. turns the runtime's trace on, which logs each compiled stage and each completed command buffer;
3. waits for the GPU service, 180 s at most. SweetGPU comes up 10-20 s into boot.
4. runs once per boot. `/private/var/tmp/<tool>.ran` holds the boot time and is written just
   before the run. If launchd restarts the tool after a crash, it finds the marker and does not
   run again. Remove the marker, or reboot, to run it again.
5. prints `<tool>: done, status N; idling` and stays in `pause()`.

Without `--daemon` the tools run once and exit with their status, as on the PC.

### Install and boot (one tool per boot)

The output reaches the XNU log as `|mtlbridgetest| ...` lines when the process name is in boot-arg
`sweetlogp` (xnu 0308). `sweetnull=backboardd,mtlbridgetest` (xnu 0048) works too. The binary
goes in a directory named after its hash so that the process name stays `mtlbridgetest`; ROOTFIX
writes only absent files, so a new build needs a new path.

From the port checkout in WSL, with `RUN` the CI run id (`gh run list -w sweetmtl.yml`):

```bash
gh="/mnt/c/Program Files/GitHub CLI/gh.exe"
d=../out/sweetmtl/run-$RUN; mkdir -p "$d"
"$gh" run download $RUN -R SourceQuality/ios-taimen -n sweetmtl -D "$(wslpath -w "$d")"
(cd "$d" && sha256sum -c SHA256SUMS)
h=$(sha256sum "$d/mtlbridgetest" | cut -c1-8)
tools=(); [ -f ../out/phone-tools/netsvcd ] && tools=(--phone-tools ../out/phone-tools)
[ -f ../out/phone-tools/RedwoodAudio ] && tools+=(--hal-plugin ../out/phone-tools)
python3 -I tools/taimen/mkrootfix.py ../research/ipsw-x/ios16-root.img ../research/taimen/rootfix-sweetmtl.bin \
  "${tools[@]}" \
  --zfile /usr/local/libexec/mtlbridgetest-$h/mtlbridgetest="$d/mtlbridgetest" \
  --daemon com.sweet.mtlbridgetest=/usr/local/libexec/mtlbridgetest-$h/mtlbridgetest,--daemon,/private/var/sweetmtl/cache,/System/Library/Frameworks/QuartzCore.framework/default.metallib
ROOT=1 ROOTFIX=../research/taimen/rootfix-sweetmtl.bin ROOTFIX_KEEP=1 \
  tools/taimen/boot-xnu.sh KERNEL OUTDIR sweetlogp=mtlbridgetest
```

How the pieces fit:

- **GPU boot.** It needs a GPU boot (ROOT=1, GPU=1, sweetgl installed). `ROOTFIX_KEEP=1` makes
  boot-xnu.sh use this blob instead of sweetgl's.
- **Size.** The blob leaves out sweetgl's `--zfile`, because the dylib is on the phone already and
  ROOTFIX only writes absent files. That keeps the payload under ABL's ~35 MiB: the tool's 6.5 MiB
  plus a development kernel's ~17 MB.
- **Phone tools.** They are passed as `sweetgl-install.sh` passes them. mkrootfix rewrites
  launchd.plist from the root image, so netsvcd's job must be in every blob.
- **mtlabitest.** Install it the same way, in a later boot:
  - `--zfile /usr/local/libexec/mtlabitest-$h/mtlabitest=...`
  - `--daemon com.sweet.mtlabitest=/usr/local/libexec/mtlabitest-$h/mtlabitest,--daemon`
  - boot-arg `sweetlogp=mtlabitest`
- **Removal.** A later blob without the `--daemon` removes the job.

### What the first mtlbridgetest run shows

On success (trace lines abbreviated):

```
|mtlbridgetest| mtlbridgetest: GPU service up after 0.0 s
|mtlbridgetest| mtlbridgetest: --daemon run, boot 1761400000.123456
|mtlbridgetest| QuartzCore metallib: <size> bytes
|mtlbridgetest| [  0.000 s] step 6501 device
|mtlbridgetest| [  0.0xx s] step 6502 fixture library
|mtlbridgetest| [  0.0xx s] step 6503 compute
|mtlbridgetest| sweetmtl: kernel compiled: 60 ir3 instructions, 5 full registers, constlen 28, local size 1x1x1
|mtlbridgetest| sweetmtl: command buffer Completed (1 commands, ... ms); device: 1 flushes, 2 dispatches, ...
|mtlbridgetest| [  ...  s] step 6504 render
|mtlbridgetest| [  ...  s] step 6505 import
|mtlbridgetest| [  ...  s] step 6506 QuartzCore
|mtlbridgetest| [  ...  s] step 650f done
|mtlbridgetest| result 0 stage 0 (... s)
|mtlbridgetest| compute bad 0/1000
|mtlbridgetest| render bad 0/64
|mtlbridgetest| import bad 0/4096
|mtlbridgetest| QuartzCore: 141 functions, kernels 3 ok 4 failed, vertex/fragment pairs 13 ok 8 failed
|mtlbridgetest| message:
|mtlbridgetest| mtlbridgetest: done, status 0; idling
```

**Pass:**

- `result 0`, and compute, render and import all bad 0;
- the QuartzCore counts as on the PC (the failures are the self-test's minimal descriptors);
- no GPU hang;
- backboardd still drawing with sweetgl.

This is the first time a second GPU client runs next to backboardd's sweetgl, and the first
execution of the shim on the A540. The other outcomes and what they mean:

| Log | Meaning |
| --- | --- |
| `no GPU service (SweetGPU) after 180 s; idling` | not a GPU boot (`sweetgpu=` missing), or SweetGPU did not start |
| `result 6501 stage 6501`, a message | the device did not open; the message is the shim's reason |
| `result 650x` with `compute/render/import bad N` | the step ran, and the GPU wrote wrong values: the first thing to debug, with the trace lines before it |
| `command buffer Error` and `the GPU did not finish the command buffer in 10 s` | the runtime's completion timeout: a GPU hang. Until the A540 hang recovery (plan M4) the kernel keeps the GPU off for the rest of the boot, so backboardd's GLES stops too. |
| `ran already this boot ...; idling` right after the first lines | the tool died (crash, jetsam) and launchd restarted it; the lines before the restart say where |

### mtlabitest, then M1-M3

1. **mtlabitest.** It must print the same lines as softpipe on the PC: every check `ok`, the
   `stage_io` check `ran`, heaps and archive serialization `not yet`, `FAIL lines: 0`. Things
   only hardware settles:
   - VS storage buffers read as UBOs;
   - the 64-byte linear pitch and offset of textures over buffers (mesa.patch's pitchalign 6);
   - the CPU texture paths after `sync_all`;
   - the triops question;
   - GPU timestamps, which are taken on the host today.
2. **M1 and M2: mtltest.** See the next section.
3. **M3.** The self-test's 0x6506 with full descriptors (M0's `m0drive.py` pairing) instead of
   the minimal ones, and the compositor pair (`fixed_vert_lph_spc` + `fixed_frag_lph_cph`) drawn
   with CA-like buffers. The pipeline cache on disk (`binary_archive_serialize`, the translator's
   `to_bytes`) is the M3 runtime work left.

### M1-M2: mtltest

`mtltest` gets its device the way an app does: `MTLCreateSystemDefaultDevice()` in Metal.framework.
That takes three pieces, all installed by one ROOTFIX blob:

- **The kernel.** Boot-arg `taimenmtl=SweetMetal-<hash8>` (xnu 0329) names the plug-in on the
  `IOAcceleratorES`: `MetalPluginName` is the bundle's name, and `MetalPluginClassName` is
  `SweetMTLDevice` (the default).
- **The bundle.** It goes to `/System/Library/Extensions/SweetMetal-<hash8>.bundle/`, where
  `<hash8>` is the first 8 hex digits of the executable's SHA-256. ROOTFIX writes only absent
  files, so each build gets its own path.
- **The clients file.** mkrootfix `--write` (written even if present) puts the artifact's
  `clients` file at `/private/var/sweetmtl/clients` at every boot of the blob. It lists `mtltest`
  only, so backboardd and SpringBoard get nil and stay on GLES. `sweetmtl-install.sh` refuses a
  clients file that lists either of them, and mtltest FAILs on one.

  Other ways to install it were considered and not taken. With mkrootfix `--file`, a stale list
  would survive. mtltest writing the file itself would make a test tool change the system's
  configuration.

From the port checkout in WSL:

```bash
tools/taimen/sweetmtl-install.sh -r RUN mtltest     # or no -r: the newest green run on taimen
ROOT=1 ROOTFIX=../research/taimen/rootfix-sweetmtl-mtltest.bin ROOTFIX_KEEP=1 \
  tools/taimen/boot-xnu.sh KERNEL OUTDIR taimenmtl=SweetMetal-<hash8> sweetlogp=mtltest
```

The script prints the exact boot-args, and `../out/sweetmtl/current.env` records them. BUILDINFO
lists the same mkrootfix arguments:

- 3 `--zfile` for the bundle;
- `--mkdir /private/var/sweetmtl`;
- `--write /private/var/sweetmtl/clients=...`;
- `--zfile` and `--daemon` for mtltest.

What mtltest checks. Each check prints one line, `ok`, `info` or `FAIL`. The run ends with
`mtltest: done, status N; idling`, where N is the number of FAIL lines.

- **M1.**
  - The clients file.
  - The `IOAcceleratorES`: its registry ID and plug-in names.
  - That the bundle is installed.
  - `MTLCreateSystemDefaultDevice` and `MTLCopyAllDevices`.
  - The device's class chain (`NativeBridgeMetalDevice < _MTLDevice < ...`), its name, and that
    its registryID equals the accelerator's.
  - Its limits and `supportsFamily:` answers.
  - `newBufferWithLength:` (zeroed, `contents` read back, `gpuAddress`) and `newBufferWithBytes:`.
- **M2.**
  - An empty command buffer.
  - `fillBuffer` and `copyFromBuffer`.
  - A render pass clearing a 64x64 BGRA8 texture (load Clear, store), read with `getBytes` and
    a texture-to-buffer blit.
  - The same clear into BGRA8 textures over new IOSurfaces (`newTextureWithDescriptor:iosurface:plane:`),
    read back through `IOSurfaceGetBaseAddress` and through `getBytes`:
    - 64x64 and 1125x48 (backboardd's 4544-byte rows) with the default import, by ID;
    - 64x64 with `SWEETMTL_IOSURFACE=host`.

  The plug-in logs the import each surface got ("imported by ID" or "imported as host pages").

The plug-in's own lines (`SweetMetal[pid]: ...` and the frontend's `native-metal: ...`) go to
stderr, so they appear among mtltest's `|mtltest|` lines. backboardd's line ("not in
/private/var/sweetmtl/clients, so no Metal device") appears under the GPU boot's `sweetnull=backboardd`.

**Pass:**

- every line `ok` (status 0);
- backboardd still drawing with sweetgl at about 60 frames/s;
- no GPU hang.

If the clear is right only with `SWEETMTL_IOSURFACE=host`, the import by ID is at fault. Try a
boot with `SWEETMTL_SURFACE_FLUSH=1` in `/private/var/sweetmtl/env` (mkrootfix `--write`): it
tells CPU cache trouble apart from a wrong bo.

## Known gaps and risks

- **Not built yet:** heaps, argument buffers and handle tables, push holes, global memory (the
  ir3 `ldg`/`stg` patch), pipeline variants beyond specialization (`Plans` refuses a non-empty
  variant), indirect dispatch and draws, visibility queries, base instance, the disk cache.
- **Multisampling on the phone.** 4x passes and their resolves record on drm-shim (the resolve is
  u_blitter's averaging shader, four `isamm` fetches) but have not executed on the a540; softpipe
  fakes 4x as 1x (Gallium's `fake_sw_msaa`), so its check covers the resolve's plumbing, not the
  averaging.
- **Neutral images.** An image binding where nothing is bound is refused, not given a dummy
  texture.
- **Framebuffer fetch.** The texture-barrier emulation is wired in the profile and in sysmem mode
  but has not been exercised by a test here.
- **No-copy buffers** cost two copies per command buffer until a SweetGPU bo-from-user-memory
  selector exists (plan 4.2, optional kernel patch).
- **A shader that hangs the GPU** takes GLES down with it until the A540 hang-recovery patch
  (plan M4).
