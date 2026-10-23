# Audit: IOFindBSDRoot → UFS → HFS+ → launchd on taimen (2026-10-23)

Read-only review of the root-mount and exec path (patches 0001-0195, 4K pages, RAM above 4 GiB).

**Overall: no hard taimen/4K blocker in this path.** The UFS driver is 64-bit-PA clean, the
controller base comes from the ADT, and the three 4K code-signing relaxations (0185) are the
same three the Galaxy S8 port needed. Ranked by how soon they can bite:

1. **UFS host hand-off preconditions** (`sweet_ufs.c:360-391`): HCE=1, HCS.DP and UTRLRDY,
   UTRLDBR==0, CAP.64AS when the DMA buffer is above 4 GiB, then UTRLBA/U re-pointed and read
   back. Taimen: ver 0x210, cap 0x1587001f (64AS set), hce 1, hcs 0x10f, ABL's UTRL at
   0x9bc39000 → all pass. One taimen-only risk: the shim runs its own UFSHCI driver for the log
   channel before XNU; it must leave UTRLDBR clear and the link up, otherwise EBUSY and the
   "SWEET UFS root ufsroot0 unavailable" panic. If it hits: have the shim wait for its last
   doorbell, or clear UTRLDBR via UTRLCLR instead of refusing.
2. **Root read-only vs the HFS+ journal:** the driver refuses every write unless `ufsrw=1`; the
   Pixel 1 always booted with it. A non-empty journal or a volume-header rewrite during
   `hfs_mountfs` needs device writes. Boot the first root attempt with `ufsrw=1` (it also enables
   the 4 MiB console ring at the end of the named partition, the log channel wanted later).
   Do not use `ufsrw=1` while `userdata` still holds LineageOS.
3. **Watchdog budget:** `taimenreboot=N` stops petting after N s and the WDT resets ~32 s later;
   the mount + launchd + first daemons take longer than 90-120 s. Omit `taimenreboot` (pets
   forever) or use >= 600 for root boots.
4. **4K code signing (0185):** `ubc_subr.c:374-378` accepts cd->pageSize 14 next to 12;
   multilevel-hash conversion is refused so `csb_hash_pageshift` stays 14. `cs_validate_page`
   returns early with ALL_FALSE for every vnode, avoiding the "offset not aligned to cshash
   boundary" panic; so no page is ever validated and `vm_fault_cs_check_violation` re-runs on
   every executable fault (perf only). Maps are not cs-enforced because `cs_enforcement_disable=1`
   plus `debug-enabled` zero both flags, iOS binaries do not carry CS_ENFORCEMENT, and sweet_amfi
   adds only CS_SIGNED|CS_PLATFORM_BINARY. **Keep `cs_enforcement_disable=1` on taimen**; it is
   required here in a way it was not on the 16K Pixel 1. Optional: validate 4 KiB-signed blobs
   for real and only skip blobs with pageshift > PAGE_SHIFT.
5. **dyld shared cache on 4K: clean.** `PAGE_SIZE_FOR_SR_SLIDE` is 4096 everywhere; the pager
   slides in 4 KiB chunks; the shared-region sub-map is forced to 16 KiB entry alignment
   regardless (the x86 configuration); `pmap_nest` needs 2 MiB alignment on 4K and
   `SHARED_REGION_BASE_ARM64` 0x180000000 / size 0x100000000 satisfy it. Only `pmap_trim` is
   skipped.
6. **Mach-O loader on 4K: clean.** `fourk_align` only when PAGE_SHIFT != 12; 16 KiB-aligned iOS
   segments pass the generic masks. No 16K literals in kern_cs.c, kern_exec.c, mach_loader.c,
   vm_fault.c.
7. **First user pmap (launchd):** commpage 0xFFFFFC000 nests the 1 GiB L1 slot on 4K;
   `MACH_VM_MAX_ADDRESS` 0xFC0000000 keeps user mappings out of it. Consistent, but the first
   untested 4K step after the mount.

## UFS driver detail (`bsd/dev/sweet_ufs.c`)

- Controller base from the ADT `/sweet-ufs` reg (0x1da4000/0x2500); `ml_io_map` of three pages,
  all registers used are below 0x64. REG_VER never read; the 2.x UTRD/UCD/PRDT layout is identical
  in 2.1. Link init left to the bootloader.
- DMA above 4 GiB: `su.dma_pa` is 64-bit; UCDBAU, PRDT addr_hi, UTRLBAU carry bits 32+; CAP.64AS
  checked; cache maintenance goes through the 64-bit `phystokv_range` path. The only 32-bit
  truncation (`cpu_cache_dispatch`) is dead. No `(uint32_t)pa` stores.
- Only 32-bit limits are LBAs (READ/WRITE(10) refuse lba > UINT32_MAX); userdata is far below.
- 4K alignment: `kmem_alloc_contig` 4 KiB; UTRL needs 1 KiB, UCD 128 B; PRDT chunks 256 KiB.
- `rd=ufsroot0` → `sweet_ufs_rootdev`: scans the GPT of LUN 0 and picks the entry whose UTF-16
  name equals boot-arg `ufsroot` (default `userdata`); taimen userdata is sda13 = LUN 0, so no
  fixed index. GPT entries must be 128 B within 1 MiB. The HFS image must start at partition
  offset 0 (it does with `fastboot flash userdata`).
- Block size: UFS_BLOCK 4096 hard-coded, no READ CAPACITY; BSD sees 512-byte sectors with RMW.
  Taimen's LUN 0 uses 4 KiB logical blocks (the shim parsed the same GPT in 4K units).
- Console log ring: last 4 MiB + 4 KiB of the named partition, only with `ufsrw=1`; the 16 GiB
  image at the start of the 53 GB partition is far away.

## bsd_init → launchd, platform touches

- Entropy: seed from ADT `random-seed`, DRBG never blocks, `entropy_collect` runs in the SWEET
  GIC path; the taimen gate removed no source it depends on. Only hazard: the "SWEET PRNG entropy
  health test failed" panic, same code as Pixel 1.
- MAC/AMFI/trust cache: sweet_amfi + permissive sandbox, platform-binary by policy, no trust
  cache, root-auth failure downgraded to a printf. launchd needs only `cs_validate_codedirectory`
  to accept the CD plus this policy.
- Root-device publishing: with `rd=ufsroot0` IOFindBSDRoot never touches IOMedia matching;
  waits up to 30 s for the IOBSD resource, ends with `setRootMedia(NULL)`.
- Console for launchd stdout: `/dev/console` → sweet log ring + fbcon; MDP addresses derived
  from the ADT kickoff register. Each write may kick the panel (rate-limited 50 ms).
- No active Pixel 1 hardware code in the path; `sweet_sample_start` threads only act on
  `sweetsample=`/`sweetprof=`.

## Boot-args: who reads them

Read by the kernel: `debug`, `serial`, `cpus`, `keepsyms`, `-noprogress`, `rd`/`rootdev`,
`ufsrw`, `ufsroot`/`ufscache`/`ufswait`, `sweetlog`/`ufslogsecs`, `cs_enforcement_disable`
(needs debug-enabled), `unentitled_ios_sim_launch` (DEVELOPMENT builds only; CI builds
DEVELOPMENT, keep it), `wdt=-1` (XNU's own, unrelated to the APSS WDT; `taimenwdt=0` disables
that one, `taimenreboot=N` bounds it), `sweetfb_noconsole`, `sweetsample`/`sweetprof`,
`sweetnull`, `sweetenv`/`sweetdeny`, driver args not started on taimen.
Userland consumers via `kern.bootargs`: `ignition_level=1` (dyld picks the cryptex cache),
`wdt_test=no_user_checkin` (watchdogd), `launchd_unsecure_cache=1` (launchd). `rootrw=1` has no
reader anywhere (vestigial). `amfi_*` are read only by Apple's AMFI kext.
Recommended first root line: the Pixel 1 line minus sailfish/sweet GPU/WLAN args, plus
`rd=ufsroot0 ufsrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 ignition_level=1
launchd_unsecure_cache=1 wdt_test=no_user_checkin`, with no or a large `taimenreboot`. `-s`
only for single-user. Boot-args cap 608 bytes (mkpayload).

Clean in one line each: hfs-627 is page-size agnostic; 0185's VM layout matches S8 0084/0093/
0097; T0SZ_BOOT 25 for 4K covers the 39-bit user VA.
