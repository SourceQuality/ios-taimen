# iOS 16 on the Pixel 2 XL (taimen, MSM8998) - working notes

Base: joranv1/ios-sailfish (Pixel 1, Snapdragon 821) in this repo (fork `SourceQuality/ios-taimen`), with the Pixel 4
port (NWylynko/ios-flame) as the ABL / SM8150-era reference.

## The phone (2026-10-22)

- Pixel 2 XL `taimen`, hw-variant GA00125-US, hw-revision rev_10, sku G011C, 4 GB DDR (Samsung), 64 GB UFS (Samsung).
- Bootloader TMZ30m, `unlocked:yes`, `is_oem_unlocked:yes`, `secure:yes`, `kernel:uefi` (Google ABL).
  `max-download-size:0x20000000` (512 MiB). Slots a/b, both bootable, current slot a.
- Baseband g8998-00034-2006052136. Runs LineageOS (Android 12, fingerprint RP1A.201005.004.A1).
- Partition backups (22, sha256-verified on device) in `../backup/` (never committed); sizes in `../backup/partition-sizes.txt`.
  boot: 40 MiB (0x2800000); dtbo 8 MiB; persist 32 MiB; userdata ~53 GB (`sda13`).
- Block devices: boot_a `sde8`, boot_b `sde22`, dtbo_a `sde13`, vbmeta_a `sde12`, persist `sda4`,
  modemst1/2 `sdf1`/`sdf2`, fsc `sdf3`, fsg `sde32`, xbl_a `sdb1`, abl_a `sde6`. UFS host at `1da4000.ufshc`.

## Boot image rules (measured from TWRP 3.7.0 and LineageOS boot_a, both `fastboot boot` fine)

- Header v0, page 4096, kernel load 0x8000, ramdisk 0x1000000, tags 0x100.
- Kernel compressed: TWRP gzip (1f 8b), Lineage LZ4 frame (04 22 4d 18). DTB appended to the kernel
  (one DTB, `d00dfeed`, ~376-392 KB). cmdline carries `androidboot.dtbo_idx=12`: ABL applies dtbo entry 12.
- AVB hash footer present on both (image padded to 41943040 = 40 MiB). Not yet proven required for `fastboot boot`;
  we add it anyway (`avbtool add_hash_footer --partition_size 41943040 --partition_name boot`).
- Packing: `port/tools/mkboot-v0.sh` (gzip Image + stock DTB kept from the template) then avbtool.
  `tools/granule-probe-taimen.sh` does this; `PROBE_SRC=` picks the probe source.

## Memory map (live FDT as patched by ABL, `../research/taimen/live.dts`)

- /memory reg = 0x80000000 + 0xFE4C0000 (one range, ~4 GB). Linux `System RAM` ranges:
  80000000-857fffff, 88f00000-8aafffff, 95b00000-afffefff, b0200000-b0dfffff, b1000000-17e4bffff.
- Splash framebuffer: `/reserved-memory/splash_region@9d600000`, but its `reg` is **0x9d400000** + 0x2400000 (36 MiB; the node name is misleading), label
  `cont_splash_mem`, status disabled in the Lineage DTB (so the kernel does not reserve it; ABL still draws there).
- Ramdisk placed by ABL at 0xa0000000.

## Display

- Panel `qcom,mdss_dsi_sw43402_dsc_qhd_cmd` (LG SW43402, QHD 1440x2880, command mode, DSC), single DSI,
  MDP at 0xc900000, DSI ctrl0 0xc994000, ctrl1 0xc996000, DSI PLL `qcom,mdss_dsi_pll_8998`.
  Continuous splash enabled by ABL.

## Firmware (pulled from /vendor/firmware, in `../research/taimen/firmware/`, NOT for git)

- GPU Adreno 540: `a530_pm4.fw`, `a530_pfp.fw` (A540 uses the A530 microcode), `a540_gpmu.fw2`, `a540_zap.{mdt,b00-b02,elf}`.
- Wi-Fi is the integrated WCN3990 (`wlanmdsp.mbn` loaded via the modem, `bdwlan.bin` board data): the flame/topaz
  Wi-Fi path, NOT sailfish's QCA6174-over-PCIe driver.
- Video: venus.mbn.

## Log

- 2026-10-22 probe 1 (`out/granule-probe-taimen.img`, PSCI-only answer): fastboot "Booting OKAY", phone left
  fastboot ~12 s later, no USB re-enumeration for 80 s. Either SYSTEM_OFF (= no 16K granule) or a hang.
  Screen state not observed. Built probe 2 (`tools/granule-probe-fb.S`) that paints the answer on the splash
  framebuffer and resets after 10 s.
- 2026-10-22 probe 2 (`out/granule-probe-fb.img`, paints bands on the ABL splash FB at 0x9d600000, waits 10 s,
  PSCI SYSTEM_RESET): fastboot OKAY 21:39:54, phone left fastboot at +11 s (ABL lingers ~11 s after "Booting OKAY"
  on every tethered boot so far), Android back on USB at +40 s, `ro.boot.bootreason=reboot`. So the probe ran
  to the end and the PSCI reset works. By the same path probe 1 ran too, and its SYSTEM_OFF answer means
  **ID_AA64MMFR0_EL1.TGran16 = 0: no 16K granule on the Kryo 280** (CPU implementer 0x51, parts 0x800/0x801:
  Qualcomm semi-custom A73/A53-class cores; A53/A57-era ID_AA64MMFR0 is 0x1122 = 4K and 64K only, while the
  sweet notes' 0x101122 on A55/A76 is the 16K-capable value). Screen bands to be confirmed by eye
  (black band + grey = unsupported).
  Consequence: XNU must be built with 4K pages like the Exynos ports (Floppikrob/ios-galaxy-s8, A5XELTE config),
  not the 16K VMAPPLE-derived config that sailfish/sweet use. Qualcomm platform drivers come from sailfish,
  the page-size work from the S8 port. ABL / Pixel boot-image rules confirmed working for arbitrary kernels.
- Phone kernel: 4.4.302-lineage (msm-4.4), so no userspace ID-register emulation; the probe is the only way to read TGran16.
- 2026-10-22 shim 1 (`shim/taimen.c`, stage-1 diag: disable APSS WDT at 0x17817000, read MDSS/MDP/SSPP/CTL/INTF/PP
  registers at 0xc9xxxxx, paint FB at 0x9d400000, CTL0 START kick, 20 s, PSCI reset): phone came back at +40 s with
  `bootreason=watchdog`, i.e. a bite ~9 s after entry; ramoops console empty.
- 2026-10-22 shim-min (`-DDIAG_MIN`: no WDT writes, no MDSS access, no CTL kick; paint + 10 s + PSCI reset):
  `bootreason=reboot` at +44 s, exactly as written. So the entry, stack, FB writes and timer are fine and the
  watchdog death is caused by the WDT write or the MDSS register access (bisect next). Ramoops console empty again:
  **RAM at 0xb0080000 does not survive a PSCI SYSTEM_RESET through this bootloader** (or Linux reads another region).
  Ramoops is not a log channel on taimen; use the screen now and the UFS log ring later.
- Linux dmesg: `mdss version = 0x30000001`, `fb0: split_mode:1 left:720 right:720` (two LM/PP/DSC, one DSI),
  `bootloader display is on`, DSI ctrl rev 0x20000000, PHY rev 0x3.
- 2026-10-22 staged diag (`shim/taimen.c`, correct MDSS offsets from the DTS: ctl 0x2000, vig 0x5000, lm 0x45000,
  intf 0x6b000+, pp 0x71000+, dsc 0x81000): ran to the end, `bootreason=reboot`. So the MDSS reads, the WDT
  write and the CTL0 START kick are all fine; shim 1 died on the two wrong offsets (INTF 0x6a800 / PP 0x70000).
- 2026-10-22 **UFS log channel works.** Pixel 4 shim's UFSHCI driver ported to 0x1da4000 (`shim/ufs_taimen.c`);
  log written to `align_parti` (sda12, 6 MiB all-zero GPT pad, backed up to `../backup/align_parti.img`), read back
  with `tools/taimen/log-pull.sh` from rooted Android. ABL leaves the host enabled: ver=0x210 (UFSHCI 2.1),
  cap=0x1587001f, hce=1, hcs=0x10f, its UTRL at 0x9bc39000. TUR ok, GPT parsed, partition first=0x14aa08
  blocks=0x5f8 (4K units), WRITE(10)+SYNC ok. Replaces ramoops.
- Entry facts: image at 0x80080000 (`_end`=0x80118000 for a 0x98000 image), DTB passed in x0 at 0x85300000,
  EL1 (`CurrentEL`=4), SCTLR_EL1=0xc00800 (MMU off), MIDR=0x51af8014 (Qualcomm Kryo 280 silver, part 0x801),
  CNTFRQ 19.2 MHz. **ID_AA64MMFR0_EL1 = 0x1122: TGran16 = 0 (no 16K), TGran64 = 0 (64K yes), TGran4 = 0 (4K yes).**
  Definitive; 4K pages it is.
