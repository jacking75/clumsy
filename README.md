# clumsy
    
**Language**: English | [한국어](README_kr.md)

__clumsy is a tool that lets you deliberately degrade your network conditions on Windows, in a controllable, interactive way.__

It uses [WinDivert](http://reqrypt.org/windivert.html) to intercept live network packets, then applies delay/drop/tampering and reinjects them whenever you want. Useful for tracking down network-related bugs or evaluating how an application behaves on a poor connection:

* No installation required.
* No proxy setup or application code changes needed.
* Captures system-wide network traffic, so it applies to every application.
* Works even offline (localhost ↔ localhost).
* Start/stop clumsy at any time while the target application keeps running.
* Control network conditions interactively, with a live view of the current state.
* Runs as a console app + built-in web dashboard — no external GUI library dependency.
* Remote/automated control via REST API, Server-Sent Events, and Named Pipe.
* **LLM / AI-agent friendly** — an AI coding agent can operate clumsy from `clumsy --help` and the self-describing REST API alone, with no prior knowledge of the tool. You describe the network condition in plain language; the agent does the rest. See [Using clumsy with an LLM or AI agent](#using-clumsy-with-an-llm-or-ai-agent).


## Prerequisites

### System requirements

- **Windows**: 7 / 8 / 10 / 11 (64-bit only), requires administrator privileges (to load the WinDivert driver)
- **Linux**: a kernel with NFQUEUE support, `CAP_NET_ADMIN` capability — see [docs/LINUX.md](docs/LINUX.md)

### Files required at runtime

To run clumsy, the following files must sit next to the executable:

| File | Description | Source |
|------|------|------|
| `WinDivert.dll` | Packet capture library | `external/WinDivert-2.2.0-A/x64/` |
| `WinDivert64.sys` | WinDivert kernel driver | `external/WinDivert-2.2.0-A/x64/` |
| `config.json` | Filter preset definitions (recommended) | `etc/config.json` |
| `config.txt` | Filter preset definitions (legacy) | `etc/config.txt` |
| `web/index.html` | Web dashboard (without it, only the REST API works) | `etc/web/index.html` |
| `web/llms.txt`, `web/llms_kr.txt` | Usage guide for AI agents in English and Korean, served at `GET /llms.txt` and `GET /llms_kr.txt` | repository root (`llms.txt`, `llms_kr.txt`) |

> A post-build step copies these files into the output directory automatically.


## Building

### Dependencies

Bundled in the repository's `external/` directory — no separate install needed:

| Library | Version | Purpose |
|-----------|------|------|
| [WinDivert](https://reqrypt.org/windivert.html) | 2.2.0-A | Network packet capture/reinjection |

WinDivert is the only external dependency. The HTTP server, JSON parser, pcap writer, and web dashboard are all implemented in-house, so no additional libraries or build tools are required.

### Method 1: Visual Studio (recommended)

**Requirements**: Visual Studio 2026 or later (C++ desktop development workload, C++23)

1. Open `msvc/clumsy.sln` in Visual Studio.
2. Select **x64** as the platform.
3. Select **Debug** or **Release** as the configuration and build.

Output paths:
- Debug: `bin/msvc/Debug/x64/clumsy.exe`
- Release: `bin/msvc/Release/x64/clumsy.exe`

Or from the command line:
```bat
MSBuild.exe msvc/clumsy.sln -p:Configuration=Release -p:Platform=x64
```

| Configuration | Output type | Description |
|------|----------|------|
| Debug | Console App | Verbose trace logging is on by default |
| Release | Console App | Only status logs are printed; enable tracing with `--verbose on` |

### Method 2: Linux (including WSL2)

**Requirements**: g++-16 or later, `libnetfilter-queue-dev`, `libmnl-dev`, `iptables`

```bash
make install-deps     # install Debian-family dependencies
make                  # → bin/linux/clumsy
make test             # packet-helper contract tests
make package-deb      # → bin/linux/clumsy_0.4_amd64.deb
```

Installing the `.deb` runs a post-install step that applies `setcap cap_net_admin,cap_net_raw+ep`, so you can **run it without sudo**.

On Linux, filtering happens in two layers (iptables rules + clumsy's own filter expression), and there are a few gotchas around privilege handling and the duplicate module.
All of this is documented in [docs/LINUX.md](docs/LINUX.md).

> There are only two build definitions: `msvc/clumsy.vcxproj` for Windows and `Makefile` for Linux.
> Adding a source file only requires updating the one for that platform.
> (The GENie / MinGW build paths that existed through 0.4 were unused and have been removed.)


## Configuration

Filter presets are defined in `config.json` (recommended) or `config.txt` (legacy).

**config.json format:**
```json
{
  "filters": [
    { "name": "localhost ipv4 all", "filter": "outbound and loopback" },
    { "name": "game server udp 7777", "filter": "udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)" }
  ]
}
```

**config.txt format (legacy):**
```
preset name: WinDivert filter expression
```

> If `config.json` is present it takes priority; otherwise `config.txt` is read.

Filter syntax: https://github.com/basil00/Divert/wiki/WinDivert-Documentation#7-filter-language

> **Note**: when filtering loopback packets, the `inbound` condition cannot be used — only the `outbound and loopback` form works.


## Running

clumsy is a console application. Run it from an **administrator console**.

```
clumsy.exe
```

On startup, the banner prints the web dashboard address:

```
clumsy 0.4 - console + web network condition simulator
  Administrator : yes
  Web dashboard : http://127.0.0.1:8080/
  Named Pipe    : \\.\pipe\clumsy
  Presets       : 34 loaded
  Press Ctrl+C to quit.
```

Open that address in a browser to access filter configuration, module toggles/parameters, live statistics graphs, the filter builder, and profile/scenario/pcap controls.


## Using clumsy with an LLM or AI agent

clumsy is built to be operated by an AI coding agent (Claude Code, Codex, Cursor, Copilot agents, …) as well as by a person. You say what network condition you want in plain language; the agent turns it into a command line or REST calls, runs the test, reads the statistics, and puts the network back to normal.

### Why it works well with an LLM

| What an agent needs | What clumsy gives it |
|---|---|
| Learn the tool without prior knowledge | `clumsy --help` is generated from the code: every module with a one-line description, every parameter with its range and default, and the exit codes. It needs no privileges and starts nothing. |
| Notice its own mistakes | An unknown option or an unusable value stops clumsy **before** anything starts (exit code 2) and the message names the fix. Nothing is silently ignored. |
| Know whether it worked | Distinct exit codes (`0` ok, `2` bad command line, `3` capture could not start, `4` clash with another instance). A capture that cannot start — not elevated, bad filter — fails immediately instead of idling until the timeout. |
| A self-describing API | `GET /api/docs` lists every endpoint with an example body and the conventions; `GET /api/modules` lists every parameter with type, range and current value. |
| Errors it can act on | Every API reply is JSON with a `status`. A wrong path or method is answered with what to use instead. A reply to a setting carries `warnings` (clamped or refused values), `unknownKeys` + `validKeys`, the `params` actually in effect, and whether a capture is running. A request in which nothing could be applied is an error, never a silent `ok`. |
| A guide written for it | [`llms.txt`](llms.txt) (Korean: [`llms_kr.txt`](llms_kr.txt)), also served by the running instance at `GET /llms.txt` and `GET /llms_kr.txt`: rules, module chooser, filter syntax, two step-by-step recipes, troubleshooting. |
| A way to verify | `GET /api/stats` shows packets captured and packets each module affected. |
| A safety net | `--timeout` ends the run no matter what; `POST /api/quit` ends it on request. |

What a typo looks like to the agent:

```
> clumsy.exe --filter "udp and udp.DstPort == 7777" --delay 150 --lag-ms 150 --jitter-dist gaussian
clumsy: unknown option --delay - clumsy spells that --lag on --lag-time <ms>
clumsy: unknown option --lag-ms - lag takes: --lag on|off, --lag-inbound, --lag-outbound, --lag-time
clumsy: --jitter-dist: "gaussian" is not one of uniform,normal,pareto
Nothing was started. Run clumsy --help for every option.
(exit code 2)
```

```
POST /api/modules/lag  {"lag-time":99999,"lag-ms":5}
{"status":"ok","module":"lag","applied":1,"enabled":false,"capturing":false,
 "params":{"lag-time":"15000","lag-inbound":"true","lag-outbound":"true"},
 "unknownKeys":"lag-ms","validKeys":"lag-inbound,lag-outbound,lag-time",
 "warnings":["lag-time: 99999 is outside 0..15000, clamped to 15000"]}
```

### How to ask

Give the agent four things, and it has everything it needs:

1. **Where clumsy is** — the path to `clumsy.exe`.
2. **Which traffic** — the application's port or server address, and TCP or UDP.
3. **What condition** — in plain words: "150 ms latency and 5 % loss", "a bad 3G connection", "a 5-second outage every 30 seconds".
4. **For how long** — and that it should stop clumsy when done.

One more thing only you can do: **start the agent's terminal as Administrator** (Windows) or with `sudo` / `CAP_NET_ADMIN` (Linux). Capturing packets needs it, and an agent cannot elevate itself.

Example prompts:

> Use `C:\tools\clumsy\clumsy.exe` to simulate a poor mobile connection for my game client. It talks to the server over UDP port 7777. Add 150 ms of latency and 5 % packet loss for 60 seconds, then show me the statistics. Run `clumsy.exe --help` first to see the options.

> Start clumsy in the background and control it over its REST API — read `http://127.0.0.1:8080/llms.txt` first. Target: TCP to 10.0.0.5 port 443. Begin with 100 ms of lag, then add 2 % more packet loss every 20 seconds until my client's reconnect logic kicks in. Tell me at which loss level that happened, and shut clumsy down afterwards.

> Write a clumsy scenario file that reproduces a subway ride: 30 s of normal 4G, a 5 s blackout, 30 s of 3G-like conditions, then recovery. Run it against UDP port 9000 with `--scenario` and save an HTML report.

> My service on localhost:5000 creates duplicate orders on flaky networks. Reproduce it with clumsy: try packet duplication, reordering and TCP resets one at a time on loopback traffic to port 5000, and tell me which one triggers the bug.

Tips:

- **Point the agent at the documentation once** ("run `clumsy --help` first" or "read `llms.txt`"). Without it a model reaches for option names from other tools (`--delay`, `--loss`); clumsy will reject those with a hint, but one line in the prompt saves the round trip.
- **Always give the port or address.** Without it the agent has to guess a filter, and a broad filter degrades everything on the machine — including the agent's own connection to its model.
- **State the duration.** The agent will pass it as `--timeout`, which also guarantees the network recovers if anything goes wrong.
- For a project you test repeatedly, put the facts in your agent instructions file (`CLAUDE.md`, `AGENTS.md`, …) so you do not have to repeat them:

  ```markdown
  ## Network condition testing
  Use clumsy (`C:\tools\clumsy\clumsy.exe`) to degrade the network on purpose.
  Before first use run `clumsy.exe --help`; while it runs, `GET http://127.0.0.1:8080/llms.txt`
  is the guide for agents and `GET /api/docs` lists the REST API.
  Our game traffic is UDP port 7777. Always pass `--timeout`, and stop clumsy when the test ends.
  The terminal must be running as Administrator.
  ```


## CLI usage

Modules can be configured via a parameterized command-line mode:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 --drop on --drop-chance 5.0
```

Run `clumsy.exe --help` to see the full argument list — it is generated from the code, so it always shows every module parameter with its range and default.

Every option is a `--key value` pair (switches are spelled out: `--lag on`). An unknown option or an unusable value stops clumsy before anything starts; an out-of-range number is clamped with a warning.

| Module | Example arguments |
|------|----------|
| lag | `--lag on`, `--lag-time 100`, `--lag-inbound on`, `--lag-outbound on` |
| jitter | `--jitter on`, `--jitter-min 20`, `--jitter-max 150`, `--jitter-dist pareto` |
| drop | `--drop on`, `--drop-chance 5.0` |
| burstloss | `--burstloss on`, `--burstloss-good 2.0`, `--burstloss-bad 80.0`, `--burstloss-gb 5.0`, `--burstloss-bg 20.0` |
| blackout | `--blackout on`, `--blackout-duration 3000`, `--blackout-gap 30000` |
| throttle | `--throttle on`, `--throttle-chance 10.0`, `--throttle-frame 30` |
| duplicate | `--duplicate on`, `--duplicate-chance 10.0` |
| ood | `--ood on`, `--ood-chance 10.0`, `--ood-buffer 5`, `--ood-delay 200` |
| tamper | `--tamper on`, `--tamper-chance 10.0`, `--tamper-position 1`, `--tamper-checksum on` |
| corrupt | `--corrupt on`, `--corrupt-chance 50`, `--corrupt-ber 5000` (ppm per bit) |
| reset | `--reset on`, `--reset-chance 5.0` |
| bandwidth | `--bandwidth on`, `--bandwidth-bandwidth 100` |

Every module accepts `--<module>-inbound on|off` / `--<module>-outbound on|off` to control direction.

Other options:
- `--timeout <sec>`: exit automatically after the given time
- `--scenario scenario.json`: run a scenario file
- `--profile <name>`: apply a profile you saved earlier (dashboard or `POST /api/profiles`); none ship by default
- `--stats-log stats.csv`: write a statistics log file (JSON if the extension is `.json`)
- `--stats-interval 1`: statistics logging interval, in seconds
- `--stats-console 10`: console status summary interval, in seconds (`0` to disable)
- `--pcap-out capture.pcap`: dump packets in libpcap format (Wireshark-compatible)
- `--pcap-stage pre|post|both`: capture before/after module processing, or both (default `post`)
- `--pcap-max-packets` / `--pcap-max-bytes`: dump size limits
- `--replay-in capture.pcap`: reinject a saved pcap (`--replay-speed`, `--replay-loop`)
- `--report-out report.html`: generate an HTML session report when capture ends
- `--enable-plugins <dir>`: load custom module DLLs (disabled by default, security-sensitive)
- `--verbose on`: per-packet trace logging
- `--elevate on`: relaunch via UAC if not already running as administrator
- `--pipe-name <name>`: control pipe `\\.\pipe\<name>` (Linux: socket `/run/<name>.sock`); default `clumsy`
- `--version`: print the version

Exit codes:

| Code | Meaning |
|------|---------|
| `0` | clean exit (`--timeout` reached, Ctrl+C, `POST /api/quit`) |
| `2` | bad command line: unknown option, unusable value, or a `--profile` / `--scenario` that could not be loaded — nothing was started |
| `3` | the capture (`--filter`) or replay (`--replay-in`) could not start, e.g. not elevated or the filter did not parse |
| `4` | clash with another running clumsy: its `--web-port` or `--pipe-name` is taken, or the filter overlaps another instance's capture |

### Several instances: different conditions per client

Several clumsy processes can run at once, for example one per client on a machine that runs
many clients, each with its own conditions. Give every instance its own `--web-port` and
`--pipe-name` (on Linux also `--queue-num`), and pin each client's flow by both of its ports:

```bash
clumsy --web-port 8081 --pipe-name client1 --lag on --lag-time 150 \
  --filter "(udp.SrcPort == 50001 and udp.DstPort == 7777) or (udp.SrcPort == 7777 and udp.DstPort == 50001)"
clumsy --web-port 8082 --pipe-name client2 --drop on --drop-chance 10 \
  --filter "(udp.SrcPort == 50002 and udp.DstPort == 7777) or (udp.SrcPort == 7777 and udp.DstPort == 50002)"
```

A filter that could match packets another running instance already captures is refused
(exit code `4`, or HTTP `409` from `POST /api/filter`), and the message names that instance and
an example packet both would match. Without this, a shared packet would be shaped by whichever
instance saw it first and silently skipped by the other. The agent guide (`GET /llms.txt`,
"Recipe C") covers finding client ports and the filter forms in detail.


## Web dashboard and REST API

The default bind is `127.0.0.1:8080`; on localhost, no token is required unless you set one with `--web-token`.

| Argument | Description |
|------|------|
| `--web off` | disable the web server |
| `--web-port 9000` | change the port |
| `--web-bind 0.0.0.0` | bind to an external interface (**forces token auth + prints a security warning**) |
| `--web-token <token>` | set a fixed token (reusable from CI scripts) |

For external bindings, pass the token via the `X-Clumsy-Token` header or a `?token=` query string.

Key endpoints (see `GET /api/docs` for the full list, with an example body for each):

| Method / path | Description |
|---|---|
| `GET /api/docs` | every endpoint with a description, an example body, and the API conventions |
| `GET /llms.txt` | plain-text usage guide for AI agents (`GET /llms_kr.txt` for Korean) |
| `GET /api/health` | liveness check (no auth, for CI health checks) |
| `GET /metrics` | Prometheus text exposition format (no auth) |
| `GET /api/status` | capture status, filter, last message |
| `GET /api/modules` | full module state + ParamSpec for auto-generating forms |
| `GET /api/stats` | live statistics |
| `GET /api/stream` | Server-Sent Events, pushes stats every 200ms |
| `GET /api/report` | download the HTML session report |
| `POST /api/filter` | set the filter and start capture |
| `POST /api/stop` | stop capture |
| `POST /api/modules/{shortName}` | enable/disable a module or change its parameters |
| `POST /api/profiles/{name}/apply` | apply a profile |
| `POST /api/profiles/{name}/delete` | delete a profile |
| `POST /api/apply` | set multiple modules at once, without saving |
| `POST /api/presets` | save a filter preset (written to config.json) |
| `POST /api/scenario/loadinline` | load a scenario from the request body, without a file |
| `POST /api/pcap/start` / `stop` | control pcap dumping |
| `POST /api/replay/start` / `stop` | control replaying a saved pcap |

While a replay is running its packets are not captured again: on Windows the
capture filter automatically gains `and not impostor` (put the word `impostor`
in your own filter to opt out), and on Linux the fwmark ACCEPT rule that
`--auto-iptables on` installs for both IPv4 and IPv6 does the same job.

Example:

```bash
curl http://127.0.0.1:8080/api/status
curl -X POST http://127.0.0.1:8080/api/modules/drop \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"drop-chance\":10.0}"
```

Every reply is JSON with `"status": "ok"` or `"error"`. A reply can be `ok` and still report that it did not do exactly what was asked: `warnings` lists values that were clamped or refused, `unknownKeys` lists keys no module has, and `params` shows the values in effect after the request. A request in which nothing could be applied is answered with a `400` error rather than a silent `ok`. Module settings only act on traffic while a capture is running, so `POST /api/filter` comes first; `capturing` in the reply tells you whether one is. Scenario loads (`POST /api/scenario/load`, `loadinline`) report unknown keys, bad values and unknown metrics in `warnings` as well, and `loadinline` takes the steps as a plain JSON array.

### Prometheus / Grafana

`GET /metrics` returns the Prometheus text format, so you can watch clumsy across
multiple test machines from a single dashboard with zero extra code. No authentication is required.

```yaml
# prometheus.yml
scrape_configs:
  - job_name: clumsy
    static_configs:
      - targets: ['10.0.0.5:8080', '10.0.0.6:8080']
    metrics_path: /metrics
```

```promql
rate(clumsy_module_affected_packets_total[1m])              # per-module throughput per second
histogram_quantile(0.95, rate(clumsy_latency_ms_bucket[5m])) # p95 latency
rate(clumsy_corrupt_flipped_bits_total[1m])                  # actual bit error rate
clumsy_capturing == 0                                        # find stalled instances
```


## Game development use cases

Handy for reproducing unstable network conditions during online game development.

### Filter presets by game engine

`config.json` ships with presets for major game engines and services:

| Preset | Port | Target |
|--------|------|------|
| `unreal engine (7777)` | UDP 7777 | Unreal Engine |
| `unity netcode (9000)` | UDP 9000 | Unity Netcode |
| `steam game` | UDP 27000-27036 | Steam |
| `photon engine` | UDP 5055-5056 | Photon |
| `minecraft java` | TCP 25565 | Minecraft Java |

See `etc/config.json` for the full list.

### Example settings by network scenario

| Scenario | Recommended settings |
|---------|---------|
| Mobile 4G | Lag 80ms + Drop 2% + Throttle 10% |
| Mobile 3G | Lag 150ms + Drop 5% + Bandwidth 500KB/s |
| Satellite | Lag 500ms + Drop 1% |
| Battle-royale stress | Lag 200ms + Duplicate 5% + OOD 10% |
| Unstable Wi-Fi | Jitter 20~200ms + Burst loss (p=2, q=80) |


## Project layout

```
clumsy/
├── src/                # C++23 source code
│   ├── main.cpp        # console entry point, app control layer, main tick loop
│   ├── common.h        # shared types/macros/Module·ParamSpec·PacketMeta
│   ├── platform.h      # Win32 <-> POSIX compatibility layer
│   ├── divert.cpp      # capture backend: WinDivert (Windows)
│   ├── divert_linux.cpp# capture backend: NFQUEUE (Linux)
│   ├── filterexpr.cpp  # filter expression parser/evaluator + iptables rule derivation
│   ├── iptables_linux.cpp # install/remove --auto-iptables rules
│   ├── httpserver.cpp  # embedded HTTP server (REST + SSE + static files)
│   ├── controlapi.cpp  # transport-independent control layer (shared by HTTP/Pipe)
│   ├── json.cpp        # minimal JSON parser/serializer
│   ├── lag.cpp         # module: fixed delay
│   ├── jitter.cpp      # module: random delay (uniform/normal/pareto)
│   ├── drop.cpp        # module: probabilistic packet drop
│   ├── burstloss.cpp   # module: burst loss (Gilbert-Elliott)
│   ├── blackout.cpp    # module: connection blackout
│   ├── throttle.cpp    # module: temporary packet suppression
│   ├── duplicate.cpp   # module: packet duplication
│   ├── ood.cpp         # module: packet reordering
│   ├── tamper.cpp      # module: payload tampering
│   ├── corrupt.cpp     # module: bit-error injection (wireless corruption)
│   ├── reset.cpp       # module: forced TCP RST
│   ├── bandwidth.cpp   # module: bandwidth limiting
│   ├── pipe.cpp        # Named Pipe control API (transport only)
│   ├── scenario.cpp    # scenario scripting (time/condition/repeat triggers)
│   ├── profile.cpp     # profile save/load
│   ├── statslog.cpp    # statistics log file output
│   ├── procfilter.cpp  # per-process filtering
│   ├── pcapexport.cpp  # libpcap-format packet dump
│   ├── pcapreplay.cpp  # replay a saved pcap (streaming parser + replay thread)
│   ├── latency.cpp     # latency histogram (p50/p95/p99)
│   ├── report.cpp      # HTML session report generation
│   └── plugin.cpp      # custom module DLL loader (optional)
├── etc/                # config files, resources
│   ├── config.json     # filter presets (JSON)
│   ├── config.txt      # filter presets (legacy)
│   ├── web/index.html  # web dashboard (single static file)
│   └── clumsy.rc       # Windows resource file
├── msvc/               # Visual Studio project
│   └── clumsy.sln
├── external/           # external libraries (WinDivert, Windows-only)
├── packaging/          # .deb / .rpm packaging definitions
├── tests/              # verification suite (see tests/README.md for details)
│   ├── packetutil_test.cpp        # packet-helper contract tests (both platforms)
│   ├── latency_test.cpp           # latency-quantile unit tests (both platforms)
│   ├── windows/api_test.ps1       # REST API regression (no privileges required)
│   ├── windows/capture_test.ps1   # Windows live-capture regression (requires admin)
│   ├── linux/api_test.sh          # REST API regression (no privileges required)
│   ├── linux/behaviour_test.sh    # live-packet behavior verification (requires root)
│   └── linux/nfqtest.cpp          # NFQUEUE capability probe
├── docs/
│   ├── CODING_STYLE.md # code style principles
│   ├── LINUX.md        # Linux build/run/limitations guide
│   ├── guide_en.html   # beginner's guide (English)
│   ├── guide.html      # beginner's guide (Korean)
│   ├── comic.html      # comic introduction to clumsy (Korean)
│   └── comic/          # comic panel images
├── Makefile            # Linux build
├── llms.txt            # usage guide for AI agents (copied to web/, served at /llms.txt)
├── llms_kr.txt         # the same guide in Korean (served at /llms_kr.txt)
├── manual_en.md        # user manual (English)
└── manual.md           # user manual (Korean)
```


## Further documentation

- **Guide for AI agents**: [llms.txt](llms.txt) (Korean: [llms_kr.txt](llms_kr.txt)) — rules, module chooser, filter syntax, recipes, troubleshooting
- **User manual**: [manual_en.md](manual_en.md) — full feature, web UI, CLI, and API reference (Korean: [manual.md](manual.md))
- **Beginner's guide**: [docs/guide_en.html](docs/guide_en.html) — illustrated walkthrough, including an AI-agent section (Korean: [docs/guide.html](docs/guide.html))
- **Comic introduction** (Korean): [docs/comic.html](docs/comic.html) — what clumsy is and how to use it, as a short comic
- **Linux guide**: [docs/LINUX.md](docs/LINUX.md) — build, privileges, iptables integration, platform differences (Korean)
- **Code style**: [docs/CODING_STYLE.md](docs/CODING_STYLE.md) — conventions since the C++ rewrite (Korean)
- **Development roadmap**: [docs/ROADMAP-v2.md](docs/ROADMAP-v2.md) — completed work and future plans (Korean)


## License

MIT
