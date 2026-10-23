# ios-taimen

**Bringing Apple's iOS 16 up on the Google Pixel 2 XL (`taimen`, Qualcomm MSM8998 / Snapdragon 835).**

This is a port of [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish) (iOS 16 on the Pixel 1, itself a port of
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)) to the Pixel 2 XL. The plan is the same: a bare-metal shim
started with `fastboot boot` acts as iBoot, Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) runs on the
phone, and the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** runs on top of it. Nothing is flashed to the
boot chain. The Pixel 1 history is kept in this repository; the taimen work starts at "taimen: Pixel 2 XL bring-up notes".

> Experimental bring-up, nowhere near a usable phone OS. **XNU runs on this phone with 4K pages** (first boot
> 2026-10-22: MMU on, VM and pmap bootstrap, screen console, kprintf, then a panic mapping the framebuffer, fixed in
> 0186). No root filesystem yet, so no iOS userland yet.

## Status (2026-10-22)

| Area | State |
| --- | --- |
| Bootloader | Google ABL, unlocked. `fastboot boot` of our images works: v0 header, gzip kernel with the stock DTB appended, AVB hash footer to 40 MiB (the shape TWRP has). Nothing flashed. |
| Shim | `shim/taimen.c` runs on the phone: paints on the ABL splash framebuffer, dumps the display engine, clock and watchdog registers, and PSCI-resets. Stage 2 (`-DSTAGE2`) loads an XNU payload from the boot image's ramdisk through `/chosen` and jumps, with the APSS watchdog armed. |
| Logs | The shim writes its RAM log to UFS (`shim/ufs_taimen.c`, a port of the Pixel 4 shim's driver) into the empty `align_parti` partition; `tools/taimen/log-pull.sh` reads it back over rooted ADB. The kernel's early log goes to the ramoops console zone (0xb0080000); Linux copies it to the alt region on the next Android boot and `/dev/access-ramoops` exposes it: `tools/taimen/xnu-log-pull.sh`. |
| Display | Measured: framebuffer 0x9d400000, 1440x2880, 32 bpp BGRA, one VIG0 pipe, LM0 → CTL0 → PP0 → INTF1/DSI0 (command mode), **autorefresh off**: every frame needs an MDP CTL0 START (0xc90201c). |
| CPU | Kryo 280, ARMv8.0, **no 16K translation granule** (ID_AA64MMFR0_EL1 = 0x1122). The kernel is built with 4K pages, which no Qualcomm port has done before; the Exynos ports (Galaxy S8 / A5) are the reference. |
| Kernel | `TAIMEN` machine config = the sweet/sailfish kernel without 16K pages (patches 0184-0186): compact 39-bit kernel VA, 4K user VA limit, code signing relaxed for the 16K-signed iOS cache, on-screen boot marks. Builds on GitHub Actions (default `machine_config=TAIMEN`), warm runs in about 3.5 minutes. **Boots**: 20 early stages through `kprintf startup complete` on the first attempt; its log comes back through the ramoops zone (`tools/taimen/xnu-log-pull.sh`). |
| Storage, Wi-Fi, GPU, touch | Not started. Taimen uses UFS 2.1 at 0x1da4000, the integrated WCN3990 (the Pixel 4 / topaz Wi-Fi path, not sailfish's PCIe QCA6174), an Adreno 540 (a5xx, sailfish's freedreno path should apply), and an ST FTM4 touch controller. |

Everything measured so far is in [NOTES-taimen.md](NOTES-taimen.md); the address-by-address map from the Pixel 1 code to
taimen is [docs/taimen-porting-map.md](docs/taimen-porting-map.md); the 4K-page work list is
[docs/taimen-4k-catalogue.md](docs/taimen-4k-catalogue.md).

## How it boots (planned)

```
ABL (stock Google bootloader, unlocked)
  -> fastboot boot taimen-boot.img          (temporary; nothing is flashed)
     -> shim (shim/taimen.c): logs to UFS, arms the watchdog, reads /chosen initrd
        -> XNU Mach-O + Apple Device Tree + boot-args (the boot image "ramdisk")
           -> XNU (TAIMEN config, 4K pages) -> UFS driver -> HFS+ root in userdata
              -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

## How to do it yourself

You need a Pixel 2 XL with an **unlocked bootloader** (check `fastboot getvar unlocked`), a Linux PC with an aarch64 GNU
toolchain (`aarch64-linux-gnu-` or the Arm GNU `aarch64-none-elf-` bare-metal toolchain), `avbtool`, `mkbootimg`,
`python3`, and a GitHub account for the kernel build (fork this repository; Actions build XNU on free macOS runners).
Every step below is non-destructive until the root filesystem is written to `userdata`, which erases Android.

1. **Back up the phone.** With rooted ADB (LineageOS: Developer options → Rooted debugging) or TWRP booted from RAM:
   `tools/taimen/backup-partitions.sh ../backup`. Keep the backups out of git (`.gitignore` already does).
   They include the modem calibration and the IMEI: private.
2. **Shim.** `EXTRA_CFLAGS=-DSTAGE2 shim/build-taimen.sh ../backup/boot_a.img PAYLOAD` packs the shim and the payload
   into `shim/out/taimen-boot.img`; `fastboot boot` it. Without a payload the stage-1 diagnostic runs instead.
   Read the log afterwards with `tools/taimen/log-pull.sh`.
3. **Kernel.** Run the `xnu` workflow with `machine_config=TAIMEN`; the artifact is `kernel.development.taimen`.
4. **Payload.** `tools/mkadt.py taimen.adt --taimen`, then
   `tools/mkpayload.py kernel.development.taimen taimen.adt payload --l2block 0x200000 --cmdline "..."`.
5. **iOS root**: unzip the three images from the iPhone X 16.7.16 IPSW (OS root `087-86698-044.dmg`, SystemOS
   cryptex `087-86593-047.dmg`, AppOS cryptex `087-86715-047.dmg`), have `apfs-fuse` on the PATH (built from
   sgan81/apfs-fuse; it decompresses Apple's LZBITMAP files, which 7-Zip cannot), then
   `tools/mkiosroot.sh OS.dmg SYSOS.dmg APPOS.dmg ios16-root.img 16G`. The script needs `sudo` for the loop mount
   and the owner-preserving copy. Convert with `tools/img2simg.py` and `fastboot flash userdata` (erases Android).
   Boot with `rd=ufsroot0` added to the boot-args (`tools/taimen/boot-xnu.sh KERNEL OUTDIR "rd=ufsroot0"`).
   Not yet exercised on this phone.

## Repository layout (taimen additions)

| Path | Contents |
| --- | --- |
| `shim/taimen.c`, `shim/build-taimen.sh` | The taimen shim and its build |
| `shim/ufs_taimen.c`, `shim/log.c` | UFS log channel (polled UFSHCI, WRITE(10) into `align_parti`) and the RAM log |
| `tools/taimen/` | `mkboot.sh` (ABL boot image), `log-pull.sh`, `backup-partitions.sh`, `granule-probe.sh` (+ `granule-probe-fb.S`) |
| `tools/mkadt.py --taimen` | The Apple Device Tree for taimen |
| `xnu/patches/0184-*`, `0185-*` | TAIMEN machine config and the 4K-page layout |
| `NOTES-taimen.md`, `docs/taimen-*.md` | Measurements, porting map, 4K catalogue |
| `.github/workflows/xnu.yml` | Kernel build with cached dependencies and ccache |

The Pixel 1 port's own README is kept as [docs/upstream-sailfish-README.md](docs/upstream-sailfish-README.md).

## Credits

- [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish): the Pixel 1 port this repository is forked from (Qualcomm drivers, GPU through freedreno, Wi-Fi).
- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet): the original iOS 16 port to the Redmi Note 10 Pro.
- [NWylynko/ios-flame](https://github.com/NWylynko/ios-flame): the Pixel 4 port; its shim's UFS driver and RAM log are reused here (MIT).
- [Floppikrob/ios-galaxy-s8](https://github.com/Floppikrob/ios-galaxy-s8) and [orhunnx/ios-a5xelte](https://github.com/orhunnx/ios-a5xelte): the 4 KiB-page XNU work the TAIMEN config follows.
- Apple's open-source releases (xnu, IOHIDFamily, hfs), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [blacktop/ipsw](https://github.com/blacktop/ipsw).

iOS belongs to Apple and is not distributed here: download the IPSW yourself.

## License

The XNU patches are derived from XNU and fall under the [APSL 2.0](https://opensource.apple.com/apsl/). The shim and
tools follow the upstream repositories' licenses (MIT).
