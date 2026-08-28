# Working Log

## 2026-08-28 12:05 KST — Windows 실캡처 회귀 검증 (마지막 미검증 항목)

- 세션 내내 "관리자 권한이 없어 불가"로 남겨뒀던 항목입니다. `Start-Process -Verb RunAs`로
  UAC를 띄우고 사용자가 승인하면, 승격된 clumsy를 REST API로 원격 제어해 자동 검증할 수
  있다는 걸 확인했습니다. UAC 클릭 1회, 약 1분.
- 하네스를 `tests/windows/capture_test.ps1`로 리포지토리에 포함 — **23항목 전부 통과**.
  드라이버 오픈, 무손상 통과(captured=20 sent=20 정확 일치), drop 100% → 0,
  **lag 400ms 설정에 실측 407ms**, tamper 변조 후에도 20/20 도착(체크섬 재계산 정확),
  duplicate ×3 → 정확히 30, bandwidth 1KB/s → 48 전달 + 72 대기, pcap 헤더, 정상 종료.
- **하네스 함정 2건**(README에 기록): (1) 수신자를 Start-Job으로 띄웠더니 바인딩 전에
  송신이 시작돼 아무것도 측정 안 됨 — lag 케이스만 패킷을 받은 게 단서였습니다(400ms 지연이
  잡에게 준비 시간을 준 것). (2) `-Verb RunAs`가 출력 리다이렉션을 못 해 쓴 `.bat` 래퍼가
  실행 전에 사라짐 — 로그 대신 REST API를 쓰도록 바꿔 제거.
- **bandwidth 오탐 해소**: 초기 "스로틀링 없음"은 결함이 아니라 부하 부족이었습니다.
  토큰 버킷이 max(64KB, 제한×2초)로 가득 찬 채 시작하므로 작은 버스트는 설계상 통과합니다.
  164KB를 밀어 넣으니 48/120 + 72 대기로 정확히 동작(48×1400B ≈ 64KB, 버킷 초기값과 일치).
  사용자가 오해하기 쉬운 지점이라 manual.md에 초기 버스트 허용량을 명시했습니다.
- **남은 항목 없음** — Phase 1~4 및 후속 정리가 모두 완료·검증되었습니다.

## 2026-08-28 11:35 KST — 히스토리에서 IUP 제거 (58MB → 1.8MB)

- 사용자 확인("원 저장소와 병합할 일 없음, 과거 삭제 가능") 후 진행. 되돌릴 수 없는 작업이라
  전체 ref를 담은 백업 번들을 먼저 만들었습니다(`clumsy-backup-20260828-112521/`, 59MB).
- **조사 결과**: 4.9에서 파일을 지워도 클론 용량이 그대로였던 이유는 히스토리에 IUP가
  3.8/3.16/3.27/3.30 네 버전, 18개 디렉토리 변종, 비압축 192MB로 쌓여 있었기 때문입니다.
  나머지 경로는 전부 소형이라 IUP만 제거하면 충분했습니다.
- `git filter-repo --path-regex '^external/iup' --invert-paths` 실행.
  glob 대신 앵커된 정규식을 쓴 이유는 무엇이 매치되는지 모호하지 않게 하기 위해서입니다.
- `jlennox-master`/`update-iup`(둘 다 2015년) 원격 브랜치 삭제 — IUP 객체를 붙들고 있어
  남기면 원격 용량이 줄지 않습니다. `gh-pages`는 IUP가 0개이고 서비스 중인 웹사이트라 유지.
- **검증**: HEAD 트리 해시가 재작성 전과 완전히 동일(워킹 트리 무변경), 히스토리 내 IUP 0개,
  커밋 122개·태그 6개 보존, 신규 클론 시 .git 1.9MB 확인, Windows/리눅스 빌드 경고 0개,
  계약 테스트 16항목 및 .deb 빌드 정상.
- **영향**: 커밋 해시 전부 변경(기존 클론 재클론 필요), 구 태그는 IUP가 빠져 빌드 불가,
  upstream과 히스토리 분기. 모두 사전 고지 후 승인받은 사항입니다.

## 2026-08-28 11:20 KST — .rpm 빌드 검증 및 패키지 심볼릭 링크 수정

- 앞서 "rpm 툴체인 미설치"로 미검증 처리했던 항목입니다. WSL에 `rpm` 패키지를 설치하면
  Debian에서도 rpmbuild(4.18.2)를 쓸 수 있어 실제로 빌드·검증했습니다.
- `BuildRequires`가 Fedora 패키지 이름이라 Debian rpm DB가 해석하지 못합니다(라이브러리는
  실제로 설치되어 있음). 스펙 검증용 `RPM_NODEPS=1` 옵션을 추가했고, 실제 Fedora 빌드에서는
  의존성 검사가 동작해야 하므로 기본값은 유지했습니다.
- **실제 결함 1건 수정**: 패키지가 `/usr/bin/config.json`을 `/usr/share/clumsy/config.json`
  절대 경로로 링크하고 있었습니다. rpm이 경고할 뿐 아니라 `--root`로 대체 루트에 설치하면
  링크가 호스트를 가리켜 깨집니다. 상대 링크로 바꾸고 `.deb`도 같이 고쳤습니다.
- 검증: rpm 9항목(경고 0, 의존성 자동 탐지, 임시 루트 설치, 상대 링크 해석, 바이너리 실행),
  deb 11항목 재검증 통과.
- 남은 항목은 Windows 실패킷 캡처 회귀(관리자 권한 필요) 하나입니다.

## 2026-08-28 11:10 KST — 미사용 자산 및 MinGW 빌드 경로 제거

- **삭제**: `external/iup-*` 4개 디렉토리(약 48MB, Phase 2에서 IUP 제거 후 미참조),
  `genie.lua`, `scripts/`(ncat 경로 하드코딩 수동 테스트 헬퍼), `clumsy-demo.gif`(제거된
  GUI 데모), `etc/clumsy.manifest`(32비트용, .rc는 clumsy64만 참조),
  `etc/clumsy-icon.png`(미참조).
- **MinGW 제거**: `common.h`의 `__MINGW32__` 분기 2곳 삭제하고 `INLINE_FUNCTION`을
  `_MSC_VER` 기준으로 단순화. POSIX 쪽 `Interlocked*`는 `platform.h`가 이미 제공해
  중복이었습니다. README/CLAUDE.md/CODING_STYLE.md의 MinGW·GENie 언급도 정리.
- **genie.lua를 지운 판단 근거**: MinGW를 빼면 남는 역할이 VS 프로젝트 생성뿐인데
  `msvc/clumsy.vcxproj`가 이미 수동 관리 중이라, 소스 추가 시 3곳(vcxproj/Makefile/genie.lua)을
  동기화해야 하는 부담만 남았습니다. 이제 빌드 정의는 Windows `msvc/clumsy.vcxproj`,
  리눅스 `Makefile` 둘뿐입니다.
- **부수적으로 고친 버그**: `msvc/clumsy.vcxproj.filters`가 존재하지 않는 `.c` 파일 21개를
  참조하고 있었습니다(Phase 1 확장자 전환 때 누락). 실제 소스 목록에서 재생성했습니다.
  빌드에는 영향이 없지만 VS 솔루션 탐색기에 깨진 항목이 보이던 문제입니다.
- **검증**: Windows Debug/Release 경고 0개, 리눅스 make 경고 0개, 계약 테스트 16항목 통과,
  `.deb` 빌드 정상, 실행 파일 정상 동작. 워킹 트리 약 50MB → 1.9MB.
  (git 히스토리에는 남아 있으므로 리포지토리 용량 자체는 줄지 않습니다.)
- **남은 항목**: `.rpm` 실제 빌드 검증(툴체인 필요), Windows 실패킷 캡처 회귀(관리자 권한 필요).

## 2026-08-28 09:49 KST — Phase 4 선택 과제 4건 구현

Phase 4 완료 시 별도 과제로 분리했던 항목들을 이어서 구현했습니다.

- **`--auto-iptables`**: `filterexpr.cpp`에 `filterDeriveIptables()`를 추가해 필터 AST에서
  iptables match 인자를 도출하고, 신규 `iptables_linux.cpp`가 설치·제거를 담당합니다.
  남은 NFQUEUE 규칙은 트래픽을 통째로 블랙홀로 만들기 때문에 "절대 규칙을 남기지 않는다"를
  설계 기준으로 삼았습니다 — 도출 규칙은 필터의 상위집합(좁으면 조용히 놓침, 넓으면 무해),
  `-I`에 쓴 문자열을 그대로 `-D`에 전달, 자기가 설치한 것만 역순 제거, 그리고 모든 규칙에
  `--queue-bypass`를 붙여 SIGKILL로 정리를 못 해도 트래픽이 통과하게 했습니다.
- **리눅스 제어 소켓**: 당초 "HTTP로 충분"이라 판단했으나 기존 Named Pipe 자동화의 이식
  비용을 고려해 Unix 도메인 소켓(`/run/clumsy.sock`, 폴백 `/tmp`)으로 구현. 양쪽 모두
  `controlDispatchJson()`을 호출하므로 JSON이 바이트 단위로 동일합니다.
- **IPv6 복제 주입**: `AF_INET6` raw 소켓 추가로 outbound IPv6 복제 지원.
  inbound 복제는 raw 소켓이 송신 전용이라 구조적으로 불가 — TUN/ifb가 필요하며
  NFQUEUE 모델에서는 얻을 수 없는 능력이라 사유와 함께 문서화했습니다.
- **패키징**: `make package-deb`로 `.deb` 생성. postinst가 `setcap cap_net_admin,cap_net_raw+ep`을
  적용해 **sudo 없이 실행 가능**(setuid root 대신 필요한 두 권한만). 데이터는
  `/usr/share/clumsy/`에 두고 `/usr/bin/`에서 심볼릭 링크(clumsy가 실행 파일 옆에서 찾기 때문).
  스테이징은 `/tmp`에서 — `/mnt/c` 체크아웃은 전부 0777로 보여 `dpkg-deb`가 거부합니다.
- **검증**: 제어 소켓 7항목 + auto-iptables 6항목(자동 설치 → 실제 drop 동작 → SIGINT 후
  잔여 규칙 0개) + 패키지 11항목(빌드→설치→capability→sudo 없이 실행→제거) 전부 통과.
  Windows MSVC Debug/Release 경고 0개 회귀 없음, 리눅스 make 경고 0개, 계약 테스트 16항목 통과.
- **남은 항목**: `.rpm` 실제 빌드 검증(rpm 툴체인 미설치, spec만 작성),
  MinGW(clang) 빌드 검증, Windows 실패킷 캡처 회귀(관리자 권한 필요),
  `external/iup-*` 삭제 여부 결정.

## 2026-08-28 01:12 KST — Phase 4.2~4.6 리눅스 지원 완성

- **4.2 리눅스 캡처 백엔드**: `divert_linux.cpp`(libnetfilter_queue)를 `divert.cpp`와 동일한
  구조로 구현. NFQUEUE 특유의 제약 2가지를 백엔드 훅으로 흡수 — 모든 패킷 id는 반드시 한 번
  verdict를 받아야 하므로 `packetBackendOnFree()`에서 NF_DROP 정산, 큐 id는 한 번만 verdict
  가능하므로 `cloneNode()`/`packetBackendPrepareClone()`으로 복제본은 raw 소켓 재주입.
- **4.3 필터 언어 계층 분리**: `filterexpr.cpp` 신규 — WinDivert 문법 서브셋을 재귀 하향
  파싱해 AST로 평가. iptables가 "무엇을 큐로 보낼지", clumsy 필터가 "무엇을 열화시킬지"를
  담당하는 2단계 구조. 미매치 패킷은 무손상 통과. 미지원 문법은 시작 시점에 오류로 보고.
- **4.4 권한 처리**: `elevate_linux.cpp` — euid만 보지 않고 `/proc/self/status`의 CapEff에서
  CAP_NET_ADMIN을 확인해 setcap/컨테이너 구성도 정상 인식. 리눅스는 재실행 대신 안내만 출력.
- **4.5 빌드 시스템**: `platform.h`/`platform_linux.cpp`로 Win32 어휘를 POSIX에 재표현(모듈을
  std::atomic/thread로 바꾸지 않는다는 CODING_STYLE 1절 원칙 준수). 플랫폼 전용 파일은
  `_win`/`_linux` 짝으로 분리, 몇 줄 차이는 `#if defined(_WIN32)`. `Makefile` 신규(실제 테스트
  경로), `genie.lua`에 CLUMSY_LINUX 타겟 추가. httpserver/plugin/pcapexport/pipe/main 포팅,
  `procfilter_linux.cpp`(/proc 기반) 신규.
- **4.6 문서화**: `docs/LINUX.md` 신규(패키지·빌드·권한·iptables 연동·플랫폼 차이·문제해결),
  README/CLAUDE.md 갱신.
- **개발 중 발견해 수정한 실제 버그 2건**:
  (1) raw 소켓 `sendto()`가 캡처 mutex를 쥔 채 블로킹 → 3스레드 데드락으로 프로세스 영구 정지.
  SOCK_NONBLOCK + MSG_DONTWAIT + EAGAIN 시 복제본 포기로 수정, divertStop 대기에 5초 상한 추가.
  (2) duplicate 무한 증폭 — 재주입 복제본이 OUTPUT 체인을 다시 타고 같은 NFQUEUE 규칙에 걸림.
  **실측 10패킷 → 246,106패킷.** 주입 소켓 fwmark(기본 0xC1) + 초당 5000 상한 자동 차단으로 수정.
- **검증**: 리눅스 실트래픽 7케이스(무설정 20/20, drop 0/20, lag +434ms, tamper 변조,
  필터 매치/비매치) + 웹/API/duplicate/pcap 17항목 전부 통과. duplicate ×3은 10→30으로 정확.
  Windows MSVC Debug/Release 경고 0개 회귀 없음, 리눅스 make 경고 0개,
  `make test` 패킷 헬퍼 계약 16항목 양 플랫폼 통과.
- **미착수(별도 과제로 명시)**: `--auto-iptables`, .deb/.rpm 패키징, 리눅스용 Named Pipe 대체,
  IPv6/inbound 복제. Windows 실패킷 캡처 경로는 여전히 관리자 권한 환경에서 미검증.

## 2026-08-27 23:37 KST — WSL2 리눅스 환경 검증 및 Phase 4.1 캡처 백엔드 추상화

- **WSL2 환경 확인**: Ubuntu 24.04.2 / 커널 6.18.35.2에 gcc·g++ 16.0.1이 이미 기본 컴파일러로
  설치되어 있어 `genie.lua`의 `LINUX_CXX='g++-16'`을 그대로 쓸 수 있음. C++23(`std::print`,
  `std::expected`) 컴파일·실행 확인. `libnetfilter-queue-dev`/`libmnl-dev`/`iptables` 설치 완료.
- **NFQUEUE 실동작 검증(Phase 4 최대 리스크 해소)**: MS 커스텀 커널에 `CONFIG_NETFILTER_NETLINK_QUEUE=m`
  등이 모듈로 존재. `tests/linux/nfqtest.cpp`를 작성해 iptables NFQUEUE 규칙을 통해 UDP 10개를
  흘려보내며 accept(10수신) / drop(0수신) / delay(약 320ms 지연) / mangle(페이로드 변조) 네 모드를
  모두 확인. clumsy의 모든 모듈 유형이 리눅스에서 구현 가능함이 입증됨.
  구현 시 함정 2건(glibc 헤더를 `<linux/netfilter.h>`보다 먼저 include, `NF_ACCEPT`는
  `<linux/netfilter.h>` 소속)을 TODO.md에 기록.
- **Phase 4.1 완료**: `common.h`에서 `windivert.h` include 제거. `PacketNode`의
  `WINDIVERT_ADDRESS`를 플랫폼 중립 `PacketMeta`로 교체하고, 백엔드 전용 데이터는 불투명
  `PacketBackendMeta`(80바이트 인라인 블롭)에 보관해 패킷당 할당 횟수를 1회로 유지.
  `reset.cpp`/`tamper.cpp`의 WinDivert 헬퍼 직접 호출을 백엔드 중립 함수 4개로 치환하고
  신규 `packetutil_win.cpp`에 구현. 이제 WinDivert 타입·API를 보는 파일은 `divert.cpp`와
  `packetutil_win.cpp` 둘뿐.
- **검증**: MSVC Debug/Release 경고 0개. 신규 `tests/packetutil_test.cpp` 계약 테스트 16개
  전부 통과(플랫폼 헤더 없이 raw 오프셋으로 패킷 조립 → 리눅스 구현체에 그대로 재사용).
  기존 Windows 회귀 12개 항목도 전부 통과.
- **남은 항목**: 실제 패킷 송수신 경로(`divertReadLoop`/`sendAllListPackets`)는 관리자 권한이
  필요해 미검증(코드 리뷰·컴파일까지만). Phase 4.2(리눅스 캡처 백엔드) 미착수.
  TODO 원안이 4.1 대상으로 적은 `procfilter.cpp`는 WinDivert 타입을 쓰지 않고 필터 문자열만
  조립하므로 4.3 소관으로 재분류.

## 2026-08-27 22:51 KST — TODO.md Phase 1~3 전체 구현

- **Phase 1**: `src/*.c` 22개를 `.cpp`로 전환하고 C++23(MSVC `stdcpp23`/`v145`, MinGW `--std=c++23`)으로
  빌드 설정 갱신. 문자열 리터럴 const 화, 네임스페이스 스코프 const 링키지, narrowing 등 전환 오류 수정.
  `/utf-8` 추가로 한글 주석 C4819 경고 제거 → Debug/Release 모두 경고 0개. `docs/CODING_STYLE.md` 신설.
- **Phase 2**: IUP 의존성 전면 제거. `Module`에서 `setupUIFunc`/`iconHandle`을 빼고 `ParamSpec` 메타데이터를
  도입해 웹 UI 폼이 자동 생성되도록 함. Release도 ConsoleApp으로 전환. 신규 `httpserver.cpp`(REST+SSE+정적
  서빙), `controlapi.cpp`(HTTP·Pipe 공용 제어 계층), `json.cpp`(최소 JSON 파서), `etc/web/index.html`
  단일 대시보드 작성. `pipe.cpp`는 트랜스포트만 남기고 응답 형식은 0.3과 바이트 단위 호환 유지.
- **Phase 3**: pcap 덤프(`pcapexport.cpp`, LINKTYPE_RAW), 시나리오 조건/반복 트리거(`when`/`op`/`value`/
  `repeat`/`times`), HTML 세션 리포트(`report.cpp`, 인라인 SVG), 원격 제어(`--web-bind`/`--web-token`/
  `/api/health`), 웹 필터 빌더, 플러그인 로더(`plugin.cpp`, 기본 비활성) 구현.
- 검증: MSVC Debug/Release 경고 0개 빌드, REST/SSE/정적서빙/토큰인증/경로탈출차단/Named Pipe 회귀 등
  32개 스모크 테스트 전부 통과. pcap 파일 헤더(magic/version/snaplen/linktype) 정합성 확인.
  구현 중 발견한 pcap·report 락 누수 2건과 오류 버퍼 미종단 1건을 수정.
- 남은 항목: MinGW(clang) 빌드 검증(환경 미설치), 관리자 권한 콘솔에서의 실제 패킷 캡처 회귀,
  `external/iup-*` 4개 디렉토리(약 48MB) 삭제 여부 결정. Phase 4(리눅스)는 미착수.

## 2026-08-27 18:02 KST — 개발 로드맵 문서(TODO.md) 작성

- external/ 라이브러리(IUP, WinDivert) 의존성 리스크 분석 및 콘솔+웹서버 전환, 리눅스 지원 가능성에 대한 논의를 바탕으로 `TODO.md` 작성.
- Phase 1(C++23 전환, VS2026/gcc16 툴체인) → Phase 2(IUP 제거, 콘솔+내장 웹서버 전환) → Phase 3(pcap 익스포트, 조건부 시나리오, 세션 리포트, 원격 제어, 필터 빌더, 플러그인 모듈 등 신규 기능) → Phase 4(리눅스 지원, libnetfilter_queue 기반) 순서로 체크리스트와 상세 구현 방법을 정리.
- 언어는 사용자 확인 후 전체 C++ 전환으로 결정. 리눅스 지원은 사용자 지시에 따라 최후순위로 배치.
- 이후 개발은 `TODO.md`를 유일한 기준 문서로 사용.
