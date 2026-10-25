# Full internet for the phone over its USB link: Windows NAT (WinNAT) for 192.168.7.0/24 out of
# this PC's own connection, so every app works (DNS, App Store, push), not only the HTTP/HTTPS
# that tools/taimen/usbproxy.py carries without admin rights. Run once from an elevated
# PowerShell while the phone's adapter is up (kernel sweetusb=2); the NAT and the forwarding
# setting persist across reboots:
#   powershell -ExecutionPolicy Bypass -File usb-share.ps1          (undo: usb-share.ps1 -Off)
param([switch]$Off)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
	[Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "run this from an elevated PowerShell (Run as administrator)" }
if ($Off) {
	Get-NetNat -Name SweetUSB -ErrorAction SilentlyContinue | Remove-NetNat -Confirm:$false
	"NAT for the phone removed"
	return
}
$ad = Get-NetAdapter | Where-Object { $_.InterfaceDescription -match 'Sweet' } | Select-Object -First 1
if (-not $ad) { throw "the phone's adapter (Sweet iOS USB Ethernet) is not present: boot the phone with sweetusb=2 first" }
$up = Get-NetRoute -DestinationPrefix 0.0.0.0/0 -AddressFamily IPv4 | Where-Object { $_.ifIndex -ne $ad.ifIndex } |
	Sort-Object { $_.RouteMetric + (Get-NetIPInterface -InterfaceIndex $_.ifIndex -AddressFamily IPv4).InterfaceMetric } |
	Select-Object -First 1
if (-not $up) { throw "this PC has no default route of its own" }
Set-NetIPInterface -InterfaceIndex $ad.ifIndex -AddressFamily IPv4 -Forwarding Enabled
Set-NetIPInterface -InterfaceIndex $up.ifIndex -AddressFamily IPv4 -Forwarding Enabled
if (-not (Get-NetNat -Name SweetUSB -ErrorAction SilentlyContinue)) {
	New-NetNat -Name SweetUSB -InternalIPInterfaceAddressPrefix 192.168.7.0/24 | Out-Null
}
"NAT on: the phone (192.168.7.2) reaches the internet through {0} ({1})" -f $up.InterfaceAlias,
	(Get-NetAdapter -InterfaceIndex $up.ifIndex).InterfaceDescription
