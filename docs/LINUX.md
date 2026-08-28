# clumsy 리눅스 가이드 (Phase 4.6)

> Windows 사용법은 [manual.md](../manual.md)를 보세요. 이 문서는 리눅스에서 달라지는 부분만 다룹니다.
> 모듈, CLI 인수, 웹 대시보드, REST API는 **양쪽이 동일**합니다.

---

## 1. 요구 사항

| 항목 | 내용 |
|------|------|
| 커널 | `CONFIG_NETFILTER_NETLINK_QUEUE` 지원 (거의 모든 배포판이 모듈로 제공) |
| 컴파일러 | **g++-16** 이상 (C++23) |
| 라이브러리 | `libnetfilter_queue`, `libmnl`, `libnfnetlink` |
| 방화벽 도구 | `iptables` 또는 `nftables` |
| 권한 | `CAP_NET_ADMIN` (+ duplicate 모듈을 쓸 경우 `CAP_NET_RAW`) |

### 배포판별 패키지

**Debian / Ubuntu**
```bash
sudo apt install g++-16 make libnetfilter-queue-dev libmnl-dev libnfnetlink-dev iptables
```
> Ubuntu 24.04에는 g++-16이 기본 저장소에 없을 수 있습니다.
> `sudo add-apt-repository ppa:ubuntu-toolchain-r/test` 후 설치하세요.

**Fedora / RHEL**
```bash
sudo dnf install gcc-c++ make libnetfilter_queue-devel libmnl-devel iptables
```

**Arch**
```bash
sudo pacman -S gcc make libnetfilter_queue libmnl iptables
```

---

## 2. 설치 (패키지)

미리 빌드된 `.deb`이 있으면 그대로 설치하면 됩니다:

```bash
sudo dpkg -i clumsy_0.4_amd64.deb
sudo apt-get install -f          # 의존성이 빠졌을 때만
```

설치 후처리가 `setcap cap_net_admin,cap_net_raw+ep`를 자동 적용하므로
**sudo 없이 실행할 수 있습니다.**

직접 패키지를 만들려면:

```bash
make package-deb     # → bin/linux/clumsy_0.4_amd64.deb
make package-rpm     # → obj_linux/rpmbuild/RPMS/x86_64/clumsy-0.4-1.x86_64.rpm
```

> `.rpm` 스펙의 `BuildRequires`는 Fedora 패키지 이름을 씁니다. Debian/Ubuntu에서 스펙만
> 검증하려면 `make package-rpm RPM_NODEPS=1`로 의존성 검사를 건너뛰세요
> (라이브러리는 설치되어 있어도 rpm DB가 Fedora 이름을 모르기 때문입니다).

패키지 없이 설치하려면 `sudo make install PREFIX=/usr/local`도 가능합니다.

> `clumsy`는 `config.json`과 `web/`을 **실행 파일과 같은 디렉토리**에서 찾습니다.
> 패키지는 실제 파일을 `/usr/share/clumsy/`에 두고 `/usr/bin/`에서 심볼릭 링크를 겁니다.

---

## 3. 빌드

```bash
make                # 릴리스 빌드 → bin/linux/clumsy
make DEBUG=1        # 디버그 빌드 (트레이스 로그 기본 활성화)
make test           # 패킷 헬퍼 계약 테스트 실행
make clean
```

`make install-deps`로 Debian 계열 의존성을 한 번에 설치할 수 있습니다.

빌드 결과물 옆에 `config.json`과 `web/index.html`이 자동 복사됩니다.
clumsy는 이 파일들을 **실행 파일과 같은 디렉토리에서** 찾으므로, 옮길 때는 함께 옮기세요.

> 리눅스 빌드 정의는 이 `Makefile` 하나뿐입니다. Windows는 `msvc/clumsy.vcxproj`를 씁니다.

---

## 4. 권한

두 가지 방법이 있습니다.

**A. sudo로 실행 (간단)**
```bash
sudo ./clumsy --filter "udp and outbound"
```

**B. 바이너리에 capability 부여 (권장, 한 번만)**
```bash
sudo setcap cap_net_admin,cap_net_raw+ep ./clumsy
./clumsy --filter "udp and outbound"      # 이제 sudo 불필요
```

clumsy는 `geteuid()==0`이 아니라 `/proc/self/status`의 `CapEff`를 확인하므로,
B 방식이나 컨테이너에서 부여한 capability도 정상 인식합니다.

권한이 없으면 clumsy는 **종료하지 않고** 웹 대시보드만 띄운 뒤, 캡처 시작 시 명확한 오류를 반환합니다.

---

## 5. Windows와 가장 크게 다른 점 — 필터가 2단계

Windows에서는 필터 표현식을 WinDivert 드라이버가 직접 해석해 "어떤 패킷을 가로챌지" 결정합니다.
리눅스에는 그런 계층이 없어서 역할이 **둘로 나뉩니다**:

```
   [1] iptables 규칙          어떤 트래픽을 NFQUEUE로 보낼지 결정 (커널)
                ↓
   [2] clumsy 필터 표현식     큐에 들어온 것 중 무엇을 실제로 열화시킬지 결정 (clumsy)
```

`[2]`에서 매치되지 않은 패킷은 **손대지 않고 그대로 통과**시킵니다.
덕분에 `config.json` 프리셋과 시나리오 파일을 Windows와 리눅스에서 그대로 공유할 수 있습니다.

### 기본 사용 절차

```bash
# 1) clumsy 실행 (아직 아무 트래픽도 오지 않음)
sudo ./clumsy --filter "udp and outbound and udp.DstPort == 9999" \
              --lag on --lag-time 100 --queue-num 0

# 2) 다른 터미널에서, 대상 트래픽을 큐로 보내는 규칙 추가
sudo iptables -I OUTPUT -m mark --mark 0xC1 -j ACCEPT          # ← 아래 6절 참고
sudo iptables -A OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num 0

# 3) 테스트 수행 ...

# 4) 반드시 규칙 제거
sudo iptables -D OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num 0
sudo iptables -D OUTPUT -m mark --mark 0xC1 -j ACCEPT
```

> ⚠️ **NFQUEUE 규칙을 걸어둔 채 clumsy가 죽으면 해당 트래픽이 전부 드롭됩니다.**
> 운영 중인 머신에서 테스트할 때는 규칙 끝에 `--queue-bypass`를 붙이는 것을 고려하세요
> (userspace 프로그램이 없으면 그냥 통과시킵니다):
> ```bash
> sudo iptables -A OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num 0 --queue-bypass
> ```

### `--auto-iptables` — 규칙을 clumsy가 직접 관리 (권장)

규칙을 손으로 넣고 빼는 대신 clumsy가 필터 표현식에서 규칙을 **도출해 자동으로
설치·제거**하게 할 수 있습니다:

```bash
sudo ./clumsy --auto-iptables on \
              --filter "udp and outbound and udp.DstPort == 9999" \
              --lag on --lag-time 100
```

위 명령은 다음 두 규칙을 자동으로 넣고, 종료 시(정상 종료·Ctrl+C 모두) 정확히 같은
규칙을 제거합니다:

```
-A OUTPUT -m mark --mark 0xc1 -j ACCEPT
-A OUTPUT -p udp -m udp --dport 9999 -j NFQUEUE --queue-num 0 --queue-bypass
```

**안전 설계**

- 도출된 규칙은 필터 표현식의 **상위집합**입니다. 넓게 잡는 것은 무해하지만(clumsy가
  다시 필터링해서 비매치는 그대로 통과) 좁게 잡으면 대상 트래픽을 조용히 놓치기 때문입니다.
  iptables로 표현할 수 없는 항목이 있으면 규칙을 넓히고 그 사실을 콘솔에 알립니다.
- 설치할 때 쓴 문자열을 **그대로** `-D`에 넘겨 제거하므로 설치/제거가 어긋날 수 없습니다.
- 이 프로세스가 실제로 설치한 규칙만, 역순으로 제거합니다.
- 모든 규칙에 `--queue-bypass`가 붙습니다. 만에 하나 정리에 실패해도(SIGKILL, 전원 차단)
  clumsy가 없으면 트래픽은 정상 통과합니다. **손으로 넣은 규칙보다 오히려 안전합니다.**
- 설치 도중 실패하면 그때까지 넣은 규칙을 롤백합니다.

제거에 실패한 규칙이 있으면 콘솔에 어떤 규칙인지 그대로 출력하므로 수동 정리가 쉽습니다.

### 지원하는 필터 표현식

WinDivert 문법의 부분집합입니다. 인식하지 못하는 항목은 **시작 시점에 오류로 보고**되므로,
Windows와 다르게 동작하는 필터가 조용히 잘못된 트래픽을 건드리는 일은 없습니다.

| 분류 | 지원 |
|------|------|
| 논리 연산 | `and` `or` `not`, `&&` `\|\|` `!`, 괄호, `true` / `false` |
| 방향 | `inbound`, `outbound`, `loopback` |
| 프로토콜 | `ip`, `ipv6`, `tcp`, `udp`, `icmp`, `icmpv6` |
| 필드 | `ip.SrcAddr`, `ip.DstAddr` (점 표기 IPv4), `ip.Protocol`,<br>`tcp.SrcPort`, `tcp.DstPort`, `udp.SrcPort`, `udp.DstPort` |
| 비교 | `==` `!=` `>` `<` `>=` `<=` (`=`는 `==`와 동일) |

**미지원**: IPv6 주소 리터럴, 페이로드 매칭, 패킷/플로우 카운터.

예:
```
udp and outbound
tcp and (tcp.DstPort == 7777 or tcp.SrcPort == 7777)
udp and not loopback and ip.DstAddr == 198.51.100.1
```

---

## 6. 패킷 주입과 fwmark (중요)

리눅스에서 **clumsy가 스스로 만들어 내보내는 패킷**은 raw 소켓으로 주입됩니다.
duplicate 모듈의 복제본과 pcap 재생(`--replay-in`)이 여기에 해당합니다.
그런데 주입된 패킷은 OUTPUT 체인을 다시 통과하므로, 아무 조치가 없으면
**자기가 만든 패킷이 다시 큐에 들어가 무한 증폭**됩니다.
(실측: 10개 전송 → 246,106개 수신)

그래서 clumsy는 주입 소켓에 fwmark(기본 `0xC1`)를 찍습니다. 이 마크를 NFQUEUE 규칙보다
**먼저** ACCEPT 시켜야 합니다:

```bash
sudo iptables -I OUTPUT -m mark --mark 0xC1 -j ACCEPT
```

마크 값이 기존 방화벽 정책과 충돌하면 `--inject-mark <n>`으로 바꾸세요.

안전장치로, 초당 5000패킷을 넘겨 주입되면 clumsy가 **주입을 자동 중단하고** 누락된 규칙을
알려줍니다. 규칙을 깜빡해도 패킷 폭풍 대신 명확한 진단 메시지가 나옵니다.

pcap 재생은 캡처와 독립적으로 동작해야 하므로 **자체 raw 소켓 쌍**을 따로 엽니다
(캡처용 소켓은 `divertStart()`~`divertStop()` 동안만 존재하기 때문입니다).
같은 fwmark를 사용하므로 위 규칙 하나로 둘 다 커버됩니다.

### duplicate 모듈의 리눅스 제약

| 상황 | 동작 |
|------|------|
| outbound IPv4 복제 | 정상 동작 |
| outbound IPv6 복제 | 정상 동작 |
| inbound 복제 | **복제본 드롭** |

**inbound 복제가 불가능한 이유**: raw 소켓은 트래픽을 *내보내는* 것만 가능합니다.
이미 도착한 패킷의 복사본을 로컬 수신 경로에 다시 밀어 넣으려면 TUN 디바이스나
ifb 리다이렉트 같은 전혀 다른 장치가 필요합니다. Windows의 WinDivert는 커널 드라이버라
양방향 주입이 가능하지만, 이는 NFQUEUE 모델에서는 얻을 수 없는 능력입니다.

원본 패킷 자체는 어느 경우에도 정상 처리됩니다. 복제본만 생성되지 않습니다.

### pcap 재생의 리눅스 제약

| 상황 | 동작 |
|------|------|
| outbound IPv4 / IPv6 재생 | 정상 동작 |
| inbound 재생 | **불가** — 시작 시 거부 |
| 필요 권한 | `CAP_NET_RAW` (`CAP_NET_ADMIN`과 함께 부여됨) |

이유는 duplicate와 동일합니다. 실측으로는 캡처한 25패킷을 그대로 재생해
25패킷이 수신 측에 도달하는 것을 확인했습니다(`tests/linux/behaviour_test.sh`).

```bash
# 캡처 → 재생 왕복
sudo ./clumsy --filter "udp and outbound" --pcap-out cap.pcap --timeout 30
sudo ./clumsy --replay-in cap.pcap --replay-speed 2.0
```

> **기록된 주소로 실제 패킷이 나갑니다.** 다른 네트워크에서 뜬 캡처를 재생하면
> 그 주소로 트래픽이 발생합니다. 의미가 통하는 환경에서만 재생하세요.

---

## 7. Windows와 다른 점 요약

| 항목 | Windows | Linux |
|------|---------|-------|
| 캡처 백엔드 | WinDivert 커널 드라이버 | NFQUEUE (`libnetfilter_queue`) |
| 대상 선택 | 필터 표현식만 | iptables 규칙 + 필터 표현식 (`--auto-iptables`로 자동화 가능) |
| 권한 | 관리자(UAC) | `CAP_NET_ADMIN` |
| 권한 상승 | `--elevate on`으로 UAC 재실행 | 불가 — sudo/setcap 안내만 출력 |
| Named Pipe API | 지원 (`\\.\pipe\clumsy`) | Unix 도메인 소켓 (`/run/clumsy.sock`) |
| 웹 대시보드 / REST / SSE | 지원 | 지원 (동일) |
| `--process` 필터 | TCP/UDP 포트 테이블 | `/proc/<pid>/fd` + `/proc/net/*` |
| duplicate 모듈 | 제약 없음 | outbound만 (IPv4/IPv6), 6절 참고 |
| pcap 재생 | send-only WinDivert 핸들, Impostor 플래그 | fwmark raw 소켓, outbound만 |
| pcap / 리포트 / 시나리오 / 프로파일 | 지원 | 지원 (동일) |
| `/metrics` (Prometheus) | 지원 | 지원 (동일) |
| 지연 히스토그램 p50/p95/p99 | 지원 | 지원 (동일) |

### 제어 소켓 (Named Pipe 대응)

리눅스에서는 Named Pipe 대신 **Unix 도메인 소켓**이 같은 자리를 채웁니다.
**프로토콜은 완전히 동일**합니다 — 양쪽 모두 `controlDispatchJson()`을 그대로 호출하므로
JSON 요청/응답이 바이트 단위로 같습니다. 기존 파이프 자동화는 연결 부분만 바꾸면 됩니다.

- 경로: `/run/clumsy.sock` (권한이 없으면 `/tmp/clumsy.sock`로 폴백)
- 퍼미션 0666 — 권한 없는 자동화 스크립트도 제어 가능 (Named Pipe의 기본 접근성과 동일)
- 종료 시 소켓 파일 자동 삭제

Python 예시:

```python
import socket, json

def clumsy(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect("/run/clumsy.sock")
    s.sendall(json.dumps(cmd).encode())
    resp = json.loads(s.recv(65536))
    s.close()
    return resp

clumsy({"cmd": "set", "module": "lag", "enabled": True, "lag-time": 200})
print(clumsy({"cmd": "get_stats"}))
```

지원 명령은 [manual.md 7-2절](../manual.md)의 Named Pipe 목록과 동일합니다.

---

## 8. WSL2에서 개발하기

이 포팅은 WSL2 Ubuntu 24.04(커널 6.18.35.2) + g++-16에서 개발·검증했습니다.
NFQUEUE 모듈이 기본 제공되므로 커스텀 커널이 필요 없습니다.

```bash
sudo modprobe nfnetlink_queue     # 보통 자동 로드됨
make && make test
```

> ⚠️ **WSL2는 자체 네트워크 네임스페이스를 사용합니다.**
> WSL2 안에서 도는 clumsy는 **Windows 호스트 애플리케이션의 트래픽을 조작할 수 없습니다.**
> WSL2는 *리눅스 포팅 개발·검증용*이며, 실제 게임 QA는 Windows 빌드나 네이티브 리눅스
> 머신에서 하세요.

---

## 9. 문제 해결

**`Failed to open NFQUEUE (Operation not permitted)`**
권한 부족입니다. 3절을 보세요.

**`Failed to bind queue 0 (Device or resource busy)`**
다른 프로세스가 그 큐를 쓰고 있습니다. `--queue-num 1` 등으로 바꾸세요.

**트래픽이 전부 멈췄다**
NFQUEUE 규칙은 남아 있는데 clumsy가 죽은 상태입니다. 규칙을 제거하세요:
```bash
sudo iptables -D OUTPUT -p udp --dport <포트> -j NFQUEUE --queue-num 0
```

**clumsy가 패킷을 전혀 못 받는다**
1. 규칙이 실제로 걸렸는지: `sudo iptables -L OUTPUT -n -v --line-numbers`
2. 규칙 카운터가 증가하는지 확인
3. 방향이 맞는지 — 나가는 트래픽은 `OUTPUT`, 들어오는 트래픽은 `INPUT` 체인
4. clumsy 필터 표현식이 너무 좁지 않은지 (`--verbose on`으로 확인)

**`nfq: ENOBUFS, kernel dropped packets`**
clumsy가 따라가지 못해 커널이 패킷을 버렸습니다. 모듈 버퍼(lag/bandwidth)를 줄이거나
필터를 좁혀 트래픽을 줄이세요.

**duplicate를 켜니 트래픽이 폭발한다**
fwmark ACCEPT 규칙이 없습니다. 6절을 보세요. `--auto-iptables on`을 쓰면 이 규칙이
자동으로 들어갑니다.

**종료했는데 iptables 규칙이 남아 있다**
`--auto-iptables`를 썼다면 clumsy가 어떤 규칙을 지우지 못했는지 콘솔에 출력합니다.
그 줄을 그대로 `-D`로 바꿔 실행하면 됩니다. 자동 규칙에는 `--queue-bypass`가 붙어 있으므로
남아 있어도 트래픽은 정상 흐릅니다.

**제어 소켓에 연결이 안 된다**
`/run/clumsy.sock`이 없으면 `/tmp/clumsy.sock`으로 폴백됐을 수 있습니다.
시작 배너의 `Control socket:` 줄에 실제 경로가 표시됩니다.
