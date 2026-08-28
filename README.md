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


## CLI usage

Modules can be configured via a parameterized command-line mode:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 --drop on --drop-chance 5.0
```

Run `clumsy.exe --help` to see the full argument list.

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
- `--profile mobile-3g`: apply a saved profile
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


## Web dashboard and REST API

The default bind is `127.0.0.1:8080`; on localhost, no token is required.

| Argument | Description |
|------|------|
| `--web off` | disable the web server |
| `--web-port 9000` | change the port |
| `--web-bind 0.0.0.0` | bind to an external interface (**forces token auth + prints a security warning**) |
| `--web-token <token>` | set a fixed token (reusable from CI scripts) |

For external bindings, pass the token via the `X-Clumsy-Token` header or a `?token=` query string.

Key endpoints (see `GET /api/docs` for the full list):

| Method / path | Description |
|---|---|
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

Example:

```bash
curl http://127.0.0.1:8080/api/status
curl -X POST http://127.0.0.1:8080/api/modules/drop \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"drop-chance\":10.0}"
```

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
│   └── LINUX.md        # Linux build/run/limitations guide
├── Makefile            # Linux build
├── manual.md           # user manual (Korean)
└── TODO.md             # development roadmap
```


## Further documentation

- **User manual**: [manual.md](manual.md) — full feature, web UI, CLI, and API reference (Korean)
- **Linux guide**: [docs/LINUX.md](docs/LINUX.md) — build, privileges, iptables integration, platform differences
- **Code style**: [docs/CODING_STYLE.md](docs/CODING_STYLE.md) — conventions since the C++ rewrite
- **Development roadmap**: [TODO.md](TODO.md) — completed work and future plans


## License

MIT
