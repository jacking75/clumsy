# clumsy User Manual

**Language**: English | [한국어](manual.md)

---

# Part 1 — Usage

## 1. Before You Start

### Requirements

- **Windows 7 / 8 / 10 / 11** (64-bit only)
- **Administrator rights** — clumsy loads a network driver, so it must run elevated.
- A browser for the web dashboard (nothing to install; it is served locally)
- **Linux** is supported too (a kernel with NFQUEUE support and `CAP_NET_ADMIN`). This manual is written for Windows;
  building, privileges, iptables integration and platform differences on Linux are covered in [docs/LINUX.md](docs/LINUX.md) (Korean).

### Running clumsy

clumsy is a **console application**. Run it from an **elevated Command Prompt or PowerShell**.

```
clumsy.exe
```

It prints a banner on startup:

```
clumsy 0.4 - console + web network condition simulator
  Administrator : yes
  Web dashboard : http://127.0.0.1:8080/
  Named Pipe    : \\.\pipe\clumsy
  Presets       : 34 loaded
  Profiles      : 2 loaded
  Press Ctrl+C to quit.
```

Open the address shown in the banner in a browser to get the dashboard. Press `Ctrl+C` to quit.

Without Administrator rights clumsy does not exit: it **prints a warning and brings up the dashboard only**.
Starting a capture in that state returns a clear error:

```
clumsy needs Administrator rights to open the WinDivert driver. Restart the console as Administrator.
```

If `--filter` was given on the command line, however, clumsy does not wait: it prints this error and exits with code `3`
(see [Chapter 5](#argument-validation-and-exit-codes)).

`--elevate on` shows a UAC prompt and relaunches clumsy elevated
(a new console window opens and the original instance exits).

> **Note**: Several clumsy instances can run at once (for instance to give each client its own conditions;
> see "Per-Client Filters and Several Instances" in chapter 3), but each needs its own `--web-port` and
> `--pipe-name`. Started on a port or pipe name that is already in use, the new instance prints why and
> exits with code `4`; the running one is left untouched.

> **Changed in 0.4**: The old IUP-based desktop GUI has been removed.
> Everything is done through the web dashboard, command-line options, the REST API and the Named Pipe API.

---

## 2. Web Dashboard Layout

Browse to `http://127.0.0.1:8080/` and you get a single page laid out like this:

```
┌───────────────────────────────────────────────────────────┐
│ ● clumsy v0.4       [Theme] [Download report] [API]       │  header
├───────────────────────────────────────────────────────────┤
│ CAPTURE                                                   │
│  filter input         Preset [▼]  Process [____]          │
│  [Start] [Stop]                                           │
│  status message                                           │
│  ▸ Filter builder  (protocol/direction/port/IP -> filter) │
├───────────────────────────────────────────────────────────┤
│ LIVE STATISTICS                                           │
│  [Captured] [Sent] [Packets/sec] [Elapsed] [Buffers]      │
│  ┌── live throughput chart ──┐                            │
│  [Delay p50] [p95] [p99] [max] [Delayed packets]          │
├───────────────────────────────────────────────────────────┤
│ MODULES                                                   │
│  ☑ Lag   Inbound ☑ Outbound ☑ Delay(ms)[250] 12 affected ▁▃▅│
│  ☐ Jitter     ...                                         │
│  ☐ Drop       ...                                         │
│  (12 modules)                                             │
├───────────────────────────────────────────────────────────┤
│ PROFILES, SCENARIO AND CAPTURE FILE                       │
│  Profile [▼] [Apply] Save as [____] [Save] [Delete]       │
│  Scenario [________] [Load] [Start] [Stop]                │
│  pcap     [________] [Start dump] [Stop dump]             │
│  replay   [________] Speed[1] ☐loop [Replay] [Stop]       │
├───────────────────────────────────────────────────────────┤
│ SCENARIO EDITOR                                           │
│  Step 1 [at time ▼]              [Remove]                 │
│    Seconds[5] Repeat[0] Max[0]                            │
│    [Lag — on/off ▼][true] [×]   [+ change]                │
│  [+ Add step]                                             │
│  Scenario JSON [ ...generated document... ]               │
│  [Load into clumsy] [Load & start]                        │
└───────────────────────────────────────────────────────────┘
```

- **Dot in the header (●)**: state of the live stream (Server-Sent Events). Green means connected.
- **Theme button**: cycles `Auto` (follow the OS) → `Light` → `Dark` → `Auto`. The choice is
  stored in `localStorage` and survives a reload. Only `Auto` follows the OS
  dark-mode setting.
- **CAPTURE**: decides which packets are intercepted. The Preset dropdown comes from `config.json`;
  the Process field takes an executable name and limits the capture to that process's packets.
- **Filter builder**: composes an expression without you having to know the WinDivert filter syntax
  (see [Filter Builder](#filter-builder)).
- **LIVE STATISTICS**: live numbers pushed every 200 ms, a packets-per-second chart,
  and percentiles of the delay Lag and Jitter actually applied.
- **MODULES**: ticking a module's checkbox expands its parameter form.
  The form is **generated** from the `ParamSpec` metadata the server sends,
  so the dashboard keeps working when a new module is added.
  An enabled module also shows a **per-second sparkline**. It plots the rate,
  not the cumulative counter, so you can see when the module was actually working.
- **SCENARIO EDITOR**: builds a scenario in the browser and runs it without a file
  (see [Chapter 8](#8-scenario-scripting)).
- **Works offline**: the dashboard is a single HTML file (`web/index.html`) with no CDN
  or external framework. It works unchanged on a test network with no internet access.

### Console Output

The console shows state changes and a periodic summary:

```
Capturing. filter="udp and outbound"
[  10s] captured=15234 sent=15102 lag=15234 drop=132
[  20s] captured=31007 sent=30761 lag=31007 drop=246
```

- Set the summary interval with `--stats-console <sec>` (default 10 seconds, `0` turns it off).
- `--verbose on` prints a per-packet trace (expensive; use it only for debugging).

---

## 3. Filters

### Basic Concept

The filter decides which packets clumsy intercepts. Only packets that match the filter are delayed, dropped and so on.

### The Preset Dropdown

The **Preset** dropdown in the dashboard's **CAPTURE** section lists the predefined filters stored in `config.json` (or the legacy `config.txt`). Both files are read from the folder that holds `clumsy.exe`; the originals in the repository live in `etc/`. Picking one fills in the filter field.

Built-in presets:

| Preset name | Description |
|------------|------|
| localhost ipv4 all | All traffic inside this PC (loopback) (`outbound and loopback`) |
| localhost ipv4 tcp | TCP traffic inside this PC |
| localhost ipv4 udp | UDP traffic inside this PC |
| all sending packets | Every outgoing packet (`outbound`) |
| all receiving packets | Every incoming packet (`inbound`) |
| all ipv4 against specific ip | Traffic with one IP address (example address `198.51.100.1`) |
| tcp ipv4 against specific ip | TCP traffic with one IP address |
| udp ipv4 against specific ip | UDP traffic with one IP address |
| all ipv4 against port | TCP and UDP traffic on one port (example port `12354`) |
| tcp ipv4 against port | TCP traffic on one port |
| udp ipv4 against port | UDP traffic on one port |

### Writing a Filter by Hand

Filters use the WinDivert filter language. Common expressions:

```
# UDP only
udp

# TCP only
tcp

# outgoing packets only
outbound

# incoming packets only
inbound

# UDP to or from one IP address
udp and (ip.DstAddr == 192.168.1.10 or ip.SrcAddr == 192.168.1.10)

# UDP on one port (e.g. 7777)
udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)

# loopback (localhost) - outbound is required
udp and outbound and loopback
```

> **Loopback caveat**: when testing localhost (127.x.x.x) traffic, always include `outbound`. The combination `inbound and loopback` does not work.

### Start / Stop

Enter a filter and press **Start** to begin intercepting packets.
- A syntax error in the filter is shown in the status area.
- The Start button is disabled while a capture is running.
- Press **Stop** to stop.

On the command line, `--filter "<expr>"` starts capturing immediately.
Over the REST API `POST /api/filter` / `POST /api/stop` do the same, and over the Named Pipe API
`{"cmd":"filter",...}` / `{"cmd":"stop_capture"}`.
Sending `POST /api/filter` while a capture is running stops it and restarts with the new filter.

### Filter Builder

Expand **Filter builder** in the dashboard and combine protocol (TCP/UDP/ICMP), direction (inbound/outbound),
port (single or a range such as `7770-7780`), remote IPv4 address and loopback; the filter
expression is generated as you type.

- **Use this**: copies the generated expression into the filter field above.
- **Save as preset...**: saves it under a name of your choice in `config.json`
  (`POST /api/presets`). It shows up in the Preset dropdown immediately.

All of this happens in the browser; nothing round-trips to the server.

### Per-Process Filtering

Enter an executable name (e.g. `game.exe`) in the **Process** field and press Start, and only that process's network traffic is targeted. Leave it empty to apply to all packets.

**How it works**: at Start, clumsy finds the process's PIDs, collects the local ports it is using from the TCP/UDP port tables, and adds them to the filter.

**Command line**: `--process game.exe`

```
# Example: 100 ms of delay on game.exe's UDP traffic only
clumsy.exe --filter "udp" --process game.exe --lag on --lag-time 100
```

**Limitations**:
- Ports are snapshotted at Start. Connections the process opens after Start are not included.
- So **launch the game and connect to the server first**, then start clumsy.
- If the process is not running or has no active ports, an error message is shown.
- When several processes share the name, all of them are targeted. To pick one, use its port as below.

### Per-Client Filters and Several Instances

When one machine runs several clients (with the server on the same machine or another one), clients are
told apart by their **local port**. Look it up after the client has connected with
`Get-NetUDPEndpoint -OwningProcess <PID>` (`Get-NetTCPConnection` for TCP), or `ss -unap` on Linux.
With the server on port 7777:

```
# every client
udp.DstPort == 7777 or udp.SrcPort == 7777

# one client (local port 50001): name both ports of its flow
(udp.SrcPort == 50001 and udp.DstPort == 7777) or (udp.SrcPort == 7777 and udp.DstPort == 50001)
```

For the **same conditions** on several clients, join their groups with `or` and give them to one clumsy.
For **different conditions** per client, run one clumsy per set of conditions, each with its own
`--web-port` and `--pipe-name` (and `--queue-num` on Linux).

```
clumsy.exe --web-port 8081 --pipe-name client1 --lag on --lag-time 150 --filter "(udp.SrcPort == 50001 and udp.DstPort == 7777) or (udp.SrcPort == 7777 and udp.DstPort == 50001)"
clumsy.exe --web-port 8082 --pipe-name client2 --drop on --drop-chance 10 --filter "(udp.SrcPort == 50002 and udp.DstPort == 7777) or (udp.SrcPort == 7777 and udp.DstPort == 50002)"
```

- **Overlap protection**: a filter that could match packets another running instance already captures is
  refused (exit code `4`, HTTP `409` from `POST /api/filter`). The message names the other instance (PID,
  web address, pipe) and an example packet both would match. A shared packet would otherwise be shaped by
  whichever instance saw it first and silently skipped by the other.
- Client ports alone, as in `udp.SrcPort == 50001 or udp.DstPort == 50001`, also match a packet from 50001
  to 50002 and so overlap the instance for 50002. Name the server port as well, as above.
- These filters match localhost traffic too (it is seen once, as outbound), so the server-on-this-machine
  case needs no `loopback` term.
- On Linux `--auto-iptables` turns each `or`-joined group into an exact iptables rule. Wrapping the groups,
  as in `udp and (...)`, widens the rule to all UDP and it overlaps every other instance.
- Give each instance its own output file names (`--stats-log`, `--report-out`, `--pcap-out`).
- A client that reconnects often gets a new port. Send `POST /api/filter` again with the new one.

---

## 4. Modules

Tick a module's checkbox in the dashboard's **MODULES** section to apply it. Several modules can be on at once.
Modules can be switched on before a capture starts, but they only act on traffic while a capture is running (after Start).
All modules start off and are applied to each packet in the order below.

Every module has **Inbound** / **Outbound** checkboxes to choose which direction it applies to
(`--<module>-inbound`, `--<module>-outbound`, both on by default).

The "Setting" column in the tables below is the name the dashboard shows; the key in parentheses is the parameter key used by the CLI and the API.

---

### Lag — Fixed Delay

Holds every packet for the given time (ms) before sending it on.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Delay (ms) (`lag-time`) | Delay | 50ms | 0 ~ 15000ms |

**How it works**: packets are held in an internal buffer and sent once the delay has passed. Every packet is delayed by the same amount.

**When to use it**: when you want a constant round-trip time (RTT). For example, reproducing "this server always has 100 ms of latency".

```
Example: Delay = 100 -> every packet arrives 100 ms late
```

---

### Jitter — Variable Delay

Adds a random delay between Min and Max to each packet.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Min (ms) (`jitter-min`) | Minimum delay | 20ms | 0 ~ 5000ms |
| Max (ms) (`jitter-max`) | Maximum delay | 100ms | 0 ~ 5000ms |
| Distribution (`jitter-dist`) | Shape of the delay distribution | uniform | uniform / normal / pareto |

**How it works**: each packet gets a random delay in `[Min, Max]`. Because the delays differ, packets can arrive in a different order (natural reordering).

**When to use it**: reproducing irregular delay like on the real internet. Testing lag compensation algorithms.

```
Example: Min=20, Max=150 -> some packets wait 20 ms, others 150 ms
     -> packets may arrive out of order
```

#### Choosing a Distribution

Real network delay is not uniform. A healthy link clusters around its average;
a congested one is mostly fine and occasionally spikes hard. Both shapes are needed to
reproduce bugs, so clumsy offers the same three distributions as Linux `tc netem`.

| Value | Shape | What it reproduces |
|----|------|---------------------|
| `uniform` (default) | spread evenly over `[Min, Max]` | sweeping a wide range with no particular assumption |
| `normal` | concentrated around the middle, rare at the ends | the natural wobble of a stable link |
| `pareto` | mostly near Min, with occasional large spikes | congestion and bufferbloat, where the long tail is the problem |

`normal` uses `(Min+Max)/2` as the mean and `range/6` as the standard deviation, so about 99.7% of
draws land inside the range; samples outside it are clamped to the ends rather than redrawn
(redrawing would leave the worst case unbounded). `pareto` uses shape=2, which puts the median
near `Min + 0.29×range` and leaves an upper tail.

Measured example (Min=0, Max=400, 60 packets each):

| Distribution | p50 | p95 |
|------|-----|-----|
| uniform | 270ms | 403ms |
| normal | 217ms | 336ms |
| pareto | 161ms | 327ms |

```bash
# Shake lag compensation with long-tail delay
clumsy --filter "udp and outbound" --jitter on \
       --jitter-min 10 --jitter-max 400 --jitter-dist pareto
```

> The CLI and the API also accept the numbers `0`/`1`/`2` in place of the names (`uniform`/`normal`/`pareto`).
> Profiles and scenarios written before the names existed keep working.

> **Lag vs Jitter**: Lag gives every packet the same delay (constant RTT); Jitter gives each packet a different delay (irregular RTT). The real internet has both.

---

### Drop — Packet Loss

Discards packets (does not send them) with the given probability.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Chance (%) (`drop-chance`) | Drop probability | 10.0% | 0 ~ 100% |

**How it works**: the probability is evaluated independently for each packet. At 10%, on average one packet in ten is dropped.

**When to use it**: testing under packet loss. Verifying retransmission and loss recovery code.

```
Example: Chance=5.0 -> about 5 of every 100 packets disappear
```

---

### Burst Loss — Clustered Loss (Gilbert-Elliott Model)

Reproduces loss that comes in clusters, the way it does on real networks.

**Two states**:
- **Good**: the normal state, where almost every packet gets through
- **Bad (burst)**: a congestion or interference state, where packets are lost heavily

| Setting | Description | Default | Range |
|------|------|--------|------|
| Good drop (%) (`burstloss-good`) | Drop probability in the Good state | 2.0% | 0 ~ 100% |
| Bad drop (%) (`burstloss-bad`) | Drop probability in the Bad state | 80.0% | 0 ~ 100% |
| Good to Bad (%) (`burstloss-gb`) | Per-packet Good→Bad transition probability | 5.0% | 0 ~ 100% |
| Bad to Good (%) (`burstloss-bg`) | Per-packet Bad→Good transition probability | 20.0% | 0 ~ 100% |

**How it works**: for every packet, the current state's drop probability decides whether it is lost, and the transition probabilities decide whether the state changes.

With the defaults:
- Average length of a Bad period: 1 ÷ 20% = **5 packets**
- Average length of a Good period: 1 ÷ 5% = **20 packets**
- Share of time spent in Bad: 5% ÷ (5% + 20%) = **20%**
- Overall average loss: 2% × 80% + 80% × 20% ≈ **17.6%**

**When to use it**: reproducing the consecutive losses of mobile networks and Wi-Fi. Testing consecutive-loss scenarios that the Drop module (independent probability) cannot produce.

```
Example: with the defaults
  -> good periods:  on average 1 packet in 50 lost (2%), lasting about 20 packets
  -> burst periods: 4 of 5 packets lost (80%), lasting about 5 packets
  -> unlike Drop, losses come "in clumps"
```

> **Drop vs Burst Loss**: Drop flips a coin for every packet independently. Burst Loss, once it enters a bad period, keeps losing packets in a row (when it snows, it keeps snowing).

---

### Blackout — Outages

Blocks all packets on a fixed cycle to reproduce a dropped connection.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Duration (ms) (`blackout-duration`) | How long each outage lasts | 3000ms | 100 ~ 60000ms |
| Gap (ms) (`blackout-gap`) | Normal time between outages | 15000ms | 1000 ~ 300000ms |
| Trigger now button (`blackout-trigger`) | Start / cancel an outage immediately | — | click (one-shot action) |

**How it works**: once enabled, after Gap has passed every packet is dropped for Duration. The cycle repeats.

```
Timeline example (Gap=15000, Dur=3000):
  0s       15s      18s      33s      36s
  |- normal -|- outage -|- normal -|- outage -| ...
```

**Trigger button**:
- Click during a normal period → an outage starts immediately (the cycle restarts)
- Click during an outage → traffic recovers immediately (the outage is cancelled)
- On the command line it is `--blackout-trigger on`; in the API and in scenarios, `"blackout-trigger": true`.
- The trigger is only honoured **while Blackout is already on and a capture is running**. A trigger sent while the module is off
  is ignored, and one sent in the same request (or scenario step) that switches the module on is cleared when the module starts.
  Switch it on first and trigger afterwards.

**When to use it**: testing client reconnect logic, connection timeout handling and session recovery. Unlike Throttle, it drops immediately with no holding.

> **Throttle vs Blackout**: Throttle holds packets for a while and then sends or discards them all at once. Blackout has no buffer; every packet in an outage is dropped on the spot.

---

### Throttle — Temporary Hold

Randomly pauses sending; when the time frame ends, the held packets are released together or all discarded.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Timeframe (ms) (`throttle-frame`) | How long a hold lasts | 30ms | 0 ~ 1000ms |
| Chance (%) (`throttle-chance`) | Probability of starting a hold | 10.0% | 0 ~ 100% |
| Drop throttled (`throttle-drop`) | Discard the held packets | OFF | ON/OFF |

**How it works**: once a hold starts, packets are buffered for the time frame (at most 1000; a full buffer ends the hold early).
- `Drop throttled OFF`: when the time is up, the buffered packets are sent at once → a momentary burst
- `Drop throttled ON`: when the time is up, the buffered packets are all discarded → a short total outage

**When to use it**: reproducing a network that keeps stalling or delivers packets in sudden bursts.

---

### Duplicate — Packet Duplication

Sends extra copies of packets.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Copies (`duplicate-count`) | Number of copies, original included | 2 | 2 ~ 50 |
| Chance (%) (`duplicate-chance`) | Probability of duplicating a packet | 10.0% | 0 ~ 100% |

**How it works**: a packet selected by the chance is copied until there are Copies of it, sent back to back. Copies=2 means 1 original + 1 copy = 2 packets sent.

**When to use it**: testing duplicate-packet handling. Especially checking how a UDP game copes with the same packet arriving twice.

---

### Out of Order — Reordering

Deliberately changes the order in which packets arrive.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Chance (%) (`ood-chance`) | Probability of capturing a packet into the buffer | 10.0% | 0 ~ 100% |
| Buffer (`ood-buffer`) | Maximum number of packets held | 5 | 2 ~ 50 |
| Delay (ms) (`ood-delay`) | Maximum hold time | 200ms | 10 ~ 2000ms |

**How it works**: packets selected by the chance are collected in an internal buffer (up to Buffer packets). When the buffer is full or the oldest packet has waited longer than Delay, every packet in the buffer is released in random order (Fisher-Yates shuffle). This reproduces real out-of-order delivery convincingly.

**When to use it**: testing packets arriving out of order. Verifying sequence-number handling and reassembly in UDP games. Larger Buffer and Delay values produce more severe reordering.

> **Compared with Jitter**: Jitter gives each packet a different delay, which reorders them as a side effect. Out of Order collects several packets, shuffles them and releases them together.

---

### Tamper — Payload Tampering

Deliberately damages part of the packet's payload.

| Setting | Description | Default | Choices |
|------|------|--------|--------|
| Chance (%) (`tamper-chance`) | Tamper probability | 10.0% | 0 ~ 100% |
| Redo checksum (`tamper-checksum`) | Recompute checksums after tampering | ON | ON/OFF |
| Position (`tamper-position`) | Which part of the payload to tamper | 1 (Center) | 0=Front / 1=Center / 2=Back / 3=Random |

**How it works**: a quarter of the payload is overwritten with an XOR pattern. Position decides which quarter.
Position is entered as a number (the dashboard shows a number field too).

| Position | Description |
|----------|------|
| 0 = Front | First quarter of the payload — hits game packet headers and sequence numbers |
| 1 = Center | Middle quarter (default) — the usual data area |
| 2 = Back | Last quarter — useful for protocols that keep a checksum or footer there |
| 3 = Random | A random offset for every packet — unpredictable damage |

With `Redo checksum` on, the IP/TCP/UDP checksums are recomputed after tampering so the receiver does not reject the packet outright for a bad checksum. Use it to test a payload that is damaged while the checksums stay valid.

Short packets of 4 bytes or less have their whole payload tampered regardless of Position.

**When to use it**: testing how an application behaves when it receives corrupted data. Damaging a game protocol's sequence numbers or headers to check error handling on server and client. Position=Front can trigger parsing errors in game packets with fixed-offset headers.

---

### Corrupt — Bit Errors (Wireless Corruption)

Flips each payload bit independently with a very low probability.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Chance (%) (`corrupt-chance`) | Probability of corrupting this packet | 10% | 0 ~ 100% |
| Bit error (ppm) (`corrupt-ber`) | **Per-bit** flip probability (parts per million) | 100 | 0 ~ 1,000,000 |
| Redo checksum (`corrupt-checksum`) | Recompute checksums after corruption | on | on/off |
| Inbound / Outbound | Direction | both | — |

**How it differs from Tamper**: Tamper replaces a whole slice of the payload with an XOR pattern —
closer to a deliberate man-in-the-middle. Corrupt reproduces the **sparse, low-rate bit flips** that
actually happen on radio links (Wi-Fi, cellular, satellite).
Both mean "the data got damaged", but the application experiences them differently.

**Getting a feel for ppm**: 100 ppm is 0.01%, roughly one flipped bit per 1250 bytes.
That is 1.1 bits on average in a 1400-byte packet. At 20000 ppm (2%) about 224 bits per packet
are flipped and the payload becomes unrecognizable.

**Redo checksum decides the outcome**:

| Setting | Result | What it tests |
|------|------|------------------|
| on | corrupted data **reaches the application** | the app's data validation and parse error handling |
| off | the kernel **discards** it for a bad checksum | loss equivalent to a link-layer CRC failure |

This was confirmed by measurement — checksum on: 20/20 arrived (all corrupted); off: 0/20 arrived.

**When to use it**: checking how an app behaves when it meets partially corrupted data on mobile and wireless links.
Testing a game protocol's checksum and serialization validation.

```bash
# Imitate a wireless link: 0.5% bit errors on half the packets
clumsy --filter "udp and outbound" --corrupt on \
       --corrupt-chance 50 --corrupt-ber 5000
```

> **Implementation note**: drawing a random number per bit would take 11,200 calls for one 1400-byte packet
> and stall the packet path. Instead the gap to the next error is drawn directly from a geometric
> distribution, so random numbers are spent only on the bits actually flipped (2-3 per packet at the default).

---

### Set TCP RST — Forced TCP Reset

Sets the RST flag on TCP packets to tear connections down.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Chance (%) (`reset-chance`) | RST injection probability | 0% | 0 ~ 100% |
| RST next packet button (`reset-next`) | Inject RST into the next packet | — | click (one-shot action) |

**How it works**: the RST bit is set in the TCP header. The receiver treats the connection as abnormally terminated.
The default Chance is 0%, so merely switching the module on does nothing until you press `RST next packet`.

**When to use it**: testing client/server reconnect handling after a forced TCP close. The `RST next packet` button cuts the connection at exactly the moment you choose.
On the command line it is `--reset-next on`; in the API and in scenarios, `"reset-next": true`. Like the Blackout trigger,
it is only honoured while the module is already on and a capture is running.

> **Note**: it has no effect on UDP. It only works on TCP connections.

---

### Bandwidth — Throughput Cap

> **Initial burst allowance**: the token bucket is `max(65535 bytes, limit×2 seconds)` in size and **starts
> full**. So even with a low limit (e.g. 1 KB/s), **the first ~64 KB pass immediately**
> and the limit applies after that. The 65535-byte floor exists so that a single maximum-size packet (65535 B)
> can never get stuck forever behind a low limit.
> If a short test looks like "the limit does nothing", the amount sent was smaller than the bucket.
> This burst only applies when the limit is greater than 0. With `Limit=0` nothing gets out from the start.

Limits throughput (KB/s). Packets over the limit are **queued and sent late**.

| Setting | Description | Default | Range |
|------|------|--------|------|
| Limit (KB/s) (`bandwidth-bandwidth`) | Bandwidth cap (1 KB = 1024 bytes) | 10 KB/s | 0 ~ 99999 KB/s |

**How it works (token bucket)**:

```
tokens = refilled every step at Limit KB/s (up to 2 seconds' worth, at least 65535 bytes)
sending a packet -> consumes tokens equal to its size
not enough tokens -> packet waits in the queue, sent in order as tokens refill
queue full (2000 packets) -> further packets are dropped (last resort)
```

- `Limit=500` → the queue drains in order at 500 KB/s; traffic above that is delayed
- `Limit=0` → the step that releases packets from the queue never runs = not a single packet gets out; they pile up in the queue and are dropped once it exceeds 2000 (total block). The initial burst above only applies when `Limit` is greater than 0
- Disabling the module → everything in the queue is released at once (no loss)

**When to use it**: reproducing slow mobile data or a congested network. Because it delays rather than drops, it suits testing TCP flow control and retransmission better.

```
Example: Limit=100 (100 KB/s)
  -> roughly one 100-byte packet per millisecond
  -> a sudden 10 KB of data is spread over about 100 ms
  -> unlike Drop, nothing disappears; it just arrives late
```

> **Drop vs Bandwidth**: Drop loses some packets for good. Bandwidth keeps every packet but limits the rate, which creates delay.

---

## Live Statistics Panel

Once a capture is running, the dashboard's **LIVE STATISTICS** section shows live numbers.
The server pushes them every 200 ms over `GET /api/stream` (Server-Sent Events),
and every counter is reset when the capture stops.

### Displayed Values

| Card | Description |
|------|------|
| `Captured` | Total packets captured |
| `Sent` | Total packets sent (reinjected) |
| `Packets / sec` | Capture rate per second |
| `Elapsed` | Time since the capture started |
| `Buffers (lag/jit/bw)` | Current internal buffer size of the Lag / Jitter / Bandwidth modules |
| `Delay p50 / p95 / p99` | Percentiles of the delay Lag and Jitter actually applied |
| `Delay max` | Largest delay observed |
| `Delayed packets` | Number of packets recorded in the delay histogram |

The chart below them plots packets per second for the last 180 samples (drawn directly with the Canvas API).
On the right of each module row are the number of packets that module has handled and, for enabled modules,
a per-second sparkline.

#### Delay Percentiles (p50 / p95 / p99)

An average hides **the occasional packet that spikes**. A session with p50=40ms and p99=900ms and one
that sits at 60 ms throughout can have similar averages and still be completely different bug reports.

The figures come from the **measured time** between Lag or Jitter putting each packet into its buffer and taking it out.
Samples are not kept individually but accumulated in a 13-bucket histogram, so memory and cost do not grow
with session length (one counter increment per packet).

Bucket upper bounds (ms): `1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, and above`

> Percentiles are **approximations** interpolated linearly within a bucket (the same method as Prometheus
> `histogram_quantile()`). The minimum and maximum are recorded exactly, though, so the
> outermost buckets are narrowed to the observed range — without that correction the
> p95 of a 0-400 ms spread comes out larger than reality, such as 467 ms.

The full distribution is in the **Delay distribution** bar chart of the session report (`Download report`).
It stays empty for a session in which neither module was used.

### What Each Module Counter Means

- **Lag / Jitter**: packets put into the delay buffer (cumulative)
- **Drop / Burst Loss / Blackout**: packets dropped
- **Throttle**: packets put into the hold buffer
- **Duplicate**: extra copies created (originals not counted)
- **Out of Order**: packets put into the reorder buffer
- **Tamper**: packets tampered with
- **Corrupt**: packets in which at least one bit was flipped
- **Set TCP RST**: packets given an RST
- **Bandwidth**: packets put into the bandwidth queue

### API Access

- **REST**: `GET /api/stats` — all of the above, plus per-module counters and pcap/scenario state, as JSON
- **SSE**: `GET /api/stream` — the same payload pushed every 200 ms
- **Named Pipe**: `{"cmd":"get_stats"}` — keeps the same reply format as before

Besides "how many packets it touched" (`modules.corrupt.affected`), Corrupt also reports "how hard it
touched them". `corrupt.bitsFlipped` in `GET /api/stats`, `clumsy_corrupt_flipped_bits_total` in
`/metrics`, and a line under the module table of the HTML report all give the same cumulative bit count;
use it to check how strongly the configured `--corrupt-ber` was actually applied. Both values reset
when a capture is restarted.

See the [Control API](#7-control-api-rest--named-pipe) chapter for details.

### Stats Log Files

With the `--stats-log` option, clumsy writes statistics to a file periodically while capturing. Useful for test reports and automated analysis in CI/CD pipelines.

```
clumsy.exe --filter "..." --stats-log stats.csv --stats-interval 1
```

| Option | Description |
|------|------|
| `--stats-log <file>` | Output file path. A `.json` extension writes a JSON array; anything else writes CSV |
| `--stats-interval <sec>` | Write interval (default: 1 second) |

#### CSV Format

```csv
elapsed_sec,captured,sent,pps,lag,jitter,drop,burstloss,blackout,throttle,duplicate,ood,tamper,corrupt,reset,bandwidth,lag_buf,jitter_buf,bw_buf,bw_limit_kbps
1.0,156,150,156,50,0,6,0,0,0,0,0,0,0,0,150,3,0,2,100
2.0,312,298,156,100,0,14,0,0,0,0,0,0,0,0,298,5,0,1,100
```

- First row: header (module columns use the `shortName`, in module processing order)
- `elapsed_sec`: seconds since the capture started
- `pps`: packets captured per second over the interval
- Module columns: cumulative packets handled (`affectedCount`)
- `lag_buf` / `jitter_buf` / `bw_buf`: internal buffer sizes at that moment
- `bw_limit_kbps`: the Bandwidth module's limit

#### JSON Format

```json
[
  {"elapsed_sec":1.0,"captured":156,"sent":150,"pps":156,"modules":{"lag":50,"jitter":0,"drop":6,...},"lag_buf":3,"jitter_buf":0,"bw_buf":2,"bw_limit_kbps":100},
  {"elapsed_sec":2.0,"captured":312,"sent":298,"pps":156,"modules":{"lag":100,...},"lag_buf":5,"jitter_buf":0,"bw_buf":1,"bw_limit_kbps":100}
]
```

> The file is closed automatically when the capture stops or the program exits.
> Logging starts afresh with every capture and overwrites an existing file at the same path.

---

## 5. Command Line (Automation)

clumsy can be driven entirely from command-line options. You do not have to open the dashboard,
and `--web off` turns the web server off altogether.

`clumsy.exe --help` prints the complete list at any time.

### Basic Form

```
clumsy.exe --filter "<filter>" --<module> on [--<module>-<option> <value>] ...
```

### Full Option List

```
--filter "<expr>"      WinDivert filter. Capture starts at once; without it clumsy waits for a filter from the dashboard/API
--timeout <sec>        quit automatically after N seconds
--scenario <file>      load a scenario JSON file (played when the capture starts)
--profile <name>       load and apply a profile from profiles.json
--process <name>       capture only one process's traffic (e.g. game.exe)

--lag on/off
--lag-time <ms>        delay (default: 50)
--lag-inbound on/off
--lag-outbound on/off

--jitter on/off
--jitter-min <ms>      minimum delay (default: 20)
--jitter-max <ms>      maximum delay (default: 100)
--jitter-inbound on/off
--jitter-outbound on/off
--jitter-dist uniform|normal|pareto   delay distribution (default: uniform)

--drop on/off
--drop-chance <%>      drop probability (default: 10.0)
--drop-inbound on/off
--drop-outbound on/off

--burstloss on/off
--burstloss-good <%>   drop probability in the Good state (default: 2.0)
--burstloss-bad <%>    drop probability in the Bad state  (default: 80.0)
--burstloss-gb <%>     Good->Bad transition probability   (default: 5.0)
--burstloss-bg <%>     Bad->Good transition probability   (default: 20.0)
--burstloss-inbound on/off
--burstloss-outbound on/off

--blackout on/off
--blackout-duration <ms>   outage length (default: 3000)
--blackout-gap <ms>        normal time between outages (default: 15000)
--blackout-trigger on      start/cancel an outage now (one-shot action)
--blackout-inbound on/off
--blackout-outbound on/off

--throttle on/off
--throttle-chance <%>  probability of starting a hold (default: 10.0)
--throttle-frame <ms>  hold length (default: 30)
--throttle-drop on/off discard held packets (default: off)
--throttle-inbound on/off
--throttle-outbound on/off

--duplicate on/off
--duplicate-chance <%> duplication probability (default: 10.0)
--duplicate-count <n>  number of copies, original included (default: 2, range: 2~50)
--duplicate-inbound on/off
--duplicate-outbound on/off

--ood on/off
--ood-chance <%>       probability of capturing into the buffer (default: 10.0)
--ood-buffer <n>       maximum packets held (default: 5, range: 2~50)
--ood-delay <ms>       maximum hold time in ms (default: 200, range: 10~2000)
--ood-inbound on/off
--ood-outbound on/off

--tamper on/off
--tamper-chance <%>       tamper probability (default: 10.0)
--tamper-position <0~3>   where to tamper: 0=Front, 1=Center (default), 2=Back, 3=Random
--tamper-checksum on/off  recompute checksums after tampering (default: on)
--tamper-inbound on/off
--tamper-outbound on/off

--corrupt on/off
--corrupt-chance <%>      probability of corrupting this packet (default: 10)
--corrupt-ber <ppm>       per-bit flip probability, parts per million (default: 100 = 0.01%)
--corrupt-checksum on/off recompute checksums after corruption (default: on)
--corrupt-inbound on/off
--corrupt-outbound on/off

--reset on/off
--reset-chance <%>     RST injection probability (default: 0)
--reset-next on        inject RST into the next TCP packet (one-shot action)
--reset-inbound on/off
--reset-outbound on/off

--bandwidth on/off
--bandwidth-bandwidth <KB/s>   bandwidth cap (default: 10)
--bandwidth-inbound on/off
--bandwidth-outbound on/off

--stats-log <file>     stats log file path (.csv or .json)
--stats-interval <sec> log interval in seconds (default: 1)
--stats-console <sec>  console summary interval (default: 10, 0 = off)

--pcap-out <file>          dump packets in libpcap format (Wireshark compatible)
--pcap-stage pre|post|both record before or after the modules (default: post)
--pcap-max-packets <n>     packet limit for the dump (0 = unlimited)
--pcap-max-bytes <n>       byte limit for the dump (0 = unlimited)

--replay-in <file>     reinject a saved libpcap file (independent of capture)
--replay-speed <x>     replay rate multiplier (default: 1.0)
--replay-loop on       start over when the end of the file is reached

--report-out <file>    write an HTML session report when the capture ends
--enable-plugins <dir> load custom module DLLs (off by default; mind the security implications)

--web off              disable the web dashboard
--web-port <n>         web server port (default: 8080)
--web-bind <addr>      bind address (default: 127.0.0.1; anything else forces token auth)
--web-token <token>    fixed auth token (reusable from CI scripts)
--pipe-name <name>     control pipe \\.\pipe\<name> (Linux: /run/<name>.sock), default clumsy

--verbose on/off       per-packet trace logging (on by default in Debug, off in Release)
--elevate on           relaunch through UAC when not already Administrator
--help                 print the option list and exit
--version              print the version and exit
```

Chances are percentages, not fractions: `--drop-chance 5` means 5%.
Linux builds have three more options: `--queue-num`, `--inject-mark` and `--auto-iptables` (see [docs/LINUX.md](docs/LINUX.md), Korean).

### Argument Validation and Exit Codes

The whole command line is checked before anything starts. This keeps a script or an AI agent (LLM)
from passing a wrong option and mistaking the result for success.

- **An unknown option** (`--lag-ms`, `--delay`, ...) is an error. The message suggests the right name
  (e.g. `clumsy: unknown option --delay - clumsy spells that --lag on --lag-time <ms>`).
- **An unusable value** (text where a number belongs, an unknown distribution name, a switch value other than `on`/`off`) is an error too.
- **An out-of-range number** is still clamped into range as before, but clumsy prints a warning and the value actually applied
  (e.g. `clumsy: warning: --lag-time: 99999 is outside 0..15000, clamped to 15000`).
- Switch values are case-insensitive (`on`/`off`, `true`/`false`, `yes`/`no`, `1`/`0`).
- If `--filter` was given and the capture cannot start (no Administrator rights, filter syntax error), clumsy exits immediately instead of waiting.
  Started without a filter, it still waits with the dashboard up as before.

| Exit code | Meaning |
|-----------|------|
| `0` | Clean exit (`--timeout` reached, Ctrl+C, `POST /api/quit`) |
| `2` | Bad command line: unknown option, unusable value, or a `--profile` / `--scenario` that could not be loaded |
| `3` | The capture (`--filter`) or replay (`--replay-in`) could not start (clumsy exits at once instead of waiting) |
| `4` | Clash with another running clumsy: its `--web-port` or `--pipe-name` is taken, or the filter overlaps another instance's capture (only the new instance exits) |

The module list in `--help` is generated from the ParamSpec tables in the code, so if it disagrees with the list above, `--help` is right.

> Note: `--tamper-position` is `0=Front, 1=Center (default), 2=Back, 3=Random`.
> (The UI dropdown up to 0.3 started at 1, but the API/CLI values start at 0.)

### Examples

```
# 100 ms of delay + 3% drop on the game server's UDP traffic (port 7777)
clumsy.exe --filter "udp and (udp.DstPort==7777 or udp.SrcPort==7777)" ^
           --lag on --lag-time 100 ^
           --drop on --drop-chance 3.0

# Run for 30 seconds only, then exit
clumsy.exe --filter "udp and outbound" --lag on --lag-time 200 --timeout 30

# 30-second test that logs stats to CSV every second
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 ^
           --stats-log stats.csv --stats-interval 1 --timeout 30

# Log stats to JSON every 5 seconds
clumsy.exe --filter "udp and outbound" --drop on --drop-chance 5.0 ^
           --stats-log result.json --stats-interval 5

# Dump packets to pcap and write a session report too (collect artifacts in CI)
clumsy.exe --filter "udp and outbound" --lag on --lag-time 120 ^
           --pcap-out capture.pcap --pcap-max-packets 50000 ^
           --report-out report.html --timeout 60

# Fully headless, with the web dashboard off
clumsy.exe --filter "udp and outbound" --drop on --drop-chance 5.0 ^
           --web off --stats-console 0 --timeout 30

# A CI runner controls a remote test machine (fixed token)
clumsy.exe --web-bind 0.0.0.0 --web-port 8080 --web-token ci-secret-token
```

---

## 6. Customizing Filter Presets

### config.json (Preferred Format)

Open `config.json` in the folder that holds `clumsy.exe` (the repository original is `etc/config.json`) in an editor to add filters you use often.

```json
{
  "filters": [
    { "name": "my game server",    "filter": "udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)" },
    { "name": "local server test", "filter": "udp and outbound and loopback" }
  ]
}
```

Add `{"name": "<name>", "filter": "<WinDivert filter>"}` objects to the `filters` array.

### config.txt (Legacy Format)

The old `config.txt` format is still supported for backward compatibility. When `config.json` is missing or fails to parse, `config.txt` is read instead.

Format: `<preset name>: <WinDivert filter>`

```
# examples
my game server: udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)
local server test: udp and outbound and loopback
```

> **Precedence**: `config.json` → `config.txt` → a built-in loopback filter. If both files exist, only `config.json` is used.

Restart clumsy after saving and the new entries appear in the Preset dropdown. Presets saved with the dashboard's **Save as preset...**
or `POST /api/presets` are written to `config.json` and appear right away, without a restart.
Up to 64 presets are read.

### Built-in Game Engine / Service Presets

config.json ships with presets for these game engines and services:

| Preset name | Protocol | Port | Target |
|-------------|----------|------|------|
| `unreal engine (7777)` | UDP | 7777 | Unreal Engine default game server |
| `unity netcode (9000)` | UDP | 9000 | Unity Netcode for GameObjects / Relay |
| `unity transport (7770-7780)` | UDP | 7770-7780 | Unity Transport default range |
| `photon engine` | UDP | 5055-5056 | Photon Realtime / PUN2 |
| `steam game` | UDP | 27000-27036 | Steam game traffic |
| `steam game+query` | UDP | 27000-27050 | Steam game + query ports |
| `epic online services` | UDP | 3478-3479 | Epic STUN/TURN |
| `godot enet (6007)` | UDP | 6007 | Godot Engine ENet |
| `mirror networking (7777)` | TCP | 7777 | Mirror Networking (Unity) |
| `fishnet (7770)` | UDP | 7770 | FishNet (Unity) |
| `minecraft java` | TCP | 25565 | Minecraft Java Edition |
| `minecraft bedrock` | UDP | 19132 | Minecraft Bedrock Edition |
| `game udp high ports` | UDP | 7000-8000 | A common game UDP port range |

### IPv6 Presets

Presets for game testing over IPv6 are included as well:

| Preset name | Description |
|-------------|------|
| `ipv6 all` | Every IPv6 packet |
| `ipv6 loopback (::1)` | IPv6 loopback (`::1`) traffic |
| `ipv6 tcp` | IPv6 TCP packets |
| `ipv6 udp` | IPv6 UDP packets |
| `ipv6 against specific ip` | One IPv6 address (example: `2001:db8::1`) |
| `ipv6 against port` | One port over IPv6 |
| `ipv6 unreal engine (7777)` | Unreal Engine over IPv6, UDP 7777 |
| `ipv6 unity netcode (9000)` | Unity Netcode over IPv6, UDP 9000 |
| `ipv6 steam game` | Steam games over IPv6, UDP 27000-27036 |
| `ipv6 game udp high ports` | Common game ports over IPv6, UDP 7000-8000 |

> The IP address (`2001:db8::1`) and port numbers in the IPv6 presets are examples. Edit config.json to match your environment.
>
> Presets that contain an IPv6 address, such as `ipv6 loopback (::1)` and `ipv6 against specific ip`, work **only in config.json**. The legacy config.txt format uses `:` as its separator, so it cannot parse filters containing IPv6 addresses (`::1`, `2001:db8::1`).

> If a preset's port numbers do not match your setup, open config.json and edit it or add a new entry.

---

## 7. Control API (REST + Named Pipe)

clumsy offers two control transports at the same time.

| Transport | Address | Use |
|-----------|------|------|
| **HTTP REST** | `http://127.0.0.1:8080` (default) | web dashboard, CI scripts, curl |
| **Named Pipe** | `\\.\pipe\clumsy` | existing automation scripts (compatible with 0.3 and earlier) |

Both transports call **the same control layer** (`controlapi.cpp`) internally,
so their behavior cannot drift apart. Existing Named Pipe scripts keep working unchanged.
On Linux a Unix domain socket (`/run/clumsy.sock`, falling back to `/tmp/clumsy.sock`) takes the
same JSON commands in place of the Named Pipe. With `--pipe-name <name>` the pipe is `\\.\pipe\<name>` and
the socket `/run/<name>.sock` (needed to run several instances).

---

### 7-1. REST API

Authentication depends on the bind address:

- **Localhost bind (default)**: no token needed. If `--web-token` is given, however, a token is required on localhost too.
- **External bind with `--web-bind`**: a token is **mandatory**.
  Set it with `--web-token`, or leave it out and one is generated at startup and printed as part of the dashboard address (`?token=...`) in the console banner.
  Pass it in the `X-Clumsy-Token` header or the `?token=` query string.
- With a token in force, every path needs it — the dashboard page and `llms.txt` included — except `GET /api/health` and `GET /metrics`.
  A missing or wrong token gets `401` and `{"status":"error","message":"missing or invalid token; ..."}`.

#### Endpoints

| Method / path | Auth | Description |
|---|---|---|
| `GET /api/health` | not required | Liveness probe. For CI health checks |
| `GET /metrics` | not required | Prometheus text exposition format (see below) |
| `GET /api/status` | required | Capture state, filter, process, last message |
| `GET /api/modules` | required | Every module's state + current params + ParamSpec form metadata |
| `GET /api/stats` | required | Live statistics (same payload as SSE) |
| `GET /api/stream` | required | Server-Sent Events, stats pushed every 200 ms |
| `GET /api/presets` | required | Filter presets from `config.json` |
| `GET /api/profiles` | required | Profile names from `profiles.json` |
| `GET /api/report` | required | HTML session report download |
| `GET /api/docs` | required | Endpoint list (JSON), with a description, example body and the API conventions for each |
| `GET /llms.txt` | required | Usage guide for AI agents (LLMs) (plain text, `web/llms.txt`) |
| `GET /llms_kr.txt` | required | The same guide in Korean (`web/llms_kr.txt`) |
| `POST /api/filter` | required | Set a filter and start capturing (restarts with the new filter if already capturing) |
| `POST /api/stop` | required | Stop capturing |
| `POST /api/quit` | required | Shut clumsy down |
| `POST /api/modules/{shortName}` | required | Enable a module / change its params |
| `POST /api/profiles` | required | Save the current state as a profile |
| `POST /api/profiles/{name}/apply` | required | Apply a profile |
| `POST /api/presets` | required | Save a filter preset (written to config.json) |
| `POST /api/profiles/{name}/delete` | required | Delete a profile (removed from `profiles.json`) |
| `POST /api/apply` | required | Set several modules at once without saving anything |
| `POST /api/scenario/load` | required | Load a scenario file |
| `POST /api/scenario/loadinline` | required | Load a scenario from the request body, without a file |
| `POST /api/scenario/start` \| `stop` | required | Control scenario playback |
| `POST /api/pcap/start` \| `stop` | required | Control the pcap dump |
| `POST /api/replay/start` \| `stop` | required | Control pcap replay |

#### Response Rules

Every `/api` reply is JSON with `"status"` set to `"ok"` or `"error"` (plus `"message"`). An unknown path or a
wrong method also gets a JSON error (`404`, `405`) that says what to use instead. The only methods are GET and POST,
and POST bodies are JSON objects.

```
GET /api/stop
{"status":"error","message":"no GET endpoint at /api/stop. Actions (filter, stop, quit, modules/{name}, apply, ...) are POST. GET /api/docs lists every endpoint with an example body."}
```

A settings request (`POST /api/modules/{shortName}`, `POST /api/apply`) can be `ok` and still not have been applied
exactly as sent, so check these fields:

| Field | Meaning |
|------|------|
| `warnings` | Values that were clamped into range, or refused as unusable (`- not applied`), with an explanation |
| `unknownKeys` / `validKeys` | Keys the module does not have, and the keys it does take (`validKeys` only from `POST /api/modules/{shortName}`) |
| `params` | The values in effect after the request (for `/api/apply`, only the keys it accepted) |
| `capturing` | `false` means the setting was stored, but with no capture running it does not affect traffic yet |

If **none** of the requested keys could be applied (all unknown, or all unusable values), the reply is a `400` error.
An `ok` with `applied: 0` would read as success to a script that only checks the status.

The `scenario` member of `POST /api/scenario/loadinline` can be sent as a plain JSON array (recommended) or, as before,
as that array wrapped in a string.

Calling `POST /api/scenario/start` with no capture running returns `ok` with a `warnings` entry saying that
no step will fire until a capture starts, because step times are measured from the capture start.

```
POST /api/modules/lag  {"lag-time":99999,"lag-ms":5}
{"status":"ok","module":"lag","applied":1,"enabled":false,"capturing":false,
 "params":{"lag-time":"15000","lag-inbound":"true","lag-outbound":"true"},
 "unknownKeys":"lag-ms","validKeys":"lag-inbound,lag-outbound,lag-time",
 "warnings":["lag-time: 99999 is outside 0..15000, clamped to 15000"]}
```

Scenario loads (`POST /api/scenario/load`, `loadinline`) also report unknown keys, bad values and unknown metrics
in `warnings`. Previously these only showed up on the console when the step fired.

#### Examples (curl)

```bash
# Read the status
curl http://127.0.0.1:8080/api/status

# Start capturing
curl -X POST http://127.0.0.1:8080/api/filter \
     -H "Content-Type: application/json" \
     -d "{\"filter\":\"udp and outbound\",\"process\":\"game.exe\"}"

# Turn lag on with a 200 ms delay
curl -X POST http://127.0.0.1:8080/api/modules/lag \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"lag-time\":200}"

# Remote instance (token required)
curl -H "X-Clumsy-Token: ci-secret-token" http://10.0.0.5:8080/api/stats

# CI health check (no auth)
curl -f http://10.0.0.5:8080/api/health || echo "clumsy is down"

# Stop capturing and collect the report
curl -X POST http://127.0.0.1:8080/api/stop
curl -o report.html http://127.0.0.1:8080/api/report

# Set several modules at once without creating a profile
curl -X POST http://127.0.0.1:8080/api/apply \
     -H "Content-Type: application/json" \
     -d "{\"lag\":true,\"lag-time\":200,\"drop\":true,\"drop-chance\":5.0}"

# Reinject a saved pcap at double speed
curl -X POST http://127.0.0.1:8080/api/replay/start \
     -H "Content-Type: application/json" \
     -d "{\"path\":\"capture.pcap\",\"speed\":2.0}"

# Prometheus scrape (no auth)
curl http://127.0.0.1:8080/metrics
```

#### Prometheus Integration

`GET /metrics` returns the Prometheus text exposition format (v0.0.4).
**It needs no authentication** — the counters are read-only, and every scrape configuration
expects to reach the endpoint without extra steps. To enforce the token on an external
bind, move the corresponding block in `httpserver.cpp` below the
`isAuthorized()` check.

```yaml
# prometheus.yml
scrape_configs:
  - job_name: clumsy
    static_configs:
      - targets: ['10.0.0.5:8080', '10.0.0.6:8080']
    metrics_path: /metrics
```

Exposed series:

| Metric | Type | Meaning |
|--------|------|------|
| `clumsy_up` | gauge | Always 1 (the scrape succeeded) |
| `clumsy_capturing` | gauge | 1 while a capture is running |
| `clumsy_captured_packets_total` | counter | Packets captured |
| `clumsy_sent_packets_total` | counter | Packets reinjected |
| `clumsy_capture_elapsed_seconds` | gauge | Time since the capture started |
| `clumsy_lag_buffer_packets` etc. | gauge | Internal buffer size per module |
| `clumsy_pcap_written_packets_total` | counter | Packets written to the pcap file |
| `clumsy_pcap_written_bytes_total` | counter | Bytes written to the pcap file |
| `clumsy_replay_active` | gauge | 1 while a pcap replay is running |
| `clumsy_replay_injected_packets_total` | counter | Packets injected by replay |
| `clumsy_replay_failed_packets_total` | counter | Replay packets that could not be injected |
| `clumsy_corrupt_flipped_bits_total` | counter | Cumulative bits flipped by the Corrupt module |
| `clumsy_module_enabled{module="..."}` | gauge | Module on/off |
| `clumsy_module_affected_packets_total{module="..."}` | counter | Packets a module handled |
| `clumsy_latency_ms` | histogram | Delay distribution (`_bucket`/`_sum`/`_count`) |

The module name is a **label** rather than part of the metric name, so a single Grafana panel
can cover every module.

```promql
# Per-module throughput
rate(clumsy_module_affected_packets_total[1m])

# Delay p95 (straight from the histogram)
histogram_quantile(0.95, rate(clumsy_latency_ms_bucket[5m]))

# Find test machines whose capture has stopped
clumsy_capturing == 0
```

#### Subscribing to the Live Stream (Python)

```python
import requests, json

with requests.get("http://127.0.0.1:8080/api/stream", stream=True) as r:
    for line in r.iter_lines(decode_unicode=True):
        if line and line.startswith("data: "):
            stats = json.loads(line[6:])
            print(stats["captured"], stats["sent"])
```

#### Generating Forms from ParamSpec

`GET /api/modules` includes each module's input form definition:

```json
{
  "shortName": "drop",
  "displayName": "Drop",
  "description": "random packet loss, evenly spread",
  "enabled": true,
  "affected": 132,
  "params": { "drop-chance": "10.0", "drop-inbound": "true", "drop-outbound": "true" },
  "paramSpecs": [
    { "key": "drop-inbound",  "label": "Inbound",    "type": "bool",    "min": 0, "max": 0 },
    { "key": "drop-outbound", "label": "Outbound",   "type": "bool",    "min": 0, "max": 0 },
    { "key": "drop-chance",   "label": "Chance (%)", "type": "percent", "min": 0, "max": 100 }
  ]
}
```

`type` is one of `int` / `float` / `percent` / `bool` / `action` / `enum`.
`action` is a one-shot trigger with no value, which the dashboard renders as a button
(e.g. `blackout-trigger`, `reset-next`).

`enum` picks one of a fixed set of names; the choices come along in an `options`
array. The dashboard draws it as a dropdown — so the form is built without knowing
anything about the module.

```json
{ "key": "jitter-dist", "label": "Distribution", "type": "enum",
  "min": 0, "max": 0, "options": ["uniform", "normal", "pareto"] }
```

---

### 7-2. Named Pipe API

On startup clumsy opens a Named Pipe server (`\\.\pipe\clumsy`) that external processes can drive with JSON commands. Use it to control clumsy from CI/CD pipelines or automated test scripts.

### Protocol

Connection: client connects → sends a JSON request → receives a JSON reply → disconnects

### Supported Commands

#### `set` — Change Module Settings

```json
{"cmd":"set","module":"<module>","enabled":true,"<param-key>":<value>}
```

`module` is the module's shortName (lag, jitter, drop, burstloss, blackout, throttle, duplicate, ood, tamper, corrupt, reset, bandwidth).

If an `enabled` field is present it switches the module on or off. Every other field is applied as one of the module's parameters.

**Examples**:
```json
{"cmd":"set","module":"lag","enabled":true,"lag-time":150}
{"cmd":"set","module":"drop","enabled":true,"drop-chance":5.0}
{"cmd":"set","module":"jitter","jitter-min":20,"jitter-max":150}
{"cmd":"set","module":"burstloss","enabled":true,"burstloss-good":1.0,"burstloss-bad":70.0,"burstloss-gb":10.0,"burstloss-bg":30.0}
{"cmd":"set","module":"blackout","blackout-duration":2000,"blackout-gap":10000}
{"cmd":"set","module":"blackout","blackout-trigger":true}
{"cmd":"set","module":"bandwidth","bandwidth-bandwidth":500}
{"cmd":"set","module":"lag","enabled":false}
```

**Parameters each module accepts**:

| Module | Parameter key | Type | Range |
|------|------------|------|------|
| lag | lag-time | integer (ms) | 0~15000 |
| jitter | jitter-min, jitter-max | integer (ms) | 0~5000 |
| jitter | jitter-dist | name | uniform / normal / pareto |
| drop | drop-chance | real (%) | 0.0~100.0 |
| burstloss | burstloss-good, burstloss-bad, burstloss-gb, burstloss-bg | real (%) | 0.0~100.0 |
| blackout | blackout-duration | integer (ms) | 100~60000 |
| blackout | blackout-gap | integer (ms) | 1000~300000 |
| blackout | blackout-trigger | true (one-shot action) | — |
| throttle | throttle-chance | real (%) | 0.0~100.0 |
| throttle | throttle-frame | integer (ms) | 0~1000 |
| throttle | throttle-drop | true/false | — |
| duplicate | duplicate-chance | real (%) | 0.0~100.0 |
| duplicate | duplicate-count | integer | 2~50 |
| ood | ood-chance | real (%) | 0.0~100.0 |
| ood | ood-buffer | integer | 2~50 |
| ood | ood-delay | integer (ms) | 10~2000 |
| tamper | tamper-chance | real (%) | 0.0~100.0 |
| tamper | tamper-position | integer | 0=Front,1=Center,2=Back,3=Random |
| tamper | tamper-checksum | true/false | — |
| corrupt | corrupt-chance | real (%) | 0.0~100.0 |
| corrupt | corrupt-ber | integer (ppm) | 0~1000000 |
| corrupt | corrupt-checksum | true/false | — |
| reset | reset-chance | real (%) | 0.0~100.0 |
| reset | reset-next | true (one-shot action) | — |
| bandwidth | bandwidth-bandwidth | integer (KB/s) | 0~99999 |
| (every module) | `<module>-inbound`, `<module>-outbound` | true/false | — |

**Reply**:
```json
{"status":"ok","module":"lag"}
```

A request with nothing wrong in it gets exactly the 0.3 reply. Only when a key is unknown or a value was clamped
are `unknownKeys` / `warnings` added:
`{"status":"ok","module":"lag","unknownKeys":"lag-ms","warnings":["lag-time: 99999 is outside 0..15000, clamped to 15000"]}`

#### `get_stats` — Read Current State

```json
{"cmd":"get_stats"}
```

**Reply**:
```json
{
  "status": "ok",
  "modules": {
    "lag":       {"enabled": true,  "affected": 1500},
    "jitter":    {"enabled": false, "affected": 0},
    "drop":      {"enabled": true,  "affected": 120},
    ...
  },
  "captured": 5000,
  "sent": 4880
}
```

- `affected`: cumulative packets the module has handled (delayed, dropped, tampered, ...)
- `captured`: total packets captured
- `sent`: total packets sent

#### `stop` — Shut Down clumsy

```json
{"cmd":"stop"}
```

**Reply**:
```json
{"status":"ok","message":"stopping"}
```

clumsy exits within about 200 ms of the reply.

#### Commands Added in 0.4

The request and reply formats of the original three commands (`set` / `get_stats` / `stop`) are unchanged.
The commands below were added so the pipe can do everything the REST API does.

| Command | Description |
|------|------|
| `{"cmd":"filter","filter":"udp and outbound","process":"game.exe"}` | Set a filter and start capturing (`"cmd":"start"` does the same) |
| `{"cmd":"stop_capture"}` | Stop capturing only (clumsy keeps running) |
| `{"cmd":"quit"}` | Shut clumsy down (same as `stop`) |
| `{"cmd":"get_status"}` | Capture state / filter / last message |
| `{"cmd":"get_modules"}` | Every module's state + ParamSpec |
| `{"cmd":"get_presets"}` | Filter preset list |
| `{"cmd":"get_profiles"}` | Profile name list |
| `{"cmd":"profile","name":"mobile-4g"}` | Apply a profile |
| `{"cmd":"delete_profile","name":"mobile-4g"}` | Delete a profile |
| `{"cmd":"apply","lag":true,"lag-time":200}` | Set several modules at once (same as `POST /api/apply`) |
| `{"cmd":"scenario","action":"load","path":"s.json"}` | Load a scenario (`action`: load/start/stop) |
| `{"cmd":"pcap","action":"start","path":"out.pcap"}` | Control the pcap dump (`action`: start/stop) |
| `{"cmd":"replay","action":"start","path":"in.pcap"}` | Control pcap replay (`action`: start/stop) |
| `{"cmd":"metrics"}` | Prometheus exposition text (returned wrapped in a JSON string field) |

Saving a profile, saving a preset and loading a scenario from the request body (`loadinline`) exist only in REST.

> **Important**: `stop` means **shut the program down**, exactly as in 0.3.
> To stop only the capture, use `stop_capture`.

> **Every reply is JSON, without exception.** That includes `metrics`, which comes back like this:
> `{"status":"ok","metrics":"# HELP clumsy_up ...\n..."}`
> If a Prometheus scraper needs the raw text, use REST `GET /metrics`.
> Requests larger than 8 KB are handled normally; beyond 256 KB, instead of being silently truncated, they get
> `{"status":"error","message":"request exceeds the 262144 byte limit"}`.

### Python Example

```python
import socket, json

PIPE = r'\\.\pipe\clumsy'

def send_command(cmd: dict) -> dict:
    """Send a JSON command to the clumsy Named Pipe and return the reply."""
    payload = json.dumps(cmd).encode()
    # Python on Windows: use win32pipe or ctypes for named pipe
    # this simple example goes through PowerShell via subprocess
    import subprocess, tempfile, os
    script = f"""
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'clumsy', 'InOut')
$pipe.Connect(2000)
$w = New-Object System.IO.StreamWriter($pipe); $w.AutoFlush=$true
$r = New-Object System.IO.StreamReader($pipe)
$w.Write('{json.dumps(cmd)}')
$resp = $r.ReadLine()
$pipe.Close()
Write-Output $resp
"""
    result = subprocess.run(['powershell', '-Command', script],
                            capture_output=True, text=True)
    return json.loads(result.stdout.strip())

# Example: turn lag on with a 200 ms delay
resp = send_command({"cmd":"set","module":"lag","enabled":True,"lag-time":200})
print(resp)  # {"status": "ok", "module": "lag"}

# Example: read the stats
stats = send_command({"cmd":"get_stats"})
print(stats)

# Example: shut down
send_command({"cmd":"stop"})
```

### PowerShell Example

```powershell
function Send-ClumsyCommand($cmd) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', 'clumsy',
        [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(2000)
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($pipe)

    $writer.Write(($cmd | ConvertTo-Json -Compress))
    $response = $reader.ReadLine()
    $pipe.Close()
    return $response | ConvertFrom-Json
}

# Usage
Send-ClumsyCommand @{cmd="set"; module="lag"; enabled=$true; "lag-time"=100}
Send-ClumsyCommand @{cmd="set"; module="drop"; enabled=$true; "drop-chance"=5.0}
$stats = Send-ClumsyCommand @{cmd="get_stats"}
Send-ClumsyCommand @{cmd="stop"}
```

---

## 8. Scenario Scripting

A scenario is a JSON file that changes network conditions automatically over time. It is useful when a QA team needs to reproduce the same conditions repeatedly, or when CI/CD should run a gradual-degradation test unattended.

### Usage

```
clumsy.exe --filter "udp and outbound" --scenario scenario.json
```

The scenario plays automatically once the capture starts (and from the beginning every time the capture is restarted). When every step has run, playback is complete and the last settings stay in effect.
If the file cannot be read or contains no usable step, nothing is started and clumsy exits with code `2`.
Problems found after loading (unknown keys, bad values, unknown metrics) are printed on the console as `scenario: WARNING ...`.

#### Scenario Editor in the Web Dashboard

You can compose steps in the browser and run them without writing a file.
In the **SCENARIO EDITOR** section at the bottom of the dashboard:

1. Add a step with `+ Add step`
2. Choose the trigger: `at time` or `when metric` (condition)
3. Add the settings to change with `+ change` — the dropdown lists **every module's on/off switch and parameters**.
   The list is built from the ParamSpec in `GET /api/modules`, so new modules
   show up automatically
4. Press `Load into clumsy` or `Load & start`

The **Scenario JSON** box below shows exactly the document that will be sent. You can edit it directly,
paste in an existing scenario, or save it to a file and use it with `--scenario`.
(The builder only overwrites the box when you change a step.)

Over REST, send the array of steps as-is in `POST /api/scenario/loadinline`
(the older form, with the array wrapped in a string, is accepted too):

```bash
curl -X POST http://127.0.0.1:8080/api/scenario/loadinline \
     -H "Content-Type: application/json" \
     -d '{"scenario":[{"at":5,"lag":true,"lag-time":250}]}'
```

`steps` in the reply is the number of steps loaded, and `warnings` is added when something is wrong:
`{"status":"ok","steps":2,"warnings":["step when=captured: unknown metric, it always reads 0"]}`

### File Format

```json
[
  { "at": seconds, "key": value, ... },
  ...
]
```

- `"at"`: how many seconds after the capture starts to apply the step (integer, seconds)
- `"<module>": true/false`: switches that module on or off (use the module's shortName)
- `"<param-key>": value`: changes a module parameter (same key names as the control API)
- Keys starting with `"_"` (`"_comment"` and so on) are treated as comments and ignored, since JSON has no comment syntax.

Module shortNames: `lag`, `jitter`, `drop`, `burstloss`, `blackout`, `throttle`, `duplicate`, `ood`, `tamper`, `corrupt`, `reset`, `bandwidth`

### Trigger Types (Extended in 0.4)

Each step has **exactly one trigger**.

#### 1) Time Trigger — `"at"`

Unchanged from before. The step applies once the given number of seconds has passed since the capture started.

```json
{ "at": 10, "lag": true, "lag-time": 200 }
```

#### 2) Condition Trigger — `"when"` / `"op"` / `"value"`

In real QA automation, conditions such as "once N packets have gone through" or "when a buffer overflows" are often more useful than time.

```json
{ "when": "captured_count", "op": ">=", "value": 10000,
  "drop": true, "drop-chance": 15.0 }
```

- `"op"`: `>=` (default), `>`, `<=`, `<`, `==`
- `"value"`: the number to compare against

Available metrics (`when`):

| Metric | Meaning |
|------|------|
| `captured_count` | Total packets captured |
| `sent_count` | Total packets sent |
| `pps` | Packets captured per second (refreshed about once a second) |
| `elapsed_sec` | Seconds since the capture started (real number) |
| `lag_buf` | Lag module internal buffer size |
| `jitter_buf` | Jitter module internal buffer size |
| `bandwidth_buf` | Bandwidth module queue size |
| `affected:<module>` | Cumulative packets that module has handled (e.g. `affected:drop`) |

#### 3) Repeat — `"repeat"` / `"times"`

After a step fires, it fires again every `repeat` seconds. `times` limits how often
(absent or `0` means forever).

```json
{ "at": 30, "repeat": 30, "times": 5, "drop-chance": 12.0 }
```

Both time and condition triggers can take `repeat`.
The scenario stops by itself once every repeating step is used up.

### Example Files

#### Basic Gradual Degradation

```json
[
  { "at": 0,  "lag": true,  "lag-time": 50 },
  { "at": 10, "lag-time": 150, "jitter": true, "jitter-min": 20, "jitter-max": 100 },
  { "at": 20, "drop": true, "drop-chance": 5.0 },
  { "at": 30, "lag-time": 300, "drop-chance": 10.0 },
  { "at": 45, "lag": false, "jitter": false, "drop": false }
]
```

Step by step:
- t=0s: 50 ms of delay
- t=10s: delay up to 150 ms, plus jitter
- t=20s: 5% drop added
- t=30s: worse — 300 ms delay, 10% drop
- t=45s: everything off

#### Mobile Degradation with Outages

```json
[
  { "at": 0,  "lag": true, "lag-time": 80,
              "jitter": true, "jitter-min": 10, "jitter-max": 60 },
  { "at": 15, "burstloss": true,
              "burstloss-good": 1.0, "burstloss-bad": 60.0,
              "burstloss-gb": 8.0,  "burstloss-bg": 25.0 },
  { "at": 30, "blackout": true,
              "blackout-duration": 3000, "blackout-gap": 10000 },
  { "at": 60, "blackout": false, "burstloss": false }
]
```

#### Bandwidth Throttling

```json
[
  { "at": 0,  "bandwidth": true, "bandwidth-bandwidth": 2000 },
  { "at": 20, "bandwidth-bandwidth": 500 },
  { "at": 40, "bandwidth-bandwidth": 100 },
  { "at": 60, "bandwidth": false }
]
```

#### Condition-Based Scenario (0.4)

This example changes conditions based on when load has actually built up.
It reproduces the same "amount of traffic" regardless of how fast the test machine is.

```json
[
  { "at": 0, "lag": true, "lag-time": 50 },

  { "when": "captured_count", "op": ">=", "value": 20000,
    "bandwidth": true, "bandwidth-bandwidth": 200 },

  { "when": "lag_buf", "op": ">", "value": 500,
    "lag-time": 20 },

  { "when": "affected:drop", "op": ">=", "value": 1000,
    "drop": false },

  { "at": 60, "repeat": 20, "times": 3,
    "blackout": true, "blackout-trigger": true }
]
```

The last step switches Blackout on the first time it fires, then toggles an outage with `blackout-trigger`
twice more, 20 seconds apart. The first trigger, sent in the same step that switches the module on, is cleared when
the module starts (see [Blackout](#blackout--outages)), so the immediate outage only happens from the second firing on.

`etc/scenario-example.json` is an example file that uses time, condition and repeat triggers along with `_comment` notes.

### Notes

- Steps with the same `"at"` value are applied in file order
- Condition steps are evaluated every tick, in file order
- Time resolution is about 200 ms (the main tick)
- If a step has both `at` and `when`, `when` wins (to avoid firing twice)
- At most 128 steps, 32 parameters per step, 64 KB per file
- A step with no trigger (`at`/`when`) is skipped and reported as a warning
- Combined with `--timeout`, clumsy can exit on its own after the scenario:
  ```
  clumsy.exe --filter "udp and outbound" --scenario test.json --timeout 70
  ```
- Scenarios can also be loaded, started and stopped from the dashboard
  (the PROFILES, SCENARIO AND CAPTURE FILE section)

---

## 9. Saving and Loading Profiles

Save a combination of module settings you use often under a name, and load it straight from the UI dropdown or the command line.

### File Location

`profiles.json` — created and loaded automatically in the same directory as the clumsy executable.

### File Format

```json
{
  "mobile-4g": {
    "lag": true,
    "lag-time": 80,
    "drop": true,
    "drop-chance": 2.0,
    "bandwidth": true,
    "bandwidth-bandwidth": 5000
  },
  "bad-wifi": {
    "lag": true,
    "lag-time": 50,
    "jitter": true,
    "jitter-min": 20,
    "jitter-max": 200,
    "burstloss": true
  }
}
```

Each profile consists of module switches (`"<module>": true`) and parameter key-value pairs. The key names are the same as the command-line options (e.g. `lag-time`, `drop-chance`).

### Using Profiles in the Web Dashboard

1. **Load**: pick a profile in the Profile dropdown of the **PROFILES** section and press **Apply**.
   Module switches and parameter values are all applied, and the module list refreshes immediately.
2. **Save**: type a name in "Save current as" and press **Save**; the currently enabled modules and their
   parameter values are saved to `profiles.json`. Saving under an existing name overwrites it.
3. **Delete**: pick one in the dropdown and press **Delete** to remove it from `profiles.json`.

The API equivalents are `POST /api/profiles/{name}/apply`, `POST /api/profiles` (body: `{"name":"..."}`)
and `POST /api/profiles/{name}/delete`.

### Using Profiles from the Command Line

```
clumsy.exe --filter "udp and outbound" --profile mobile-4g
```

`--profile` enables the modules defined in the profile and sets their parameters. A capture has to be running for it to affect traffic, so it is usually combined with `--filter`; other module options can be added as well (the profile is applied last, so it wins for any key both set).
A name that is not in `profiles.json` prints the saved profiles and exits with code `2`.

### Notes

- Without `profiles.json` the profile dropdown is simply empty (no error)
- Since 0.4, profiles also store the direction flags (`lag-inbound` and so on)
- Up to 32 profiles, and up to 32 keys per profile (module switches included)
- Only enabled modules are recorded when saving. That is why applying a profile leaves modules it does not mention unchanged
  (to switch a module off, add `"<module>": false` to the JSON yourself)
- You can also add or edit profiles by editing the JSON file directly

---

## 10. pcap Packet Dump

The stats log (`--stats-log`) only keeps aggregate numbers. When you need to see the actual packets,
dump them to a standard libpcap file and analyze it in Wireshark or similar tools.

### Usage

```
clumsy.exe --filter "udp and outbound" --pcap-out capture.pcap
```

In the dashboard, enter a file path in the pcap field of the **PROFILES, SCENARIO AND CAPTURE FILE** section
and use **Start dump** / **Stop dump**.
The API is `POST /api/pcap/start` (body: `{"path":"out.pcap","maxPackets":0,"maxBytes":0}`) and
`POST /api/pcap/stop`.

### Recording Point (`--pcap-stage`)

| Value | When packets are recorded | Use |
|----|----------|------|
| `post` (default) | after all modules, just before the packet goes out on the network | to see what the other side actually receives |
| `pre` | right after capture, before any module touches it | to keep the original traffic |
| `both` | at both points | to compare before and after (the file gets bigger) |

With `both` the same packet is written twice, so in Wireshark, in time order,
they appear as "original → modified" pairs.

### Size Limits

An unlimited dump can fill the disk, so setting a limit is recommended.

```
clumsy.exe --filter "udp" --pcap-out capture.pcap ^
           --pcap-max-packets 100000 --pcap-max-bytes 104857600
```

When a limit is reached the file is **closed properly** and a message is printed on the console.
The capture itself keeps going.

### File Format

- The link type is `LINKTYPE_RAW` (101). WinDivert hands over bare IPv4/IPv6
  datagrams without an Ethernet frame, so RAW is the accurate description.
- Wireshark recognizes it automatically. It is normal for it to report no Ethernet header.
- The snap length is 65535 bytes.

### pcap Replay

A saved pcap can be **injected again**. Use it to turn a bug that only reproduces with a particular traffic sequence
from an anecdote into a repeatable test.

```bash
# Replay with the original timing
clumsy.exe --replay-in capture.pcap

# Double speed, looping at the end of the file
clumsy.exe --replay-in capture.pcap --replay-speed 2.0 --replay-loop on
```

REST: `POST /api/replay/start` `{"path":"capture.pcap","speed":1.0,"loop":false}`,
`POST /api/replay/stop`. Progress shows up in the `replay` member of `GET /api/stats`
(`read` / `sent` / `failed`).

**How it works**

- The file is **streamed**. Even a capture of hundreds of MB keeps only one packet in memory.
- The gaps between record timestamps are honoured, but instead of sleeping a fixed time between packets,
  playback is **aligned to the original timeline**. Otherwise the injection cost accumulates and
  playback falls further and further behind the original.
- Replay is **independent** of capture. It works with no filter set.

**Supported link types**

| Value | Format | Handling |
|----|------|------|
| 101 | `LINKTYPE_RAW` | the format clumsy itself writes; injected as-is |
| 1 | `LINKTYPE_ETHERNET` | the 14-byte Ethernet header is stripped before injection (tcpdump/Wireshark default) |
| 228 / 229 | IPv4 / IPv6 | injected as-is |

Frames in an Ethernet file that are not IP (ARP, VLAN tags and so on) are not injected and are counted
as `failed`. **pcapng is not supported** — when saving from Wireshark, choose
"Wireshark/tcpdump/... - pcap". A bad file is rejected at the magic-number check before playback
starts, and the reason is included in the reply.

**Platform constraints**

| | Windows | Linux |
|---|---------|-------|
| Injection path | a dedicated send-only WinDivert handle | a raw socket with an fwmark |
| Privilege | Administrator | `CAP_NET_RAW` |
| Direction | outbound (marked with the Impostor flag) | **outbound only** |
| Recapturing itself | `and not impostor` is appended to the capture filter automatically | excluded by an fwmark ACCEPT rule |
| IPv6 header | sent exactly as recorded | `IPV6_HDRINCL` (kernel 4.5 or later) |

**Replayed traffic is not captured again.** Replayed packets leave through a send-only handle separate
from the capture handle, so they used to come straight back in through a capture filter running at the same time,
get lag/drop and the rest applied twice, and inflate the stats, pcap and report figures. Now on Windows
`appStartCapture()` wraps the user filter in parentheses and appends `and not impostor` automatically.
If you deliberately want to capture traffic injected by another WinDivert tool, put the word
`impostor` in your filter yourself and the automatic clause is turned off.
On Linux a rule that ACCEPTs the replay socket's fwmark does the same job, and
`--auto-iptables on` installs it for both IPv4 and IPv6.

Inbound replay is impossible on Linux for the same reason as with the duplicate module —
a raw socket can only create and send traffic; pushing it into the local receive path
needs a TUN device or an ifb redirect.

> **Addresses go out unchanged**: the recorded source and destination IPs are used without modification.
> Replaying a file captured on another network sends real packets to those addresses,
> so replay only where the addresses make sense.

---

## 11. HTML Session Report

After a test, generate a single HTML file you can share with the team showing
"under what conditions, and how much, we tested". It references no external resources,
so it opens as-is when attached to an issue tracker or sent by email.

### Generating a Report

**Command line** — generated automatically when the capture ends (Stop or `--timeout`):

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 120 ^
           --report-out report.html --timeout 60
```

**Web dashboard** — the **Download report** link on the right of the header
(`GET /api/report`). You can download a report of the session so far even while the capture is running.

### Report Contents

| Section | Content |
|------|------|
| Header | clumsy version, session start time, duration |
| Capture | the filter expression applied (process filter included) |
| Cards | total packets captured/sent, average packets per second |
| Throughput | packets-per-second chart (inline SVG, no chart library) |
| Delay distribution | delay histogram bar chart + min/mean/p50/p95/p99/max cards |
| Modules | each module's state at the end, packets handled, and all of its parameters |
| Timeline | session start/stop and the scenario steps that fired |
| Packet capture | file path and packet count when pcap was used |
| Packet replay | packets read/injected/skipped when replay was used |

When a scenario is used, the timeline records **the second at which each step actually fired**,
so you can check afterwards when a condition trigger kicked in.

### Limitations

- Chart samples are recorded once a second, up to 600 (10 minutes).
  Longer sessions only chart the first 10 minutes; the totals stay exact.
- At most 256 timeline events.

---

## 12. Remote Control (Distributed QA)

A setup in which a CI runner drives clumsy on a remote test client.

### On the Test Machine

```
clumsy.exe --web-bind 0.0.0.0 --web-port 8080 --web-token ci-secret-token
```

- When `--web-bind` is not localhost, **token authentication is enforced**.
- Without `--web-token` a token is generated and printed on the console.
  In CI, fix the value so scripts can reuse it.
- An external bind prints a security warning (`*** SECURITY WARNING ***`) on the console.

### On the CI Runner

```bash
CLUMSY=http://10.0.0.5:8080
TOKEN=ci-secret-token

# 1. Check it is alive (no auth)
curl -f $CLUMSY/api/health || exit 1

# 2. Start the capture, then set conditions
curl -sf -X POST $CLUMSY/api/filter -H "X-Clumsy-Token: $TOKEN" \
     -H "Content-Type: application/json" \
     -d '{"filter":"udp and outbound"}'
curl -sf -X POST $CLUMSY/api/modules/lag -H "X-Clumsy-Token: $TOKEN" \
     -H "Content-Type: application/json" \
     -d '{"enabled":true,"lag-time":150}'

# 3. Run the tests ...

# 4. Clean up and collect artifacts
curl -sf -X POST $CLUMSY/api/stop -H "X-Clumsy-Token: $TOKEN"
curl -sf -o report.html $CLUMSY/api/report -H "X-Clumsy-Token: $TOKEN"
```

### Security Notes

clumsy runs as Administrator and can manipulate network traffic.
Use remote binding **only on a trusted, isolated test network**.

- The token travels over plain HTTP (no TLS). Do not expose the port to the internet.
- Restricting the port at the firewall to the CI runner's IP is recommended.
- The only endpoints reachable without authentication are `/api/health` (which exposes nothing but the version and whether a capture is running) and `/metrics` (read-only counters).
  Locking down `/metrics` as well requires the source change described under [Prometheus Integration](#prometheus-integration).

---

## 13. Custom Module Plugins (Optional)

Loads an experimental module from a DLL without recompiling.
It is **disabled by default** and can only be turned on with `--enable-plugins <directory>`.

### Security Warning

A plugin DLL runs with **the same privileges as clumsy (Administrator)**.
Only load DLLs you built yourself or trust completely.
When enabled, the console prints a warning along with the name of every DLL it loads.

### Writing a Plugin

The DLL exports a single function:

```cpp
#include "common.h"

static volatile short myEnabled = 0;

static void myStartUp() { /* ... */ }
static void myCloseDown(PacketNode *head, PacketNode *tail) { /* ... */ }
static short myProcess(PacketNode *head, PacketNode *tail) { /* ... */ return 0; }
static int  mySetParam(const char *key, const char *value) { return 0; }
static int  myGetParams(ParamKV *kv, int maxKv) { return 0; }

static const ParamSpec mySpecs[] = {
    { "drop-chance", "Chance (%)", "percent", 0, 100 },
};

static Module myModule = {
    "My Drop", "drop", (short*)&myEnabled,
    myStartUp, myCloseDown, myProcess,
    mySetParam, myGetParams,
    mySpecs, 1,
    0, 0, 0
};

extern "C" __declspec(dllexport) Module* clumsyGetModule(void) {
    return &myModule;
}
```

### Usage

```
clumsy.exe --enable-plugins .\plugins --filter "udp and outbound"
```

Every `*.dll` in the directory is scanned, and only those that export `clumsyGetModule` are loaded.
On Linux clumsy looks for `*.so` instead; drop `__declspec(dllexport)` from the export declaration.

### Current Limitations

- A plugin can only register by **replacing an existing built-in module**.
  Its `shortName` must match one of the built-in modules; otherwise it is skipped.
  (The module table size `MODULE_CNT` is a compile-time constant.
  Growing the table is future work.)
- At most 8 plugins are loaded.
- A module with a required field left empty (`startUp` / `closeDown` / `process` and so on) is skipped, not loaded.
- Module command-line options (`--drop on` and so on) are applied **before** plugins are loaded, so they never reach a replaced module.
  Configure plugin modules through the dashboard, the REST/Pipe API, scenarios or `--profile`.
- Loaded DLLs stay loaded until the process exits
  (`modules[]` points into DLL memory, so unloading would risk a use-after-free).

---

---

# Part 2 — Use Cases

---

## Case 1. Basic Latency Test — "The Server Is Far Away"

**Situation**: the game server is overseas, so there is always 150 ms of delay, and you want to test that.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Lag | ON, Delay = 150ms |

**Dashboard steps**:
1. Type `udp and outbound` in the filter field (the `all sending packets` preset would insert `outbound`, which covers every outgoing packet, not just UDP)
2. Click Start
3. Tick the Lag checkbox
4. Enter `150` in Delay (ms) in the expanded form

**How to check**: run the game and confirm the ping stays around 150 ms.

**CLI**:
```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 150
```

---

## Case 2. Unstable Internet — "Wi-Fi That Keeps Cutting Out"

**Situation**: like Wi-Fi with a weak signal: delay jumps around and packets occasionally vanish.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Jitter | ON, Min = 30ms, Max = 200ms |
| Drop | ON, Chance = 3.0% |

**What this combination reproduces**:
- Some packets delayed 30 ms, others 200 ms → irregular response times
- About 3 packets in 100 lost → recurring data loss

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --jitter on --jitter-min 30 --jitter-max 200 ^
           --drop on --drop-chance 3.0
```

---

## Case 3. Reproducing Mobile 4G

**Situation**: simulating a user playing on a smartphone over 4G.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Lag | ON, Delay = 60ms |
| Jitter | ON, Min = 10ms, Max = 80ms |
| Drop | ON, Chance = 2.0% |
| Bandwidth | ON, Limit = 5000 KB/s |

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --lag on --lag-time 60 ^
           --jitter on --jitter-min 10 --jitter-max 80 ^
           --drop on --drop-chance 2.0 ^
           --bandwidth on --bandwidth-bandwidth 5000
```

---

## Case 4. Reproducing Mobile 3G / Poor Coverage

**Situation**: a very weak 3G signal, like on the subway or in an elevator.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Lag | ON, Delay = 150ms |
| Jitter | ON, Min = 50ms, Max = 300ms |
| Drop | ON, Chance = 8.0% |
| Bandwidth | ON, Limit = 500 KB/s |

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --lag on --lag-time 150 ^
           --jitter on --jitter-min 50 --jitter-max 300 ^
           --drop on --drop-chance 8.0 ^
           --bandwidth on --bandwidth-bandwidth 500
```

---

## Case 5. Reproducing a Satellite Link

**Situation**: extremely high latency, as with satellite internet. The delay is steady but very large.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Lag | ON, Delay = 500ms |
| Jitter | ON, Min = 20ms, Max = 80ms |
| Drop | ON, Chance = 1.0% |

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --lag on --lag-time 500 ^
           --jitter on --jitter-min 20 --jitter-max 80 ^
           --drop on --drop-chance 1.0
```

**Note**: an RTT of 500 ms or more over satellite makes real-time games nearly unplayable. This lets you check how the game behaves in that environment (timeouts, disconnect handling and so on).

---

## Case 6. Consecutive Packet Loss — "Radio Interference / Wireless Congestion"

**Situation**: in a mobile game, packets vanish in a row for a short stretch, as when a train enters a tunnel. Drop alone scatters the losses, but on a real wireless link they cluster.

**Just switch it on with the defaults**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Burst Loss | ON (defaults) |

What the defaults mean:
- Normal (Good): 1 packet in 50 lost (2%)
- Burst (Bad): 8 packets in 10 lost (80%), lasting 5 packets on average
- About 20% of the time is spent in bursts → about 17% loss overall

**Tuning examples**:

| Goal | Good drop (%) | Bad drop (%) | Good to Bad (%) | Bad to Good (%) |
|------|---------|--------|--------|--------|
| Short, frequent bursts | 1.0 | 70.0 | 10.0 | 40.0 |
| Long, rare bursts | 0.5 | 90.0 | 2.0 | 10.0 |
| Mild bursts      | 1.0 | 30.0 | 5.0  | 20.0 |

**CLI** (short, frequent bursts):
```
clumsy.exe --filter "udp and outbound" ^
           --burstloss on --burstloss-good 1.0 --burstloss-bad 70.0 ^
           --burstloss-gb 10.0 --burstloss-bg 40.0
```

**Compare with Drop**: apply Drop and Burst Loss separately at the same overall loss rate (say 10%) and the game reacts differently.
- Drop 10%: losses evenly spread → the server copes well
- Burst Loss: losses clustered → consecutive lost inputs can cause a "momentary freeze"

---

## Case 7. Testing a Lag Compensation Algorithm

**Situation**: verifying that server-side lag compensation works under irregular delay.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)` |
| Jitter | ON, Min = 50ms, Max = 250ms |
| Out of Order | ON, Chance = 15.0%, Buffer = 8, Delay = 300ms |

**What this combination tests**:
- Delay varying between 50 and 250 ms → mismatches between client prediction and server confirmation
- Up to 8 packets collected and released shuffled → the server receiving older packets later
- Delay 300 ms → packets are never held longer than 0.3 s, even when the buffer is not full

**CLI**:
```
clumsy.exe --filter "udp and (udp.DstPort==7777 or udp.SrcPort==7777)" ^
           --jitter on --jitter-min 50 --jitter-max 250 ^
           --ood on --ood-chance 15.0 --ood-buffer 8 --ood-delay 300
```

---

## Case 8. Testing Reconnect Logic

**Situation**: testing that the client's reconnect code works correctly.

---

**Method A — periodic outages with Blackout** ← recommended

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Blackout | ON, Duration = 3000ms, Gap = 20000ms |

The connection is cut completely for 3 seconds every 20 seconds. During that time you can check, in order, whether the client:
1. notices the outage (is a timeout implemented?)
2. tries to reconnect (reconnect logic)
3. restores the game state correctly after reconnecting (session recovery)

**Reconnect timeout test**: make the outage shorter or longer than the client's timeout (e.g. 5 seconds) to check behavior at the boundary.

```
Duration=4000, Gap=30000  -> cut off just short of the timeout
Duration=6000, Gap=30000  -> cut off past the timeout
```

**Using the Trigger now button**: cut the connection manually at exactly the moment you want to test a reconnect (e.g. right after a particular game event).

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --blackout on --blackout-duration 3000 --blackout-gap 20000
```

---

**Method B — forced close with TCP RST** (for TCP game servers)

| Setting | Value |
|------|-----|
| Filter | `tcp and (tcp.DstPort == 9000 or tcp.SrcPort == 9000)` |
| Set TCP RST | ON |
| RST next packet button | click at the moment you choose |

The TCP connection is torn down the moment you press the button. Check that the client handles the `connection reset` error and reconnects.

---

**Comparison**:

| | Blackout | TCP RST |
|--|---------|---------|
| Protocol | UDP and TCP | TCP only |
| How the link dies | packets silently dropped (induces a timeout) | immediate notice via the RST flag |
| What it reproduces | entering a tunnel or a dead zone | the server process being killed |
| Periodic repetition | automatic | a manual click each time |

---

## Case 9. Testing UDP Duplicate Handling

**Situation**: testing that server and client handle the same packet arriving twice (retransmission by network equipment) correctly.

**Settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Duplicate | ON, Copies = 2, Chance = 20.0% |

One packet in five is sent along with a copy. Check that the server ignores the duplicate or processes it only once.

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --duplicate on --duplicate-count 2 --duplicate-chance 20.0
```

---

## Case 10. Testing a Local Server During Development (localhost)

**Situation**: client and server both run on the same PC and you want to test network conditions.

**Filter caveat**: loopback always needs `outbound`.

```
udp and outbound and loopback
```

**Example settings**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound and loopback` |
| Lag | ON, Delay = 80ms |
| Drop | ON, Chance = 2.0% |

**Dashboard steps**:
1. Pick `localhost ipv4 udp` in the Preset dropdown → `udp and outbound and loopback` is filled in
2. Click Start
3. Tick Lag and Drop and set their values

---

## Case 11. Automated Test Scenarios in CI/CD

**Situation**: running automated tests in the build pipeline while stepping through degraded network conditions.

### Method A — CLI + `--timeout` (simple, no condition changes)

```powershell
# Stage 1: normal conditions (30 seconds)
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --lag on --lag-time 20 --timeout 30' `
    -Verb RunAs -Wait

# Stage 2: poor conditions (30 seconds)
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --lag on --lag-time 200 --jitter on --jitter-min 50 --jitter-max 300 --drop on --drop-chance 5.0 --timeout 30' `
    -Verb RunAs -Wait
```

### Method B — Named Pipe API (single process, live condition changes)

Start clumsy once and change conditions on the fly over the Named Pipe. Scenarios switch with no restart overhead.

```powershell
function Send-ClumsyCommand($cmd) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.','clumsy','InOut')
    $pipe.Connect(2000)
    $w = New-Object System.IO.StreamWriter($pipe); $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($pipe)
    $w.Write(($cmd | ConvertTo-Json -Compress))
    $resp = $r.ReadLine(); $pipe.Close()
    return $resp | ConvertFrom-Json
}

# With clumsy already running as Administrator, started with --filter "udp and outbound" --timeout <sec>
# (if it was started without a filter, start the capture first with Send-ClumsyCommand @{cmd="filter";filter="udp and outbound"}):

# --- Stage 1: normal delay (20 ms) ---
Send-ClumsyCommand @{cmd="set";module="lag";enabled=$true;"lag-time"=20}
Start-Sleep 30
Run-YourTests

# --- Stage 2: switch to poor conditions ---
Send-ClumsyCommand @{cmd="set";module="lag";"lag-time"=200}
Send-ClumsyCommand @{cmd="set";module="jitter";enabled=$true;"jitter-min"=50;"jitter-max"=300}
Send-ClumsyCommand @{cmd="set";module="drop";enabled=$true;"drop-chance"=5.0}
Start-Sleep 30
Run-YourTests

# --- Stage 3: add a bandwidth cap ---
Send-ClumsyCommand @{cmd="set";module="bandwidth";enabled=$true;"bandwidth-bandwidth"=200}
Start-Sleep 30
Run-YourTests

# --- Done: shut clumsy down ---
Send-ClumsyCommand @{cmd="stop"}
```

**Why Method B**: conditions switch instantly, and `get_stats` lets you check the state during the test. It suits complex multi-stage scenarios.

### Method C — Scenario File (simplest, fully automatic)

One scenario JSON file defines the whole flow.

`ci_scenario.json`:
```json
[
  { "at": 0,  "lag": true, "lag-time": 20 },
  { "at": 30, "lag-time": 200, "jitter": true, "jitter-min": 50, "jitter-max": 300,
              "drop": true, "drop-chance": 5.0 },
  { "at": 60, "lag-time": 500, "drop-chance": 15.0,
              "bandwidth": true, "bandwidth-bandwidth": 200 }
]
```

```powershell
# Play the scenario for 90 seconds, then exit
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --scenario ci_scenario.json --timeout 90' `
    -Verb RunAs -Wait
```

**Why Method C**: only the scenario file needs to be version-controlled. The QA team can reproduce exactly the same conditions from the same file at any time.

---

## Case 12. Packet Tampering — Damaging Game Headers and Sequence Numbers

**Situation**: testing that server and client handle errors correctly when a game packet's header or sequence number is damaged.

**Setting A — damaged sequence numbers (Front)**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Tamper | ON, Chance = 5.0%, Position = 0 (Front), Redo checksum = ON |

Damages the first quarter of the payload. Most game UDP packets put their header (packet type, sequence number, player ID and so on) at the front, so this verifies how the server handles a wrong sequence number or an unknown packet type.

**Setting B — unpredictable damage (Random)**:

| Setting | Value |
|------|-----|
| Filter | `udp and outbound` |
| Tamper | ON, Chance = 10.0%, Position = 3 (Random), Redo checksum = ON |

Damages a different spot in every packet to cover a wide range of error cases. It works much like fuzzing and is good at finding unexpected parse errors or crashes.

**CLI (Front position)**:
```
clumsy.exe --filter "udp and outbound" ^
           --tamper on --tamper-chance 5.0 --tamper-position 0 --tamper-checksum on
```

**CLI (Random position)**:
```
clumsy.exe --filter "udp and outbound" ^
           --tamper on --tamper-chance 10.0 --tamper-position 3 --tamper-checksum on
```

> **Redo checksum caveat**: with it OFF, the IP/UDP checksums are broken too and the OS may drop the packet. Keep it ON when testing application-level error handling.

---

## Quick Reference — Settings by Environment

| Environment | Lag | Jitter | Drop | Bandwidth | Blackout |
|------|-----|--------|------|-----------|---------|
| Ideal local | — | — | — | — | — |
| Typical home fiber | 5ms | 2~15ms | 0.1% | — | — |
| Mobile 4G | 60ms | 10~80ms | 2% | 5000 KB/s | — |
| Mobile 3G | 150ms | 50~300ms | 8% | 500 KB/s | Dur=2s Gap=60s |
| Crowded public Wi-Fi | 80ms | 30~200ms | 3% | 2000 KB/s | — |
| Subway / dead zone | 100ms | 20~150ms | 5% | — | Dur=3s Gap=20s |
| Satellite | 500ms | 20~80ms | 1% | — | — |
| Overseas server (US) | 150ms | 10~40ms | 0.5% | — | — |
| Overseas server (Europe) | 200ms | 10~50ms | 0.5% | — | — |
| Extreme stress | 300ms | 100~500ms | 15% | 200 KB/s | Dur=5s Gap=10s |

> **About Bandwidth**: Bandwidth does not drop packets over the limit; it queues them and sends them late. The lower the value, the longer packets wait and the more delay builds up. With `Bandwidth + Lag`, total delay = the Lag delay + time spent waiting in the Bandwidth queue.
