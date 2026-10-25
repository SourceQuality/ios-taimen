# Live XNU log from the phone over its USB Ethernet (kernel sweetusb=2, xnu 0302+).
#   powershell -ExecutionPolicy Bypass -File usblog.ps1 [-Out FILE] [-Minutes N]
# The phone's DHCP server gives this PC 192.168.7.1 on the NCM adapter (no admin rights needed);
# the phone is 192.168.7.2 and serves its console on TCP 5141. This side connects out, so the
# Windows firewall needs no rule. Reconnects until -Minutes run out (default 30); appends to -Out.
param([string]$Out = "", [int]$Minutes = 30)
$end = (Get-Date).AddMinutes($Minutes)
$buf = New-Object byte[] 65536
$said = $false
while ((Get-Date) -lt $end) {
	$c = New-Object Net.Sockets.TcpClient
	try {
		$t = $c.ConnectAsync("192.168.7.2", 5141)
		if (-not $t.Wait(2000) -or -not $c.Connected) { throw "no connection" }
		Write-Host ("{0} connected to 192.168.7.2:5141" -f (Get-Date -Format HH:mm:ss))
		$said = $false
		$s = $c.GetStream()
		$s.ReadTimeout = 5000
		while ((Get-Date) -lt $end) {
			try { $n = $s.Read($buf, 0, $buf.Length) } catch [System.IO.IOException] {
				if ($c.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead) -and $c.Client.Available -eq 0) { break }
				continue
			}
			if ($n -le 0) { break }
			$text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
			[Console]::Out.Write($text)
			if ($Out) { [IO.File]::AppendAllText($Out, $text) }
		}
		Write-Host ("{0} disconnected" -f (Get-Date -Format HH:mm:ss))
	} catch {
		if (-not $said) { Write-Host ("{0} waiting for the phone (192.168.7.2:5141)" -f (Get-Date -Format HH:mm:ss)); $said = $true }
		Start-Sleep -Seconds 1
	} finally {
		$c.Close()
	}
}
