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
- 2026-10-23 move to the Windows PC done: WSL tree at ~/Projects/ios-taimen (Ubuntu 26.04, gcc 15 cross), phone on
  Windows fastboot.exe. Found on arrival: **slot a marked unbootable** (retry count 0; TWRP never marks the slot
  successful, so the resets of boots 13-16 used up its retries) and current slot b (successful, retry count 3,
  LineageOS boot_b). A reset into boot_b would have started Android on the HFS+ userdata. The log viewer
  (`-DLOGVIEW`) is now on **both** boot_a and boot_b and slot b stays active, so every reset lands in the viewer.
  Also: `research` (symlink to ../research) did not come over, and mkadt.py then only warns and leaves out
  `/product` (9772-byte ADT instead of 12456); setup-wsl.sh now creates the link.
- 2026-10-23 **boot 17 (r22 = 0200, ROOT=1, taimenreboot=180, ADT WITHOUT /product): iOS USERLAND RUNS.** Viewer
  photo `../research/taimen/boot17-logview.jpg`. The 1 MiB ring wrapped and kept only 1241-1377 s (the counter runs
  from power-on; the phone had sat ~20 min in fastboot, so kernel start ~1196 s). launchd is starting LaunchDaemons:
  xpcproxy (many), adprivacyd, calaccessd, ScreenTimeAgent, akd, mapspushd, wcd, assistantd, routined,
  appleaccountd, remotepairingdeviced, navd, installcoordinationd, ind, profiled, CircleJoinRequested,
  siriactionsd. **backboardd runs** and writes `IOServiceMatchingfailed for: AppleM2ScalerCSCDriver` every ~17 ms
  (once per 60 Hz frame; via the sweetnull /dev/null tee), which is what flushed the boot start out of the ring.
  No other port mentions M2Scaler; first suspect is the missing /product (sweet: MobileGestalt needs it for the
  display mode). Deadline at 1376.296 s (`WDT EN 0x1 STS 0x5003b BARK 0xfffff`), `TAIMEN RESET: PS_HOLD = 0` at
  1376.798 s, log lines continue ~0.3 s after it, then the reset and the viewer. Screen during the run: shim text.
- 2026-10-23 boot 18 (r22, ROOT=1, taimenreboot=120, ADT WITH /product, 12456 bytes): photo
  `../research/taimen/boot18-logview.jpg`. Same picture: launchd starts daemons (contextstored, dasd, logd_helper,
  iconservicesagent, nanoregistryd, aggregated, audioclocksyncd, analyticsd, passd, apsd, carkitd, geod, securityd,
  mobileactivationd, coreduetd, UserEventAgent, 36-54 s) and backboardd still writes `IOServiceMatchingfailed for:
  AppleM2ScalerCSCDriver` every ~17 ms to the end (deadline 146.3 s, PS_HOLD at 146.8 s). So /product is not the
  cause. The ring wrapped again (oldest kept line ~36 s). Cause found in the kernel: AppleCLCD reports the panel size
  (1440x2880, GetDisplaySize) while the ADT canvas is the iPhone X 1125x2436; sweet's 1080x2400 panel was smaller
  than the canvas, so there AppleCLCD presented 1125x2436 and the two agreed. Here CoreAnimation must scale canvas ->
  display every frame and asks for the hardware scaler. **0202**: TAIMEN presents 1125x2436, copied 1:1 into the
  middle of the panel (black borders). **0201** counts repeated /dev/null tee lines. **0203** logs the fbcon kickoff
  counters and MDP CTL0/VIG0/INTR + DSI0 registers every 20 s (the kernel's frames still never reach the panel). Log
  viewer: key lines without exec/mount/hfs (they crowded out everything), half the screen, 32768-line index. r23 = 0201
  (CI 40007805836, not booted), r24 = 0201-0203 (CI 40009260799).
- 2026-10-23 **boot 19 (r24 = 0201-0203, ROOT=1, taimenreboot=120): the M2Scaler loop is gone (0202) and backboardd
  presents ~48 frames/s.** Photo `../research/taimen/boot19-logview.jpg` (new viewer keys). BSD root ufsroot0 at 215.3 s
  (counter base again from power-on), `SWEET FB: AppleCLCD 1440x2880 stride 5760 at 0x9d400000 ... presented as
  1125x2436`, launchd.development absent -> /sbin/launchd, launchd boot tasks fsck, mount-phase-1/2, data-protection,
  finish-obliteration, detect-installed-roots, commit-boot-mode, restore-datapartition, init-with-data-volume,
  MSUEarlyBootTask, fips, keybag, usermanagerd, dirs_cleaner; daemons still starting at the deadline (fairplayd.H2,
  CommCenter, bluetoothd, syncdefaultsd, fmflocatord, bluetoothuserd at 335-338 s). 0203 report: at 0 s `kicks 17 done
  16 timeout 1, INTR_EN 0 STS 0x111000, CTL0 LAYER 0x1000005 TOP 0x20020 FLUSH 0x60000000, VIG0 SIZE 0xb4005a0 SRC0
  0x9d400000 YSTR 0x1680, DSI0 STATUS0 0x5 INTR_CTRL 0x500`; at 120 s `kicks 5815 done 5814 timeout 1`, registers the
  same. Identical to the shim's own dump (`../research/taimen/shim-diag-ufs.txt`). So the MDP takes and completes every
  kickoff with the shim's pipeline, yet the panel showed the shim's picture during boots 17-18: the open question is
  what the panel memory in RAM holds. **0204** reads rows 60/1440/2870 through uncached windows every 20 s and tests a
  write-combine write against an uncached read. The viewer's tail was 80-line `PROF` dumps (sweet_sample profiler of
  backboardd, on by default; `sweetprof=-` turns it off). Current slot was a again at this boot (both slots carry the
  same image, so it does not matter).
- 2026-10-23 **unattended boots: the log reader.** `tools/taimen/mkreader.sh` -> `shim/out/taimen-reader.img` = TWRP
  3.7.0's kernel (`skip_initramfs` renamed so a normal boot keeps the initramfs) + TWRP's DTB + a busybox initramfs
  (`tools/taimen/reader/init`). On both boot slots it replaces the on-screen viewer: after the PS_HOLD reset it copies
  pstore (XNU's PRZ) and the last 1025 blocks of userdata (the 4 MiB UFS console ring) into a FAT image, shows that
  to the PC as a USB drive labelled TAIMENLOG, and reboots into fastboot (`rebootbl bootloader`); a 150 s guard
  reboots into fastboot whatever happens. `tools/taimen/reader-pull.ps1` copies the drive on Windows;
  `tools/taimen/autoboot.sh KERNEL OUTDIR args` runs fastboot -> XNU -> reader -> logs -> fastboot with no hands.
- 2026-10-23 log reader first run (boot 19's logs): fastboot 15:22:23 -> reader drive on the PC 19 s after reboot
  -> fastboot again 15:23:30, no hands. Boot 19's whole UFS log (274 KB, ring not wrapped) shows **SpringBoard
  running** (exec, AppleKeyStore, /product reads, no crash) and **backboardd swapping 1125x2436 BGRA frames**
  between surfaces 2 and 3 (`SWEET FB: swap N layer 0 surface 2 1125x2436 fmt 0x42475241 bpr 4608`). Crashes only
  CommCenter (x3, no baseband) and driverkitd (sig 9). hfs mounted iOSRoot read-write after a journal replay; "root
  volume is not authenticated; continuing unsealed". The UFS ring missed the last ~5 s (flush every 10 s; 0205 kicks
  it before PS_HOLD); pstore has them.
- 2026-10-23 **boot 20 (r25 = +0204), first fully unattended run** (autoboot.sh: push -> CI -> boot -> reader ->
  logs -> fastboot 15:31:22). **The kernel's writes reach the panel memory**: `WC write 5a5a1234 over ff000000,
  Device reads 5a5a1234`; rows 60/1440/2870 of 0x9d400000 read black 0xff000000 (AppleCLCD's clear, not the shim's
  picture), row 2870 gets white console pixels from 40 s, row 1440's checksum changes once frames arrive and then
  stays (static picture). MDP: kicks 4043 done 4042 by 100 s, registers as in the shim, no DSI error bit. So RAM
  is right and the MDP sends; whatever keeps the shim's picture on the glass sits between the MDP and the panel.
  backboardd's power request (sel 12) was "on": no blanking. Next: 0206 + shim DSI0/PP0 dumps to compare.
- 2026-10-23 **boot 22 (r27 = +0205 screenshots, +0206 DSI0/PP0 dumps): iOS draws its boot screen; the DSI hangs.**
  Kernel screenshot of backboardd's last frame (`../out/taimen-xnu-r27/shots/end-surface2.png`, 1/5): the iOS boot
  screen, Apple logo and progress bar (data migration on first boot), ~15 % at 120 s. The panel memory thumbnail
  holds the same picture centered, console text in the borders. Shim (frames show) vs kernel (they do not):
  DSI0 STATUS0 0x0 / busy 0 of 20000 reads vs 0x5 / busy 20000 of 20000 (at 0 s and 60 s); FIFO_STATUS 0x11111080
  vs **0x99991080 = HS FIFO underflow on DLN0-3**; +0xa8 lane status 0x1f1f vs 0x1f00 at 60 s (lanes out of LP-11);
  PP0 line count 0xb40 vs 0x692; DSI0 +0x120 (CLK_STATUS) 0x2343 vs 0x8037c3. Everything else in DSI0 0x000-0x1ff
  and PP0 is identical. One starved transfer hung the DSI command engine for good (PP0 still says done each kick),
  which also explains the old photos' noise band at the top (the frame that broke). **0207**: recover with the
  msm-4.4 `mdss_dsi_sw_reset()` (CTRL off, CLK_CTRL 0x23f, SOFT_RESET 0x118 pulse, CTRL back, W1C the FIFO errors)
  when the FIFO underflowed or the DSI is still busy at the 50 ms timeout; done = PP0 done and DSI idle; AppleCLCD
  waits for the frame in flight before copying. ROOT boots add `sweetfb_noconsole=1`. pstore had no XNU text this
  time (0 bytes): the screenshots wrapped the 1 MiB PRZ; the 4 MiB UFS ring held all 1.6 MB.
- 2026-10-23 **boot 23 (r28 = +0207 DSI recovery, taimenreboot=180): the DSI recovers and stays healthy.** Two
  resets right after the kernel start (`DSI resets 2, last FIFO_STATUS 0x99991080`: the underflow happens during the
  early boot's memory traffic), none afterwards. At 60 s DSI0 STATUS0 0, FIFO_STATUS 0x11111000 (clean), lanes back in
  LP-11 between frames (+0xa8 0x1f1f), busy in 4296 of 20000 reads: transfers start and finish, ~24 frames/s by 180 s
  (3647 kicks, 3645 done, 2 timeouts). Screenshots: the boot progress screen again, bar ~22 % at 180 s (~15 % at 120 s
  in boot 22): first-boot data migration, ~7 %/min. The panel memory keeps the console text the kernel drew in the
  borders before the first swap muted it. Next: boot 24, 25 minutes, to let the migration finish on screen.
- 2026-10-23 **boot 24 (r28, taimenreboot=1500, sweetprof=-): 25 minutes of iOS, DSI stable.** 40552 kicks, 40550
  done, still only the 2 early DSI resets. Screenshots: boot progress at ~1 % at 60 s and ~85 % at 1500 s (slower towards
  the end): **the first-boot data migration starts over on every boot** (boot 23 had ~22 % at 180 s), so it needs one
  uninterrupted run of 30+ minutes; once done it is recorded on the read-write root. 65 CommCenter crashes (no
  baseband), fairplayd.H2 and bluetoothd relaunched 145 times each, SpringBoard alive throughout. Boot 25: r30 (0209
  touch driver) for 45 minutes.
- 2026-10-23 0209 taimen touch: `bsd/dev/taimen_touch.c` from `docs/taimen-touch-plan.md` (FTM4 at 0x49 on BLSP1 QUP5,
  RPM L28/L6 over GLINK at 0x778000, TLMM tiles, polled attention pin), SweetHID mapping panel 1440x2880 onto the
  1125x2436 canvas. boot-xnu.sh now passes debug=0x10000c: with 0xc the DB_NMI bit set panicDebugging and a panic
  parked the phone (no watchdog bite since boot 13); DB_REBOOT_ALWAYS sends it through PS_HOLD to the log reader.
- 2026-10-23 **boot 25 (r30 = +0209 touch, taimenreboot=2700; ended by the user holding power at ~16:36): the FTM4
  answers.** `SWEET RPM: link up` over GLINK at 0x778000, `L28 3300000 uV set 0/1: ok`, `L6 1800000 uV set 0/1: ok` (RPM
  works from XNU at EL1 on taimen); before power SDA/SCL read 0, after it 1 (rails on, switch gpio75 low); clocks on
  (vote 0x808a9d80, QUP5 CBCR 0x1, RCGR XO /1); QUP hw 0x20070000, io 0xa5, 64-byte input FIFO = 32 data bytes per read;
  INT disable ACKed, CONTROLLER_READY seen after the system reset; chip id read `00 36 36 36 36 36 36`: the first byte
  is right but repeats, i.e. after the STOP between command and read the FTM4 restarts its read pointer (plan risk 1).
  0211: reads as one QUP run START|w, DATA.., START|r, REC|len (repeated start). The data migration was ~10 % when the
  user reset it. A hard reset by the power key also lands in the log reader: logs came back as usual.
- 2026-10-23 0210 ROOTFIX (no reflash): `bsd/kern/taimen_rootfix.c` writes the files of a ROOTFIX payload blob into
  the mounted root before launchd; `tools/taimen/mkrootfix.py` builds it from the raw root image (7-Zip reads HFS+;
  `tools/simg2img.py`): launchd.plist (7 _PanicOnCrash renamed; CommCenter, fairplayd.H2, vmd, ReportCrash, spindump,
  tailspind, aggregated(.addaily) removed; backboardd CA_BACKDROP_SCALE=1), MediaToolbox StartRecorderServer off,
  containers, ExtensionKit flag, SpringBoard/Setup prefs, and LastSystemVersion 20H392 in
  ByHost/com.apple.migration.<IOPlatformUUID>.plist to skip the data migration. r31 failed to build (no strstr in the
  kernel); r32 = 0210 fixed + 0211.
- 2026-10-23 **boot 26 (r32 = +0210 fixed, +0211 repeated start; ROOTFIX blob; 5 min): TOUCH WORKS.** `chip id 00 36
  70 01 00 64 20`, `fw 0x2064 config 0x228 main 0x2c`, sense on, polled on gpio125; the user's taps came in as `down 0
  at 905,1734` / `up 0` etc. (panel pixels). ROOTFIX: every write failed with **EROFS** (error 30; ENOENT for files in
  the dirs that could not be made): the root comes up read-only and launchd's boot tasks remount it later. The user saw
  a **black panel** the whole run, although the panel memory (screenshot) held the Apple logo + progress bar with clean
  borders (0208), the display was never powered off, and the DSI completed every transfer after the 2 early resets
  (DSI0 0x000-0x1ff identical to the shim's afterwards; DSC settings live above). 0212: no frame is sent before
  AppleCLCD's first present (no underflow in the early boot's memory traffic, so no reset), dumps to 0x3ff, and
  ROOTFIX updates / to read-write (kernel_mount MNT_UPDATE) first. IOPlatformUUID A9683613-3704-53F2-95D0-98A8FB574E78
  (synthetic ECID).
- 2026-10-23 **boot 27 (r33 = +0212 no early kicks): the display path is clean.** No DSI reset at all, 5635 kicks /
  5635 done / 0 timeouts, DSI0 0x000-0x3ff identical to the shim's dump (DSC settings at 0x2a8 included), panel memory
  = Apple logo + progress bar. Whether the glass showed it is unconfirmed (the user was not sure). ROOTFIX's
  kernel_mount(MNT_UPDATE) on / returned EFAULT (HFS local mount args are copied in; bsdinit_task runs as init).
  0213: a kernel thread waits for launchd's read-write remount and writes then.
- 2026-10-23 **boot 28 (r34 = +0213): ROOTFIX writes, then launchd dies.** Root read-write 4.2 s after launch, all
  39 records written; `launchd_cache_loader` read launchd.plist 1.3 s later (so edits apply in the same boot), launchd
  exited "No service cache", `SWEET PANIC: initproc exited`, PS_HOLD, reader (the panic path works). Cause:
  vnode_open() does not truncate (O_TRUNC is the caller's job), the 792 KB plist overwrote the start of the 1.45 MB
  original and kept its old binary-plist trailer. **The phone's launchd.plist is corrupt** until a ROOTFIX boot with
  0214 (vnode_setsize to 0 first; remount polled every 50 ms) rewrites it: always boot with ROOTFIX now. The UFS ring
  of a boot that dies this early still holds the previous boot's log; pstore has the new one.
- 2026-10-23 boot 29 (r35 = +0214 truncate; ROOTFIX): **the user sees the Apple logo on the panel** (the first iOS
  picture on the Pixel 2 XL's own display). ROOTFIX 39 records, 10 written, 0 failed, launchd fine; no CommCenter and
  no fairplayd launches any more (boot 24: 65 and 145); 7466 frames, all done, no DSI reset. Still migrating.
- 2026-10-23 boot 30 (r36 = +0215 remove op, sentinel /private/var/datamigrator/migrate removed): still migrating (the
  sentinel was not the trigger), but **fast now** (the user: "the bar appears to be moving quickly", 1/4 in ~4 min;
  fairplayd's loop had been blocking locationd's CoreLocation migrator, sweet NOTES). The reader came ~4 min after the
  deadline under the migration's load, after autoboot's window had closed (window now deadline + 10 min).
- 2026-10-23 **boot 31 (r36 + ROOTFIX, taimenreboot=1200): iOS 16 HOME SCREEN on the Pixel 2 XL, touch works.** The
  migration finished in one run; Setup skipped by the ROOTFIX prefs; SpringBoard shows the home screen (screenshot
  `../research/taimen/shots/boot31-home.png`: status bar, two "Waiting..." widgets, Photos, App Store, Settings, Search,
  dock Phone/Safari/Messages). The user: "super laggy, but the touch screen works; the display is smaller than the full
  pixel screen" (the 1125x2436 canvas is shown 1:1 in the middle). 30 touches logged, 14008 frames / 14008 done / 0 DSI
  resets in 20 minutes; crashes: assetsd x2, driverkitd. Next: CPU clock (0216 measures it), secondary cores
  (docs/taimen-smp-plan.md, in progress), MDP hardware scaling (docs/taimen-display-scaling-plan.md, in progress).
- 2026-10-23 boot 32 (r37 = +0216 CPU clock; ROOTFIX, 5 min): CPU0 is a Kryo 280 **silver** core (MIDR 0x51af8014,
  part 0x801) at **~1554 MHz** (20M-iteration loop in 12.86 ms at 0 s and at 60 s). The lag is one small core plus
  software rendering. Lock screen: 27 frames in 5 min (an idle lock screen draws little; the 5600-7400 of boots 27-29
  were the migration's progress bar).
- 2026-10-23 boot 33 (r38 = +0217): the calendar comes from the PC (getGMTTimeOfDay = taimenepoch + 25 s). launchd's
  log and the status bar show the right date and time in iOS's default zone (Cupertino, PC time - 3 h).
- 2026-10-23 boot 34 (r39 = +0218, taimenhwscale=1 = scaling plan stage E2): each iOS frame is copied into a scan buffer
  below 4 GB (SCAN0 0x9e400000, stride 4608: the SSPP source address is 32-bit and iOS surfaces sit above 4 GB), VIG0
  shows it 1:1 at (157,222) (geometry 2). VIG0 SIZE 0x9840465 SRC0 0x9e400000 YSTR 0x1200, CURRENT_SRC0 = SCAN0 at
  60 s; 144 kicks / 144 done / 0 timeouts / 0 DSI resets; the user went to Settings > General > About (end screenshot).
  Census: one mixer LM0 (out 0x0b4005a0 = 1440x2880), DSC0 on and DSC1 zero, QSEED3 HW_VERSION 0x1002, ABL's VIG0
  SRC_OP_MODE 0x80000000 (scaler off).
- 2026-10-23 boot 35 (r40 = +0219, cpus=2, 180 s): **PSCI over smc works from XNU** in every context (thread; IRQs
  off; IRQs off with PMR 0): VERSION 0x10000 (PSCI 1.0), FEATURES CPU_ON / AFFINITY_INFO / SYSTEM_RESET all 0,
  AFFINITY_INFO 0 (on) for 0x0 and 1 (off) for 0x1-0x3 and 0x100-0x103. The probe's single **hvc** PSCI_VERSION never
  returned: the log ends there and ABL reported bootreason=watchdog. **Never hvc on taimen** (the hypervisor does not
  take it). CPU_ON was never sent. 0220: the hvc probe only runs with taimenpscihvc=1.
- 2026-10-23 boot 36 (r40, taimenhwscale=2 = stage E3: QSEED3 scales the canvas to 1330x2880 at x 55, one scan
  buffer; cpus=1, 240 s): **the user: "the screen was filled"**. Geometry 3 from the first present on, 0 DSI resets,
  0 frame timeouts, 20 frames in 240 s. At ~88 s, after two swipes up from the bottom edge and a touch-and-hold, iOS
  asked for display off (IOMFB power change: "SWEET FB: display off", CTL0 LAYER 0x1000000); SpringBoard then logged
  "Unbalanced calls to begin/end appearance transitions for SBSwitcherViewController" and AccessibilityUIServer
  started. Why iOS switched off is not known. The 60 touches after it did not wake it and the side button has no
  driver (sweetkeys=0): black until the deadline. 0221: a new touch on the dark panel sends one side-button press
  (kHIDUsage_Csmr_Power through SweetHIDButtons), at most every 3 s.
- 2026-10-23 **boot 37 (r41 = +0220, cpus=2, 180 s): the second core runs.** CPU_ON (smc, thread context, target 0x1,
  entry_pa 0x100429000) rc=0; `cpu=1 mpidr=0x1 came alive; SGI1 OK`, `2/2 CPUs initialized`. iOS ran the whole 180 s
  with no panic, ROOTFIX applied; the lock screen came ~20 s after the first log tick (30-45 s with cpus=1). CPU0
  still ~1554 MHz.
- 2026-10-23 boot 38 (r42 = +0221 tap to wake; cpus=4, taimenhwscale=2, 300 s): CPU_ON rc=0 for 0x1-0x3, 4/4 CPUs,
  SGI1 verified. 718 frames in 300 s, 0 timeouts, 0 DSI resets, geometry 3 throughout; 106 frames in 80-100 s and 171
  in 100-120 s while the user used it (one core: 20 in 240 s). The user: "still very laggy, but way better, multiple
  frames per second now".
- 2026-10-23 **boot 39 (r42, cpus=8, 300 s): all 8 cores run**, the gold cluster included: CPU_ON rc=0 for 0x100-0x103,
  each came alive within ~20 us, `8/8 CPUs initialized, SGI1 verified`; iOS ran the whole 300 s. **Tap to wake works**:
  between 60 and 80 s iOS switched the display off, the next touch logged `touch on the dark panel: side button press` and the
  display came back on. 207 frames in 300 s (little use). 0222 turns on the performance probe (sweetperf=1) to see the
  gold cores' clock.
- 2026-10-23 boot 40 (r43 = +0222, cpus=8, sweetperf=1): no SWEET PERF line, because sweet_ufs.c starts none of the
  Pixel 1 drivers on TAIMEN; 0223 starts the probe there. Otherwise a quiet 300 s (18 frames, no touches).
- 2026-10-23 **boot 41 (r44 = +0223, cpus=8, sweetperf=1): the gold cores run at 299 MHz.** Per core (MIDR, MPIDR,
  clock, memcpy of 4 MiB cached): silver 0x51af8014 0x0-0x3 at 1555 MHz, 0.4-1.3 GB/s; gold 0x51af8001 0x100-0x103
  at **299 MHz**, 1.55 GB/s. ABL leaves the gold cluster there and nothing raises it. Lock screen (first "display on")
  at 35 s with 8 cores (boots 39-41), 15 s with 4 (boot 38), 20 s with 2 (37), 30 s with 1 (36): the slow gold cores
  hurt. SWEET TOP while iOS settles: 300-580 % in all, kernel_task 50-110 %, SpringBoard up to 118 %, backboardd up to
  58 %, locationd/identityservicesd/healthd 30-55 %; idle after ~200 s. boot-xnu.sh ROOT=1 now defaults to cpus=4
  taimenhwscale=2. Next: gold/silver/DDR clocks (docs/taimen-clocks-plan.md) and the GPU (docs/taimen-gpu-plan.md).
- 2026-10-23 boot 42 (r44, cpus=4, taimenhwscale=3 = E4 double-buffered, sweetperf=1, 600 s): mode 3 ran the whole
  600 s (SRC0 alternating SCAN0/SCAN1, 0 timeouts, 0 DSI resets); no touches, so only idle numbers. The display stayed
  on for 10 minutes untouched: boot 36's display-off was a gesture, not the idle timer.
- 2026-10-23 boot 43 (r44, 90 s): the log reader now also copies `/vendor/firmware/a5*` from vendor_a (mounted
  read-only): 14 files (a530_pm4/pfp, a540_gpmu.fw2, a540_zap.mdt/.b00-.b02/.elf, ...), moved to
  `../research/taimen/firmware/` (not for git). docs/taimen-gpu-plan.md and docs/taimen-clocks-plan.md written.
- 2026-10-23 **boot 44 (r45 = +0224 clock stages; taimenclkdump=2 taimenclkmeasure=1 taimenddr=1804): the DDR vote
  works.** RPM clock handoff 0 failures, scaling on, bimc 1804000 kHz ok; debug mux bimc (sel 0x14e) 648 -> 902 MHz
  (sel 0xa9 reads 0), snoc 200 -> 239, cnoc 73; cached memcpy ~0.8 -> 1.09 GB/s on every silver core (cpu0 241 ->
  1089 MB/s across the vote while iOS was starting). Display and UFS unaffected. Every C0a and C0b region read
  cleanly: OSM ENABLE 0 on both clusters (nothing runs it), CPRh unprogrammed, SAW4 PMIC_STS 0 (no voltage there),
  APM and MEM-ACC 0, ACD 0x10000000. GFMUX cfg silver 0x301, **gold 0x403 (on GPLL0 / 2)**; silver PLL L 0x51
  (1555.2 MHz), **gold PLL running at L 0x20 = 614.4 MHz (mode 0x7) but not selected**. TSENS 35-39 C. boot-xnu.sh
  ROOT=1 now adds sweetperf=1 taimenddr=1804. Next: V1 (SPMI read of the S10/S13 rail voltages), then gold onto its PLL.
- 2026-10-23 boot 45 (r46 = +0225 SPMI; taimenvread=1 sweetkeys=1): **SPMI works from XNU** (arbiter v3 0x30000000,
  PON on APID 4). Rails, all FTS426 (type 0x1c, subtype 0x0a), enabled, mode 0x06: PM8998 S10 (silver) 828 mV,
  S13 (gold) **896 mV**, PM8005 S1 (GFX) 752 mV. Volume up read cfg 0 and would not take its pull-up: the msm8998
  TLMM is three tiles (WEST 0x3500000, NORTH 0x3900000, EAST 0x3d00000; pinctrl-msm8998.c) and GPIO 126 is in EAST.
- 2026-10-23 **boot 46 (r47 = +0226 G1; cpus=8 taimengold=1728 sweetkeys=1, 300 s): gold at 1728 MHz.** Preconditions
  all held (OSM 0, GFMUX 0x403, MEM-ACC 0, APM 0, S13 896 mV, gold PLL USER/CONFIG = silver's 0x9/0x20000aa8). Steps
  902.4, 1132.8, 1344.0, 1574.4, 1728.0 MHz: each locked, every gold core measured 19.2 x L and matched CPU0's
  self-test. Then MHz 1555 x 4 / 1727 x 4 for the whole run, no TSENS park. memcpy: silver ~1.0-1.1 GB/s, **gold
  7.9 GB/s**. Volume-up GPIO 126 now cfg 0x3 in 1; no false key events. Lock screen at ~20 s (35 s with gold at
  299 MHz, 15 s with 4 silver cores; 5 s resolution, and this boot also ran the ramp and the per-core benchmarks).
  boot-xnu.sh ROOT=1 defaults: cpus=8 taimengold=1728 sweetkeys=1 (+ full screen, DDR 1804).
- 2026-10-23 boot 47 (r47, the new defaults, 900 s): gold 1727 MHz for the whole 15 minutes, no TSENS park, 41 frames
  (nobody used it), 0 timeouts, 0 DSI resets, geometry 3.
- 2026-10-23 boot 48 (r48 = +0227 GPU census E0): everything reads without a stall. GCC: cfg_ahb 0x20008001,
  gpu_bimc_gfx 0x1 and iref 0x1 on, bimc_gfx 0x80000000 off, GPU_MISC 0x10000, GPLL0 vote 0x2. GPU CC 0x5065000:
  GPUPLL0 MODE 0 (off) but L/CAL_L 0x1a, FRAC 0xc555 (514 MHz VCO = the 257 MHz corner), USER 0x107, CONFIG
  0x20485699; GX_DOMAIN_MISC 0x1 (clamped); CX and GX GDSCR 0x222001 (collapsed), GDS_HW_CTRL 0x5ff4; CXO CBCR 0x1;
  gfx3d and rbbmtimer RCGs 0x80000000 (ROOT_OFF), CFG 0; gfx3d CBCR 0x80000620. mem-acc 0x1fcf004 = 0. PM8005 S1
  (GFX) FTS426 at 752 mV, on: enough for XO and 257 MHz (ceiling 724 mV), so the first power-up needs no rail write.
- 2026-10-23 boot 49 (r49 = +0228 GPU E1 at XO): bimc_gfx on, CX GDSC on (GDSCR 0xa0222000, GDS_HW_CTRL
  0x80001fe0), both RCG updates done; the rbbmtimer branch then stayed CLK_OFF (0x80000001): it needs GX on (kgsl
  enables the GPU clocks after both GDSCs). 0229 moves it after the GX GDSC.
- 2026-10-23 **boot 50 (r50 = +0229): the Adreno 540 is powered and answers.** GX: BCR and GMEM-reset pulses, clamp
  off (GX_DOMAIN_MISC 0), GX GDSC on (0xa0222000); gfx3d CBCR 0x6621 and rbbmtimer CBCR 0x1 running on XO;
  **VBIF_VERSION 0x200a0000, RBBM_STATUS 0x1, RBBM_ALWAYSON_COUNTER 0xac9 -> 0x1547 across a 100 us delay**. iOS ran
  normally around it. Next: E2, the command processor (SMMU identity map, a530 PM4/PFP microcode, ring, ME_INIT,
  CP_MEM_WRITE), from sweet_gpu.c's cp_test with the A540 values (docs/taimen-gpu-plan.md 2.6, 3.1).
- 2026-10-23 **boot 51 (r51 = +0230, sweetgpu=2 GPUFW): the A540 command processor works.** SMMU sCR0 0x200626,
  IDR0 0x48016403 (3 SMRs), IDR1 0x20008003 (3 CBs, CB space at +0x8000), SMR0 free, S2CR0 0x200ff (fault) before;
  identity map accepted (SMR0 0x80000000, S2CR0 0, CBAR0 0x10000, SCTLR 7, tlbsync 1). a530 PM4 19648 / PFP 17104
  bytes loaded, RBCCU and SP power on (0x140000), RBBM_STATUS 0x800001c1 before ME start; the ring was consumed (RPTR
  = WPTR = 14, CP_HW_FAULT 0, TRUST still 1) and CP_MEM_WRITE put c0ffee01 in the scratch page. CB0 FSR 0x400, FAR 0
  afterwards (to watch).
- 2026-10-23 boot 52 (r52 = +0231): the same result with the short cycle: the GPU thread starts after 10 s and pauses
  2 s; taimenend=1 ended the boot 15 s into the pet loop ("ending early ... GPU test done"); the PC ejected the reader
  drive after copying and the reader went straight to fastboot; the reader was not reflashed (inputs unchanged).
  **fastboot boot to fastboot again in 64 s** (was ~5-6 min). The PowerShell file list printed during the copy can
  show short sizes (the \wsl.localhost view lags); the files are complete.
- 2026-10-23 boot 53 (r53 = +0232 zap, sweetgpu=3): **SCM from XNU works on taimen**: INFO
  IS_CALL_AVAIL(PAS_INIT_IMAGE) -> 0/1. PAS_IS_SUPPORTED(13) -> 0/0, and 0232 stopped on it; 0233 only logs it
  (mainline's zap loader never asks; Android loaded this zap through PAS 13).
- 2026-10-23 **boot 54 (r54 = +0233): the A540 leaves secure mode and blits.** a540_zap: metadata (6684 bytes) at
  0x9ef00000, init_image 0/0, mem_setup 0x95a00000 + 0x2000 0/0, segment written, auth_and_reset 0/0;
  CP_SET_SECURE_MODE 0 -> TRUST 0, marker 5ec0de00 landed. **RGBA8 64x64 2D copy: 0 mismatches ("GPU 2D BLIT
  WORKS")**. RGBA8 -> RGB888 and the 32x32 -> 46x46 scale into RGB888 came out partly written (the dst holds 4-byte
  pixels with alpha 0x80: the A540 2D path does not take RB5_R8G8B8_UNORM as the A530 did). Not needed on taimen: the
  scan buffers are 32-bit and QSEED3 scales. CB0 FSR 0x400 with FAR 0 throughout. 70 s fastboot to fastboot.
- 2026-10-23 boot 55 (r55 = +0234 GPU clock, taimengpuclk=257): the phone came back to **fastboot** 22 s after the
  fastboot boot and `fastboot reboot` stayed there: slot-unbootable a and b, retry-count 0. ABL gives a slot 3 boots
  that never mark it successful (the reader never does); reflashing the reader every run used to reset that, and
  0231 stopped reflashing an unchanged reader, so both slots ran out after six reader boots. `fastboot
  --set-active=a` reset it (retry-count 3); autoboot.sh now runs it before every boot. The UFS ring then still held
  boot 54's log (boot 55 never flushed one).
- 2026-10-23 **boot 55b (r55 again): gfx3d at 257 MHz.** XO measured 19 MHz; GPUPLL0 relocked at L 0x1a / FRAC 0xc555
  (MODE 0xd0000005, USER 0x7), gfx3d RCG 0x103 (out_even / 2), **measured 256 MHz** (CP_PERFCTR_CP_0 always-count
  against the always-on counter); S1 left at 752 mV, MX not voted; the RGBA8 blit again 0 mismatches. 78 s fastboot to
  fastboot.
- 2026-10-23 **boot 56 (r55, taimengpuclk=596): gfx3d at 596 MHz.** rwmx vlvl 256 ok; PM8005 S1 752 -> 970 mV
  (FTS426 0x40/0x41, LSB then MSB, read back 970: the first PMIC write from XNU on taimen); GPUPLL0 L 0x3e FRAC 0x1555
  locked (MODE 0xd0000005); **measured 596 MHz**; the RGBA8 blit 0 mismatches. 82 s fastboot to fastboot.
- 2026-10-23 **boot 57 (r56 = +0235, sweetgpu=8 sweetgpuclk=600): the GPU service runs.** gfx3d 342 MHz (S1 775 mV,
  measured 341), GPU VA window 8-16 GB, scattered buffers mapped, sweet_gpu_quad API self-test OK (0 bad pixels,
  EINVAL/EFAULT as expected); MX 256, S1 970 mV, gfx3d measured 595 MHz, API OK again. The quad bench returned 0
  silently (no 4 MB contiguous buffer on the 4K kernel, most likely).
- 2026-10-23 boot 58 (r57 = +0236): quad bench at taimen's 1125x2436 (pitch 4608, scattered pages): 19.6 ms at 342
  MHz, 11.5 ms at 596 MHz. **E5 works**: with taimengpucopy=1 the A540 copies each iOS frame into the scan buffer
  ("frame 1/101 by the GPU in ~8 ms"), no fallbacks, 0 timeouts; not faster than the CPU copy while the CPU renders
  (present avg 8-23 ms, against 5-8 ms: sweet_gpu_quad cleans the 11 MB source out of the CPU caches first). Off by
  default until Mesa renders on the GPU. 0237 binds the presenter thread to CPU 7 (gold) instead.
- 2026-10-23 boot 59 (r58 = +0237 presenter on CPU 7, 300 s): "presenter bound to cpu 7"; with the user swiping, **7-13
  frames/s** (1165 kicks in 300 s, the most yet), 0 timeouts; present avg still 5-20 ms (in single-buffer mode the
  copy waits for the panel to finish the previous frame, sweet_fbcon_wait_idle).
- 2026-10-23 **boot 60 (r58, E6: sweetgl.dylib (PRs #1 + #2, Mesa 26.3.0-rc2) in backboardd): Mesa runs on the A540.**
  ROOTFIX installed /usr/lib/sweetgl-92a13443.dylib (19.2 MB, write-if-absent); SWEET ENV injected it; sweetgl found
  the iPhone X (dsc-x) shared cache, installed its QuartzCore fixes, opened "screen FD540 (freedreno)" and made
  CA's EAGLContext (API 3) on Mesa; backboardd flushed 37-60 frames/s; GPU copy (taimengpucopy=1) 900+ frames at
  ~7-8 ms. **The panel stayed black**: every attach of the display surfaces failed, "attachImage: surface 2 b3a8
  1125x2436 pitch 0 plane 0: format failed". On the iPhone X root backboardd's display surfaces are 'b3a8': BGRA8
  plane 0 (bpe 4, rows 4544) plus an A8 plane (13.88 MB); bind_surface refused multi-plane surfaces, and the kernel
  IOSurface reports no per-plane rows. Also texImageIOSurface copy errors 0x506 on LA08 230x186 / 256x1 surfaces.
- 2026-10-23 **boot 61 (r58 + sweetgl b3a8 fix, /usr/lib/sweetgl-<hash>.dylib): the lock screen drawn by Mesa on the
  A540** (the user saw it; first GPU-rendered iOS frame on taimen). Swiping up started the unlock animation, which
  then froze; the power button (now driven, xnu 0225) turned the display off and on. From ~100 s no more swaps.
  backboardd's SMP sample, symbolicated against sweetgl.dylib (Mach-O symbols read by a scratch script; load base
  0x1024c4000 from impl_thrd_routine): the CA render thread in glDrawElements -> fd5_draw_vbos -> ir3_cache_lookup ->
  ir3_get_shader -> util_queue_fence_wait, waiting for two shader-compile threads (util_queue ->
  create_initial_variants_async -> ir3_shader_get_variant -> compile_variant) that both sat at ir3_block_create+0
  while backboardd used 1-8 % CPU. Two threads at the first instruction of a function not run before = a page-in
  of the dylib's text that does not complete: no free page (XNU has 512 MiB; kernel_task was at 193 % around 90 s)
  or a stuck UFS read. 0238 logs memory every 10 s and adds synthetic swipes; TAIMEN_MEM tries more RAM.
- 2026-10-23 **boot 62 (r60 = +0238, CPU rendering, TAIMEN_MEM=0x40000000, synthetic swipes from 60 s, 180 s): 1 GiB
  of RAM works** (vm_page_bootstrap 251474 free pages, against 117761 at 512 MiB). The CPU baseline for the synthetic
  swipes (taimenswipe, xnu 0238: one swipe up from the bottom edge, then page swipes): **10-20 swaps/s** while
  backboardd ran at 150-540 % CPU (CA's CPU renderer on 5+ cores); TAIMEN MEM free 2-28k pages, compressed 36-65k.
- 2026-10-23 boot 63 (r60, TAIMEN_MEM=0x7e400000, the whole free upper bank, ~2 GiB): panic at 50 s,
  "kmem_range_init: vm_map_locate_space failing for claim kmem_ptr_range_meta @vm_kern.c:3645". The 4K-page kernel
  has a 39-bit kernel VA space and kmem_range_init sizes its claims from the memory size; ~2 GiB does not fit. 1 GiB
  stays the default for the GPU tests until the claims are made smaller.
- 2026-10-23 boot 64 (r60, E6 with 1 GiB: sweetgl-489cf405.dylib in backboardd, sweetgpu=8 at 596 MHz, GPU copy,
  synthetic swipes every 20 s, thread samples every 20 s, 300 s): **no freeze**, but **0.5-2 swaps/s**: sweetgl 0.3-0.6
  flushes/s, glFinish 7-15 ms per flush, 1.7-2.9 s between flushes; swap gaps all over 54 ms. backboardd used only
  8-11 % CPU during the animations (SWEET TOP), the system 15-22 %: the frames are gated, not slow to draw. Wired grew
  58k -> 91k pages (each imported 13.9 MB display surface stays wired while sweetgl caches it). The thread samples
  (symbolicated with the new tools/sweetgl/dsc-sym.py against research/dsc-x) found backboardd's CA::WindowServer::
  IOMFBServer threads idle in mach_msg and the Mesa queue threads idle in pthread_cond_wait at every sample. Boot 60
  (display surfaces failing to bind) flushed 37-60 frames/s, so the gate came with rendering into the imported
  display surfaces.
- 2026-10-23 boot 65 (r60, boot 64 plus the PC profiler on backboardd, swipes every 10 s): first try refused (boot-args
  640 bytes; boot-xnu.sh now drops overridden keys, XNU takes the last occurrence) and autoboot then waited 14 min for
  a reader that never came (fixed: it stops when boot-xnu.sh did not boot). The real boot: **sweetgl refused 17
  EAGLContexts before the GPU service was up** (service ready at log line 2668, backboardd loaded sweetgl at 925) and
  CA kept its **software renderer** for the display: the profile (tools/sweetgl/prof-sym.py, shared cache through
  dsc-sym.py) is CA::OGL::SW (scanline, image_sampler, SWContext::draw_elements, GaussianBlurFilter) plus kernel
  workqueue park/unpark and ticket-lock contention; backboardd 54-84 % CPU for ~0.7 swaps/s on the home screen.
  (Boot 62's 10-20 swaps/s were in Settings, which the unlock swipe brought back: not the same scene.) Fixed in
  sweetgl 5c9d70c/57ebaea: the first context waits for the service (SWEETGL_WAIT, default 60 s).
- 2026-10-23 boot 66 (sweetgl b15f7dc: GL-call gaps over 50 ms logged with frame-pointer backtraces): Mesa drew
  (API 3); 0.6-1.2 flushes/s, glFinish 5-17 ms, **frame build 100-340 ms** from the first GL call to the flush, with
  no gap over 50 ms inside the display frames and backboardd at 7-12 % CPU: the time is spread over GL calls that
  block. Every frame after a pause starts in CA::WindowServer::IOMFBDisplay::can_update_ <- Server::render_for_time
  <- IOMFBServer::vsync_callback <- display_timer_callback: CA paces itself with its own display timer (it never
  enables our IOMFB vsync notifications, selector 72) and can_update_ polls IOMobileFramebufferSwapWait on the
  earlier swaps (our selector 6 returns at once). A second thread (CA::Render::Server::server_thread ->
  _XRenderDisplayLayerSurface -> EAGLServer::render_surface, SpringBoard's snapshots into the 13.9 MB b3a8 surface
  it creates on every swipe) stalls 600-730 ms before the glGenTextures of GLESContext::make_buffer_current.
  tools/sweetgl/dsc-dis.py disassembles shared-cache functions (binutils objdump) with call targets named.
- 2026-10-24 boot 67 (sweetgl 57ebaea, the GPU wait): "GPU service up after 7125 ms of waiting", no context
  refused, Mesa from the first frame; otherwise as boot 66 (0.6-1.2 flushes/s).
- 2026-10-24 **boot 68 (sweetgl 260b63a: time inside every GLI slot): Mesa's frames are CPU-bound in
  glDrawElements**, ~1.4 ms per draw (e.g. 1134 ms for 744 draws in one swipe's 10 s); ~70 draws per frame make
  the ~100 ms frame build. glFinish (sgl_Flush) 9-10 ms per frame, shader compile/link 0.1-0.2 s once, texture
  uploads and IOSurface binds small; glTestObjectAPPLE not among the top 8. sweetgl issues
  _mesa_TextureBarrierNV before every draw whose program samples its own render target (CA's destination reads,
  feedback_barrier) and every fbfetch draw, and Mesa 26.3's freedreno implements texture_barrier as a whole
  batch flush (fd_context_flush): a GPU submit per such draw, each listing the 13.9 MB display surface for the
  kernel's per-page cache check. sweetgl 0c843a9 adds an A5xx barrier inside the batch for GMEM bypass to
  mesa.patch (CCU color/depth flush with timestamps, WFI, UCHE invalidate, CCU color invalidate); 5544c49 counts
  submits and their kernel time in the 10 s report.
- 2026-10-24 boot 69 (sweetgl 5544c49: GPU submits counted): **the draw cost is GPU submits**: one swipe window
  had 742 draws, 194 submits and 1380 ms inside the kernel submit call against 1353 ms in glDrawElements, ~7 ms
  of kernel time per submit; 361 submits / 1855 ms in the busiest window.
- 2026-10-24 boot 70 (sweetgl 0c843a9: the A5xx in-batch texture barrier): submits per swipe window down to
  89-131 (221 in the busiest), 1.1-3.6 flushes/s, some windows 2.7-3.2 swaps/s with 18-19 swap gaps under 20 or
  54 ms; still ~7 ms of kernel time per submit. The rest are batch flushes freedreno makes when a pass samples
  what an earlier pass rendered (CA's blur and backdrop passes). Mesa's App Library at the end is the same as in
  boot 69 (shots/end-surface*.png): the barrier draws correctly. Next: the kernel's ~7 ms per submit (profile).
- 2026-10-24 boot 71 (r61 = +0239 kmem claims logged on failure, TAIMEN_MEM=0x7e400000): no XNU output at all this
  time (empty pstore, the UFS ring still boot 70's, bootreason watchdog ~44 s after the fastboot boot), unlike boot
  63's kmem_range_init panic: ~2 GiB also fails before the console. Left for later; 1 GiB stays.
- 2026-10-24 boot 72 (sweetgl 565f8e0 = barrier + 96 MB surface cache, PC profiler on backboardd): the kernel part
  of backboardd is SweetGPUUserClient::submit (928 of the inclusive samples): FlushPoC_DcacheRegion,
  arm_force_fast_fault_with_flush_range under pmap_clear_modify, hw_lock_bit, pmap_is_modified: the cache clean and
  write-protect of the pages Mesa wrote into its CPU-cached bos. ~13 frames per swipe at ~70 ms.
- 2026-10-24 **boot 73 (r62 = +0240 taimengpuheap=256: Mesa's bos from a write-combined heap at 0x16e400000):
  works**, "SWEET GPU: heap 256 MB at 0x16e400000"; same App Library picture at the end; kernel time per submit
  ~7 -> ~4 ms; 1.5-1.9 flushes/s, 1000-1700 draws per swipe window, swap gaps mostly 37-54 ms during the
  animations (~20-25 frames/s, from ~1 at the start of the night). 0241 splits the remaining ~4 ms.
- 2026-10-24 boot 74 (r63 = +0241 submit split, heap on): **the ring never waits** ("0 ms waiting for room"; the
  ring part 1 ms per 10 s) and **every page checked was flushed** (e.g. 618229 of 618229 in one 10 s window), ~1 us
  each, ~4 ms per submit: the imported 13.9 MB display surfaces, ~3400 pages each, at every submit. Surface buffers
  (IOBufferMemoryDescriptor) keep a writable kernel mapping, so pmap_clear_modify cannot clear their modified bit
  and flush_if_written saw every page as written. 0242: imported buffer surfaces are flushed whole only when the
  surface seed moved since the last flush (CPU writers lock; each write unlock bumps the seed, and the state page
  is read-only for users).
- 2026-10-24 **boot 75 (r64 = +0242, heap on, sweetgl 565f8e0): ~60 frames/s during the swipe animations.**
  Submit cache work gone ("1342044 pages checked, 0 flushed", 0-4 ms per 10 s), ~0.1 ms of kernel time per
  submit, 350-750 submits per swipe window; 4-11 flushes/s; swap gaps mostly under 20 ms (one window 84 of 98, the
  others 33-38 per swipe); 3000-4000 draws per window at 60-90 us each, ~600 ms of GL time per 10 s; backboardd
  4-10 % CPU; wired steady at ~62k pages (90k+ before the 96 MB surface cache limit). The home screen at 60 s and
  the App Library at the end match the earlier boots, on the surface and on the panel (shots/).
  The night's chain, ~1 -> ~60 frames/s: the first EAGL context waits for the GPU service (CA had kept its software
  renderer), an A5xx texture barrier inside the batch (mesa.patch; CA's destination reads each flushed a batch),
  Mesa's bos write-combined from a heap above XNU's RAM (0240), imported surfaces flushed only when their seed moves
  (0242).
- 2026-10-24 boot 76 (boot 75's setup, 600 s, 50 swipes every 10 s): **stable at ~60 frames/s throughout** (each
  swipe window 28-36 swap gaps under 20 ms, 9-20 under 37, at most 2 longer); no panic, no GPU hang; wired levels
  off at ~87k pages, free 3-9k, compressed ~47k. driverkitd is killed (sig 9) once in this boot as in every boot
  since at least 58, the CPU-only boot 62 included: not the GPU path.
- 2026-10-24 boot 77 (r64, the plain ROOT=1 default, only taimenreboot and swipes added): boot-xnu.sh took the GPU
  setup from ../out/sweetgl/current.env ("GPU boot: sweetgl 565f8e0"), the heap came up, sweetgl waited 5.9 s for
  the GPU service, Mesa drew at the same ~60 frames/s.
- 2026-10-24 boot 78 (r65 = +0243 taimengest: synthetic Settings launch / home / Control Center, the GPU default):
  **ABL did not start it**: "+23s: back in fastboot", the shim log unchanged, no XNU log, empty pstore; the same
  again in 78b. The payload was 36715104 bytes against boot 77's 36698688 (r65's kernel is 16 KB larger): ABL
  starts no ramdisk over ~35 MiB (36700160 lies between), and the 19 MB sweetgl dylib in the ROOTFIX had used up
  the room. autoboot then waited 14 min for a reader and would have raised the "phone hung" alert: it now exits
  (7) when boot-xnu.sh sees the phone back in fastboot, and boot-xnu.sh refuses payloads over 35 MiB (after
  retrying with the base ROOTFIX when the dylib is already on the phone). xnu 0244: sweetenv leaves out a
  DYLD_INSERT_LIBRARIES file that is missing (dyld would refuse to start backboardd). xnu 0245 + mkrootfix.py
  --zfile: zlib-compressed ROOTFIX files (op 5), the dylib 19190992 -> 4376919 bytes, the default ROOTFIX 5.2 MB.
- 2026-10-24 boot 78c (r66 = +0244, base ROOTFIX by the size rule, gestures): booted, the dylib from earlier boots
  was there and injected, all six gesture cycles ran; but **PM8005 S1 (GFX) was off** ("S1 ok en 00 752 mV", every
  earlier boot en 80) after the two refused images, the GPU service stopped ("GFX rail off"), sweetgl waited its
  60 s and CA drew with its CPU renderer. xnu 0246 switches S1 on at 752 mV when found off.
- 2026-10-24 **boot 79 (r68 = +0245 zlib ROOTFIX files, +0246 GFX rail on; GPU setup, the gesture script)**:
  payload 22.9 MB; "create (zlib) /private/var/tmp/zfile-test-....bin ... ok" (op 5 inflates); S1 on this time,
  the GPU service ready, Mesa after 5.7 s. Settings opened and Control Center came down, both drawn correctly
  by Mesa (shots/settings-surface4.png, cc-surface4.png). Frame pacing: home screen paging stays ~60 frames/s;
  the Settings launch/close and Control Center windows mostly 20-37 ms swap gaps with many under 20
  (~35-50 frames/s); 1200-2700 submits per 10 s at ~0.1 ms of kernel time, glFinish 2-4 ms per frame,
  backboardd 20-22 % CPU, SpringBoard 26-42 %, Preferences 26 % while it launches.
- 2026-10-24 boot 80 (r68, the gesture script, PC profiler on backboardd): no hotspot. CA::OGL render_layers /
  LayerNode::apply / ImagingNode::render hold ~12 % of the inclusive samples, the Updater's prepare_layer ~4 %,
  Mesa's draw path (fd5_program_emit, fd5_emit_state, update_program, u_vbuf, hash lookups) a few hundred
  samples spread thin, the kernel ~2.6 % (sweet_gpu_wait, the GPU user client). The heavier scenes are CPU work in
  CA and Mesa. With one scheduler pset, wakeups go to the lowest idle CPU, a silver one (amp plan; which cores
  these threads ran on is not measured). xnu 0247 taimenslow=0 measures gold-only placement.
- 2026-10-24 boot 81 (r69 = +0247, taimenslow=0: CPU0 + the four gold cores; the gesture script): "TAIMEN SCHED:
  cpu1-3 derecommended: 0". Share of swap gaps under 20 ms against boot 79 on eight cores: Control Center windows
  ~54 % (79: ~41 %), Settings launch/close ~38 % (79: ~33 %), the rest mostly 20-37 ms. Gold placement buys some,
  not all: the gold clock (1728 of ~2457 MHz without OSM/CPR) is the other half. xnu 0248 taimenamp=1 keeps eight
  cores and places only above-utility threads gold first.
- 2026-10-24 **boot 82 (r70 = +0248, taimenamp=1 on eight cores, the gesture script)**: "TAIMEN SCHED: gold cpus 4-7
  at 1728 MHz, preferred with taimenamp". Swap gaps under 20 ms: Control Center windows ~69 % (61-75 %; boot 79
  stock ~41 %, boot 81 gold-only ~54 %), Settings launch/close ~45 % (33 %, 38 %). Gold first for above-utility
  threads beats gold-only: silver keeps the background work off the gold cores.
- 2026-10-24 boot 83 (r70, taimenamp=1, 25 gesture cycles, 620 s): all 25 cycles done, no panic, no GPU hang,
  wired ~65k pages, free 7-15k; swap gaps through it as in boot 82 (Control Center windows 97-114 under 20 ms
  of ~130-160). taimenamp=1 is now in boot-xnu.sh's ROOT=1 defaults.
- 2026-10-24 boot 84 (sweetgl dea23e0 with SWEETGL_FINISH=0: CA's end-of-frame flush without glFinish; r70,
  taimenamp=1, gestures): the new dylib went in through the zlib ROOTFIX record ("create (zlib)
  /usr/lib/sweetgl-d307593f.dylib (4376940 bytes): ok"), so new sweetgl builds install again. **Worse
  pacing, and backboardd crashed** once (relaunched): EXC_BAD_ACCESS far 0x30 in
  _mesa_hash_table_search_pre_hashed <- fd_submit_append_bo <- fd_batch_resource_read_slowpath <-
  batch_draw_tracking <- fd_draw_vbo <- ... <- sgl_DrawElements: freedreno's deferred submit racing the next
  frame's draws once the render thread no longer waits. glFinish stays (the default); the switch is off.
- 2026-10-24 **boot 85 (r70, the plain ROOT=1 default: GPU path + taimenamp=1; 1900 s, taimengestn=90)**: 88
  Settings launch / home / Control Center cycles until the deadline, no panic, no crash of any process, no GPU
  hang, gold never parked; wired 64-70k pages, free 9-25k, compressed 31-62k; swap gaps the same through the
  half hour (Control Center windows 90-112 under 20 ms, 40-57 under 37).
- 2026-10-24 boot 86 (sweetgl d94f72e, SWEETGL_FINISH=0 now a fenced st_flush without the wait; r70, gestures):
  no crash, but **worse pacing** than glFinish: Control Center windows 13-32 swap gaps over 54 ms each (boot 82:
  0-1). The CPU running ahead only moves the wait elsewhere (buffer reuse, the kernel's synchronous GPU copy of
  the frame). glFinish at CA's flush stays; SWEETGL_FINISH stays off.
- 2026-10-24 **boot 87 (r70, the plain ROOT=1 default, 3700 s, taimengestn=175)**: ran to the deadline (WDT pet
  at 3695 s); 155 gesture cycles in what the 4 MB ring kept (it wrapped), no panic, no crash, no GPU hang;
  wired flat at 65-68k pages for the hour, free 7-17k, compressed 29-46k.
- 2026-10-24 boot 88 (r70, TAIMEN_MEM=0x60000000 = 1.5 GiB, GPU setup): like boot 71's 2 GiB, watchdog reset ~44 s
  after the fastboot boot, empty pstore, no XNU log at all; the shim log ends at "saved (before loader)" as it
  always does. Above 1 GiB XNU now dies before its console (boot 63 at 2 GiB once reached kmem_range_init).
  1 GiB stays.
- 2026-10-24 hand-off boot (r70, the plain ROOT=1 default, no deadline, booted 06:45; log pulled at 08:15): the user
  used it for ~85 min (233 touches): "the frame rate is way higher, but [the screen] flickers a lot". No panic, no
  GPU hang. **The presenter is the limit and CA draws into what it copies**: while the user swiped CA swapped
  37-46 frames/s, the presenter put 22-27 on the panel, ~30 ms per present (the gesture boots 79-87 too: present
  avg 25-32 ms, presents ~60 % of swaps). The swap-gap figures of boots 75-87 counted CA's swaps, not panel frames.
  SwapWait answered at once, so CA reused a surface the presenter was still copying: torn and half-drawn frames.
  xnu 0249: SwapWait honest (done once copied), a reuse counter, and the GPU copy skips the source clean when the
  seed has not moved.
- 2026-10-24 boots 89-91 (r71 = +0249, GPU default, gesture script, 260 s each): **0249 ends the
  flicker's cause, two scan buffers end the slow presents.** Per 10 s window, "swaps of the surface
  being copied": 89 (0249) 0-1; 90 (sweetfb_swapwait=0, the old answer) 45-75, CA drew over the
  frame the presenter was copying. With 0249 CA waits for the copy, so its rate is the presenter's:
  89 presented 78 % of the swaps at 27-31 ms each (one buffer: wait for the panel, then copy).
  91 (taimenhwscale=3, two scan buffers, no panel wait before the copy): present 8-15 ms, presents =
  swaps (99 %+), reuse 0, the GPU copy ~4 ms; screenshots of Control Center and Settings correct.
  taimenhwscale=3 is the ROOT=1 default from here.
- 2026-10-24 boots 92-93 (r71, the shim watcher, SHIM_WATCH=1: CPU 7 stays in the shim, XNU on 7
  cores): **more than 1 GiB fails in kmem_range_init, not before the console.** 92 (1.5 GiB): the
  watcher saw the XNU log stop 10.6 s after the jump and saved it: the same panic as boot 63, now
  with 0239's claims dump. The compressor asks for 0x600268000 (24 GiB: macOS sizing, 16 x max_mem,
  which TAIMEN inherits) next to the 24 GiB zone map; the free hole is 0xb67d60000 (45.6 GiB), so
  the remaining ranges' sizes underflow (kmem_data_range 0xffff7fff405ac000). The panic's MACH
  Reboot then hangs (no PE_halt_restart before IOKit) and the watchdog reset of boots 71 and 88
  came back with no pstore; the watcher's PS_HOLD kept the zone (console-ramoops 105917 bytes).
  Also: vm_page_bootstrap counted 95954 free + 2350 wired = exactly a quarter of 1.5 GiB (boot 63
  at 2 GiB: a quarter too; 1 GiB: all of it). 93 (1 GiB control): the watcher stood down (XNU's UFS
  driver took the controller), the boot ran its 200 s; with cpus=7 the gold clock stays at 300 MHz
  ("needs cpus=8"). xnu 0250: compressor VA 4 x max_mem on taimen, the page accounting printed.
  Linux's System RAM runs unbroken from 0xb1000000 through the 4 GB line to 0x17e4bffff (~3.2 GiB):
  xnu 0251 + mkadt TAIMEN_BASE=0xb1000000 put XNU's one range there.
- 2026-10-24 hand-off boot 2 (r71, two scan buffers by default, 40 min deadline): nobody touched the phone
  (0 touch moves), so no flicker verdict yet.
- 2026-10-24 boots 94-95 (r73 = +0250 compressor VA, +0251 RAM from the low bank; the shim watcher, gestures):
  **94 (1.5 GiB at 0x100000000) runs iOS normally**: kmem_range_init passes, gestures, presents = swaps at
  7-12 ms. The "quarter of the pages" of boots 63 and 92 is not a loss: above 1 GB stock XNU picks 16 KB VM
  pages over taimen's 4 KB tables (arm_init.c, `args->memSize > 1 GB`), so 95951 pages x 16 KB is all of it
  ("SWEET VM: max_mem 0x60000000", lopage/secluded 0). Memory under the gesture script: free 23-75 MB,
  compressed ~290 MB. **95 (TAIMEN_BASE=0xb1000000, 2.96 GiB)**: start.s and the bootstrap tables held,
  191053 x 16 KB pages, kmem claims fit, then "write_pte_fast: PTE write is unaligned" from
  pmap_create_commpages: the PFZ text page takes a random 4 KB leaf index (138), and a 16 KB page needs
  4 PTEs from a 4-aligned one. Any boot above 1 GB had a 1 in 4 chance of an aligned draw (94 got one).
  xnu 0254 rounds the index down. xnu 0252-0253: gold through CPRh and the OSM (taimenosm=ROW,
  docs/taimen-osm-cprh.md), not booted yet.
- 2026-10-24 ~09:43, the user on boot 96 (r75: 0249 + two scan buffers, RAM from 0xb1000000): **"I don't see any flickering"**. The flicker of the first hand-off boot is gone.
- 2026-10-24 **boots 96-98 (r75 = +0252-0254): 2.96 GiB of RAM and gold at 2342 MHz.** 96 (RAM from
  0xb1000000 to the GPU heap, the shim watcher, gestures, 300 s): ran to its deadline; free 125-900 MB and
  **no compressed page at all** (1 GiB: 13-18k compressed pages, 1.5 GiB: ~18k); gold 299 MHz only because
  the watcher's cpus=7 refuses G1. 97 (taimenosm=19, 2.96 GiB): the OSM path at G1's clock: SCM probe 0/1,
  fuses combo 19, CPRh apc1 readback as the spec (CTL 0x80a00b5, CORNER(19) 0x3661656d), 10 sequencer
  writes through TZ accepted, enable at row 0: S13 896 -> 652 mV (SAW 0x4028c), GFMUX 0x400; row 19: PLL L
  0x5a, S13 796 mV (100 mV under XBL's 896 at the same clock), all four gold cores 1727 MHz and the
  self-test. 98 (taimenosm=33): every all-core row checked on all four gold cores; the APM went to APC (3)
  at row 20 (1804.8, 824 mV), MEM-ACC to level 3 (0x1370 = 1) at row 21 (1881.6, 860 mV), as the spec
  said. Row 33 (2361.6) settles on row 31's 2342.4 MHz / 1052 mV (CPRh corner 28); not chased (<1 %).
  The swipes: **44-46 swaps/s, every one presented** (r71 at 1728: ~17), present 9-14 ms. One window
  read gold at 1677-1996 MHz under load and came back (the OSM's own FSMs, cause unknown). No panic,
  no crash. TSENS only printed above 90 C (none); xnu 0255 logs the hottest sensor every tick.
  boot-xnu.sh ROOT=1 defaults: TAIMEN_BASE=0xb1000000 TAIMEN_MEM=0xbd400000, taimenosm=33 (G1 fallback).
- 2026-10-24 hand-off boot 3 (boot 99, r76 = the new ROOT=1 defaults: 2.96 GiB from 0xb1000000, gold OSM row
  33, two scan buffers; 45 min): no panic; nobody touched it (0 touch windows). Gold settled at 2342 MHz /
  1052 mV as in boot 98. **Temperatures (0255): hottest 61.1 C (boot), 42-44 C idle at 2342 MHz.** Free
  memory 160-220 MB after settling, never a compressed page. Userland: driverkitd sig 9 and PosterBoard
  sig 5, both already in r70's 1 GiB hand-off (not new). Boot 98's swipes: backboardd in GL 3.3 s of 10 s,
  Flush (GPU wait) 1.0-1.6 s of that: CA's CPU side still the larger part at 45 frames/s. xnu 0256:
  silver through its OSM (taimenosmsilver=ROW).
- 2026-10-24 **boot 100 (r77 = +0256, the defaults + taimenosmsilver=21, gestures, 300 s): silver at 1900.8
  MHz.** Gold through the refactored A1 exactly as boot 98 (row 33 -> 2342 MHz, 1052 mV). Silver: fuse row 67
  0x...a288edbbbb as expected for combo 19, entered on its PLL (GFMUX 0x301, L 0x51), S10 828 -> 644 mV at
  row 0, then rows 17 (1555.2, 796 mV), 18 (1670.4, 840 mV, APM to APC: status 0xff000303), 19 (1747.2,
  868 mV, MEM-ACC level 3), 20, 21 (1900.8, 928 mV), each checked on CPUs 0-3 (1897-1900 MHz, self-test
  ok). Ran to its deadline; hottest 72.8 C during the ramps, then 44-55 C. taimenosmsilver=21 is in the
  ROOT=1 defaults (boot-args with the test args now ~602 of 607 bytes).
- 2026-10-24 hand-off boot 4 (boot 101, r77, all defaults incl. silver 1900.8; 60 min): no panic, nobody
  touched it. Silver 1900 / gold 2342 MHz the whole hour, 44-46 C idle (73.1 C during the boot ramps),
  no compressed page, free 15.6k -> 12.6k pages (16 KB) over the hour. driverkitd sig 9 and PosterBoard
  sig 5 as on every boot since r70.
- 2026-10-24 boot 102 (r78 = +0257, the defaults with sweetbattery=1, gestures, 300 s): **the battery
  reads.** PMI8998 SOC 0xff = 100 %, 4255-4312 mV, -24 mA idle and -115 mA under load (the battery
  supplies the phone while USB is in and charging is inhibited), 35-37 C; USBIN plugged (0x10), charger
  status 0x46 (6: inhibit, full). The first poll ran before SPMI was up ("read failed at step 1", the
  fallback), the next one switched to the gauge. Unplugged and discharging still to be seen on the
  phone itself. xnu 0258: taimencprcl=1, CPRh closed loop.
- 2026-10-24 boot 103 (r81 build = +0258-0260, the defaults + taimencprcl=1, gestures, 300 s): **CPRh closed
  loop**: gold settles at 1012 mV at 2342 MHz (open loop 1052), silver at 888 mV at 1900.8 (928): 40 mV, the
  floor-to-ceiling range Linux allows. Every row checked, ran to its deadline, 41-43 C idle. In the ROOT=1
  defaults. The first try did not boot: boot-args 615 of 607 bytes; the safe list lost sweettouch, sweetusb,
  sweetcharge and sweetbl (dead on taimen), 46 bytes.
- 2026-10-24 boot 104 (r81 = +0260, the defaults (now closed loop) + sweetbl=1, gestures, 300 s): backboardd
  set brightness 113 of 225 and the DCS 0x51 write completed (CMD_DMA_DONE) through the CMD_DMA_TPG FIFO;
  0 DSI resets, 0 kick timeouts, frames as before; closed loop again (gold 1016, silver 892 mV, 44 C).
  Whether the panel visibly dims is for a person to see. sweetbl=1 is in the ROOT=1 defaults.
- 2026-10-24 ~12:30, the user on hand-off boot 5 (r81, all defaults): **"The phone feels pretty responsive, a
  huge improvement from the first boot. The brightness works as well."** Also: "why is this iOS so stripped
  down? It only has 6 apps": the 32 staged stock apps were never moved into /Applications on taimen (xnu 0262,
  mkrootfix --stage-apps; boot 107 with the data migration once).
- 2026-10-24 boot 106 (r83 build = +0261-0262, the defaults + sweetgpuclk=710, gestures, 300 s): **the A540 at
  710 MHz** (corner 8): S1 775 -> 1024 mV read back, MX turbo, GPUPLL0 L 0x49 frac 0xf555, measured 710 MHz,
  API self-test ok; full-screen quad 10.7 ms (14.8 at 300 MHz), the per-frame GPU copy 3.3 ms (3.9-4.0 at
  596). Ran to its deadline, hottest 70.8 C. sweetgpuclk=710 is the default. Hand-off 5 (ended early by
  the user): brightness 79/1/28 from the slider, the battery 98 -> 96 % and the charger restarted on its own
  (chgr 0x46 inhibit -> 0x03 full-on). The user: two hours of hand-off is wasted time; hand-offs stay short.
- 2026-10-24 boot 107 (r83, the defaults + mkrootfix --migrate --stage-apps, 40 min): the data migration ran,
  but all 32 renames came back "already present" (a skip was not logged then). Boot 108 never ran: 0265-0267
  did not build (EIO, then MIN undeclared in TaimenModem.cpp; 0267, 0268 add the includes).
- 2026-10-24 boot 109 (r88 = +0263-0268, the defaults + taimenwifi=3 + the --stage-apps ROOTFIX, 240 s): every
  rename says "not there": the apps are no longer in /private/var/staged_system_apps. There is no data volume
  here ("mount: data volume missing"), so /private/var is the root's own directory: iOS's first boot moved or
  removed them. 0271 adds ROOTFIX op 7 (list a directory into the log) to find them.
  Wi-Fi M0: SMEM v11 (version word 0xb0000), initialized, ptable $TOC v1 with 12 partitions, hwlock 3 free;
  partition 1 is APPS<->2 and 2 is APPS<->3 at 0x1cf000/0x19f000 (0x30000 each, cacheline 32, empty). The log
  stopped there: partition 3's header belongs to two other hosts, is XPU-protected, and reading it hung that
  CPU while iOS ran on (0270: M0 only reads partitions APPS is a host of). The phone came back to fastboot by
  itself after the deadline reset.
- 2026-10-24 boot 110 (r93 = +0263-0273, the defaults + taimenwifi=6 + a ROOTFIX with --list, 240 s):
  **the stock apps are gone from the phone**: /private/var/staged_system_apps is empty, containers/Bundle is
  empty, /Applications has 143 entries (the system's own UI services and the 6 visible apps). iOS's first boot
  removed the staged copies after its install of them failed. The image has them: 32 apps, 3840 files, 5575
  directories, 487 MiB. Too big for the payload (ABL starts none over ~35 MiB), so 0275 adds ROOTFIX op 8 (apply
  an RFX1 blob from a raw partition) and mkrootfix --apps-archive / --apps-part: the apps go to system_b.
  Wi-Fi: **M0-M2 pass.** SMEM ptable (12 entries): APPS<->MPSS is partition 7 at 0xf3000, 0x94000 bytes, empty;
  items 421/428/435/478 absent (the modem is cold); hyp_assign available, PAS_IS_SUPPORTED(modem) = -1 (no PAS:
  the MBA path, as Linux). M1: pool page HLOS -> HLOS+MSS_MSA -> HLOS, both 0, reads and writes fine. M2:
  modem_a FAT16 (16 KB clusters, LUN 4), image/ = mba.mbn (234152 bytes, ELF), modem.mdt (8348 bytes, 29 phdrs,
  24 loadable, 54693 KiB in modem.b00-b27), modemr.jsn, qdsp6m.qdb; no wlanmdsp.mbn or modemuw.jsn there (the
  WLAN PD firmware is elsewhere: vendor). EFS: modemst1/2 (LUN 5), fsg (LUN 4), fsc. M3: proxy votes ok, MBA
  in its carve-out and assigned to MSS_MSA, restart released, branch clocks on, Q6 out of reset (pwr 0x30c0000,
  mem 0x1fffffff, reset 0x4, gfmux 0x102), but **PBL status stayed 0 for 1 s**. Cause found: the mss node's
  bus_clk is clk_gcc_bimc_mss_q6_axi_clk = GCC 0x4401C; M3 had enabled GCC_MSS_Q6_BIMC_AXI_CBCR 0x8A040 instead,
  so the Q6 had no path to DDR for the MBA (0276).
- 2026-10-24 boot 111 (r96 = +0263-0276, the defaults + the apps archive on system_b (op 8) + taimenwifi=7, 360 s):
  **the 32 stock apps are back in /Applications**: 9415 records (5575 mkdir, 3840 zlib files) written in 21 s,
  280 MB read from system_b; /Applications 143 -> 175 entries. Not yet registered with LaunchServices (boot 112).
  **The modem runs.** 0276's clock was it: PBL status 1, MBA status 1 (XPU unlocked); AXI halt ports idle, not
  halted. M4: modem.mdt is relocatable (0x8cc00000..0x94400000, 27 loadable phdrs counting zero-filled ones),
  metadata 980 + 7368 bytes -> META_DATA_AUTH_SUCCESS, 27 segments (121736 KiB with BSS) loaded and announced in
  296 ms, AUTH_COMPLETE; the modem's SMP2P item 435 at +60 ms, slave-kernel READY|HANDOVER at +130 ms. M5: the
  modem had made the GLINK descriptor (478), but its FIFO 480 was never found (rx 0 messages): smem_part_get
  only searched a partition's uncached list (0279 adds the cached list, and a dump of both at +5 s). With no
  rmtfs the modem went FATAL at +35.8 s, as flame predicted (~40 s); item 421 not found (no crash text).
  The reader brought the /vendor WLAN firmware (wlanmdsp.mbn 3723164, bdwlan.bin/.b09 19152, modemuw.jsn:
  wlan_pd instance 180 with kernel/elf_loader, tms/servreg, wlan/fw) into research/taimen/firmware (not git).
- 2026-10-24 boot 112 (r99 = +0263-0279, the defaults + LastBuildInfo.plist removed + taimenwifi=8 + the WLAN
  firmware blobs, payload 21.4 MB, 360 s): **M5 and M6 pass.** The modem's FIFO 480 is in the cached list (0279);
  GLINK VERSION 1 (features 0x7 -> 0x1), the modem opens glink_ssr, IPCRTR, SSM_RTR_MODEM_APPS, later apr_*,
  LOOPBACK_CTL_MPSS, DS, DATA1-4/11; QRTR HELLO from node 0, 40 services (servreg-notif 0x42, ssctl 0x2b, ipa
  0x31, nas, uim, wds, dms, wda, ... and WLFW 0x45 at +1.28 s). Our services: rmtfs opens modem_fs1/fs2/fsg/fsc
  (fsg_oem_1/2 refused, harmless), ALLOC_BUFF 2 MB at 0x8900b000 (to HLOS+MSS_MSA ok), 3 reads; pd-mapper answers
  tms/pddump_disabled (0), tms/pdr_enabled, avs/audio (0), kernel/elf_loader (wlan_pd); tftp serves
  /readonly/firmware/image/wlanmdsp.mbn from the payload blob (blk 7680, wsize 10), /readwrite in RAM,
  /readonly/mbn/mcfg/... not found. **The modem stayed up the whole 200 s watch.** M7: IND_REGISTER ok,
  MSA_INFO ok (2 regions: 0x8920c000+0x4000 secure, 0x89210000+0xfc000; both assigned), MSA_READY ind, CAP ok:
  chip_id 0x30214 family 0x4001, board_id 0xff, soc 0x40010002, fw 0x224a00a1 WLAN.HL.2.2.4-00161-QCAHLSW8998MTPL-1
  (2020-05-20). Then no FW_READY and no pin-connect indication: the WLAN rails were never voted (m7_on was set
  after M3; 0280). The apps: not on the home screen (end screenshot: the same 6), so removing LastBuildInfo.plist
  does not make installd register them; 0280's op 9 removes LaunchServices' database instead.
- 2026-10-24 boot 113 (r100 = +0263-0280, the defaults + LaunchServices database removal + taimenwifi=8, 360 s): the
  WLAN rails were voted before the MBA (L5 0.8 V, L7 1.8 V, L17 1.304 V, L25 3.312 V, all acked), M0-M6 as boot
  112, M7 again to CAP and no further: no pin-connect indication, no FW_READY in 200 s. Android reaches FW_READY
  3.8 s after "QMI Server Connected" (dmesg:3542, 3639). Differences found: (1) Android's WCN3990 supplies L7,
  L17, L25 are the pin-ctrl twins (qcom,enable-with-pin-ctrl <0 4>, init-pin-ctrl-mode 4: enable and HPM follow
  HW_EN2; L5's mode follows EN0), a bare swen vote is probably low-power mode; (2) after CAP icnss sends
  DYNAMIC_FEATURE_MASK (FW_REJUVENATE) and VBATT (icnss_init_vph_monitor). 0282: rails with a 100 mA load, both
  messages. The LaunchServices database was not under /private/var/containers/Data/System (depth 4) nor mobile's
  Library/Caches: 0 removed; boot 114 searches wider.
- 2026-10-24 boot 114 (r102 = +0263-0282, the defaults + a wider LaunchServices search + taimenwifi=10, 420 s):
  **the 32 stock apps are on the home screen.** LaunchServices' database is
  /private/var/mobile/Containers/Data/InternalDaemon/<UUID>/Library/Caches/com.apple.LaunchServices-4035-v2.csstore
  (op 9 removed it; /private/var/containers/Data holds only System); lsd rebuilt it from the app directories and
  the end screenshot shows Calendar, Photos, Mail, Clock, News, Reminders, TV, Podcasts, Maps, Health, Wallet,
  Music next to the old six. So: apps written by op 8 (boot 111) + the database removed once. Later boots must
  not remove it again. Wi-Fi: the rails voted with 100 mA each (all acked), DYNAMIC_FEATURE_MASK ok (previous 0),
  VBATT ok, and still no FW_READY in 260 s; M8/M9 never started. 0283 (MSA 1 MB aligned, PDR listener) next.
- 2026-10-24 boot 115 (r103 = +0263-0283, a plain ROOTFIX + taimenwifi=10, 420 s): the PDR listener works like
  Android's: servreg-notif 0x42:0xb401 at +142 ms, REGISTER_LISTENER for msm/modem/wlan_pd ok (state 0x7fffffff
  = uninit), at +1302 ms wlan_pd UP (0x1fffffff, transaction 1) ACKed, then WLFW appears; the MSA is now 1 MB
  aligned (0x89300000: 16 KB secure + 0xfc000). Still no FW_READY in 260 s. Next: 0284, the board data download
  + CAL_REPORT (cnss-daemon's job on Android, the mainline ath10k order).
- 2026-10-24 boot 116 (r104 = +0263-0284, a plain ROOTFIX + taimenwifi=10, 420 s): **WLAN mission mode, HTC and
  WMI READY.** The board data was the missing step (cnss-daemon's job on Android): after VBATT, bdwlan.bin
  (19152 bytes, board_id 0xff) in 4 BDF_DOWNLOAD segments, CAL_REPORT ok; +3.3 s later the pin-connect result
  (power 0x1, phy io 0, rf 0x1002b), FW_READY at 4.69 s, WLAN_CFG ok, WLAN_MODE mission ok. M8: the anoc2 SMMU
  (sCR0 0x00200626, 14 SMRs, 10 CBs, 4 KB pages x32) took the identity bank for 0x1900/1 (CB4, SMR0, read back
  as written); CE host memory 0x89402000+0xa5000, 12 rings; HTC READY (credits 2 x 2184, 22 endpoints), HTT eid
  1, WMI eid 2, SETUP_COMPLETE_EX; WMI SERVICE_READY (fw 0x224a00a1, 2 rf chains), INIT with no host memory
  chunks, WMI READY (no fw MAC). M9: chan list (37), vdev, passive scan started and completed after 2.6 s with
  0 beacons. Then the modem went FATAL at +14.3 s; crash reason now readable (item 421 is in the cached list):
  "dog_hb.c:266:[dog_hb]:DOG_HB detects starvation of task 0xf9ac5b10, triage wit[h ...]". Topaz: without the
  HTT host rx ring the radio receives but nothing reaches the firmware's rx path (no beacons) — and a starved rx
  task fits the watchdog too. 0285 ports topaz 0125 (HTT version, frag bank, 64-bit rx ring, aggr) before the
  scan. The modem never raised an IPA power query (no "ipa" entry in its SMP2P item).
- 2026-10-24 boot 117 (r105 = +0263-0285, a plain ROOTFIX + taimenwifi=10, 420 s): **the passive scan works.**
  HTT VERSION_REQ -> VERSION_CONF 3.83 in 2 ms, frag bank, the 64-bit rx ring (255 buffers) and aggr cfg
  accepted; the scan (37 channels, all passive) completed in 2.6 s with 47 networks from 61 beacons/probe
  responses (no htt msdus: management comes over WMI). The modem still went FATAL at +14.2 s, 9.5 s after
  mission as in boot 116: DOG_HB starvation again, a different task address. So the rx ring is needed for the
  scan but is not the crash. From 0286 the scan logs each network by channel and RSSI only: no SSIDs or BSSIDs
  of other people's networks in the logs (boot 117's own log in out/ is local and never shared). Boot 118:
  M0-M7 only, to see whether mission mode alone (no CE/HTC/WMI) brings the starvation.
- 2026-10-24 boot 118 (r106 = +0263-0286, a plain ROOTFIX + taimenwifi=8: to mission mode and no further, 420 s):
  the modem still went FATAL at +14.3 s, 9.5 s after mission: DOG_HB starvation (task 0x84cc5ad0). So mission
  mode itself brings it, not our CE/HTC/WMI/scan traffic. Nothing is asked of APPS in those 9.5 s (no QRTR
  lookups, no data to ports we do not serve, no new GLINK opens; mcfg tftp at +9.1 s as in the boots that
  lived). The modem's SMP2P item has smp2p=0, slave-kernel=7, wlan=0 (no "ipa" query). Android has IPA up
  (ipa3 + WDI2 for the WLAN: qcom,ipa-wdi2, an ipa_smmu_wlan stream) long before FW_READY; here IPA is never
  clocked, and a modem task stalled on an unclocked IPA would show as starvation. 0287: the RPM IPA clock
  ("ipa" 0) at 150 MHz before the modem boots.
- 2026-10-24 boot 119 (r107 = +0263-0287, a plain ROOTFIX + taimenwifi=10, 420 s): the IPA core clock voted at
  150 MHz (acked) changed nothing: M0-M9 as boot 117 (scan: 34 networks, 49 beacons/probe responses), FATAL at
  +14.24 s, DOG_HB starvation (task 0xd21c63b0). rmtfs/tftp traffic is the same as in the boots that lived. Two
  targeted guesses (rx ring, IPA clock) have missed, so 0288 stops guessing: it copies icnss's last difference
  (WLAN hardware off at FW_READY, on again before WLAN_CFG/mission) and, after a FATAL, dumps the starved task's
  control block (its address through the modem.mdt phdrs, the MPSS carve-out taken back after halting the modem)
  and the full error text.
- 2026-10-24 boot 120 (r108 = +0263-0288, a plain ROOTFIX + taimenwifi=10, 420 s): the WLAN power cycle at
  FW_READY (rails off, 10 ms, on again, then WLAN_CFG/mission) went through and changed nothing: scan fine (44
  networks), FATAL at +14.21 s, DOG_HB starvation. The crash dump could not place the task address (no phdr
  covers it) and gave nothing actionable; 0289 removes it. Also found: icnss's default quirks are VBATT_DISABLE
  and FW_REJUVENATE_ENABLE, i.e. Android never sends VBATT but does register for the rejuvenate indication.
  0289: no VBATT, rejuvenate_enable in IND_REGISTER, REJUVENATE indications logged and ACKed.
- 2026-10-24 boot 121 (r109 = +0263-0289, a plain ROOTFIX + taimenwifi=10, 420 s): rejuvenate registered, no
  VBATT: no REJUVENATE indication ever came, FATAL at +14.2 s as before (scan 39 networks). Four changes to match
  icnss (IPA clock, power cycle, rejuvenate, no VBATT) did nothing for the starvation. The one standing anomaly
  on our side: the modem opens ten GLINK channels besides IPCRTR (glink_ssr, SSM_RTR_MODEM_APPS, apr_voice_svc,
  apr_apps2, LOOPBACK_CTL_MPSS, DS, DATA1-4, DATA11) and none was ever acknowledged. 0290 acknowledges and opens
  them all, gives them intents and discards what arrives (counted).
- 2026-10-24 boot 122 (r110 = +0263-0290, a plain ROOTFIX + taimenwifi=10, 420 s): all ten extra modem GLINK
  channels acknowledged and open both ways (glink_ssr, SSM_RTR_MODEM_APPS, apr_voice_svc, apr_apps2,
  LOOPBACK_CTL_MPSS, DS, DATA1-4, DATA11); the modem sent nothing on any of them. FATAL at +14.2 s as before
  (scan 43 networks). Ruled out so far for the starvation: the HTT rx ring, the IPA clock, the power cycle at
  FW_READY, rejuvenate/VBATT, the unacknowledged channels. Boot 123 bisects: FW_READY, then the WLAN left off
  as Android does without a WLAN driver (taimenmission=0, 0291), no WLAN_CFG/mission.
- 2026-10-24 boot 123 (r111 = +0263-0291, a plain ROOTFIX + taimenwifi=8 taimenmission=0, 360 s): FW_READY at
  +4.70 s, the WLAN hardware powered off and left off (Android without a WLAN driver), no WLAN_CFG/mission:
  **the modem stayed up the whole 200 s watch.** So the starvation comes from WLAN_CFG or WLAN_MODE, not from
  the BDF or FW_READY. Boot 124: taimenmission=2, WLAN_CFG without WLAN_MODE (0292).
- 2026-10-24 boot 124 (r112 = +0263-0292, a plain ROOTFIX + taimenwifi=8 taimenmission=2, 360 s): FW_READY,
  WLAN_CFG ok, no WLAN_MODE: **the modem lived the whole watch.** So WLAN_MODE mission is the trigger (with or
  without our CE/HTC/WMI afterwards); our request is Android's (mode 0, hw_debug 0). The WCN3990's BT node also
  names S3 (1.352 V io), S5 (2.04 V xtal) and BoB pin1 (3.3-3.6 V) as chip supplies; mainline runs WLAN on
  msm8998 without voting those, so power stays a weaker lead. What mainline ath10k does and icnss does not:
  HOST_CAP with daemon_support 0 (8-bit, its msm8998 quirk) right after IND_REGISTER. Android has cnss-daemon;
  a firmware not told there is none may wait for it in mission mode. 0293 sends it; boot 125 is the full M0-M9.
- 2026-10-24 boot 125 (r113 = +0263-0293, a plain ROOTFIX + taimenwifi=10, 420 s): HOST_CAP (daemon_support 0,
  8-bit) accepted at +1.29 s; FW_READY, mission, scan fine (36 networks); FATAL at +14.21 s as before. Not the
  daemon either. Last physical lead before stepping back: power headroom once the radio is live. 0294 votes BoB
  (L25's parent; bobb 1) at 3.312 V in PWM, and S3 1.352 V / S5 2.04 V (the WCN3990 BT node's io and xtal
  supplies) before the modem boots.
- 2026-10-24 boot 126 (r114 = +0263-0294, a plain ROOTFIX + taimenwifi=10, 420 s): BoB 3.312 V PWM (bobm 3 acked),
  S3 1.352 V, S5 2.04 V voted; FW_READY, mission, scan (35 networks); FATAL at +14.21 s as before.
  **Where Wi-Fi stands (boots 116-126):** everything up to a working passive scan runs (modem, GLINK/QRTR, rmtfs,
  pd-mapper, tftp, PDR listener, WLFW with BDF to FW_READY and mission, SMMU, CE, HTC, WMI READY, HTT, scan), and
  the modem dies on DOG_HB task starvation 9.5 s after WLAN_MODE mission, every time (14.20-14.34 s from modem
  start). Bisected: FW_READY alone (boot 123) and WLAN_CFG alone (boot 124) leave the modem alive; the mission
  mode is the trigger, with or without CE/HTC/WMI afterwards (boot 118). Ruled out: the HTT rx ring, the IPA core
  clock, the power cycle at FW_READY, rejuvenate, VBATT, the unacknowledged GLINK channels, HOST_CAP (no
  daemon), BoB PWM and S3/S5. The 80-byte SFR does not name the task. Stopped guessing here; next steps need a
  known-good reference (e.g. this firmware under a mainline kernel) or a different priority.
- 2026-10-24 boot 127 (r115 = +0263-0295, a plain ROOTFIX + taimenwifi=10, 420 s): host_version "5.2.1.1L" in
  WLAN_CFG (accepted); FW_READY, mission, scan (32 networks); FATAL at +14.21 s as before. Every difference left
  in our messages up to the mission mode has now been matched to icnss (and HOST_CAP to mainline) with no effect.
- 2026-10-24 a lead from mainline: on the Pixel 2 XL (and the OnePlus 5) the WLAN firmware "crashes every 10 seconds
  or so" unless diag-router runs (Caleb Connolly, 2021 Pixel 2 XL DT patch, lkml 2103.0/09882); Bjorn Andersson:
  diag-router makes the modem keep the debug subsystem (QDSS) clock on, and the real fix is RPM_SMD_QDSS_CLK and
  RPM_SMD_RF_CLK2_PIN in the wifi node. Ours: the RPM handoff votes QDSS (clk0 1 Enab) before clock scaling is on,
  and the RPM drops votes from before scaling (the DDR rate vote, sweet_rpm.c). 0296 votes QDSS, RF_CLK2 and
  RF_CLK2_PIN again in both sets with the WLAN rails, after scaling. Also noted for later (postmarketOS msm8998):
  iwd's ControlPortOverNL80211=false "is required to avoid modem firmware crash".
- 2026-10-24 boot 128 (r116 = +0263-0296, a plain ROOTFIX + taimenwifi=10, 420 s): QDSS (clk0 1 Enab), RF_CLK2 and
  RF_CLK2_PIN voted again in both sets after clock scaling (all acked); FW_READY, mission, scan (33 networks);
  FATAL at +14.19 s as before. The clock votes alone are not mainline's fix here.
- 2026-10-24 Ubuntu Touch on taimen: a Halium 9 port (manifest halium-devices/manifests/google_walleye_taimen.xml:
  Flohack74's android_device_google_{wahoo,muskie,taimen} and android_kernel_google_wahoo, TheMuppets vendor
  blobs). Its Wi-Fi is Android's own stack in a container: the downstream kernel (icnss, qcacld) and the vendor
  init services of device/google/wahoo init.hardware.rc (cnss-daemon -n -l, pd-mapper, rmt_storage,
  tftp_server, irsc_util, pm-service/pm-proxy, time_daemon, thermal-engine, wpa_supplicant, init.qcom.wlan.sh,
  among others). No separate recipe to copy: the parts we can reimplement from public protocols (rmtfs, tftp,
  pd-mapper, the BDF download) already are; the rest are closed vendor binaries.
- 2026-10-24 Wi-Fi parked at the scan (the user: "let's get everything else working"). USB networking instead: sweet's
  DWC3 device-mode gadget (USB Ethernet + the live log over UDP) ported to taimen (0297), as CDC-NCM for Windows'
  built-in UsbNcm (0298). Boot 129 (r117 = +0297, sweetusb=2, 240 s): GDSC and all USB clocks on, DWC3 3.00a
  (GSNPSID 0x5533300a) in device mode as ABL left it, DCFG speed 4 (ABL runs SuperSpeed); ue0 192.168.7.2 up;
  soft reset done in 1 ms, then endpoint 0 SETEPCONFIG timed out (1 ms timeout). 0299: GUSB3PIPECTL.SUSPHY
  before the reset (Linux's default; no PIPE clock wait at high speed), 10 ms after it, 50 ms command timeout,
  the PHY registers logged on a timeout.
- 2026-10-24 boot 131 (r119 = +0298-0299, sweetusb=2 NCM): the same endpoint 0 timeout after 50 ms, DEPCMD
  0x401 (still active), DSTS 0x00520000 (halted, link U3). The probe's DSTS before the reset was 0x00d3a474:
  ABL is *connected at SuperSpeed* on this cable/port, and the QUSB2 PHY reads PWR_CTRL1 0x23 (power-down bit
  set): ABL drives the port over the USB3 PHY with the USB2 PHY off, so at high speed there is no UTMI clock.
  0300: stay at SuperSpeed (DCFG speed 4) with USB 3.0 descriptors (ep0 2^9, BOS, endpoint companions, 1024-byte
  bulk), SET_SEL/SET_ISOCH_DELAY accepted, ep0 kept at 512 after connect. sweetusbss=0 forces high speed.
- 2026-10-24 boot 132 (r120 = +0300, SuperSpeed): no endpoint 0 timeout, the gadget runs, but the link stays in
  RX.Detect (DSTS 0x0096032c, link state 5) and the PC sees nothing. The probe's DSTS 0x00d32544 says halted,
  SS.Disabled, last connected at SuperSpeed: ABL stopped its fastboot link and powered its PHYs down on the way
  out. 0301: both PHYs back up before the controller restarts, as Linux brings them up (phy-qcom-qusb2.c
  msm8998: clear POWER_DOWN only; phy-qcom-qmp-usbc.c msm8998 tables when the USB3 PHY is down; aux and pipe
  clocks GCC 0x50000/0x50004), the link state logged on every change.
- 2026-10-24 **boot 133 (r121 = +0301): Windows enumerates the phone** ("Sweet iOS (USB Ethernet)", class Net,
  UsbNcm bound). QUSB2 back up (PWR_CTRL1 0x23 -> 0x22, PLL_STATUS 0x17 ready); ABL left the USB3 PHY running
  (32 table values differ from Linux's; left alone). The SuperSpeed attempt fails (RX.Detect, then SS.Disabled)
  and the core falls back to high speed by itself 83 ms in: bus reset, connect at high speed, address,
  descriptors, SET_CONFIGURATION, NCM data alt 1, GET_NTB_PARAMETERS; ue0 rx 158 / tx 50 frames. The PC had no
  address on the link (169.254, no admin rights to set one). 0302: the gadget answers the PC's DHCP itself
  (192.168.7.1/24, no router, no DNS) and serves the console on TCP 5141 (the PC connects out: no firewall
  rule; tools/windows/usblog.ps1).
- 2026-10-24 **boot 134 (r122 = +0302, 360 s): USB networking works.** DHCP DISCOVER/OFFER/REQUEST/ACK in the
  first second, Windows' adapter 192.168.7.1/24 (Dhcp), ping 192.168.7.2 0-1 ms, the TCP live log 1.6 MB in
  2 minutes, ue0 rx 1943 / tx 4675 frames by the deadline; WSL reaches 192.168.7.2 too. netsvcd
  (tools/phone/netsvcd.c, CI workflow phone-tools.yml, ad hoc signed with the SCDynamicStore write
  entitlement; mkrootfix.py --zfile + --daemon com.sweet.netsvcd) was written by ROOTFIX and exec'd by
  launchd once (never restarted), but its stdout on /dev/console does not reach the kernel log.
  Internet for iOS, two ways: (1) NAT on the PC, admin once: tools/windows/usb-share.ps1 (WinNAT
  192.168.7.0/24); (2) no admin: netsvcd adds an HTTP/HTTPS proxy 127.0.0.1:8080 to the service while the
  PC's agent (tools/taimen/usbproxy.py, WSL) keeps idle connections open to 192.168.7.2:5142, and splices app
  connections onto them; netsvcd status on 5143 (usbproxy.py --status). 0303 logs the first off-link packets
  (proof of the default route). 0304: remote control on TCP 5144 (tools/taimen/phonectl.py tap / swipe /
  unlock / shot OUT.png at 1125x2436 from the last swapped surface).
- 2026-10-24 **boot 135 (r123 = +0263-0304, netsvcd v3, 900 s): iOS is online through the PC, no admin rights.**
  netsvcd published ue0 4 s after the PC agent connected: State:/Network/Global/IPv4 PrimaryInterface ue0,
  router 192.168.7.1, global DNS 1.1.1.1/8.8.8.8 and the global proxy 127.0.0.1:8080 (read back on port 5143).
  Within a minute iOS's daemons went through usbproxy.py: iCloud gateway, iTunes bag, OCSP, Siri, weather,
  maps (gspe21-ssl.ls), configuration.apple.com, mesu, amp-api (Music/TV widgets): 317 requests, 2 failed
  (no DNS for internal Apple names), 73 MB down in 7 minutes. The remote control drove the UI from WSL
  (tools/taimen/phonectl.py): unlock, home screen, Safari, tap Google. **Safari loads google.com** (lock, the
  URL, the page's white theme colour in the status bar; CONNECT www.google.com, gstatic, fonts) **but the
  content area stays black** (as on the Pixel 4 port), two minutes later too; no WebKit crash, the GPU process
  runs (first IOSurface selector 44). The App Store shows its welcome and ads sheets, then "Cannot Connect to
  App Store" (fairplayd is removed from launchd). Screenshots: 11 MB in 10-26 s (~1 MB/s: 0305 polls faster).
