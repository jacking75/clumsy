#!/usr/bin/env bash
# REST API regression - no CAP_NET_ADMIN needed.
#
# Exercises the control plane, which is entirely shared code: the same
# controlapi.cpp, latency.cpp, scenario.cpp, profile.cpp and pcapreplay.cpp
# back the Windows build, so a pass here covers both platforms for everything
# except the capture and injection backends themselves. Those need the driver
# (tests/windows/capture_test.ps1) or an NFQUEUE (tests/linux/nfqtest.cpp).
#
#   make && tests/linux/api_test.sh

set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$REPO/bin/linux/clumsy"
PORT=18312
BASE="http://127.0.0.1:$PORT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$EXE" ] || { echo "build clumsy first: $EXE not found"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  [ok]   $1"; }
bad() { fail=$((fail+1)); echo "  [FAIL] $1"; [ -n "${2:-}" ] && echo "         $2"; }

get()  { curl -sS --max-time 8 "$BASE$1" 2>/dev/null; }
code() { curl -sS --max-time 8 -o /dev/null -w '%{http_code}' "$BASE$1" 2>/dev/null; }
post() { curl -sS --max-time 8 -X POST -H 'Content-Type: application/json' \
              --data-binary "${2:-}" "$BASE$1" 2>/dev/null; }
pcode() { curl -sS --max-time 8 -o /dev/null -w '%{http_code}' -X POST \
               -H 'Content-Type: application/json' --data-binary "${2:-}" "$BASE$1" 2>/dev/null; }
has() { case "$1" in *"$2"*) return 0;; *) return 1;; esac; }

echo "########## starting clumsy ##########"
cd "$(dirname "$EXE")"
./clumsy --web-port "$PORT" --timeout 120 --stats-console 0 >"$TMP/log" 2>&1 &
CLUMSY_PID=$!
ready=0
for _ in $(seq 1 30); do
  sleep 0.4
  if has "$(get /api/health)" '"status":"ok"'; then ready=1; break; fi
done
[ "$ready" = 1 ] || { echo "  clumsy did not come up"; cat "$TMP/log"; kill $CLUMSY_PID 2>/dev/null; exit 1; }
ok "server is up on port $PORT"

echo
echo "########## 1. corrupt module (T2) ##########"
MODS="$(get /api/modules)"
has "$MODS" '"shortName":"corrupt"' && ok "corrupt appears in /api/modules" || bad "corrupt module missing"
has "$MODS" '"corrupt-ber"'         && ok "corrupt publishes its ParamSpec form" || bad "corrupt-ber spec missing"
R="$(post /api/modules/corrupt '{"enabled":true,"corrupt-ber":5000,"corrupt-chance":100.0}')"
has "$R" '"applied":2' && ok "corrupt parameters applied over REST" || bad "corrupt setParam" "$R"
has "$(get /api/modules)" '"corrupt-ber":"5000"' \
  && ok "corrupt-ber round-trips through getParams" || bad "corrupt-ber did not persist"
post /api/modules/corrupt '{"enabled":false}' >/dev/null

echo
echo "########## 2. jitter distribution enum (T3) ##########"
has "$MODS" '"key":"jitter-dist"' && ok "jitter-dist is published" || bad "jitter-dist spec missing"
has "$MODS" '"options":["uniform","normal","pareto"]' \
  && ok "enum options list reaches the client" || bad "enum options missing"
for d in normal pareto uniform; do
  post /api/modules/jitter "{\"jitter-dist\":\"$d\"}" >/dev/null
  has "$(get /api/modules)" "\"jitter-dist\":\"$d\"" \
    && ok "jitter-dist = $d accepted" || bad "jitter-dist $d not stored"
done
post /api/modules/jitter '{"jitter-dist":"2"}' >/dev/null
has "$(get /api/modules)" '"jitter-dist":"pareto"' \
  && ok "numeric distribution index still accepted" || bad "numeric jitter-dist rejected"
post /api/modules/jitter '{"jitter-dist":"uniform"}' >/dev/null

echo
echo "########## 3. Prometheus /metrics (T1) ##########"
[ "$(code /metrics)" = 200 ] && ok "/metrics returns 200" || bad "/metrics status" "$(code /metrics)"
CT="$(curl -sS --max-time 8 -o /dev/null -w '%{content_type}' "$BASE/metrics")"
has "$CT" 'version=0.0.4' && ok "Prometheus Content-Type is set" || bad "wrong Content-Type" "$CT"
MET="$(get /metrics)"
for m in clumsy_up clumsy_captured_packets_total clumsy_module_enabled \
         clumsy_latency_ms_bucket clumsy_latency_ms_count; do
  has "$MET" "$m" && ok "exposes $m" || bad "missing $m"
done
has "$MET" 'clumsy_module_affected_packets_total{module="corrupt"}' \
  && ok "per-module label includes the new module" || bad "corrupt label missing"
H="$(printf '%s\n' "$MET" | grep -c '^# HELP ')"
T="$(printf '%s\n' "$MET" | grep -c '^# TYPE ')"
[ "$H" = "$T" ] && [ "$H" -gt 0 ] && ok "HELP/TYPE pairs balance ($H each)" \
  || bad "HELP/TYPE mismatch" "$H HELP vs $T TYPE"
# cumulative buckets must never decrease
BUCKETS="$(printf '%s\n' "$MET" | sed -n 's/^clumsy_latency_ms_bucket{le="[^"]*"} \([0-9]*\)$/\1/p')"
NB="$(printf '%s\n' "$BUCKETS" | grep -c .)"
MONO=1; PREV=-1
for v in $BUCKETS; do [ "$v" -lt "$PREV" ] && MONO=0; PREV="$v"; done
[ "$NB" = 13 ] && [ "$MONO" = 1 ] && ok "13 cumulative buckets, non-decreasing" \
  || bad "histogram buckets wrong" "count=$NB monotonic=$MONO"

echo
echo "########## 4. inline apply + profile delete (T5) ##########"
R="$(post /api/apply '{"lag":true,"lag-time":123,"drop":true,"drop-chance":7.5}')"
has "$R" '"applied":4' && ok "POST /api/apply set 4 keys at once" || bad "inline apply" "$R"
has "$(get /api/modules)" '"lag-time":"123"' \
  && ok "inline apply reached the module" || bad "lag-time not applied"
R="$(post /api/apply '{"lag-time":50,"no-such-key":1}')"
has "$R" '"unknownKeys":"no-such-key"' && ok "unknown keys are reported back" || bad "unknownKeys missing" "$R"

post /api/profiles '{"name":"apitest-profile"}' >/dev/null
has "$(get /api/profiles)" 'apitest-profile' && ok "profile saved" || bad "profile not saved"
[ "$(pcode /api/profiles/apitest-profile/delete)" = 200 ] \
  && ok "profile delete returns 200" || bad "delete status"
has "$(get /api/profiles)" 'apitest-profile' \
  && bad "profile survived the delete" || ok "profile is gone"
[ "$(pcode /api/profiles/does-not-exist/delete)" = 404 ] \
  && ok "deleting an unknown profile is a 404" || bad "expected 404"

echo
echo "########## 5. inline scenario load (T4) ##########"
SC='{"scenario":"[{\"at\":1,\"lag\":true,\"lag-time\":200},{\"when\":\"captured_count\",\"op\":\">=\",\"value\":50,\"drop\":true}]"}'
R="$(post /api/scenario/loadinline "$SC")"
has "$R" '"steps":2' && ok "two inline steps parsed" || bad "inline scenario" "$R"
has "$(get /api/stats)" '"scenario":{"loaded":true,"active":false,"steps":2}' \
  && ok "stats reports the loaded scenario" || bad "scenario not visible in stats"
[ "$(pcode /api/scenario/loadinline '{"scenario":"[{\"nothing\":1}]"}')" = 400 ] \
  && ok "a step with no trigger is rejected with 400" || bad "expected 400"
[ "$(pcode /api/scenario/loadinline '{"wrong":"field"}')" = 400 ] \
  && ok "missing 'scenario' field is a 400" || bad "expected 400"

echo
echo "########## 6. pcap replay (T7) ##########"
[ "$(pcode /api/replay/start '{"path":"no-such-file.pcap"}')" = 400 ] \
  && ok "a missing replay file is rejected up front" || bad "expected 400 for missing file"
head -c 64 /dev/urandom > "$TMP/junk.bin"
R="$(post /api/replay/start "{\"path\":\"$TMP/junk.bin\"}")"
has "$R" 'not a classic libpcap' \
  && ok "a non-pcap file is rejected on its magic number" || bad "magic check" "$R"

# One valid LINKTYPE_RAW file with three IPv4/TCP records, 1ms apart.
python3 - "$TMP/replay.pcap" <<'PY'
import struct, sys
ip = bytes([0x45,0,0,0x28, 0,1,0,0, 0x40,6,0,0, 127,0,0,1, 127,0,0,1,
            0x30,0x39, 0x30,0x3A, 0,0,0,0, 0,0,0,0, 0x50,0x02,0x20,0x00, 0,0,0,0])
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 101))
    for n in range(3):
        f.write(struct.pack('<IIII', 1700000000, 1000*n, len(ip), len(ip)))
        f.write(ip)
PY
R="$(post /api/replay/start "{\"path\":\"$TMP/replay.pcap\",\"speed\":10}")"
has "$R" '"status":"ok"' && ok "a valid RAW pcap is accepted" || bad "replay start" "$R"
sleep 2
ST="$(get /api/stats)"
READ="$(printf '%s' "$ST" | sed -n 's/.*"replay":{"active":[a-z]*,"read":\([0-9]*\).*/\1/p')"
[ "$READ" = 3 ] && ok "all 3 records were read back" || bad "wrong record count" "read=$READ"
# Injection needs CAP_NET_RAW; without it every packet must fail cleanly.
has "$ST" '"failed":' && ok "injection failures are counted, not fatal" || bad "failed counter missing"
[ "$(pcode /api/replay/stop)" = 200 ] && ok "replay stop returns 200" || bad "replay stop"

# An Ethernet-framed file must have its 14-byte header stripped rather than
# being injected as-is.
python3 - "$TMP/eth.pcap" <<'PY'
import struct, sys
eth = bytes(12) + b'\x08\x00'
ip = bytes([0x45,0,0,0x28, 0,1,0,0, 0x40,6,0,0, 127,0,0,1, 127,0,0,1,
            0x30,0x39, 0x30,0x3A, 0,0,0,0, 0,0,0,0, 0x50,0x02,0x20,0x00, 0,0,0,0])
arp = bytes(12) + b'\x08\x06' + bytes(28)   # must be skipped, not injected
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    for payload in (eth + ip, arp, eth + ip):
        f.write(struct.pack('<IIII', 1700000000, 0, len(payload), len(payload)))
        f.write(payload)
PY
R="$(post /api/replay/start "{\"path\":\"$TMP/eth.pcap\",\"speed\":50}")"
has "$R" '"status":"ok"' && ok "an Ethernet-framed pcap is accepted" || bad "eth replay start" "$R"
sleep 1.5
ST="$(get /api/stats)"
READ="$(printf '%s' "$ST" | sed -n 's/.*"replay":{"active":[a-z]*,"read":\([0-9]*\).*/\1/p')"
[ "$READ" = 3 ] && ok "all 3 Ethernet records were read" || bad "eth record count" "read=$READ"
post /api/replay/stop '' >/dev/null

echo
echo "########## 7. latency block in /api/stats (T8) ##########"
ST="$(get /api/stats)"
has "$ST" '"latency":{"count":0' && ok "latency starts as an empty histogram" \
  || bad "latency block missing or non-zero at rest" "$ST"
for k in '"p50"' '"p95"' '"p99"' '"mean"' '"max"'; do
  has "$ST" "$k" && ok "latency exposes $k" || bad "missing $k"
done

echo
echo "########## 8. docs and existing endpoints ##########"
DOCS="$(get /api/docs)"
for e in /metrics /api/apply /api/replay/start /api/scenario/loadinline '/api/profiles/{name}/delete'; do
  has "$DOCS" "$e" && ok "documented: $e" || bad "undocumented: $e"
done
[ "$(code /api/status)"  = 200 ] && ok "GET /api/status still works"  || bad "status regressed"
[ "$(code /api/presets)" = 200 ] && ok "GET /api/presets still works" || bad "presets regressed"
[ "$(code /)"            = 200 ] && ok "dashboard is served"          || bad "dashboard 404"
[ "$(code /api/report)"  = 200 ] && ok "session report renders"       || bad "report failed"
has "$(get /api/report)" 'Delay distribution' \
  && ok "report contains the delay distribution section" || bad "report missing latency section"

echo
echo "########## 9. replies a script or an AI agent can act on ##########"
# A wrong path or method must still answer in JSON: a client that parses every
# reply would otherwise fail on the one reply that tells it what it got wrong.
R="$(get /api/stop)"
[ "$(code /api/stop)" = 404 ] && has "$R" '"status":"error"' && has "$R" 'POST' \
  && ok "a GET on a POST-only endpoint is a JSON error that says so" \
  || bad "unknown endpoint reply is not actionable JSON" "$R"

# Out of range: applied, but the reply has to show the value that is in effect.
R="$(post /api/modules/lag '{"lag-time":99999}')"
has "$R" '"warnings":[' && has "$R" '"lag-time":"15000"' \
  && ok "a clamped value is reported, with the value actually in effect" \
  || bad "clamping was silent" "$R"
# Unusable: must not be coerced into some other setting and reported as ok.
post /api/modules/jitter '{"jitter-dist":"normal"}' >/dev/null
R="$(post /api/modules/jitter '{"jitter-dist":"gaussian"}')"
[ "$(pcode /api/modules/jitter '{"jitter-dist":"gaussian"}')" = 400 ] \
  && has "$R" 'uniform,normal,pareto' && has "$(get /api/modules)" '"jitter-dist":"normal"' \
  && ok "an unknown enum name is refused, with the choices, and the old value stays" \
  || bad "bad enum value was applied or not reported" "$R"
post /api/modules/jitter '{"jitter-dist":"uniform"}' >/dev/null
post /api/modules/lag '{"lag-time":50}' >/dev/null
# A request made only of names no module has changed nothing, and "ok" with
# "applied":0 reads as success to anything that checks the status alone.
R="$(post /api/apply '{"latency":150,"loss":5}')"
[ "$(pcode /api/apply '{"latency":150,"loss":5}')" = 400 ] && has "$R" 'latency' \
  && ok "a request that applied nothing is a 400 naming the keys" \
  || bad "an all-unknown request was reported as ok" "$R"

# The steps as a real JSON array, which is what a client sends unprompted.
R="$(post /api/scenario/loadinline '{"scenario":[{"at":0,"lag":true},{"at":5,"lag":false}]}')"
has "$R" '"steps":2' && ok "a scenario sent as a JSON array is accepted" \
  || bad "array-form scenario rejected" "$R"

# A mistyped scenario key only shows on the console when its step fires, long
# after the client that loaded it has moved on - so it is reported at load.
R="$(post /api/scenario/loadinline '{"scenario":"[{\"at\":0,\"lag\":true,\"lag-ms\":80}]"}')"
has "$R" '"warnings":[' && has "$R" 'lag-ms' \
  && ok "scenario load reports a key no module has" || bad "scenario typo went unreported" "$R"

# The agent guides are hand-written, so they are the one reference that can
# drift from the code - and there are two of them, English and Korean, which
# can also drift from each other. Every parameter the modules publish has to
# appear in both.
KEYS="$(get /api/modules | grep -o '"key":"[a-z-]*"' | cut -d'"' -f4 | grep -Ev -- '-(inbound|outbound)$')"
for doc in /llms.txt /llms_kr.txt; do
  GUIDE="$(get "$doc")"
  MISSING=""
  for k in $KEYS; do has "$GUIDE" "$k" || MISSING="$MISSING $k"; done
  [ "$(code "$doc")" = 200 ] && [ -z "$MISSING" ] \
    && ok "$doc is served and documents every module parameter" \
    || bad "$doc is missing or incomplete" "missing:$MISSING"
done

./clumsy --lag-ms 100 >/dev/null 2>&1; RC=$?
[ "$RC" = 2 ] && ok "an unknown command line option exits with code 2" \
  || bad "unknown option was accepted" "exit code $RC"

echo
echo "########## several instances ##########"
# A second clumsy on a taken socket name or web port must refuse to run: a
# script addressing "port N" or the control socket would otherwise drive the
# wrong one. --timeout 5 bounds a regression that starts anyway.
./clumsy --web-port $((PORT + 1)) --timeout 5 >/dev/null 2>&1; RC=$?
[ "$RC" = 4 ] && ok "a second instance on the same socket name exits with code 4" \
  || bad "a taken socket name was accepted" "exit code $RC"
./clumsy --web-port "$PORT" --pipe-name clumsy-apitest --timeout 5 >/dev/null 2>&1; RC=$?
[ "$RC" = 4 ] && ok "a second instance on the same web port exits with code 4" \
  || bad "a taken web port was accepted" "exit code $RC"
./clumsy --pipe-name bad/name --timeout 5 >/dev/null 2>&1; RC=$?
[ "$RC" = 2 ] && ok "an unusable --pipe-name exits with code 2" \
  || bad "an unusable --pipe-name was accepted" "exit code $RC"

./clumsy --web-port $((PORT + 1)) --pipe-name clumsy-apitest --timeout 30 --stats-console 0 \
  >"$TMP/log2" 2>&1 &
SECOND_PID=$!
up=0
for _ in $(seq 1 20); do
  sleep 0.4
  if has "$(curl -sS --max-time 4 "http://127.0.0.1:$((PORT + 1))/api/health" 2>/dev/null)" '"status":"ok"'; then
    up=1; break
  fi
done
[ "$up" = 1 ] && ok "a second instance with its own web port and socket name runs alongside" \
  || bad "the second instance did not come up" "$(cat "$TMP/log2")"
SOCK="$(grep -o '/[a-z]*/clumsy-apitest.sock' "$TMP/log2" | head -1)"
R="$(printf '{"cmd":"quit"}' | python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.stdin.buffer.read())
print(s.recv(4096).decode())' "$SOCK" 2>&1)"
for _ in $(seq 1 20); do kill -0 $SECOND_PID 2>/dev/null || break; sleep 0.3; done
if has "$R" '"status":"ok"' && ! kill -0 $SECOND_PID 2>/dev/null; then
  ok "its own socket reaches it, and only it"
else
  kill -9 $SECOND_PID 2>/dev/null; bad "--pipe-name did not reach the second instance" "$R"
fi
has "$(get /api/health)" '"status":"ok"' && ok "the first instance is unaffected" \
  || bad "the first instance stopped answering"

echo
echo "########## shutting down ##########"
post /api/quit '' >/dev/null
for _ in $(seq 1 20); do kill -0 $CLUMSY_PID 2>/dev/null || break; sleep 0.3; done
if kill -0 $CLUMSY_PID 2>/dev/null; then
  kill -9 $CLUMSY_PID 2>/dev/null; bad "had to kill the process"
else
  ok "clumsy exited cleanly"
fi

echo
echo "==================================================="
echo "  passed: $pass   failed: $fail"
echo "==================================================="
[ "$fail" -eq 0 ]
