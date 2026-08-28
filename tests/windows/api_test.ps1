# REST API regression - runs WITHOUT Administrator.
#
# Everything here exercises the control plane rather than the capture path, so
# it needs no WinDivert driver and no UAC prompt. tests/windows/capture_test.ps1
# is the elevated companion that checks real packets.
#
#   powershell -ExecutionPolicy Bypass -File tests\windows\api_test.ps1
#
# Build first:  MSBuild msvc\clumsy.vcxproj /p:Configuration=Release /p:Platform=x64

$ErrorActionPreference = "Continue"
$repo = Split-Path (Split-Path $PSScriptRoot)
$exe  = Join-Path $repo "bin\msvc\Release\x64\clumsy.exe"
$dir  = Split-Path $exe
$port = 18311

if (-not (Test-Path $exe)) { Write-Host "build clumsy first: $exe not found"; exit 1 }

$script:pass = 0
$script:fail = 0
function Ok($m)      { $script:pass++; Write-Host "  [ok]   $m" }
function Bad($m, $d) { $script:fail++; Write-Host "  [FAIL] $m"; if ($d) { Write-Host "         $d" } }

# Minimal HTTP client: one request per connection, matching the server.
function Http($method, $path, $body) {
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.ReceiveTimeout = 8000; $c.SendTimeout = 8000
    $c.Connect("127.0.0.1", $port); $st = $c.GetStream()
    if ($method -eq "GET") {
      $req = "GET $path HTTP/1.1`r`nHost: x`r`nConnection: close`r`n`r`n"
    } else {
      $bytes = [Text.Encoding]::UTF8.GetBytes($(if ($body) { $body } else { "" }))
      $req = "POST $path HTTP/1.1`r`nHost: x`r`nContent-Length: $($bytes.Length)`r`n" +
             "Content-Type: application/json`r`nConnection: close`r`n`r`n" + $body
    }
    $b = [Text.Encoding]::UTF8.GetBytes($req); $st.Write($b, 0, $b.Length); $st.Flush()
    $sr = New-Object IO.StreamReader($st, [Text.Encoding]::UTF8)
    $o = $sr.ReadToEnd(); $c.Close()
    return $o
  } catch { return "" }
}
function Body($raw) { if ($raw) { ($raw -split "`r`n`r`n", 2)[1] } else { "" } }
function Code($raw) { if ($raw -match '^HTTP/1\.1 (\d+)') { [int]$Matches[1] } else { 0 } }

Write-Host "########## starting clumsy (no elevation needed) ##########"
$p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -WindowStyle Hidden `
     -ArgumentList "--web-port", $port, "--timeout", "120", "--stats-console", "0"
$ready = $false
for ($i = 0; $i -lt 30; $i++) {
  Start-Sleep -Milliseconds 400
  if ((Body (Http GET "/api/health" $null)) -like '*"status":"ok"*') { $ready = $true; break }
}
if (-not $ready) { Write-Host "  clumsy did not come up"; exit 1 }
Ok "server is up on port $port"

Write-Host ""
Write-Host "########## 1. corrupt module is registered (T2) ##########"
$mods = Body (Http GET "/api/modules" $null)
if ($mods -match '"shortName":"corrupt"') { Ok "corrupt appears in /api/modules" }
else { Bad "corrupt module missing" $mods }
if ($mods -match '"corrupt-ber"') { Ok "corrupt exposes its ParamSpec form metadata" }
else { Bad "corrupt-ber spec missing" "" }

$r = Http POST "/api/modules/corrupt" '{"enabled":true,"corrupt-ber":5000,"corrupt-chance":100.0}'
if ((Body $r) -match '"applied":2') { Ok "corrupt parameters applied over REST" }
else { Bad "corrupt setParam" (Body $r) }
$mods = Body (Http GET "/api/modules" $null)
if ($mods -match '"corrupt-ber":"5000"') { Ok "corrupt-ber round-trips through getParams" }
else { Bad "corrupt-ber did not persist" "" }
[void](Http POST "/api/modules/corrupt" '{"enabled":false}')

Write-Host ""
Write-Host "########## 2. jitter distribution enum (T3) ##########"
if ($mods -match '"key":"jitter-dist"' -and $mods -match '"type":"enum"') {
  Ok "jitter-dist is published as an enum"
} else { Bad "jitter-dist enum spec missing" "" }
if ($mods -match '"options":\["uniform","normal","pareto"\]') {
  Ok "enum options list reaches the client"
} else { Bad "enum options missing" "" }
foreach ($d in @("normal", "pareto", "uniform")) {
  [void](Http POST "/api/modules/jitter" ('{"jitter-dist":"' + $d + '"}'))
  $m2 = Body (Http GET "/api/modules" $null)
  if ($m2 -match ('"jitter-dist":"' + $d + '"')) { Ok "jitter-dist = $d accepted" }
  else { Bad "jitter-dist $d not stored" "" }
}
# numeric form kept for scenarios written before the names existed
[void](Http POST "/api/modules/jitter" '{"jitter-dist":"2"}')
if ((Body (Http GET "/api/modules" $null)) -match '"jitter-dist":"pareto"') {
  Ok "numeric distribution index still accepted"
} else { Bad "numeric jitter-dist rejected" "" }
[void](Http POST "/api/modules/jitter" '{"jitter-dist":"uniform"}')

Write-Host ""
Write-Host "########## 3. Prometheus /metrics (T1) ##########"
$raw = Http GET "/metrics" $null
if ((Code $raw) -eq 200) { Ok "/metrics returns 200" } else { Bad "/metrics status" (Code $raw) }
if ($raw -match 'text/plain; version=0\.0\.4') { Ok "Prometheus Content-Type is set" }
else { Bad "wrong Content-Type" "" }
$met = Body $raw
foreach ($m in @("clumsy_up", "clumsy_captured_packets_total", "clumsy_module_enabled",
                 "clumsy_latency_ms_bucket", "clumsy_latency_ms_count")) {
  if ($met -match [regex]::Escape($m)) { Ok "exposes $m" } else { Bad "missing $m" "" }
}
if ($met -match 'clumsy_module_affected_packets_total\{module="corrupt"\}') {
  Ok "per-module label includes the new module"
} else { Bad "corrupt label missing from metrics" "" }
# every HELP must be followed by a TYPE, or promtool rejects the scrape
$helps = ([regex]::Matches($met, '(?m)^# HELP ')).Count
$types = ([regex]::Matches($met, '(?m)^# TYPE ')).Count
if ($helps -eq $types -and $helps -gt 0) { Ok "HELP/TYPE pairs balance ($helps each)" }
else { Bad "HELP/TYPE mismatch" "$helps HELP vs $types TYPE" }
# le buckets must be non-decreasing (cumulative histogram)
$cum = [regex]::Matches($met, 'clumsy_latency_ms_bucket\{le="[^"]+"\} (\d+)') |
       ForEach-Object { [long]$_.Groups[1].Value }
$mono = $true
for ($i = 1; $i -lt $cum.Count; $i++) { if ($cum[$i] -lt $cum[$i-1]) { $mono = $false } }
if ($cum.Count -eq 13 -and $mono) { Ok "13 cumulative histogram buckets, non-decreasing" }
else { Bad "histogram buckets wrong" "count=$($cum.Count) monotonic=$mono" }

Write-Host ""
Write-Host "########## 4. inline apply + profile delete (T5) ##########"
$r = Body (Http POST "/api/apply" '{"lag":true,"lag-time":123,"drop":true,"drop-chance":7.5}')
if ($r -match '"applied":4') { Ok "POST /api/apply set 4 keys at once" }
else { Bad "inline apply" $r }
$mods = Body (Http GET "/api/modules" $null)
if ($mods -match '"lag-time":"123"') { Ok "inline apply reached the module" }
else { Bad "lag-time not applied" "" }
$r = Body (Http POST "/api/apply" '{"lag-time":50,"no-such-key":1}')
if ($r -match '"unknownKeys":"no-such-key"') { Ok "unknown keys are reported back" }
else { Bad "unknownKeys missing" $r }

[void](Http POST "/api/profiles" '{"name":"apitest-profile"}')
if ((Body (Http GET "/api/profiles" $null)) -match 'apitest-profile') { Ok "profile saved" }
else { Bad "profile not saved" "" }
$r = Http POST "/api/profiles/apitest-profile/delete" ""
if ((Code $r) -eq 200) { Ok "profile delete returns 200" } else { Bad "delete status" (Code $r) }
if ((Body (Http GET "/api/profiles" $null)) -notmatch 'apitest-profile') { Ok "profile is gone" }
else { Bad "profile survived the delete" "" }
$r = Http POST "/api/profiles/does-not-exist/delete" ""
if ((Code $r) -eq 404) { Ok "deleting an unknown profile is a 404" }
else { Bad "expected 404" (Code $r) }

Write-Host ""
Write-Host "########## 5. inline scenario load (T4) ##########"
$sc = '[{"at":1,"lag":true,"lag-time":200},{"when":"captured_count","op":">=","value":50,"drop":true}]'
$r = Body (Http POST "/api/scenario/loadinline" (@{ scenario = $sc } | ConvertTo-Json -Compress))
if ($r -match '"steps":2') { Ok "two inline steps parsed" } else { Bad "inline scenario" $r }
if ((Body (Http GET "/api/stats" $null)) -match '"scenario":\{"loaded":true,"active":false,"steps":2') {
  Ok "stats reports the loaded scenario"
} else { Bad "scenario not visible in stats" "" }
$r = Http POST "/api/scenario/loadinline" '{"scenario":"[{\"nothing\":1}]"}'
if ((Code $r) -eq 400) { Ok "a step with no trigger is rejected with 400" }
else { Bad "expected 400 for triggerless step" (Code $r) }
$r = Http POST "/api/scenario/loadinline" '{"wrong":"field"}'
if ((Code $r) -eq 400) { Ok "missing 'scenario' field is a 400" } else { Bad "expected 400" (Code $r) }

Write-Host ""
Write-Host "########## 6. pcap replay control plane (T7) ##########"
$r = Http POST "/api/replay/start" '{"path":"no-such-file.pcap"}'
if ((Code $r) -eq 400 -and (Body $r) -match 'cannot open') {
  Ok "a missing replay file is rejected before the thread starts"
} else { Bad "expected 400 for missing file" ((Code $r).ToString() + " " + (Body $r)) }

# A file that exists but is not a pcap must be refused on its magic number.
$junk = Join-Path $dir "not-a-pcap.bin"
[IO.File]::WriteAllBytes($junk, (1..64 | ForEach-Object { [byte]$_ }))
$r = Http POST "/api/replay/start" ('{"path":"' + ($junk -replace '\\', '\\\\') + '"}')
if ((Code $r) -eq 400 -and (Body $r) -match 'not a classic libpcap') {
  Ok "a non-pcap file is rejected on its magic number"
} else { Bad "expected a magic-number rejection" ((Code $r).ToString() + " " + (Body $r)) }
Remove-Item $junk -ErrorAction SilentlyContinue

# Build a tiny valid LINKTYPE_RAW file: one 40-byte TCP/IPv4 packet.
$pcap = Join-Path $dir "replay-selftest.pcap"
$ms = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter($ms)
$bw.Write([uint32]2712847316)   # 0xa1b2c3d4
$bw.Write([uint16]2); $bw.Write([uint16]4)
$bw.Write([int32]0); $bw.Write([uint32]0)
$bw.Write([uint32]65535); $bw.Write([uint32]101)   # LINKTYPE_RAW
$ip = [byte[]]@(
  0x45,0x00,0x00,0x28, 0x00,0x01,0x00,0x00, 0x40,0x06,0x00,0x00,
  127,0,0,1, 127,0,0,1,
  0x30,0x39, 0x30,0x3A, 0,0,0,0, 0,0,0,0, 0x50,0x02,0x20,0x00, 0,0,0,0)
foreach ($n in 0,1,2) {
  $bw.Write([uint32]1700000000); $bw.Write([uint32](1000 * $n))
  $bw.Write([uint32]$ip.Length); $bw.Write([uint32]$ip.Length)
  $bw.Write($ip)
}
$bw.Flush(); [IO.File]::WriteAllBytes($pcap, $ms.ToArray()); $bw.Close()

$r = Http POST "/api/replay/start" ('{"path":"' + ($pcap -replace '\\', '\\\\') + '","speed":10}')
if ((Code $r) -eq 200) { Ok "a valid RAW pcap is accepted" } else { Bad "replay start" (Body $r) }
Start-Sleep -Seconds 2
$st = Body (Http GET "/api/stats" $null)
if ($st -match '"replay":\{"active":(true|false),"read":(\d+)') {
  $readCount = [int]$Matches[2]
  if ($readCount -eq 3) { Ok "all 3 records were read back from the file" }
  else { Bad "wrong record count" "read=$readCount" }
} else { Bad "replay block missing from stats" $st }
# Injection itself needs the driver, so unelevated it must fail cleanly rather
# than crash: every packet lands in 'failed', and the run still completes.
if ($st -match '"failed":(\d+)') {
  Ok "injection failures are counted, not fatal (failed=$($Matches[1]))"
} else { Bad "failed counter missing" "" }
$r = Http POST "/api/replay/stop" ""
if ((Code $r) -eq 200) { Ok "replay stop returns 200" } else { Bad "replay stop" (Code $r) }
Remove-Item $pcap -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "########## 7. latency block in /api/stats (T8) ##########"
$st = Body (Http GET "/api/stats" $null)
if ($st -match '"latency":\{"count":0') { Ok "latency reports an empty histogram before any delay" }
else { Bad "latency block missing or non-zero at rest" $st }
foreach ($k in @('"p50"', '"p95"', '"p99"', '"mean"', '"max"')) {
  if ($st -match [regex]::Escape($k)) { Ok "latency exposes $k" } else { Bad "missing $k" "" }
}

Write-Host ""
Write-Host "########## 8. docs and regression of existing endpoints ##########"
$docs = Body (Http GET "/api/docs" $null)
foreach ($e in @("/metrics", "/api/apply", "/api/replay/start", "/api/scenario/loadinline",
                 "/api/profiles/{name}/delete")) {
  if ($docs -match [regex]::Escape($e)) { Ok "documented: $e" } else { Bad "undocumented: $e" "" }
}
if ((Code (Http GET "/api/status" $null)) -eq 200) { Ok "GET /api/status still works" }
else { Bad "status regressed" "" }
if ((Code (Http GET "/api/presets" $null)) -eq 200) { Ok "GET /api/presets still works" }
else { Bad "presets regressed" "" }
if ((Code (Http GET "/" $null)) -eq 200) { Ok "dashboard is served" } else { Bad "dashboard 404" "" }
if ((Code (Http GET "/api/report" $null)) -eq 200) { Ok "session report renders" }
else { Bad "report failed" "" }
$rep = Body (Http GET "/api/report" $null)
if ($rep -match "Delay distribution") { Ok "report contains the delay distribution section" }
else { Bad "report missing latency section" "" }

Write-Host ""
Write-Host "########## shutting down ##########"
[void](Http POST "/api/quit" "")
Start-Sleep -Seconds 2
if ($p.HasExited) { Ok "clumsy exited cleanly" }
else { $p | Stop-Process -Force; Bad "had to kill the process" "" }

Write-Host ""
Write-Host "==================================================="
Write-Host "  passed: $script:pass   failed: $script:fail"
Write-Host "==================================================="
if ($script:fail -gt 0) { exit 1 }
