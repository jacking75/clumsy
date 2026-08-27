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
- [ ] 1.1 빌드 설정 갱신 (msvc/clumsy.vcxproj: VS2026 툴셋, C++23)
- [ ] 1.2 소스 파일 확장자 전환 (.c → .cpp, genie.lua/vcxproj 패턴 갱신)
- [ ] 1.3 C→C++ 컴파일 오류 수정
- [ ] 1.4 코드 스타일 원칙 문서화 및 팀 공유
- [ ] 1.5 리눅스 툴체인 설정 준비 (gcc16, C++23 — genie.lua에 옵션만 추가, Phase 4에서 실사용)
- [ ] 1.6 회귀 테스트 및 완료 확인

### Phase 2 — IUP(GUI) 제거 → 콘솔 + 내장 웹서버
- [ ] 2.1 IUP 사용처 전수 조사 및 제거 계획 수립
- [ ] 2.2 Module 구조체 변경 (setupUIFunc/iconHandle 제거, ParamSpec 도입)
- [ ] 2.3 콘솔 출력 모드 전환 (Release도 ConsoleApp, LOG 매크로 통일)
- [ ] 2.4 HTTP 서버 코어 구현 (src/httpserver.cpp)
- [ ] 2.5 REST API 설계 및 구현 (pipe.c 로직 공용화)
- [ ] 2.6 실시간 스트리밍 (Server-Sent Events)
- [ ] 2.7 웹 프론트엔드 (정적 대시보드)
- [ ] 2.8 인증 (토큰 기반)
- [ ] 2.9 Named Pipe API 존치 및 리팩터링
- [ ] 2.10 완료 확인 (회귀 테스트)

### Phase 3 — 신규 기능 추가
- [ ] 3.1 pcap 익스포트
- [ ] 3.2 조건부/트리거 기반 시나리오 확장
- [ ] 3.3 세션 리포트 내보내기 (HTML)
- [ ] 3.4 원격 제어 강화 (분산 QA 환경)
- [ ] 3.5 웹 UI 비주얼 필터 빌더
- [ ] 3.6 플러그인형 커스텀 모듈 (선택 과제, 우선순위 낮음)
- [ ] 3.7 완료 확인 (문서화 포함)

### Phase 4 — 리눅스 지원 (최후순위)
- [ ] 4.1 캡처 백엔드 추상화 (PacketMeta 도입)
- [ ] 4.2 리눅스 캡처 백엔드 구현 (libnetfilter_queue)
- [ ] 4.3 필터 언어 계층 분리
- [ ] 4.4 권한 처리 (capability 기반)
- [ ] 4.5 빌드 시스템 (genie.lua 리눅스 네이티브 타겟, gcc16)
- [ ] 4.6 배포 문서화
- [ ] 4.7 완료 확인

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
- [ ] Debug/Release x64 MSVC 빌드 경고 0개로 성공
- [ ] 기존 MinGW(clang) 빌드가 기존과 동일하게 동작 확인
- [ ] 매뉴얼(manual.md)의 기본 시나리오(필터 적용 → lag/drop 등 모듈 동작 → 통계 확인)로 회귀 테스트 통과

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
- [ ] `external/iup-*` 참조가 `msvc/clumsy.vcxproj`, `genie.lua`, `README.md`, `CLAUDE.md`에서 모두 제거됨
- [ ] 브라우저 접속 → 필터 설정 → Start → 모듈 토글 → 실시간 통계 확인까지 전체 플로우 동작
- [ ] 기존 Named Pipe 자동화 스크립트가 수정 없이 동작(회귀 없음)
- [ ] 관리자 권한 없이 실행 시 웹 UI는 뜨되, 캡처 시작 시 명확한 에러 메시지 반환(`elevate.cpp` 로직과 연동)

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
- [ ] 각 기능은 독립적으로 온/오프 가능(다른 기능에 영향 없음)
- [ ] `manual.md`에 각 기능 사용법 추가
- [ ] 웹 API 엔드포인트 전체 목록을 `GET /api/docs` 또는 README에 문서화

---

## Phase 4 — 리눅스 지원 (최후순위)

### 4.1 캡처 백엔드 추상화
- `common.h`의 `PacketNode`에서 `WINDIVERT_ADDRESS addr` 필드를 제거하고, 플랫폼 중립 `PacketMeta`(방향, 인터페이스 인덱스, 타임스탬프, IP 버전 등 공통 필드만) 구조체로 교체.
- `divertStart`/`divertStop`/`divertConsumeStep`을 캡처 백엔드 인터페이스로 재정의(Windows는 기존 WinDivert 구현체, Linux는 4.2의 신규 구현체가 동일 인터페이스를 구현).
- 영향 파일(직접 grep 확인 완료 — WinDivert 타입을 직접 참조 중인 코어 외 파일): `packet.cpp`, `reset.cpp`, `tamper.cpp`, `procfilter.cpp`. 각 파일에서 WinDivert 전용 API 호출부를 `PacketMeta` 기반 공용 API 호출로 치환.

### 4.2 리눅스 캡처 백엔드 (libnetfilter_queue)
- 신규 `src/divert_linux.cpp`: `libnetfilter_queue` + `libmnl` 기반 NFQUEUE 콜백 구현. 커널 드라이버를 새로 작성/서명할 필요가 없습니다(넷필터가 커널에 이미 내장) — WinDivert보다 오히려 "미래에 사라질 리스크"가 낮은 성숙한 리눅스 표준 스택입니다.
- 큐 번호(`--queue-num`)를 CLI/웹 UI에서 지정 가능하게 하고, 프로그램 시작 시 필요한 `iptables`/`nft` NFQUEUE 규칙 예시를 콘솔에 안내 출력.

### 4.3 필터 언어 계층 분리
- WinDivert 필터 표현식은 그대로 유지하되, 리눅스에서는 이것이 "패킷 필터링"이 아니라 "어떤 패킷을 NFQUEUE로 보낼지"를 결정하는 iptables/nftables 규칙과 역할이 다르다는 점을 매뉴얼에 명확히 문서화합니다.
- **1차 구현(권장)**: 사용자가 직접 iptables 규칙으로 대상 트래픽을 NFQUEUE로 보내고, clumsy는 큐에서 받은 모든 패킷에 기존 필터 표현식(WinDivert 필터 문법 서브셋을 재구현)을 재적용.
- **2차 구현(후순위)**: `--auto-iptables` 지정 시 clumsy가 filter 표현식을 최소 iptables 규칙으로 번역해 자동 추가/제거(종료 시 정리). 구현 복잡도가 높아 1차 구현 이후 별도 과제로 분리.

### 4.4 권한 처리
- `elevate.cpp`의 UAC 관련 함수(`IsElevated`, `IsRunAsAdmin`, `tryElevate`)를 리눅스에서는 `geteuid() == 0` 체크 + `CAP_NET_ADMIN`/`CAP_NET_RAW` capability 확인(`libcap` 또는 `/proc/self/status`의 `CapEff` 파싱)으로 대체하는 조건부 구현 추가.

### 4.5 빌드 시스템 (gcc16, C++23)
- `genie.lua`에 리눅스 네이티브 타겟 블록 추가(`_ACTION == 'gmake'`이면서 리눅스에서 실행되는 경우를 위한 별도 분기 — 기존 `MINGW_ACTION` 블록은 Windows용 MinGW+clang 전용이므로 건드리지 않음).
- 컴파일러를 `g++-16`으로 지정, `buildoptions({'--std=c++23'})` 적용(Phase 1.5에서 준비해둔 설정을 여기서 실제 연결).
- 링크 대상을 `libnetfilter_queue`/`libmnl`로 변경(Windows 전용 `comctl32`/`Winmm`/`ws2_32`/`iphlpapi` 등은 조건부 제외).

### 4.6 배포 문서화
- 최소 요구 패키지 정리(`libnetfilter-queue-dev`, `libmnl-dev` 등 배포판별 패키지명).
- `.deb`/`.rpm` 패키징은 우선순위 낮은 선택 과제로 Phase 4 완료 후 별도 분리.

### 4.7 완료 기준
- [ ] 리눅스에서 `--queue-num N` + 사용자가 건 iptables 규칙으로 lag/drop 등 기본 모듈이 Windows와 동일하게 동작
- [ ] 웹 UI가 리눅스에서도 동일하게 접속/제어 가능(Phase 2 산출물 재사용 확인)
- [ ] `README.md`/`manual.md`에 리눅스 설치/실행 가이드 추가

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
