# Windows side of the taimen log reader (tools/taimen/reader/init): wait for its USB drive
# (a removable FAT volume holding reader.txt, label TAIMENLOG) and copy every file to -Dest.
# Called by tools/taimen/autoboot.sh through powershell.exe. Exit 0 = copied, 1 = timeout.
param([Parameter(Mandatory = $true)][string]$Dest, [int]$Timeout = 600)
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
	$disk = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 2' -ErrorAction SilentlyContinue |
		Where-Object { $_.VolumeName -eq 'TAIMENLOG' -or (Test-Path "$($_.DeviceID)\reader.txt") } |
		Select-Object -First 1
	if ($disk) {
		Start-Sleep -Milliseconds 500
		New-Item -ItemType Directory -Force $Dest | Out-Null
		Copy-Item "$($disk.DeviceID)\*" $Dest -Force
		$files = Get-ChildItem $Dest | ForEach-Object { "$($_.Name) $($_.Length)" }
		# Eject (SCSI START STOP UNIT): the gadget closes its LUN, and the reader reboots into
		# fastboot at once instead of after its 45 s wait (tools/taimen/reader/init).
		try { (New-Object -ComObject Shell.Application).Namespace(17).ParseName($disk.DeviceID).InvokeVerb('Eject') } catch {}
		"reader drive $($disk.DeviceID) after $([int]((Get-Date) - $t0).TotalSeconds) s: $($files -join ', ')"
		exit 0
	}
	Start-Sleep -Seconds 2
}
"no reader drive within $Timeout s"
exit 1
