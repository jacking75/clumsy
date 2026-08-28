# tests/

## 한눈에 보기

| 스위트 | 권한 | 무엇을 확인하나 |
|--------|------|----------------|
| `packetutil_test.cpp` | 없음 | 백엔드 중립 패킷 헬퍼 4개의 동작 계약 (16 assert) |
| `latency_test.cpp` | 없음 | 지연 분위수 보간의 정확도 (35 assert) |
| `linux/api_test.sh` | 없음 | REST 제어 평면 전체 (57 assert) |
| `windows/api_test.ps1` | 없음 | 같은 내용의 Windows판 (55 assert) |
| `linux/behaviour_test.sh` | root | 실제 패킷이 정말 바뀌는지 (24 assert) |
| `windows/capture_test.ps1` | 관리자 | WinDivert 실캡처 회귀 (23 assert) |
| `linux/nfqtest.cpp` | root | NFQUEUE 능력 프로브 (포팅 전 사전 조사용) |

앞의 두 개는 `make test`가 자동으로 빌드·실행합니다. 나머지는 포트나 권한이
필요하므로 직접 실행합니다.

```bash
make test                              # 단위 테스트 2종
tests/linux/api_test.sh                # REST 회귀
sudo tests/linux/behaviour_test.sh     # 실패킷 검증
```

```bat
powershell -ExecutionPolicy Bypass -File tests\windows\api_test.ps1
powershell -ExecutionPolicy Bypass -File tests\windows\capture_test.ps1
```

**제어 평면은 두 플랫폼이 완전히 같은 코드**입니다(`controlapi.cpp`, `latency.cpp`,
`scenario.cpp`, `profile.cpp`, `pcapreplay.cpp`). 따라서 한쪽 API 테스트가 통과하면
캡처·주입 백엔드를 제외한 나머지는 양쪽 모두 검증된 것으로 봐도 됩니다.

## `packetutil_test.cpp` — 패킷 헬퍼 계약 테스트

`common.h`에 선언된 백엔드 중립 헬퍼 4개(`packetGetPayload`, `packetRecalcChecksums`,
`packetSetTcpRst`, `packetMinTcpSize`)의 동작 계약을 검증합니다.
`tamper.cpp`와 `reset.cpp`가 패킷을 들여다보는 유일한 통로이므로, **캡처 백엔드를 추가하면
반드시 이 테스트를 통과해야 합니다.**

패킷을 플랫폼 헤더 없이 raw 오프셋으로 조립하므로 Windows/Linux 양쪽에서 같은 소스가 돕니다.

**Windows** (VS 개발자 명령 프롬프트, 리포지토리 루트에서):

```bat
cl /nologo /std:c++latest /EHsc /DX64 /Isrc /Iexternal\WinDivert-2.2.0-A\include ^
   tests\packetutil_test.cpp src\packetutil_win.cpp ^
   /link /LIBPATH:external\WinDivert-2.2.0-A\x64 WinDivert.lib ws2_32.lib
packetutil_test.exe
```

`WinDivert.dll`이 실행 경로에 있어야 합니다. **관리자 권한은 필요 없습니다** —
`WinDivertHelper*`는 드라이버를 건드리지 않는 순수 유저스페이스 함수입니다.

**Linux** (Phase 4.2에서 `packetutil_linux.cpp` 추가 후):

```bash
g++-16 -std=c++23 -Isrc tests/packetutil_test.cpp src/packetutil_linux.cpp -o packetutil_test
./packetutil_test
```

## `linux/nfqtest.cpp` — NFQUEUE 능력 검증

Phase 4 착수 전, 대상 커널에서 libnetfilter_queue로 clumsy가 필요로 하는 네 가지 동작이
가능한지 확인하는 프로브입니다.

| 모드 | 동작 | 대응 모듈 |
|------|------|-----------|
| `accept` | 패킷을 보고 통과시킴 | 캡처 경로 |
| `drop` | 패킷 폐기 | drop / blackout / burstloss |
| `delay` | verdict를 미뤘다가 나중에 발급 | lag / jitter / throttle / bandwidth |
| `mangle` | 페이로드 변조 후 재주입 | tamper / reset |

빌드·실행:

```bash
sudo apt install -y libnetfilter-queue-dev libmnl-dev iptables
g++-16 -std=c++23 -O2 tests/linux/nfqtest.cpp -o nfqtest \
    $(pkg-config --cflags --libs libnetfilter_queue)

sudo modprobe nfnetlink_queue
sudo iptables -I OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num 0
sudo ./nfqtest drop --queue 0 --ms 5000
sudo iptables -D OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num 0   # 반드시 정리
```

> **주의**: NFQUEUE 규칙을 걸어둔 채 userspace 프로그램이 없으면 해당 트래픽이 전부
> 드롭됩니다. 규칙은 항상 정리하세요(또는 `--queue-bypass` 사용).

WSL2 Ubuntu 24.04(커널 6.18.35.2)에서 네 모드 모두 검증 완료. 환경 구축과 제약은
[docs/LINUX.md](../docs/LINUX.md) 참조.

## `windows/capture_test.ps1` — Windows 실캡처 회귀 테스트

**관리자 권한이 필요합니다** (WinDivert 드라이버). 실행하면 UAC 창이 한 번 뜨고,
승인 후 나머지는 REST API로 자동 진행됩니다. 소요 시간 약 1분.

```bat
MSBuild msvc\clumsy.vcxproj /p:Configuration=Release /p:Platform=x64
powershell -ExecutionPolicy Bypass -File tests\windows\capture_test.ps1
```

검증 항목 23개 — 드라이버 오픈, 무손상 통과, `captured`/`sent` 카운터,
drop 100%, lag 지연 실측, tamper(체크섬 재계산 포함), duplicate 배수,
bandwidth 스로틀링, pcap 헤더, 정상 종료.

**설계 메모 두 가지** (같은 함정을 다시 밟지 않도록):

1. 수신 소켓은 **보내기 전에 메인 런스페이스에서 바인딩**합니다. 초기 버전은 수신자를
   `Start-Job`으로 띄웠는데, 잡이 바인딩을 마치기 전에 송신이 시작돼 아무것도 측정하지
   못했습니다. 유일하게 패킷을 받은 게 lag 케이스였는데, clumsy가 400ms 지연시킨 덕분에
   잡이 준비될 시간이 생겼던 것뿐이었습니다.
2. 지연은 **패킷 1개**로 측정합니다. 버스트의 패킷 간 간격이 수치를 뭉갭니다.

**bandwidth 테스트에 큰 부하를 쓰는 이유**: 토큰 버킷이
`max(65535, limit*1024*2)` 바이트로 시작하므로 작은 버스트는 설계상 그대로 통과합니다.
120×1400B = 164KB로 64KB 버킷을 넘겨야 상한이 관측됩니다. 20패킷 테스트는
"스로틀링 없음"으로 나오는데 이는 결함이 아니라 부하 부족입니다.

## `latency_test.cpp` — 지연 분위수 단위 테스트

`latency.cpp`의 히스토그램은 표본을 보관하지 않고 13개 구간의 카운터만 유지합니다.
그래서 **분위수 보간이 틀려도 그럴듯해 보입니다** — 답을 미리 아는 분포를 넣어
비교하는 것 외에는 검증할 방법이 없습니다.

`make test`가 자동으로 빌드·실행합니다. 수동으로는:

```bash
g++-16 -std=c++23 -Isrc tests/latency_test.cpp src/latency.cpp        src/platform_linux.cpp -o latency_test -pthread
./latency_test
```

**Windows에서도 돌려야 합니다.** `latency.cpp`는 공용 코드지만 `Interlocked*`가
플랫폼마다 다른 구현(Win32 intrinsic vs GCC `__atomic` 빌트인)으로 풀리므로,
산술 결과가 같다고 가정하지 말고 확인합니다. VS 개발자 명령 프롬프트에서:

```bat
cl /nologo /std:c++latest /utf-8 /EHsc /O1 /D NDEBUG /D _CRT_SECURE_NO_WARNINGS ^
   /Isrc tests\latency_test.cpp src\latency.cpp /Fe:latency_test.exe
latency_test.exe
```

`platform_linux.cpp`가 필요 없고 관리자 권한도 필요 없습니다.

검증 항목 35개 — 빈 히스토그램, 단일 표본, 균등분포에서 p25/p50/p95 복원,
분위수 단조성, 구간 합계 일치, 롱테일에서 p50과 p99의 분리, 좁은 구간의 과대추정
방지, 1e9ms를 넘는 합계의 롤오버, 범위 밖 입력 차단.

**이 테스트가 실제로 잡아낸 결함**: 초기 구현은 0~400ms 균등분포의 p95를 **467ms**로
보고했습니다. 답이 들어 있는 구간의 명목 상한이 500ms인데 실제 데이터는 400ms까지만
있어서, 보간이 데이터가 없는 100ms 구간까지 퍼뜨린 탓이었습니다. 최소·최대값은
정확히 기록하므로 양 끝 구간을 실제 관측 범위로 좁히도록 고쳐 오차를 87ms에서
25ms 이내로 줄였습니다.

## `linux/api_test.sh`, `windows/api_test.ps1` — REST 제어 평면 회귀

권한이 필요 없습니다. clumsy를 띄우고 모든 엔드포인트를 두드립니다.

검증 범위: corrupt 모듈 등록과 파라미터 왕복, jitter 분포 enum과 옵션 목록 전달,
`/metrics`의 Content-Type·HELP/TYPE 짝·누적 버킷 단조성, `POST /api/apply`의
다중 적용과 미인식 키 보고, 프로파일 삭제와 404, 인라인 시나리오 로드와 400 처리,
pcap 재생의 파일 검증(없는 파일 / 매직 넘버 불일치 / RAW / 이더넷), `/api/stats`의
latency 블록, 신규 엔드포인트의 문서화 여부, 기존 엔드포인트 회귀.

## `linux/behaviour_test.sh` — 실패킷 동작 검증 (root)

api_test가 "API가 맞게 답하는가"라면, 이쪽은 **"패킷이 실제로 바뀌는가"**입니다.
loopback UDP와 전용 포트에 한정한 iptables 규칙을 걸고, 종료 시 제거합니다.

```bash
make && sudo tests/linux/behaviour_test.sh
```

검증 항목 24개:

| 섹션 | 확인 내용 |
|------|----------|
| 기준선 | 모듈이 꺼져 있으면 페이로드가 바이트 단위로 동일 |
| corrupt | checksum 켬 → 20/20 손상된 채 **도달**, 끔 → 0/20 (커널이 폐기) |
| 지연 히스토그램 | lag 300ms 설정 시 실측 wire delay와 p50이 일치, `/metrics`와 `/api/stats` 수치 일치 |
| jitter 분포 | uniform 중앙값이 이론값 200ms 부근, pareto < uniform, normal의 p95 ≤ uniform |
| 시나리오 | 인라인으로 로드한 100% drop이 실제로 20/20을 없앰 |
| pcap 왕복 | 25패킷 캡처 → 재생 → **25패킷 주입** → 수신 측 25개 도달 |

**설계 메모**: 분포를 바꿀 때는 **측정 전에** 캡처를 재시작합니다.
`statsReset()`이 히스토그램을 지우기 때문인데, 순서를 반대로 했던 첫 버전은
직전 섹션의 300ms lag 표본이 섞여 uniform 중앙값을 303ms로 보고했습니다
(이론값은 200ms). 지금은 이 값 자체를 단언합니다.
