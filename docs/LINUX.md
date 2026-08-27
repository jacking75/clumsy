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

## 2. 빌드

```bash
make                # 릴리스 빌드 → bin/linux/clumsy
make DEBUG=1        # 디버그 빌드 (트레이스 로그 기본 활성화)
make test           # 패킷 헬퍼 계약 테스트 실행
make clean
```

`make install-deps`로 Debian 계열 의존성을 한 번에 설치할 수 있습니다.

빌드 결과물 옆에 `config.json`과 `web/index.html`이 자동 복사됩니다.
clumsy는 이 파일들을 **실행 파일과 같은 디렉토리에서** 찾으므로, 옮길 때는 함께 옮기세요.

> `genie.lua`가 여전히 정식 빌드 정의이지만 GENie 바이너리가 Windows 전용이라,
> 리눅스에서 실제로 테스트되는 경로는 이 `Makefile`입니다. 소스 파일을 추가하면 양쪽 모두 갱신하세요.

---

## 3. 권한

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

## 4. Windows와 가장 크게 다른 점 — 필터가 2단계

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
sudo iptables -I OUTPUT -m mark --mark 0xC1 -j ACCEPT          # ← 아래 5절 참고
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

## 5. duplicate 모듈과 fwmark (중요)

리눅스에서 복제 패킷은 **raw 소켓으로 재주입**됩니다. 그런데 재주입된 패킷은 OUTPUT 체인을
다시 통과하므로, 아무 조치가 없으면 **자기가 만든 복제본이 다시 큐에 들어가 무한 증폭**됩니다.
(실측: 10개 전송 → 246,106개 수신)

그래서 clumsy는 주입 소켓에 fwmark(기본 `0xC1`)를 찍습니다. 이 마크를 NFQUEUE 규칙보다
**먼저** ACCEPT 시켜야 합니다:

```bash
sudo iptables -I OUTPUT -m mark --mark 0xC1 -j ACCEPT
```

마크 값이 기존 방화벽 정책과 충돌하면 `--inject-mark <n>`으로 바꾸세요.

안전장치로, 초당 5000패킷을 넘겨 주입되면 clumsy가 **주입을 자동 중단하고** 누락된 규칙을
알려줍니다. 규칙을 깜빡해도 패킷 폭풍 대신 명확한 진단 메시지가 나옵니다.

### duplicate 모듈의 리눅스 제약

| 상황 | 동작 |
|------|------|
| outbound IPv4 복제 | 정상 동작 |
| inbound 복제 | 복제본 드롭 (raw 소켓은 송신만 가능) |
| IPv6 복제 | 복제본 드롭 |

원본 패킷 자체는 어느 경우에도 정상 처리됩니다. 복제본만 생성되지 않습니다.

---

## 6. Windows와 다른 점 요약

| 항목 | Windows | Linux |
|------|---------|-------|
| 캡처 백엔드 | WinDivert 커널 드라이버 | NFQUEUE (`libnetfilter_queue`) |
| 대상 선택 | 필터 표현식만 | iptables 규칙 + 필터 표현식 |
| 권한 | 관리자(UAC) | `CAP_NET_ADMIN` |
| 권한 상승 | `--elevate on`으로 UAC 재실행 | 불가 — sudo/setcap 안내만 출력 |
| Named Pipe API | 지원 | **미지원** (HTTP API 사용) |
| 웹 대시보드 / REST / SSE | 지원 | 지원 (동일) |
| `--process` 필터 | TCP/UDP 포트 테이블 | `/proc/<pid>/fd` + `/proc/net/*` |
| duplicate 모듈 | 제약 없음 | outbound IPv4만 (5절 참고) |
| pcap / 리포트 / 시나리오 / 프로파일 | 지원 | 지원 (동일) |

Named Pipe만 빠진 이유: 제어 계층(`controlapi.cpp`)이 양쪽 공용이라 HTTP API로 모든 명령을
쓸 수 있고, 아무도 쓰지 않을 리눅스 전용 IPC를 새로 만들 이유가 없기 때문입니다.

---

## 7. WSL2에서 개발하기

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

## 8. 문제 해결

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
fwmark ACCEPT 규칙이 없습니다. 5절을 보세요.
