# clumsy

__clumsy는 Windows의 네트워크 환경을 의도적으로 악화시키는 도구입니다. 단, 제어 가능하고 대화형 방식으로 동작합니다.__

[WinDivert](http://reqrypt.org/windivert.html)를 활용하여 실행 중인 네트워크 패킷을 가로챈 뒤, 원하는 시점에 지연/드롭/변조 등을 적용하고 재전송합니다. 네트워크 관련 버그를 추적하거나, 열악한 연결 환경에서 애플리케이션을 평가할 때 유용합니다:

* 설치 불필요.
* 프록시 설정이나 애플리케이션 코드 변경 불필요.
* 시스템 전체 네트워크를 캡처하므로 모든 애플리케이션에 적용 가능.
* 오프라인 환경(localhost ↔ localhost)에서도 동작.
* 애플리케이션이 실행 중인 상태에서 clumsy를 언제든 시작/중지 가능.
* 네트워크 상태를 대화형으로 제어하며, 현재 상태를 시각적으로 확인 가능.


## 사전 준비

### 시스템 요구사항

- **OS**: Windows 7 / 8 / 10 / 11 (64비트 전용)
- **권한**: 관리자(Administrator) 권한 필요 (WinDivert 드라이버 로드를 위해)

### 실행 시 필요 파일

clumsy를 실행하려면 실행파일과 같은 디렉토리에 다음 파일들이 있어야 합니다:

| 파일 | 설명 | 출처 |
|------|------|------|
| `WinDivert.dll` | 패킷 캡처 라이브러리 | `external/WinDivert-2.2.0-A/x64/` |
| `WinDivert64.sys` | WinDivert 커널 드라이버 | `external/WinDivert-2.2.0-A/x64/` |
| `iup.dll` | GUI 라이브러리 | `external/iup-3.30_Win64_dll16_lib/` |
| `config.json` | 필터 프리셋 정의 (권장) | `etc/config.json` |
| `config.txt` | 필터 프리셋 정의 (레거시) | `etc/config.txt` |

> 빌드 시 post-build 단계에서 이 파일들이 출력 디렉토리에 자동 복사됩니다.


## 빌드

### 의존성

리포지토리의 `external/` 디렉토리에 포함되어 있어 별도 설치가 필요 없습니다:

| 라이브러리 | 버전 | 용도 |
|-----------|------|------|
| [WinDivert](https://reqrypt.org/windivert.html) | 2.2.0-A | 네트워크 패킷 캡처/재전송 |
| [IUP](https://www.tecgraf.puc-rio.br/iup/) | 3.30 | 크로스 플랫폼 GUI 툴킷 |

### 방법 1: Visual Studio (권장)

**요구사항**: Visual Studio 2022 이상 (C/C++ 데스크톱 개발 워크로드)

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
| Debug | Console App | 콘솔 출력으로 디버그 로그 확인 가능 |
| Release | Windows App | 콘솔 없음, `OutputDebugString`으로 로그 출력 |

### 방법 2: GENie + MinGW (MSYS2/Clang)

**요구사항**:
- [MSYS2](https://www.msys2.org/) 환경
- Clang 컴파일러 (`pacman -S mingw-w64-x86_64-clang`)
- [GENie](https://github.com/bkaradzic/GENie) 빌드 시스템

```bash
# 1. Makefile 생성
genie.exe gmake

# 2. 빌드
cd build
make config=release_x64
```

출력 경로: `bin/gmake/Release/x64/clumsy.exe`

### 방법 3: GENie + Visual Studio 프로젝트 생성

GENie로 VS 솔루션을 자동 생성할 수도 있습니다:

```bat
genie.exe vs2022
```

생성 경로: `build/clumsy.sln`


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


## CLI 사용법

실행 인수로 모듈을 설정하는 파라미터화 모드를 지원합니다:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 --drop on --drop-chance 5.0
```

| 모듈 | 인수 예시 |
|------|----------|
| lag | `--lag on`, `--lag-time 100`, `--lag-inbound on`, `--lag-outbound on` |
| jitter | `--jitter on`, `--jitter-min 20`, `--jitter-max 150` |
| drop | `--drop on`, `--drop-chance 5.0` |
| burstloss | `--burstloss on`, `--burstloss-p 2.0`, `--burstloss-q 80.0` |
| blackout | `--blackout on`, `--blackout-duration 3000`, `--blackout-gap 30000` |
| throttle | `--throttle on`, `--throttle-chance 10.0`, `--throttle-frame 30` |
| duplicate | `--duplicate on`, `--duplicate-chance 10.0` |
| ood | `--ood on`, `--ood-chance 10.0`, `--ood-buffer 5`, `--ood-delay 200` |
| tamper | `--tamper on`, `--tamper-chance 10.0`, `--tamper-position 4` |
| bandwidth | `--bandwidth on`, `--bandwidth-bandwidth 100` |

기타:
- `--timeout <초>`: 지정 시간 후 자동 종료
- `--scenario scenario.json`: 시나리오 파일 실행
- `--profile mobile-3g`: 프로파일 적용
- `--stats-log stats.csv`: 통계 로그 파일 출력
- `--stats-interval 1`: 통계 기록 간격(초)


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
├── src/              # C 소스 코드
│   ├── main.c        # GUI 초기화, 이벤트 루프
│   ├── divert.c      # WinDivert 패킷 캡처/전송
│   ├── common.h      # 공통 타입/매크로/Module 구조체
│   ├── lag.c          # 모듈: 고정 지연
│   ├── jitter.c       # 모듈: 랜덤 지연 (min~max)
│   ├── drop.c         # 모듈: 확률적 패킷 드롭
│   ├── burstloss.c    # 모듈: 버스트 손실 (Gilbert-Elliott)
│   ├── blackout.c     # 모듈: 연결 두절
│   ├── throttle.c     # 모듈: 일시적 패킷 억제
│   ├── duplicate.c    # 모듈: 패킷 복제
│   ├── ood.c          # 모듈: 패킷 순서 뒤섞기
│   ├── tamper.c       # 모듈: 페이로드 변조
│   ├── reset.c        # 모듈: TCP RST 강제 전송
│   ├── bandwidth.c    # 모듈: 대역폭 제한
│   ├── pipe.c         # Named Pipe 제어 API
│   ├── scenario.c     # 시나리오 스크립팅
│   ├── profile.c      # 프로파일 저장/불러오기
│   ├── statslog.c     # 통계 로그 파일 출력
│   ├── procfilter.c   # 프로세스별 필터링
│   └── ...
├── etc/              # 설정 파일, 리소스
│   ├── config.json   # 필터 프리셋 (JSON)
│   ├── config.txt    # 필터 프리셋 (레거시)
│   └── clumsy.rc     # Windows 리소스 파일
├── msvc/             # Visual Studio 프로젝트
│   └── clumsy.sln
├── external/         # 외부 라이브러리 (WinDivert, IUP)
├── genie.lua         # GENie 빌드 스크립트
├── manual.md         # 사용자 매뉴얼 (한국어)
└── TODO.md           # 개발 로드맵
```


## 상세 문서

- **사용자 매뉴얼**: [manual.md](manual.md) — 전체 기능, UI, CLI, API 상세 설명
- **개발 로드맵**: [TODO.md](TODO.md) — 완료된 작업 및 향후 계획


## 라이선스

MIT
