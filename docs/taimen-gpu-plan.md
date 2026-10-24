# Plan: GPU acceleration on taimen (Adreno 540) from XNU (2026-10-23)

**Status 2026-10-24: E1-E6 done.** CoreAnimation draws with Mesa on the A540 in backboardd at ~60
frames/s during home screen animations (boot 75), and ROOT=1 boots use it by default
(`tools/taimen/boot-xnu.sh`, `tools/taimen/sweetgl-install.sh`). E1-E5: xnu 0227-0237, boots 47-59.
E6: boots 60-75 in NOTES-taimen.md; the steps that took it from ~1 to ~60 frames/s:

| What | Where | Effect |
| --- | --- | --- |
| The first EAGL context waits for the GPU service | sweetgl `eagl.c` (`gpu_ready`) | CA no longer stays on its software renderer when backboardd starts before the GPU (boot 65) |
| A5xx texture barrier inside the batch in GMEM bypass | `tools/sweetgl/mesa.patch` (`fd5_texture_barrier`) | a destination-read draw no longer flushes the batch (one GPU submit per such draw, boot 69) |
| Mesa's buffers write-combined from a 256 MB heap above XNU's RAM | xnu 0240 (`taimengpuheap`) | no cache clean and write-protect of Mesa's pages per submit (boot 72 profile) |
| Imported surfaces flushed only when their seed moved | xnu 0242 | the display surfaces (~3400 pages) no longer cleaned at every submit (boot 74: ~4 ms each) |
| Imported-surface cache bounded to 96 MB | sweetgl `eagl.c` (`SURF_RES_BYTES`) | SpringBoard's per-swipe snapshot surfaces no longer stay wired |

The rest of this file is the plan as written before any of it ran.

Plan only: nothing was built, booted or committed. Sources read: `scratch/xnu` at `fb73534` (patches
0001-0223; line numbers below refer to that tree), `port/` (NOTES-taimen.md, docs, tools/sweetgl,
tools/taimen), `research/taimen/live.dts`, the phone's own LineageOS boot log and memory map
(`hw/dmesg.txt`, `hw/iomem.txt`), Linux v6.12 (drm/msm adreno, clk/qcom gpucc/gcc/alpha-pll/gdsc,
msm8998.dtsi and two msm8998 boards, qcom_scm, mdt_loader, spmi-pmic-arb, qcom_spmi-regulator, rpmpd,
arm-smmu-qcom), the Pixel 2 msm-4.4 kernel (LineageOS `android_kernel_google_wahoo`, branch
`lineage-19.1`: kgsl `adreno_a5xx.c` and `adreno-gpulist.h`, `clock-gpu-8998.c`, `clock-alpha-pll.c`,
`subsys-pil-tz.c`, `peripheral-loader.c`, `msm-clocks-8998.h`) and Mesa 25.3.6 (freedreno). Linux and
Mesa line numbers refer to those versions:
https://github.com/torvalds/linux/blob/v6.12/ ,
https://github.com/LineageOS/android_kernel_google_wahoo/blob/lineage-19.1/ ,
https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-25.3.6/ .
Statements marked **(unverified)** are inferences that no log, file or source in hand confirms.

## 0. Short version

- No GPU code runs on taimen today, and none may run until a TAIMEN branch exists. `sweet_ufs.c:664-676`
  starts only touch and the perf probe, so `sweetgpu=` is ignored. Below the command processor,
  `sweet_gpu.c` knows only MSM8996 blocks: MMCC 0x8c0000, GPU 0xb00000, SMMU 0xb40000, and PMI8994 S2
  through the MSM8996 SPMI arbiter. On MSM8998, 0x8c0000 is the camera block (`live.dts:5431-5433`)
  and the other two are unclaimed.
- Above the clock controller the A540 is an A530 with about ten different register values. It has the
  same CP microcode (a530_pm4/pfp), packets, RBCCU/SP power islands and SMMUv2 layout (CB0 at +0x8000,
  SID 0 user, SID 2 secure), the same PAS id 13 and 1 MiB of GMEM. The differences:
  - VBIF_GATE_OFF_WRREQ_EN 9, CP_MERCIU_SIZE 0x400, UCHE_DBG_ECO_CNTL_2 = 2;
  - the LMLOADKILL quirk; ME_INIT workaround 0;
  - chip id 0x05040001;
  - A540 ECO values in the kernel's own compute state;
  - the 4K-page fixes `taimen-4k-catalogue.md` already lists.
- The power-up is new code, about 300 lines:
  - GPU CC at 0x5065000: CX GDSC; GX BCR, GMEM clamp/reset and GX GDSC; the gfx3d RCG, fed by XO or by
    the GPU CC's own Fabia PLL. Neither Linux driver feeds gfx3d from GPLL0, as MSM8996 did.
  - GCC branches at 0x171004, 0x171010, 0x146040 and 0x188010.
  - The GFX rail, PM8005 S1 (ppid 0x514). Linux drives it over SPMI from the APPS EE, not through the
    RPM. XNU has no SPMI on taimen yet. The arbiter has the v2/v3 layout that `sweet_keys.c` already
    speaks, at new addresses.
  - CPR4 stays off: one fixed corner at the DT ceiling voltage, open loop.
  - MX/CX levels go to the RPM (`rwmx`/`rwcx`, key `vlvl`) over the GLINK path that already drives the
    touch rails.
- The GPMU/LM firmware (a540_gpmu.fw2) is optional. Mainline runs the A540 without it. Leave it for
  later and stay at or below 596 MHz.
- Zap shader: PAS id 13 (`live.dts:8747`). Android loaded only `a540_zap.mdt` and `.b02`
  (`dmesg.txt:2174-2194`), so there is one loadable segment. The PIL node has no memory-region, so if
  the segment is not relocatable it must go to its own p_paddr.
  - 0x95A00000 (`pil_ipa_gpu`, 1 MiB, outside Linux RAM) is the likely place (**unverified** until the
    ELF header is read).
  - On the 4K kernel the metadata no longer fits in one page and lands above 4 GB. Give it contiguous
    memory below 4 GB, as mainline does.
- SMMU writes from EL1 are what Linux does on this SoC: mainline programs SMR, S2CR and CB for
  msm8998. Under QHEE they are probably trapped and emulated, as on the Pixel 1.
  - Keep the Pixel 1 rules: no `str wzr`, and an exact SMR for SID 0 only.
  - Add three: scan the SMRs first, use a free index, never touch the secure CB.
- The firmware is not on this PC. `backup/` holds 22 partitions with no vendor_a/vendor_b image, and
  `research/taimen/firmware/` does not exist. NOTES-taimen.md:41-43 describes an earlier machine.
  Getting the five files is the first prerequisite (section 6).
- User space: sweetgl.dylib must be rebuilt.
  - `sweet_drm.c:180-181` reports GPU_ID 530, while Mesa picks the A540 ECO bits by gpu_id 540
    (`fd5_emit.c:967-979`).
  - The scripts point at a Mac: an Xcode 15.4 tree and /Volumes paths. WSL2 here has no clang, lld,
    meson, ninja or iOS SDK.
  - A macos-14 GitHub job, like `xnu.yml`, is the shortest path. A WSL2 build with Ubuntu clang and
    ld64.lld also works, but needs an iPhoneOS SDK from the user.
  - The Mesa checkout was ≥ 25.3.0 (glapi layout). Pin mesa-25.3.6.
  - `eagl.c` hard-codes QuartzCore addresses of the iPhone 8 shared cache, and taimen runs the iPhone X
    root.
- Present path: the GPU can write straight into SCAN0/SCAN1, because 0x9e400000 and 0x9d400000 sit
  inside its identity map.
  - That removes the CPU memcpy: 11.2 MB per frame at 0.4-1.3 GB/s, 9-28 ms on a silver core.
  - QSEED3 keeps scaling.
  - This alone does not fix 2-8 frames/s, because CoreAnimation still renders on the CPU. Mesa in
    backboardd (E6) does.
  - Zero-copy scan-out of low-memory swap surfaces is a later step.
- Experiment ladder:
  - E0: read-only census.
  - E1: power at XO, then the PLL.
  - E2: CP microcode.
  - E3: SCM probe, zap shader, blits.
  - E4: compute, service mode, 596 MHz.
  - E5: GPU copy in present.
  - E6: Mesa in backboardd.
- Effort **(unverified estimates)**:
  - Prerequisites: about 2 days.
  - E0-E4: about 1 week and 10 boots.
  - E5: about 1 day.
  - E6: 3-5 days to first frames, then open-ended renderer fixes.

## 1. The audit checked against the live DT and the phone's own Linux

| item | finding | live.dts | this phone's Linux | verdict |
|---|---|---|---|---|
| GPU registers | 0x5000000 + 0x40000 | 8764-8769 | iomem.txt:21 `kgsl-3d0` | as audited |
| GPU IRQ | `<0 0x12C 0>` = SPI 300, INTID 332 | 8770 | - | as audited; polled anyway |
| chip id | 0x5040001 = A540 patch 1 (kgsl a540v1 = patch 0) | 8773 | - | as audited; ME_INIT workaround 0 |
| GPU CC | `qcom,gpucc-8998-v2` 0x5065000 + 0x9000; `qcom,gfxcc-8998-v2` at the same base (gfx3d, PLL) | 2430-2450 | dmesg:241, 298 | as audited |
| GDSCs | CX 0x5066004 (hw_ctrl 0x5066008); GX 0x5066094, domain_addr 0x5065130, sw_reset 0x5066090, `retain-periph`, `reset-aon-logic`, root clk gfx3d_clk_src forced on, parent gfx_corner | 658-680 | dmesg:385 `gdsc_gpu_gx: supplied by gfx_corner` | new |
| GCC | `qcom,gcc-8998-v2` 0x100000 + 0xB0000 | 2408-2410 | - | as audited |
| GFX rail | PM8005 S1: SID 5, base 0x1400, `qcom,qpnp-regulator` (APPS SPMI, not RPM), 516-1088 mV, enable-time 500 us | 1937-1950 | - | as audited |
| CPR | `cpr4-ctrl@5061000` (`qcom,cpr4-msm8998-v2-mmss-regulator`): vdd pm8005_s1, mem-acc gfx_mem_acc_corner, 8 corners, fuses at 0x784000 | 4937-4990 | dmesg:269-275: fused open loop 586/678/832/1024 mV, "closed-loop"; dmesg:296-297: OPP 257 MHz 648 mV, 710 MHz 988 mV | as audited |
| mem-acc | `regulator@1fcf004` bit 0, corner-acc-map <1 0>; gfx corners 1-3 -> 1, 4-8 -> 0 | 4974, 4994-5003 | dmesg:1054 | new (porting map:172 has "?") |
| CX / MX | gpucc vdd_dig = pm8998_s1_level (RPM `rwcx` 0); gfxcc vdd_mx/vdd_gpu_mx = pm8998_s9_level (RPM `rwmx` 0) | 2434, 2444-2446, 2702-2722, 2839-2858 | - | new |
| freq -> corner | 180/257/342/414/515/596/670/710 MHz -> corners 1-8, MX 0x80 up to 414, 0x100 at 515/596, 0x180 at 670/710 | 2448-2449 | - | new |
| GPU SMMU | 0x5040000 + 0x10000, `qcom,skip-init`, `qcom-tz-device-id = "GPU"`, vdd gdsc_gpu_cx, context IRQs SPI 329-331 | 5370-5386 | iomem.txt:22; dmesg:551-552 | "TZ-owned" is too strong: Linux programs SID 0 / CB0 from EL1 (section 3.1) |
| SIDs | gfx3d_user SID 0, `qcom,gpu-offset 0x48000` (= CB0 at SMMU + 0x8000); gfx3d_secure SID 2 | 8902-8923 | - | as audited |
| zap | `qcom,kgsl-hyp`: pil-tz-generic, pas-id 0xD, `a540_zap`, **no memory-region** | 8745-8749 | dmesg:535, 2174-2194: `.mdt`, then `.b02` only, "Brought out of reset" | PAS 13 confirmed; carveout unverified |
| pil_ipa_gpu | node named @95200000, reg 0x95A00000 + 1 MiB, removed-dma-pool; nothing references phandle 0x3D8 (ipa_fws has none either, 2599-2603) | 16387-16391 | dmesg:8, 49; System RAM skips 95a00000-95afffff (NOTES:29-30) | address confirmed, role unverified |
| GPMU | a540_gpmu.fw2, version 3.0, lm-limit 6000, max-power 5448, `gpu-quirk-lmloadkill-disable`, highest-bank-bit 15 | 8776-8790 | dmesg:2158 | as audited |
| kgsl clocks | core gpucc_gfx3d_clk, iface gcc_gpu_cfg_ahb_clk, rbbmtimer, mem gcc_bimc_gfx_clk, mem_iface gcc_gpu_bimc_gfx_clk, isense, rbcpr, iref gcc_gpu_iref_clk (hashes decoded with wahoo `msm-clocks-8998.h:104, 195-202, 474-483`) | 8791-8792 | - | new |
| kgsl supplies | vddcx = gdsc_gpu_cx, vdd = gdsc_gpu_gx | 8801-8803 | - | new |

The audit also says a540_zap has exactly one PT_LOAD of 0x1334 bytes. That cannot be checked on this
PC, because there is no firmware here. The dmesg is consistent with it.

## 2. Power-up: what in sweet_gpu.c is MSM8996, and what MSM8998 needs (question 1)

### 2.1 MSM8996-specific parts of `bsd/dev/sweet_gpu.c`

| lines | what | MSM8998 |
|---|---|---|
| 1-75 | header: A530 recipe, levels 1-8 | keep the level numbers; TAIMEN recipe below |
| 111-119 | MMCC_PA 0x8c0000, GCC_PA 0x300000, GPU_PA 0xb00000, MEMACC_PA 0x7af000, GFX_S2 0x317, ZAP_PA 0x90f00000, SMMU_PA 0xb40000 (GPU_PAS_ID 13) | GPUCC 0x5065000, GCC 0x100000, GPU 0x5000000, mem-acc 0x1fcf004 bit 0, S1 ppid 0x514, zap per its ELF header (3.3), SMMU 0x5040000; PAS 13 unchanged |
| 222-223, 1196-1203 | `mmcc` and `gfx_clock()`: MMCC gfx3d RCG 0x4000/0x4004, source 5 = GPLL0, half-step divider | GPUCC gfx3d RCG 0x1070/0x1074 has only source 0 = XO and source 1 = GPUPLL0 out_even (mainline `gpucc-msm8998.c:103-111, 132-149`); GPLL0 (source 5) feeds only rbcpr/rbbmtimer/isense (`:93-100`; wahoo `clock-gpu-8998.c:39-40`) |
| 282-290 | `dump()` of MMCC/GCC registers | GPU CC / GCC census (E0) |
| 292-306 | `branch_on` / `gdsc_on`: CBCR bit 0 + CLK_OFF poll; SW_COLLAPSE + PWR_ON | same semantics. The GCC GPU branches have unreliable halt bits (mainline BRANCH_HALT_SKIP, `gcc-msm8998.c:2112-2163`): log them, don't gate on them. The CX GDSC reports in GDS_HW_CTRL 0x1008 (`gdsc.c:60-91`) |
| 328-375 | 16 KB ring in a PAGE_SIZE page | 4K bug (`taimen-4k-catalogue.md:87-93`) |
| 419-469 | `smmu_identity()`: SMR0/S2CR0 at index 0, CB0 at +0x8000, 1 GB blocks for 2-8 GB | same CB layout; DRAM 0x80000000-0x17e4bffff fits; SMR index from a scan (3.1) |
| 588-651 | `zap_load()`: one relocatable segment at ZAP_PA, mem_setup always, metadata ≤ PAGE_SIZE in XNU RAM | 3.3 |
| 1058 | `compute_run()` SP_DBG_ECO_CNTL 0x40000800, already marked "not A540" | A540: SP_DBG_ECO 0x800, HLSQ_DBG_ECO 0, VPC_DBG_ECO 0x800400 (Mesa `fd5_emit.c:967-976`) |
| 1706-1716 | chip_id 0x05030002 exported to Mesa | 0x05040001 (`live.dts:8773`) |
| 1718-1751 | `gfx_raise()`: PMI8994 S2 VSET 825/1015 mV, MX by `sweet_rpm_mx_corner` ("smpa" 2, key "corn": `sweet_rpm.c:395-411`), GPLL0 /1.5 or /1 | PM8005 S1 VSET (2.3), MX `rwmx` 0 `vlvl`, mem-acc per corner, GPUPLL0 L/FRAC |
| 1956-1963 | RBCCU then SP power islands, 0x778000 | unchanged: kgsl `a5xx_regulator_enable` (`adreno_a5xx.c:483-520`) and mainline `a5xx_pm_resume` (`a5xx_gpu.c:1351-1395`) treat A530 and A540 alike |
| 1965-2000 | `a5xx_hw_init` subset with A530 values | 2.6 |
| 2009-2017 | ME_INIT, 6th dword 0xb (A530 token-end-sync, 2015) | 0 |
| 2056-2089 | service mode at 300 MHz, then 400/600 via `gfx_raise`; log text names S2 and GPLL0 | taimen corners 257-596 MHz with their own log text |
| 2115-2236 | `gpu_thread()`: waits for PMI8994 S2 (2120-2128), maps MMCC/GCC/MEMACC (2129-2137), S2 at 745 mV (2139-2183), MMSS GDSCs and clocks (2185-2216), first GPU reads (2218-2235) | replaced by the sequence in 2.2 |
| 2239-2249 | `sweet_gpu_init()` | not called on TAIMEN; add a call in the TAIMEN branch at `sweet_ufs.c:664-676` |

### 2.2 MSM8998 power-up sequence

Proposed home: a TAIMEN section of `sweet_gpu.c`, or a new `taimen_gpu.c` that reuses the CP/SMMU/zap
code. Each write is logged before it is done, and each poll has a timeout. A failure stops before the
next block and leaves everything as it is.

Addresses:
- GPU CC (one `ml_io_map(0x5065000, 0x2000)`: on 4K a 0x1000 map covers one page only,
  `taimen-4k-catalogue.md:118-125`):
  - Fabia PLL0: MODE 0x5065000, L 0x5065004, CAL_L 0x5065008, USER_CTL 0x506500c, CONFIG_CTL 0x5065014,
    OPMODE 0x506502c, FRAC 0x5065038.
  - Other control: GX_DOMAIN_MISC 0x5065130, DD_WRAP_CTRL 0x5065430.
  - GDSCs and branches: CX GDSCR 0x5066004, GDS_HW_CTRL 0x5066008, CXO CBCR 0x5066020, GX BCR 0x5066090,
    GX GDSCR 0x5066094, gfx3d CBCR 0x5066098, rbbmtimer CBCR 0x50660d0.
  - RCGs: gfx3d CMD 0x5066070 / CFG 0x5066074, rbbmtimer CMD 0x50660b0 / CFG 0x50660b4.
  - CRC: SID_FSM 0x50660a0, MND_CFG 0x50660a4.
  - Sources: mainline `gpucc-msm8998.c:34-298`, wahoo `msm-clocks-hwio-8998.h:230-244`.
- GCC:
  - Branches: GPU_CFG_AHB 0x171004, GPU_BIMC_GFX 0x171010, BIMC_GFX 0x146040, GPU_IREF_EN 0x188010.
  - GPU_MISC 0x171028, and the GPLL0 vote 0x15200c (bit 4 gpu_gpll0, bit 3 gpu_gpll0_div).
  - Sources: mainline `gcc-msm8998.c:1416-1440, 2112-2163`, wahoo `msm-clocks-hwio-8998.h:164-170, 228`.

Steps:

1. **GCC.**
   - Read every register above.
   - Set bit 0 of GPU_CFG_AHB, GPU_BIMC_GFX, BIMC_GFX and GPU_IREF_EN (kgsl `iref_clk`).
   - Leave GPU_MISC alone (mainline writes 0x10003 there at boot, `gcc-msm8998.c:3353-3355`) and
     GCC_GPU_BCR 0x171000 too.
   - Whether the GPU CC registers need `gcc_gpu_cfg_ahb_clk` is **unverified**. Neither DT lists it
     for the GPU CC, so do this step first anyway.
2. **XO into the GPU CC.** Turn on CXO CBCR 0x5066020 bit 0; mainline marks it critical
   (`gpucc-msm8998.c:33-49`).
3. **Rails** (2.3, 2.4).
   - S1: read TYPE 0x04, SUBTYPE 0x05, RANGE 0x40, VSET 0x41, MODE 0x45, EN 0x46 and STATUS 0x08.
     Write VSET for the corner, then EN = 0x80; wait 500 us (`live.dts:1949`); read back.
   - RPM: MX `rwmx` vlvl at the corner's level, and CX `rwcx` vlvl ≥ 128.
   - mem-acc bit for the corner.
4. **CX GDSC.** Clear SW_COLLAPSE (0x5066004 bit 0). Poll GDS_HW_CTRL 0x5066008 bit 31 for up to
   500 us (`live.dts:664` gds-timeout 0x1F4), then wait 10 us (`clock-gpu-8998.c:407-413`).
5. **Root clocks on XO.**
   - gfx3d: CFG 0x5066074 = 0 (source XO, no divider). Set CMD 0x5066070 bit 1 (ROOT_EN; the DT's
     `qcom,force-enable-root-clk`) and bit 0 (UPDATE). Poll until UPDATE clears.
   - rbbmtimer: the same on 0x50660b0/b4, then CBCR 0x50660d0 bit 0.
6. **GX.** Follows wahoo `enable_gfx_crc()` (`clock-gpu-8998.c:401-444`), which is the clock driver's
   own power-up.
   - GX BCR 0x5066090 = 1, wait 1 us, then 0.
   - GX_DOMAIN_MISC 0x5065130: set bit 4 (GMEM reset), wait 1 us, clear bit 4, then clear bit 0
     (GMEM clamp).
   - GX GDSCR 0x5066094: clear SW_COLLAPSE, poll bit 31, wait 10 us.
   - Do not copy mainline's AON-reset helper: it writes `GMEM_RESET_MASK` with the value 1, which
     leaves bit 4 untouched (`gdsc.c:243-249`).
7. **gfx3d branch.** Set CBCR 0x5066098 bits 14 and 13 (retain mem/periph: mainline gpucc probe
   `:332-335`, kgsl `a5xx_clk_set_options` `adreno_a5xx.c:1642-1668`), then bit 0. Poll CLK_OFF clear.
8. **Optional, logged.**
   - The one-time CRC setup that downstream does with GX on: CRC_MND_CFG 0x50660a4 = 0x00015010,
     CRC_SID_FSM 0x50660a0 = 0x00800000 (`clock-gpu-8998.c:447-449`).
   - DD_WRAP 0x5065430 bit 0 (`:703-705`, leakage only).
   - Mainline does neither. Run E1 without them first.
9. **First GPU reads.** Do them only now: on Qualcomm an unpowered or unclocked block can stall the bus
   (**unverified** for this one).
   - VBIF_VERSION 0x3000, RBBM_STATUS 0x4f5, RBBM_ALWAYSON_COUNTER 0x4d2 twice, as `sweet_gpu.c:2225-2231`
     does.
   - Clock measurement: RBBM_PERFCTR_CNTL 0x464 = 1, CP_PERFCTR_CP_SEL_0 0xbb0 = 0 (PERF_CP_ALWAYS_COUNT,
     as mainline `a5xx_gpu.c:740-744`). Then gfx3d = 19.2 MHz × Δ RBBM_PERFCTR_CP_0 (0x3a0) / Δ ALWAYSON
     over 10 ms. That the counter runs before the CP starts is **(unverified)**.
10. **GPUPLL0** (only with `taimengpuclk=` > 19).
    - Out of reset (MODE bit 2 RESET_N = 0): set HW_UPDATE_LOGIC_BYPASS (bit 23) and RESET_N, as the
      downstream handoff does (`clock-alpha-pll.c:1032-1045`).
    - Write L, CAL_L (the same value) and FRAC from the table in 2.3. That is the downstream
      `fabia_alpha_pll_set_rate` (`:952-981`), which avoids mainline's separate calibration run
      (`clk-alpha-pll.c:1408-1461`).
    - Out_even post-divider USER_CTL[11:8] = 0 (÷1, mainline `gpucc-msm8998.c:56-91`).
    - Enable: OUTCTRL 0, OPMODE STANDBY, RESET_N, OPMODE RUN, wait LOCK_DET (MODE bit 31), USER_CTL |= 7,
      OUTCTRL (mainline `clk-alpha-pll.c:1270-1323`; downstream `clock-alpha-pll.c:862-903`).
    - Only after lock: gfx3d CFG = (1 << 8) | 3 (source 1, ÷2, mainline `ftbl_gfx3d_clk_src` pre_div 3,
      `gpucc-msm8998.c:132-135`), then UPDATE, then measure again. On a lock timeout, stay on XO.
    - CONFIG_CTL is written by neither Linux driver; that its reset value is right is **(unverified)**.
11. **RBCCU, then SP power islands** (`sweet_gpu.c:1958-1963`, unchanged).

### 2.3 Corners, voltages and PLL values

| gfx3d MHz | corner | VCO, L / FRAC | ceiling combo 0 / combos 1-7 (mV) | floor | MX vlvl | mem-acc | proposed S1 |
|---|---|---|---|---|---|---|---|
| 19.2 (XO) | - | PLL off | - | 516 | none | 1 | as ABL left it if on, else 725 |
| 257 | 2 | 514 MHz, 0x1a / 0xc555 | 716 / 724 | 516 | 128 SVS | 1 | 725 (VSET 129) |
| 342 | 3 | 684, 0x23 / 0xa000 | 772 / 772 | 532 | 128 | 1 | 775 (139) |
| 414 | 4 | 828, 0x2b / 0x2000 | 880 / 832 | 584 | 128 | 0 | 880 (160) |
| 515 | 5 | 1030, 0x35 / 0xa555 | 908 / 916 | 632 | 256 NOM | 0 | 920 (168) |
| 596 | 6 | 1192, 0x3e / 0x1555 | 948 / 968 | 672 | 256 | 0 | 970 (178) |
| 670, 710 | 7, 8 | not planned (no thermal management, no LM) | 1016-1024, 1088 | 712, 756 | 384 | 0 | - |

- Sources:
  - Corners and MX levels: `live.dts:2448-2449`.
  - Ceilings, floors and the mem-acc map: `live.dts:4972-4974, 5000`.
  - VCO = 2 × gfx3d, L = VCO / 19.2 MHz, FRAC = 16 fractional bits (`clock-alpha-pll.c:72`). Wahoo's v2
    table uses the same PLL rates (`clock-gpu-8998.c:168-176`).
- The proposed S1 voltage is the higher of the two ceilings. The fuse combo is unknown, and it is not
  needed at the maximum.
- FTS2.5 range 0 is 80 mV + 5 mV × VSET (`qcom_spmi-regulator.c:529-532`; the Pixel 1 rule,
  `sweet_gpu.c:2159-2165`). The S1 subtype is **unverified**:
  - Read 0x04/0x05 first.
  - FTS426 (subtype 0x0a) uses 4 mV steps in 0x40/0x41 (`:534-536, 891-902`). Stop rather than guess.
  - Never write a VSET that reads back outside 516-1088 mV (`live.dts:1947-1948`).
- Margin: Linux on this phone ran 257 MHz at ≤ 648 mV and 710 MHz at ≤ 988 mV (`dmesg.txt:296-297`).
  The ceilings are 75-100 mV higher: safe for timing, but warmer.
- Mainline msm8998 boards do not scale this rail at all. S1 is `regulator-always-on` with the comment
  "Hack until we rig up the gpu consumer" (`msm8998-oneplus-common.dtsi:261-270`,
  `msm8998-sony-xperia-yoshino.dtsi:306-315`).

### 2.4 CPR, CX, MX, mem-acc, DDR

- **CPR.** Leave the CPR4 block at 0x5061000 alone; no closed loop. It changes the voltage only
  through Linux's regulator driver, so off means open loop.
- **MX.** RPM request type `rwmx` (0x786d7772), id 0, active set, KVPs `swen` = 1 and `vlvl` = level
  (mainline `rpmpd.c:24-41, 798-813, 959-969`; levels in `include/dt-bindings/power/qcom-rpmpd.h:406-417`:
  128 SVS, 256 NOM, 384 TURBO).
  - New helper beside `sweet_rpm_mx_corner`. That one is the MSM8996 "smpa" 2 with "corn".
  - Raise MX before the clock, lower it after.
- **CX.** `rwcx` id 0 `vlvl` ≥ 128, for gpucc vdd_dig. Whether this is needed is **unverified**: CX
  already carries the display and UFS.
- **mem-acc.** 0x1FCF004 bit 0 = 1 for corners 1-3 and 0 for 4-8. Set it before raising past 342 MHz
  and after lowering. Linux writes it from EL1 (mem-acc-regulator); that XNU can too is
  **(unverified)**.
- **DDR.** No bandwidth vote today; the GPU gets whatever DDR clock the RPM picks. The RPM "clk2"
  id 0 path exists (`sweet_rpm.c:475-506`) but is untested on MSM8998. Not needed for function.

### 2.5 GPMU / limits management (A540)

- Mainline `a5xx_power_init` (`a5xx_power.c:295-322`) runs `a540_lm_setup` (175-209) and
  `a5xx_pc_init` (211-217).
- `a5xx_gpmu_init` returns 0 when there is no firmware (220-276). Mainline therefore runs without
  `a540_gpmu.fw2`, which linux-firmware does not ship: its qcom/ directory has only a530_pfp, a530_pm4
  and a530v3_gpmu.
- kgsl enables the GPMU and SP/TP power collapse on A540 (`adreno-gpulist.h:241-285`; `a540_lm_init`
  at `adreno_a5xx.c:1519-1563`). From then on every power-level change needs a GPMU voltage handshake
  (`:1604-1640`).
- Plan: no GPMU in E1-E6, as the Pixel 1 ran its A530. The cost is no current limiting at high clocks,
  so stay ≤ 596 MHz.
- Optional E4b, with a GPU-GPMU blob:
  - Load it as mainline does (`a5xx_power.c:324-390`): data[0] = size, data[1] == 2, commands at
    data + data[2] + 3, PKT4 into GPMU_INST_RAM_BASE 0x8800.
  - Run it with CP_SET_PROTECTED_MODE 0, an IB, then protected mode 1, then GPMU_CM3_SYSRESET 0xa8d8 = 0.
  - Expect GPMU_GENERAL_0 0xa8e0 = 0xBABEFACE and GPMU_GENERAL_1 0xa8e1 = 0.

### 2.6 hw_init and CP: A530 values in sweet_gpu.c against A540

| register (dword) | sweet_gpu.c | A540 | source |
|---|---|---|---|
| VBIF_ROUND_ROBIN_QOS_ARB 0x3049 | 3 (1966) | 3 | `a5xx_gpu.c:700`; kgsl `adreno_a5xx.c:50-55` |
| VBIF_GATE_OFF_WRREQ_EN 0x302a | not written | 9 | `a5xx_gpu.c:702-704`; kgsl as above |
| CP_MERCIU_SIZE 0x826 | 0x40 (1975) | 0x400 | `a5xx_gpu.c:776-780`; kgsl `:1953-1957` |
| UCHE_DBG_ECO_CNTL_2 0xe8f | not written | 2 (highest bank bit 15 - 13) | `a5xx_gpu.c:842-844`; kgsl `:2064-2067` |
| VPC_DBG_ECO_CNTL 0xe60 | bit 10 (1985) | bits 10 and 23 | `a5xx_gpu.c:846-847, 916-919` |
| HLSQ_DBG_ECO_CNTL 0xe04 | not written | clear bit 18 | `a5xx_gpu.c:916-919`; kgsl `:2077-2080` |
| TPL1_MODE / RB_MODE | 2 << 7, 2 << 1 (1983-1984) | same | `live.dts:8780` |
| ME_INIT 6th dword | 0xb (2015) | 0 | `a5xx_gpu.c:494-507`; kgsl `:2276-2300` |
| HWCG | off | off is fine (A540 adds only GPMU delay registers when on, `a5xx_gpu.c:466-469`) | - |
| CP_PROTECT | not written | optional | `a5xx_gpu.c:849-886` |
| kernel compute state | SP_DBG_ECO 0x40000800 (1058) | SP 0x800, HLSQ 0, VPC 0x800400 | Mesa `fd5_emit.c:967-976` |
| chip id to Mesa | 0x05030002 (1710) | 0x05040001 | `live.dts:8773`; `a5xx_catalog.c:133-150` |
| GMEM | 1 MiB at 0x100000 (1972-1973, 1711-1712) | same | `a5xx_catalog.c:141` |

Microcode needs no change: mainline uses a530_pm4/pfp for the A540 and skips their 4-byte header
(`adreno_gpu.c:554-566`), as `cp_test` already expects.

### 2.7 4K pages

`taimen-4k-catalogue.md:85-105` lists the kernel side:
- the ring, `kernel_page`, the zap `mdt_len > PAGE_SIZE` check, `vm_buffer`, the sample source page
  and the stack array.

Two more:
- `blit_test` (665-681) puts a 64×64 RGBA source (16 KB) into one 4 KB page.
- `VM_PAD` (763) is 8 pages, which is 32 KB on 4K. Keep it at 128 KB in bytes: overruns into a fault
  hung the A530 (752-755).

In user space, `sweet_drm.c:222` rounds every BO to 16 KB while the kernel rounds to the page
(`SweetGPU.cpp:359`). Use `vm_page_size`.

## 3. SMMU, secure mode and the zap shader (question 2)

### 3.1 GPU SMMU

- **Same block as on the Pixel 1.** SMMUv2 with CB0 at +0x8000 (kgsl `gpu-offset 0x48000` is the GPU
  base + 0x40000 + 0x8000) and SID 0 for the user. Mainline programs it from EL1 on msm8998:
  - `arm-smmu-qcom.c:277-345`: the bypass-quirk test writes S2CR of the last group, reserves a CB, and
    keeps the SMRs that firmware left valid.
  - `:535-561`: `qcom,msm8998-smmu-v2` uses `qcom_smmu_v2_data`.
  - msm-4.4 kgsl attaches gfx3d_user the same way.
  - So it is HLOS-programmable. Only the secure side (SID 2, `qcom,hyp_secure_alloc`, `live.dts:8911`)
    belongs to TZ/QHEE.
- **Power first.** The SMMU sits behind gdsc_gpu_cx and the three GCC clocks (`live.dts:5381-5383`).
  Read no SMMU register before step 4 of 2.2.
- **Pixel 1 rules, which carry over** (`sweet_gpu.c:257-266, 401-412`):
  - firmware traps and emulates S2CR writes;
  - a trapped `str wzr` stalled a CPU;
  - an SMR matching every stream stalled the CPU;
  - S2CR starts as FAULT, and the first CP fetch then reset the phone.
- **New on taimen:**
  - Read sCR0, IDR0 (NUMSMRG), IDR1 (NUMCB, page size) and every SMRn/S2CRn, and log them.
  - Use an SMR index that is invalid, or one that already matches SID 0 exactly, instead of the
    hard-coded index 0 (456-457).
  - Stop if a valid SMR already covers SID 0 with another CB, or if CB0's SCTLR is already enabled.
  - Derive the CB base from IDR1 and check that it is +0x8000.
- **Leave out `attach-impl-defs`** (`live.dts:5385`, offsets 0x6000-0x6b68). The Pixel 1 ran without
  them. Their role (micro-TLB and prefetch tuning) is **unverified**.
- **Identity map scope.** The 2-8 GB identity map (434-437) covers all DRAM, the secure carve-outs
  included. A stray GPU access there would trip an XPU, with an **unverified** effect.
  - Optional tightening: 2 MB blocks for the splash region 0x9d400000-0x9f800000, plus XNU's RAM
    0x1_0000_0000-0x1_2000_0000 (`mkadt.py:54`), plus the GPU VA window.
- **Hypervisor risk.** QHEE is in the path for some calls. `hvc` resets the board (NOTES:395-398), and
  PSCI SYSTEM_RESET came back untouched (NOTES:212-216). That SMMU MMIO writes go through stage-2
  emulation is **unverified** on this firmware.
  - Containment: one 32-bit `str` from a W register per write, each logged before it (`SWR`), each
    read back.
  - Run with `cpus=4` so a stalled core leaves the deadline thread running.

### 3.2 Secure mode

- The GPU comes up secure (RBBM_SECVID_TRUST_CNTL = 1). Writing 0 from EL1 reset the Pixel 1
  (`sweet_gpu.c:24-26`).
- The A540 lists ADRENO_CONTENT_PROTECTION (`adreno-gpulist.h:248-250, 270-272`). So Linux leaves
  secure mode through the zap shader and CP_SET_SECURE_MODE 0 (kgsl `a5xx_set_unsecured_mode`;
  mainline `a5xx_gpu.c:977-1005`), and so will XNU.
- Never write TRUST_CNTL on taimen.

### 3.3 Zap shader

- **What is known:**
  - PAS id 13 (`live.dts:8747`); the PIL loaded `a540_zap.mdt` and then `a540_zap.b02` only
    (`dmesg.txt:2174-2194`). NOTES:43 lists b00-b02 too: b00 is the ELF header, b01 the hash segment
    (inside .mdt).
  - One loadable segment fits sweet's `zap_load` check (620-624).
- **Placement rule** (msm-4.4 PIL, used by Android on this phone):
  - The node has no memory-region (`live.dts:8745-8749`).
  - Relocatable means p_flags bit 27 (`peripheral-loader.c:343-346`).
  - A relocatable image gets a DMA region anywhere (`pil_alloc_region`, `:437-475`). Otherwise the
    region is the image's own p_paddr range (`:481-535`).
  - Mainline has the same rule: mem_setup only when relocatable, and segments must fall inside the
    carve-out (`mdt_loader.c:202-260, 293-352`).
  - So: read p_paddr, p_memsz and p_flags from the .mdt. If the image is fixed, use that address
    (probably inside 0x95A00000-0x95AFFFFF, **unverified**). If it is relocatable, use 0x95A00000:
    Linux leaves it out of System RAM, and XNU does not own it.
- **Changes to `zap_load()`:**
  - Call PAS_MEM_SETUP (634) only for a relocatable image.
  - Allocate the metadata contiguous with `round_page(mdt_len)` (599), below 4 GB:
    - mainline allocates it with the default 32-bit DMA mask (`qcom_scm.c:575-610`); msm-4.4 uses a
      64-bit mask (`subsys-pil-tz.c:581-643`), so above 4 GB may also work **(unverified)**;
    - candidates: the tail of the 1 MiB carve-out past p_memsz (for a relocatable image), or a page in
      the splash region past SCAN0's end at 0x9eeb4800, which is free until E7 (section 5).
  - Keep writing the segment through `ml_io_map` (639-646).
- **SCM from XNU on taimen has never been tried.** PSCI fast calls work (NOTES:395-397).
  `scm_call()` (551-576) is the SMC64 SiP standard call mainline uses.
  - Before PAS, probe with harmless queries and log raw x0/x1, noting "x0 unchanged" as the SMP plan
    does for PSCI:
    - INFO IS_CALL_AVAIL (fn 0x42000601, x1 = 1, x2 = 0x02000201 for PAS_INIT_IMAGE;
      `qcom_scm.c:340-360`);
    - PAS_IS_SUPPORTED(13) (fn 0x42000207; `:776-795`).
- **Order.** As the Pixel 1 and mainline do (`a5xx_gpu.c:954-1005`): CP running, ME_INIT done, GPU
  clocked; then init_image, mem_setup if relocatable, auth_and_reset; then CP_SET_SECURE_MODE 0. kgsl
  loads the zap shader before the ME starts (`adreno_a5xx.c:2222-2273`), so either order works on
  this firmware.
- **Crypto/bus votes.** msm-4.4 votes bus bandwidth around PAS calls (`subsys-pil-tz.c:170-200`).
  Mainline's msm8998 scm node has no clocks (`msm8998.dtsi:314-318`), so the votes are probably not
  needed **(unverified)**.
- **Afterwards.** The carve-out is locked to the secure world: never read it from the CPU again.
  - Resume matters only after a GX power collapse: SCM_SVC_BOOT cmd 0xA with (0, 13), kgsl
    `:2241-2260`, mainline `a5xx_zap_shader_resume` `:642-660`.
  - XNU keeps GX on.

## 4. User space: sweetgl on taimen (question 3)

### 4.1 What the scripts need, and what this PC has

| file:line | needs | here |
|---|---|---|
| `build.sh:9-13` | `research/mesa` (patched checkout), `research/mesa-ios`, `research/xcode15/Xcode.app` (DEVELOPER_DIR), `research/tools/venv` and `bin` (meson, mako, ninja) | none; WSL2 has python3, git, curl, but no clang, lld, meson, ninja or cmake |
| `build.sh:27` | Apple `nm -g` on a Mach-O archive; writes `out/` before the `mkdir -p` (33) | `llvm-nm`; create out/ first |
| `build.sh:28, 47-50` | `xcrun -sdk iphoneos clang/clang++`, frameworks IOKit, CoreFoundation, IOSurface, OpenGLES, CoreGraphics, `-lobjc -lc++ -lz` | needs an iPhoneOS SDK |
| `build.sh:51` | `codesign -f -s -` (ad-hoc) | ld64.lld `-adhoc_codesign`, `ldid -S` or `rcodesign` |
| `build.sh:52-56` | BSD `stat -f %z`; 64 MiB padding for `push.sh` | `stat -c %s`; padding not needed on taimen |
| `mesa-setup.sh:6-20` | Xcode and venv; meson options | every option exists in 25.3.6 and 26.2.5 (checked in `meson.options`) |
| `ios-cross.txt:1-18` | absolute `/Volumes/SamsungSSD/ios-sailfish/...` paths to Xcode's clang and `iPhoneOS17.5.sdk` | must be generated per host |
| `push.sh` | Pixel 1 extents on sda35 | replace with ROOTFIX (4.5) |

### 4.2 Mesa version

- It is not recorded: `research/mesa` was a checkout on the Mac.
- `build.sh:23` links `src/mesa/glapi/glapi/libglapi_bridge.a` and `src/mesa/glapi/shared-glapi/`. That
  layout exists from mesa-25.3.0 on; 25.2.x and earlier have `src/mapi`.
- `mesa.patch` applies to 25.3.6 and to 26.2.5, with line offsets.
- Pin **mesa-25.3.6** (tarball and sha256 in the build script), and confirm it with a build.

### 4.3 A540 in freedreno, and what sweetgl must change

- `freedreno_devices.py:287-303`: GPUId(530) and GPUId(540) share one GPUInfo (A5XX, highest bank
  bit 15, GMEM tiles).
- `freedreno_dev_info.c:25-28`: the lookup compares gpu_id when both sides have one.
- The only behavioural A540 case in the a5xx driver is `fd5_emit.c:967-979` (ECO registers when
  `gpu_id == 540`). The device name comes from the table, "FD%d" (`freedreno_devices.py:80`).
- `sweet_drm.c:180-181` returns MSM_PARAM_GPU_ID 530. Derive it from the kernel's chip id instead:
  core × 100 + major × 10 + minor = 540.
- `sweet_drm.c:222`: 16 KB BO rounding (2.7).
- `eagl.c` patches QuartzCore at fixed addresses of the iPhone 8 shared cache ("dsc-8"):
  - the format hook at 0x18840433c (`:2125`), the texture-function table 0x1d5b44a10 and its cases
    0x18855a1e4 (`:2186-2187`), and `sgl_hook_def` (`:2109`);
  - taimen runs the iPhone X 16.7.16 (20H392) root (`research/ipsw-x/meta.json`: iPhone10,3);
  - whether its cache has the same layout is **unverified**;
  - the guards fail safe ("format hook: unexpected code", "texture function fixes: unexpected
    QuartzCore, not installed");
  - but then the renderer bugs those fixes cured return: the lock-screen clock, vibrant colour
    matrices, blur (commits c3a5101, 8f5cd89, 48b0226).

### 4.4 Can this PC build sweetgl.dylib?

- **Plan A, recommended: a macos-14 GitHub job.** It is the toolchain the Pixel 1 used.
  - `xnu.yml:45, 78-83` already selects an Xcode on that runner.
  - Steps:
    1. Fetch the Mesa tarball by tag and sha256, then `patch -p1 < tools/sweetgl/mesa.patch`.
    2. `pip install meson mako pyyaml packaging`, `brew install ninja bison`.
    3. Write the cross file from `xcrun --sdk iphoneos --show-sdk-path`.
    4. Run `mesa-setup.sh` and `build.sh`, with DEVELOPER_DIR and the paths taken from the environment
       instead of `research/`.
    5. Upload `sweetgl-unpadded.dylib`.
  - The SDK version is not critical: anything ≥ 16 headers.
- **Plan B: WSL2 locally.**
  - Packages: `apt install clang lld llvm meson ninja-build bison flex python3-mako python3-yaml
    python3-packaging` (Ubuntu 26.04's LLVM).
  - Compile with `clang -target arm64-apple-ios16.0 -isysroot $SDK`.
  - Link with `-fuse-ld=lld` (ld64.lld reads the SDK's .tbd stubs) and `-Wl,-adhoc_codesign`. lld
    signs by default only for macOS and simulator targets (**unverified** for the installed version).
  - Tools: llvm-ar, llvm-ranlib, llvm-nm and llvm-strip in the cross file.
  - Blocker: the iPhoneOS SDK. It comes from Xcode, which needs the user's Apple ID: copy it from a
    Mac, or unpack `Xcode_15.4.xip` with xar/pbzx.
  - Also **unverified**:
    - Mesa's darwin cross build with a non-Apple linker;
    - Objective-C in `eagl.c` with Linux clang;
    - lld with `-framework OpenGLES`.
- **Signing.** Ad-hoc is enough. taimen boots with `cs_enforcement_disable=1` and permissive AMFI
  (`boot-xnu.sh:20`).

### 4.5 Getting it onto the phone

- `push.sh` writes Pixel 1 extents and does not apply.
- Use a ROOTFIX record instead: op 1 (write) or 2 (create), mode 0755, path `/usr/lib/sweetgl.dylib`.
  `mkrootfix.py` needs an option to add a file from the PC; today it only edits files from the root
  image. Take the unpadded build: the 64 MiB padding existed only for in-place rewrites. Size limits
  of the payload blob are **unverified**; Mesa static plus GL is roughly 15-30 MB.
- Boot-args, as on the Pixel 1:
  - `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`;
  - `sweetnull=backboardd` to see sweetgl's own lines, which it writes to /dev/null
    (`sweetgl.c:68-75`).
- Length: the E6 command line comes to about 510 of the 607 bytes `mkpayload.py:193` keeps (the rest
  is cut silently).

## 5. The present path (question 4)

- **Today.** `taimen_present_scaled()` (`SweetFramebuffer.cpp:959-1035`) copies the swapped surface
  into SCAN0 0x9e400000 with `memcpy` (1014-1020): 4608 × 2436 = 11,225,088 bytes. In mode 3 it
  alternates with SCAN1 0x9d400000. Then it flips VIG0 and QSEED3 scales to 1330×2880.
  - The copy runs on a silver core at 0.4-1.3 GB/s (boot 41, NOTES:423-425): 9-28 ms per frame. In
    mode 2 it also waits for the transfer first (1009-1011).
- **The Pixel 1 GPU path does not apply.** It is gated on the 24-bpp `fScaled` (1043), and
  `present()` returns at 1060-1061 before reaching it.
  - Its panel buffers 0xc0800000/0xc1000000 (661-663) lie in Linux System RAM b1000000-17e4bffff,
    outside XNU.
  - Its 8 MiB slots are too small for 1440×2880×4 (673), and it scales on the GPU, which QSEED3 now
    does for free.
- **Can the GPU write straight into a scan buffer below 4 GB? Yes.**
  - The SMMU identity map covers 0x80000000-0x1ffffffff with GPU VA = PA (434-437).
  - `sweet_gpu_vm_map` takes any PA list and skips cache maintenance for pages XNU does not own
    (976-986).
  - The MDP keeps fetching the PA through its 32-bit SRC0_ADDR.
  - GPU writes are normal non-cacheable (MAIR 0x44, 445) and complete at the CACHE_FLUSH_TS fence, so
    the data is in DRAM before the flip.
- **E5 design.** In `taimen_present_scaled()`, when `sweet_gpu_ready()` and `taimengpucopy=1`:
  - Map SCAN0/SCAN1 for the GPU once.
  - Map the swap surface read-only through a small cache like `AppleCLCD::gpuMap` (696-735).
  - Run one `sweet_gpu_quad` with `SWEET_GPU_QUAD_COPY | SWEET_GPU_QUAD_NEAREST`, identity transform,
    1125×2436. Pitch 4608 = 72 × 64 meets the 64-byte rule (`sweet_gpu.h:24-26`). It is synchronous,
    then flip.
  - Use mode 3 (double buffer). Mode 2 keeps `sweet_fbcon_wait_idle`.
  - Any error falls back to the memcpy, with one log line.
- **The gain while CoreAnimation renders on the CPU is smaller than the copy time.** `sweet_gpu_quad`
  cleans the source with VA-range maintenance (`vm_cache`, 976-986): 11 MB per frame, cost
  **unverified**. With Mesa (E6) the surfaces are GPU-written and no CPU cache traffic remains.
- **If the quad is slow:** the 2D blitter (CP_BLIT copy, `emit_blit` 497-543, proven on the A530 by
  `blit_test`) needs no shader. Add `sweet_gpu_copy()`.
- **E7, later: zero copy.** Back backboardd's swap surfaces (1125×2436 BGRA, bpr 4608) with low physical
  memory, then flip VIG0 straight to the surface PA.
  - Three fit in the 36 MiB splash region: 0x9d400000 + 3 × 0xab4800 = 0x9f41d800 < 0x9f800000.
  - Use `sweet_surface_create_physical` (`SweetSurface.cpp:239-258`, write-combined).
  - It needs three things:
    - an allocation policy in SweetSurface (size, format, task);
    - holding the presented surface until its transfer ends;
    - GPU rendering: WC memory is slow for CoreAnimation's CPU renderer.
  - This is option 1 in `taimen-display-scaling-plan.md:424-447`.

## 6. Firmware and other prerequisites

- **P1, firmware.** Needed: `a530_pm4.fw`, `a530_pfp.fw` (strip 4 bytes: `tail -c +5`),
  `a540_zap.mdt`, `a540_zap.b02`, and optionally `a540_gpmu.fw2`. Store them in
  `research/taimen/firmware/`, never in git.
  - None of them is on this PC. `backup/` has no vendor image, and a search of the WSL filesystem, the
    Windows user profile, OneDrive and drive E: found none.
  - Ways to get them:
    1. The log reader (its Linux and busybox are already in the loop) mounts vendor_a read-only and
       copies `/vendor/firmware/a5*` onto the TAIMENLOG drive. This is a `tools/taimen/reader/init`
       change.
    2. TWRP `adb pull /vendor/firmware/...` (TWRP adbd is unreliable, NOTES:222-228).
    3. Google's taimen factory image (vendor.img inside image-taimen-*.zip, about 1.4 GB), with the
       user's approval. Whether its zap matches this phone's TZ is **unverified**, but it should, since
       the files are signed by Google for taimen.
  - With an image in hand, extract:

    ```
    python3 -I port/tools/simg2img.py vendor.img vendor.raw    # only if sparse (magic ed26ff3a)
    "/mnt/c/Program Files/7-Zip/7z.exe" e -o"$(wslpath -w research/taimen/firmware)" "$(wslpath -w vendor.raw)" \
        firmware/a530_pm4.fw firmware/a530_pfp.fw firmware/a540_gpmu.fw2 "firmware/a540_zap.*"
    ```

  - linux-firmware's a530_pm4/pfp would do for E2 (mainline uses them on A540; downloading them needs
    the user's approval). Nothing public replaces the device-signed zap shader.
- **P2, SPMI on taimen.**
  - Version: the arbiter at core 0x800F000, chnls 0x8400000, obsrvr 0x9400000, cnfg 0x800A000
    (`live.dts:710-722`). Read `PMIC_ARB_VERSION` (core + 0): v2 needs ≥ 0x20010000, v3 ≥ 0x30000000.
  - Access: v3 uses the v2 PPID->APID map (core + 0x800 + 4n), channel offset 0x8000 × apid for EE 0
    and owner table cnfg + 0x700 + 4n (`spmi-pmic-arb.c:23-27, 1545-1553, 1593-1621, pmic_arb_offset_v2`).
    That is what `sweet_keys.c:59-200` already does.
  - Port it with the taimen bases.
  - Allow writes only to ppid 0x514, registers 0x40/0x41/0x45/0x46, and only if the owner is 0.
- **P3, RPM rail levels.** A `rwmx`/`rwcx` `vlvl` helper in `sweet_rpm.c` (2.4).
- **P4, hooks.**
  - A TAIMEN `sweet_gpu_init()` call in `sweet_ufs.c:664-676`.
  - `boot-xnu.sh` gets `GPUFW=dir`, which adds `--blob GPU-PM4=.. GPU-PFP=.. GPU-ZAP-MDT=..
    GPU-ZAP-SEG=..` (and `GPU-GPMU`) next to ROOTFIX (`boot-xnu.sh:33-35`).
- **P5, 4K fixes** (2.7), and the SMR scan (3.1).

## 7. Risks, and how each attempt stays bounded and visible

| risk | effect | containment |
|---|---|---|
| GPU CC or GPU register read with the block unpowered or unclocked | the bus stalls on that core | strict order in 2.2; a log line before the first read of each block; `cpus=4` keeps the deadline thread alive on another core; `taimenreboot` -> PS_HOLD -> log reader |
| whole-system stall | no pets | the APSS WDT bit after a stuck hvc (boot 35, NOTES:395-398), but did not in boots 13-15 (NOTES:200-216); otherwise autoboot exit 3, attention.sh, the user holds power, and pstore keeps the log (boot 25, NOTES:332-339) |
| wrong PMIC write (bad APID map, wrong subtype formula) | another rail changes, or S1 is overvolted | write only ppid 0x514 after checking the map's PPID and the owner; refuse VSETs outside 516-1088 mV; read back each write |
| S1 too low for the corner | GPU errors or hang, not a CPU hang (**unverified**) | ceiling voltages (2.3); poll timeouts; the GPU service goes off (`gpu_hung`) |
| SMMU write trapped badly by QHEE | CPU stall or reset (Pixel 1 0121a/0122a) | Pixel 1 rules plus the SMR scan; each write logged first; pstore survives the reset |
| CP runs with S2CR = FAULT | reset (Pixel 1 0120a) | never start the CP unless `smmu_identity` reads every value back (469) |
| SCM call swallowed by QHEE | x0 comes back unchanged | IS_CALL_AVAIL probe first; no PAS calls unless the probe answers |
| zap rejected (wrong carve-out, metadata above 4 GB, signature) | error code, GPU stays secure | log and stop before CP_SET_SECURE_MODE; the blit tests do not run |
| CP_SET_SECURE_MODE 0 without a zap | CP hangs (Pixel 1 0129a) | only after auth_and_reset returns 0/0; `kick` times out after 200 ms |
| GPU hang under load | fence stuck | `sweet_gpu_wait` declares it hung after 2 s and the service goes off (1686-1688); present falls back to memcpy |
| PLL does not lock | gfx3d stays on XO | switch the RCG only after LOCK_DET |
| heat at high clocks | no thermal management, no LM | ≤ 596 MHz; long runs at 257-414 MHz first |
| stray GPU address into a secure carve-out | XPU violation, effect **unverified** | optional tighter identity map (3.1) |
| sweetgl QuartzCore hooks miss on the iPhone X cache | renderer glitches, no crash (guards) | logged at backboardd start; re-derive the addresses (4.3) |

Visibility: every line goes through printf/IOLog into the pstore PRZ and the UFS ring, which the
reader returns after any reset. Keep `taimenreboot` short (180 s) for E0-E4.

## 8. Experiments (question 5)

### 8.1 Ladder

All runs: `ROOT=1` (boot-xnu then defaults to `cpus=4 taimenhwscale=2`, `boot-xnu.sh:17-21`),
`ROOTFIX=<current blob>`, `GPUFW=../research/taimen/firmware`.
New boot-args: `taimengpudry`, `taimengpuclk` (MHz, default 19 = XO) and `taimengpucopy`.

| stage | boot-args | proves | effort |
|---|---|---|---|
| E0 census | `sweetgpu=1 taimengpudry=1 taimenreboot=180` | SPMI port, S1 state, ABL's GCC/GPU CC state; no writes except `gcc_gpu_cfg_ahb_clk` | P2 + about 100 lines, 1 boot |
| E1 power | `sweetgpu=1 taimenreboot=180`, then `+ taimengpuclk=257` | rail, GDSCs, clocks, first GPU reads at XO; then GPUPLL0 | about 250 lines, 2-4 boots |
| E2 CP | `sweetgpu=2 taimenreboot=180` (XO) | SMMU from EL1 on MSM8998, microcode, ring, CP_MEM_WRITE | about 120 changed lines + P1, 1-2 boots |
| E3 zap + blit | `sweetgpu=3 taimenreboot=240` (XO) | SCM from XNU, PAS 13, secure exit, 2D blitter writes memory | about 80 lines, 1-3 boots (least predictable) |
| E4 compute / service | `sweetgpu=7`, then `sweetgpu=8 taimengpuclk=596`, `taimenreboot=300` | shaders, VA window, quad API, 596 MHz at 970 mV | about 40 lines, 1-2 boots |
| E5 GPU copy | `sweetgpu=8 taimengpuclk=596 taimengpucopy=1 taimenhwscale=3 taimenreboot=600` | GPU writes the scan buffer, CPU copy gone | about 120 lines, 2 boots |
| E6 Mesa | E5 + `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib sweetnull=backboardd`, ROOTFIX with the dylib | CoreAnimation renders on the A540 | build pipeline 1-2 days, sweetgl fixes, QuartzCore check; then open-ended (the Pixel 1 needed dozens of sweetgl fixes, most carry over) |

### 8.2 Expected lines, failure modes, bounds

**E0** (the strings are a spec for the new code):

```
TAIMEN GPU: census (taimengpudry=1)
TAIMEN SPMI: arbiter version 0x3....... (v3), ppid 0x514 apid N owner 0
TAIMEN GPU: S1 type 1c subtype 09 range 00 vset NNN (MMM mV) mode xx en 80|00 status xx
TAIMEN GPU: gcc cfg_ahb xxxxxxxx gpu_bimc_gfx xxxxxxxx bimc_gfx xxxxxxxx iref xxxxxxxx misc xxxxxxxx vote xxxxxxxx
TAIMEN GPU: gcc cfg_ahb on: xxxxxxxx
TAIMEN GPU: reading gpucc
TAIMEN GPU: gpucc cxo xxxxxxxx cx xxxxxxxx/xxxxxxxx gx xxxxxxxx misc xxxxxxxx gfx3d xxxxxxxx/xxxxxxxx cbcr xxxxxxxx pll mode xxxxxxxx l xx cal xx user xxxxxxxx opmode x frac xxxx
TAIMEN GPU: mem-acc 0x1fcf004 = xxxxxxxx
```

- Expected values **(unverified)**:
  - GDSCs collapsed (PWR_ON clear);
  - GX_DOMAIN_MISC bit 0 set (clamped);
  - PLL MODE 0 (in reset);
  - gfx3d CBCR bit 31 set;
  - the GCC GPU branches off, unless ABL used the GPU.
- Failure readings:
  - an arbiter version below 0x20010000, or owner ≠ 0, stops E1;
  - a log that ends at "reading gpucc" means the GPU CC needs more than cfg_ahb (bounded by the
    deadline on another core).

**E1:**

```
TAIMEN GPU: S1 725 mV (vset 129): en 80 status 8x
TAIMEN RPM: rwmx vlvl 128: ok; rwcx vlvl 128: ok
TAIMEN GPU: cx gdsc on (hw_ctrl 8xxxxxxx)
TAIMEN GPU: gx: bcr pulse, gmem reset pulse, clamp off (misc xxxxxxx0), gdsc on (8xxxxxxx)
TAIMEN GPU: gfx3d xo: rcg xxxxxxxx/00000000 cbcr xxxxxxxx; rbbmtimer cbcr xxxxxxxx
TAIMEN GPU: reading GPU registers
SWEET GPU: VBIF_VERSION xxxxxxxx RBBM_STATUS xxxxxxxx ALWAYSON xxxxxxxx -> xxxxxxxx (counting: GPU alive)
TAIMEN GPU: gfx3d measured 19.2 MHz
(with taimengpuclk=257:)
TAIMEN GPU: gpupll0 L 1a frac c555 locked in N us (mode 8xxxxxxx)
TAIMEN GPU: gfx3d measured 257.0 MHz
SWEET GPU: RBCCU power on (xxxxxxxx), SP power on (xxxxxxxx)
```

- Good values: both RCGs with UPDATE (bit 0) clear and ROOT_OFF (bit 31) clear; the gfx3d CBCR with
  bit 31 clear and bits 14, 13 and 0 set; the two ALWAYSON reads different.
- Failure readings:
  - "S1 enable FAILED" or the wrong subtype stops at once;
  - a GDSC timeout logs GDSCR and HW_CTRL and stops, leaving everything powered;
  - "not counting" means the clock tree is wrong (compare the CBCRs);
  - a measured clock far from the target means the RCG or PLL value is wrong; go back to XO;
  - no line after "reading GPU registers" is a bus stall; the deadline or the watchdog ends it.

**E2** (mostly existing strings, `sweet_gpu.c:1934-2033`):

```
SWEET GPU: SMMU sCR0 xxxxxxxx IDR0 xxxxxxxx IDR1 xxxxxxxx SMR0 xxxxxxxx S2CR0 xxxxxxxx CB0 SCTLR xxxxxxxx
TAIMEN GPU: SMMU NUMSMRG n NUMCB m CB base +0x8000; valid SMRs: <list or none>; using SMR i
SWEET GPU: SMMU identity ok: pt xxxxxxxxx SMR0 80000000 S2CR0 00000000 CBAR0 00010000 SCTLR 00000007 TCR 0080001c TTBR0 xxxxxxxx tlbsync 1
SWEET GPU: ring consumed: RBBM_STATUS xxxxxxxx RPTR n WPTR n CP_HW_FAULT 00000000 ... TRUST 00000001
SWEET GPU: scratch c0ffee01 -> CP_MEM_WRITE landed: COMMAND PROCESSOR WORKS
```

- With SMR index i the identity line shows SMRi/S2CRi.
- Failure readings:
  - "REJECTED" means a value did not read back: stop, and never start the CP;
  - "ring NOT consumed" with CP_HW_FAULT ≠ 0 points at microcode or the 4K ring;
  - an SMMU FSR ≠ 0 after the ring means the translation is wrong;
  - a stall on one SMMU write is identified by the last `smmu wr` line.

**E3:**

```
TAIMEN SCM: is_call_avail(02000201) -> x0 0 x1 1
TAIMEN SCM: pas_supported(13) -> x0 0 x1 1
TAIMEN GPU: zap mdt NNNN bytes: 1 PT_LOAD paddr xxxxxxxx memsz xxxx flags xxxxxxxx (fixed|relocatable) -> at xxxxxxxx
SWEET GPU: zap init_image (meta xxxxxxxx, NNNN bytes) -> 0/0
SWEET GPU: zap mem_setup 95a00000 + xxxx -> 0/0          (relocatable only)
SWEET GPU: zap auth_and_reset -> 0/0
SWEET GPU: marker after secure-mode packet 5ec0de00 (want 5ec0de00)
SWEET GPU: blit copy RGBA8 64x64: fence 100 (want 100), ... 0 mismatches ... -> GPU 2D BLIT WORKS
SWEET GPU: blit RGBA8 -> RGB888 64x64: fence 101 (want 101), ... -> GPU 2D BLIT WORKS
SWEET GPU: blit scale RGBA8 32x32 -> RGB888 46x46: fence 102 ...
```

- The scale blit may report mismatches: nearest against pixel-centre sampling; the Pixel 1 only
  counted them.
- That TRUST_CNTL then reads 0 is **(unverified)**.
- Failure readings:
  - "x0 unchanged" means the SCM is not reaching TZ: stop, and compare with PSCI (NOTES:395-398);
  - init_image ≠ 0: metadata placement or size (move it below 4 GB);
  - mem_setup or auth ≠ 0: wrong carve-out;
  - "CP_SET_SECURE_MODE 0 NOT read": the zap is not active;
  - blits "nothing written": still secure (Pixel 1 0128a).

**E4:**

```
SWEET GPU: GPU VA window 8-16 GB ready
SWEET GPU: sample...: fence ... (want ...), max channel diff N, 0 bad ... -> GPU TEXTURED QUAD MATCHES THE CPU
SWEET GPU: service ready (257 MHz, GPU VA window 8-16 GB)
SWEET GPU: api selftest: sweet_gpu_quad -> 0 in N us, 0 bad pixels; bad rect -> 22, unmapped -> 14 -> API OK
TAIMEN GPU: corner 6: S1 970 mV, MX 256, mem-acc 0, gpupll0 1192 MHz: gfx3d measured 596.0 MHz
SWEET GPU: full-screen quad N us at 257 MHz, M us at 596 MHz
```

- Failure readings:
  - "wrong" on the compute or sample tests at A540 ECO values: compare with the A530 values once (one
    boot);
  - fence stuck at 596 MHz but fine at 257: voltage or MX; stay at the lower corner.

**E5:**

```
TAIMEN SCALE: mode 3 (QSEED3 1330x2880, double-buffered)
TAIMEN SCALE: GPU copy on: SCAN0 9e400000 -> gva xxxxxxxxx, SCAN1 9d400000 -> gva xxxxxxxxx
TAIMEN SCALE: GPU copy: N frames, avg X us, max Y us (memcpy before: Z us)
```

- Failure readings:
  - "GPU copy failed (errno N): memcpy from now on": `sweet_gpu_quad` returned an error;
  - wrong colours or a torn picture: swizzle or buffer handover;
  - an SMMU fault line names the mapping below the fault address (1607-1609).
- The MDP side is unchanged: CTL0 kicks and DSI health as in boots 34-36.

**E6:**

```
SWEET GPU: user client opened by backboardd
sweetgl: screen FD540 (freedreno)
sweetgl: EAGLContext 0x... API N on Mesa (GLI 0x...)
sweetgl: texture function 0x.. = ...: installed     (or: texture function fixes: unexpected QuartzCore, not installed)
SWEET FB: N swaps
```

- "screen FD530" means the sweet_drm GPU_ID fix is missing.
- Success is measured in frames: SWEET FB swap counts and the swap-gap statistics (`sweet_fb_stat_gap`,
  `SweetFramebuffer.cpp:835-864`) against today's 2-8 frames/s.
  - The Pixel 1 reached 20-40 frames/s at 750×1334, limited by CoreAnimation's CPU work.
  - taimen draws 2.7 times the pixels on a faster GPU, and its CPU work runs on 1.55 GHz silver cores
    while the gold cores sit at 299 MHz (NOTES:423-425).
  - The expected rate is therefore **unverified**; the gold-core clock is the clocks plan's job.
- Failure readings:
  - a backboardd crash: ReportCrash is off in ROOTFIX, so read the kernel's exception lines;
  - a GPU hang: the service goes off, backboardd sees ENXIO, and the frames stop. Bounded by the
    deadline.
  - Rendering bugs repeat the Pixel 1 history: start with the QuartzCore address check.

### 8.3 Boot command

```
GPUFW=../research/taimen/firmware ROOT=1 ROOTFIX=<current blob> \
    tools/taimen/autoboot.sh <kernel rN> out/taimen-xnu-rN-gpuE1 sweetgpu=1 taimenreboot=180
```

## 9. Open questions

- Is PM8005 S1 on when XNU starts, at what VSET, and which regulator subtype is it? (E0)
- Do GPU CC registers need `gcc_gpu_cfg_ahb_clk`? (E0)
- Is the arbiter v2 or v3, and is ppid 0x514 owned by EE 0? (E0)
- a540_zap: p_paddr, memsz, relocatable flag, and .mdt size. (P1)
- Does TZ accept PAS metadata above 4 GB, and does a SiP SCM call from XNU reach TZ at all? (E3)
- Does QHEE emulate GPU SMMU writes on this firmware the way the Pixel 1's did? (E2)
- Does the downstream CRC setup (CRC_MND_CFG/CRC_SID_FSM) matter without the GPMU? (E1 with and without)
- Does the iPhone X shared cache match eagl.c's iPhone 8 addresses? (E6, or offline from the root image)
- Which Mesa revision built the Pixel 1 dylib? (Only matters if 25.3.6 behaves differently.)
- DDR clock under GPU load without a bandwidth vote; GPU temperature without TSENS.
