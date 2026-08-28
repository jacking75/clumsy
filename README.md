# clumsy

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


## CLI 사용법

실행 인수로 모듈을 설정하는 파라미터화 모드를 지원합니다:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 --drop on --drop-chance 5.0
```

전체 인수 목록은 `clumsy.exe --help`로 확인할 수 있습니다.

| 모듈 | 인수 예시 |
|------|----------|
| lag | `--lag on`, `--lag-time 100`, `--lag-inbound on`, `--lag-outbound on` |
| jitter | `--jitter on`, `--jitter-min 20`, `--jitter-max 150` |
| drop | `--drop on`, `--drop-chance 5.0` |
| burstloss | `--burstloss on`, `--burstloss-good 2.0`, `--burstloss-bad 80.0`, `--burstloss-gb 5.0`, `--burstloss-bg 20.0` |
| blackout | `--blackout on`, `--blackout-duration 3000`, `--blackout-gap 30000` |
| throttle | `--throttle on`, `--throttle-chance 10.0`, `--throttle-frame 30` |
| duplicate | `--duplicate on`, `--duplicate-chance 10.0` |
| ood | `--ood on`, `--ood-chance 10.0`, `--ood-buffer 5`, `--ood-delay 200` |
| tamper | `--tamper on`, `--tamper-chance 10.0`, `--tamper-position 1`, `--tamper-checksum on` |
| reset | `--reset on`, `--reset-chance 5.0` |
| bandwidth | `--bandwidth on`, `--bandwidth-bandwidth 100` |

모든 모듈은 `--<module>-inbound on|off` / `--<module>-outbound on|off`로 방향을 지정할 수 있습니다.

기타:
- `--timeout <초>`: 지정 시간 후 자동 종료
- `--scenario scenario.json`: 시나리오 파일 실행
- `--profile mobile-3g`: 프로파일 적용
- `--stats-log stats.csv`: 통계 로그 파일 출력 (`.json` 확장자면 JSON)
- `--stats-interval 1`: 통계 기록 간격(초)
- `--stats-console 10`: 콘솔 상태 요약 출력 간격(초, `0`이면 끔)
- `--pcap-out capture.pcap`: 패킷을 libpcap 형식으로 덤프 (Wireshark 호환)
- `--pcap-stage pre|post|both`: 모듈 적용 전/후 중 어느 시점을 기록할지 (기본 `post`)
- `--pcap-max-packets` / `--pcap-max-bytes`: 덤프 크기 상한
- `--report-out report.html`: 캡처 종료 시 HTML 세션 리포트 생성
- `--enable-plugins <디렉토리>`: 커스텀 모듈 DLL 로드 (기본 비활성, 보안 주의)
- `--verbose on`: 패킷 단위 트레이스 로그
- `--elevate on`: 관리자 권한이 아니면 UAC로 재실행


## 웹 대시보드와 REST API

기본값은 `127.0.0.1:8080`이며 로컬호스트에서는 토큰 없이 접근할 수 있습니다.

| 인수 | 설명 |
|------|------|
| `--web off` | 웹 서버 비활성화 |
| `--web-port 9000` | 포트 변경 |
| `--web-bind 0.0.0.0` | 외부 인터페이스 바인딩 (**토큰 인증 강제 + 보안 경고 출력**) |
| `--web-token <토큰>` | 고정 토큰 지정 (CI 스크립트에서 재사용) |

외부 바인딩 시 토큰은 `X-Clumsy-Token` 헤더나 `?token=` 쿼리스트링으로 전달합니다.

주요 엔드포인트 (전체 목록은 `GET /api/docs`):

| 메서드 / 경로 | 설명 |
|---|---|
| `GET /api/health` | 생존 확인 (인증 불필요, CI 헬스체크용) |
| `GET /api/status` | 캡처 상태, 필터, 마지막 메시지 |
| `GET /api/modules` | 모듈 전체 상태 + 폼 자동 생성용 ParamSpec |
| `GET /api/stats` | 실시간 통계 |
| `GET /api/stream` | Server-Sent Events, 200ms 주기 통계 push |
| `GET /api/report` | HTML 세션 리포트 다운로드 |
| `POST /api/filter` | 필터 설정 후 캡처 시작 |
| `POST /api/stop` | 캡처 중지 |
| `POST /api/modules/{shortName}` | 모듈 활성화/파라미터 변경 |
| `POST /api/profiles/{name}/apply` | 프로파일 적용 |
| `POST /api/presets` | 필터 프리셋 저장 (config.json에 기록) |
| `POST /api/pcap/start` / `stop` | pcap 덤프 제어 |

예시:

```bash
curl http://127.0.0.1:8080/api/status
curl -X POST http://127.0.0.1:8080/api/modules/drop \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"drop-chance\":10.0}"
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
│   ├── jitter.cpp      # 모듈: 랜덤 지연 (min~max)
│   ├── drop.cpp        # 모듈: 확률적 패킷 드롭
│   ├── burstloss.cpp   # 모듈: 버스트 손실 (Gilbert-Elliott)
│   ├── blackout.cpp    # 모듈: 연결 두절
│   ├── throttle.cpp    # 모듈: 일시적 패킷 억제
│   ├── duplicate.cpp   # 모듈: 패킷 복제
│   ├── ood.cpp         # 모듈: 패킷 순서 뒤섞기
│   ├── tamper.cpp      # 모듈: 페이로드 변조
│   ├── reset.cpp       # 모듈: TCP RST 강제 전송
│   ├── bandwidth.cpp   # 모듈: 대역폭 제한
│   ├── pipe.cpp        # Named Pipe 제어 API (트랜스포트만)
│   ├── scenario.cpp    # 시나리오 스크립팅 (시간/조건/반복 트리거)
│   ├── profile.cpp     # 프로파일 저장/불러오기
│   ├── statslog.cpp    # 통계 로그 파일 출력
│   ├── procfilter.cpp  # 프로세스별 필터링
│   ├── pcapexport.cpp  # libpcap 형식 패킷 덤프
│   ├── report.cpp      # HTML 세션 리포트 생성
│   └── plugin.cpp      # 커스텀 모듈 DLL 로더 (옵션)
├── etc/                # 설정 파일, 리소스
│   ├── config.json     # 필터 프리셋 (JSON)
│   ├── config.txt      # 필터 프리셋 (레거시)
│   ├── web/index.html  # 웹 대시보드 (단일 정적 파일)
│   └── clumsy.rc       # Windows 리소스 파일
├── msvc/               # Visual Studio 프로젝트
│   └── clumsy.sln
├── external/           # 외부 라이브러리 (WinDivert, Windows 전용)
├── packaging/          # .deb / .rpm 패키징 정의
├── tests/              # 독립 실행 검증 프로그램
│   ├── packetutil_test.cpp   # 패킷 헬퍼 계약 테스트 (양 플랫폼 공용)
│   └── linux/nfqtest.cpp     # NFQUEUE 능력 프로브
├── docs/
│   ├── CODING_STYLE.md # 코드 스타일 원칙
│   └── LINUX.md        # 리눅스 빌드/실행/제약 가이드
├── Makefile            # 리눅스 빌드
├── manual.md           # 사용자 매뉴얼 (한국어)
└── TODO.md             # 개발 로드맵
```


## 상세 문서

- **사용자 매뉴얼**: [manual.md](manual.md) — 전체 기능, 웹 UI, CLI, API 상세 설명
- **리눅스 가이드**: [docs/LINUX.md](docs/LINUX.md) — 빌드, 권한, iptables 연동, 플랫폼 차이
- **코드 스타일**: [docs/CODING_STYLE.md](docs/CODING_STYLE.md) — C++ 전환 이후 코드 규칙
- **개발 로드맵**: [TODO.md](TODO.md) — 완료된 작업 및 향후 계획


## 라이선스

MIT
