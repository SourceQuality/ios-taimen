# Porting map: Pixel 1 (sailfish, MSM8996) constants and their Pixel 2 XL (taimen, MSM8998) counterparts

Every hardware constant the Pixel 1 port hard-codes, grouped by subsystem, with the taimen value
where the taimen Linux device tree (`../research/taimen/live.dts`, the FDT as patched by ABL) or
`/proc/iomem` gives one. "?" means not yet measured.

**Conventions**
- Paths are relative to this repo. `live.dts` is the decompiled taimen FDT; `iomem` is the taimen `/proc/iomem`.
- For kernel patches, `NNNN:L` is the line number inside `xnu/patches/NNNN-*.patch`.
- INTID = GIC SPI + 32.

**What the Pixel 1 build actually compiles.** `shim/build-pixel.sh:16` builds only `start.S pixel.c
pstore.c console.c fdt.c loader.c` with `-DPIXEL1 -DWDT_SECS=120`. `shim/shim.c`, `shim/ufs.c` and
`shim/mmu.c`/`mmu.h` belong to the sweet (Redmi, SM7150) build. Their constants are not Pixel 1 values,
but they are listed below as "sweet-only" because a taimen shim would reuse them as a template.

**How the shim finds things: almost all hard-coded.**
- The Pixel shim never parses the FDT. `pixel.c:60` only logs the x0 DTB pointer.
- `fdt.c` reads only `/chosen linux,initrd-start/end`, and only through `loader_boot()` (`loader.c:79-87`).
  That is the sweet path; Pixel calls `loader_boot_rd(_end, TAGS_ADDR)` at `pixel.c:79`.
- `phys_base`/`mem_size` reach the loader through the payload header. `tools/mkpayload.py:162` takes
  them from the ADT's `/chosen dram-base/dram-size`, so they come from `mkadt.py`.
- The kernel learns GIC, vtimer, framebuffer, kickoff register and UFS from the ADT that `mkadt.py`
  builds. Everything else (TLMM, SPMI, RPM, GCC, GPU, SMMU, DSI, VIG0, IMEM, pstore, IRQs, GPIOs) is
  hard-coded `#define`s or offsets in the patches.

**Blocker.** Probes 1 and 2 (see `NOTES-taimen.md`) show ID_AA64MMFR0.TGran16 = 0: the Kryo 280 has no
16K granule. The kernel's 16K assumptions break on taimen, including the 32 MB L2-block early-log
mapping in 0081 and `shim/mmu.c`. The 4K-page path of the Exynos ports (Floppikrob/ios-galaxy-s8,
patches 0082/0084/0107 of that series) is the model.

---

### 1. GIC and timer

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| GICD base / size | 0x9bc0000 / 0x10000 (size taken from global `GICD_SIZE`) | mkadt.py:40, 24, 206 | 0x17a00000 / 0x10000 | live.dts:682-684 |
| GICR base / size | 0x9c00000 / 0x100000 | mkadt.py:40, 25 | **0x17b00000** / 0x100000 | live.dts:684 |
| GICR stride | 0x40000; ADT `redistributor-stride`, kernel accepts only 0x20000 or 0x40000 | mkadt.py:40, 207; 0079:9-39 | 0x20000, so the property can be dropped | live.dts:691 |
| GIC ITS | unused | n/a | 0x17a20000 | live.dts:694-697 |
| vtimer INTID | 27 (ADT `virtual-timer-intid`) | mkadt.py:41, 205 | **19** (PPI 3), same as sweet. Timer PPIs: 1 secure, 2 NS-phys (INTID 18), 3 virt (INTID 19), 0 hyp | live.dts:700-703 |
| timebase | 19.2 MHz (`TIMEBASE`), also used for cpu `timebase/peripheral/fixed-frequency` | mkadt.py:31, 191-196; 0083:57; 0181:204 (literal 19200000) | 19.2 MHz (0x124F800) | live.dts:703 |
| SPI attach helper | GICD ICFGR/IROUTER at `gicd_base` from the ADT | 0181:18-60 | same mechanism | n/a |
| sweet-only GIC (shim.c) | GICD 0x17a00000, GICR 0x17a60000, size 0x100000, PE size 0x20000, VTIMER 19, PTIMER 18 | shim.c:19-34 | GICD matches; GICR must become 0x17b00000 | live.dts:684 |
| sweet-only timer | `to_us` assumes 19.2 MHz; UFS timeout 19200000 | shim.c:370; ufs.c:123 | 19.2 MHz | live.dts:703 |

### 2. CPUs, SMP, PSCI

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| CPU MPIDRs | 0x0, 0x1, 0x100, 0x101 (2+2 Kryo) | mkadt.py:46-47 | 0x0-0x3 and 0x100-0x103 (4+4 Kryo 280) | live.dts `cpus` (cpu@0..3, cpu@100..103) |
| CPU max clocks | 1593600000 / 2150400000 | mkadt.py:46-47 | ? (spec: about 1.9 / 2.45 GHz) | not in the DTS directly |
| CPU compatible match | `qcom,kryo` | mkadt.py:182; 0085:10 | any string will do (DTS says `arm,armv8`) | n/a |
| MPIDR sanity check | Aff1 = cluster, Aff0 < 16 | 0088:11-14 | 0x10x still passes | n/a |
| `l2-cache-id` | cluster index | mkadt.py:189 | same scheme | n/a |
| PSCI | SYSTEM_RESET 0x84000009 via `smc #0` | pixel.c:12, 24-30; stub.c:63 | PSCI 1.0, smc. Probe 2 confirmed SYSTEM_RESET works | live.dts:16337-16340 |
| sweet-only PSCI probe | CPU_ON `(i<<8)` MPIDR pattern, NR_CPUS 8 | shim.c:37-48, 193 | the `i<<8` pattern is wrong for taimen | n/a |
| CPU clocks (perf) | APC0 PLL 0x6400000, APC1 0x6480000, CBF mux 0x9a11000, CBF PLL 0x9a20000; PLL config 0x200d4828 | 0093:24-27, 73-78; 0095:75-78 | ? Different block: APCS at 0x17911000 / 0x17811000 (cfg_rcgr +0x54, pll user_ctl 0x1791600c / 0x1781600c) | live.dts:2479-2482 |
| CPU voltage probe | KRYO0 0x99a2000, KRYO1 0x99d2000 | 0094:93-94 | ? (8996-only) | n/a |
| turbo via SAW | SAW 0x9a10000, MEMACC 0x99e0000 | 0102:27-28 | ? (8996-only) | n/a |

### 3. Memory layout, payload, boot_args

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| kernel RAM (ADT dram-base/size, boot_args physBase/memSize) | 0x100000000 + 0x7e550000 (upper DDR bank, no carve-outs) | mkadt.py:39, 95, 102-103; mkstub-payload.py:11-12; loader.c:128-141 | Candidate 0x100000000 + 0x7e4c0000 (top of `b1000000-17e4bffff`). The whole range 0xb1000000-0x17e4bffff (0xcd4c0000) is also contiguous System RAM | iomem last line; live.dts:16333-16336 (memory 0x80000000 + 0xFE4C0000) |
| kernel load (LK) | Image at 0x80080000; payload appended at `_end` | pixel.c:40-45 | ? ABL (UEFI) placement not measured. Linux runs at 0x80080000. The first RAM range ends at 0x857fffff (`removed_regions@85800000`), so Image + payload get at most about 87 MiB from 0x80080000 | iomem; live.dts `removed_regions@85800000` |
| DTB / tags address | 0x82600000 (`TAGS_ADDR`, used only as rd_end in the overlap check) | pixel.c:45, 79 | ? Use x0 at entry | n/a |
| ramdisk | unused on Pixel: LK forces 0x82800000 | pixel.c:42-43 | ABL puts it at 0xa0000000 (inside RAM range 95b00000-afffefff) | NOTES-taimen.md |
| Image header | text_offset 0, flags 0xa | start.S:10-12 | probe images boot with this | n/a |
| ADT + boot_args placement | after kernel_end, 16K aligned | loader.c:118-122 | 16K assumption (see blocker above) | n/a |
| virt base (stub) | 0xfffffff007004000 | mkstub-payload.py:10 | n/a | n/a |
| kernel io map size | 64 MiB | 0165:14 | n/a | n/a |
| sweet-only | PHYS 0xb0000000 + 0x4d000000; highmem 0x100000000 + 0xfcc00000 | mkadt.py:14-20 | n/a | n/a |

### 4. Framebuffer and display (MDP/DSI)

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| FB base | 0xc0001000 (read off MDP VIG0 SRC0_ADDR) | console.h:5; mkadt.py:42; loader.c:131 | **Conflict.** The node is named `splash_region@9d600000` but `reg = 0x9D400000 + 0x2400000`. Probe 2 painted at 0x9d600000; bands not yet confirmed by eye. Settle it by reading VIG0 SRC0_ADDR at 0xc905014 | live.dts:16469-16473; NOTES |
| FB size | 0x31ff000 | mkadt.py:42 | 0x2400000 region | live.dts:16470 |
| FB geometry | 1080x1920, 24 bpp (bytes B,G,R), stride 3240 (0xca8) | console.h:6-8, 15; console.c:20-25; mkadt.py:42, 254-255 | 1440x2880, panel bpp 24 (stride 4320 if ABL also uses RGB888). Read SRC_FORMAT/YSTRIDE from VIG0 | live.dts:12818-12819, 12831 |
| panel | command mode; S3708 touch coordinates 1079x1919 | 0096:22 | SW43402, `dsi_cmd_mode`, DSC v1.1, **2 DSC encoders**, slice 720x16, 8 bpp, 60 Hz. Expect a split (two LM/PP) path | live.dts:12813-12815, 12878-12897 |
| MDSS/MDP base | 0x900000 (MDP top 0x901000) | implied: 0164:519-528; 0168:138 | 0xc900000 (mdp_phys 0x90000), VBIF 0xc9b0000 | live.dts:12678-12681 |
| kickoff (CTL0 START) | 0x90201c = CTL0 0x902000 + 0x1c | console.c:46; mkadt.py:43, 257; 0079:123-129 | likely 0xc90201c (ctl-off 0x2000). With DSC split, CTL1 may also be involved | live.dts:12721 |
| CTL0 LAYER / FLUSH | kick-0x1c+... ; `kickoff[-7]` LAYER(LM0), `kickoff[-1]` FLUSH | 0167:30-39; 0164:486 | same offsets if the CTL layout holds | n/a |
| MDP INTR_STATUS / CLEAR, PP0_DONE bit 8 | 0x901014 / 0x901018 | console.c:42-51; 0099:16, 22-23; 0164:526-528 | ? Probably 0xc901014 / 0xc901018. Bit 8 is unverified on MDSS 3.x (PP offsets 0x71000+) | live.dts:12726 |
| VIG0 pipe | 0x905000 (= kick - 0x1c + 0x3000); SRC_FORMAT +0x30, UNPACK +0x34 | 0164:483-484, 519-525 | 0xc905000 (vig-off 0x5000 0x7000 0x9000 0xB000) | live.dts:12712 |
| DSI0 controller | 0x994000 (= `(kick & ~0xffff) + 0x94000`) | 0168:18, 137-141 | 0xc994000 (the formula still works); ctrl1 0xc996000; PLL 0xc994400 | live.dts:14393-14400, 14422, 14567 |
| GPU present buffers | 0xc0800000 / 0xc1000000, slot 0x800000 (low bank, below 4G) | 0164:201-203 | ? Needs a new home (no separate low bank). Must be below 4 GB and outside reserved regions | n/a |
| MDP IRQ | polled | n/a | SPI 83, INTID 115 | live.dts:12682 |
| ADT display identity | canvas 750x1334, display-scale 2, iPhone 8 / D20AP, sRGB gamut, home-button-type 1 | mkadt.py:119, 228-229, 278, 283-288, 295 | 1440x2880 (2:1) fits the iPhone X path better (1125x2436, scale 3) | n/a |
| sweet-only | FB 0x9c000000 1080x2400x4; FB_PHYS 0x9c000000 | console.h:10-13; mmu.h:35; mkadt.py:26-28 | n/a | n/a |
| backlight | DCS through DSI0, max 255 | 0168:171-172 | ? (SW43402 DCS) | n/a |

### 5. Persistent logs (pstore / ramoops, UFS log ring)

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| ramoops region | 1 MiB at 0x83500000: 8 x 32K records, then 512K console | pstore.c:2-5 | 2 MiB at 0xb0000000: record 0x20000, console 0x100000, pmsg 0x80000, no ECC. `alt-memory-region` 0xb0e00000; metadata 0xaffff000 | live.dts:16272-16279, 16475-16491; iomem persistent_ram lines |
| console zone (PRZ_BASE) | 0x83540000, sig "DBGC" 0x43474244, data max 0x80000-12 | pstore.c:10-12; 0081:138-139, 263 | console zone **0xb0080000**, data max 0x100000-12. Check whether Android reads the main or the alt region after reset | iomem `b0080000-b017ffff` |
| early log block (kernel V=P Device block) | 0x82000000 (32 MB block holding the PRZ) | 0081:20, 54, 94-95, 262 | 0xb0000000 (32 MB aligned), but a 4K granule means 2 MB L2 blocks | n/a |
| console/panic to pstore | same PRZ | 0083:41-51; 0177 | same | n/a |
| UFS log ring | last 4 MiB + 4 KiB of the root GPT partition (`SWEETLOG` header), flushed every `ufslogsecs` (10 s) | 0047:31-43; 0069:144, 175 | same mechanism | n/a |
| crash FB (info) | n/a | n/a | 0xb0200000 + 0xc00000 | live.dts:16493-16497 |

### 6. Storage (UFS)

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| UFS host | 0x624000 / 0x2500 (ADT `/sweet-ufs reg`) | mkadt.py:44, 261-265; kernel lookup at 0028:510 | **0x1da4000 / 0x2500** | live.dts:3751-3753; iomem `01da4000-01da64ff` |
| UFS IRQ | polled | 0028 | SPI 265, INTID 297 | live.dts:3754 |
| sweet-only shim probe | 0x1d84000 | ufs.c:16; mkadt.py:30 | n/a | n/a |

### 7. Watchdog, reset, IMEM

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| APSS WDT base | 0x9830000, 32768 Hz; RST +4, EN +8, BARK +0x10, BITE +0x14; disabled at entry, re-armed to WDT_SECS (120) | pixel.c:13-17, 52-53, 72-78; build-pixel.sh:14 | 0x17817000, same msm-watchdog register layout; bark 16 s, pet 10 s; IRQs SPI 3/4 (INTID 35/36) | live.dts:4481-4486; iomem `17817000` |
| IMEM restart_reason | 0x066bf65c = 0x77665500 (reboot to fastboot) | 0117:24, 30; 0176:15-17 | 0x146bf65c | live.dts:4521-4537 |
| IMEM (older, sweet value) | 0x146aa65c | shim.c:15; 0115:20-22 | superseded | n/a |
| PS_HOLD | 0x004ab000 | 0117:11, 25 | 0x010ac000 (tcsr-boot-misc 0x1fd3000) | live.dts:705-708; iomem `010ac000` |
| sweet-only PSHOLD | 0x0c264000 | shim.c:17 | n/a | n/a |
| ADT `/arm-io/wdt` | `no-pmu` only (no hardware) | mkadt.py:219 | same | n/a |

### 8. PMIC / SPMI, keys, battery

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| SPMI arbiter | core 0x400f000, obsrvr 0x4c00000, cnfg 0x400a000; v2 PPID-to-APID map at core+0x800+4n, 256 APIDs; version check 0x20000000-0x4fffffff | 0089:30-34, 57-66, 168 | core 0x800f000, chnls 0x8400000, obsrvr 0x9400000, intr 0xa400000, cnfg 0x800a000; periph IRQ SPI 326 (INTID 358). Arb version ? Read the version register and compare with Linux `spmi-pmic-arb.c` before reusing the 0x800 map offset | live.dts:710-714; iomem 0800a000..0a61ffff |
| SPMI write channels | 0x4400000 (EE 0) | 0118:25 | 0x8400000 | live.dts:712 |
| power key | PM8994 SID 0 PON RT_STS 0x810 bit 0 (KPDPWR) | 0089:12-13 | PM8998 SID 0 PON @0x800 (kpdpwr), probably the same 0x810 bit 0 | live.dts:734-750 |
| volume up | TLMM GPIO 126, active low | 0089:14, 40 | GPIO 126 (0x7e), active low | live.dts:16051-16055 |
| volume down (reported as Home) | TLMM GPIO 121 | 0089:15-16, 41 | **Not a GPIO**: PON RESIN (`pon_2`, KEY_VOLUMEDOWN 0x72), so RT_STS 0x810 bit 1 | live.dts:756-760 |
| hall sensor | n/a | n/a | GPIO 124 | live.dts:16058-16063 |
| GFX rail | PMI8994 S2, ppid 0x317 (SID 3); vset +0x41, enable +0x46 | 0119:37-38, 156-175 | PM8005 S1, SID 5 regulator@1400, ppid 0x514, CPR `gfx_corner` | live.dts:1937-1945, 4950-4965 |
| LDO probe | PM8994 LDOn ppid 0x100 \| (0x40+n-1) | 0098:17-25 | PM8998 (SID 1) layout ? | n/a |
| battery FG | PMI8994 SID 2, FG_SOC 0x4009 (gen2), ppid 0x200\|addr>>8; 2770 mAh | 0114:11, 32, 151 | PMI8998 SID 2 `fg-gen3` (different registers); charger `qpnp-smb2` | live.dts:1249, 1382, 1484 |
| charger writes | SEC_ACCESS 0xd0 = 0xa5 | 0118:137-145 | ? (smb2) | n/a |

### 9. GPIO / TLMM, I2C, touch

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| TLMM base | 0x1010000, 4 KiB tile per GPIO (CFG +0, IN_OUT +4, INTR_CFG +8, INTR_STATUS +0xc) | 0089:39; 0096:53; 0169:172; 0181:311-321 | **0x3400000** (size 0xc00000), tile stride ? (8998 is also 0x1000 per GPIO in Linux pinctrl-msm8998) | live.dts:8926-8928 |
| TLMM summary IRQ | SPI 208, INTID 240 | 0181:311-318 | SPI 208, INTID 240 (same) | live.dts:8929 |
| touch controller | Synaptics S3708 RMI4 at I2C 0x20 | 0096:10, 58 | **STM FTM4 (`ftm4_fts`) at I2C 0x49**, firmware ftm4_fw.ftb, max 1439x2879 | live.dts:14873-14891 |
| touch I2C bus | BLSP1 QUP3 0x7577000, 400 kHz from XO | 0096:11, 52, 130-131 | `i2c@c179000` (BLSP1 QUP5 by address), 400 kHz, qup IRQ SPI 99 (INTID 131) | live.dts:14850-14862 |
| touch GPIOs | SDA 47, SCL 48 (func 3), reset 89, attn 125, I2C mux 80 (0 = AP) | 0096:15, 54-57; 0097:12-13, 22, 86-95 | SDA 87 (0x57), SCL 88 (0x58), reset 89, irq 125, switch 75 (0x4b), ta_detect 50 | live.dts:14870-14871, 14882-14889 |
| touch IRQ | TLMM summary INTID 240, level low | 0181:311-321, 367 | direct line SPI 216, INTID 248 (GIC is the parent), or the summary at 240 | live.dts:14877-14878, 12 |
| touch clocks | GCC vote 0x352004 bit 17 (BLSP1 AHB), QUP3 I2C CBCR 0x31d008, RCGR +0x20 | 0096:13-14, 50-51 | ? GCC base 0x100000. Look up blsp1_ahb and blsp1_qup5_i2c_apps offsets in Linux `gcc-msm8998.c` | live.dts:2408-2410 |
| touch rails | PM8994 L6/L18/L23 1.8 V, L17 3.3 V, L19 2.85 V via RPM (`sweetrpm` boot-arg) | 0100:18-19, 38-44 | pm8998 L6 (dvdd), L28 (avdd) | live.dts:14886-14887 |
| other I2C (info) | n/a | n/a | c175000..c17a000, c1b5000..c1ba000 | live.dts:14766-15232 |

### 10. RPM (GLINK) and clocks

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| RPM msg RAM | 0x68000, 24 KiB, TOC "grt0" in last 256 B | 0100:57-61, 79-85 | 0x778000, size 0x7000 | live.dts:3554-3559 |
| RPM doorbell | APCS IPC 0x9820000 + 0x10, bit 0 (code writes `ipc+0x10`) | 0100:81, 141 | 0x17911008, mask 0x1. The +0x10 offset must change. RPM IRQ SPI 168, INTID 200 | live.dts:3556-3558 |
| RPM resources | ldoa n; clk1 pnoc/snoc/cnoc, clk2 bimc, mmxi, ipa, ce1, aggr, clka...; MX = smpa 2 corner | 0146:101-123; 0147:41, 81-102; 0164:109-111 | ? 8998 RPM resource ids (MX/CX rails, clk ids) need checking | n/a |
| GCC | 0x300000; DBG 0x362000; XO4 0x343000; PCIe/AGGRE0 CBCRs | 0119:44, 64; 0145:17-18; 0169:171, 188-206 | 0x100000 (size 0xb0000); offsets ? | live.dts:2408-2410 |
| MMSS CC | 0x8c0000 (GPU GDSC 0x4034, GX GDSC 0x4024, gfx3d RCG 0x4000, ...) | 0119:40-43, 63 | MMSS CC 0xc8c0000. GPU clocks moved to GPU CC / GFX CC at 0x5065000 | live.dts:2418-2433 |

### 11. GPU (Adreno 530 to 540) and SMMU

| item | Pixel 1 value | where | taimen value | taimen source |
|---|---|---|---|---|
| GPU base | 0xb00000 | 0119:45, 65 | 0x5000000 / 0x40000, chipid 0x5040001 | live.dts:8764-8774; iomem |
| GPU IRQ | none attached (polled) | 0120-0164 | SPI 300, INTID 332 | live.dts:8769 |
| GPU mem_acc | 0x7af000 | 0119:66 | ? | n/a |
| GPU SMMU | 0xb40000 (SMMUv2, CB at +0x8000, GR1 +0x1000) | 0120:9, 38; 0121:17-52 | 0x5040000 / 0x10000; context IRQs SPI 329-331 (INTID 361-363); `qcom,skip-init` | live.dts:5370-5380 |
| SMMU stream | SMR0 = stream 0 (gfx3d_user); stream 2 = secure | 0123:8-19 | gfx3d_user = SID 0, gfx3d_secure = SID 2 (same) | live.dts:8915-8920 |
| zap shader | PA 0x90f00000 (1 MB gpu_mem), PAS id 13 | 0130:12, 29-31 | PAS id 13 (`a540_zap`); candidate region `pil_ipa_gpu_region` reg 0x95a00000 + 1 MB (node named @95200000) | live.dts:8745-8749, 16387-16391 |
| firmware | a530 pm4/pfp | n/a | a530_pm4/pfp + a540_gpmu.fw2 | NOTES; live.dts:8787-8790 |
| GPU VA | GVA_BASE 0x200000000, 8 GB | 0137:76-78 | SoC-independent | n/a |

### 12. Wi-Fi (0169-0181): not applicable to taimen

Taimen uses WCN3990 through the modem, not QCA6174 over PCIe. Listed for completeness only.

| item | Pixel 1 value | where | taimen value |
|---|---|---|---|
| PCIe RC0 | PARF 0x600000, PHY 0x34000, DBI 0xc000000, CONF 0xc100000, BAR 0xc400000 | 0169:173-176; 0170:124 | PCIe RC0 0x1c00000 serves Easel (mnh), not Wi-Fi (live.dts:4371-4376) |
| WLAN DMA | 0xa0000000 | 0170:125 | **Collides with ABL's ramdisk at 0xa0000000** |
| anoc0 SMMU | 0x1640000 | 0170:127 | anoc1 0x1680000, anoc2 0x16c0000 (iomem) |
| WLAN IRQ | INTID 276 (SPI 244) | 0181:179-188 | n/a |
| ADT `wlan` node | `wlan-pcie,sailfish` | mkadt.py:231-238 | drop or replace |

### Where each kind of constant lives

- **Shim (C/asm, Pixel build):** WDT base and offsets, PSCI function IDs, TAGS_ADDR, FB base/geometry/bpp (`console.h`), the MDP INTR/START addresses in `con_flush`, PRZ_BASE, Image header flags.
- **mkadt.py `PIXEL1` dict / `--pixel1` branches:** RAM base/size, GICD/GICR/stride, vtimer, FB/kickoff, UFS, CPUs and clocks, display canvas and scale, wlan node, identity (D20AP). Shared globals: TIMEBASE, GICD_SIZE/GICR_SIZE, SOC_BASE 0x1000000.
- **Kernel patches:** everything else. Pstore and early-log addresses are in `SWEET.h` (0081:262-263); IMEM/PSHOLD, TLMM, SPMI, RPM, GCC/MMCC, GPU/SMMU, zap, GPU present buffers, IRQ INTIDs, GPIO numbers and I2C addresses are inline `#define`s. DSI0 and VIG0 are derived from the ADT kickoff register by fixed offsets (0164:519-528, 0168:137-141), so they follow automatically if the MDSS layout keeps the 0x1000/0x2000/0x5000/0x94000 offsets.
