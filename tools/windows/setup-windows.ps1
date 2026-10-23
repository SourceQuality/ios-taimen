# Windows side of the ios-taimen setup (see tools/windows/README.md).
# Run once in an elevated PowerShell:
#   Set-ExecutionPolicy -Scope Process Bypass; .\setup-windows.ps1
# Installs: WSL2 + Ubuntu 24.04, Google platform-tools (adb/fastboot) in
# C:\platform-tools, Google's USB driver (fastboot/bootloader interface).
$ErrorActionPreference = 'Stop'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	throw 'Run this from an elevated (Administrator) PowerShell.'
}
$tmp = Join-Path $env:TEMP 'ios-taimen-setup'
New-Item -ItemType Directory -Force $tmp | Out-Null

Write-Host '== 1/3 platform-tools -> C:\platform-tools'
Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile "$tmp\pt.zip"
if (Test-Path C:\platform-tools) { Remove-Item -Recurse -Force C:\platform-tools }
Expand-Archive "$tmp\pt.zip" C:\   # the zip holds a top-level platform-tools\ folder
$p = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($p -notlike '*C:\platform-tools*') { [Environment]::SetEnvironmentVariable('Path', "$p;C:\platform-tools", 'User') }
& C:\platform-tools\adb.exe version | Select-Object -First 2

Write-Host '== 2/3 Google USB driver (adb + fastboot interfaces)'
Invoke-WebRequest 'https://dl.google.com/android/repository/usb_driver_r13-windows.zip' -OutFile "$tmp\usb.zip"
Expand-Archive -Force "$tmp\usb.zip" $tmp
pnputil /add-driver "$tmp\usb_driver\android_winusb.inf" /install

Write-Host '== 3/3 WSL2 + Ubuntu 24.04'
$have = (wsl.exe -l -q 2>$null) -replace "`0", '' | Where-Object { $_ -match 'Ubuntu' }
if ($have) { Write-Host "already installed: $have" } else { wsl.exe --install -d Ubuntu-24.04 }

Write-Host @'

Done. Next (also in tools/windows/README.md):
  1. Reboot if WSL asked for it, open "Ubuntu" from the Start menu, create the Linux user.
  2. Plug the phone in, open Device Manager while it is in fastboot mode: it must show
     "Android Bootloader Interface". If it shows "Android" with a yellow mark, right-click,
     Update driver, Browse, Let me pick, Android Device, Android Bootloader Interface.
  3. In Ubuntu: run tools/windows/setup-wsl.sh (README step 3).
'@
