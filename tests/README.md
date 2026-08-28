# tests/

빌드 시스템에 묶여 있지 않은 독립 검증용 프로그램들입니다. 필요할 때 직접 컴파일해 돌립니다.

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

WSL2 Ubuntu 24.04(커널 6.18.35.2)에서 네 모드 모두 검증 완료. 자세한 결과는
[TODO.md](../TODO.md)의 "4.0 WSL2 개발 환경" 절 참조.

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
