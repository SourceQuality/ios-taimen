# Audit: IOKit personalities and device-tree lookups vs the taimen ADT (2026-10-23)

Read-only review of the patched kernel tree (HEAD "0195 cpu personality kryo280") against what
`tools/mkadt.py --taimen` emits. Trigger: the CPU personality only matched `qcom,kryo` (Pixel 1)
while the taimen cpu nodes say `qcom,kryo280`, so no CPU driver started and the boot thread waited
forever in `ml_wait_max_cpus` (boot 11).

**Headline: no second mismatch of that class exists.** Every built-in personality and every
device-tree lookup on the boot path is satisfied by the taimen ADT with the right name, type and
size. The remaining findings are latent: reachable only with boot-args the boot script sets to 0,
or with `cpus>1`.

## Ranked findings

1. **AppleARMBacklight has no TAIMEN gate** (medium; bites on the first boot without the `safe`
   boot-args). `SweetFramebuffer.cpp:428-431` creates it whenever `sweetbl != 0` (default 1);
   `apply()` ends in `sweet_fbcon_dcs(0x51, level)` (`pe_serial.c:853-891`), a DSI command-FIFO
   sequence written for the Pixel 1 panel and the msm-3.18 DSI (offsets 0x15c/0x17c/0x04c/0x090/
   0x110/0x1ec). The DSI base is derived from the ADT kickoff register, so on taimen it lands on
   the real ctrl0 at 0xc994000 with a different panel (sw43402, DSC). Only triggered by a userland
   brightness set. Fix: `bl_on = 0` under `ARM64_BOARD_CONFIG_TAIMEN` (pattern of
   `SweetBattery.cpp:307-311`).
2. **Topology / SMP is correct for 2x4 Kryo 280, with dependencies to remember** (medium, when
   `cpus>1` is tried). `HAS_CLUSTER` is not defined for SWEET, so `phys_cluster_id` is 0 for all
   cores, `MAX_CPU_CLUSTERS=1`; mkadt emits `cluster-type` 0 so the asserts hold. Do not emit
   P/E cluster types without raising `MAX_CPU_CLUSTERS`. `ml_get_cpu_number` is a linear search on
   Aff1|Aff0, so MPIDR 0x100-0x103 map correctly; `signalCPU` puts Aff1 into ICC_SGI1R; `startCPU`
   passes phys_id to PSCI CPU_ON; `pe_fiq.c:86-114` finds the GICR by affinity over 8 frames.
   `SWEET_SMP_MAX_CPUS` is 8. Keep `cpus=1` until the SGI path to the 0x1xx frames is confirmed.
3. **Pixel 1 GPU present buffers hard-coded** (low, inert): `SweetFramebuffer.cpp:547-549`
   0xc0800000 / 0xc1000000. Unreachable on taimen (`fScaled` requires a 24 bpp boot framebuffer,
   taimen is 32 bpp; `sweet_gpu_init` is not called on TAIMEN). Replace before any GPU work.
4. **`sweetfastboot=N` / `sweetresetfb=1` do nothing on taimen** (low): retargeted constants in
   `sweet_perf.c` but `sweet_perf_init()` is inside the TAIMEN gate (`sweet_ufs.c:663-679`).
5. **Canvas vs framebuffer size disagree** (low, userland only): mkadt emits disp0 canvas
   1125x2436; AppleCLCD publishes the native 1440x2880 because 1440 > `SWEET_FB_LOGICAL_W`.
   `SweetHID.cpp:448-449` SurfaceWidth/Height 62x111 mm are Pixel 1. Align when SpringBoard layout
   is next looked at.
6. **Early-boot addresses are duplicated in code** (low): `start.s:548,714-734`,
   `sweet_early.c:42-50`, `arm_vm_init.c:1866-1869` hard-code 0x9d400000, stride 5760,
   0xc800000, 0xc90201c, 0xc901014; they match mkadt and the Linux DTS today but will not follow
   an ADT change.

## Personalities (`iokit/KernelConfigTables.cpp`, all under `ARM64_BOARD_CONFIG_SWEET`)

| Personality | Match | taimen ADT | Result |
| --- | --- | --- | --- |
| SweetPlatformExpert | IONameMatch `xiaomi,sweet` | root compatible includes `xiaomi,sweet`, `google,taimen`, `qcom,msm8998` | matches |
| SweetCPU | `qcom,kryo`, `qcom,kryo280` (0195) | cpu0..7 `qcom,kryo280`; needs `logical-cpu-id` (set by IOPlatformExpert for topology CPUs) and a 4-byte `reg` | matches; with `cpus=1` the other 7 nubs fail `start()` by design |
| AppleKeyStore, IOWatchdog, IOCoreSurfaceRoot, SweetHIDButtons/Touch, SweetBattery, SweetGPU, IONetworkStack | IOResources | no DT | match (battery forced synthetic; GPU start only allocates a fence page) |
| AppleCLCD | `sweet,display` | `/arm-io/disp0` | matches; geometry comes from `boot_args.Video`, not the ADT |
| SailfishWLAN | IOResources + `sailfishwlan=1` | no `wlan` node on taimen | inert |
| IOPanicPlatform | fallback | | generic |

## Runtime lookups, all satisfied with exact name/type/size

`/chosen` dram-base/dram-size (u64), random-seed, debug-enabled, firmware versions,
unique-chip-id, effective-production-status-ap, certificate-*/security-mode, `/chosen/asmb`;
`/chosen/memory-map` present and empty (TrustCache, RAMDisk, BootCLUT optional); `/defaults`
(pmap tunables optional); `/cpus` state/reg/timebase 19200000 (= DTS 0x124F800); `arm-io`
device_type and ranges; `/arm-io/gic` reg (4 x u64 relative to SOC base → 0x17a00000 /
0x17b00000), `virtual-timer-intid` 19, redistributor stride default 0x20000; `/arm-io/wdt`;
`sweet-fbcon` reg/width/height/stride/bpp 32/kickoff-reg 0xc90201c (derived VIG0 0xc905000, INTR
0xc901014, DSI0 0xc994000 all match the DTS); `/sweet-ufs` reg 0x1da4000/0x2500 (`qcom,ufshc`);
`/product` from d22ap.adt; target-type `D22`, model `iPhone10,3`. Lookups absent on both phones
and handled: interrupt-controller=master, timer device_type, cpu-debug-interface, pram,
socd-trace-ram, `/options`, serial/dockchannel nodes, machine-timeouts.

## Hard-coded addresses reachable on taimen

Only the APSS WDT 0x17817000 (taimen, under ifdef), PSCI CPU_ON (generic), the MDSS addresses
derived from the ADT kickoff register, and the backlight DCS sequence (finding 1). Every Pixel 1
/ sweet driver with MSM8996 or SM7150 addresses (`sweet_keys.c`, `sweet_touch.c`, `sweet_usb.c`,
`sweet_perf.c`, `sweet_rpm.c`, `sweet_rpmh.c`, `sweet_spmi.c`, `sweet_gpu.c`, `sailfish_wlan.c`)
is compiled in (MakeInc.def defines both board configs) but not started on TAIMEN.

## Minimal fix list

1. Force `bl_on = 0` under TAIMEN in `SweetFramebuffer.cpp:428`.
2. Keep `cpus=1` until GICR frames 4-7 / SGI to Aff1=1 are checked; no code change needed.
3. Nothing else needs gating for `rd=ufsroot0`.
4. Later: align disp0 canvas with AppleCLCD's size; move the sweet_perf fastboot thread out of
   the gate if wanted; replace `SWEET_FB_GPU_PA*` before GPU work.
