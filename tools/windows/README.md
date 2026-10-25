# Working on the taimen port from a Windows PC

The kernel builds on GitHub's CI (xnu.yml on macOS runners), so the PC only needs to build the
boot shim, run the Python tools, and drive the phone. The layout:

- **WSL2 Ubuntu 24.04** holds the checkout, the shim toolchain, avbtool, gh and Claude Code. All
  `tools/` scripts are bash and run there unchanged.
- **The phone stays on Windows.** `adb.exe` and `fastboot.exe` from Google's platform-tools talk to
  it, with Google's USB driver for the bootloader interface. Don't pass the phone's USB through into
  WSL with usbipd: the phone re-enumerates on every reboot, and the scripts reboot it constantly.
- `tools/windows/wsl-adb` and `wsl-fastboot` are installed as `~/.local/bin/adb` and `fastboot`.
  They call the Windows executables and convert local file arguments to Windows paths, so
  `fastboot boot shim/out/x.img` and `adb pull /dev/... out.txt` work as they do on Linux.
- `tools/taimen/attention.sh` detects WSL and alerts through Windows: a sound, a notification
  balloon and speech.

Keep the checkout in the WSL filesystem (`~/Projects/ios-taimen`), not under `/mnt/c`. Builds are
much faster there and the file modes are kept.

## 1. Windows (elevated PowerShell)

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\setup-windows.ps1        # from a copy of this folder, e.g. the transfer bundle
```

This installs platform-tools in `C:\platform-tools`, Google's USB driver, and WSL2 with Ubuntu
24.04. Reboot if WSL asks, then open **Ubuntu** from the Start menu and create the Linux user.

Put the phone in fastboot mode (hold power and volume-down) and check Device Manager. It must
show **Android Bootloader Interface**. If it shows a yellow "Android" device instead:

1. Right-click it and choose **Update driver**.
2. Choose **Browse my computer**, then **Let me pick**.
3. Choose **Android Device**, then **Android Bootloader Interface**.

## 2. Copy the private files (not in git)

On the old Linux laptop, `tools/windows/make-transfer-bundle.sh` writes
`~/Projects/ios-taimen/transfer/`:

| file | contents |
| --- | --- |
| `ios-taimen-private.tar.zst` | `backup/` (partition dumps, holds the serial/IMEI: keep private), `research/taimen/` (notes, TWRP images, live DT, photos), the small `research/ipsw-x/` parts plus `ios16-root-tail.simg`, `hw/`, `out/kernels/taimen-r22` and `out/taimen-xnu-r22`, the patch-authoring xnu tree (`handoff/xnu-scratch.tar.zst`), and the Claude memory and transcript of the Linux sessions |
| `ios16-root.simg` (optional, 12 GB) | the full iOS root image. Only needed to re-flash userdata from scratch; the root is on the phone already |
| `SHA256SUMS` | checksums of the above |

Copy the folder to the PC with a USB stick, or put it in `C:\transfer` over the network. Never
copy it to a cloud drive or into git. Then, in Ubuntu:

```bash
mkdir -p ~/Projects/ios-taimen && cd ~/Projects/ios-taimen
git clone -b taimen https://github.com/SourceQuality/ios-taimen.git port
(cd /mnt/c/transfer && sha256sum -c SHA256SUMS)
tar --zstd -xf /mnt/c/transfer/ios-taimen-private.tar.zst      # backup/ research/ hw/ out/ handoff/
cp /mnt/c/transfer/ios16-root.simg research/ipsw-x/ 2>/dev/null || true
```

## 3. Ubuntu (WSL)

```bash
cd ~/Projects/ios-taimen/port
tools/windows/setup-wsl.sh     # aarch64 toolchain, avbtool, gh; adb/fastboot wrappers
gh auth login                  # GitHub account SourceQuality, for CI runs and kernel downloads
curl -fsSL https://claude.ai/install.sh | bash      # Claude Code: run it from ~/Projects/ios-taimen/port (CLAUDE.md)
cp ../handoff/CLAUDE.md .      # working rules for Claude (gitignored, stays local)
```

The patch-authoring xnu tree (dxb + xnu-8796.141.3 + patches 0001-0200, one commit per patch):

```bash
cd ~/Projects/ios-taimen && mkdir -p scratch
tar --zstd -xf handoff/xnu-scratch.tar.zst -C scratch     # scratch/xnu, scratch/dxb
```

## 4. Check it end to end

```bash
cd ~/Projects/ios-taimen/port
adb devices; fastboot devices            # the phone appears in one of them
shim/build-taimen.sh ../backup/boot_a.img   # shim builds -> shim/out/taimen-boot.img
tools/taimen/attention.sh "test alert"      # sound, balloon, speech
```

Next step after the move: boot 17, the on-screen log viewer (NOTES-taimen.md, last entries).

## 5. The phone on the network over its USB cable (xnu 0297-0308)

Boot with `sweetusb=2`. The phone's kernel is a USB Ethernet (CDC-NCM) device; Windows binds
its built-in UsbNcm driver ("Sweet iOS (USB Ethernet)" in Device Manager) and gets
**192.168.7.1** from the phone's own DHCP server, so nothing needs admin rights. The phone is
**192.168.7.2**. Everything below connects *out* from the PC, so the Windows firewall needs no
rules.

- **Live kernel log**: TCP 5141. `powershell -ExecutionPolicy Bypass -File tools\windows\usblog.ps1 -Out log.txt`.
- **Internet for iOS**: `tools/taimen/usbproxy.py` in WSL (leave it running). It keeps idle
  connections open to the phone's netsvcd (192.168.7.2:5142), which makes the link iOS's primary
  network with an HTTP/HTTPS proxy and DNS that both go through this agent. Safari, iCloud and the
  system daemons work this way. `usbproxy.py --status` prints netsvcd's state.
  The agent finds a rebooted phone by itself (TCP keepalive).
- **Full internet (every app, not only proxy-aware traffic)**: once, from an elevated PowerShell
  while the phone's adapter is up: `tools\windows\usb-share.ps1` (Windows NAT for
  192.168.7.0/24; `-Off` undoes it). With NAT on, stop `usbproxy.py`, and netsvcd then gives iOS
  1.1.1.1 / 8.8.8.8 and no proxy.
- **Remote control**: TCP 5144, `tools/taimen/phonectl.py`: `shot out.png` (1125x2436, ~1 s),
  `unlock`, `tapc X Y` / `swipec ...` in screenshot coordinates, `type TEXT`, `enter`,
  `key power|volup|voldown|home`, and `run STEP...` for scripts.
