# iOS 16 on the Pixel 2 XL (taimen, MSM8998) - working notes

Base: joranv1/ios-sailfish (Pixel 1, Snapdragon 821) in this repo (fork `SourceQuality/ios-taimen`), with the Pixel 4
port (NWylynko/ios-flame) as the ABL / SM8150-era reference.

## The phone (2026-10-22)

- Pixel 2 XL `taimen`, hw-variant GA00125-US, hw-revision rev_10, sku G011C, 4 GB DDR (Samsung), 64 GB UFS (Samsung).
- Bootloader TMZ30m, `unlocked:yes`, `is_oem_unlocked:yes`, `secure:yes`, `kernel:uefi` (Google ABL).
  `max-download-size:0x20000000` (512 MiB). Slots a/b, both bootable, current slot a.
- Baseband g8998-00034-2006052136. Runs LineageOS (Android 12, fingerprint RP1A.201005.004.A1).
- Partition backups (22, sha256-verified on device) in `../backup/` (never committed); sizes in `backup/partition-sizes.txt`.
  boot: 40 MiB (0x2800000); dtbo 8 MiB; persist 32 MiB; userdata ~53 GB (`sda13`).
- Block devices: boot_a `sde8`, boot_b `sde22`, dtbo_a `sde13`, vbmeta_a `sde12`, persist `sda4`,
  modemst1/2 `sdf1`/`sdf2`, fsc `sdf3`, fsg `sde32`, xbl_a `sdb1`, abl_a `sde6`. UFS host at `1da4000.ufshc`.

## Boot image rules (measured from TWRP 3.7.0 and LineageOS boot_a, both `fastboot boot` fine)

- Header v0, page 4096, kernel load 0x8000, ramdisk 0x1000000, tags 0x100.
- Kernel compressed: TWRP gzip (1f 8b), Lineage LZ4 frame (04 22 4d 18). DTB appended to the kernel
  (one DTB, `d00dfeed`, ~376-392 KB). cmdline carries `androidboot.dtbo_idx=12`: ABL applies dtbo entry 12.
- AVB hash footer present on both (image padded to 41943040 = 40 MiB). Not yet proven required for `fastboot boot`;
  we add it anyway (`avbtool add_hash_footer --partition_size 41943040 --partition_name boot`).
- Packing: `tools/mkboot-v0.sh` (gzip Image + stock DTB kept from the template) then avbtool.
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
  log written to `align_parti` (sda12, 6 MiB all-zero GPT pad, backed up to `backup/align_parti.img`), read back
  with `tools/taimen/log-pull.sh` from rooted Android. ABL leaves the host enabled: ver=0x210 (UFSHCI 2.1),
  cap=0x1587001f, hce=1, hcs=0x10f, its UTRL at 0x9bc39000. TUR ok, GPT parsed, partition first=0x14aa08
  blocks=0x5f8 (4K units), WRITE(10)+SYNC ok. Replaces ramoops.
- Entry facts: image at 0x80080000 (`_end`=0x80118000 for a 0x98000 image), DTB passed in x0 at 0x85300000,
  EL1 (`CurrentEL`=4), SCTLR_EL1=0xc00800 (MMU off), MIDR=0x51af8014 (Qualcomm Kryo 280 silver, part 0x801),
  CNTFRQ 19.2 MHz. **ID_AA64MMFR0_EL1 = 0x1122: TGran16 = 0 (no 16K), TGran64 = 0 (64K yes), TGran4 = 0 (4K yes).**
  Definitive; 4K pages it is.
- 2026-10-22 **FIRST XNU BOOT ON TAIMEN.** TAIMEN kernel (CI run 39892141965, patches 0184+0185, 4K pages, link
  0xfffffff007004000), payload via the boot-image ramdisk, STAGE2 shim, 512 MiB RAM at 0x100000000, `cpus=1`.
  Rescued early log (sweet_early PRZ at 0xb0080000): stage 0 (MMU off) .. 20 "kprintf startup complete" all passed:
  arm_init, PE_init_platform, cpu topology, cpu_bootstrap, kernel_startup_bootstrap, cpu_init, arm_vm_init (bootstrap
  TTBR0 left, log alias live, TLB flushed, physmap rebound, pmap_bootstrap, early I/O page tables), serial_init with the
  fbcon mapped. Then `panic: pmap_map_bd @pmap.c:1885` from initialize_screen -> io_map -> pmap_map_bd (pmap_pte ==
  NULL: no L3 table for the IO-region VA). Watchdog reset ~37 s later (bootreason=watchdog).
- **Kernel log channel found:** Linux copies ramoops main (0xb0000000) -> alt (0xb0e00000) at every boot and Google's
  access_ramoops driver exposes the alt region as `/dev/access-ramoops` (console zone at +0x80000). So XNU's PRZ
  survives watchdog reset + Android boot and is read with `tools/taimen/xnu-log-pull.sh`. Also seen by the shim at
  0xb0e80000. RAM survives resets on this phone; only Linux re-initialising the main zone hid it before.
- IMEM restart_reason confirmed at 0x146bf65c (imem@146bf000 + 0x65c); writing 0x77665500 + PSCI reset still booted
  Android, so ABL must take the reason from the PMIC PON (PM8998 SOFT_RB_SPARE) instead. Not needed now.
- 2026-10-22 boot 2 (kernel r3 = 0184-0186, ADT fb_size 16 MiB): past initialize_screen; pmap_startup, vm_page_bootstrap
  (122642 free pages), VM maps, zone_init (zones 24G, compressor 8G, kmem ranges) all fine at 512 MiB, "Firehose
  configured", then **panic: Undefined kernel instruction pc=0xfffffff007274a0c instr=f8280128** =
  `ldadd x8, x8, [x9]` (LSE, ARMv8.1) in `__firehose_buffer_tracepoint_flush` (libfirehose_kernel.a built by CI with
  Xcode's default CPU). Fix: build libfirehose arm64-only with -mcpu=kryo (as tools/xnu-build-local.sh), fakeroot
  cache rekeyed. The watchdog reset came ~37 s after the panic.
- Log channel, corrected: after the Android boot that follows an XNU run, `/sys/fs/pstore/console-ramoops` holds
  XNU's PRZ text followed by Linux's console (548 KB this time). `/dev/access-ramoops` (alt region) is encrypted by
  the vendor `ramoops_sh` service after boot, so it is unreliable. `tools/taimen/xnu-log-pull.sh` now reads pstore.
- 2026-10-22 iOS root inputs ready: IPSW iPhone10,3 16.7.16 (sha1 verified) in `research/ipsw-x/`, the three images
  extracted to `research/ipsw-x/ipsw/` (OS root 087-86698-044.dmg 4.7 GB, SystemOS cryptex 087-86593-047.dmg 2.7 GB,
  AppOS 087-86715-047.dmg). `apfs-fuse` built from source into `~/.local/bin` (zlib/bz2/fuse3 headers pulled as RPMs
  into a scratch sysroot, no root needed); an unprivileged `apfs-fuse -o ro` mount of the OS image works and shows
  owners (mobile 501:501), symlinks and decompressed files (LZBITMAP). 7-Zip 26 also reads the APFS but skips
  LZBITMAP resource-fork files (6 in SystemOS incl. dyld linkedit subcaches) and refuses `..` symlinks: not usable.
  Building the HFS+ root image (`tools/mkiosroot.sh OS SYSOS APPOS out.img 16G`) still needs sudo for the loop
  mount and the root rsync.
- 2026-10-22 boot 3 (kernel r4: 0184-0186 + libfirehose built with -mcpu=kryo): **no panic**. Firehose, OSLog,
  log queues, timeslicing, mig, kdp, "iBoot version: sweet-shim", SWEET AMFI, vnguard, platform expert, AKS, WDT
  (IOWatchdog), IOCoreSurfaceRoot, HID buttons/touch, static battery, `SWEET FB: AppleCLCD 1440x2880 stride 5760 at
  0x9d400000`, SweetGPU registered; then the log ends (5.8 KB) and the phone reset at the usual +67 s with
  bootreason=watchdog. The APSS WDT BARK/BITE registers are 20 bits (defaults 0x7ffff / 0xfffff = 16 / 32 s), so the
  shim's "120 s" (0x3c0000) truncated to 0xc0000 = 24 s after the arm: the kernel must pet it (or the shim must arm
  the 32 s maximum and the kernel pet it). `debug_log_init: gPanicBase not initialized` x2 is cosmetic.
- 2026-10-23 **iOS root image built** (user granted passwordless sudo): `tools/mkiosroot.sh` with apfs-fuse, OS root +
  SystemOS + AppOS cryptexes into a 16 GiB journaled HFS+ (`research/ipsw-x/ios16-root.img`, 12 GB used), 19 cryptex
  hard-link fixups, `fsck.hfsplus -y` once (Linux leaves the unmounted bit), sparse copy `ios16-root.simg` (~12 GB).
  Install plan: `tools/taimen/install-root.sh ROOT.simg TWRP.img` = TWRP into boot_a (so resets never boot Android,
  which would reformat the HFS+ userdata) + `fastboot flash userdata`. Not run yet: the kernel must schedule threads
  first (boot 4: pet thread created but never ran; no "SWEET IRQ" lines; timer interrupt suspected).
- 2026-10-23 boot 4 (kernel r5 = +0187 gates, +0188 wdt pet, `rd=ufsroot0 taimenreboot=90`): "TAIMEN WDT: petting
  every 5 s" logged at 19.974 s, then the usual IOKit registrations to "SweetGPU registered" at 19.997 s, then nothing;
  watchdog reset at 31 s (the shim's arm). No "SWEET UFS" lines, so bsd_init / IOFindBSDRoot was never reached.
  Build 6 (pet logging) was cancelled by the push of 0189 (GIC state dump into the early log); build 7 has both.
- 2026-10-23 boot 5 (kernel r7: +0188 pet logging +0189) and boot 6 (r8: GIC dump via printf), both `rd=ufsroot0
  taimenreboot=90`: **threads do run** ("TAIMEN WDT: pet thread running", "pet at 0 s" at 20.0 s) but the thread never
  wakes from its first IOSleep, nothing else progresses, watchdog at 31 s. No "SWEET IRQ" lines ever. GIC state right
  after pe_init_fiq (runs from PE_init_cpu at arm_init.c:623, after mark 20, so the sweet_early_hex lines are dropped):
  `vtimer intid 19 gicr_pe_base 0xfffffff0369e6000 GICD_CTLR 0x12 GICD_TYPER 0x7b04f3 GICR_WAKER 0x0 GICR_TYPER 0x1
  ISENABLER0 0x0 IGROUPR0 0x0 | ICC_SRE 0x1 IGRPEN1 0x1 PMR 0xf0 CTLR 0x440 CNTV_CTL 0x0 CNTFRQ 0x124f800 DAIF 0x3c0`.
  **GICR_ISENABLER0 reads 0 right after the kernel wrote (1<<19)|(1<<1), IGROUPR0 reads 0 after 0xFFFFFFFF**: the
  redistributor ignores the non-secure enable for PPI 19 (and IGROUPR0 is RAZ/WI from NS with DS=0). Hypotheses:
  wrong redistributor frame for this CPU, or the PPI not assigned to NS Group 1 by TZ for the frame we touch. Linux on the
  same phone uses hwirq 19 for arch_timer, so the hardware can deliver it. Next: per-frame GICR_TYPER affinity dump,
  readback after each write, GICR_IIDR, ISPENDR0/ISACTIVER0, and compare with /proc/interrupts on Android.
- Screen (user's photos, 2026-10-23): the panel keeps showing the shim's stage-1 text; XNU's stage-0 band (row 2600)
  and the sweet_early stripes (rows 2000+) never appear; a ~100-row band of colour noise appears at the very top.
  The kernel's frame kicks or FB writes do not reach the panel: the on-screen console is not usable yet.
- The scratch xnu tree used to generate patches was lost with a session restart; regenerate with
  `git clone --depth 1 --branch xnu-8796.141.3 apple-oss-distributions/xnu` + apply port/xnu/patches in sorted order.
- 2026-10-23 boots 7-8 (kernels r10/r11, 0190 diagnostics): GICR window scan from inside XNU: RD frames at +0x00000,
  +0x20000, ... +0xe0000 with GICR_IIDR 0x1143b (Arm GIC-500) and TYPER affinities 0,1,2,3,0x100..0x103 (frame 7 has
  Last); CPU0 = frame 0 as assumed. But every SGI/PPI register (ISENABLER0, IPRIORITYR, ICFGR1, ISPENDR0) reads 0 even
  right after XNU writes them, and **GICD_CTLR reads 0x0 at handoff** (sweet/sailfish/flame bootloaders leave ARE_NS
  set; the sweet code relies on that and only sets ARE_NS|EnableGrp1A after the redistributor writes). With ARE_NS
  clear the redistributor SGI/PPI registers are RAZ/WI from Non-secure state -> the vtimer PPI 19 was never enabled ->
  no timer interrupt -> the scheduler never woke anything (threads ran once, IOSleep never returned). Fix 0191: set
  GICD_CTLR.ARE_NS (RWP wait) before the redistributor writes, TAIMEN only. Linux's gic_dist_init does the same order.
- 2026-10-23 root image verified (agent, read-only mount): launchd/dyld arm64 Mach-O, 44 dyld cache files hard-linked
  from the SystemOS cryptex (no 0-byte files), cryptex symlinks relative, owners (mobile 501, uuidtext/diagnostics 272,
  keybag 0600), SystemVersion 16.7.16/20H392, 181767 files / 137195 dirs / 2328 symlinks, 11.67 GiB used; every path of
  the IPSW listing present (22 renamed by fsck Unicode normalisation; 2 Thai-named shortcuts unresolvable under Linux
  hfsplus only). `fsck.hfsplus -n` keeps saying "minor repair" after a clean -y; Linux mounts it without complaint.
- 2026-10-23 boot 9 (r12 = 0191 ARE_NS first): `GICD_CTLR now 0x10`, then **ISENABLER0 0x80002, IPRIO 0xa0a0a0a0**
  (the PPI enable finally sticks), ISPENDR0 0x40200000 (PPIs 21 and 30 already pending from firmware). The log ends
  right there, as in boots 7-8 (first boots with 0190): the dump reads ICC_HPPIR0_EL1 (Group 0 = TrustZone; traps to
  EL3 when SCR_EL3.FIQ is set) and CNTP_CTL_EL0 (physical timer, trapped by the hypervisor) -> immediate reset
  (+43 s cycle, "watchdog"). 0192 drops both reads.
- 2026-10-23 **boot 10 (r13 = 0191 ARE_NS first + 0192): TIMER INTERRUPTS WORK.** "TAIMEN WDT: pet at 0 s .. 115 s"
  every 5 s, `taimenreboot=120 reached, no more pets: reset follows`, phone back at +195 s by the watchdog exactly as
  designed. Scheduler, IOSleep and the vtimer are all functional on the 4K kernel. Root cause of boots 3-9: GICD_CTLR
  ARE_NS clear at handoff on taimen (0191). After the dump: ICC_IGRPEN1 reads 0 (enabled later), HPPIR1 0x3ff.
- 2026-10-23 display: the agent's analysis (saved in docs/taimen-display-kicks.md) shows all three XNU paint paths
  write the right addresses; the fault is cadence: ~20 bare CTL0 STARTs ~1 ms apart during the first command-mode
  frame desync the DSC panel (noise band at the top, picture frozen). 0193: early kicks wait for PP0 done, clear it,
  then START (the shim's sequence); start.s no longer STARTs at all.
- 2026-10-23 boot 11 (r16 = 0193 kick cadence + 0194 thread dump): 20 kernel threads, all idle/waiting; **the
  bootstrap thread (tid 101) sleeps on `max_cpus_initialized`** = ml_wait_max_cpus(), no bsd_init / IOFindBSDRoot
  marker. Nobody calls ml_set_max_cpus because no CPU driver starts: the built-in SweetCPU personality
  (iokit/KernelConfigTables.cpp) has IONameMatch 'qcom,kryo' and the taimen ADT cpu nodes say 'qcom,kryo280'.
  0195: match both. (Screen outcome of the 0193 cadence fix not yet observed: needs a photo during a boot.)
- 2026-10-23 **boot 12 (r17 = 0195 CPU personality): CPU driver starts ("SWEET CPU: cpu=0 mpidr=0x0 came alive"),
  bsd_init entered, MAC/AMFI/Sandbox stand-ins, HFS registered, networking stubs, IOFindBSDRoot entered.** Then
  panic `pmap_ttne: Attempt to demote L2 block, tte=0x6000009da0060d, addr=0xfffffff00099a000` from
  sweet_ufs_rootdev -> io_map -> pmap_map: kernel_map's io_submap was carved at 0xfffffff000000000, right over the
  20 early alias blocks (FB/log/MDP) at 0xfffffff000200000+. The arm64 kmem_init reserves only pmap_virtual_region 0
  = [gVirtBase & 8M, virtual_space_start) and region 1; the 1 GB L1 slot below the physmap is free VA. 0196: TAIMEN
  region 2 = [link & ~(1G-1), gVirtBase & 8M) reserved.
- 2026-10-23 ~07:05-10:50 interruption: the session hit its usage limit mid-chain. The 0196 chain had NOT committed
  (patch file empty), "build 18" re-watched build 17 and "boot 13" re-booted the r17 kernel (same panic, nothing new),
  and the scratchpad (patched xnu tree, agent transcripts) was wiped. Rebuilt the tree the CI way (dxb at the pinned
  SHA + xnu-8796.141.3 + dxb patches + 0001-0195; the libpthread/IONetworking staging scripts need xcrun so those
  files and patches 0031/0041 are absent locally; irrelevant for patch authoring). Four audits ordered by the user are
  in port/docs/taimen-audit-*.md: personalities/ADT (no second kryo-class mismatch), display/touch/GPU/Wi-Fi
  (present() never kicks on the 32 bpp path = SpringBoard blocker; touch protocol must be rewritten for FTM4),
  platform/SMP (per-CPU GIC diagnostics would blow the 5 s secondary budget; no restart hook), root path (UFS driver
  64-bit clean; boot the root with ufsrw=1, cs_enforcement_disable=1, no taimenreboot).
- 0196 (re-done) reserve the alias 1 GB slot; 0197: present() kicks on taimen, bl_on=0, PE_halt_restart = PSCI
  SYSTEM_RESET, GIC diagnostics once. Build 19.
- 2026-10-23 **boot 13 (r19 = 0196 + 0197): the UFS root path works.** `SWEET UFS: "userdata" on LUN 0: first block
  0x14b000, 54307 MiB, read-only, DMA at 0x1029af000` (DMA above 4 GiB fine), `BSD root: ufsroot0`, then
  `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 20` / `cannot mount root, errno = 19` every 5 s for
  16 minutes (userdata is still LineageOS; expected). The kernel ran 982 s stably until the user held power. The APSS
  WDT did NOT bite after the pets stopped at 144 s (it did in boots 9-12; cause unknown). 0198: at the deadline the pet
  thread calls the PSCI SYSTEM_RESET hook itself. Screen: still the shim text + noise band, so 0193/0197 did not get
  the kernel's frames onto the panel. Tooling: attention.sh (chimes + speech + notification) on "phone did not come
  back"; xnu-log-pull.sh times out instead of blocking forever.
- 2026-10-23 boot 14 (r20 = 0198): deadline at 84.6 s, `PSCI SYSTEM_RESET now` logged, then the kernel kept running
  (mount retries to 281 s, user held power). So `smc #0` SYSTEM_RESET from XNU at EL1 does nothing (no panic, no
  exception logged), and the APSS WDT bite does not reset either since boot 13. 0199: logged reset ladder at the
  deadline (WDT register dump, smc, hvc, 1 s bite, PS_HOLD).
- 2026-10-23 boot 15 (r21 = 0199 reset ladder, deadline 60 s): `WDT EN 0x1 STS 0x2b8 BARK 0xfffff BITE 0xfffff`;
  smc SYSTEM_RESET returned x0 = 0x84000009 untouched; hvc the same; a 1 s WDT bite (BARK/BITE 0x8000, RST, EN) did
  nothing in 3 s (STS 0xf2); **PS_HOLD = 0 (0x10ac000) reset at once**, RAM log intact, back in Android at +107 s,
  bootreason "watchdog". So from XNU at EL1 under QHEE: PSCI calls are swallowed, the APSS WDT no longer bites
  (it did in boots 1-12; unexplained), PS_HOLD works. 0200: PS_HOLD for the deadline and PE_halt_restart.
- 2026-10-23 root install: TWRP 3.7.0 (../research/taimen/twrp, checksums verified) on boot_a, ios16-root.simg to
  userdata with `fastboot flash`. Chunks 1-23 went fine; chunk 24 is a 4.4 GiB FILL (zeros) that ABL writes very
  slowly, and the flash was killed by mistake. Fix: ../research/ipsw-x/ios16-root-tail.simg (DONT_CARE up to 11604 MiB,
  then the remaining chunks with big FILLs turned into DONT_CARE; 356 MB), flashed in 11 s. **The iOS root is fully
  on userdata.** Lesson: turn large zero FILLs into DONT_CARE before flashing (userdata was zeroed beforehand).
- 2026-10-23 **boot 16 (r22 = 0200, ROOT=1, taimenreboot=300): the phone came back into TWRP at +343 s by itself =
  the PS_HOLD reset works.** The log was NOT retrieved: TWRP's adbd is unreliable (shell hangs, "error: closed",
  "offline", then not listed although the USB interface is there). Same with Google's adb, so it is TWRP's side;
  made worse by concurrent adb jobs and `adb kill-server` killing transfers. What sometimes worked: MTP off in TWRP,
  one kill-server, then commands at once. TWRP 3.6.2 behaves the same; 3.5.2 via fastboot boot booted LineageOS.
  The UFS console ring (last 4 MiB + 4 KiB of userdata, ufsrw=1) should still hold boot 16's log; TWRP reads gave
  86 KB, 348 KB, then 0. Rule: one adb user at a time, never kill-server while another job uses adb.
- On-screen log viewer (no USB needed): shim built with -DLOGVIEW (`EXTRA_CFLAGS=-DLOGVIEW OUT_NAME=taimen-logview
  shim/build-taimen.sh ../backup/boot_a.img`) reads the ramoops PRZ at 0xb0080000 (sig 0x43474244, start +4, size
  +8, data +12) and draws at scale 2 a header, the key lines (panic, BSD root, SWEET UFS, mount, launchd, TAIMEN
  RESET, hfs, exec, Exception, Fatal, kernel trap) and the tail; re-kicks the panel every 5 s. Plan (boot 17):
  flash it to boot_a, `ROOT=1 BOOT_WAIT=25 NO_ALERT=1 tools/taimen/boot-xnu.sh r22 out/taimen-xnu-r22b
  taimenreboot=180`; after the PS_HOLD reset ABL boots the viewer from boot_a; photograph the screen. Not run yet.
- 2026-10-23 move to a Windows PC (WSL2 + Windows adb/fastboot): tools/windows/README.md.
