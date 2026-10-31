# ios-taimen

**Bringing Apple's iOS 16 up on the Google Pixel 2 XL (`taimen`, Qualcomm MSM8998 / Snapdragon 835).**

This is a port of [joranv1/ios-sailfish](https://github.com/joranv1/ios-sailfish) (iOS 16 on the Pixel 1, itself a port of
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)) to the Pixel 2 XL. The plan is the same: a bare-metal shim
started with `fastboot boot` acts as iBoot, Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) runs on the
phone, and the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** runs on top of it. Nothing is flashed to the
boot chain. The Pixel 1 history is kept in this repository; the taimen work starts at "taimen: Pixel 2 XL bring-up notes".

> Experimental, and not a phone you would carry. iOS 16 fills the Pixel 2 XL's screen with all eight CPU cores running
> and the Adreno 540 drawing it. Touch, the buttons, both speakers, the microphones and the motion sensors work, and
> most of the stock apps open: Maps renders its map with Metal, searches, and shows Look Around. There is no Wi-Fi,
> Bluetooth, camera, cellular or GPS yet. The internet comes over the USB cable, through a proxy that runs on the PC.

## Status (2026-10-31)

| Area | State |
| --- | --- |
| Bootloader | Google ABL, unlocked. `fastboot boot` of our images works: v0 header, gzip kernel with the stock DTB appended, AVB hash footer to 40 MiB (the shape TWRP has). The iOS root is on userdata; the boot slots hold the log reader. |
| Shim | `shim/taimen.c`: stage 2 (`-DSTAGE2`) loads the XNU payload from the boot image's ramdisk through `/chosen` and jumps, with the APSS watchdog armed; it also logs a DSI/pingpong register dump to UFS (`align_parti`). |
| Logs, unattended boots | `tools/taimen/autoboot.sh` runs fastboot -> XNU -> log reader -> fastboot with nobody at the phone. Once iOS is up, the kernel streams its log to the PC over USB networking, and `tools/taimen/phonectl.py` taps, swipes, types, takes screenshots and fetches any IOSurface's bytes the same way. After XNU's PS_HOLD reset, the reader (`tools/taimen/mkreader.sh`: TWRP's kernel + a busybox initramfs, on both boot slots) hands pstore and the kernel's 4 MiB UFS console ring to Windows as a USB drive and reboots into fastboot. A kernel panic resets the same way (`debug=0x10000c`). |
| Display | The whole height of the 1440x2880 command-mode DSC panel. The GPU copies each frame into a scan buffer, and the MDP's QSEED3 scaler stretches the 1125x2436 iPhone X canvas to 1330x2880 (`taimenhwscale=3`, double-buffered). The brightness slider works. The kernel sends nothing before backboardd's first frame: frames sent during the early boot starved the DSI lanes (HS FIFO underflow) and hung its command engine (0206-0212). |
| CPU | Kryo 280, ARMv8.0, **no 16K translation granule**: the kernel is built with 4K pages. All eight cores run, the gold cluster at up to 2.36 GHz. |
| GPU | Adreno 540, with Mesa's freedreno in user space submitting through the kernel's SweetGPU. backboardd and SpringBoard composite with OpenGL ES (`tools/sweetgl`). Apps that ask for a Metal device get SweetMetal (`tools/sweetmtl`), which translates their Metal shaders with metal2vulkan and runs them on Gallium; Maps draws its map, its place cards and Look Around (with its occlusion-tested 3D labels) this way. A GPU access outside its mappings is terminated alone (the SMMU's HUPCF), so one bad read no longer stalls the GPU, and a hung GPU is reset without a reboot. |
| Kernel | `TAIMEN` machine config = the sweet/sailfish kernel without 16K pages; patches up to `xnu/patches/0375`, built on GitHub Actions (macos-15, Xcode 16) in 10-15 minutes (`tools/windows/ci-kernel.sh`). Taimen findings on the way: GICD_CTLR.ARE_NS off at handoff (0191), CNTP/ICC_HPPIR0 reads trap (0192), PSCI calls from EL1 do nothing, PS_HOLD resets (0200), the DSI underflow (0207, 0212). |
| Root | HFS+ (XNU's open-source tree has HFS+, not APFS). `tools/taimen/mkrootfix.py` + xnu 0210-0215 patch it at every boot without reflashing (the Pixel 1 port's userland tweaks: no CommCenter/fairplayd/diagnostics jobs, Setup skipped, ...; Location Services on). |
| Touch, buttons | ST FTM4 on BLSP1 QUP5 (`bsd/dev/taimen_touch.c`, 0209/0211): rails through the RPM over GLINK, I2C with repeated start, events to SweetHID mapped onto the canvas ([docs/taimen-touch-plan.md](docs/taimen-touch-plan.md)). The power and volume keys come from the PMIC. |
| Audio | Both TAS2557 speaker amplifiers play through the ADSP. The microphones reach iOS through the WCD9340 codec over SLIMbus, with the ADSP as bus manager: Voice Memos records and plays back ([docs/taimen-mic-plan.md](docs/taimen-mic-plan.md)). |
| Sensors | Accelerometer, gyroscope, magnetometer, barometer, ambient light and proximity, read from the SLPI sensor hub over QMI. The screen rotates, the Compass app points the right way, and CoreMotion gets device motion from a fusion in the kernel ([docs/taimen-compass-notes.md](docs/taimen-compass-notes.md)). |
| Network | USB only: a CDC-NCM gadget to the PC, where `tools/taimen/usbproxy.py` (no admin rights needed) is iOS's HTTP proxy to the internet. Safari, video playback and Maps' search go through it. |
| Apps | Everything on the first home screen page opens and draws; FaceTime stops at its Apple ID sign-in. Weather and the App Store open but get no data: their requests need a FairPlay (Mescal) signature, and FairPlay finds no Apple hardware identity on this phone. |
| Not working | Wi-Fi (the WCN3990 firmware loads on the modem, which never reports it ready; parked), Bluetooth, cameras, cellular, GPS. Location Services are on but have no position source. |

Everything measured so far is in [NOTES-taimen.md](NOTES-taimen.md); the address-by-address map from the Pixel 1 code to
taimen is [docs/taimen-porting-map.md](docs/taimen-porting-map.md); the 4K-page work list is
[docs/taimen-4k-catalogue.md](docs/taimen-4k-catalogue.md).

## How it boots

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
| `tools/taimen/autoboot.sh`, `boot-xnu.sh`, `phonectl.py`, `usbproxy.py` | Unattended boots, remote control and the PC end of the phone's internet |
| `tools/sweetgl/`, `tools/sweetmtl/` | OpenGL ES for backboardd and SpringBoard; SweetMetal, the Metal device for apps |
| `tools/phone/` | Phone-side helpers: `netsvcd` (USB networking), `oslogd` (os_log into the kernel log), the RedwoodAudio HAL plug-in |
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
