# clumsy — CLAUDE.md

## 아키텍처

### 핵심 구조

```
공통
  main.cpp          ← 콘솔 진입점, 인수 파싱, 앱 제어 계층, 메인 틱 루프
  packet.cpp        ← PacketNode 연결 리스트 (패킷 큐)
  common.h          ← 전역 타입/매크로/Module·ParamSpec·PacketMeta 선언
  platform.h        ← Win32 ↔ POSIX 호환 계층 (타입/Interlocked/스레드/락)
  utils.cpp         ← 확률 계산, 타이머, CLI 인수 저장소, 콘솔 로깅
  controlapi.cpp    ← 트랜스포트 독립 제어 계층 (HTTP·Named Pipe 공용)
  httpserver.cpp    ← 내장 HTTP 서버 (REST + SSE + 정적 파일 서빙)
  json.cpp          ← 최소 JSON 파서/이스케이퍼
  filterexpr.cpp    ← 필터 표현식 파서. 리눅스에서는 평가기·iptables 규칙 도출,
                      양쪽 모두에서 인스턴스 간 필터 겹침 판정(filterOverlap)
  instance.cpp      ← 인스턴스 등록부: 캡처 중인 인스턴스끼리 필터 겹침을 막음

Windows 전용
  divert.cpp        ← 캡처 백엔드: WinDivert 래퍼, 스레드 관리
  packetutil_win.cpp← 패킷 검사 헬퍼 (페이로드/체크섬/RST)
  elevate.cpp       ← UAC 권한 상승
  procfilter.cpp    ← 프로세스→포트 조회 (GetExtendedTcp/UdpTable)
  instance_win.cpp  ← 등록부 저장소: %ProgramData%\clumsy\instances, DELETE_ON_CLOSE
  pipe.cpp          ← 제어 트랜스포트: Windows는 Named Pipe, POSIX는 Unix 소켓

Linux 전용
  platform_linux.cpp  ← 스레드/뮤텍스/핸들 구현
  divert_linux.cpp    ← 캡처 백엔드: NFQUEUE (libnetfilter_queue)
  packetutil_linux.cpp← 패킷 검사 헬퍼 (직접 헤더 파싱)
  elevate_linux.cpp   ← CAP_NET_ADMIN 확인
  procfilter_linux.cpp← 프로세스→포트 조회 (/proc/<pid>/fd + /proc/net/*)
  iptables_linux.cpp  ← --auto-iptables 규칙 설치/제거
  instance_linux.cpp  ← 등록부 저장소: /tmp/clumsy-instances, flock으로 생존 판별
```

### 플랫폼 분기 규칙

- 파일 전체가 플랫폼 전용이면 **`_win` / `_linux` 짝**으로 나눕니다. 빌드에서 반대쪽을
  제외하므로 `#ifdef`가 필요 없습니다.
- 한 파일 안에서 몇 줄만 다르면 `#if defined(_WIN32)`를 씁니다 (예: `httpserver.cpp`의 소켓).
- Win32 어휘(`DWORD`, `InterlockedExchange16`, `CRITICAL_SECTION`, `GetTickCount`)는
  `platform.h`가 POSIX에서도 제공하므로 **그대로 쓰면 됩니다.** `std::atomic`이나
  `std::thread`로 바꾸지 마세요 — 기존 코드는 컴파일러만 바꾼다는 원칙
  ([docs/CODING_STYLE.md](docs/CODING_STYLE.md) 1절) 대상입니다.
- 빌드 정의는 **둘뿐**입니다: Windows는 `msvc/clumsy.vcxproj`, 리눅스는 `Makefile`.
  소스 파일을 추가하면 해당 플랫폼 것만 갱신하면 됩니다
  (`msvc/clumsy.vcxproj.filters`는 IDE 표시용이라 같이 갱신해두면 좋습니다).

### 캡처 백엔드 경계 (Phase 4.1)

**`common.h`는 `windivert.h`를 include하지 않습니다.** WinDivert 타입/API를 보는 파일은
`divert.cpp`와 `packetutil_win.cpp` 둘뿐이며, 이 경계를 넘지 마세요.
리눅스에서는 `divert_linux.cpp` / `packetutil_linux.cpp`가 같은 자리를 채웁니다.

모듈이 패킷에 대해 알 수 있는 것은 `PacketMeta`뿐입니다:

```c
typedef struct {
    unsigned char outbound;   // 1 = 이 호스트에서 나가는 패킷
    unsigned char ipVersion;  // 4 또는 6
    unsigned char loopback;
    unsigned char impostor;   // 1 = 우리가 주입한 패킷
    unsigned int  ifIdx, subIfIdx;
} PacketMeta;
```

백엔드가 재주입에 필요한 데이터는 `PacketNode::backend`(불투명 80바이트 블롭)에 들어갑니다.
WinDivert는 여기에 `WINDIVERT_ADDRESS`를, 리눅스 백엔드는 NFQUEUE packet id를 넣습니다.
모듈은 이 블롭을 절대 해석하지 않습니다. 패킷을 복제할 때는 `cloneNode()`를 쓰세요
(`duplicate.cpp` 참고) — 백엔드마다 복제의 의미가 다르기 때문입니다.

백엔드 훅 두 개도 이 경계에 속합니다:

- `packetBackendOnFree(node)` — `freeNode()`가 호출합니다. NFQUEUE는 넘겨받은 모든
  패킷 id에 반드시 verdict를 줘야 하므로(주지 않으면 커널 큐가 멈춤), 리눅스 백엔드가
  여기서 `NF_DROP`을 발급합니다. Windows에서는 아무것도 하지 않습니다.
- `packetBackendPrepareClone(src, dst)` — Windows는 캡처 주소를 그대로 복사하고,
  리눅스는 "합성 패킷"으로 표시해 raw 소켓으로 내보냅니다(큐 id는 한 번만 verdict 가능).

패킷 내용을 들여다봐야 하면 백엔드 중립 헬퍼 4개를 씁니다 — 직접 파싱하지 마세요:

```c
int  packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen);
void packetRecalcChecksums(char *packet, UINT len);
int  packetSetTcpRst(char *packet, UINT len);
UINT packetMinTcpSize(void);
```

이 네 함수의 계약은 `tests/packetutil_test.cpp`가 검증합니다. 백엔드를 추가하면
같은 테스트를 통과해야 합니다.

프로젝트에 GUI가 없습니다. 사용자 인터페이스는 **콘솔 로그 + 웹 대시보드**(`etc/web/index.html`)이며,
자동화는 REST API와 Named Pipe API로 합니다. IUP 의존성은 Phase 2에서 완전히 제거되었습니다.

### 모듈 시스템

각 기능은 `Module` 구조체로 구현됩니다 (`common.h`):

```cpp
typedef struct {
    const char *displayName;   // UI 표시 이름
    const char *shortName;     // CLI 인수 접두사 (예: "lag", "drop")
    short *enabledFlag;        // 활성화 상태 (volatile)
    void (*startUp)();         // 모듈 활성화 시 호출
    void (*closeDown)(head, tail); // 비활성화 시 호출 (버퍼 플러시)
    short (*process)(head, tail);  // 매 클럭 틱마다 패킷 처리
    int (*setParam)(const char *key, const char *value);  // 파라미터 설정
    int (*getParams)(ParamKV *kv, int maxKv);             // 현재 값 조회
    const ParamSpec *paramSpecs;   // 웹 UI 폼 자동 생성용 메타데이터
    int paramSpecCount;
    // 런타임 필드
    short lastEnabled;
    short processTriggered;
    volatile LONG affectedCount;
} Module;
```

`ParamSpec`은 웹 UI가 모듈을 전혀 모른 채 입력 폼을 그릴 수 있게 해 줍니다:

```cpp
typedef struct {
    const char *key;        // setParam과 동일한 키 (예: "lag-time")
    const char *label;      // 사람이 읽는 표시명
    const char *type;       // "int" | "float" | "percent" | "bool" | "action"
    double minVal, maxVal;  // 입력 범위 (해당 없으면 0)
} ParamSpec;
```

`GET /api/modules` 응답이 이 배열을 그대로 직렬화하므로, 새 모듈을 추가해도
`etc/web/index.html`은 수정할 필요가 없습니다.

현재 모듈 목록 (`main.cpp` 상단, 처리 순서 = 우선순위):

| 파일 | 모듈 | 기능 |
|------|------|------|
| `lag.cpp` | Lag | 고정 지연(ms) |
| `jitter.cpp` | Jitter | 랜덤 지연 (min~max) |
| `drop.cpp` | Drop | 확률적 패킷 드롭 |
| `burstloss.cpp` | Burst Loss | 버스트 손실 (Gilbert-Elliott) |
| `blackout.cpp` | Blackout | 주기적 연결 두절 |
| `throttle.cpp` | Throttle | 일시적 패킷 억제 (버스트 후 일괄 전송/드롭) |
| `duplicate.cpp` | Duplicate | 패킷 복제 |
| `ood.cpp` | Out of Order | 패킷 순서 뒤섞기 |
| `tamper.cpp` | Tamper | 패킷 페이로드 변조 |
| `reset.cpp` | Reset | TCP RST 강제 전송 |
| `bandwidth.cpp` | Bandwidth | 대역폭 상한 제한 (KB/s) |

### 스레드 구조

```
메인 스레드 (틱 루프, 200ms)
    │   scenarioTick() / statsLogTick() / reportTick()  ← appLock 보호
    ├─ divertReadLoop    ← WinDivertRecv() 블로킹 수신   (divert mutex)
    ├─ divertClockLoop   ← 40ms 주기로 패킷 처리 트리거  (divert mutex)
    ├─ pipeServerLoop    ← Named Pipe 요청 처리
    └─ httpAcceptLoop    ← 연결마다 워커 스레드 생성
         └─ connectionThread × N (SSE 스트림은 장시간 유지)
```

동기화 규칙:

| 대상 | 보호 수단 |
|------|----------|
| 패킷 리스트(`head`/`tail`), 모듈 버퍼 | divert mutex (divert.cpp 내부) |
| `PacketNode::backend` 블롭 | 소유 백엔드만 접근, divert mutex 안에서만 |
| 모듈 파라미터/활성화 플래그 | `InterlockedExchange16` / `InterlockedExchange` |
| 캡처 시작/중지, 통계 로그, 리포트 샘플링 | `appLock` (main.cpp) |
| pcap 파일 핸들 | `pcapLock` (pcapexport.cpp) |
| 리포트 샘플/이벤트 배열 | `reportLock` (report.cpp) |
| 인스턴스 등록부(프로세스 간) | `regLock()` — Windows 명명 뮤텍스, 리눅스 `.lock` flock |

### 여러 인스턴스

clumsy는 여러 프로세스가 동시에 돕니다(클라이언트마다 다른 조건 등). 인스턴스마다 `--web-port`,
`--pipe-name`(리눅스는 `--queue-num`도)이 달라야 하며, 이미 쓰이면 종료 코드 4입니다. 그래서:

- 이름이 고정된 프로세스 전역 자원(파이프 이름, 소켓 경로, 잠금 파일)을 새로 만들지 마세요.
  필요하면 `--pipe-name`처럼 인스턴스마다 다르게 만들 수 있어야 합니다.
- 웹 포트는 Windows에서 `SO_EXCLUSIVEADDRUSE`로 엽니다. `SO_REUSEADDR`로 되돌리면 두 번째
  인스턴스가 같은 포트를 공유해 버립니다.
- 캡처 시작(`appStartCapture`)은 `instanceClaim()`으로 다른 인스턴스의 필터와 겹치는지 먼저
  검사합니다. 겹치면 `CAPTURE_CONFLICT`(CLI 종료 코드 4, REST 409). 리눅스 `--auto-iptables`에서는
  필터가 아니라 도출된 iptables 규칙(`filterCaptureExpr`)끼리 비교합니다. NFQUEUE는 먼저 매치된
  규칙이 패킷을 가져가기 때문입니다.
- 겹침 판정 `filterOverlap()`은 "분리"를 잘못 답하면 안 됩니다. 모르는 항목은 자유 변수로 두어
  항상 "겹칠 수 있음" 쪽으로 틀리게 합니다. 바꾸면 `tests/filteroverlap_test.cpp`를 통과해야 합니다.

HTTP/Pipe 스레드는 패킷 리스트를 절대 건드리지 않고 파라미터와 앱 제어 함수만 호출합니다.

### 패킷 흐름

```
WinDivertRecv()
    → PacketNode 생성 → appendNode() (tail 앞에 삽입)
    → divertConsumeStep()
        → pcapExportWriteStage(PRE, ...)   [--pcap-stage pre|both일 때]
        → 각 모듈 process(head, tail)       [순서대로]
        → sendAllListPackets()
            → pcapExportWriteStage(POST, ...) [기본]
            → WinDivertSend()
```

---

## 빌드 주의사항 (개발자)

- 언어는 **C++23**입니다. MSVC는 `stdcpp23` + `/utf-8`(한글 주석의 C4819 경고 방지),
  리눅스는 `g++-16 --std=c++23`을 사용합니다. 플랫폼 툴셋은 `v145`(VS2026).
  MinGW/clang 빌드는 지원하지 않습니다.
- Debug/Release 모두 **ConsoleApp**입니다. `LOG()`는 항상 컴파일되지만 런타임 플래그
  `logVerbose`로 게이팅되며(Debug 기본 on, Release 기본 off, `--verbose on|off`로 변경),
  `INFO()`는 항상 출력됩니다.
- 콘솔에 출력되는 **문자열 리터럴은 ASCII만** 사용합니다. Windows 콘솔은 사용자 OEM
  코드페이지(한국어 환경은 949)로 동작하므로 UTF-8 em-dash 등은 깨집니다. 주석은 UTF-8 자유.
- 외부 의존성은 WinDivert 하나뿐입니다. HTTP 서버/JSON 파서/pcap 라이터/웹 UI는 자체 구현이며,
  이 방침의 근거는 [docs/CODING_STYLE.md](docs/CODING_STYLE.md) 5절에 있습니다.
- 리눅스 개발은 **WSL2(Ubuntu 24.04) + g++-16**에서 합니다. 빌드·권한·iptables 연동·플랫폼
  차이는 [docs/LINUX.md](docs/LINUX.md)에 전부 정리되어 있습니다.
  단, WSL2는 자체 네트워크 네임스페이스라 **Windows 호스트 앱의 트래픽은 조작하지 못합니다** —
  리눅스 포팅 개발·검증 전용입니다.
- 리눅스에서 duplicate 모듈을 쓰려면 fwmark ACCEPT 규칙이 **반드시** 필요합니다. 없으면
  재주입된 복제본이 큐로 되돌아와 무한 증폭됩니다(실측 10 → 246,106).
  `--auto-iptables on`이 이 규칙까지 알아서 넣어줍니다. 자세한 내용은
  [docs/LINUX.md](docs/LINUX.md) 6절.
- `--auto-iptables`가 설치하는 규칙에는 **항상 `--queue-bypass`가 붙어야 합니다.**
  정리에 실패했을 때 트래픽이 블랙홀이 되는 것과 그냥 통과하는 것의 차이입니다.
  `iptables_linux.cpp`를 수정할 때 이 불변식을 깨지 마세요.

---

## 새 모듈 추가 방법

1. `src/<name>.cpp` 생성
2. `Module` 구조체 인스턴스 정의 (`lag.cpp` 참고)
   - `setParam` / `getParams`에 `<name>-inbound`, `<name>-outbound`를 포함시킵니다.
   - `paramSpecs` 배열을 정의하면 웹 UI 폼이 자동 생성됩니다.
3. `common.h` 에 `extern Module <name>Module;` 선언 추가
4. `main.cpp` 의 `modules[]` 배열에 추가 (처리 순서 결정)
5. `MODULE_CNT` 값 증가 (`common.h`)
6. `msvc/clumsy.vcxproj`의 `ClCompile` 목록과 `Makefile`의 `SOURCES`에 추가
7. `utils.cpp`의 `moduleDescription()` 표에 한 줄 설명 추가 (`--help`와 `GET /api/modules`에 표시)
8. `llms.txt`와 `llms_kr.txt`의 모듈 표에 파라미터 추가 — API 테스트가 양쪽의 누락을 잡습니다

패킷 방향은 `pac->meta.outbound`로 판별하고, 페이로드 조작이 필요하면 위의 백엔드 중립
헬퍼를 쓰세요. `windivert.h`를 include하는 순간 리눅스 빌드가 깨집니다.

CLI 인수(`--<name> on`, `--<name>-<param> <값>`)와 REST API 노출은 자동입니다:
`main.cpp`의 `applyCliModuleParams()`가 `paramSpecs`를 순회하고,
`controlapi.cpp`가 `modules[]`를 순회하기 때문입니다. `--help`의 모듈 목록도
`printModuleUsage()`가 `paramSpecs`에서 생성합니다.

---

## LLM/스크립트가 쓰는 인터페이스 (깨면 안 되는 약속)

이 도구는 사람뿐 아니라 AI 에이전트가 `--help`와 REST 응답만 보고 다루는 것을 전제로 합니다.
에이전트는 결과로만 판단하므로, **조용히 무시하거나 조용히 바꾸는 동작을 만들지 마세요.**

- **모듈 외 CLI 옵션을 추가하면** `main.cpp`의 `globalArgs[]`와 `printUsage()`에 함께 넣습니다.
  빠뜨리면 `validateCliArgs()`가 그 옵션을 "unknown option"으로 거부합니다(종료 코드 2).
- **외부에서 들어온 값은 검증 경로로 적용합니다**: `applyModuleParamChecked()` /
  `applyModuleKVChecked()`(`utils.cpp`). `setParam`은 범위 초과를 조용히 자르고
  `atoi("abc")`를 0으로 받기 때문에 직접 부르면 안 됩니다. 검증 없는 `applyModuleKV()`는
  clumsy가 직접 쓴 값(프로파일)과 로드 시 `scenarioLint()`를 거친 시나리오 전용입니다.
- **모든 `/api` 응답은 `status`가 있는 JSON**입니다. 오류도 마찬가지이며(`sendApiNotFound`),
  메시지에는 무엇을 대신 써야 하는지를 담습니다.
- **종료 코드**: 0 정상, 2 명령줄 오류, 3 캡처/재생 시작 실패, 4 다른 인스턴스와 충돌(웹 포트,
  파이프 이름, 필터 겹침).
  `--filter`를 줬는데 캡처를 못 시작하면 대기하지 않고 3으로 끝납니다.
- **엔드포인트를 추가하면** `controlapi.cpp`의 `apiDocsJson()` 표에 설명·예시 본문과 함께 넣습니다.
- 에이전트용 가이드는 저장소 루트의 `llms.txt`(영문)와 `llms_kr.txt`(한국어) 한 쌍입니다
  (빌드가 실행 파일 옆 `web/`에 복사하고, `GET /llms.txt`, `GET /llms_kr.txt`로 제공). 손으로 쓰는
  참조 문서이므로 동작을 바꾸면 **둘 다** 고칩니다.

### 문서는 한국어·영어 양쪽을 함께 고친다

사용자에게 보이는 동작을 바꾸면 두 언어 문서를 같은 변경에서 함께 갱신합니다. 한쪽만 고치고 끝내지 마세요.

| 영문 | 한국어 |
|------|--------|
| `README.md` | `README_kr.md` |
| `llms.txt` | `llms_kr.txt` |
| `manual_en.md` | `manual.md` |
| `docs/guide_en.html` | `docs/guide.html` |

매뉴얼과 가이드는 두 언어판의 절 구성(제목 순서·개수, 가이드의 section id)이 1:1로 같습니다. 한쪽에 절을
추가·삭제하면 다른 쪽도 똑같이 맞춥니다. 가이드 두 파일의 `<style>`·`<script>`는 사용자에게 보이는 문자열 외에는
동일하게 유지합니다.

`docs/LINUX.md`, `docs/CODING_STYLE.md`, `docs/ROADMAP-v2.md`, `docs/comic.html`은 한국어판만 있습니다. `LINUX.md`에 적은 내용 중
사용자가 알아야 하는 것(옵션, 권한, 플랫폼 차이)은 영문 독자가 볼 수 있도록 `README.md`·`manual_en.md`에도 들어가야 합니다.
`--help`와 API 메시지는 영문(ASCII) 전용입니다.

### 문서 파일 이름

이 저장소에는 문서 파일 이름 끝에 일련번호를 붙이는 규칙(`설계메모-7.md` 같은 형식, 사용자 공통 규칙)을
**적용하지 않습니다.** 새 문서는 번호 없이 이름만 붙이고, 번호 대장(`doc_numbers.md`)도 두지 않습니다.
