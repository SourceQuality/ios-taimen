# Taimen display: hardware scaling with VIG0 + QSEED3 (plan, 2026-10-23)

Status: plan only. Nothing here has run on the phone. Items marked **[unverified]** are inferences or
come from drivers for the same SoC, not from this phone. Line numbers in `scratch/xnu` refer to the
patched tree as it is today (patches up to 0217).

Goal: backboardd's 1125x2436 BGRA frame fills the panel height (1125x2436 -> 1330x2880, centred at x=55,
aspect kept) through the MDP's VIG0 pipe and its QSEED3 scaler, double-buffered, so the CPU never writes
the buffer the MDP is sending. The ABL topology (CTL0, LM0, PP0, DSC, INTF1, DSI0) is left as it is.

## 0. Summary

- The scaler needs no LUT. QSEED3's Y/RGB filter mode 3 (bilinear) is what every Linux driver uses by
  default, with `lut_flag = 0`. The only registers involved are VIG0 (0xc905000) and its QSEED3 block at
  VIG0+0xA00, both in the 4 KiB page the kernel already maps for `sweet_fbcon_flip`. The only flush bit
  needed is CTL0_FLUSH bit 0 (VIG0).
- ABL most likely drives a **single layer mixer** (LM0, 1440 wide) and one DSC encoder with two 720-pixel
  slices. It does not use source split or Linux's dual-LM/DSC-merge layout ("split_mode:1" is how Linux
  sets things up later). So one scaled VIG0 rect at OUT_XY (55,0) is enough. **[inferred; experiment E0
  checks it]**
- The MDP fetches physical addresses directly: no HLOS SMMU context is active after ABL, and SRC0_ADDR is
  32 bits. Scan buffers must therefore sit below 4 GiB. Both fit in the 36 MiB splash region
  (0x9d400000-0x9f800000) next to or over the old panel buffer.
- **This plan does not remove the per-frame copy.** backboardd's surfaces are paged kernel memory above
  4 GiB, so the CPU still copies 11 MB per frame into a low buffer. What the plan gains:
  - the picture fills the panel height;
  - the copy runs while the previous frame is being sent, instead of waiting for the transfer to finish;
  - the MDP fetches 11.0 MB per frame instead of 16.6 MB;
  - no border clears.
  Removing the copy needs the MDP to fetch backboardd's surfaces directly (section 8, Phase 3), which is
  a separate project.

## 1. What ABL leaves (evidence)

From `research/taimen/shim-diag-ufs.txt` (lines 11-33). The kernel's 0203 report shows the same values
during iOS (NOTES-taimen.md:272-275).

| Register | Address | Value | Decode |
|---|---|---|---|
| VIG0 SRC_SIZE | 0xc905000 | 0x0b4005a0 | 1440x2880 (h<<16 \| w) |
| VIG0 OUT_XY | 0xc905010 | 0 | |
| VIG0 SRC0_ADDR | 0xc905014 | 0x9d400000 | physical address (section 4) |
| VIG0 YSTRIDE0 | 0xc905024 | 0x1680 | 5760 bytes |
| VIG0 SRC_FORMAT | 0xc905030 | 0x000237ff | 4 bytes/pixel, 4 components, 8-bit, tight, SRCC3_EN (bit 8, alpha used) |
| VIG0 UNPACK | 0xc905034 | 0x03020001 | memory bytes B,G,R,A |
| CTL0 LAYER(LM0) | 0xc902000 | 0x01000005 | BORDER_OUT (bit 24), VIG0 at mixer value 5 = blend stage 3 |
| CTL0 LAYER(LM1) | 0xc902004 | 0 | nothing staged on LM1, not even border |
| CTL0 TOP | 0xc902014 | 0x00020020 | command mode (bit 17), interface select 2 = INTF1 |
| CTL0 FLUSH | 0xc902018 | 0x60000000 | bits 29/30; never changes over 5800 kicks (boot 19) |
| DSC0 COMMON_MODE | 0xc981000 | 0 | no split, no merge, command mode |
| PP1 AUTOREFRESH | 0xc971830 | 0 | (PP1 untouched) |

How the fields decode:
- **SRC_FORMAT and UNPACK** follow `mdss_mdp_format_setup` (wahoo `mdss_mdp_pipe.c:2123-2155`) and
  `sde_hw_sspp_setup_format` (`sde_hw_sspp.c:280-298`).
- **Mixer value 5 in CTL_LAYER** follows the mdss stage enum (BASE=1, STAGE_0=2 ... STAGE_3=5,
  `mdss_mdp_hwio.h:444-455`) and SDE's `mix = i + 1` (`sde_hw_ctl.c:306-315`). Both give blend stage 3,
  whose blend registers sit at LM+0xB0 (`mdss_mdp_hwio.h:464`; mainline `msm8998_lm_sblk` blendstage_base
  0xb0, `dpu_hw_catalog.c:374-381`).
- **CTL_TOP** follows `mdss_mdp_hwio.h:197-198`, `sde_hw_ctl.c:384-401`, and mdss's INTF enum
  (INTF0=1, INTF1=2).
- **FLUSH bits 29/30** are INTF2/INTF1 in SDE's layout (`sde_hw_ctl.c:200-219`). mdss never writes them.
  The kernel never writes FLUSH on the current present path, so this is ABL's last value. Whether the
  bits are pending or just read back is **[unverified]**.
- **DSC COMMON_MODE**: BIT(0) is split and BIT(1) is merge (`mdss_mdp_ctl.c:3072-3090`). Linux's
  dual-LM/DSC-merge layout writes 3. A value of 0 matches Linux's single-LM path
  (`__dsc_setup_single_lm_single_display`, `mdss_mdp_ctl.c:3366-3414`): one encoder doing
  `pic_width / slice_width` = 2 slices per line.

Interpretation of the topology **[inferred]**: one VIG pipe on LM0 (1440x2880) feeds PP0, then DSC0 (two
720x16 slices), then INTF1, then DSI0. Nothing goes through LM1, and there is no source split (that would
stage VIG0 in both CTL0 LAYER0 and LAYER1 and set LM1 OP_MODE bit 31, `mdss_mdp_ctl.c:5119-5120`). The
DSI side agrees: one stream of 1440 compressed bytes per line. In the kernel's DSI0 dump,
STREAM0_CTRL is 0x05a10039 (word count 0x5a1 = 1440+1) and STREAM0_TOTAL is 0x0b4001e0 (2880 lines of
480 pclk). Linux's own layout comes from the panel DT: `lm-split <0x2d0 0x2d0>`, 2 encoders,
720x16 slices (`live.dts:13068-13071` for `qcom,mdss_dsi_sw43402_dsc_qhd_cmd` at `live.dts:12990`).

The shim's "LM0 out_size @0c945000" read the wrong offset: LM OUT_SIZE is at +0x04
(`mdss_mdp_hwio.h:459-462`). LM0 OUT_SIZE has never been read. E0 reads it.

## 2. Register recipe

### 2.1 Offsets (all relative to MDSS 0xc900000)

- **VIG0** is at 0x5000 (`live.dts:12712`). Mainline lists `sspp_0` at 0x4000 because its offsets are
  relative to MDP (0xc901000): `dpu_3_0_msm8998.h:67-73`.
- **QSEED3** is at VIG+0xA00 and its LUT at VIG+0xB00 (`live.dts:12763-12765`;
  `msm8998-sde.dtsi:89,113-116` "qseedv3", `qcom,sde-vig-qseed-off = <0xa00>`; mainline scaler_blk base
  0xa00, len 0xa0, `dpu_hw_catalog.c:281-283`). Mainline gives MSM8998 VIG scaler version 1.2
  (`dpu_vig_sblk_qseed3_1_2`, `dpu_hw_catalog.c:338-339`). SDE's register write path for that version
  is `scaler_blk.version == 0x1002` (`sde_hw_sspp.c:639-649`).
- **SSPP registers** (`mdss_mdp_hwio.h:243-306`; `sde_hw_sspp.c:22-66`):
  - SRC_SIZE 0x00, SRC_IMG_SIZE 0x04, SRC_XY 0x08, OUT_SIZE 0x0C, OUT_XY 0x10
  - SRC0_ADDR 0x14, YSTRIDE0 0x24, YSTRIDE1 0x28
  - SRC_FORMAT 0x30, UNPACK 0x34, SRC_OP_MODE 0x38
  - CURRENT_SRC0_ADDR 0xA4 (read-only), DECIMATION_CONFIG 0xB4
  - SW_PIX_EXT_C0 LR/TB/REQ_PIXELS at 0x100/0x104/0x108, C1C2 at 0x110/0x114/0x118, C3 at
    0x120/0x124/0x128
- **QSEED3 registers** (`mdss_mdp_hwio.h:322-377`; `sde_hw_sspp.c:90-128`; mainline `dpu_hw_util.c:17-51`):
  - HW_VERSION 0x00, OP_MODE 0x04, PHASE_INIT 0x0C
  - PHASE_STEP_Y_H 0x10, Y_V 0x14, UV_H 0x18, UV_V 0x1C
  - PRELOAD 0x20, detail enhancer 0x24-0x3C
  - SRC_SIZE_Y_RGB_A 0x40, SRC_SIZE_UV 0x44, DST_SIZE 0x48, COEF_LUT_CTRL 0x4C, CLK_STATUS 0x5C
  - MISR 0x70-0x78, PHASE_INIT_Y_H..UV_V 0x90-0x9C (other scaler versions only)
- **OP_MODE bits** (`mdss_mdp_hwio.h:355-377`):
  - EN bit 0, DIR_EN bit 4, DE_EN bit 8, ALPHA_EN bit 10
  - COLOR_SPACE bit 12 (YUV), BIT_WIDTH bit 14 (set = 8-bit, "not DX")
  - Y_FILTER_CFG bits 16-17, UV_FILTER_CFG bits 24-25, ALPHA_FILTER_CFG bit 30 (version 1.2)
  - BLEND_CFG bit 31
- **Filter values** (`mdss_mdp_hwio.h:378-384`): 0 edge-directed 2D, 1 circular 2D, 2 separable 1D,
  3 **bilinear**.

### 2.2 Fixed-point format

- **Phase step**: unsigned, 21 fractional bits, 24-bit field (U3.21). Computed as
  step = floor(2^21 * src / dst):
  - wahoo: `PHASE_STEP_SHIFT 21` in `mdss_mdp.h:34-36`, `mdss_mdp_calc_phase_step` in
    `mdss_mdp_util.c:1304-1313`, used by `mdss_mdp_overlay.c:638-653`; register mask `PHASE_STEP_BITS
    0xFFFFFF` in `mdss_mdp_hwio.h:367`
  - SDE: `sde_plane.c:44,635-640`
  - mainline: `dpu_plane.c:38,444-447`
- **Init phase**: 6-bit fields (`PHASE_BITS 0x3F`). All kernel-generated configs leave it 0
  (memset in `mdss_mdp_overlay.c:638`; `sde_plane.c:631`).
- **Preload**: H=4, V=3 for QSEED3 (`mdss_mdp_pipe.c:45-46,3143,3154`; `sde_plane.c:63-64,669-670`;
  `dpu_plane.c:53-54,477-478`). The register packs Y_H in bits 0-6, Y_V in 8-14, UV_H in 16-22 and
  UV_V in 24-30 (`mdss_mdp_hwio.h:374-377`).
- **LUTs: none.** The kernel-default QSEED3 configuration in all three drivers is bilinear for Y/RGB with
  `lut_flag = 0` and `blend_cfg = 1`:
  - `mdss_mdp_pipe_calc_qseed3_cfg`, `mdss_mdp_pipe.c:3172-3180`
  - `_sde_plane_setup_scaler3`, `sde_plane.c:680-687`
  - `_dpu_plane_setup_scaler3`, `dpu_plane.c:485-492`

  LUTs are written only when `lut_flag` is set (`mdss_mdp_pp.c:1795-1807`; `sde_hw_sspp.c:636-637`;
  `dpu_hw_util.c:324-329`) and only for filters 0-2. QSEED3 has no nearest-neighbour Y/RGB mode; the
  NEAREST filter in `mdss_mdp_hwio.h:417-420` belongs to QSEED2. Nearest would need a custom separable
  LUT. **[unverified, not needed]**
- **Clock**: upscaling adds no MDP clock requirement. mdss only uses the QSEED3 clock formula when
  scaling down vertically (`mdss_mdp_ctl.c:633-637`).

### 2.3 Values for 1125x2436 BGRX -> 1330x2880 at (55,0)

Computed with `floor(2^21*src/dst)`. The last sample lands inside the source: x 1124.15 < 1125,
y 2435.15 < 2436. Aspect error is -0.004 %. If odd x ever misbehaves, 1332 wide at x=54 (+0.15 %) is
the even alternative. mdss only demands even coordinates for YUV.

| Register | Offset | Value | Note |
|---|---|---|---|
| SRC_SIZE | VIG+0x00 | 0x09840465 | 2436<<16 \| 1125 |
| SRC_IMG_SIZE | VIG+0x04 | 0x09840465 | whole buffer (`mdss_mdp_pipe.c:2049,2063`) |
| SRC_XY | VIG+0x08 | 0 | |
| OUT_SIZE | VIG+0x0C | 0x0b400532 | 2880<<16 \| 1330 |
| OUT_XY | VIG+0x10 | 0x00000037 | x=55, y=0, relative to LM0 |
| SRC0_ADDR | VIG+0x14 | scan buffer PA | below 4 GiB, 64-byte aligned (page aligned here) |
| YSTRIDE0 | VIG+0x24 | 0x00001200 | 4608 (backboardd's bpr, so a frame is one contiguous memcpy) |
| YSTRIDE1 | VIG+0x28 | 0 | |
| SRC_FORMAT | VIG+0x30 | 0x000236ff | BGRX (alpha off), as `pe_serial.c:900` already writes |
| UNPACK | VIG+0x34 | 0x03020001 | B,G,R,X |
| SRC_OP_MODE | VIG+0x38 | ABL value \| BIT(31) | bit 31 = pixel-extension override; mdss sets it whenever the scaler is on (`mdss_mdp_pipe.c:2169-2170`), SDE/DPU always (`sde_hw_sspp.c:44,309`; `dpu_hw_sspp.c:86,305`) |
| DECIMATION_CONFIG | VIG+0xB4 | 0 | |
| SW_PIX_EXT_C0_LR / _TB | VIG+0x100/0x104 | 0 / 0 | QSEED3 repeats internally; repeat/fetch fields stay 0 (`mdss_mdp_overlay.c:564-567`) |
| SW_PIX_EXT_C0_REQ_PIXELS | VIG+0x108 | 0x09840465 | QSEED3: src_h<<16 \| src_w (`mdss_mdp_pipe.c:2863-2866,3145,3156`; `dpu_plane.c:502-510`) |
| C1C2 LR/TB/REQ | VIG+0x110/0x114/0x118 | 0 / 0 / 0x09840465 | the same for RGB |
| C3 LR/TB/REQ | VIG+0x120/0x124/0x128 | 0 / 0 / 0x09840465 | |
| QSEED3 PHASE_INIT | VIG+0xA0C | 0 | version 1.2 layout |
| PHASE_STEP_Y_H | VIG+0xA10 | 0x001b1152 | 1773906 = 0.845864 |
| PHASE_STEP_Y_V | VIG+0xA14 | 0x001b1111 | 1773841 = 0.845833 |
| PHASE_STEP_UV_H | VIG+0xA18 | 0x001b1152 | RGB: same as Y (`dpu_plane.c:450-453`, chroma subsampling 1) |
| PHASE_STEP_UV_V | VIG+0xA1C | 0x001b1111 | |
| PRELOAD | VIG+0xA20 | 0x03040304 | |
| SRC_SIZE_Y_RGB_A | VIG+0xA40 | 0x09840465 | |
| SRC_SIZE_UV | VIG+0xA44 | 0x09840465 | |
| DST_SIZE | VIG+0xA48 | 0x0b400532 | |
| OP_MODE (written last) | VIG+0xA04 | 0x80034001 | EN \| 8-bit (bit 14) \| bilinear (3<<16) \| blend_cfg (bit 31); no alpha bits for BGRX |

These values meet mdss's own consistency check (`__mdss_mdp_validate_qseed3_cfg`,
`mdss_mdp_overlay.c:529-620`): REQ_PIXELS == fetched source == QSEED3 SRC_SIZE, and none exceeds
SRC_IMG_SIZE.

Leave alone: VIG_OP_MODE/CSC (0x200, 0x1A00; RGB needs no CSC), QoS/danger/safe LUTs, CDP and traffic
shaper (ABL's), and COEF_LUT_CTRL. QSEED3 version **[unverified]**: read HW_VERSION (VIG+0xA00) in E0.
If it is not version 1.2, write the init phase to 0x90-0x9C as well (all zero), as mainline does for
versions other than 0x1002 (`dpu_hw_util.c:331-347`).

### 2.4 Flush bits and order

- VIG0 latches on CTL0_FLUSH **bit 0** (mdss `mdp_pipe_hwio[VIG0].flush_bit = 0`,
  `mdss_mdp_ctl.c:45-46,5365-5371`; `sde_hw_ctl.c:102-103`; mainline `dpu_hw_ctl.c:182-183`). The
  QSEED3 block is part of the pipe and is programmed on the same flush. **[per drivers; E3 confirms]**
- Not needed, because neither LM0 nor CTL0 changes: LM0 is bit 6 and CTL is bit 17 (`sde_hw_ctl.c:157-179`;
  `mdss_mdp_ctl.c:5113,5818`). `sweet_fbcon_blank` keeps using bits 6+17 for its LAYER(LM0) change.
- Order, as Linux's command-mode commit does it (`mdss_mdp_display_commit`, `mdss_mdp_ctl.c:5726-6036`):
  1. Wait for the previous frame: `wait4pingpong`, lines 5882-5883.
  2. Write CTL_FLUSH: line 6005.
  3. Kick off (CTL_START): `display_fnc`, line 6036.

  Linux writes pipe registers earlier (`mdss_mdp_pipe_queue_data`), possibly while a frame is still out,
  because they are double-buffered. The kernel can be simpler and write everything after "done", inside
  the existing gate in `fbcon_try_kick` (`pe_serial.c:869-920`). Sequence:
  1. PP0 done (INTR_STATUS bit 8) and DSI0 not busy (`fbcon_frame_done`, `pe_serial.c:836-856`).
  2. DSI recovery check.
  3. INTR_CLEAR bit 8.
  4. Write all VIG0, PE and QSEED3 registers (QSEED3 OP_MODE last).
  5. `dsb sy`.
  6. CTL0_FLUSH = 0x1.
  7. `dsb sy`.
  8. CTL0_START = 1.

  The first switch must write the geometry **and** a SRC0_ADDR pointing at a 1125x2436 buffer before the
  same flush. Never latch the scaled geometry with the old 1440-stride buffer, or the reverse.
- Per frame after that: SRC0_ADDR only, then FLUSH bit 0, then START. That is the existing
  `sweet_fbcon_flip` path (`pe_serial.c:897-913, 977-986`), with stride 0x1200.

## 3. Mixers and the split

- **Expected (single LM0, 1440x2880):** nothing changes in CTL0 or LM0. VIG0 stays at blend stage 3 over
  the border colour (BORDER_OUT bit 24 in LAYER0). The new OUT rect is (55,0) 1330x2880 inside LM0, and
  the 55 columns on each side come out in LM0's border colour (LM+0x08/+0x10; Linux writes 0, i.e. black,
  `mdss_mdp_ctl.c:5124-5127`). The DSC encoder sees a normal 1440x2880 LM output.
- **If E0 shows dual LM** (LM0 OUT_SIZE 720 wide, DSC0 COMMON_MODE 3, LM1 staged) **[not expected]**:
  - This MDP has source split (`live.dts:12736`). With it, one pipe whose destination crosses the left
    LM's width is staged on both LMs at the same stage (`mdss_mdp_layer.c:1271-1285`, no scaling
    restriction there). That means:
    - CTL0 LAYER0 and LAYER1 both carry VIG0;
    - LM1 OP_MODE gets bit 31 (`mdss_mdp_ctl.c:5119-5120`);
    - OUT_XY stays in full-panel coordinates (x=55) for non-base stages (`mdss_mdp_pipe.c:1805-1815,
      1992-2018`).
  - Without source split it would take two pipes (VIG0 left half, VIG1 right half, the right one with a
    non-zero init phase).
  - Recommendation for this case: do not change the topology; stop and re-plan.
- **Alternative not chosen:** the destination scaler after the LM (`qcom,mdss-has-dest-scaler`, block
  0x61000, DS0/DS1 at +0x800/+0x1000, input width at most 2048: `live.dts:12697-12698,12766-12769`). It
  would need LM0 OUT_SIZE changed to about 1218x2436 under a running DSC and PP, plus DS flush bits
  (`mdss_mdp_pp.c:2663`). That is more state to change than a VIG rect.

## 4. Addresses, SMMU, buffers

### 4.1 SRC0_ADDR and the SMMU

- SRC0_ADDR is 32 bits with no high-address register. mdss writes `u32 addr[]` (`mdss_mdp_pipe.c:2253,
  2283`), and even Linux's MDP IOVA space ends below 4 GiB (`mdss_smmu.c:41,697`: 128K up to
  0xF0000000 - 128K).
- The MDP sits behind `arm,smmu-mmss@cd00000` (`live.dts:5348-5368`, `qcom,skip-init`,
  `qcom,no-smr-check`), as unsecure SID 0 and secure SID 1 (`live.dts:12771-12786`).
- After ABL no HLOS context translates MDP traffic. Linux keeps the boot splash on screen by first
  creating a 1:1 IOVA=PA mapping of the splash region and only then attaching the SMMU
  (`mdss_mdp_splash_logo.c:149-190`; "1-to-1 mapping" at 270-271; the comment at 275-282 explains it
  avoids MDP accesses before the IOMMU is attached). The MDP has been fetching physical addresses all
  along. Measured on the phone: kernel writes at PA 0x9d400000 reach the panel (boots 20 and 22-27).
- Not known **[unverified]**:
  - whether this is global bypass (sCR0) or an S2CR bypass entry for SID 0;
  - whether a hypervisor stage-2 limits the MDP to HLOS RAM.
  Nothing in XNU touches this SMMU, so the state persists while XNU runs.
- Consequence: the MDP can only fetch PAs below 4 GiB. XNU's RAM (0x100000000 + 512 MiB,
  `mkadt.py:54`) and every IOSurface are out of reach unless an SMMU context bank for SID 0 is
  programmed (section 8).
- Safest place for scan buffers: inside the splash region, which is known to be fetched. Other free low
  RAM (Linux System RAM 0xb1000000-0xffffffff while XNU runs) is probably fine but untested.

### 4.2 Layout

Splash region 0x9d400000-0x9f800000, 36 MiB (`live.dts:16469-16474`; ABL's FB, status disabled in
Linux). One canvas buffer is 4608 x 2436 = 0xab4800 bytes, rounded to 0xab5000 (10.71 MiB). The old panel
buffer is 1440x2880x4 = 0xfd2000 (15.8 MiB). Panel buffer plus two canvas buffers is 38.5 MB, which does
not fit, so the second scan buffer overlaps the old panel buffer:

| Name | PA range | Use |
|---|---|---|
| panel FB (legacy) | 0x9d400000-0x9e3d2000 | today's scanout; fbcon, panic text, 1:1 fallback |
| SCAN0 | 0x9e400000-0x9eeb5000 | first scan buffer; outside the panel FB and outside the ADT's 16 MiB fb mapping (`mkadt.py:57`) |
| SCAN1 | 0x9d400000-0x9deb5000 | second scan buffer, used from E4 on; overwrites panel FB rows 0-1949 |
| free | 0x9eeb5000-0x9f800000 | 9.3 MiB |

- Map each buffer with `sweet_surface_create_physical(pa, 1125, 2436, 4608, 'BGRA')`
  (`SweetSurface.cpp:238-260`: `withPhysicalAddress` + write-combine mapping, the same route as the
  16 MiB panel surface today, `SweetFramebuffer.cpp:476-477`). That avoids `ml_io_map_wcomb` and the
  small io submap.
- Write-combine means the MDP needs no cache maintenance. Put a `dsb sy` after the copy, before FLUSH
  (already in `fbcon_try_kick`).
- Once SCAN1 is in use, the text console must not draw into the panel FB. Set `sweet_console_muted = 1`
  when the scaled mode starts; ROOT boots already use `sweetfb_noconsole=1`. A panic still draws there;
  optionally the panic path can restore the ABL VIG0 state so the text shows.
- Optional tidy-up: describe the scan buffers in the ADT `sweet-fbcon` node (`mkadt.py:264-272`) instead
  of constants in the kernel.

## 5. Kernel changes (where)

- **`pexpert/arm/pe_serial.c`**
  - `fbcon_probe` (1145-1208): snapshot ABL's VIG0 0x000-0x154 and 0xA00-0xA9C into a static array
    before anything is written. This is the fallback source. The VIG0 page mapped at 1185-1189 already
    covers 0xA00 and 0x100-0x128.
  - New `sweet_fbcon_scale(mode)`. Under `fbcon_kick_lock` and after `fbcon_frame_done`, it writes the
    section 2.3 set (mode on) or the snapshot (mode off) together with a SRC0_ADDR, then FLUSH bit 0 and
    START. Model it on `fbcon_layer_next` (780, 895-907) so the writes happen in the same gated block.
  - Scaled mode: a 50 ms timeout is a failure (section 7), not a reason to START again.
  - `sweet_fbcon_regs` (1057-1073): add CURRENT_SRC0_ADDR (VIG+0xA4), OUT_SIZE, OUT_XY and QSEED3
    OP_MODE.
- **`iokit/Kernel/arm/SweetFramebuffer.cpp`**
  - `start()` (458-472): in scaled mode the canvas is the buffer. fOffX = fOffY = 0, the clip size is
    1125x2436, the row bytes are 4608, and fPixels points at the back buffer.
  - `present()` (929-1099):
    - copy into the back buffer: with mask 1 and the full 1125x2436 rect, one 11 MB memcpy; otherwise
      the existing per-layer loop, 1068-1074;
    - then `sweet_fbcon_flip(back_pa, 4608)` and toggle the back index;
    - drop `sweet_fbcon_wait_idle` (948-952) and the border clears (485-493, 953-961) in this mode.
  - Keep the 1:1 path as it is for fallback.
  - The SHOT "panel" capture (420-423, 543-546) should capture the front scan buffer.
- **`iokit/Kernel/arm/hid/SweetHID.cpp:449-461, 494-498`**: the touch mapping must change in the same
  patch as E3:
  - lx = (x - 55) * 1125 / 1330, ly = y * 2436 / 2880 (clamped);
  - SurfaceWidth/Height become about 63 x 136 mm (panel 68 x 136 mm, `live.dts:13052-13053`);
  - read the mode from a shared flag so fallback restores the old mapping.
- **`iokit/Kernel/arm/SweetPlatformExpert.cpp:124-150, 192-198`**: E0 register census in the existing
  "DISP tag" line format, and the scale state in the TAIMEN FB report.
- **Boot-arg `taimenhwscale`**:
  - 0 = today's 1:1 copy (default until E4 passes)
  - 1 = E2
  - 2 = scaled, single buffer
  - 3 = scaled, double-buffered

## 6. Experiments (each one boot, unattended via `tools/taimen/autoboot.sh`, logs from the reader)

**E0 - read-only census (no behaviour change).**
- What: at the 0 s report (before AppleCLCD's first present), and again at 20 s, log hex dumps of only the
  ranges in the DT's `qcom,regs-dump-mdp` list (`live.dts:12743-12744`). Linux dumps exactly these with
  the MDP clocked. MDSS-relative offsets:
  - VIG0 0x5000-0x5154 and 0x5A00-0x5B00
  - CTL0 0x2000-0x2094
  - LM0 0x45000-0x452BC and LM1 0x46000-0x462BC
  - DSC0 0x81000-0x81140 and DSC1 0x81400-0x81540
  - PP1 0x71800-0x718D4
  - MDP top 0x1000-0x1458
  - also MMCC 0xc8ce004, 0xc8ce008 and 0xc8ce020: the BIMC SMMU AHB/AXI CBCR and GDSC
    (`mmcc-msm8998.c:2456-2487,2618-2627`; `live.dts:580`)

  New pages: 0xc945000, 0xc946000, 0xc981000, 0xc8ce000 (four `ml_io_map` pages).
- Expect **[inferred]**:
  - LM0 OUT_SIZE (0xc945004) 0x0b4005a0
  - LM1 unused
  - DSC0 PICTURE (+0x08) 0x05a00b40 and SLICE (+0x0c) 0x02d00010 (`mdss_mdp_ctl.c:2957-2963`)
  - DSC1 COMMON_MODE 0
  - VIG0 OUT_SIZE 0x0b4005a0
  - QSEED3 OP_MODE 0
  - CURRENT_SRC0 0x9d400000
- Also record: SRC_OP_MODE (is bit 31 set?), the stage-3 blend op (LM0+0xB0/0xB4/0xB8), the border
  colour, and QSEED3 HW_VERSION.
- Go/no-go: single LM (section 3). If dual LM, stop and re-plan.

**E1 - latch test, geometry unchanged.**
- What: call `sweet_fbcon_flip(0x9d400000 + 64*5760, 5760)` for 2 s, then flip back to 0x9d400000.
- Log each time:
  - CTL0 FLUSH right after the write (expect bit 0 set) and after PP0 done (expect bit 0 clear);
  - VIG0 CURRENT_SRC0_ADDR (expect the new address after the START);
  - kicks/done/timeouts and DSI resets.
- Visible: the picture moves up 64 rows, then back.
- This also checks SRC_FORMAT 0x236ff, which was never used on taimen (the audit's cosmetic note,
  `taimen-audit-display-touch-gpu-wifi.md:80-81`).
- Fallback: flip back (it is one register).

**E2 - canvas buffer, no scaler** (`taimenhwscale=1`).
- What: copy frames into SCAN0 (one buffer, `wait_idle` kept). Set VIG0 SRC_SIZE = SRC_IMG_SIZE =
  OUT_SIZE = 0x09840465, OUT_XY = 0x00de009d (157,222), stride 0x1200.
  - If ABL had SRC_OP_MODE bit 31 set, also write the PE REQ_PIXELS to 0x09840465.
  - QSEED3 stays off.
- Visible: the same centred picture as today, but the borders now come from LM0's border colour.
- Proves:
  - the MDP fetches 0x9e400000 (outside the region it has used so far);
  - the new stride works;
  - OUT_XY inside LM0 works;
  - the border fill is clean.
- Pass: 0 timeouts, no new DSI resets, CURRENT_SRC0 = 0x9e400000, photo matches.

**E3 - scaler on** (`taimenhwscale=2`). Optional E3a first: QSEED3 on at unity.
- E3a values: steps 0x200000, DST 0x09840465, OUT 1125x2436 at (157,222). This separates "scaler
  enabled" from "size changed".
- E3 then programs the section 2.3 set, including SweetHID's new mapping.
- Log:
  - QSEED3 OP_MODE/CLK_STATUS readback and VIG0 OUT_SIZE/OUT_XY readback;
  - kicks/done/timeouts and DSI resets/FIFO_STATUS;
  - optionally QSEED3 MISR (enable MISR_CTRL, read SIGNATURE_0/1). The signature should be stable for
    a static frame and change with content. **[unverified register semantics]**
- Visible: full-height picture with 55-px black bars. The user photographs it and taps the corners to
  check touch.

**E4 - double buffering** (`taimenhwscale=3`).
- What: alternate SCAN0 and SCAN1; mute the console at the switch; no `wait_idle` before the copy.
- Log `SWEET PERF` present time and frames/s, and compare with boot 24 (about 24 frames/s by 180 s).
- Pass: no DSI resets during the run and no timeouts.

**E5 - soak.** 25 minutes like boot 24. DSI resets, timeouts and the frame rate should stay flat.

**E6 - optional, read-only, risky: SMMU state** (only needed for Phase 3, section 8).
- Proceed only if E0 showed the BIMC SMMU AHB CBCR (0xc8ce004) with CLK_OFF (bit 31) = 0 and the GDSC on.
- Then read the MMSS SMMU, logging a marker before each read: sCR0 0xcd00000, sIDR0-2 0xcd00020-28,
  SMR0-7 0xcd00800+, S2CR0-7 0xcd00c00+ (standard SMMUv2 GR0 layout **[unverified on this instance]**).
- Risk: an unclocked or hypervisor-trapped access can hang the bus. The APSS WDT no longer bites
  (boot 13+), so this run must be attended.

## 7. What can hang the display or the DSI, and the fallback

| Hazard | Cause | Mitigation |
|---|---|---|
| START while a frame is out | the Pixel 1 and boot 7-8 pathology (`taimen-display-kicks.md:35`); in scaled mode a 50 ms timeout re-kick would do this | keep the PP0-done + DSI-idle gate; in scaled mode a timeout triggers fallback instead of another START |
| Pipe fetch stall | SRC_SIZE / SRC_IMG_SIZE / PE REQ_PIXELS / QSEED3 SRC_SIZE disagree, or PE override set with stale REQ_PIXELS | assert the section 2.3 equalities in code before writing; E2 exercises REQ_PIXELS without the scaler |
| DSI HS FIFO underflow | as in boot 22: the MDP late, or memory contention, now with the CPU copy overlapping the transfer in E4; bilinear needs 2 prefill lines (`live.dts:12747`) **[effect unverified in command mode]** | 0207 recovery and its counters (`pe_serial.c:786-833`); if E4 alone underflows, keep `wait_idle` (copy only between transfers) |
| Bus hang | reading/writing unclocked or non-existent registers; shim 1 died on mainline's MDP-relative INTF/PP offsets used as MDSS offsets (NOTES-taimen.md:76-78) | only DT `regs-dump-mdp` ranges with MDSS-relative offsets; nothing outside the VIG0 page is written |
| MDP fetch fault | an address outside what the MDP can reach | buffers in the splash region; E2 tests SCAN0 while the panel FB is still intact |
| OUT rect outside LM0 | | 55+1330 = 1385 <= 1440, 2880 <= 2880 |
| Scribbles on SCAN1 | console or panic text drawn into the old panel FB | console muted in scaled mode; panic optionally restores the ABL VIG0 state |

Fallback design:
1. **Static:** `taimenhwscale=0` keeps today's path untouched (the copy into the centre of the panel FB
   with START, `SweetFramebuffer.cpp:1068-1092`).
2. **Runtime:** on the first scaled-mode timeout, or more than N DSI resets in a window, log
   `TAIMEN SCALE: fallback` and then:
   1. write back the E0 snapshot of VIG0 (all fields, including SRC_OP_MODE and QSEED3 OP_MODE = ABL's);
   2. clear the panel FB to black once (16.6 MB);
   3. after done, FLUSH bit 0 and START;
   4. switch `present()` and SweetHID back to the 1:1 mapping (offsets -157/-222, `wait_idle`).
3. **If PP0 done never returns** even after the restore: Linux's primitive is CTL_SW_RESET (CTL+0x30,
   `sde_hw_ctl.c:250-267`). It clears CTL state (LAYER0 0x01000005 and TOP 0x00020020 would have to be
   rewritten) and is untested here. Do not automate it: log, stop kicking, and let the PS_HOLD deadline
   and the reader bring the logs back.

## 8. Removing the copy (Phase 3, sketch, all **[unverified]**)

backboardd renders into surfaces that SweetSurface allocates with
`IOBufferMemoryDescriptor::inTaskWithOptions` (`SweetSurface.cpp:759-776`). These are cacheable, not
physically contiguous, and above 4 GiB, so the MDP cannot fetch them. Options:

1. **Low display pool.**
   - What: give backboardd's canvas-sized surfaces (1125x2436 'BGRA', bpr 4608) physical slots in the
     splash region (3 x 0xab5000 = 32.1 MiB, at the cost of the panel FB fallback) and flip SRC0_ADDR
     to the swapped surface.
   - Cache mode problem: XNU maps PAs outside its RAM as `VM_WIMG_IO` unless `pmap-io-ranges` in the
     ADT says otherwise (`osfmk/arm/pmap/pmap.c:10083-10097`). CoreAnimation's CPU renderer on uncached
     memory would be slow, so either declare the range cacheable in the ADT and clean the surface to
     PoC (`dc cvac`, 11 MB) before each flip, or accept write-combine and measure.
   - Lifetime: backboardd alternates two surfaces and draws the next frame at once, so the surface the
     MDP is sending would be drawn into. This needs a third surface or a swap completion that waits for
     PP0 done.
2. **SMMU context bank** for SID 0 mapping IOVA below 4 GiB onto the IOSurface pages above 4 GiB. Needs
   SMMU programming from EL1 under the hypervisor; GPU SMMU writes were trapped on the Pixel 1
   (`taimen-audit-display-touch-gpu-wifi.md:54-56`). E6 first.
3. **GPU copy** (as the Pixel 1 `gpuLayer` path does, `SweetFramebuffer.cpp:737-779`). Needs the A540
   bring-up (audit item 5).

## 9. Open items to settle on the phone

- [ ] Single-LM topology: LM0 OUT_SIZE, DSC0 PICTURE/SLICE, LM1 unused (E0).
- [ ] ABL SRC_OP_MODE bit 31 and PE registers; QSEED3 HW_VERSION (E0).
- [ ] Meaning of CTL0 FLUSH bits 29/30, and whether bit 0 reads back pending until START (E1).
- [ ] MDP fetch from 0x9e400000 (E2).
- [ ] QSEED3 latched by flush bit 0; bilinear output correct; no DSI underflow (E3/E4).
- [ ] CPU time per present before and after (SWEET PERF) (E4).
- [ ] SMMU bypass mechanism (E6, optional).

## References

Wahoo msm-4.4 (Pixel 2 kernel), base
https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-wahoo-4.4-android11/
- mdss fbdev driver (the one wahoo uses; live DT node `qcom,mdss_mdp`, `live.dts:12678`):
  - `drivers/video/fbdev/msm/mdss_mdp_hwio.h`
  - `mdss_mdp_pipe.c`
  - `mdss_mdp_pp.c` (QSEED3 `mdss_mdp_qseed3_setup` 1722-1858, `mdss_mdp_scale_setup` 1860-1888)
  - `mdss_mdp_ctl.c`
  - `mdss_mdp_overlay.c`
  - `mdss_mdp_util.c`
  - `mdss_mdp_layer.c`
  - `mdss_mdp_splash_logo.c`
  - `mdss_smmu.c`
  - `mdss_mdp.h`
  - `include/uapi/linux/msm_mdp_ext.h` (FILTER_BILINEAR 0x3, line 683)
- SDE (DRM) driver for the same chip:
  - `drivers/gpu/drm/msm/sde/sde_hw_sspp.c`
  - `sde_plane.c`
  - `sde_hw_ctl.c`
  - `arch/arm/boot/dts/qcom/msm8998-sde.dtsi`

Mainline v6.12, base https://github.com/torvalds/linux/blob/v6.12/
- `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_util.c` (`dpu_hw_setup_scaler3` 282-382)
- `dpu_hw_catalog.c`
- `catalog/dpu_3_0_msm8998.h`
- `dpu_plane.c`
- `dpu_hw_sspp.c`
- `dpu_hw_ctl.c`
- `drivers/clk/qcom/mmcc-msm8998.c`

This repository:
- `research/taimen/live.dts` (mdss_mdp 12678-12811, panel 12990-13072, MMSS SMMU 5348-5368,
  reserved-memory 16376-16512)
- `research/taimen/shim-diag-ufs.txt`
- `port/NOTES-taimen.md` (boots 19-28)
- `port/docs/taimen-display-kicks.md`
- `port/docs/taimen-audit-display-touch-gpu-wifi.md`
- `scratch/xnu/pexpert/arm/pe_serial.c`
- `scratch/xnu/iokit/Kernel/arm/SweetFramebuffer.cpp`
- `scratch/xnu/iokit/Kernel/arm/SweetSurface.cpp`
- `scratch/xnu/iokit/Kernel/arm/hid/SweetHID.cpp`
- `port/tools/mkadt.py`
