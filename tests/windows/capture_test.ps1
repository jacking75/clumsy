# Windows capture regression - requires Administrator (the WinDivert driver).
#
# Launches one elevated clumsy (a single UAC prompt) and drives everything else
# over the REST API from this non-elevated shell.
#
#   powershell -ExecutionPolicy Bypass -File tests\windows\capture_test.ps1
#
# Build first:  MSBuild msvc\clumsy.vcxproj /p:Configuration=Release /p:Platform=x64
#
# Design note: the receive socket is bound in the main runspace *before* anything
# is sent. An earlier version used a Start-Job receiver and measured nothing,
# because the job had not finished binding when the sender fired - the only test
# that saw traffic was the lag case, where clumsy's own 400ms delay accidentally
# gave the job time to come up. Latency is measured with a single packet so the
# inter-packet spacing of a burst cannot smear the number.

$ErrorActionPreference = "Continue"
$repo = Split-Path (Split-Path $PSScriptRoot)
$exe  = Join-Path $repo "bin\msvc\Release\x64\clumsy.exe"
$dir  = Split-Path $exe
$port = 18300
$udp  = 9999
# Narrow filter on the test port only: a broad "udp and outbound" sweeps up
# unrelated system traffic and makes the captured/sent counters meaningless.
$filter = "udp and outbound and udp.DstPort == $udp"

if (-not (Test-Path $exe)) { Write-Host "build clumsy first: $exe not found"; exit 1 }

$script:pass = 0
$script:fail = 0
function Ok($m)  { Write-Host "  PASS  $m"; $script:pass++ }
function Bad($m,$d) { Write-Host "  FAIL  $m  [$d]"; $script:fail++ }

function Http($method, $path, $body) {
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.ReceiveTimeout = 8000; $c.SendTimeout = 8000
    $c.Connect("127.0.0.1", $port)
    $s = $c.GetStream()
    if ($method -eq "GET") {
      $req = "GET $path HTTP/1.1`r`nHost: x`r`nConnection: close`r`n`r`n"
    } else {
      $len = if ($body) { $body.Length } else { 0 }
      $req = "POST $path HTTP/1.1`r`nHost: x`r`nContent-Type: application/json`r`nContent-Length: $len`r`nConnection: close`r`n`r`n$body"
    }
    $b = [Text.Encoding]::ASCII.GetBytes($req)
    $s.Write($b, 0, $b.Length); $s.Flush()
    $sr = New-Object IO.StreamReader($s)
    $o = $sr.ReadToEnd(); $c.Close()
    return ($o -split "`r`n`r`n", 2)[1]
  } catch { return "" }
}

function New-Receiver($timeoutMs) {
  $u = New-Object System.Net.Sockets.UdpClient
  $u.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::Socket,
                            [Net.Sockets.SocketOptionName]::ReuseAddress, $true)
  $u.Client.ReceiveTimeout = $timeoutMs
  $u.Client.ReceiveBufferSize = 1048576
  $u.Client.Bind((New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, $udp)))
  return $u
}

# Send n datagrams, then drain. Returns how many arrived and the first payload.
function Measure-Count($n) {
  $rx = New-Receiver 1500
  $tx = New-Object System.Net.Sockets.UdpClient
  for ($i = 0; $i -lt $n; $i++) {
    $p = [Text.Encoding]::ASCII.GetBytes(("PKT{0:D2}-payload" -f $i))
    [void]$tx.Send($p, $p.Length, "127.0.0.1", $udp)
    Start-Sleep -Milliseconds 15
  }
  $tx.Close()
  $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
  $got = 0; $first = ""
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt 3000) {
    try {
      $d = $rx.Receive([ref]$ep)
      if ($got -eq 0) { $first = [Text.Encoding]::ASCII.GetString($d) }
      $got++
    } catch { break }
  }
  $rx.Close()
  return [pscustomobject]@{ Count = $got; First = $first }
}

# One packet, timed end to end. The receive socket is already bound, so this is
# purely how long clumsy held the packet.
function Measure-Latency {
  $rx = New-Receiver 3000
  $tx = New-Object System.Net.Sockets.UdpClient
  $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
  $p  = [Text.Encoding]::ASCII.GetBytes("LATENCY-PROBE")
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [void]$tx.Send($p, $p.Length, "127.0.0.1", $udp)
  $ms = -1
  try { [void]$rx.Receive([ref]$ep); $ms = $sw.ElapsedMilliseconds } catch { }
  $tx.Close(); $rx.Close()
  return $ms
}

function Configure($mod, $json) { [void](Http POST "/api/modules/$mod" $json); Start-Sleep -Milliseconds 250 }

# ---------------------------------------------------------------------------
Write-Host "########## launching clumsy elevated (approve the UAC prompt) ##########"
# clumsy.exe is launched directly rather than through a .bat wrapper. The wrapper
# only existed so the elevated process could redirect stdout, since -Verb RunAs
# forbids -RedirectStandardOutput. It proved unreliable (script-created .bat
# files disappeared before launch) and everything the log was used for is
# readable over the REST API anyway.
Start-Process -FilePath $exe -WorkingDirectory $dir -Verb RunAs -WindowStyle Hidden `
  -ArgumentList "--web-port", $port, "--timeout", "300", "--stats-console", "0"
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Seconds 1
  if ((Http GET "/api/health" $null) -like '*"status":"ok"*') { $ready = $true; break }
}
if (-not $ready) { Write-Host "  clumsy did not come up"; exit 1 }
$st0 = Http GET "/api/status" $null
if ($st0 -like '*"admin":true*') { Ok "running with Administrator rights" } else { Bad "not elevated" $st0 }

Write-Host ""
Write-Host "########## 1. open the WinDivert driver ##########"
$r = Http POST "/api/filter" ("{""filter"":""" + $filter + """}")
Start-Sleep -Milliseconds 800
if ($r -like '*"status":"ok"*') { Ok "divertStart opened the driver" } else { Bad "capture did not start" $r }
if ((Http GET "/api/status" $null) -like '*"capturing":true*') { Ok "capture is live" } else { Bad "not capturing" "" }
Write-Host "  filter: $filter"

Write-Host ""
Write-Host "########## 2. baseline ##########"
$b = Measure-Count 20
Write-Host "  received $($b.Count)/20, first payload '$($b.First)'"
if ($b.Count -eq 20) { Ok "20/20 delivered untouched" } else { Bad "delivered $($b.Count)/20" "" }
if ($b.First -eq "PKT00-payload") { Ok "payload intact" } else { Bad "payload changed" $b.First }
$stats = Http GET "/api/stats" $null
$cap = if ($stats -match '"captured":(\d+)') { [int]$Matches[1] } else { 0 }
$snt = if ($stats -match '"sent":(\d+)') { [int]$Matches[1] } else { 0 }
Write-Host "  clumsy counters: captured=$cap sent=$snt"
if ($cap -ge 20) { Ok "divertReadLoop captured our traffic ($cap)" } else { Bad "captured=$cap" "expected >=20" }
if ($snt -ge 20) { Ok "sendAllListPackets re-injected ($snt)" } else { Bad "sent=$snt" "expected >=20" }

$baseMs = Measure-Latency
Write-Host "  baseline latency: ${baseMs}ms"
if ($baseMs -ge 0) { Ok "latency probe round-trips" } else { Bad "probe lost" "" }

Write-Host ""
Write-Host "########## 3. drop 100% ##########"
Configure "drop" '{"enabled":true,"drop-chance":100.0}'
$d = Measure-Count 20
Write-Host "  received $($d.Count)/20"
if ($d.Count -eq 0) { Ok "drop 100% delivered 0/20" } else { Bad "delivered $($d.Count)/20" "expected 0" }
Configure "drop" '{"enabled":false}'

Write-Host ""
Write-Host "########## 4. lag 400ms ##########"
Configure "lag" '{"enabled":true,"lag-time":400}'
$lagMs = Measure-Latency
$delta = $lagMs - $baseMs
Write-Host "  latency ${lagMs}ms (baseline ${baseMs}ms, delta ${delta}ms)"
if ($lagMs -ge 0) { Ok "lagged packet still delivered" } else { Bad "packet lost" "" }
if ($delta -ge 300 -and $delta -le 700) { Ok "lag added ${delta}ms (configured 400)" } else { Bad "delta ${delta}ms" "expected 300-700" }
$l = Measure-Count 20
Write-Host "  burst under lag: $($l.Count)/20"
if ($l.Count -eq 20) { Ok "no packets lost while lagging" } else { Bad "delivered $($l.Count)/20" "" }
Configure "lag" '{"enabled":false}'

Write-Host ""
Write-Host "########## 5. tamper ##########"
Configure "tamper" '{"enabled":true,"tamper-chance":100.0,"tamper-checksum":true}'
$t = Measure-Count 20
Write-Host "  received $($t.Count)/20, first payload '$($t.First)'"
if ($t.Count -gt 0) { Ok "tampered packets pass UDP checksum validation ($($t.Count)/20)" } else { Bad "nothing arrived" "checksum wrong?" }
if ($t.First -ne "PKT00-payload" -and $t.First -ne "") { Ok "payload was modified" } else { Bad "payload unchanged" $t.First }
Configure "tamper" '{"enabled":false}'

Write-Host ""
Write-Host "########## 6. duplicate x3 ##########"
Configure "duplicate" '{"enabled":true,"duplicate-chance":100.0,"duplicate-count":3}'
$dp = Measure-Count 10
Write-Host "  received $($dp.Count) (sent 10, expect ~30)"
if ($dp.Count -gt 10) { Ok "duplicate injected extra copies ($($dp.Count))" } else { Bad "got $($dp.Count)" "expected >10" }
Configure "duplicate" '{"enabled":false}'

Write-Host ""
Write-Host "########## 7. bandwidth cap ##########"
# The token bucket starts at max(65535, limit*1024*2) bytes, so a small burst
# passes untouched by design. 120 x 1400B = 164KB clears the 64KB bucket and
# makes the cap observable; a 20-packet test here reported "no throttling" and
# was simply too small.
Configure "bandwidth" '{"enabled":true,"bandwidth-bandwidth":1}'
$rxb = New-Receiver 900
$txb = New-Object System.Net.Sockets.UdpClient
$txb.Client.SendBufferSize = 8388608
$big = New-Object byte[] 1400
for ($i = 0; $i -lt 120; $i++) { [void]$txb.Send($big, 1400, "127.0.0.1", $udp) }
$txb.Close()
$epb = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
$gotb = 0; $swb = [Diagnostics.Stopwatch]::StartNew()
while ($swb.ElapsedMilliseconds -lt 3000) { try { [void]$rxb.Receive([ref]$epb); $gotb++ } catch { } }
$rxb.Close()
$sb = Http GET "/api/stats" $null
$queued = if ($sb -match '"bandwidth":(\d+)') { $Matches[1] } else { "?" }
Write-Host "  1 KB/s: delivered $gotb/120 in 3s, $queued still queued"
if ($gotb -lt 120) { Ok "bandwidth cap throttled delivery ($gotb/120, $queued queued)" }
else { Bad "no throttling" "$gotb/120" }
Configure "bandwidth" '{"enabled":false}'
Start-Sleep -Seconds 1

Write-Host "########## 8. pcap of real captured traffic ##########"
$pcap = "$dir\admintest.pcap"
Remove-Item $pcap -ErrorAction SilentlyContinue
[void](Http POST "/api/pcap/start" '{"path":"admintest.pcap"}')
Start-Sleep -Milliseconds 300
[void](Measure-Count 15)
[void](Http POST "/api/pcap/stop" "")
Start-Sleep -Milliseconds 600
if (Test-Path $pcap) {
  $bytes = [IO.File]::ReadAllBytes($pcap)
  $magic = [BitConverter]::ToUInt32($bytes, 0)
  $link  = [BitConverter]::ToUInt32($bytes, 20)
  $snap  = [BitConverter]::ToUInt32($bytes, 16)
  Write-Host ("  {0} bytes, magic=0x{1:x8}, snaplen={2}, linktype={3}" -f $bytes.Length, $magic, $snap, $link)
  if ($bytes.Length -gt 24) { Ok "contains real packet records" } else { Bad "header only" "$($bytes.Length)B" }
  # 0xa1b2c3d4 as a PowerShell literal is Int32 (negative); compare as UInt32.
  if ($magic -eq [uint32]2712847316) { Ok "pcap magic 0xa1b2c3d4" } else { Bad "bad magic" $magic }
  if ($link -eq 101) { Ok "linktype 101 (LINKTYPE_RAW)" } else { Bad "linktype $link" "expected 101" }
} else { Bad "pcap not created" "" }

Write-Host ""
Write-Host "########## 9. clean stop ##########"
$r = Http POST "/api/stop" ""
if ($r -like '*"status":"ok"*') { Ok "capture stopped" } else { Bad "stop failed" $r }
if ((Http GET "/api/status" $null) -like '*"capturing":false*') { Ok "state back to idle" } else { Bad "still capturing" "" }
[void](Http POST "/api/quit" "")
Start-Sleep -Seconds 3
if ((Get-Process clumsy -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0) { Ok "process exited" } else { Bad "still running" "" }
if ((Http GET "/api/health" $null) -eq "") { Ok "web server shut down with the process" } else { Bad "server still answering" "" }

Write-Host ""
Write-Host "########## RESULT: $script:pass passed, $script:fail failed ##########"

