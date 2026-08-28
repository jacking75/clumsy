#!/usr/bin/env bash
# Real-packet behaviour test - needs root (NFQUEUE + iptables).
#
# api_test.sh proves the control plane answers correctly. This proves the
# packets actually change: that corrupt flips bits in the payload the receiver
# gets, that the latency histogram measures the delay lag really applied, and
# that the three jitter distributions produce visibly different spreads.
#
#   make && sudo tests/linux/behaviour_test.sh
#
# Traffic is UDP on loopback to a port nothing else uses, and the iptables rule
# is scoped to that port and removed on exit.

set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$REPO/bin/linux/clumsy"
PORT=18313
BASE="http://127.0.0.1:$PORT"
UDP=19998
QUEUE=7
TMP="$(mktemp -d)"

[ "$(id -u)" = 0 ] || { echo "run me as root: sudo $0"; exit 1; }
[ -x "$EXE" ] || { echo "build clumsy first: $EXE not found"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  [ok]   $1"; }
bad() { fail=$((fail+1)); echo "  [FAIL] $1"; [ -n "${2:-}" ] && echo "         $2"; }

cleanup() {
  [ -n "${CLUMSY_PID:-}" ] && kill -9 "$CLUMSY_PID" 2>/dev/null
  iptables -D OUTPUT -p udp --dport "$UDP" -j NFQUEUE --queue-num "$QUEUE" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

get()  { curl -sS --max-time 8 "$BASE$1" 2>/dev/null; }
post() { curl -sS --max-time 8 -X POST -H 'Content-Type: application/json' \
              --data-binary "${2:-}" "$BASE$1" 2>/dev/null; }
json() { printf '%s' "$1" | python3 -c "import sys,json;d=json.load(sys.stdin);print(eval('d'+sys.argv[1]))" "$2" 2>/dev/null; }

# Sends $1 datagrams of a known payload and prints what actually arrived.
# The receiver binds before the sender starts - a background receiver that has
# not finished binding measures nothing, which is the trap the Windows harness
# fell into.
send_and_receive() {
  python3 - "$1" "$2" <<'PY'
import socket, sys, time
count = int(sys.argv[1]); port = int(sys.argv[2])
payload = bytes(range(256)) * 4                      # 1024 bytes, known content
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("127.0.0.1", port))
rx.settimeout(0.4)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
start = time.time()
for _ in range(count):
    tx.sendto(payload, ("127.0.0.1", port))
tx.close()
got = 0; corrupted = 0; first_latency_ms = None
deadline = time.time() + 3.0
while time.time() < deadline:
    try:
        data, _ = rx.recvfrom(65535)
    except socket.timeout:
        if got >= count: break
        continue
    if first_latency_ms is None:
        first_latency_ms = (time.time() - start) * 1000.0
    got += 1
    if data != payload:
        corrupted += 1
rx.close()
print("%d %d %.0f" % (got, corrupted, first_latency_ms if first_latency_ms else -1))
PY
}

echo "########## setup ##########"
iptables -D OUTPUT -p udp --dport "$UDP" -j NFQUEUE --queue-num "$QUEUE" 2>/dev/null
iptables -I OUTPUT -p udp --dport "$UDP" -j NFQUEUE --queue-num "$QUEUE" \
  && ok "NFQUEUE rule installed for udp/$UDP" || { bad "iptables failed"; exit 1; }

cd "$(dirname "$EXE")"
./clumsy --web-port "$PORT" --queue-num "$QUEUE" --timeout 180 --stats-console 0 \
         --filter "udp and outbound" >"$TMP/log" 2>&1 &
CLUMSY_PID=$!
for _ in $(seq 1 30); do
  sleep 0.4
  case "$(get /api/health)" in *'"status":"ok"'*) break;; esac
done
sleep 1
case "$(get /api/status)" in
  *'"capturing":true'*) ok "capture is running on NFQUEUE $QUEUE";;
  *) bad "capture did not start" "$(tail -5 "$TMP/log")"; exit 1;;
esac

echo
echo "########## 1. baseline: nothing enabled ##########"
R="$(send_and_receive 20 $UDP)"
set -- $R
[ "$1" -ge 18 ] && ok "packets pass through untouched ($1/20 delivered)" \
  || bad "baseline delivery" "got=$1"
[ "$2" = 0 ] && ok "payloads are byte-identical with no module on" \
  || bad "payload changed with no module enabled" "corrupted=$2"

echo
echo "########## 2. corrupt actually flips bits (T2) ##########"
# chance 100% and a high bit-error rate, so every packet must arrive damaged.
post /api/modules/corrupt '{"enabled":true,"corrupt-chance":100.0,"corrupt-ber":20000}' >/dev/null
sleep 0.5
R="$(send_and_receive 20 $UDP)"; set -- $R
[ "$1" -ge 15 ] && ok "corrupted packets still reach the receiver ($1/20)" \
  || bad "corrupt dropped everything" "got=$1"
[ "$2" -ge 15 ] && ok "$2 of $1 arrived with a damaged payload" \
  || bad "payload was not corrupted" "corrupted=$2 of $1"
# With the checksum recomputed the OS delivers the damaged packet; that is the
# whole point, and it is what distinguishes this from a link-layer drop.
AFF="$(get /api/stats | python3 -c "import sys,json;print(json.load(sys.stdin)['modules']['corrupt']['affected'])")"
[ "${AFF:-0}" -ge 15 ] && ok "module counted $AFF affected packets" \
  || bad "affected count too low" "$AFF"

# Turning the checksum fix off must make the kernel discard them instead.
post /api/modules/corrupt '{"corrupt-checksum":false}' >/dev/null
sleep 0.5
R="$(send_and_receive 20 $UDP)"; set -- $R
[ "$1" -lt 5 ] && ok "without the checksum fix the stack discards them ($1/20 arrived)" \
  || bad "expected the kernel to drop bad-checksum packets" "got=$1"
post /api/modules/corrupt '{"enabled":false,"corrupt-checksum":true}' >/dev/null

echo
echo "########## 3. latency histogram measures real delay (T8) ##########"
post /api/apply '{"lag":true,"lag-time":300}' >/dev/null
sleep 0.5
R="$(send_and_receive 20 $UDP)"; set -- $R
FIRST_MS="$3"
[ "$1" -ge 15 ] && ok "lagged packets arrive ($1/20)" || bad "lag lost packets" "got=$1"
# The wire latency and the histogram should agree; both are measuring the same
# 300ms, one from outside and one from inside.
if [ "$FIRST_MS" -ge 250 ] && [ "$FIRST_MS" -le 800 ]; then
  ok "measured wire delay ${FIRST_MS}ms for a 300ms setting"
else
  bad "wire delay out of range" "${FIRST_MS}ms"
fi
LAT="$(get /api/stats)"
LN="$(json "$LAT" "['latency']['count']")"
LP50="$(json "$LAT" "['latency']['p50']")"
LMAX="$(json "$LAT" "['latency']['max']")"
[ "${LN:-0}" -ge 15 ] && ok "histogram recorded $LN delayed packets" \
  || bad "histogram did not fill" "count=$LN"
P50I="$(printf '%.0f' "${LP50:-0}")"
if [ "$P50I" -ge 250 ] && [ "$P50I" -le 600 ]; then
  ok "p50 = ${P50I}ms, consistent with the 300ms setting"
else
  bad "p50 outside the expected band" "p50=${LP50} max=${LMAX}"
fi
# The Prometheus histogram must agree with the JSON one.
MB="$(get /metrics | grep -c '^clumsy_latency_ms_bucket')"
MC="$(get /metrics | sed -n 's/^clumsy_latency_ms_count \([0-9]*\)$/\1/p')"
[ "$MB" = 13 ] && ok "/metrics exposes the same 13 buckets" || bad "bucket count" "$MB"
[ "${MC:-0}" = "${LN:-0}" ] && ok "/metrics count matches /api/stats ($MC)" \
  || bad "metrics and stats disagree" "metrics=$MC stats=$LN"
post /api/apply '{"lag":false}' >/dev/null

echo
echo "########## 4. jitter distributions differ in shape (T3) ##########"
# Same [0, 400] range for all three; only the distribution changes. Normal
# should cluster near the middle, pareto should sit low with a long tail.
for dist in uniform normal pareto; do
  # Restart the capture FIRST: statsReset() clears the histogram, and measuring
  # before clearing would blend the previous section's samples into this one.
  # (Getting this backwards is what made the first run report a uniform median
  # of 303ms - it still held the 300ms lag samples from the section above.)
  post /api/stop '' >/dev/null
  sleep 0.5
  post /api/filter '{"filter":"udp and outbound"}' >/dev/null
  sleep 1
  post /api/apply "{\"jitter\":true,\"jitter-min\":0,\"jitter-max\":400,\"jitter-dist\":\"$dist\"}" >/dev/null
  sleep 0.4
  send_and_receive 60 $UDP >/dev/null
  sleep 1
  S="$(get /api/stats)"
  N="$(json "$S" "['latency']['count']")"
  P50="$(printf '%.0f' "$(json "$S" "['latency']['p50']")")"
  P95="$(printf '%.0f' "$(json "$S" "['latency']['p95']")")"
  echo "  $dist: n=$N p50=${P50}ms p95=${P95}ms"
  eval "P50_$dist=$P50; P95_$dist=$P95"
  post /api/apply '{"jitter":false}' >/dev/null
done
# Uniform over [0,400] has a true median of 200; anything far off means the
# histogram or the sampler is wrong, not merely differently shaped.
if [ "${P50_uniform:-0}" -ge 140 ] && [ "${P50_uniform:-0}" -le 280 ]; then
  ok "uniform median (${P50_uniform}ms) matches the expected 200ms"
else
  bad "uniform median is off" "p50=${P50_uniform}, expected ~200"
fi
# Pareto is defined by mass near the floor; its median must be the lowest.
if [ "${P50_pareto:-999}" -le "${P50_uniform:-0}" ]; then
  ok "pareto median (${P50_pareto}ms) sits below uniform (${P50_uniform}ms)"
else
  bad "pareto is not bottom-weighted" "pareto=${P50_pareto} uniform=${P50_uniform}"
fi
# Normal concentrates around the centre, so its p95 stays under the uniform p95.
if [ "${P95_normal:-999}" -le "$(( ${P95_uniform:-0} + 40 ))" ]; then
  ok "normal p95 (${P95_normal}ms) does not exceed uniform (${P95_uniform}ms)"
else
  bad "normal spread is wider than uniform" "normal=${P95_normal} uniform=${P95_uniform}"
fi

echo
echo "########## 5. scenario editor payload drives real modules (T4) ##########"
post /api/scenario/loadinline \
  '{"scenario":"[{\"at\":0,\"drop\":true,\"drop-chance\":100.0}]"}' >/dev/null
post /api/scenario/start '' >/dev/null
sleep 1.5
R="$(send_and_receive 20 $UDP)"; set -- $R
[ "$1" -le 2 ] && ok "the inline scenario really enabled a 100% drop ($1/20 arrived)" \
  || bad "scenario did not take effect" "got=$1"
post /api/scenario/stop '' >/dev/null
post /api/apply '{"drop":false}' >/dev/null

echo
echo "########## 6. pcap round trip: export then replay (T7) ##########"
# Record real traffic, then play it back and prove the packets actually reach
# the wire. api_test.sh can only check the control plane, because injection
# needs CAP_NET_RAW - which this suite has.
CAP="$TMP/roundtrip.pcap"
post /api/pcap/start "{\"path\":\"$CAP\"}" >/dev/null
sleep 0.4
send_and_receive 25 $UDP >/dev/null
sleep 0.6
WROTE="$(json "$(get /api/stats)" "['pcap']['packets']")"
post /api/pcap/stop '' >/dev/null
[ "${WROTE:-0}" -ge 20 ] && ok "captured $WROTE packets into a pcap file" \
  || bad "pcap export wrote too little" "packets=$WROTE"

# A receiver has to be listening or the replayed packets are answered with
# ICMP port-unreachable and nothing is observable.
python3 - "$UDP" "$TMP/rx.txt" <<'PY' &
import socket, sys, time
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("127.0.0.1", int(sys.argv[1])))
rx.settimeout(0.5)
got = 0
end = time.time() + 8
while time.time() < end:
    try:
        rx.recvfrom(65535); got += 1
    except socket.timeout:
        pass
open(sys.argv[2], "w").write(str(got))
PY
RX_PID=$!
sleep 0.6

R="$(post /api/replay/start "{\"path\":\"$CAP\",\"speed\":20}")"
has() { case "$1" in *"$2"*) return 0;; *) return 1;; esac; }
has "$R" '"status":"ok"' && ok "replay started on the recorded file" || bad "replay start" "$R"
sleep 4
RS="$(get /api/stats)"
RREAD="$(json "$RS" "['replay']['read']")"
RSENT="$(json "$RS" "['replay']['sent']")"
RFAIL="$(json "$RS" "['replay']['failed']")"
[ "${RREAD:-0}" -ge 20 ] && ok "replay read $RREAD records back" \
  || bad "replay read too few" "read=$RREAD"
# This is the assertion api_test.sh cannot make: with CAP_NET_RAW the raw
# socket accepts the packets instead of every one landing in 'failed'.
[ "${RSENT:-0}" -ge 20 ] && ok "replay injected $RSENT packets onto the wire (failed=$RFAIL)" \
  || bad "replay could not inject" "sent=$RSENT failed=$RFAIL"
post /api/replay/stop '' >/dev/null

wait $RX_PID 2>/dev/null
RXGOT="$(cat "$TMP/rx.txt" 2>/dev/null || echo 0)"
[ "${RXGOT:-0}" -ge 10 ] && ok "the listener received $RXGOT replayed datagrams" \
  || bad "replayed packets never arrived" "received=$RXGOT"

echo
echo "########## shutting down ##########"
post /api/quit '' >/dev/null
for _ in $(seq 1 20); do kill -0 $CLUMSY_PID 2>/dev/null || break; sleep 0.3; done
kill -0 $CLUMSY_PID 2>/dev/null && bad "process did not exit" || ok "clumsy exited cleanly"
CLUMSY_PID=""

echo
echo "==================================================="
echo "  passed: $pass   failed: $fail"
echo "==================================================="
[ "$fail" -eq 0 ]
