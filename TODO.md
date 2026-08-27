# clumsy 개발 로드맵

> **이 문서는 clumsy 프로젝트의 유일한 개발 가이드입니다.** 이후 모든 개발은 이 문서의 순서와 항목을 기준으로 진행합니다.
> 각 작업을 완료하면 아래 체크리스트와 본문의 체크박스를 `[x]`로 표시하세요.

## 진행 순서 및 이유

```
Phase 1  C++ 전환 & 툴체인 현대화      ← 이후 모든 작업의 기반 (언어/빌드 설정)
Phase 2  IUP(GUI) 제거 → 콘솔 + 웹서버  ← 외부 GUI 바이너리 리스크 제거
Phase 3  신규 기능 추가                ← Phase 2의 웹/콘솔 기반 위에 기능 확장
Phase 4  리눅스 지원                   ← 반드시 최후순위 (사용자 지정)
```

Phase 1을 가장 먼저 두는 이유: 이후 Phase에서 작성하는 모든 신규 코드(웹서버, REST API, 리눅스 백엔드)를 C++ 관용구로 바로 작성하기 위해서입니다. 언어 전환을 나중에 하면 새로 짠 코드까지 다시 손봐야 합니다.

Phase 2를 Phase 3보다 먼저 두는 이유: 신규 기능(웹 UI 필터 빌더, 실시간 그래프 등) 대부분이 Phase 2에서 만드는 웹 서버/API 위에 얹히므로, 그 기반이 먼저 있어야 합니다.

Phase 4(리눅스)를 마지막에 두는 이유: 사용자 지정 사항이며, 동시에 Phase 1~3의 산출물(추상화된 코드, 콘솔+웹 UI)이 준비되어 있어야 리눅스 포팅 범위가 최소화되기 때문입니다.

---

## 전체 작업 체크리스트

### Phase 1 — C++ 전환 & 툴체인 현대화
- [x] 1.1 빌드 설정 갱신 (msvc/clumsy.vcxproj: VS2026 툴셋, C++23)
- [x] 1.2 소스 파일 확장자 전환 (.c → .cpp, genie.lua/vcxproj 패턴 갱신)
- [x] 1.3 C→C++ 컴파일 오류 수정
- [x] 1.4 코드 스타일 원칙 문서화 및 팀 공유
- [x] 1.5 리눅스 툴체인 설정 준비 (gcc16, C++23 — genie.lua에 옵션만 추가, Phase 4에서 실사용)
- [x] 1.6 회귀 테스트 및 완료 확인

### Phase 2 — IUP(GUI) 제거 → 콘솔 + 내장 웹서버
- [x] 2.1 IUP 사용처 전수 조사 및 제거 계획 수립
- [x] 2.2 Module 구조체 변경 (setupUIFunc/iconHandle 제거, ParamSpec 도입)
- [x] 2.3 콘솔 출력 모드 전환 (Release도 ConsoleApp, LOG 매크로 통일)
- [x] 2.4 HTTP 서버 코어 구현 (src/httpserver.cpp)
- [x] 2.5 REST API 설계 및 구현 (pipe.c 로직 공용화)
- [x] 2.6 실시간 스트리밍 (Server-Sent Events)
- [x] 2.7 웹 프론트엔드 (정적 대시보드)
- [x] 2.8 인증 (토큰 기반)
- [x] 2.9 Named Pipe API 존치 및 리팩터링
- [x] 2.10 완료 확인 (회귀 테스트)

### Phase 3 — 신규 기능 추가
- [x] 3.1 pcap 익스포트
- [x] 3.2 조건부/트리거 기반 시나리오 확장
- [x] 3.3 세션 리포트 내보내기 (HTML)
- [x] 3.4 원격 제어 강화 (분산 QA 환경)
- [x] 3.5 웹 UI 비주얼 필터 빌더
- [x] 3.6 플러그인형 커스텀 모듈 (선택 과제, 우선순위 낮음)
- [x] 3.7 완료 확인 (문서화 포함)

### Phase 4 — 리눅스 지원 (최후순위)
- [x] 4.1 캡처 백엔드 추상화 (PacketMeta 도입)
- [x] 4.2 리눅스 캡처 백엔드 구현 (libnetfilter_queue)
- [x] 4.3 필터 언어 계층 분리
- [x] 4.4 권한 처리 (capability 기반)
- [x] 4.5 빌드 시스템 (genie.lua 리눅스 네이티브 타겟, gcc16)
- [x] 4.6 배포 문서화
- [x] 4.7 완료 확인

---

## Phase 1 — C++ 전환 & 툴체인 현대화

### 배경
현재 전체 소스(`src/*.c`, 22개 파일)가 C11(`CompileAsC`)로 작성되어 있습니다. `iup.h`, `windivert.h` 모두 `#ifdef __cplusplus / extern "C"` 가드가 이미 있어(직접 확인 완료) C++ 컴파일러로 전환해도 링크 문제는 없습니다. 헤더 래핑 작업 불필요.

`msvc/clumsy.vcxproj`의 `PlatformToolset`은 이미 `v145`로 되어 있습니다. VS2026 정식 설치 후 실제 툴셋 문자열은 IDE의 프로젝트 속성에서 재확인해야 합니다(설치 버전에 따라 값이 다를 수 있음).

### 1.1 빌드 설정 갱신 (msvc/clumsy.vcxproj)
- `<CompileAs>CompileAsC</CompileAs>` 태그 제거(1.2에서 파일 확장자를 `.cpp`로 바꾸면 VS가 자동으로 C++ 컴파일러를 사용하므로 명시 불필요).
- `<LanguageStandard_C>stdc11</LanguageStandard_C>` 제거, `<LanguageStandard>stdcpp23</LanguageStandard>` 추가(Debug/Release 양쪽 `ItemDefinitionGroup`).
- `PlatformToolset`을 VS2026 설치 후 실제 최신 값으로 재설정(프로젝트 속성 → 일반 → 플랫폼 도구 집합에서 확인).
- `_CRT_SECURE_NO_WARNINGS`는 유지(레거시 CRT 함수 호출부가 아직 남아있는 동안 경고 억제 목적).

### 1.2 소스 파일 확장자 전환
대상 (전체 `src/*.c`):
```
main.c divert.c packet.c elevate.c utils.c lag.c jitter.c drop.c
burstloss.c blackout.c throttle.c duplicate.c ood.c tamper.c reset.c
bandwidth.c pipe.c statslog.c procfilter.c scenario.c profile.c
```
모두 `.cpp`로 리네임(`common.h`는 헤더이므로 확장자 유지).

리네임 후 갱신할 곳:
- `genie.lua:37` — `files({'src/**.c', 'src/**.h'})` → `files({'src/**.cpp', 'src/**.h'})`
- `msvc/clumsy.vcxproj`의 `<ClCompile Include=...>` 20개 항목 전부 `.c` → `.cpp`
- `CLAUDE.md`의 "새 모듈 추가 방법" 절 — `src/<name>.c` → `src/<name>.cpp`로 갱신

### 1.3 C→C++ 컴파일 오류 수정 체크리스트
리네임 후 빌드하며 아래 항목을 순서대로 점검합니다.
- **`void*` 암시적 변환**: `malloc`/`calloc` 반환값을 특정 포인터 타입 변수에 대입하는 모든 곳에 명시적 캐스팅 추가. 확인 대상: `packet.c`의 `createNode`, `scenario.c`의 `malloc(SCENARIO_BUF_SIZE)` 등.
- **예약어 충돌**: `new`, `delete`, `class`, `template`, `namespace`, `private`, `public`, `this` 등이 변수/파라미터명으로 쓰였는지 전체 grep.
- **지정 초기화(designated initializer)**: C99 스타일 `.field = value`는 C++20부터 부분 지원되어 대체로 문제 없으나, 배열 지정 초기화(`[i] = x`)는 C++ 미지원 — 발견 시 일반 초기화로 수정.
- **함수 포인터 타입 엄격화**: IUP 콜백 등록부(`IupSetCallback` 등, Phase 2에서 어차피 제거됨)와 `Module.setParam`/`getParams` 함수 포인터 등록부에서 시그니처 불일치 경고 확인.
- **`common.h`의 매크로**(`assert` 재정의, `LOG`/`VsLog`, MinGW GCC atomic builtin)는 C++에서도 동일 동작 — 별도 수정 불필요, 컴파일 확인만.

### 1.4 코드 스타일 원칙
- **기존 코드**(11개 모듈 + 코어 — `divert.cpp`, `packet.cpp` 등)는 로직을 그대로 유지하고 컴파일러만 전환합니다. 클래스/템플릿으로 무리하게 재설계하지 않습니다(회귀 리스크 대비 이득 낮음). 전역 `Module` 구조체(함수 포인터 테이블) 아키텍처는 유지 — `CLAUDE.md`의 기존 모듈 확장 절차와 호환성 유지 목적.
- **신규 코드**(Phase 2의 웹서버/REST API, Phase 4의 리눅스 백엔드 등)는 `std::string`/`std::vector`/`std::optional`/RAII를 적극 사용해 손수 만든 파서·버퍼 관리 코드(예: 기존 `pipe.c`의 flat-JSON 수제 파서)를 대체합니다.

### 1.5 리눅스 툴체인 설정 준비 (실사용은 Phase 4)
- `genie.lua`의 `MINGW_ACTION` 블록은 현재 Windows용 MinGW+clang 빌드 전용이므로 건드리지 않습니다.
- 별도로 리눅스 네이티브 빌드를 위한 컴파일러 지정 로직을 준비해둡니다: `premake.gcc.cc = 'g++-16'`, `buildoptions({'--std=c++23'})`. 이 설정은 Phase 4.5에서 실제 리눅스 타겟 블록에 연결하기 전까지는 사용되지 않습니다(지금 단계에서 코드만 준비, 활성화하지 않음).

### 1.6 완료 기준
- [x] Debug/Release x64 MSVC 빌드 경고 0개로 성공 (VS2026 v145, `/std:c++23 /utf-8`)
- [ ] 기존 MinGW(clang) 빌드가 기존과 동일하게 동작 확인
      — **미검증**: 개발 환경에 clang/make가 설치되어 있지 않음.
      `genie.lua`는 `src/**.cpp` + `--std=c++23`으로 갱신 완료, MSYS2 환경에서 확인 필요.
- [x] 매뉴얼(manual.md)의 기본 시나리오로 회귀 테스트 통과
      — 필터 설정 / 모듈 토글 / 파라미터 / 프로파일 / 프리셋 / 통계 경로 확인 완료.
      실제 패킷 캡처 구간은 관리자 권한 콘솔에서 재확인 필요 (아래 2.10 참고).

---

## Phase 2 — IUP(GUI) 제거 → 콘솔 + 내장 웹서버

### 목표
IUP 의존성을 완전히 제거하고, 콘솔 로그 출력 + 로컬 HTTP 서버 기반 웹 대시보드로 전면 대체합니다. "언제 배포가 끊길지 모르는 외부 GUI 바이너리"에 대한 리스크 제거가 1차 목적입니다.

이미 `src/pipe.c`에 GUI와 완전히 분리된 제어 계층(Named Pipe + flat-JSON, `dispatchCommand`)이 존재하므로, 이번 Phase는 이 계층 위에 HTTP 트랜스포트를 얹는 작업에 가깝습니다.

### 2.1 IUP 사용처 전수 조사 및 제거 계획
- `main.cpp`(893줄) 전체의 `IupOpen`/`IupShow`/`IupSetCallback` 등 초기화·이벤트 루프 코드
- 11개 모듈 각각의 `setupUIFunc`(IUP 컨트롤 생성) 및 `common.h`의 `uiSyncChance`/`uiSyncToggle`/`uiSyncInteger`/`uiSyncFixed`/`uiSyncInt32` 콜백
- 트레이 아이콘 로직(`iconHandle` 필드, `main.cpp` 내 `IMAGE` 속성 업데이트 지점들)
- 위 목록을 모듈 단위로 쪼개 커밋 단위를 나눕니다(모듈 11개 각각 1커밋 권장 — 리뷰 용이성).

### 2.2 Module 구조체 변경 (`common.h`)
- `Ihandle* (*setupUIFunc)()` 필드 제거
- `Ihandle *iconHandle` 필드 제거
- 대신 웹 UI가 입력 폼을 자동 생성할 수 있도록 파라미터 메타데이터 배열 추가:
  ```
  typedef struct {
      const char *key;        // setParam과 동일한 키 (예: "lag-time")
      const char *label;      // 사람이 읽는 표시명
      const char *type;       // "int" | "float" | "percent" | "bool"
      double minVal, maxVal;  // 입력 범위 (해당 없으면 0)
  } ParamSpec;
  ```
  `Module`에 `const ParamSpec *paramSpecs; int paramSpecCount;` 필드 추가. `GET /api/modules` 응답 시 이 배열을 그대로 JSON 직렬화해 프론트엔드가 폼을 그립니다.
- `setParam`/`getParams`/`enabledFlag`/`affectedCount`는 이미 GUI 독립적이므로 변경 없이 유지.

### 2.3 콘솔 출력 모드 전환
- `genie.lua`: Release 구성의 `kind("WindowedApp")` → `kind("ConsoleApp")`으로 변경(Debug는 이미 ConsoleApp).
- `msvc/clumsy.vcxproj` Release `ItemDefinitionGroup`: `<SubSystem>Windows</SubSystem>` → `<SubSystem>Console</SubSystem>`. `<EntryPointSymbol>mainCRTStartup</EntryPointSymbol>` 수동 지정 제거하고 표준 `main` 진입점으로 복귀(현재 수동 지정은 WindowedApp 특유의 처리였을 가능성이 높음 — 제거 후 링크 오류 없는지 확인).
- `common.h`의 `LOG()` 매크로에서 `#ifdef _DEBUG` 분기 제거, 항상 `printf` 기반으로 통일(`OutputDebugString` 경로 삭제). Release에서도 로그가 콘솔에 보이게 되어 `main.c:298`의 기존 `// FIXME as Release is built as WindowedApp, stdout/stderr won't show` 주석이 자연히 해소됩니다.
- 프로그램 시작 시 콘솔에 현재 상태(필터, 웹 UI 접속 주소, 인증 토큰)를 배너로 출력.

### 2.4 HTTP 서버 코어 구현 (신규 `src/httpserver.cpp`)
- Winsock2(Windows) 기반 accept 루프를 별도 스레드로 실행 — 기존 `divertReadLoop`/`divertClockLoop`와 동일한 스레드 모델(단일 mutex 동기화) 재사용.
- 지원 범위를 의도적으로 좁게 유지: HTTP/1.1 GET/POST만, Keep-Alive 미지원(요청마다 연결 종료 — 로컬 관리 도구 특성상 성능 문제 없음), chunked encoding/TLS 미지원. 이 범위라면 외부 HTTP 라이브러리 없이 직접 구현하는 편이 WinDivert/IUP를 벤더링하는 것보다 리스크가 낮습니다(수백 줄 규모, 팀이 영구 유지보수 가능).
- 요청 라인 + 헤더(`Content-Length`만 파싱) + 바디를 읽고, 경로로 정적 파일 서빙(GET)과 JSON API(GET/POST)를 분기.
- 기본 바인딩 `127.0.0.1:8080`(`--web-port`로 변경 가능). 외부 인터페이스 바인딩은 `--web-bind 0.0.0.0` 등 명시적 지정 시에만 허용하고 2.8의 토큰 인증을 강제합니다.

### 2.5 REST API 설계 (`pipe.cpp` 로직 공용화)
- `pipe.cpp`의 `dispatchCommand`/`handleSet`/`handleGetStats`/`handleStop`을 트랜스포트에 의존하지 않는 함수로 신규 `src/controlapi.cpp`에 추출. Named Pipe와 HTTP가 동일 함수를 호출하도록 리팩터링(로직 중복 제거).
- 엔드포인트:
  | 메서드/경로 | 설명 |
  |---|---|
  | `GET /api/modules` | 전체 모듈 상태(enabled, 파라미터 값, ParamSpec) 조회 |
  | `POST /api/modules/{shortName}` | 모듈 설정 변경 (body: JSON) |
  | `GET /api/stats` | 실시간 통계 (`get_stats`와 동일 페이로드) |
  | `POST /api/filter` | 필터 표현식 설정 및 캡처 시작 |
  | `POST /api/stop` | 캡처 중지 |
  | `GET /api/profiles`, `POST /api/profiles/{name}/apply` | `profile.cpp` 연동 |
  | `POST /api/scenario/load`, `POST /api/scenario/start` | `scenario.cpp` 연동 |
- 기존 flat-JSON 파서(`jsonGet`/`applyModuleParams`)는 중첩 구조(ParamSpec 배열 등)가 필요해지는 시점부터 한계에 부딪히므로, 이 시점에 최소 기능 JSON 파서를 C++로 재작성(1.4 원칙에 따라 `std::string`/`std::vector` 기반, 외부 라이브러리 없이 최소 구현 유지).

### 2.6 실시간 스트리밍 (Server-Sent Events)
- `GET /api/stream` — `text/event-stream`으로 통계를 주기적으로(`ICON_UPDATE_MS`=200ms 재사용) push. WebSocket 대비 핸드셰이크/프레이밍이 단순해 자체 구현 난이도가 낮습니다 — Phase 2.4에서 정한 "직접 만들 수 있는 범위" 원칙에 부합.

### 2.7 웹 프론트엔드
- 빌드 도구 없는 단일 정적 파일(`etc/web/index.html`, 인라인 CSS/JS)로 대시보드 제작 — 외부 CDN/프레임워크 의존 금지(오프라인에서도 동작해야 한다는 clumsy의 기존 원칙, README 7번째 특징 "오프라인 환경에서도 동작"과 일치).
- 화면 구성: 모듈별 토글+파라미터 폼(ParamSpec 기반 자동 생성), 실시간 통계 그래프(Canvas API로 직접 그림, 차트 라이브러리 불필요), 필터 입력창 + 프리셋 드롭다운(`config.json` 재사용).
- 정적 리소스는 실행 파일과 함께 배포(`etc/` 폴더 복사, 기존 postbuild 복사 스크립트에 추가).

### 2.8 인증
- 최초 실행 시 랜덤 토큰 생성 → 콘솔에 `Web UI: http://127.0.0.1:8080/?token=...` 형태로 출력.
- `X-Clumsy-Token` 헤더 또는 쿼리스트링으로 전달, 서버가 매 요청마다 비교.
- 로컬호스트 바인딩 시에는 토큰 없이도 접근 허용(기본값, 단일 사용자 편의). `--web-bind`로 외부 바인딩 시에는 토큰 필수화 + 콘솔에 보안 경고 출력.

### 2.9 Named Pipe API 존치
- 기존 자동화 스크립트(매뉴얼 7장 Python 연동 예시) 호환을 위해 `pipe.cpp`는 유지하되, 2.5에서 추출한 공용 dispatch 함수를 호출하도록 리팩터링.

### 2.10 완료 기준
- [x] `external/iup-*` 참조가 `msvc/clumsy.vcxproj`, `genie.lua`, `README.md`, `CLAUDE.md`에서 모두 제거됨
      (`etc/clumsy.rc`의 `iupPreviewDlg` 리소스도 함께 제거)
- [x] 브라우저 접속 → 필터 설정 → Start → 모듈 토글 → 실시간 통계 확인까지 전체 플로우 동작
      — REST/SSE/정적 서빙/토큰 인증 전 경로 검증 완료.
      **실제 패킷 캡처 구간은 관리자 권한 콘솔에서 최종 확인 필요** (검증 환경이 비관리자였음)
- [x] 기존 Named Pipe 자동화 스크립트가 수정 없이 동작(회귀 없음)
      — `set` / `get_stats` / `stop` 응답 형식 바이트 단위 동일 확인
- [x] 관리자 권한 없이 실행 시 웹 UI는 뜨되, 캡처 시작 시 명확한 에러 메시지 반환

> **남은 정리 작업(사용자 결정 필요)**: `external/iup-*` 4개 디렉토리(약 48MB)는 이제
> 어떤 빌드 파일도 참조하지 않습니다. `git rm -r external/iup-*`로 삭제할 수 있으나,
> 되돌리기 어려운 작업이라 의도적으로 남겨두었습니다.

---

## Phase 3 — 신규 기능 추가

### 3.1 pcap 익스포트
**배경**: `statslog.cpp`는 집계 수치만 남기고 실제 패킷 페이로드는 보존하지 않습니다. Wireshark 등 기존 분석 도구와 연동하려면 표준 `.pcap` 덤프가 필요합니다.

**구현**:
- 신규 `src/pcapexport.cpp`. libpcap 파일 포맷은 24바이트 글로벌 헤더 + (16바이트 레코드 헤더 + 원본 패킷)의 단순 반복 구조라 외부 라이브러리 없이 직접 구현 가능.
- `divertConsumeStep`에서 각 모듈의 `process()` 호출 전/후 패킷을 옵션으로 기록(`--pcap-out captured.pcap`, 웹 API `POST /api/pcap/start`).
- 대용량 방지를 위해 최대 파일 크기/패킷 수 제한 옵션 제공.

### 3.2 조건부/트리거 기반 시나리오 확장
**배경**: 현재 `scenario.cpp`는 `"at"` 초 단위 절대 시간 트리거만 지원(코드 확인 완료 — 조건부 트리거 없음). 실제 QA 자동화에서는 "패킷 N개 캡처되면", "대역폭 임계치 초과 시" 같은 조건부 트리거가 더 유용합니다.

**구현**:
- `ScenarioStep`에 `atSec` 외에 옵션 `condition` 필드 추가: `{"when": "captured_count", "op": ">=", "value": 10000}` 형태.
- `scenarioTick()`에서 시간 조건과 조건식 조건을 함께 평가하도록 확장(각 스텝은 독립적으로 자신의 트리거 타입 하나만 가짐 — 시간 기반 로직은 그대로 유지).
- `repeat` 필드로 스텝 반복 지원(예: 30초마다 드롭 확률 재조정) — 장시간 무작위 시나리오 자동화에 유용.

### 3.3 세션 리포트 내보내기
**배경**: 테스트 종료 후 "어떤 조건으로 얼마나 테스트했는지"를 팀에 공유할 산출물이 없음(현재는 CSV/JSON 로그 원본만 존재).

**구현**:
- 캡처 종료(Stop) 시점에 `statslog.cpp`가 수집한 데이터를 요약해 단일 HTML 리포트 자동 생성(`--report-out report.html`, 또는 웹 UI의 "세션 종료 후 리포트 다운로드" 버튼).
- 리포트 내용: 적용 필터, 모듈별 파라미터 타임라인(시나리오 스텝 로그 포함), 총 캡처/영향 패킷 수, 시간대별 그래프(SVG 인라인 — 별도 라이브러리 불필요).

### 3.4 원격 제어 강화 (분산 QA 환경)
**배경**: Phase 2.8에서 마련한 토큰 인증 + 바인딩 옵션을, CI 러너가 원격 테스트 클라이언트의 clumsy를 제어하는 실제 분산 테스트 워크플로우에 맞게 다듬습니다.

**구현**:
- CLI에 `--web-bind <addr> --web-token <token>`(토큰을 고정 지정해 CI 스크립트에서 재사용 가능하도록) 옵션 추가.
- `GET /api/health`(인증 불필요, 단순 생존 확인) 엔드포인트 추가 — CI 헬스체크용.
- 원격 바인딩 시 콘솔에 명확한 보안 경고(신뢰된 네트워크에서만 사용 권장) 출력.

### 3.5 웹 UI 비주얼 필터 빌더
**배경**: WinDivert 필터 문법(`udp and (udp.DstPort == 7777 ...)`)을 매번 손으로 작성해야 함 — 매뉴얼 3장에 예시가 많다는 것 자체가 진입장벽의 방증입니다.

**구현**:
- 웹 UI에 프로토콜(TCP/UDP/모두)·방향(in/out)·포트·IP 조건을 체크박스/입력창으로 조합하면 필터 문자열을 실시간 생성하는 폼 추가(JS만으로 구현, 서버 왕복 불필요).
- 생성된 필터를 `config.json` 프리셋으로 저장하는 "프리셋으로 저장" 버튼(`POST /api/presets`) 연동.

### 3.6 플러그인형 커스텀 모듈 (선택 과제, 우선순위 낮음)
**배경**: 현재 새 모듈 추가는 `CLAUDE.md`에 정리된 대로 소스 파일 추가 후 재컴파일이 필요합니다. 커뮤니티가 재컴파일 없이 실험적 모듈을 추가하고 싶을 수 있습니다.

**구현**:
- `Module` 구조체의 함수 포인터들을 동적 라이브러리(Windows DLL / Linux `.so`)에서 `LoadLibrary`/`dlopen`으로 로드하는 로더 추가.
- **리스크**: 신뢰되지 않은 외부 DLL 로드는 보안 리스크이므로 기본 비활성화, `--enable-plugins <dir>` 명시적 옵션으로만 활성화. 다른 항목 대비 우선순위가 낮으므로 Phase 3의 다른 작업을 모두 마친 뒤 착수해도 무방합니다.

### 3.7 완료 기준
- [x] 각 기능은 독립적으로 온/오프 가능(다른 기능에 영향 없음)
      — pcap/report/plugin 모두 옵션 인수로만 활성화, 미사용 시 런타임 비용 없음
- [x] `manual.md`에 각 기능 사용법 추가 (10~13장 신설)
- [x] 웹 API 엔드포인트 전체 목록을 `GET /api/docs`와 README에 문서화

---

## Phase 4 — 리눅스 지원 (최후순위)

### 4.1 캡처 백엔드 추상화  — 완료

**구현 결과**

- `common.h`에서 `#include "windivert.h"`를 제거. 이제 WinDivert 타입/API를 보는 파일은
  `divert.cpp`(윈도우 캡처 백엔드)와 `packetutil_win.cpp` 둘뿐입니다.
- `PacketNode`의 `WINDIVERT_ADDRESS addr` → 플랫폼 중립 `PacketMeta meta`로 교체:
  ```c
  typedef struct {
      unsigned char outbound;   // 1 = leaving this host
      unsigned char ipVersion;  // 4 or 6
      unsigned char loopback;
      unsigned char impostor;
      unsigned int  ifIdx, subIfIdx;
  } PacketMeta;
  ```
- 백엔드가 재주입에 필요한 데이터(WinDivert는 80바이트 `WINDIVERT_ADDRESS`, 리눅스는 NFQUEUE
  packet id)는 `PacketBackendMeta backend` 불투명 블롭에 인라인 저장. 포인터+별도 malloc 대신
  인라인으로 둬서 패킷당 할당 횟수를 1회로 유지했습니다.
  크기 정합성은 `divert.cpp`의 `static_assert`로 컴파일 타임에 강제합니다.
- `createNode(buf, len, const PacketMeta*, const void *backendMeta)`로 시그니처 변경.
- 모듈의 `pac->addr.Outbound` → `pac->meta.outbound` (11개 모듈).
- `reset.cpp` / `tamper.cpp`가 쓰던 WinDivert 헬퍼 직접 호출을 백엔드 중립 API로 치환.
  신규 `src/packetutil_win.cpp`가 이 4개 함수를 구현합니다:
  ```c
  int  packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen);
  void packetRecalcChecksums(char *packet, UINT len);
  int  packetSetTcpRst(char *packet, UINT len);
  UINT packetMinTcpSize(void);
  ```
  Phase 4.2는 `packetutil_linux.cpp`로 같은 4개를 구현하면 되고, 모듈 코드는 그대로입니다.

**검증**

- MSVC Debug/Release 경고 0개 빌드.
- `tests/packetutil_test.cpp` — 위 4개 함수의 **계약 테스트** 16개 전부 통과.
  플랫폼 헤더 없이 raw 오프셋으로 패킷을 조립하므로 **리눅스 구현체에도 그대로 재사용**합니다.
  Phase 4.2에서 `packetutil_linux.cpp`를 붙인 뒤 이 테스트를 통과시키는 것이 완료 조건입니다.
- 기존 Windows 회귀 12개 항목(CLI 파라미터 적용, 모듈 11개 노출, REST/Pipe API, 리포트,
  대시보드, 정상 종료) 전부 통과.

**주의 — `procfilter.cpp`는 이번 범위가 아님**

TODO 원안은 `procfilter.cpp`를 4.1 영향 파일로 적었지만, 실제로 확인해 보니 이 파일은
WinDivert **타입이나 API를 전혀 쓰지 않습니다**. 하는 일은 WinDivert **필터 문자열**을
조립하는 것뿐이라, 이것은 4.3(필터 언어 계층 분리)에서 다룰 사안입니다. 4.1에서는 건드리지
않았습니다.

**남은 리스크**

`divertReadLoop` / `sendAllListPackets`의 실제 패킷 송수신 경로는 관리자 권한이 필요해
이 환경에서 실행 검증을 못 했습니다. 코드 리뷰와 컴파일까지만 확인된 상태입니다.

### 4.2 리눅스 캡처 백엔드 — 완료

`src/divert_linux.cpp`가 `divert.cpp`와 같은 구조(read 루프 + clock 루프 + 단일 mutex)를
그대로 따르므로 모듈 계약과 타이밍 특성이 Windows와 동일합니다.
`--queue-num`으로 큐 번호를 지정할 수 있고, 시작 시 필요한 iptables 규칙을 콘솔에 안내합니다.

**NFQUEUE 때문에 WinDivert와 근본적으로 달랐던 두 가지**

1. **모든 패킷 id는 반드시 정확히 한 번 verdict를 받아야 합니다.** 모듈이 드롭한 패킷도
   `NF_DROP`으로 답하지 않으면 커널 큐가 멈춥니다. Windows에서는 "안 보내면 드롭"이라
   이런 개념 자체가 없습니다. → `freeNode()`에 `packetBackendOnFree()` 훅을 추가해
   리눅스 백엔드가 여기서 정산하도록 했습니다. 멱등이라 이미 전송된 패킷에는 무해합니다.
2. **큐 id는 한 번만 verdict 가능**하므로 duplicate 모듈의 복제본은 원본의 id를 탈 수 없습니다.
   → `cloneNode()` + `packetBackendPrepareClone()` 훅을 도입해, 리눅스에서는 복제본을
   "합성 패킷"으로 표시하고 raw 소켓으로 재주입합니다.

**개발 중 실제로 터진 버그 2건 (둘 다 수정 완료)**

- **raw 소켓 blocking으로 인한 전체 데드락.** `sendto()`가 캡처 mutex를 쥔 채 블로킹되어
  read 루프가 멈추고, clock 루프가 mutex를 기다리고, `divertStop()`이 두 스레드를
  INFINITE로 기다리며 프로세스가 영구 정지했습니다.
  → 소켓을 `SOCK_NONBLOCK` + `MSG_DONTWAIT`으로 열고 EAGAIN은 복제본 포기로 처리.
  추가로 `divertStop()`의 대기를 5초 상한으로 바꿔, 백엔드가 물려도 원인을 출력하고 빠져나옵니다.
- **duplicate 무한 증폭.** raw 소켓으로 재주입한 복제본이 OUTPUT 체인을 다시 타고
  같은 NFQUEUE 규칙에 걸려 또 복제되는 피드백 루프. **실측 10패킷 → 246,106패킷.**
  → 주입 소켓에 fwmark(기본 `0xC1`, `--inject-mark`)를 찍어 사용자가 NFQUEUE 규칙보다
  먼저 ACCEPT 하도록 하고, 초당 5000패킷을 넘으면 주입을 자동 중단하며 누락된 규칙을
  안내합니다. 규칙을 깜빡해도 패킷 폭풍 대신 명확한 진단이 나옵니다.

**리눅스 제약 (문서화 완료)**

| 상황 | 동작 |
|------|------|
| outbound IPv4 복제 | 정상 |
| inbound 복제 | 복제본 드롭 (raw 소켓은 송신 전용) |
| IPv6 복제 | 복제본 드롭 |

원본 패킷 처리는 어느 경우에도 정상입니다.

**검증** — WSL2 Ubuntu 24.04에서 실제 트래픽으로:

| 케이스 | 결과 |
|--------|------|
| 무설정 | 20 전송 → 20 수신 |
| drop 100% | 20 → **0** |
| lag 400ms | 20 → 20, 첫 패킷 **+434ms** (설정 400ms) |
| tamper 100% | 페이로드 변조되어 도착 (= 체크섬 재계산이 정확) |
| duplicate ×3 | 10 → **30** (증폭 없음) |
| pcap | libpcap 헤더 정합, 레코드 기록 확인 |

### 4.3 필터 언어 계층 분리 — 1차 구현 완료

계획대로 1차 구현(사용자가 iptables 규칙을 걸고, clumsy가 큐에서 받은 패킷에 필터
표현식을 재적용)을 했습니다. 매치되지 않은 패킷은 **손대지 않고 그대로 통과**시킵니다.

신규 `src/filterexpr.cpp` — 재귀 하향 파서가 작은 AST를 만들고 패킷마다 평가합니다.
지원 범위:

| 분류 | 지원 |
|------|------|
| 논리 | `and` `or` `not`, `&&` `\|\|` `!`, 괄호, `true`/`false` |
| 방향 | `inbound`, `outbound`, `loopback` |
| 프로토콜 | `ip`, `ipv6`, `tcp`, `udp`, `icmp`, `icmpv6` |
| 필드 | `ip.SrcAddr`, `ip.DstAddr`, `ip.Protocol`, `{tcp,udp}.{Src,Dst}Port` |
| 비교 | `==` `!=` `>` `<` `>=` `<=` |

**미지원**: IPv6 주소 리터럴, 페이로드 매칭, 패킷/플로우 카운터.
인식하지 못하는 항목은 **캡처 시작 시점에 오류로 보고**합니다 — Windows와 다르게
동작할 필터가 조용히 엉뚱한 트래픽을 건드리는 것보다 낫다는 판단입니다.

덕분에 `config.json` 프리셋과 시나리오 파일을 양 플랫폼에서 그대로 공유할 수 있습니다.
검증: 같은 트래픽에 대해 `udp.DstPort == 7777`(비매치)는 20/20 통과,
`udp.DstPort == 9999`(매치)는 drop 100%가 걸려 0/20.

**2차 구현(`--auto-iptables`)은 계획대로 미착수** — 별도 과제로 남깁니다.

### 4.4 권한 처리 — 완료

신규 `src/elevate_linux.cpp`가 같은 3개 함수를 구현합니다.

`geteuid() == 0`**만으로 판단하지 않습니다.** `/proc/self/status`의 `CapEff`를 파싱해
`CAP_NET_ADMIN` 보유 여부를 확인하므로, `setcap`으로 권한을 준 바이너리나 해당 capability를
가진 컨테이너에서도 정상 인식합니다. euid만 봤다면 이런 정상 구성을 틀리게 거부했을 겁니다.
libcap 의존성을 추가하지 않으려고 비트 번호(12/13)는 직접 정의했습니다.

`tryElevate()`는 리눅스에서 **재실행하지 않고 안내만 출력**합니다. UAC 같은 프롬프트가
없어서, 실행 중인 프로세스를 sudo로 다시 띄우는 것보다 방법을 알려주는 편이 낫습니다:

```
sudo clumsy --filter "..."
sudo setcap cap_net_admin,cap_net_raw+ep ./clumsy
```

권한이 없으면 Windows와 동일하게 **종료하지 않고** 대시보드를 띄운 뒤, 캡처 시작 시
명확한 오류를 반환합니다.

### 4.5 빌드 시스템 — 완료

**신규 `src/platform.h` + `src/platform_linux.cpp`** — Win32 어휘(타입, `Interlocked*`,
`GetTickCount`, `CRITICAL_SECTION`, `CreateThread`/`WaitForSingleObject`, `CreateMutex`)를
POSIX로 재표현합니다. 모든 모듈을 `std::atomic`/`std::thread`로 바꾸는 대안은 패킷 핫패스를
전부 건드리는 일이라 [CODING_STYLE.md](docs/CODING_STYLE.md) 1절 원칙에 따라 배제했습니다.

**플랫폼 분기 방식**: 파일 전체가 플랫폼 전용이면 `_win`/`_linux` 짝으로 나누고 빌드에서
반대쪽을 제외합니다(`divert`, `packetutil`, `elevate`, `procfilter`). 몇 줄만 다르면
`#if defined(_WIN32)`를 씁니다(`httpserver.cpp`의 소켓, `main.cpp`의 단일 인스턴스/시그널).

**`genie.lua`**: `CLUMSY_LINUX=1` + `gmake`일 때 `g++-16`/`--std=c++23`/netfilter 링크를
적용하고 Windows 전용 소스와 `.rc`를 제외합니다.

**`Makefile` (신규, 리포지토리 루트)**: GENie가 이 리포지토리에 Windows 바이너리로만 들어
있어서, **실제로 테스트되는 리눅스 빌드 경로는 이 Makefile입니다.** `make` / `make DEBUG=1` /
`make test` / `make clean` / `make install-deps`. 소스를 추가하면 양쪽 모두 갱신해야 합니다
(CLAUDE.md에 명시).

**포팅한 공용 파일**: `httpserver.cpp`(Winsock→BSD 소켓, SIGPIPE 무시, timeval 타임아웃),
`plugin.cpp`(LoadLibrary→dlopen, FindFirstFile→opendir), `pcapexport.cpp`(FILETIME→
clock_gettime), `pipe.cpp`(Named Pipe는 Windows 전용, POSIX는 빈 구현 — HTTP API로 충분),
`main.cpp`(단일 인스턴스는 flock 기반 pidfile, Ctrl+C는 signal 핸들러).
신규 `src/procfilter_linux.cpp`는 `/proc/<pid>/fd` + `/proc/net/{tcp,udp}`로 프로세스별
포트를 조회합니다.

**검증**: 리눅스 `make` 경고 0개, Windows MSVC Debug/Release 경고 0개(회귀 없음),
`make test` 계약 테스트 16개 통과.

### 4.6 배포 문서화 — 완료

**신규 [docs/LINUX.md](docs/LINUX.md)** — 배포판별 패키지, 빌드, 권한(sudo/setcap),
필터 2단계 구조와 iptables 연동, duplicate fwmark 주의사항, Windows와의 차이 표,
WSL2 개발 안내, 문제 해결 절.

`README.md`에 리눅스 빌드 절과 프로젝트 구조 갱신, `CLAUDE.md`에 플랫폼 분기 규칙과
백엔드 훅 계약 추가.

`.deb`/`.rpm` 패키징은 계획대로 **미착수** — 별도 과제로 남깁니다.

### 4.0 WSL2 개발 환경 — 검증 완료

Phase 4 착수 전 실환경 확인 결과 (2026-08-27):

| 항목 | 결과 |
|------|------|
| 배포판 | Ubuntu 24.04.2 LTS on WSL2, 커널 `6.18.35.2-microsoft-standard-WSL2` |
| 컴파일러 | **gcc/g++ 16.0.1** 설치 완료, `g++-16` 심볼릭 링크 존재 → `genie.lua`의 `LINUX_CXX` 그대로 사용 가능 |
| C++23 | `std::print` / `std::expected` 컴파일·실행 확인 |
| NFQUEUE 커널 지원 | `CONFIG_NETFILTER_NETLINK_QUEUE=m`, `CONFIG_NETFILTER_XT_TARGET_NFQUEUE=m`, `CONFIG_NFT_QUEUE=m` — **모듈로 제공되어 커스텀 커널 불필요** |
| 라이브러리 | `libnetfilter-queue-dev` 1.0.5, `libmnl-dev` 1.0.5, `libnfnetlink-dev`, `iptables` 1.8.10 설치 완료 |

**실동작 검증** — 최소 NFQUEUE 프로그램으로 4가지 동작을 모두 확인했습니다
(UDP 10개를 iptables NFQUEUE 규칙을 통과시켜 수신 측에서 계수):

| 모드 | 결과 | 대응 모듈 |
|------|------|-----------|
| accept | 10 전송 → 10 수신 | 캡처 + 통과 |
| drop | 10 전송 → **0 수신** | drop / blackout / burstloss |
| delay | 10 수신, 약 **320ms 지연**(설정 300ms) | lag / jitter / throttle / bandwidth |
| mangle | 페이로드 변조되어 도착 | tamper / reset |

즉 clumsy의 모든 모듈 유형이 리눅스에서 구현 가능함이 확인되었습니다.

**구현 시 주의할 점 2가지** (검증 중 실제로 걸린 부분):

1. glibc 네트워크 헤더(`netinet/in.h`, `arpa/inet.h`)를 **`<linux/netfilter.h>`보다 먼저**
   include해야 합니다. 그렇지 않으면 `struct in_addr` 중복 정의로 컴파일이 깨집니다.
   (`linux/libc-compat.h`는 glibc가 먼저 왔을 때만 커널 쪽 정의를 억제합니다.)
2. `NF_ACCEPT` / `NF_DROP`은 libnetfilter_queue 헤더가 아니라 `<linux/netfilter.h>`에 있습니다.

**WSL2의 한계 (문서화 필요)**

WSL2는 자체 네트워크 네임스페이스를 사용하므로, WSL2에서 도는 clumsy는
**Windows 호스트 애플리케이션의 트래픽을 조작할 수 없습니다.**
따라서 WSL2는 *리눅스 포팅 개발·검증 환경*으로만 쓰고, 실제 게임 QA는 Windows 빌드나
네이티브 리눅스 머신을 사용해야 합니다. 이 내용은 4.6 배포 문서화에 포함시킬 것.

### 4.7 완료 기준
- [x] 리눅스에서 `--queue-num N` + 사용자가 건 iptables 규칙으로 lag/drop 등 기본 모듈이
      Windows와 동일하게 동작 — 실트래픽 7케이스 통과 (4.2 표 참고)
- [x] 웹 UI가 리눅스에서도 동일하게 접속/제어 가능 — REST/SSE/정적서빙/리포트/pcap 17항목 통과
- [x] `README.md` / `docs/LINUX.md`에 리눅스 설치/실행 가이드 추가

**남은 선택 과제 (Phase 4 범위 밖으로 명시적 분리)**
- `--auto-iptables`: 필터 표현식 → iptables 규칙 자동 번역 (4.3의 2차 구현)
- `.deb` / `.rpm` 패키징
- 리눅스용 Named Pipe 대체 IPC (현재는 HTTP API로 충분하다고 판단)
- IPv6 복제 / inbound 복제 지원 (raw 소켓 한계)

---

## 부록 A — 아키텍처 원칙 요약

1. **C-with-C++-컴파일러 우선**: 기존 로직은 그대로 두고 컴파일러만 전환. 신규 코드만 C++ 관용구 사용. ([Phase 1.4](#14-코드-스타일-원칙))
2. **직접 만들 수 있는 것과 없는 것 구분**: 커널 드라이버(WinDivert) 같은 영역은 대체 불가 — 신뢰 가능한 외부 라이브러리에 계속 의존. 로컬호스트 HTTP 서버처럼 범위가 좁은 것은 직접 구현이 오히려 저리스크. (Phase 2.4)
3. **오프라인 우선**: 웹 UI를 포함해 외부 CDN/서비스에 의존하지 않음 — clumsy의 기존 원칙(README: "오프라인 환경에서도 동작")과 일관성 유지. (Phase 2.7)
4. **로컬 우선 보안**: 기본값은 항상 로컬호스트 전용, 원격/외부 바인딩은 명시적 옵션 + 인증 필수. (Phase 2.8, 3.4)
5. **Module 구조체 아키텍처 유지**: `CLAUDE.md`에 정의된 모듈 확장 절차와의 호환성을 Phase 전체에서 유지.

## 부록 B — 참고 자료

- 아키텍처 개요: [CLAUDE.md](CLAUDE.md)
- 사용자 매뉴얼: [manual.md](manual.md)
- WinDivert 필터 문법: https://github.com/basil00/Divert/wiki/WinDivert-Documentation#7-filter-language
- libnetfilter_queue: https://www.netfilter.org/projects/libnetfilter_queue/
