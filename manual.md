# clumsy 사용 매뉴얼

---

# 1부 — 사용 방법

## 1. 시작하기 전에

### 필요 조건

- **Windows 7 / 8 / 10 / 11** (64비트 전용)
- **관리자 권한 필수** — clumsy는 네트워크 드라이버를 로드하므로 반드시 관리자로 실행해야 합니다.
- 웹 대시보드를 볼 브라우저 (별도 설치 불필요, 로컬에서 동작)

### 실행 방법

clumsy는 **콘솔 애플리케이션**입니다. **관리자 권한 명령 프롬프트/PowerShell**에서 실행하세요.

```
clumsy.exe
```

실행하면 배너가 출력됩니다:

```
clumsy 0.4 - console + web network condition simulator
  Administrator : yes
  Web dashboard : http://127.0.0.1:8080/
  Named Pipe    : \\.\pipe\clumsy
  Presets       : 34 loaded
  Profiles      : 2 loaded
  Press Ctrl+C to quit.
```

배너에 표시된 주소를 브라우저로 열면 대시보드가 나타납니다. 종료는 `Ctrl+C`입니다.

관리자 권한이 아니면 clumsy는 종료되지 않고 **경고를 출력한 뒤 대시보드만 띄웁니다.**
이 상태에서 캡처를 시작하면 다음과 같은 명확한 오류가 반환됩니다:

```
clumsy needs Administrator rights to open the WinDivert driver.
Restart the console as Administrator.
```

`--elevate on`을 붙이면 UAC 창을 띄워 관리자 권한으로 자기 자신을 재실행합니다
(새 콘솔 창이 열리고 원래 인스턴스는 종료됩니다).

> **주의**: 이미 실행 중인 clumsy가 있으면 두 번째 실행은 자동으로 종료됩니다. 한 번에 하나만 실행 가능합니다.

> **버전 0.4부터의 변경**: 기존 IUP 기반 데스크톱 GUI 창은 제거되었습니다.
> 모든 조작은 웹 대시보드 · CLI 인수 · REST API · Named Pipe API로 수행합니다.

---

## 2. 웹 대시보드 화면 구성

브라우저로 `http://127.0.0.1:8080/`에 접속하면 다음 구성의 단일 페이지가 나타납니다.

```
┌───────────────────────────────────────────────────────────┐
│ ● clumsy v0.4          [Download report] [API]            │  헤더
├───────────────────────────────────────────────────────────┤
│ CAPTURE                                                   │
│  필터 입력창          Preset [▼]  Process [____]          │
│  [Start] [Stop]                                           │
│  상태 메시지                                              │
│  ▸ Filter builder  (프로토콜/방향/포트/IP 조합 → 필터 생성)│
├───────────────────────────────────────────────────────────┤
│ LIVE STATISTICS                                           │
│  [Captured] [Sent] [Packets/sec] [Elapsed] [Buffers]      │
│  ┌── 실시간 throughput 그래프 ──┐                         │
├───────────────────────────────────────────────────────────┤
│ MODULES                                                   │
│  ☑ Lag        Inbound ☑ Outbound ☑ Delay(ms)[250]  12 affected│
│  ☐ Jitter     ...                                         │
│  ☐ Drop       ...                                         │
│  (11개 모듈)                                              │
├───────────────────────────────────────────────────────────┤
│ PROFILES, SCENARIO AND CAPTURE FILE                       │
│  Profile [▼] [Apply]   Save as [____] [Save]              │
│  Scenario [________] [Load] [Start] [Stop]                │
│  pcap     [________] [Start dump] [Stop dump]             │
└───────────────────────────────────────────────────────────┘
```

- **헤더의 점(●)**: 실시간 스트림(Server-Sent Events) 연결 상태. 초록이면 정상 연결입니다.
- **CAPTURE**: 어떤 패킷을 가로챌지 설정합니다. Preset 드롭다운은 `config.json`에서,
  Process 입력란은 실행파일 이름으로 해당 프로세스의 패킷만 대상으로 삼습니다.
- **Filter builder**: WinDivert 필터 문법을 몰라도 조합으로 표현식을 생성합니다
  ([3.5절](#필터-빌더) 참조).
- **LIVE STATISTICS**: 200ms 주기로 push되는 실시간 수치와 초당 패킷 그래프입니다.
- **MODULES**: 각 모듈의 체크박스를 켜면 파라미터 입력 폼이 펼쳐집니다.
  이 폼은 서버가 내려주는 `ParamSpec` 메타데이터로 **자동 생성**되므로,
  새 모듈이 추가되어도 대시보드는 그대로 동작합니다.
- **오프라인 동작**: 대시보드는 CDN이나 외부 프레임워크를 전혀 사용하지 않는
  단일 HTML 파일(`web/index.html`)입니다. 인터넷이 없는 테스트망에서도 그대로 동작합니다.

### 콘솔 출력

콘솔에는 상태 변화와 주기적 요약이 출력됩니다:

```
Capturing. filter="udp and outbound"
[  10s] captured=15234 sent=15102 lag=15234 drop=132
[  20s] captured=31007 sent=30761 lag=31007 drop=246
```

- 요약 주기는 `--stats-console <초>`로 조정합니다 (기본 10초, `0`이면 끔).
- `--verbose on`을 주면 패킷 단위 상세 트레이스가 출력됩니다 (성능 영향이 크므로 디버깅 시에만).

---

## 3. 필터 설정

### 기본 개념

필터는 "어떤 패킷을 clumsy가 가로챌 것인가"를 결정합니다. 필터에 걸린 패킷만 지연/드롭 등의 효과를 받습니다.

### Presets 드롭다운 활용

창 오른쪽의 **Presets** 드롭다운을 클릭하면 `etc/config.json` (또는 레거시 `etc/config.txt`)에 저장된 미리 정의된 필터 목록이 나타납니다. 선택하면 필터 입력창에 자동으로 채워집니다.

기본 제공 프리셋:

| 프리셋 이름 | 설명 |
|------------|------|
| localhost ipv4 all | 내 PC 내부 통신(loopback) 전체 |
| localhost ipv4 tcp | 내 PC 내부 TCP 통신 |
| localhost ipv4 udp | 내 PC 내부 UDP 통신 |
| all sending packets | 모든 송신 패킷 |
| all receiving packets | 모든 수신 패킷 |
| all ipv4 against specific ip | 특정 IP와의 통신 |
| udp ipv4 against port | 특정 포트의 UDP 통신 |

### 필터 직접 입력

필터 문법은 WinDivert의 필터 언어를 사용합니다. 자주 쓰는 표현:

```
# UDP 통신만
udp

# TCP 통신만
tcp

# 송신 패킷만
outbound

# 수신 패킷만
inbound

# 특정 IP와의 UDP 통신
udp and (ip.DstAddr == 192.168.1.10 or ip.SrcAddr == 192.168.1.10)

# 특정 포트(예: 7777) UDP
udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)

# loopback(localhost) — 반드시 outbound 조건 필요
udp and outbound and loopback
```

> **loopback 주의사항**: localhost(127.x.x.x) 통신을 테스트할 때는 반드시 `outbound` 조건을 포함해야 합니다. `inbound and loopback` 조합은 동작하지 않습니다.

### Start / Stop

필터를 입력한 후 **Start** 버튼을 누르면 패킷 가로채기가 시작됩니다.
- 필터 문법이 잘못되면 상태 영역에 오류 메시지가 표시됩니다.
- 실행 중에는 Start 버튼이 비활성화됩니다.
- 중지하려면 **Stop** 버튼을 누릅니다.

CLI에서는 `--filter "식"`을 주면 실행 즉시 캡처가 시작됩니다.
REST API로는 `POST /api/filter` / `POST /api/stop`, Named Pipe API로는
`{"cmd":"filter",...}` / `{"cmd":"stop_capture"}`가 같은 동작을 합니다.

### 필터 빌더

대시보드의 **Filter builder**를 펼치면 프로토콜(TCP/UDP/ICMP), 방향(inbound/outbound),
포트(단일 또는 `7770-7780` 범위), 원격 IPv4, loopback 여부를 조합해
필터 표현식이 실시간으로 생성됩니다.

- **Use this**: 생성된 표현식을 위 필터 입력창에 넣습니다.
- **Save as preset...**: 이름을 지정해 `config.json`에 프리셋으로 저장합니다
  (`POST /api/presets`). 저장된 프리셋은 Preset 드롭다운에 바로 나타납니다.

전부 브라우저에서 처리되므로 서버 왕복이 없습니다.

### 프로세스별 필터링

**Process** 입력란에 실행파일 이름(예: `game.exe`)을 입력하고 Start를 누르면, 해당 프로세스의 네트워크 트래픽만 필터 대상이 됩니다. 비워두면 전체 패킷에 적용됩니다.

**동작 원리**: Start 시점에 해당 프로세스의 PID를 찾고, TCP/UDP 포트 테이블에서 그 프로세스가 사용 중인 로컬 포트를 수집하여 필터에 자동 삽입합니다.

**CLI 인수**: `--process game.exe`

```
# 예시: game.exe 프로세스의 UDP 트래픽에만 100ms 지연 적용
clumsy.exe --filter "udp" --process game.exe --lag on --lag-time 100
```

**제한 사항**:
- 포트는 Start 시점에 스냅샷됩니다. Start 이후에 프로세스가 새로 연 연결은 필터에 포함되지 않습니다.
- 따라서 **게임을 먼저 실행하고 서버에 접속한 후** clumsy를 Start하는 것이 좋습니다.
- 프로세스가 실행 중이 아니거나 활성 포트가 없으면 오류 메시지가 표시됩니다.

---

## 4. 기능(모듈) 설명

Start를 누른 후, Functions 영역에서 원하는 기능의 체크박스를 켜면 효과가 적용됩니다. 여러 기능을 동시에 켤 수 있습니다.

각 기능에는 공통으로 **Inbound(수신)** / **Outbound(송신)** 체크박스가 있어, 방향별로 적용 여부를 선택할 수 있습니다.

---

### Lag — 지연

패킷을 지정한 시간(ms)만큼 지연시킨 후 전송합니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Delay(ms) | 지연 시간 | 50ms | 0 ~ 15000ms |

**동작 방식**: 패킷을 내부 버퍼에 저장했다가 지정 시간이 지나면 전송합니다. 모든 패킷이 동일한 시간만큼 지연됩니다.

**언제 사용**: 일정한 왕복 지연(RTT)을 만들고 싶을 때. 예: "이 서버는 항상 100ms 지연이 있다"는 상황 재현.

```
예시: Delay = 100 → 모든 패킷이 100ms 늦게 도착
```

---

### Jitter — 지연 편차

패킷마다 Min~Max 범위 내의 랜덤한 지연을 추가합니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Min(ms) | 최소 지연 | 20ms | 0 ~ 5000ms |
| Max(ms) | 최대 지연 | 100ms | 0 ~ 5000ms |

**동작 방식**: 각 패킷에 `[Min, Max]` 범위의 랜덤 지연을 부여합니다. 지연이 다르므로 패킷 도착 순서가 바뀔 수 있습니다(자연스러운 순서 뒤섞임 발생).

**언제 사용**: 실제 인터넷 환경처럼 지연이 불규칙하게 변하는 상황 재현. 랙 보상(lag compensation) 알고리즘 테스트.

```
예시: Min=20, Max=150 → 어떤 패킷은 20ms, 어떤 패킷은 150ms 지연
     → 패킷 도착 순서가 섞일 수 있음
```

> **Lag vs Jitter**: Lag은 모든 패킷에 동일한 지연(일정한 RTT), Jitter는 패킷마다 다른 지연(불규칙한 RTT). 실제 인터넷은 두 가지가 혼재합니다.

---

### Drop — 패킷 드롭

지정한 확률로 패킷을 버립니다(전송하지 않음).

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Chance(%) | 드롭 확률 | 10.0% | 0 ~ 100% |

**동작 방식**: 각 패킷에 대해 독립적으로 확률을 계산합니다. 10%면 평균적으로 10개 중 1개가 버려집니다.

**언제 사용**: 패킷 손실이 발생하는 환경 테스트. 재전송 로직, 패킷 손실 복구 코드 검증.

```
예시: Chance=5.0 → 패킷 100개 중 약 5개가 사라짐
```

---

### Burst Loss — 연속 패킷 손실 (Gilbert-Elliott 모델)

실제 네트워크처럼 패킷 손실이 연속적으로 뭉쳐서 발생하는 상황을 재현합니다.

**두 가지 상태**:
- **Good(정상)**: 패킷이 거의 정상 전달되는 평상시 상태
- **Bad(버스트)**: 패킷이 집중적으로 손실되는 혼잡/간섭 상태

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Good(%) | Good 상태에서의 드롭 확률 | 2.0% | 0 ~ 100% |
| Bad(%) | Bad 상태에서의 드롭 확률 | 80.0% | 0 ~ 100% |
| G>B(%) | 패킷당 Good→Bad 전환 확률 | 5.0% | 0 ~ 100% |
| B>G(%) | 패킷당 Bad→Good 전환 확률 | 20.0% | 0 ~ 100% |

**동작 방식**: 매 패킷마다 현재 상태의 드롭 확률로 손실 여부를 결정하고, 전환 확률에 따라 상태를 바꿉니다.

기본값 기준 계산:
- Bad 상태 평균 지속 시간: 1 ÷ 20% = **패킷 5개**
- Good 상태 평균 지속 시간: 1 ÷ 5% = **패킷 20개**
- Bad 상태에 머무는 비율: 5% ÷ (5% + 20%) = **20%**
- 전체 평균 손실률: 2% × 80% + 80% × 20% ≈ **17.6%**

**언제 사용**: 모바일 네트워크나 Wi-Fi에서 발생하는 연속 손실 재현. Drop 모듈(독립 확률)로는 재현할 수 없는 연속 패킷 손실 시나리오 테스트.

```
예시: 기본값으로 사용 시
  → 정상 구간:  패킷 20개마다 평균 1개만 손실 (2%)
  → 버스트 구간: 패킷 5개 중 4개 손실 (80%), 5개 패킷 동안 지속
  → Drop 모듈과 달리 손실이 "몰려서" 발생
```

> **Drop vs Burst Loss**: Drop은 패킷마다 독립적으로 확률 계산(동전 던지기). Burst Loss는 한번 나쁜 구간에 진입하면 연속적으로 손실(눈이 오면 계속 온다).

---

### Blackout — 연결 두절

지정한 주기로 모든 패킷을 완전히 차단해 연결이 끊기는 상황을 재현합니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Dur(ms) | 연결 두절 지속 시간 | 3000ms | 100 ~ 60000ms |
| Gap(ms) | 두절 사이의 정상 간격 | 15000ms | 1000 ~ 300000ms |
| Trigger 버튼 | 즉시 두절 시작 / 취소 | — | 클릭 |

**동작 방식**: 활성화 후 Gap(ms)가 지나면 Dur(ms) 동안 모든 패킷을 드롭합니다. 이 사이클이 반복됩니다.

```
타임라인 예시 (Gap=15000, Dur=3000):
  0s       15s      18s      33s      36s
  |─ 정상 ─|─ 두절 ─|─ 정상 ─|─ 두절 ─| ...
```

**Trigger 버튼**:
- 정상 구간 중 클릭 → 즉시 두절 시작 (주기 리셋)
- 두절 중 클릭 → 즉시 정상 복구 (두절 취소)

**언제 사용**: 클라이언트 재접속 로직, 연결 타임아웃 처리, 세션 복구 코드 테스트. Throttle(일시 억제)과 달리 보류 없이 즉각 드롭합니다.

> **Throttle vs Blackout**: Throttle은 패킷을 잠시 쌓았다가 한꺼번에 보내거나 버립니다. Blackout은 버퍼 없이 두절 구간의 패킷을 즉시 전부 드롭합니다.

---

### Throttle — 일시적 패킷 억제

확률적으로 패킷 전송을 일시 중단하고, 지정 시간이 지나면 묶어서 한꺼번에 전송하거나 전부 버립니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Timeframe(ms) | 억제 지속 시간 | 30ms | 0 ~ 1000ms |
| Chance(%) | 억제 시작 확률 | 10.0% | 0 ~ 100% |
| Drop Throttled | 억제된 패킷을 버릴지 여부 | OFF | ON/OFF |

**동작 방식**: 억제가 시작되면 해당 시간 동안 패킷을 버퍼에 쌓습니다.
- `Drop Throttled OFF`: 시간이 지나면 쌓인 패킷을 한꺼번에 전송 → 순간적인 패킷 폭발(burst) 발생
- `Drop Throttled ON`: 시간이 지나면 쌓인 패킷 모두 버림 → 짧은 완전 연결 두절

**언제 사용**: 네트워크가 주기적으로 끊기거나 폭발적으로 패킷이 몰리는 상황 재현.

---

### Duplicate — 패킷 복제

패킷을 지정한 개수만큼 복제해서 보냅니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Count | 복제 개수(원본 포함) | 2 | 2 ~ 50 |
| Chance(%) | 복제 적용 확률 | 10.0% | 0 ~ 100% |

**동작 방식**: 확률에 걸린 패킷을 Count개 복사해서 연속 전송합니다. Count=2면 원본 1개 + 복사본 1개 = 총 2개 전송.

**언제 사용**: 중복 패킷 처리 로직 테스트. 특히 UDP 게임에서 같은 패킷이 두 번 오는 상황 검증.

---

### Out of Order — 패킷 순서 뒤섞기

패킷의 도착 순서를 의도적으로 바꿉니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Chance(%) | 버퍼에 포획할 확률 | 10.0% | 0 ~ 100% |
| Buf | 최대 보류 패킷 수 | 5 | 2 ~ 50 |
| Delay(ms) | 최대 보류 시간 | 200ms | 10 ~ 2000ms |

**동작 방식**: 확률에 걸린 패킷을 내부 버퍼(최대 Buf개)에 쌓습니다. 버퍼가 가득 차거나 가장 오래된 패킷이 Delay(ms)를 초과하면 버퍼 내 모든 패킷을 Fisher-Yates 셔플로 무작위 순서로 방출합니다. 실제 네트워크의 비순서 도착을 현실적으로 재현합니다.

**언제 사용**: 패킷이 순서 없이 도착하는 상황 테스트. UDP 기반 게임의 시퀀스 번호 처리, 패킷 재조립 로직 검증. Buf와 Delay를 크게 설정할수록 더 심각한 순서 뒤섞임을 만들 수 있습니다.

> **Jitter와의 차이**: Jitter는 각 패킷에 다른 지연을 줘서 자연스럽게 순서가 뒤섞립니다. Out of Order는 여러 패킷을 모아 셔플 후 한꺼번에 방출합니다.

---

### Tamper — 패킷 변조

패킷 내용(페이로드)의 일부를 의도적으로 손상시킵니다.

| 항목 | 설명 | 기본값 | 선택지 |
|------|------|--------|--------|
| Chance(%) | 변조 확률 | 10.0% | 0 ~ 100% |
| Redo Checksum | 변조 후 체크섬 재계산 | ON | ON/OFF |
| Position | 변조할 페이로드 위치 | Center | Front / Center / Back / Random |

**동작 방식**: 페이로드의 1/4 크기 구간을 XOR 패턴으로 덮어씁니다. 어느 부분을 건드릴지는 Position으로 결정합니다.

| Position | 설명 |
|----------|------|
| Front | 페이로드 앞쪽 1/4 — 게임 패킷 헤더·시퀀스 번호 영역 타격 |
| Center | 페이로드 중간 1/4 (기본값) — 일반적인 데이터 영역 |
| Back | 페이로드 뒤쪽 1/4 — 체크섬·푸터가 여기 있는 프로토콜에 유효 |
| Random | 매 패킷마다 오프셋을 랜덤 선택 — 예측 불가한 손상 재현 |

`Redo Checksum`이 켜져 있으면 변조 후 IP/TCP/UDP 체크섬을 재계산해서 수신 측이 체크섬 오류로 즉시 거부하지 않게 합니다. 체크섬 무결성을 유지한 채로 페이로드만 손상된 상황을 테스트할 때 사용합니다.

4바이트 이하의 짧은 패킷은 Position 설정과 무관하게 페이로드 전체를 변조합니다.

**언제 사용**: 손상된 데이터 수신 시 애플리케이션 동작 테스트. 게임 프로토콜의 시퀀스 번호나 헤더를 손상시켜 서버/클라이언트의 에러 처리 로직 확인. Position=Front로 설정하면 고정 오프셋 헤더를 가진 게임 패킷의 파싱 오류를 유발할 수 있습니다.

---

### Set TCP RST — TCP 연결 강제 종료

TCP 패킷에 RST 플래그를 설정해서 연결을 강제로 끊습니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Chance(%) | RST 주입 확률 | 0% | 0 ~ 100% |
| RST next packet 버튼 | 다음 패킷에 즉시 RST 주입 | — | 버튼 클릭 |

**동작 방식**: TCP 패킷의 헤더에 RST 비트를 설정합니다. 수신 측은 연결이 비정상 종료된 것으로 처리합니다.

**언제 사용**: TCP 연결 강제 종료 시 클라이언트/서버의 재연결 처리 테스트. `RST next packet` 버튼으로 원하는 시점에 즉시 연결을 끊을 수 있습니다.

> **주의**: UDP에는 효과가 없습니다. TCP 연결에만 동작합니다.

---

### Bandwidth — 대역폭 제한

> **초기 버스트 허용량**: 토큰 버킷은 `max(64KB, 제한값×2초)` 크기로 **가득 찬 상태에서
> 시작**합니다. 따라서 낮은 제한값(예: 1KB/s)을 걸어도 **처음 약 64KB는 즉시 통과**하고
> 그 이후부터 제한이 걸립니다. 64KB 하한이 있는 이유는 최대 크기 패킷(65535B) 하나가
> 낮은 제한값에서 영원히 나가지 못하는 상황을 막기 위해서입니다.
> 짧은 테스트에서 "제한이 안 걸린다"고 보이면 전송량이 버킷보다 작은 것입니다.

초당 전송량(KB/s)을 제한합니다. 한도를 초과하는 패킷은 **큐에 쌓아 지연 전송**합니다.

| 항목 | 설명 | 기본값 | 범위 |
|------|------|--------|------|
| Limit(KB/s) | 대역폭 상한 | 10 KB/s | 0 ~ 99999 KB/s |

**동작 방식 (토큰 버킷 알고리즘)**:

```
토큰 = Limit KB/s 속도로 매 스텝 충전 (최대 2초치 저장)
패킷 전송 시 → 패킷 크기만큼 토큰 소비
토큰 부족 시 → 큐에 보관, 토큰이 충전되면 순서대로 전송
큐가 가득 찼을 때(2000개) → 초과 패킷 드롭 (최후 수단)
```

- `Limit=500` → 초당 500KB 속도로 큐에서 순서대로 방출, 초과 트래픽은 지연됨
- `Limit=0` → 토큰 충전 없음 = 모든 패킷 큐에 쌓이다가 결국 드롭 (완전 차단)
- 모듈 비활성화 시 → 큐에 쌓인 패킷 즉시 전부 방출 (손실 없음)

**언제 사용**: 저속 모바일 데이터, 혼잡한 네트워크 환경 재현. 패킷을 버리는 대신 지연을 발생시키므로 TCP 흐름 제어, 재전송 메커니즘 테스트에 더 적합합니다.

```
예시: Limit=100 (100KB/s)
  → 100바이트 패킷은 약 1ms마다 1개 전송 가능
  → 갑자기 10KB 데이터가 들어오면 약 100ms에 걸쳐 분산 전송
  → Drop처럼 사라지지 않고 뒤늦게 도착
```

> **Drop vs Bandwidth**: Drop은 일부 패킷을 영구 손실시킵니다. Bandwidth는 패킷을 보존하되 속도를 제한해 지연을 만듭니다.

---

## 실시간 통계 패널

필터링이 시작되면 대시보드의 **LIVE STATISTICS** 영역에 실시간 수치가 표시됩니다.
서버가 `GET /api/stream`(Server-Sent Events)으로 200ms 주기로 push하며,
정지(Stop) 시 모든 카운터가 초기화됩니다.

### 표시 항목

| 카드 | 설명 |
|------|------|
| `Captured` | 총 캡처 패킷 수 |
| `Sent` | 총 전송(재주입) 패킷 수 |
| `Packets / sec` | 초당 캡처 속도 |
| `Elapsed` | 캡처 경과 시간 |
| `Buffers` | Lag / Jitter / Bandwidth 모듈의 현재 내부 버퍼 크기 |

그 아래 그래프는 최근 180개 샘플의 초당 패킷 수를 그립니다(Canvas API 직접 렌더링).
각 모듈 행 오른쪽에는 해당 모듈이 처리한 누적 패킷 수가 표시됩니다.

### 모듈별 카운터 의미

- **Lag / Jitter**: 지연 버퍼에 들어간 패킷 수 (누적)
- **Drop / Burst Loss / Blackout**: 드롭된 패킷 수
- **Throttle**: 억제 버퍼에 들어간 패킷 수
- **Duplicate**: 생성된 복제 패킷 수 (원본 제외)
- **Out of Order**: 재정렬 버퍼에 들어간 패킷 수
- **Tamper**: 변조된 패킷 수
- **Set TCP RST**: RST 주입된 패킷 수
- **Bandwidth**: 대역폭 큐에 들어간 패킷 수

### API 연동

- **REST**: `GET /api/stats` — 위 수치 전부 + 모듈별 카운터 + pcap/시나리오 상태를 JSON으로 반환
- **SSE**: `GET /api/stream` — 같은 페이로드를 200ms마다 push
- **Named Pipe**: `{"cmd":"get_stats"}` — 기존과 동일한 응답 형식 유지

자세한 내용은 [제어 API](#7-제어-api-rest--named-pipe) 섹션을 참조하세요.

### 통계 로그 파일 출력

CLI 인수 `--stats-log`를 지정하면, 필터링 동작 중 주기적으로 통계 데이터를 파일에 기록합니다. 테스트 결과 리포트, CI/CD 파이프라인에서의 자동 분석에 유용합니다.

```
clumsy.exe --filter "..." --stats-log stats.csv --stats-interval 1
```

| 인수 | 설명 |
|------|------|
| `--stats-log <파일>` | 출력 파일 경로. 확장자가 `.json`이면 JSON 배열, 그 외는 CSV |
| `--stats-interval <초>` | 기록 주기 (기본: 1초) |

#### CSV 형식

```csv
elapsed_sec,captured,sent,pps,lag,jitter,drop,burstloss,blackout,throttle,duplicate,ood,tamper,reset,bandwidth,lag_buf,jitter_buf,bw_buf,bw_limit_kbps
1.0,156,150,156,50,0,6,0,0,0,0,0,0,0,150,3,0,2,100
2.0,312,298,156,100,0,14,0,0,0,0,0,0,0,298,5,0,1,100
```

- 첫 행: 헤더 (모듈 이름은 `shortName` 기준)
- `elapsed_sec`: 필터링 시작 후 경과 시간 (초)
- `pps`: 해당 구간 초당 캡처 패킷 수
- 모듈 열: 누적 처리 패킷 수 (`affectedCount`)
- `lag_buf` / `jitter_buf` / `bw_buf`: 해당 시점 내부 버퍼 크기
- `bw_limit_kbps`: Bandwidth 모듈 설정값

#### JSON 형식

```json
[
  {"elapsed_sec":1.0,"captured":156,"sent":150,"pps":156,"modules":{"lag":50,"jitter":0,"drop":6,...},"lag_buf":3,"jitter_buf":0,"bw_buf":2,"bw_limit_kbps":100},
  {"elapsed_sec":2.0,"captured":312,"sent":298,"pps":156,"modules":{"lag":100,...},"lag_buf":5,"jitter_buf":0,"bw_buf":1,"bw_limit_kbps":100}
]
```

> 필터링을 정지(Stop)하거나 프로그램이 종료되면 파일이 자동으로 닫힙니다.

---

## 5. CLI 사용법 (자동화)

명령줄 인수만으로 clumsy를 완전히 구동할 수 있습니다. 대시보드를 열지 않아도 되고,
`--web off`를 주면 웹 서버 자체를 끌 수도 있습니다.

`clumsy.exe --help`로 전체 목록을 언제든 확인할 수 있습니다.

### 기본 형식

```
clumsy.exe --filter "필터식" --모듈명 on [--모듈명-옵션 값] ...
```

### 전체 인수 목록

```
--filter "식"          WinDivert 필터 (필수)
--timeout <초>         지정 초 후 자동 종료
--scenario <파일>      시나리오 JSON 파일 로드 (필터링 시작과 동시에 재생)
--profile <이름>       profiles.json에서 프로파일 불러와 적용
--process <이름>       특정 프로세스의 트래픽만 필터링 (예: game.exe)

--lag on/off
--lag-time <ms>        지연 시간 (기본: 50)
--lag-inbound on/off
--lag-outbound on/off

--jitter on/off
--jitter-min <ms>      최소 지연 (기본: 20)
--jitter-max <ms>      최대 지연 (기본: 100)
--jitter-inbound on/off
--jitter-outbound on/off

--drop on/off
--drop-chance <%>      드롭 확률 (기본: 10.0)
--drop-inbound on/off
--drop-outbound on/off

--burstloss on/off
--burstloss-good <%>   Good 상태 드롭 확률 (기본: 2.0)
--burstloss-bad <%>    Bad 상태 드롭 확률  (기본: 80.0)
--burstloss-gb <%>     Good->Bad 전환 확률 (기본: 5.0)
--burstloss-bg <%>     Bad->Good 전환 확률 (기본: 20.0)
--burstloss-inbound on/off
--burstloss-outbound on/off

--blackout on/off
--blackout-duration <ms>   두절 지속 시간 (기본: 3000)
--blackout-gap <ms>        두절 사이 정상 간격 (기본: 15000)
--blackout-inbound on/off
--blackout-outbound on/off

--throttle on/off
--throttle-chance <%>
--throttle-frame <ms>  억제 지속 시간 (기본: 30)
--throttle-inbound on/off
--throttle-outbound on/off

--duplicate on/off
--duplicate-chance <%>
--duplicate-count <n>  복제 개수 (기본: 2)
--duplicate-inbound on/off
--duplicate-outbound on/off

--ood on/off
--ood-chance <%>
--ood-buffer <n>       최대 보류 패킷 수 (기본: 5, 범위: 2~50)
--ood-delay <ms>       최대 보류 시간 ms (기본: 200, 범위: 10~2000)
--ood-inbound on/off
--ood-outbound on/off

--tamper on/off
--tamper-chance <%>
--tamper-position <1~4>   변조 위치: 1=Front, 2=Center(기본), 3=Back, 4=Random
--tamper-checksum on/off
--tamper-inbound on/off
--tamper-outbound on/off

--reset on/off
--reset-chance <%>     RST 주입 확률 (기본: 0)
--reset-inbound on/off
--reset-outbound on/off

--bandwidth on/off
--bandwidth-bandwidth <KB/s>   대역폭 상한 (기본: 10)
--bandwidth-inbound on/off
--bandwidth-outbound on/off

--stats-log <파일>     통계 로그 파일 경로 (.csv 또는 .json)
--stats-interval <초>  로그 기록 주기, 초 단위 (기본: 1)
--stats-console <초>   콘솔 상태 요약 출력 주기 (기본: 10, 0이면 끔)

--pcap-out <파일>          패킷을 libpcap 형식으로 덤프 (Wireshark 호환)
--pcap-stage pre|post|both 모듈 적용 전/후 중 어느 시점을 기록할지 (기본: post)
--pcap-max-packets <n>     덤프 패킷 수 상한 (0=무제한)
--pcap-max-bytes <n>       덤프 바이트 수 상한 (0=무제한)

--report-out <파일>    캡처 종료 시 HTML 세션 리포트 생성
--enable-plugins <dir> 커스텀 모듈 DLL 로드 (기본 비활성, 보안 주의)

--web off              웹 대시보드 비활성화
--web-port <n>         웹 서버 포트 (기본: 8080)
--web-bind <주소>      바인딩 주소 (기본: 127.0.0.1, 그 외에는 토큰 인증 강제)
--web-token <토큰>     고정 인증 토큰 (CI 스크립트에서 재사용)

--verbose on/off       패킷 단위 트레이스 로그 (Debug 기본 on, Release 기본 off)
--elevate on           관리자 권한이 아니면 UAC로 자기 자신을 재실행
--help                 인수 목록 출력 후 종료
```

> 참고: `--tamper-position`은 `0=Front, 1=Center(기본), 2=Back, 3=Random`입니다.
> (0.3까지의 UI 드롭다운은 1부터 시작했지만, API/CLI 값은 0부터입니다.)

### 예시

```
# 게임 서버(7777 포트) UDP에 100ms 지연 + 3% 드롭 적용
clumsy.exe --filter "udp and (udp.DstPort==7777 or udp.SrcPort==7777)" ^
           --lag on --lag-time 100 ^
           --drop on --drop-chance 3.0

# 30초 동안만 실행 후 자동 종료
clumsy.exe --filter "udp and outbound" --lag on --lag-time 200 --timeout 30

# 통계를 CSV로 매초 기록하면서 30초간 테스트
clumsy.exe --filter "udp and outbound" --lag on --lag-time 100 ^
           --stats-log stats.csv --stats-interval 1 --timeout 30

# 통계를 JSON으로 5초 간격 기록
clumsy.exe --filter "udp and outbound" --drop on --drop-chance 5.0 ^
           --stats-log result.json --stats-interval 5

# 패킷을 pcap으로 덤프하면서 세션 리포트까지 생성 (CI에서 산출물 수집)
clumsy.exe --filter "udp and outbound" --lag on --lag-time 120 ^
           --pcap-out capture.pcap --pcap-max-packets 50000 ^
           --report-out report.html --timeout 60

# 웹 대시보드를 끄고 완전 헤드리스로 실행
clumsy.exe --filter "udp and outbound" --drop on --drop-chance 5.0 ^
           --web off --stats-console 0 --timeout 30

# CI 러너가 원격 테스트 머신을 제어 (토큰 고정)
clumsy.exe --web-bind 0.0.0.0 --web-port 8080 --web-token ci-secret-token
```

---

## 6. 필터 프리셋 커스터마이징

### config.json (권장 형식)

`etc/config.json`을 편집기로 열어 자주 쓰는 필터를 추가할 수 있습니다.

```json
{
  "filters": [
    { "name": "my game server",    "filter": "udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)" },
    { "name": "local server test", "filter": "udp and outbound and loopback" }
  ]
}
```

`filters` 배열에 `{"name": "이름", "filter": "WinDivert 필터식"}` 객체를 추가하면 됩니다.

### config.txt (레거시 형식)

하위 호환을 위해 기존 `config.txt` 형식도 지원합니다. `config.json`이 없거나 파싱에 실패하면 자동으로 `config.txt`를 읽습니다.

형식: `프리셋 이름: WinDivert 필터식`

```
# 예시 추가
my game server: udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)
local server test: udp and outbound and loopback
```

> **우선순위**: `config.json` → `config.txt` → 기본 루프백 필터. 두 파일이 모두 있으면 `config.json`만 사용됩니다.

저장 후 clumsy를 재시작하면 Presets 드롭다운에 나타납니다.

### 기본 제공 게임 엔진/서비스 프리셋

config.json에 다음 게임 엔진 및 서비스용 프리셋이 기본 포함되어 있습니다:

| 프리셋 이름 | 프로토콜 | 포트 | 대상 |
|-------------|----------|------|------|
| `unreal engine (7777)` | UDP | 7777 | Unreal Engine 기본 게임 서버 |
| `unity netcode (9000)` | UDP | 9000 | Unity Netcode for GameObjects / Relay |
| `unity transport (7770-7780)` | UDP | 7770-7780 | Unity Transport 기본 범위 |
| `photon engine` | UDP | 5055-5056 | Photon Realtime / PUN2 |
| `steam game` | UDP | 27000-27036 | Steam 게임 트래픽 |
| `steam game+query` | UDP | 27000-27050 | Steam 게임 + 쿼리 포트 |
| `epic online services` | UDP | 3478-3479 | Epic STUN/TURN |
| `godot enet (6007)` | UDP | 6007 | Godot Engine ENet |
| `mirror networking (7777)` | TCP | 7777 | Mirror Networking (Unity) |
| `fishnet (7770)` | UDP | 7770 | FishNet (Unity) |
| `minecraft java` | TCP | 25565 | Minecraft Java Edition |
| `minecraft bedrock` | UDP | 19132 | Minecraft Bedrock Edition |
| `game udp high ports` | UDP | 7000-8000 | 일반적인 게임 UDP 포트 범위 |

### IPv6 프리셋

IPv6 환경에서의 게임 테스트를 위한 프리셋도 기본 포함되어 있습니다:

| 프리셋 이름 | 설명 |
|-------------|------|
| `ipv6 all` | 모든 IPv6 패킷 |
| `ipv6 loopback (::1)` | IPv6 루프백 주소 (`::1`) 트래픽 |
| `ipv6 tcp` | IPv6 TCP 패킷 |
| `ipv6 udp` | IPv6 UDP 패킷 |
| `ipv6 against specific ip` | 특정 IPv6 주소 대상 (예시: `2001:db8::1`) |
| `ipv6 against port` | IPv6에서 특정 포트 대상 |
| `ipv6 unreal engine (7777)` | Unreal Engine IPv6 UDP 7777 |
| `ipv6 unity netcode (9000)` | Unity Netcode IPv6 UDP 9000 |
| `ipv6 steam game` | Steam 게임 IPv6 UDP 27000-27036 |
| `ipv6 game udp high ports` | 일반 게임 IPv6 UDP 7000-8000 |

> IPv6 프리셋의 IP 주소(`2001:db8::1`)와 포트 번호는 예시입니다. 실제 환경에 맞게 config.json을 수정하세요.
>
> `ipv6 loopback (::1)`, `ipv6 against specific ip` 등 IPv6 주소를 포함하는 프리셋은 **config.json에서만** 사용 가능합니다. 레거시 config.txt 형식은 구분자로 `:`를 사용하므로 IPv6 주소(`::1`, `2001:db8::1`)가 포함된 필터를 올바르게 파싱할 수 없습니다.

> 프리셋의 포트 번호가 실제 환경과 다르면, config.json을 열어 수정하거나 새 항목을 추가하세요.

---

## 7. 제어 API (REST + Named Pipe)

clumsy는 두 가지 제어 트랜스포트를 동시에 제공합니다.

| 트랜스포트 | 주소 | 용도 |
|-----------|------|------|
| **HTTP REST** | `http://127.0.0.1:8080` (기본) | 웹 대시보드, CI 스크립트, curl |
| **Named Pipe** | `\\.\pipe\clumsy` | 기존 자동화 스크립트 (0.3 이하와 호환) |

두 트랜스포트는 내부적으로 **동일한 제어 계층**(`controlapi.cpp`)을 호출하므로
동작이 갈라지지 않습니다. 기존 Named Pipe 스크립트는 수정 없이 그대로 동작합니다.

---

### 7-1. REST API

인증은 바인딩 주소에 따라 결정됩니다:

- **로컬호스트 바인딩(기본)**: 토큰 불필요
- **`--web-bind`로 외부 바인딩**: 토큰 **필수**.
  `--web-token`으로 직접 지정하거나, 생략하면 시작 시 자동 생성되어 콘솔 배너에 출력됩니다.
  전달 방법은 `X-Clumsy-Token` 헤더 또는 `?token=` 쿼리스트링입니다.

#### 엔드포인트 목록

| 메서드 / 경로 | 인증 | 설명 |
|---|---|---|
| `GET /api/health` | 불필요 | 생존 확인. CI 헬스체크용 |
| `GET /api/status` | 필요 | 캡처 상태, 필터, 프로세스, 마지막 메시지 |
| `GET /api/modules` | 필요 | 모듈 전체 상태 + 현재 파라미터 + ParamSpec 폼 메타데이터 |
| `GET /api/stats` | 필요 | 실시간 통계 (SSE와 동일 페이로드) |
| `GET /api/stream` | 필요 | Server-Sent Events, 200ms 주기 통계 push |
| `GET /api/presets` | 필요 | `config.json`의 필터 프리셋 목록 |
| `GET /api/profiles` | 필요 | `profiles.json`의 프로파일 이름 목록 |
| `GET /api/report` | 필요 | HTML 세션 리포트 다운로드 |
| `GET /api/docs` | 필요 | 엔드포인트 목록(JSON) |
| `POST /api/filter` | 필요 | 필터 설정 후 캡처 시작 |
| `POST /api/stop` | 필요 | 캡처 중지 |
| `POST /api/quit` | 필요 | clumsy 종료 |
| `POST /api/modules/{shortName}` | 필요 | 모듈 활성화/파라미터 변경 |
| `POST /api/profiles` | 필요 | 현재 상태를 프로파일로 저장 |
| `POST /api/profiles/{name}/apply` | 필요 | 프로파일 적용 |
| `POST /api/presets` | 필요 | 필터 프리셋 저장 (config.json에 기록) |
| `POST /api/scenario/load` | 필요 | 시나리오 파일 로드 |
| `POST /api/scenario/start` \| `stop` | 필요 | 시나리오 재생 제어 |
| `POST /api/pcap/start` \| `stop` | 필요 | pcap 덤프 제어 |

#### 사용 예 (curl)

```bash
# 상태 조회
curl http://127.0.0.1:8080/api/status

# 캡처 시작
curl -X POST http://127.0.0.1:8080/api/filter \
     -H "Content-Type: application/json" \
     -d "{\"filter\":\"udp and outbound\",\"process\":\"game.exe\"}"

# lag 켜고 지연 200ms
curl -X POST http://127.0.0.1:8080/api/modules/lag \
     -H "Content-Type: application/json" \
     -d "{\"enabled\":true,\"lag-time\":200}"

# 원격 인스턴스 (토큰 필요)
curl -H "X-Clumsy-Token: ci-secret-token" http://10.0.0.5:8080/api/stats

# CI 헬스체크 (인증 불필요)
curl -f http://10.0.0.5:8080/api/health || echo "clumsy is down"

# 캡처 중지 후 리포트 수집
curl -X POST http://127.0.0.1:8080/api/stop
curl -o report.html http://127.0.0.1:8080/api/report
```

#### 실시간 스트림 구독 (Python)

```python
import requests, json

with requests.get("http://127.0.0.1:8080/api/stream", stream=True) as r:
    for line in r.iter_lines(decode_unicode=True):
        if line and line.startswith("data: "):
            stats = json.loads(line[6:])
            print(stats["captured"], stats["sent"])
```

#### ParamSpec으로 폼 자동 생성

`GET /api/modules`는 각 모듈의 입력 폼 정의를 함께 내려줍니다:

```json
{
  "shortName": "drop",
  "displayName": "Drop",
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

`type`은 `int` / `float` / `percent` / `bool` / `action` 중 하나입니다.
`action`은 값이 없는 일회성 트리거로, 대시보드는 버튼으로 렌더링합니다
(예: `blackout-trigger`, `reset-next`).

---

### 7-2. Named Pipe API

clumsy는 시작 시 자동으로 Named Pipe 서버(`\\.\pipe\clumsy`)를 열어 외부 프로세스에서 JSON 명령을 통해 제어할 수 있습니다. CI/CD 파이프라인이나 자동화 테스트 스크립트에서 clumsy를 원격으로 조작할 때 사용합니다.

### 프로토콜

연결 방식: 클라이언트 접속 → JSON 요청 전송 → JSON 응답 수신 → 연결 해제

### 지원 명령

#### `set` — 모듈 설정 변경

```json
{"cmd":"set","module":"모듈명","enabled":true,"파라미터키":값}
```

`module`은 모듈의 shortName (lag, jitter, drop, burstloss, blackout, throttle, ood, duplicate, tamper, reset, bandwidth).

`enabled` 필드가 있으면 모듈을 켜거나 끕니다. 그 외 필드는 해당 모듈의 파라미터로 적용됩니다.

**예시**:
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

**각 모듈의 설정 가능한 파라미터**:

| 모듈 | 파라미터 키 | 타입 | 범위 |
|------|------------|------|------|
| lag | lag-time | 정수(ms) | 0~15000 |
| jitter | jitter-min, jitter-max | 정수(ms) | 0~5000 |
| drop | drop-chance | 실수(%) | 0.0~100.0 |
| burstloss | burstloss-good, burstloss-bad, burstloss-gb, burstloss-bg | 실수(%) | 0.0~100.0 |
| blackout | blackout-duration | 정수(ms) | 100~60000 |
| blackout | blackout-gap | 정수(ms) | 1000~300000 |
| blackout | blackout-trigger | true/false | — |
| throttle | throttle-chance | 실수(%) | 0.0~100.0 |
| throttle | throttle-frame | 정수(ms) | 0~1000 |
| ood | ood-chance | 실수(%) | 0.0~100.0 |
| ood | ood-buffer | 정수 | 2~50 |
| ood | ood-delay | 정수(ms) | 10~2000 |
| duplicate | duplicate-chance | 실수(%) | 0.0~100.0 |
| duplicate | duplicate-count | 정수 | 2~50 |
| tamper | tamper-chance | 실수(%) | 0.0~100.0 |
| tamper | tamper-position | 정수 | 0=Front,1=Center,2=Back,3=Random |
| reset | reset-chance | 실수(%) | 0.0~100.0 |
| bandwidth | bandwidth-bandwidth | 정수(KB/s) | 0~99999 |

**응답**:
```json
{"status":"ok","module":"lag"}
```

#### `get_stats` — 현재 상태 조회

```json
{"cmd":"get_stats"}
```

**응답**:
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

- `affected`: 해당 모듈이 처리(지연/드롭/변조 등)한 누적 패킷 수
- `captured`: 총 캡처된 패킷 수
- `sent`: 총 전송된 패킷 수

#### `stop` — 프로그램 종료

```json
{"cmd":"stop"}
```

**응답**:
```json
{"status":"ok","message":"stopping"}
```

응답 후 최대 200ms 내에 clumsy가 종료됩니다.

#### 0.4에서 추가된 명령

기존 세 명령(`set` / `get_stats` / `stop`)의 요청·응답 형식은 그대로 유지됩니다.
아래는 REST API와 동일한 기능을 파이프에서도 쓸 수 있도록 추가된 명령입니다.

| 명령 | 설명 |
|------|------|
| `{"cmd":"filter","filter":"udp and outbound","process":"game.exe"}` | 필터 설정 후 캡처 시작 |
| `{"cmd":"stop_capture"}` | 캡처만 중지 (clumsy는 계속 실행) |
| `{"cmd":"quit"}` | clumsy 종료 (`stop`과 동일) |
| `{"cmd":"get_status"}` | 캡처 상태/필터/마지막 메시지 |
| `{"cmd":"get_modules"}` | 모듈 전체 상태 + ParamSpec |
| `{"cmd":"get_presets"}` | 필터 프리셋 목록 |
| `{"cmd":"get_profiles"}` | 프로파일 이름 목록 |
| `{"cmd":"profile","name":"mobile-4g"}` | 프로파일 적용 |
| `{"cmd":"scenario","action":"load","path":"s.json"}` | 시나리오 로드 (`action`: load/start/stop) |
| `{"cmd":"pcap","action":"start","path":"out.pcap"}` | pcap 덤프 제어 (`action`: start/stop) |

> **중요**: `stop`은 0.3과 동일하게 **프로그램 종료**를 의미합니다.
> 캡처만 멈추려면 `stop_capture`를 사용하세요.

### Python 연동 예시

```python
import socket, json

PIPE = r'\\.\pipe\clumsy'

def send_command(cmd: dict) -> dict:
    """clumsy Named Pipe에 JSON 명령을 보내고 응답을 받는다."""
    payload = json.dumps(cmd).encode()
    # Python on Windows: use win32pipe or ctypes for named pipe
    # 간단한 예시는 subprocess로 PowerShell을 통해 처리
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

# 예시: lag 켜기 + 지연 200ms 설정
resp = send_command({"cmd":"set","module":"lag","enabled":True,"lag-time":200})
print(resp)  # {"status": "ok", "module": "lag"}

# 예시: 상태 조회
stats = send_command({"cmd":"get_stats"})
print(stats)

# 예시: 종료
send_command({"cmd":"stop"})
```

### PowerShell 연동 예시

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

# 사용 예
Send-ClumsyCommand @{cmd="set"; module="lag"; enabled=$true; "lag-time"=100}
Send-ClumsyCommand @{cmd="set"; module="drop"; enabled=$true; "drop-chance"=5.0}
$stats = Send-ClumsyCommand @{cmd="get_stats"}
Send-ClumsyCommand @{cmd="stop"}
```

---

## 8. 시나리오 스크립팅

시간 순서에 따라 네트워크 조건이 자동으로 변하는 시나리오를 JSON 파일로 정의합니다. QA 팀이 동일한 조건을 반복 재현하거나, CI/CD에서 단계적 열화 테스트를 자동화할 때 유용합니다.

### 사용법

```
clumsy.exe --filter "udp and outbound" --scenario scenario.json
```

필터링이 시작되면 시나리오가 자동으로 재생됩니다. 모든 스텝이 실행되면 재생이 완료되고 마지막 설정 상태가 유지됩니다.

### 파일 형식

```json
[
  { "at": 초, "변경키": 값, ... },
  ...
]
```

- `"at"`: 필터링 시작 후 몇 초에 적용할지 (정수, 초 단위)
- `"모듈명": true/false`: 해당 모듈 활성화/비활성화 (모듈 shortName 사용)
- `"파라미터키": 값`: 모듈 파라미터 변경 (제어 API와 동일한 키 이름)

모듈 shortName 목록: `lag`, `jitter`, `drop`, `burstloss`, `blackout`, `throttle`, `ood`, `duplicate`, `tamper`, `reset`, `bandwidth`

### 트리거 종류 (0.4에서 확장)

각 스텝은 **트리거를 하나만** 가집니다.

#### 1) 시간 트리거 — `"at"`

기존과 동일합니다. 캡처 시작 후 지정한 초가 지나면 적용됩니다.

```json
{ "at": 10, "lag": true, "lag-time": 200 }
```

#### 2) 조건 트리거 — `"when"` / `"op"` / `"value"`

실제 QA 자동화에서는 "패킷이 N개 쌓이면", "버퍼가 넘치면" 같은 조건이 시간보다 유용합니다.

```json
{ "when": "captured_count", "op": ">=", "value": 10000,
  "drop": true, "drop-chance": 15.0 }
```

- `"op"`: `>=`(기본), `>`, `<=`, `<`, `==`
- `"value"`: 비교할 숫자

사용 가능한 지표(`when`):

| 지표 | 의미 |
|------|------|
| `captured_count` | 총 캡처 패킷 수 |
| `sent_count` | 총 전송 패킷 수 |
| `pps` | 초당 캡처 패킷 수 (약 1초 주기로 갱신) |
| `elapsed_sec` | 캡처 시작 후 경과 초 (실수) |
| `lag_buf` | Lag 모듈 내부 버퍼 크기 |
| `jitter_buf` | Jitter 모듈 내부 버퍼 크기 |
| `bandwidth_buf` | Bandwidth 모듈 큐 크기 |
| `affected:<모듈명>` | 해당 모듈이 처리한 누적 패킷 수 (예: `affected:drop`) |

#### 3) 반복 — `"repeat"` / `"times"`

스텝이 발동한 뒤 `repeat`초마다 다시 발동합니다. `times`로 횟수를 제한합니다
(생략하거나 `0`이면 무제한).

```json
{ "at": 30, "repeat": 30, "times": 5, "drop-chance": 12.0 }
```

시간 트리거와 조건 트리거 모두 `repeat`을 붙일 수 있습니다.
반복 스텝이 전부 소진되면 시나리오가 자동 종료됩니다.

### 예시 파일

#### 기본 단계적 열화 시나리오

```json
[
  { "at": 0,  "lag": true,  "lag-time": 50 },
  { "at": 10, "lag-time": 150, "jitter": true, "jitter-min": 20, "jitter-max": 100 },
  { "at": 20, "drop": true, "drop-chance": 5.0 },
  { "at": 30, "lag-time": 300, "drop-chance": 10.0 },
  { "at": 45, "lag": false, "jitter": false, "drop": false }
]
```

스텝 해석:
- t=0s: 지연 50ms 적용
- t=10s: 지연 150ms로 증가 + 지터 추가
- t=20s: 드롭 5% 추가
- t=30s: 지연 300ms, 드롭 10%로 악화
- t=45s: 모두 해제

#### 모바일 네트워크 열화 + 두절 시나리오

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

#### 대역폭 스로틀링 시나리오

```json
[
  { "at": 0,  "bandwidth": true, "bandwidth-bandwidth": 2000 },
  { "at": 20, "bandwidth-bandwidth": 500 },
  { "at": 40, "bandwidth-bandwidth": 100 },
  { "at": 60, "bandwidth": false }
]
```

#### 조건 기반 시나리오 (0.4)

부하가 실제로 걸린 시점을 기준으로 조건을 바꾸는 예입니다.
테스트 머신의 속도에 관계없이 동일한 "트래픽 양" 기준으로 재현됩니다.

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

`etc/scenario-example.json`에 위 패턴이 모두 들어간 예제 파일이 들어 있습니다.

### 주의사항

- `"at"` 값이 같은 스텝이 여러 개면 파일 순서대로 적용됩니다
- 조건 트리거 스텝은 파일 순서대로 매 틱 평가됩니다
- 시간 해상도는 약 200ms (메인 틱 주기)
- 한 스텝에 `at`과 `when`이 모두 있으면 `when`이 우선합니다 (중복 발동 방지)
- 최대 128개 스텝, 스텝당 32개 파라미터
- `--timeout`과 함께 사용하면 시나리오 완료 후 자동 종료 가능:
  ```
  clumsy.exe --filter "udp and outbound" --scenario test.json --timeout 70
  ```
- 대시보드에서도 시나리오를 로드/시작/중지할 수 있습니다
  (PROFILES, SCENARIO AND CAPTURE FILE 영역)

---

## 9. 프로파일 저장/불러오기

자주 쓰는 모듈 조합 설정을 이름으로 저장하고, UI 드롭다운이나 CLI에서 바로 불러올 수 있습니다.

### 파일 위치

`profiles.json` — clumsy 실행 파일과 같은 디렉토리에 자동 생성/로드됩니다.

### 파일 형식

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

각 프로파일은 모듈 활성화 플래그(`"모듈명": true`)와 파라미터 키-값 쌍으로 구성됩니다. 키 이름은 CLI 인수와 동일합니다 (예: `lag-time`, `drop-chance`).

### 웹 대시보드에서 사용

1. **불러오기**: **PROFILES** 영역의 Profile 드롭다운에서 프로파일을 고르고 **Apply**를 누릅니다.
   모듈 활성화 상태와 파라미터 값이 모두 반영되고, 모듈 목록이 즉시 갱신됩니다.
2. **저장**: "Save current as" 입력란에 이름을 넣고 **Save**를 누르면 현재 활성화된 모듈과
   파라미터 값이 `profiles.json`에 저장됩니다. 같은 이름으로 저장하면 덮어씁니다.

API로는 `POST /api/profiles/{name}/apply`, `POST /api/profiles` (body: `{"name":"..."}`)입니다.

### CLI에서 사용

```
clumsy.exe --filter "udp and outbound" --profile mobile-4g
```

`--profile` 인수로 프로파일을 적용하면, 해당 프로파일에 정의된 모듈이 자동으로 활성화되고 파라미터가 설정됩니다. `--filter`와 함께 사용해야 하며, 다른 모듈 인수와 조합할 수도 있습니다.

### 주의사항

- `profiles.json`이 없으면 프로파일 드롭다운은 빈 상태로 표시됩니다 (오류 없음)
- 0.4부터 프로파일에는 방향 플래그(`lag-inbound` 등)도 함께 저장됩니다
- 최대 32개 프로파일까지 저장 가능
- 프로파일 적용 시 명시적으로 포함되지 않은 모듈의 상태는 변경되지 않습니다
- JSON 파일을 직접 편집하여 프로파일을 추가/수정할 수도 있습니다

---

## 10. pcap 패킷 덤프

통계 로그(`--stats-log`)는 집계 수치만 남기므로, 실제 패킷 내용을 봐야 할 때는
표준 libpcap 파일로 덤프해 Wireshark 등에서 분석할 수 있습니다.

### 사용법

```
clumsy.exe --filter "udp and outbound" --pcap-out capture.pcap
```

대시보드에서는 **PROFILES, SCENARIO AND CAPTURE FILE** 영역의 pcap 입력란에
파일 경로를 넣고 **Start dump** / **Stop dump**로 제어합니다.
API는 `POST /api/pcap/start` (body: `{"path":"out.pcap","maxPackets":0,"maxBytes":0}`)와
`POST /api/pcap/stop`입니다.

### 기록 시점 (`--pcap-stage`)

| 값 | 기록 시점 | 용도 |
|----|----------|------|
| `post` (기본) | 모든 모듈을 거친 뒤, 실제로 네트워크에 나가기 직전 | 상대방이 실제로 받는 것을 보고 싶을 때 |
| `pre` | 캡처 직후, 어떤 모듈도 손대기 전 | 원본 트래픽을 보존하고 싶을 때 |
| `both` | 두 시점 모두 | 변조 전후를 비교하고 싶을 때 (파일이 커집니다) |

`both`로 기록하면 같은 패킷이 두 번 들어가므로, Wireshark에서 시간순으로 보면
"원본 → 변조본" 쌍으로 나타납니다.

### 크기 제한

무제한 덤프는 디스크를 채울 수 있으므로 상한을 지정하는 것을 권장합니다.

```
clumsy.exe --filter "udp" --pcap-out capture.pcap ^
           --pcap-max-packets 100000 --pcap-max-bytes 104857600
```

상한에 도달하면 파일이 **정상적으로 닫히고** 콘솔에 메시지가 출력됩니다.
캡처 자체는 계속 진행됩니다.

### 파일 형식

- 링크 타입은 `LINKTYPE_RAW`(101)입니다. WinDivert는 이더넷 프레임 없이
  IPv4/IPv6 데이터그램만 넘겨주므로 RAW가 정확한 표현입니다.
- Wireshark에서 열면 자동으로 인식됩니다. 이더넷 헤더가 없다고 나오는 것이 정상입니다.
- 스냅 길이는 65535바이트입니다.

---

## 11. HTML 세션 리포트

테스트가 끝난 뒤 "어떤 조건으로 얼마나 테스트했는지"를 팀에 공유할 수 있는
단일 HTML 파일을 생성합니다. 외부 리소스를 전혀 참조하지 않으므로
이슈 트래커에 첨부하거나 이메일로 보내도 그대로 열립니다.

### 생성 방법

**CLI** — 캡처가 끝나는 시점(Stop 또는 `--timeout` 만료)에 자동 생성:

```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 120 ^
           --report-out report.html --timeout 60
```

**웹 대시보드** — 헤더 오른쪽의 **Download report** 링크
(`GET /api/report`). 캡처 진행 중에도 현재 시점까지의 리포트를 받을 수 있습니다.

### 리포트 내용

| 섹션 | 내용 |
|------|------|
| 헤더 | clumsy 버전, 세션 시작 시각, 지속 시간 |
| Capture | 적용된 필터 표현식(프로세스 필터 포함) |
| 카드 | 총 캡처/전송 패킷 수, 평균 초당 패킷 |
| Throughput | 초당 패킷 수 그래프 (인라인 SVG, 차트 라이브러리 없음) |
| Modules | 모듈별 종료 시점 활성화 여부, 처리 패킷 수, 파라미터 전체 |
| Timeline | 세션 시작/종료 및 발동한 시나리오 스텝 기록 |
| Packet capture | pcap을 함께 사용한 경우 파일 경로와 패킷 수 |

시나리오를 함께 쓰면 각 스텝이 **실제로 몇 초에 발동했는지**가 타임라인에 남으므로,
조건 트리거가 언제 걸렸는지 사후 확인할 수 있습니다.

### 제한 사항

- 그래프 샘플은 1초 간격으로 최대 600개(10분)까지 기록됩니다.
  더 긴 세션은 그래프가 앞 10분까지만 그려지고, 총계는 정확히 유지됩니다.
- 타임라인 이벤트는 최대 256개입니다.

---

## 12. 원격 제어 (분산 QA 환경)

CI 러너가 원격 테스트 클라이언트의 clumsy를 조종하는 구성입니다.

### 테스트 머신에서

```
clumsy.exe --web-bind 0.0.0.0 --web-port 8080 --web-token ci-secret-token
```

- `--web-bind`가 로컬호스트가 아니면 **토큰 인증이 강제**됩니다.
- `--web-token`을 생략하면 토큰이 자동 생성되어 콘솔에 출력됩니다.
  CI에서는 값을 고정해야 스크립트에서 재사용할 수 있습니다.
- 외부 바인딩 시 콘솔에 보안 경고가 출력됩니다.

### CI 러너에서

```bash
CLUMSY=http://10.0.0.5:8080
TOKEN=ci-secret-token

# 1. 살아 있는지 확인 (인증 불필요)
curl -f $CLUMSY/api/health || exit 1

# 2. 조건 설정 후 캡처 시작
curl -sf -X POST $CLUMSY/api/filter -H "X-Clumsy-Token: $TOKEN" \
     -H "Content-Type: application/json" \
     -d '{"filter":"udp and outbound"}'
curl -sf -X POST $CLUMSY/api/modules/lag -H "X-Clumsy-Token: $TOKEN" \
     -H "Content-Type: application/json" \
     -d '{"enabled":true,"lag-time":150}'

# 3. 테스트 실행 ...

# 4. 정리 및 산출물 수집
curl -sf -X POST $CLUMSY/api/stop -H "X-Clumsy-Token: $TOKEN"
curl -sf -o report.html $CLUMSY/api/report -H "X-Clumsy-Token: $TOKEN"
```

### 보안 주의사항

clumsy는 관리자 권한으로 실행되며 네트워크 트래픽을 조작할 수 있습니다.
원격 바인딩은 **신뢰된 격리 테스트망에서만** 사용하세요.

- 토큰은 평문 HTTP로 전달됩니다 (TLS 미지원). 인터넷에 노출하지 마세요.
- 방화벽에서 해당 포트를 CI 러너 IP로만 제한하는 것을 권장합니다.
- `/api/health`만 인증 없이 접근 가능하며, 버전과 캡처 여부만 노출합니다.

---

## 13. 커스텀 모듈 플러그인 (선택 기능)

재컴파일 없이 실험용 모듈을 DLL로 로드하는 기능입니다.
**기본적으로 비활성화**되어 있으며 `--enable-plugins <디렉토리>`로만 켤 수 있습니다.

### 보안 경고

플러그인 DLL은 clumsy와 **동일한 권한(관리자)** 으로 실행됩니다.
직접 빌드했거나 완전히 신뢰하는 DLL만 로드하세요.
활성화하면 콘솔에 경고와 함께 로드하는 DLL 파일명이 모두 출력됩니다.

### 플러그인 작성 방법

DLL은 함수 하나를 export 하면 됩니다:

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

### 사용법

```
clumsy.exe --enable-plugins .\plugins --filter "udp and outbound"
```

지정한 디렉토리의 모든 `*.dll`을 스캔하여 `clumsyGetModule`을 export 하는 것만 로드합니다.

### 현재 제한 사항

- 플러그인은 **기존 내장 모듈을 대체**하는 방식으로만 등록됩니다.
  `shortName`이 내장 모듈 중 하나와 일치해야 하며, 일치하지 않으면 건너뜁니다.
  (모듈 테이블 크기 `MODULE_CNT`가 컴파일 타임 상수이기 때문입니다.
  테이블 확장은 향후 과제입니다.)
- 최대 8개까지 로드됩니다.
- 로드된 DLL은 프로세스 종료 시까지 언로드되지 않습니다
  (`modules[]`가 DLL 내부 메모리를 가리키므로 use-after-free 방지 목적).

---

---

# 2부 — 활용 케이스

---

## 케이스 1. 기본 지연 테스트 — "서버가 멀다"

**상황**: 게임 서버가 해외에 있어서 항상 150ms 지연이 발생하는 상황을 테스트하고 싶다.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Lag | ON, Delay = 150ms |

**대시보드 순서**:
1. Preset 드롭다운에서 `all sending packets` 선택 (또는 직접 `udp and outbound` 입력)
2. Start 클릭
3. Lag 체크박스 ON
4. 펼쳐진 폼의 Delay (ms)에 `150` 입력

**확인 방법**: 게임을 실행하고 핑(ms)이 150ms 전후로 고정되는지 확인합니다.

**CLI**:
```
clumsy.exe --filter "udp and outbound" --lag on --lag-time 150
```

---

## 케이스 2. 불안정한 인터넷 환경 — "가끔 끊기는 Wi-Fi"

**상황**: 신호가 약한 Wi-Fi처럼 지연이 들쑥날쑥하고 가끔 패킷이 사라지는 환경.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Jitter | ON, Min = 30ms, Max = 200ms |
| Drop | ON, Chance = 3.0% |

**이 조합이 재현하는 것**:
- 어떤 패킷은 30ms, 어떤 패킷은 200ms 지연 → 불규칙한 응답 시간
- 100개 중 약 3개 패킷이 사라짐 → 주기적인 데이터 손실

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --jitter on --jitter-min 30 --jitter-max 200 ^
           --drop on --drop-chance 3.0
```

---

## 케이스 3. 모바일 4G 환경 재현

**상황**: 스마트폰 4G 환경에서 게임을 플레이하는 유저를 시뮬레이션.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
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

## 케이스 4. 모바일 3G / 열악한 환경 재현

**상황**: 지하철이나 엘리베이터처럼 신호가 매우 약한 3G 환경.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
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

## 케이스 5. 위성 통신 환경 재현

**상황**: 위성 인터넷처럼 지연이 극단적으로 높은 환경. 지연은 일정하지만 매우 큼.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
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

**참고**: 위성 통신에서 500ms+ RTT는 실시간 게임을 거의 불가능하게 만듭니다. 게임이 이 환경에서 어떻게 동작하는지(타임아웃, 연결 끊김 처리 등) 확인할 수 있습니다.

---

## 케이스 6. 연속 패킷 손실 테스트 — "전파 간섭 / 무선 혼잡"

**상황**: 모바일 게임에서 전철 터널 진입처럼 짧은 구간 동안 연속으로 패킷이 사라지는 상황. Drop만 쓰면 손실이 흩어지지만, 실제 무선 환경은 손실이 연속으로 몰림.

**기본값으로 그냥 켜기**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Burst Loss | ON (기본값 그대로) |

기본값 해석:
- 평소(Good): 패킷 50개 중 1개 손실(2%)
- 버스트(Bad): 패킷 10개 중 8개 손실(80%), 평균 5패킷 지속
- 약 20% 구간이 버스트 → 전체 약 17% 손실

**파라미터 튜닝 예시**:

| 목적 | Good(%) | Bad(%) | G>B(%) | B>G(%) |
|------|---------|--------|--------|--------|
| 짧고 잦은 버스트 | 1.0 | 70.0 | 10.0 | 40.0 |
| 길고 드문 버스트 | 0.5 | 90.0 | 2.0 | 10.0 |
| 약한 버스트      | 1.0 | 30.0 | 5.0  | 20.0 |

**CLI** (짧고 잦은 버스트):
```
clumsy.exe --filter "udp and outbound" ^
           --burstloss on --burstloss-good 1.0 --burstloss-bad 70.0 ^
           --burstloss-gb 10.0 --burstloss-bg 40.0
```

**Drop과 비교 실험**: 동일한 전체 손실률(예: 10%)로 Drop과 Burst Loss를 각각 적용해보면 게임 반응이 다릅니다.
- Drop 10%: 손실이 고르게 분포 → 서버가 잘 대처함
- Burst Loss: 손실이 몰림 → 연속 입력 손실로 "순간 멈춤" 발생 가능

---

## 케이스 7. 랙 보상(Lag Compensation) 알고리즘 테스트

**상황**: 서버 측 랙 보상 로직이 불규칙한 지연 환경에서 제대로 동작하는지 검증.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)` |
| Jitter | ON, Min = 50ms, Max = 250ms |
| Out of Order | ON, Chance = 15.0%, Buf = 8, Delay = 300ms |

**이 조합이 테스트하는 것**:
- 지연이 50~250ms로 변동 → 클라이언트 예측값과 서버 확인값의 오차 발생
- 최대 8개 패킷을 모아 셔플 방출 → 서버가 과거 패킷을 나중에 받는 상황
- Delay 300ms → 버퍼가 덜 찰 때도 0.3초 이상 묵히지 않음

**CLI**:
```
clumsy.exe --filter "udp and (udp.DstPort==7777 or udp.SrcPort==7777)" ^
           --jitter on --jitter-min 50 --jitter-max 250 ^
           --ood on --ood-chance 15.0 --ood-buffer 8 --ood-delay 300
```

---

## 케이스 8. 재연결 로직 테스트

**상황**: 클라이언트의 재접속 처리 코드가 올바르게 동작하는지 테스트.

---

**방법 A — Blackout으로 주기적 연결 두절** ← 권장

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Blackout | ON, Dur = 3000ms, Gap = 20000ms |

20초마다 3초간 완전히 끊깁니다. 이 동안 클라이언트가:
1. 연결 두절을 감지하는지 (타임아웃 구현 여부)
2. 재연결을 시도하는지 (reconnect 로직)
3. 재연결 후 게임 상태가 정상 복구되는지 (세션 복구)

를 순서대로 확인할 수 있습니다.

**재연결 타임아웃 테스트**: 클라이언트 타임아웃 설정(예: 5초)보다 두절 시간을 짧게/길게 바꿔 경계값 동작을 확인합니다.

```
Dur=4000, Gap=30000  → 타임아웃 직전까지만 끊기는 상황
Dur=6000, Gap=30000  → 타임아웃을 초과해서 끊기는 상황
```

**Trigger 버튼 활용**: 재연결 테스트를 원하는 시점(예: 특정 게임 이벤트 직후)에 수동으로 즉시 끊을 수 있습니다.

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --blackout on --blackout-duration 3000 --blackout-gap 20000
```

---

**방법 B — TCP RST로 연결 강제 종료** (TCP 게임 서버인 경우)

| 항목 | 값 |
|------|-----|
| 필터 | `tcp and (tcp.DstPort == 9000 or tcp.SrcPort == 9000)` |
| Set TCP RST | ON |
| RST next packet 버튼 | 원하는 시점에 클릭 |

버튼을 누르는 순간 TCP 연결이 즉시 강제 종료됩니다. 클라이언트가 `connection reset` 오류를 처리하고 재연결하는지 확인합니다.

---

**방법 비교**:

| | Blackout | TCP RST |
|--|---------|---------|
| 프로토콜 | UDP / TCP 모두 | TCP만 |
| 두절 방식 | 조용히 패킷 드롭 (timeout 유도) | RST 플래그로 즉시 종료 알림 |
| 재현 환경 | 터널, 음영 지역 진입 | 서버 프로세스 강제 종료 |
| 주기 반복 | 자동 반복 가능 | 매번 수동 클릭 필요 |

---

## 케이스 9. UDP 중복 패킷 처리 테스트

**상황**: 같은 패킷이 두 번 오는 경우(네트워크 장비의 재전송)에 서버/클라이언트가 올바르게 처리하는지 테스트.

**설정**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Duplicate | ON, Count = 2, Chance = 20.0% |

패킷 5개 중 1개가 복사본과 함께 전송됩니다. 서버가 중복 패킷을 무시하거나 한 번만 처리하는지 확인합니다.

**CLI**:
```
clumsy.exe --filter "udp and outbound" ^
           --duplicate on --duplicate-count 2 --duplicate-chance 20.0
```

---

## 케이스 10. 로컬 서버 개발 중 테스트 (localhost)

**상황**: 같은 PC에서 클라이언트와 서버를 모두 실행하면서 네트워크 조건을 테스트.

**필터 주의사항**: loopback은 반드시 `outbound` 조건 필요.

```
udp and outbound and loopback
```

**설정 예시**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound and loopback` |
| Lag | ON, Delay = 80ms |
| Drop | ON, Chance = 2.0% |

**대시보드 순서**:
1. Preset 드롭다운에서 `localhost ipv4 udp` 선택 → 자동으로 `udp and outbound and loopback` 입력됨
2. Start 클릭
3. Lag, Drop 체크박스를 켜고 값 설정

---

## 케이스 11. CI/CD 자동화 테스트 시나리오

**상황**: 빌드 파이프라인에서 네트워크 열화 조건을 단계적으로 바꾸며 자동화 테스트를 실행.

### 방법 A — CLI + `--timeout` (간단, 조건 변경 불필요)

```powershell
# 1단계: 정상 환경 테스트 (30초)
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --lag on --lag-time 20 --timeout 30' `
    -Verb RunAs -Wait

# 2단계: 열악한 환경 테스트 (30초)
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --lag on --lag-time 200 --jitter on --jitter-min 50 --jitter-max 300 --drop on --drop-chance 5.0 --timeout 30' `
    -Verb RunAs -Wait
```

### 방법 B — Named Pipe API (단일 프로세스, 실시간 조건 전환)

clumsy를 한 번 실행해두고 Named Pipe로 조건을 동적으로 바꾸는 방식. 재시작 오버헤드 없이 시나리오 전환이 가능합니다.

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

# clumsy를 먼저 관리자 권한으로 실행해둔 상태에서:

# --- 단계 1: 정상 지연 (20ms) ---
Send-ClumsyCommand @{cmd="set";module="lag";enabled=$true;"lag-time"=20}
Start-Sleep 30
Run-YourTests

# --- 단계 2: 열악한 환경으로 전환 ---
Send-ClumsyCommand @{cmd="set";module="lag";"lag-time"=200}
Send-ClumsyCommand @{cmd="set";module="jitter";enabled=$true;"jitter-min"=50;"jitter-max"=300}
Send-ClumsyCommand @{cmd="set";module="drop";enabled=$true;"drop-chance"=5.0}
Start-Sleep 30
Run-YourTests

# --- 단계 3: 대역폭 제한 추가 ---
Send-ClumsyCommand @{cmd="set";module="bandwidth";enabled=$true;"bandwidth-bandwidth"=200}
Start-Sleep 30
Run-YourTests

# --- 완료 후 clumsy 종료 ---
Send-ClumsyCommand @{cmd="stop"}
```

**방법 B의 장점**: 조건 전환이 즉각적이며 테스트 중 상태를 `get_stats`로 확인 가능. 여러 단계에 걸친 복잡한 시나리오에 적합합니다.

### 방법 C — 시나리오 파일 (가장 간단, 완전 자동)

시나리오 JSON 파일 하나로 전체 흐름을 정의합니다.

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
# 90초 동안 시나리오 자동 재생 후 종료
Start-Process "clumsy.exe" -ArgumentList `
    '--filter "udp and outbound" --scenario ci_scenario.json --timeout 90' `
    -Verb RunAs -Wait
```

**방법 C의 장점**: 시나리오 파일만 버전 관리하면 됩니다. QA 팀이 동일한 파일로 언제든지 동일한 조건을 재현할 수 있습니다.

---

## 케이스 12. 패킷 변조 — 게임 헤더·시퀀스 번호 손상 테스트

**상황**: 게임 패킷의 헤더나 시퀀스 번호가 손상됐을 때 서버/클라이언트가 올바르게 에러 처리하는지 테스트.

**설정 A — 시퀀스 번호 손상 (Front)**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Tamper | ON, Chance = 5.0%, Position = Front, Redo Checksum = ON |

페이로드 앞쪽 1/4을 손상시킵니다. 대부분의 게임 UDP 패킷은 헤더(패킷 타입, 시퀀스 번호, 플레이어 ID 등)가 앞쪽에 배치되므로, 서버가 잘못된 시퀀스 번호나 알 수 없는 패킷 타입을 받았을 때의 처리를 검증할 수 있습니다.

**설정 B — 예측 불가 손상 (Random)**:

| 항목 | 값 |
|------|-----|
| 필터 | `udp and outbound` |
| Tamper | ON, Chance = 10.0%, Position = Random, Redo Checksum = ON |

매 패킷마다 다른 위치를 손상시켜 광범위한 에러 케이스를 커버합니다. 퍼징(fuzzing)과 비슷한 효과로, 예상치 못한 파싱 오류나 크래시를 발견하는 데 유용합니다.

**CLI (Front 포지션)**:
```
clumsy.exe --filter "udp and outbound" ^
           --tamper on --tamper-chance 5.0 --tamper-position 1 --tamper-checksum on
```

**CLI (Random 포지션)**:
```
clumsy.exe --filter "udp and outbound" ^
           --tamper on --tamper-chance 10.0 --tamper-position 4 --tamper-checksum on
```

> **Redo Checksum 주의**: OFF로 설정하면 IP/UDP 체크섬도 틀어져서 OS가 패킷을 드롭할 수 있습니다. 애플리케이션 레벨 에러 처리를 테스트할 때는 ON 유지를 권장합니다.

---

## 빠른 참조 — 환경별 설정 요약

| 환경 | Lag | Jitter | Drop | Bandwidth | Blackout |
|------|-----|--------|------|-----------|---------|
| 이상적인 로컬 | — | — | — | — | — |
| 일반 가정 광랜 | 5ms | 2~15ms | 0.1% | — | — |
| 모바일 4G | 60ms | 10~80ms | 2% | 5000 KB/s | — |
| 모바일 3G | 150ms | 50~300ms | 8% | 500 KB/s | Dur=2s Gap=60s |
| 혼잡한 공용 Wi-Fi | 80ms | 30~200ms | 3% | 2000 KB/s | — |
| 지하철 / 음영 구간 | 100ms | 20~150ms | 5% | — | Dur=3s Gap=20s |
| 위성 통신 | 500ms | 20~80ms | 1% | — | — |
| 해외 서버 (미국) | 150ms | 10~40ms | 0.5% | — | — |
| 해외 서버 (유럽) | 200ms | 10~50ms | 0.5% | — | — |
| 극한 스트레스 | 300ms | 100~500ms | 15% | 200 KB/s | Dur=5s Gap=10s |

> **Bandwidth 동작 참고**: Bandwidth는 한도 초과 패킷을 드롭하지 않고 큐에 보관했다가 지연 전송합니다. 값이 낮을수록 패킷이 오래 대기하여 지연이 증가합니다. `Bandwidth + Lag` 조합 시 총 지연 = Lag 지연 + Bandwidth 큐 대기 시간이 됩니다.
