# Audit: display, touch, GPU, Wi-Fi drivers vs taimen hardware (2026-10-23)

Read-only review of the drivers backboardd/SpringBoard will need, against the Linux device tree
(`research/taimen/live.dts`) and the shim's MDP register dump. Build note: `MakeInc.def` defines
both `ARM64_BOARD_CONFIG_SWEET` and `ARM64_BOARD_CONFIG_TAIMEN`, so every sweet/sailfish driver
compiles into the taimen kernel and is held off only by boot-args.

## Ranked (what blocks SpringBoard first)

1. **BLOCKER: the 32-bpp AppleCLCD present path never kicks a frame.** `SweetFramebuffer.cpp:362`
   enters `startScaled()` only for `v_depth == 24`; taimen hands 32 bpp, so `fScaled` is false.
   `present()` (:947-957) kicks only via the GPU flip or `if (fScaled) sweet_fbcon_kick(true)`:
   neither runs, the frame is memcpy'd into the splash memory and no CTL0 START is written. The
   panel is command mode with autorefresh off, so the screen freezes once `sweetfb_noconsole=1`
   silences the console's newline kicks. The GPU present is also gated on `fScaled`. Fix: after
   the copy loop, `else sweet_fbcon_kick(true)` whenever a kickoff register exists (the kick
   already waits for PP0 done).
2. **Display geometry/identity mismatch on the 32-bpp path.** Logical 1125x2436 is applied only
   when the panel is at most that size (:375); taimen 1440x2880 fails, so `GetDisplaySize`
   returns 1440x2880, dot pitch 458, while mkadt publishes a 1125x2436 canvas with scale 3 and an
   iPhone X identity. CoreAnimation surfaces of one size, panel copy of another, no scaling. The
   scaled path is 24-bpp/Pixel-1-only (750x1334 shadow, 3-byte pixels). Choice: (a) a 32-bpp
   scaled path: 1125x2436 BGRA shadow scaled 1.28x into the 1440x2880 panel, plus kick (what
   Pixel 1 ships); or (b) `sweetfb_native=1` with a 1440x2880 canvas and accept UIKit layout
   problems. Coupled: `SweetHID.cpp:359-360` touch panel 1080x1920 and the clamp at :446-447;
   FTM4 reports 0..1439 x 0..2879 (`stm,max_coords`); SurfaceWidth/Height 62x111 mm vs 68x136 mm.
3. **Touch: protocol is Pixel 1 (Synaptics RMI4), bus driver reusable.** `sweet_touch.c:40-50`:
   QUP 0x7577000, GCC votes 0x352004/0x31d008, TLMM 0x1010000, SDA 47/SCL 48, reset 89, attn 125,
   addr 0x20. Taimen: `i2c@c179000` (reg 0xC179000+0x600, irq SPI 99, 400 kHz), TLMM 0x3400000,
   SDA 87/SCL 88, reset 89, irq GPIO 125 = direct SPI 216 (INTID 248), switch GPIO 75, addr 0x49,
   rails pm8998 L6/L28, GCC base 0x100000 (blsp1 AHB vote and qup5 clock offsets unmeasured).
   Reusable: the generic QUP master (FIFO mode, v1 tags; both SoCs are `qcom,i2c-msm-v2`; verify
   with the QUP_HW_VERSION readback). Rewrite: `rmi_init`/`touch_poll` (:312-500) into the ST FTS
   "ftm4" event-FIFO protocol (reference: the taimen msm-4.4 ftm4 driver). The RPM rail path
   (`sweet_rpm.c` msg RAM 0x68000, IPC 0x9820000) is MSM8996; taimen 0x778000 / 0x17911008.
4. **Buttons / SPMI** (power key wakes the display; not needed for SpringBoard to appear).
   `sweet_keys.c:31-41` SPMI core 0x400f000/obsrvr 0x4c00000/cnfg 0x400a000/chnls 0x4400000,
   TLMM 0x1010000, vol-up GPIO 126, vol-down GPIO 121 reported as Home. Taimen: arbiter core
   0x800F000, chnls 0x8400000, obsrvr 0x9400000, intr 0xA400000, cnfg 0x800A000; power key PON
   @0x800 SID 0; vol-up GPIO 126; vol-down is PON RESIN (RT_STS 0x810 bit 1), not a GPIO. Fix:
   read the arbiter version and take the APID-map/observer offsets for that revision from Linux
   spmi-pmic-arb.c; drop the vol-down→Home mapping. `sweet_spmi.c` (battery) uses the Redmi
   SM7150 arbiter addresses; dead on taimen (battery forced synthetic).
5. **GPU** (nice-to-have; `sweetgpu` defaults to 0, SpringBoard rides the CPU present path).
   Everything below the CP is MSM8996: MMCC 0x8c0000, GCC 0x300000, GPU 0xb00000, MEMACC,
   PMI8994 S2 rail, ZAP_PA 0x90f00000, SMMU 0xb40000. Taimen: GPU 0x5000000+0x40000, IRQ SPI 300,
   chip id 0x5040001, GPUCC `qcom,gpucc-8998-v2` 0x5065000+0x9000, GCC 0x100000, GFX rail PM8005
   S1 (SID 5, 0x1400) with CPR, GPU SMMU 0x5040000+0x10000 (TZ-owned, skip-init), SIDs 0/2 as
   Pixel 1. The power-up sequence (`gpu_thread`, :2115-2237) and `gfx_clock/gfx_raise` must be
   rewritten. Zap path is portable: SCM PAS id 13 matches `qcom,kgsl-hyp` pas-id 0xD; a540_zap
   has exactly one PT_LOAD (filesz 0x1334 = a540_zap.b02); change ZAP_PA to the pil_ipa_gpu
   region 0x95A00000+0x100000. Chip id 0x05030002 hard-coded at :1710 (exported to Mesa). GMEM
   1 MiB right for A540; A540 GPMU init (a540_gpmu.fw2, version 3.0) is absent (drm/msm
   a5xx_power.c is the reference). a530_pm4/pfp microcode is shared. GPU buffers above 4 GiB are
   fine (36-bit SMMU, lo/hi CP packets); the 8996 SMMU writes were already hyp-trapped, expect the
   same.
6. **Wi-Fi / USB / battery / audio / sensors: none blocks SpringBoard launching.**
   `sailfish_wlan.c` is PCIe/QCA6174 throughout (clocks, PARF/PHY/DBI, BAR 0xc400000, DMA pool
   0xa0000000 which collides with ABL's ramdisk address); taimen Wi-Fi is WCN3990 via
   `icnss@18800000` (membase 0x18800000+0x800000, SMMU IOVA 0xa0000000) with the firmware on the
   modem DSP, so it needs the modem subsystem up (PIL via TZ) plus the wlfw QMI control path; only
   the CE/HTC/WMI-TLV/HTT layers carry over. `sweet_usb.c` targets the Redmi (DWC3 0xa600000);
   taimen ssusb@a800000 / qusb@c012000. PMI8994 fuel-gauge registers vs PMI8998 fg-gen3/smb2:
   different; already disabled.
7. **32-bit physical-address truncation: mostly clean.** `pe_serial.c:820` writes
   `(uint32_t)src` into SSPP_SRC0_ADDR: the register is 32-bit in hardware (MDP5 3.0 has no high
   address register; above 4 GiB only through the SMMU), so the risk is who feeds it: today only
   the fixed Pixel 1 PAs 0xc0800000/0xc1000000, which on taimen sit in Linux system RAM outside
   XNU's window and work by accident. Add a `pa < 4 GiB` guard. The MDSS has `iommus` on the mmss
   SMMU (skip-init, no-smr-check) and the bootloader's VIG0 fetches 0x9d400000 directly, so SID 0
   passes sub-4 GiB addresses at handoff. Options: (a) give the MDP only kernel-owned buffers
   below 4 GiB (reserve a low window in the ADT memory map inside 0xb1000000-0xffffffff, clear of
   ramoops and crash_fb), recommended; or (b) program an mmss-SMMU context bank for SID 0 the way
   `sweet_gpu.c smmu_identity` does. IOSurfaces themselves may live above 4 GiB (CPU and GPU
   only). Clean: UFS UTRD/PRDT lo/hi, USB TRB/event lo/hi, GPU, SweetGPU getPhysicalSegment.
8. **Display items that are clean or cosmetic.** All MDP addresses derive from the ADT kickoff
   register (CTL0 0xc902000, VIG0 0xc905000, INTR 0xc901014/18, DSI0 0xc994000) and match the
   DTS and the shim dump; no MSM8996 offsets survive. The kernel never touches PP0, DSC0/1, INTF1,
   LM0 OUT_SIZE, CTL0 TOP or DSI timing: a flip must keep the buffer exactly 1440x2880 stride 5760
   and never change SRC_SIZE/SRC_XY. Cosmetic: the flip writes SRC_FORMAT 0x236ff where ABL left
   0x237ff. Backlight: DCS 0x51 through the DSI command DMA; register offsets are the DSI-6G
   family, unverified on taimen's ctrl rev 0x20000000; SWEET_BL_MAX 255 vs panel 225; first DCS
   write happens only when CoreBrightness sets brightness, so `sweetbl=0` isolates it. No rotation
   anywhere; portrait-native panel. Default surface = splash memory below 4 GiB; 16 MiB mapping
   covers 1440x2880x4.

## Suggested order

Fix 1 (one kick); decide 2 (32-bpp scaled path vs native) and move the HID constants with it;
then touch (3), buttons (4); GPU (5) and Wi-Fi/USB (6) after SpringBoard is on screen.
