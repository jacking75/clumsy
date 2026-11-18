# clumsy

**Language**: [English](README.md) | 한국어

__clumsy는 Windows의 네트워크 환경을 의도적으로 악화시키는 도구입니다. 단, 제어 가능하고 대화형 방식으로 동작합니다.__

[WinDivert](http://reqrypt.org/windivert.html)를 활용하여 실행 중인 네트워크 패킷을 가로챈 뒤, 원하는 시점에 지연/드롭/변조 등을 적용하고 재전송합니다. 네트워크 관련 버그를 추적하거나, 열악한 연결 환경에서 애플리케이션을 평가할 때 유용합니다:

* 설치 불필요.
* 프록시 설정이나 애플리케이션 코드 변경 불필요.
* 시스템 전체 네트워크를 캡처하므로 모든 애플리케이션에 적용 가능.
* 오프라인 환경(localhost ↔ localhost)에서도 동작.
* 애플리케이션이 실행 중인 상태에서 clumsy를 언제든 시작/중지 가능.
* 네트워크 상태를 대화형으로 제어하며, 현재 상태를 시각적으로 확인 가능.
* 콘솔 + 내장 웹 대시보드로 동작 — 외부 GUI 라이브러리 의존성 없음.
* REST API / Server-Sent Events / Named Pipe로 원격·자동화 제어 가능.
* **LLM / AI 에이전트 친화적** — AI 코딩 에이전트가 clumsy를 전혀 모르는 상태에서도 `clumsy --help`와 자기 설명형 REST API만으로 직접 다룰 수 있습니다. 원하는 네트워크 상태를 말로 설명하면 나머지는 에이전트가 처리합니다. [LLM / AI 에이전트로 사용하기](#llm--ai-에이전트로-사용하기) 참고.


## 사전 준비

### 시스템 요구사항

- **Windows**: 7 / 8 / 10 / 11 (64비트 전용), 관리자 권한 필요 (WinDivert 드라이버 로드)
- **Linux**: NFQUEUE 지원 커널, `CAP_NET_ADMIN` 권한 — [docs/LINUX.md](docs/LINUX.md) 참고

### 실행 시 필요 파일

clumsy를 실행하려면 실행파일과 같은 디렉토리에 다음 파일들이 있어야 합니다:

| 파일 | 설명 | 출처 |
|------|------|------|
| `WinDivert.dll` | 패킷 캡처 라이브러리 | `external/WinDivert-2.2.0-A/x64/` |
| `WinDivert64.sys` | WinDivert 커널 드라이버 | `external/WinDivert-2.2.0-A/x64/` |
| `config.json` | 필터 프리셋 정의 (권장) | `etc/config.json` |
| `config.txt` | 필터 프리셋 정의 (레거시) | `etc/config.txt` |
| `web/index.html` | 웹 대시보드 (없으면 REST API만 동작) | `etc/web/index.html` |
| `web/llms.txt`, `web/llms_kr.txt` | AI 에이전트용 사용 가이드 영문·한국어판 (`GET /llms.txt`, `GET /llms_kr.txt`로 제공) | `etc/web/` |

> 빌드 시 post-build 단계에서 이 파일들이 출력 디렉토리에 자동 복사됩니다.


## 빌드

### 의존성

리포지토리의 `external/` 디렉토리에 포함되어 있어 별도 설치가 필요 없습니다:

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| [WinDivert](https://reqrypt.org/windivert.html) | 2.2.0-A | 네트워크 패킷 캡처/재전송 |

WinDivert가 유일한 외부 의존성입니다. HTTP 서버, JSON 파서, pcap 라이터, 웹 대시보드는
모두 직접 구현되어 있어 추가 라이브러리나 빌드 도구가 필요 없습니다.

### 방법 1: Visual Studio (권장)

**요구사항**: Visual Studio 2026 이상 (C++ 데스크톱 개발 워크로드, C++23)

1. `msvc/clumsy.sln`을 Visual Studio에서 엽니다.
2. 플랫폼을 **x64**로 선택합니다.
3. 구성을 **Debug** 또는 **Release**로 선택 후 빌드합니다.

출력 경로:
- Debug: `bin/msvc/Debug/x64/clumsy.exe`
- Release: `bin/msvc/Release/x64/clumsy.exe`

또는 커맨드라인에서:
```bat
MSBuild.exe msvc/clumsy.sln -p:Configuration=Release -p:Platform=x64
```

| 구성 | 출력 유형 | 설명 |
|------|----------|------|
| Debug | Console App | 상세 트레이스 로그가 기본 활성화 |
| Release | Console App | 상태 로그만 출력, `--verbose on`으로 트레이스 활성화 |

### 방법 2: Linux (WSL2 포함)

**요구사항**: g++-16 이상, `libnetfilter-queue-dev`, `libmnl-dev`, `iptables`

```bash
make install-deps     # Debian 계열 의존성 설치
make                  # → bin/linux/clumsy
make test             # 패킷 헬퍼 계약 테스트
make package-deb      # → bin/linux/clumsy_0.4_amd64.deb
```

`.deb`를 설치하면 후처리가 `setcap cap_net_admin,cap_net_raw+ep`을 적용하므로
**sudo 없이 실행**할 수 있습니다.

리눅스에서는 필터가 2단계(iptables 규칙 + clumsy 필터 표현식)로 동작하고,
권한 처리와 duplicate 모듈에 주의할 점이 있습니다.
전부 [docs/LINUX.md](docs/LINUX.md)에 정리되어 있습니다.

> 빌드 정의는 이 둘뿐입니다: Windows는 `msvc/clumsy.vcxproj`, 리눅스는 `Makefile`.
> 소스 파일을 추가하면 해당 플랫폼 것만 갱신하면 됩니다.
> (0.4까지 있던 GENie / MinGW 빌드 경로는 사용하지 않아 제거했습니다.)


## 설정

필터 프리셋은 `config.json` (권장) 또는 `config.txt` (레거시)에 정의합니다.

**config.json 형식:**
```json
{
  "filters": [
    { "name": "localhost ipv4 all", "filter": "outbound and loopback" },
    { "name": "game server udp 7777", "filter": "udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)" }
  ]
}
```

**config.txt 형식 (레거시):**
```
필터이름: WinDivert 필터 표현식
```

> `config.json`이 있으면 우선 사용되고, 없으면 `config.txt`를 읽습니다.

필터 문법: https://github.com/basil00/Divert/wiki/WinDivert-Documentation#7-filter-language

> **주의**: loopback 패킷 필터링 시 `inbound` 조건은 사용 불가. `outbound and loopback` 형태로만 가능.


## 실행 방법

clumsy는 콘솔 애플리케이션입니다. **관리자 권한 콘솔**에서 실행하세요.

```
clumsy.exe
```

시작하면 배너에 웹 대시보드 주소가 출력됩니다:

```
clumsy 0.4 - console + web network condition simulator
  Administrator : yes
  Web dashboard : http://127.0.0.1:8080/
  Named Pipe    : \\.\pipe\clumsy
  Presets       : 34 loaded
  Press Ctrl+C to quit.
```

브라우저로 접속하면 필터 설정, 모듈 토글/파라미터, 실시간 통계 그래프,
필터 빌더, 프로파일/시나리오/pcap 제어를 모두 사용할 수 있습니다.


## LLM / AI 에이전트로 사용하기

clumsy는 사람뿐 아니라 AI 코딩 에이전트(Claude Code, Codex, Cursor, Copilot 에이전트 등)가 직접 다루도록 만들어져 있습니다. 원하는 네트워크 상태를 말로 설명하면, 에이전트가 그것을 명령줄이나 REST 호출로 바꿔 테스트를 돌리고, 통계를 읽고, 끝나면 네트워크를 원래대로 돌려놓습니다.

### 왜 LLM이 잘 다룰 수 있나

| 에이전트에게 필요한 것 | clumsy가 제공하는 것 |
|---|---|
| 사전 지식 없이 사용법 파악 | `clumsy --help`는 코드에서 자동 생성됩니다. 모듈마다 한 줄 설명, 파라미터마다 범위와 기본값, 그리고 종료 코드까지 나옵니다. 권한이 필요 없고 아무것도 시작하지 않습니다. |
| 자기 실수를 알아채기 | 모르는 옵션이나 쓸 수 없는 값이 있으면 **아무것도 시작하기 전에** 멈추고(종료 코드 2), 메시지가 고칠 방법을 알려줍니다. 조용히 무시되는 것은 없습니다. |
| 성공 여부 판단 | 종료 코드가 구분됩니다(`0` 정상, `2` 명령줄 오류, `3` 캡처 시작 실패, `4` 이미 실행 중). 관리자 권한이 없거나 필터가 잘못돼 캡처를 못 시작하면 타임아웃까지 기다리지 않고 즉시 실패합니다. |
| 자기 설명형 API | `GET /api/docs`가 모든 엔드포인트를 예시 본문·규칙과 함께 알려주고, `GET /api/modules`가 모든 파라미터의 타입·범위·현재 값을 알려줍니다. |
| 조치 가능한 오류 | 모든 API 응답은 `status`가 있는 JSON입니다. 경로나 메서드가 틀리면 무엇을 써야 하는지 알려줍니다. 설정 응답에는 `warnings`(범위를 벗어나 잘렸거나 거부된 값), `unknownKeys` + `validKeys`, 실제로 적용된 `params`, 캡처 실행 여부가 들어 있습니다. 아무것도 적용되지 않은 요청은 조용한 `ok`가 아니라 오류로 답합니다. |
| 에이전트용 가이드 | [`etc/web/llms_kr.txt`](etc/web/llms_kr.txt)(영문: [`llms.txt`](etc/web/llms.txt)). 실행 중인 clumsy가 `GET /llms_kr.txt`, `GET /llms.txt`로도 제공합니다. 규칙, 모듈 고르는 법, 필터 문법, 단계별 절차 2종, 문제 해결을 담았습니다. |
| 동작 검증 수단 | `GET /api/stats`가 캡처된 패킷 수와 모듈별로 영향을 준 패킷 수를 보여줍니다. |
| 안전장치 | `--timeout`은 무슨 일이 있어도 실행을 끝내고, `POST /api/quit`은 요청 즉시 끝냅니다. |

에이전트가 옵션을 틀렸을 때 보게 되는 출력:

```
> clumsy.exe --filter "udp and udp.DstPort == 7777" --delay 150 --lag-ms 150 --jitter-dist gaussian
clumsy: unknown option --delay - clumsy spells that --lag on --lag-time <ms>
clumsy: unknown option --lag-ms - lag takes: --lag on|off, --lag-inbound, --lag-outbound, --lag-time
clumsy: --jitter-dist: "gaussian" is not one of uniform,normal,pareto
Nothing was started. Run clumsy --help for every option.
(종료 코드 2)
```

```
POST /api/modules/lag  {"lag-time":99999,"lag-ms":5}
{"status":"ok","module":"lag","applied":1,"enabled":false,"capturing":false,
 "params":{"lag-time":"15000","lag-inbound":"true","lag-outbound":"true"},
 "unknownKeys":"lag-ms","validKeys":"lag-inbound,lag-outbound,lag-time",
 "warnings":["lag-time: 99999 is outside 0..15000, clamped to 15000"]}
```

### LLM에게 이렇게 요청하세요

에이전트에게 네 가지만 알려주면 됩니다.

1. **clumsy가 어디 있는지** — `clumsy.exe` 경로
2. **어떤 트래픽인지** — 대상 애플리케이션의 포트나 서버 주소, TCP/UDP 여부
3. **어떤 상태를 원하는지** — 평소 말로: "지연 150ms에 손실 5%", "상태 나쁜 3G", "30초마다 5초씩 끊김"
4. **얼마 동안인지** — 그리고 끝나면 clumsy를 종료하라는 것

그리고 사용자만 할 수 있는 일이 하나 있습니다. **에이전트를 실행하는 터미널을 관리자 권한으로 여세요**(Windows). 리눅스는 `sudo` 또는 `CAP_NET_ADMIN`이 필요합니다. 패킷 캡처에 필요한 권한이고, 에이전트가 스스로 권한을 올릴 수는 없습니다.

요청 예시:

> `C:\tools\clumsy\clumsy.exe`를 써서 내 게임 클라이언트에 나쁜 모바일 네트워크 환경을 만들어줘. 게임은 서버와 UDP 7777 포트로 통신해. 60초 동안 지연 150ms와 패킷 손실 5%를 걸고, 끝나면 통계를 보여줘. 먼저 `clumsy.exe --help`로 옵션을 확인해.

> clumsy를 백그라운드로 띄우고 REST API로 제어해줘. 먼저 `http://127.0.0.1:8080/llms_kr.txt`를 읽어. 대상은 10.0.0.5의 TCP 443 포트야. 지연 100ms로 시작해서 20초마다 패킷 손실을 2%씩 올리고, 내 클라이언트의 재접속 로직이 언제 동작하는지 알려줘. 끝나면 clumsy를 종료해.

> 지하철 탑승 상황을 재현하는 clumsy 시나리오 파일을 만들어줘. 정상 4G 30초, 5초 완전 두절, 3G 수준 30초, 그리고 회복 순서로. UDP 9000 포트에 `--scenario`로 실행하고 HTML 리포트도 저장해.

> localhost:5000에서 도는 내 서비스가 네트워크가 불안정할 때 주문을 중복 생성해. clumsy로 재현해줘. 5000 포트로 가는 루프백 트래픽에 패킷 복제, 순서 뒤섞기, TCP 리셋을 하나씩 걸어보고 어느 것이 버그를 일으키는지 알려줘.

요령:

- **문서를 한 번 가리켜 주세요**("먼저 `clumsy --help`를 실행해" 또는 "`llms_kr.txt`를 읽어"). 그러지 않으면 모델이 다른 도구의 옵션 이름(`--delay`, `--loss`)부터 시도합니다. clumsy가 힌트와 함께 거부하긴 하지만, 요청에 한 줄 넣으면 그 왕복이 없어집니다.
- **포트나 주소를 꼭 알려주세요.** 없으면 에이전트가 필터를 추측해야 하고, 범위가 넓은 필터는 PC의 모든 통신을 망가뜨립니다. 에이전트 자신이 모델과 통신하는 연결도 포함됩니다.
- **시간을 정해주세요.** 에이전트가 이를 `--timeout`으로 넘기고, 그러면 문제가 생겨도 네트워크가 반드시 복구됩니다.
- 반복해서 테스트하는 프로젝트라면 에이전트 지침 파일(`CLAUDE.md`, `AGENTS.md` 등)에 적어 두면 매번 설명하지 않아도 됩니다.

  ```markdown
  ## 네트워크 상태 테스트
  네트워크를 일부러 나쁘게 만들 때는 clumsy(`C:\tools\clumsy\clumsy.exe`)를 사용한다.
  처음 쓸 때 `clumsy.exe --help`를 실행한다. 실행 중에는 `GET http://127.0.0.1:8080/llms_kr.txt`가
  에이전트용 가이드이고 `GET /api/docs`가 REST API 목록이다.
  우리 게임 트래픽은 UDP 7777 포트다. 항상 `--timeout`을 주고, 테스트가 끝나면 clumsy를 종료한다.
  터미널은 관리자 권한으로 실행되어 있어야 한다.
  ```


## CLI 사용법

실행 인수로 모듈을 설정하는 파라미터화 모드를 지원합니다:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 --drop on --drop-chance 5.0
```

전체 인수 목록은 `clumsy.exe --help`로 확인할 수 있습니다. 코드에서 자동 생성되므로 모든 모듈 파라미터의 범위와 기본값이 항상 최신으로 나옵니다.

모든 옵션은 `--키 값` 쌍입니다(스위치도 `--lag on`처럼 값을 씁니다). 모르는 옵션이나 쓸 수 없는 값이 있으면 아무것도 시작하지 않고 종료하며, 범위를 벗어난 숫자는 경고와 함께 범위 안으로 조정됩니다.

| 모듈 | 인수 예시 |
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
| corrupt | `--corrupt on`, `--corrupt-chance 50`, `--corrupt-ber 5000` (비트당 ppm) |
| reset | `--reset on`, `--reset-chance 5.0` |
| bandwidth | `--bandwidth on`, `--bandwidth-bandwidth 100` |

모든 모듈은 `--<module>-inbound on|off` / `--<module>-outbound on|off`로 방향을 지정할 수 있습니다.

기타:
- `--timeout <초>`: 지정 시간 후 자동 종료
- `--scenario scenario.json`: 시나리오 파일 실행
- `--profile <이름>`: 미리 저장해 둔 프로파일 적용 (대시보드 또는 `POST /api/profiles`로 저장, 기본 제공 프로파일은 없음)
- `--stats-log stats.csv`: 통계 로그 파일 출력 (`.json` 확장자면 JSON)
- `--stats-interval 1`: 통계 기록 간격(초)
- `--stats-console 10`: 콘솔 상태 요약 출력 간격(초, `0`이면 끔)
- `--pcap-out capture.pcap`: 패킷을 libpcap 형식으로 덤프 (Wireshark 호환)
- `--pcap-stage pre|post|both`: 모듈 적용 전/후 중 어느 시점을 기록할지 (기본 `post`)
- `--pcap-max-packets` / `--pcap-max-bytes`: 덤프 크기 상한
- `--replay-in capture.pcap`: 저장된 pcap을 다시 주입 (`--replay-speed`, `--replay-loop`)
- `--report-out report.html`: 캡처 종료 시 HTML 세션 리포트 생성
- `--enable-plugins <디렉토리>`: 커스텀 모듈 DLL 로드 (기본 비활성, 보안 주의)
- `--verbose on`: 패킷 단위 트레이스 로그
- `--elevate on`: 관리자 권한이 아니면 UAC로 재실행
- `--version`: 버전 출력

종료 코드:

| 코드 | 의미 |
|------|------|
| `0` | 정상 종료 (`--timeout` 도달, Ctrl+C, `POST /api/quit`) |
| `2` | 명령줄 오류: 모르는 옵션, 쓸 수 없는 값, 불러올 수 없는 `--profile` / `--scenario` — 아무것도 시작하지 않음 |
| `3` | 캡처(`--filter`)나 재생(`--replay-in`)을 시작하지 못함. 예: 관리자 권한 없음, 필터 문법 오류 |
| `4` | 다른 clumsy가 이미 실행 중 |


## 웹 대시보드와 REST API

기본값은 `127.0.0.1:8080`이며 로컬호스트에서는 토큰 없이 접근할 수 있습니다. 단, `--web-token`으로 토큰을 지정하면 로컬호스트에서도 토큰이 필요합니다.

| 인수 | 설명 |
|------|------|
| `--web off` | 웹 서버 비활성화 |
| `--web-port 9000` | 포트 변경 |
| `--web-bind 0.0.0.0` | 외부 인터페이스 바인딩 (**토큰 인증 강제 + 보안 경고 출력**) |
| `--web-token <토큰>` | 고정 토큰 지정 (CI 스크립트에서 재사용) |

외부 바인딩 시 토큰은 `X-Clumsy-Token` 헤더나 `?token=` 쿼리스트링으로 전달합니다.

주요 엔드포인트 (전체 목록과 엔드포인트별 예시 본문은 `GET /api/docs`):

| 메서드 / 경로 | 설명 |
|---|---|
| `GET /api/docs` | 모든 엔드포인트의 설명, 예시 본문, API 규칙 |
| `GET /llms.txt` | AI 에이전트용 사용 가이드 (일반 텍스트, 한국어판은 `GET /llms_kr.txt`) |
| `GET /api/health` | 생존 확인 (인증 불필요, CI 헬스체크용) |
| `GET /metrics` | Prometheus 텍스트 노출 형식 (인증 불필요) |
| `GET /api/status` | 캡처 상태, 필터, 마지막 메시지 |
| `GET /api/modules` | 모듈 전체 상태 + 폼 자동 생성용 ParamSpec |
| `GET /api/stats` | 실시간 통계 |
| `GET /api/stream` | Server-Sent Events, 200ms 주기 통계 push |
| `GET /api/report` | HTML 세션 리포트 다운로드 |
| `POST /api/filter` | 필터 설정 후 캡처 시작 |
| `POST /api/stop` | 캡처 중지 |
| `POST /api/modules/{shortName}` | 모듈 활성화/파라미터 변경 |
| `POST /api/profiles/{name}/apply` | 프로파일 적용 |
| `POST /api/profiles/{name}/delete` | 프로파일 삭제 |
| `POST /api/apply` | 여러 모듈을 저장 없이 한 번에 설정 |
| `POST /api/presets` | 필터 프리셋 저장 (config.json에 기록) |
| `POST /api/scenario/loadinline` | 요청 본문의 시나리오를 파일 없이 로드 |
| `POST /api/pcap/start` / `stop` | pcap 덤프 제어 |
| `POST /api/replay/start` / `stop` | 저장된 pcap 재생 제어 |

재생 중인 패킷은 다시 캡처되지 않습니다. Windows에서는 캡처 필터에
`and not impostor`가 자동으로 붙고(필터에 `impostor`를 직접 쓰면 비활성화),
Linux에서는 `--auto-iptables on`이 IPv4·IPv6 양쪽에 설치하는 fwmark ACCEPT
규칙이 같은 역할을 합니다.

예시:

```bash
curl http://127.0.0.1:8080/api/status
curl -X POST http://127.0.0.1:8080/api/modules/drop \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"drop-chance\":10.0}"
```

모든 응답은 `"status"`가 `"ok"` 또는 `"error"`인 JSON입니다. `ok`여도 요청한 그대로 되지 않았을 수 있으며, 그 경우 응답이 알려줍니다. `warnings`는 범위를 벗어나 조정됐거나 거부된 값, `unknownKeys`는 어떤 모듈에도 없는 키, `params`는 요청 후 실제로 적용된 값입니다. 아무것도 적용되지 않은 요청은 조용한 `ok`가 아니라 `400` 오류로 답합니다. 모듈 설정은 캡처가 실행 중일 때만 트래픽에 작용하므로 `POST /api/filter`가 먼저이며, 응답의 `capturing`이 캡처 실행 여부를 알려줍니다. 시나리오 로드(`POST /api/scenario/load`, `loadinline`)도 모르는 키·잘못된 값·없는 메트릭을 `warnings`로 알려주고, `loadinline`은 스텝을 JSON 배열 그대로 받습니다.

### Prometheus / Grafana

`GET /metrics`가 Prometheus 텍스트 형식을 반환하므로, 여러 테스트 머신의 clumsy를
코드 한 줄 없이 한 대시보드에서 볼 수 있습니다. 인증이 필요 없습니다.

```yaml
# prometheus.yml
scrape_configs:
  - job_name: clumsy
    static_configs:
      - targets: ['10.0.0.5:8080', '10.0.0.6:8080']
    metrics_path: /metrics
```

```promql
rate(clumsy_module_affected_packets_total[1m])              # 모듈별 초당 처리량
histogram_quantile(0.95, rate(clumsy_latency_ms_bucket[5m])) # 지연 p95
rate(clumsy_corrupt_flipped_bits_total[1m])                  # 실제 적용된 비트 오류율
clumsy_capturing == 0                                        # 멈춘 인스턴스 찾기
```


## 게임 개발 활용

온라인 게임 개발 시 불안정한 네트워크 환경을 재현하는 데 활용할 수 있습니다.

### 게임 엔진별 필터 프리셋

`config.json`에 주요 게임 엔진/서비스용 프리셋이 기본 포함되어 있습니다:

| 프리셋 | 포트 | 대상 |
|--------|------|------|
| `unreal engine (7777)` | UDP 7777 | Unreal Engine |
| `unity netcode (9000)` | UDP 9000 | Unity Netcode |
| `steam game` | UDP 27000-27036 | Steam |
| `photon engine` | UDP 5055-5056 | Photon |
| `minecraft java` | TCP 25565 | Minecraft Java |

전체 목록은 `etc/config.json`을 참조하세요.

### 네트워크 시나리오별 설정 예시

| 시나리오 | 추천 설정 |
|---------|---------|
| 모바일 4G | Lag 80ms + Drop 2% + Throttle 10% |
| 모바일 3G | Lag 150ms + Drop 5% + Bandwidth 500KB/s |
| 위성 통신 | Lag 500ms + Drop 1% |
| 배틀그라운드 스트레스 | Lag 200ms + Duplicate 5% + OOD 10% |
| Wi-Fi 불안정 | Jitter 20~200ms + Burstloss (p=2, q=80) |


## 프로젝트 구조

```
clumsy/
├── src/                # C++23 소스 코드
│   ├── main.cpp        # 콘솔 진입점, 앱 제어 계층, 메인 틱 루프
│   ├── common.h        # 공통 타입/매크로/Module·ParamSpec·PacketMeta
│   ├── platform.h      # Win32 ↔ POSIX 호환 계층
│   ├── divert.cpp      # 캡처 백엔드: WinDivert (Windows)
│   ├── divert_linux.cpp# 캡처 백엔드: NFQUEUE (Linux)
│   ├── filterexpr.cpp  # 필터 표현식 파서/평가기 + iptables 규칙 도출
│   ├── iptables_linux.cpp # --auto-iptables 규칙 설치/제거
│   ├── httpserver.cpp  # 내장 HTTP 서버 (REST + SSE + 정적 파일)
│   ├── controlapi.cpp  # 트랜스포트 독립 제어 계층 (HTTP/Pipe 공용)
│   ├── json.cpp        # 최소 JSON 파서/직렬화
│   ├── lag.cpp         # 모듈: 고정 지연
│   ├── jitter.cpp      # 모듈: 랜덤 지연 (uniform/normal/pareto)
│   ├── drop.cpp        # 모듈: 확률적 패킷 드롭
│   ├── burstloss.cpp   # 모듈: 버스트 손실 (Gilbert-Elliott)
│   ├── blackout.cpp    # 모듈: 연결 두절
│   ├── throttle.cpp    # 모듈: 일시적 패킷 억제
│   ├── duplicate.cpp   # 모듈: 패킷 복제
│   ├── ood.cpp         # 모듈: 패킷 순서 뒤섞기
│   ├── tamper.cpp      # 모듈: 페이로드 변조
│   ├── corrupt.cpp     # 모듈: 비트 에러 주입 (무선 손상)
│   ├── reset.cpp       # 모듈: TCP RST 강제 전송
│   ├── bandwidth.cpp   # 모듈: 대역폭 제한
│   ├── pipe.cpp        # Named Pipe 제어 API (트랜스포트만)
│   ├── scenario.cpp    # 시나리오 스크립팅 (시간/조건/반복 트리거)
│   ├── profile.cpp     # 프로파일 저장/불러오기
│   ├── statslog.cpp    # 통계 로그 파일 출력
│   ├── procfilter.cpp  # 프로세스별 필터링
│   ├── pcapexport.cpp  # libpcap 형식 패킷 덤프
│   ├── pcapreplay.cpp  # 저장된 pcap 재주입 (스트리밍 파서 + 재생 스레드)
│   ├── latency.cpp     # 지연 히스토그램 (p50/p95/p99)
│   ├── report.cpp      # HTML 세션 리포트 생성
│   └── plugin.cpp      # 커스텀 모듈 DLL 로더 (옵션)
├── etc/                # 설정 파일, 리소스
│   ├── config.json     # 필터 프리셋 (JSON)
│   ├── config.txt      # 필터 프리셋 (레거시)
│   ├── web/index.html  # 웹 대시보드 (단일 정적 파일)
│   ├── web/llms.txt    # AI 에이전트용 사용 가이드 (/llms.txt로 제공)
│   ├── web/llms_kr.txt # 같은 가이드의 한국어판 (/llms_kr.txt로 제공)
│   └── clumsy.rc       # Windows 리소스 파일
├── msvc/               # Visual Studio 프로젝트
│   └── clumsy.sln
├── external/           # 외부 라이브러리 (WinDivert, Windows 전용)
├── packaging/          # .deb / .rpm 패키징 정의
├── tests/              # 검증 스위트 (자세한 내용은 tests/README.md)
│   ├── packetutil_test.cpp        # 패킷 헬퍼 계약 테스트 (양 플랫폼 공용)
│   ├── latency_test.cpp           # 지연 분위수 단위 테스트 (양 플랫폼 공용)
│   ├── windows/api_test.ps1       # REST API 회귀 (권한 불필요)
│   ├── windows/capture_test.ps1   # Windows 실캡처 회귀 (관리자 권한 필요)
│   ├── linux/api_test.sh          # REST API 회귀 (권한 불필요)
│   ├── linux/behaviour_test.sh    # 실패킷 동작 검증 (root 필요)
│   └── linux/nfqtest.cpp          # NFQUEUE 능력 프로브
├── docs/
│   ├── CODING_STYLE.md # 코드 스타일 원칙
│   ├── LINUX.md        # 리눅스 빌드/실행/제약 가이드
│   ├── guide_en-2.html # 초보자용 가이드 (영문)
│   └── guide.html      # 초보자용 가이드 (한국어)
├── Makefile            # 리눅스 빌드
├── doc_numbers.md      # 새 문서에 붙이는 일련번호 대장
├── manual_en-1.md      # 사용자 매뉴얼 (영문)
└── manual.md           # 사용자 매뉴얼 (한국어)
```


## 상세 문서

- **AI 에이전트용 가이드**: [etc/web/llms_kr.txt](etc/web/llms_kr.txt)(영문: [etc/web/llms.txt](etc/web/llms.txt)) — 규칙, 모듈 고르는 법, 필터 문법, 단계별 절차, 문제 해결
- **사용자 매뉴얼**: [manual.md](manual.md) — 전체 기능, 웹 UI, CLI, API 상세 설명 (영문: [manual_en-1.md](manual_en-1.md))
- **초보자용 가이드**: [docs/guide.html](docs/guide.html) — 그림으로 따라 하는 안내서, AI 에이전트 섹션 포함 (영문: [docs/guide_en-2.html](docs/guide_en-2.html))
- **리눅스 가이드**: [docs/LINUX.md](docs/LINUX.md) — 빌드, 권한, iptables 연동, 플랫폼 차이
- **코드 스타일**: [docs/CODING_STYLE.md](docs/CODING_STYLE.md) — C++ 전환 이후 코드 규칙
- **개발 로드맵**: [docs/ROADMAP-v2.md](docs/ROADMAP-v2.md) — 완료된 작업 및 향후 계획


## 라이선스

MIT
