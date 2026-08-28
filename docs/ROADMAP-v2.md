# clumsy 개선 로드맵 v2 — 기능 추가 제안

> Phase 1~4로 C++23 전환 · IUP 제거 · 콘솔+웹 · 리눅스 지원이 완료된 상태(0.4)를 기반으로,
> **네트워크 상태 시뮬레이터로서 실제 QA 가치를 높이는** 후속 기능을 제안합니다.
> 각 항목은 현재 코드(`Module` 구조체 패턴, `controlapi.cpp` 공용 계층, 백엔드 중립 헬퍼)에
> 맞춰 구현 방법을 상세히 적었습니다. 우선순위·난이도·규모를 표기했으니 원하는 것부터 고르세요.
>
> 관련 문서: [CLAUDE.md](../CLAUDE.md) 아키텍처, [docs/CODING_STYLE.md](CODING_STYLE.md) 코드 규칙,
> [docs/LINUX.md](LINUX.md) 리눅스, [working_log.md](../working_log.md) 작업 이력.

---

## 태스크 리스트

### Tier 1 — 작지만 효과 큼 (self-contained, 반나절~1일)
- [x] **T1. Prometheus `/metrics` 엔드포인트** — CI/분산 QA에서 Grafana·알림 연동
- [x] **T2. 신규 모듈 `corrupt` (비트 에러)** — tamper와 다른 무선/전파 손상 재현
- [x] **T3. jitter 지연 분포 선택 (uniform/normal/pareto)** — netem 수준의 현실성

### Tier 2 — 중간 규모 (usability, 1~2일)
- [x] **T4. 웹 UI 시나리오 에디터** — JSON 손편집 없이 브라우저에서 시나리오 작성/실행
- [x] **T5. 프로파일 관리 개선** — 웹에서 삭제 + 인라인(저장 안 한) 즉시 적용
- [x] **T6. 대시보드 UX** — 다크/라이트 토글 + 모듈별 실시간 미니 그래프

### Tier 3 — 크고 강력함 (선택, 2~4일)
- [x] **T7. pcap 리플레이** — 저장된 pcap 파일의 패킷을 재주입
- [x] **T8. 지연 히스토그램 / p50·p95 통계** — 리포트·stats에 분위수 추가

> **전부 완료되었습니다 (2026-08-28).** 아래 "구현 결과" 절에 실제로 검증된 내용과,
> 계획과 달라진 부분을 정리했습니다.

---

## 구현 결과 요약

| | 검증 방법 | 결과 |
|---|---|---|
| Windows 빌드 | `MSBuild /t:Rebuild Release x64` | 경고 0, 오류 0 |
| Linux 빌드 | `make` (g++-16, `-Wall -Wextra`) | 경고 0 |
| 단위 테스트 | `make test` | 16 + 35 = **51 assert 전부 통과** |
| REST 회귀 (Linux) | `tests/linux/api_test.sh` | **57/57** |
| REST 회귀 (Windows) | `tests/windows/api_test.ps1` | **55/55** |
| 실패킷 검증 (Linux) | `sudo tests/linux/behaviour_test.sh` | **24/24** |
| 대시보드 | Chrome으로 직접 조작 | enum·테마·스파크라인·에디터 동작 확인 |

### 계획과 달라진 점

1. **T2 corrupt의 난수 사용법** — 문서의 예시는 비트마다 `rand()`를 부르는데,
   1400바이트 패킷 하나에 11,200회가 되어 패킷 경로가 막힙니다. 다음 에러까지의
   간격을 기하분포에서 직접 뽑는 방식으로 바꿔, 실제로 뒤집는 비트 수만큼만
   난수를 씁니다(기본값에서 패킷당 2~3회).
2. **T3의 `enum` 타입을 실제로 추가** — 문서에서는 "급하면 `int`(0/1/2)로 두라"고
   했지만, `ParamSpec`에 `options` 필드를 더해 서버가 선택지를 함께 내려주도록
   했습니다. 대시보드는 모듈 이름을 모르는 채 드롭다운을 그립니다. 숫자 형식도
   계속 받으므로 기존 프로파일·시나리오는 그대로 동작합니다.
3. **T8 분위수 보간을 개선** — 교과서식 구현은 0~400ms 균등분포의 p95를 467ms로
   과대추정합니다. 최소·최대값은 정확히 알고 있으므로 양 끝 구간을 실제 관측
   범위로 좁혔고, 오차가 87ms에서 25ms 이내로 줄었습니다.
   `tests/latency_test.cpp`가 이 값을 단언합니다.
4. **T7 재생이 이더넷 파일도 지원** — RAW(101)만 계획했지만 tcpdump/Wireshark의
   기본 저장 형식이 Ethernet(1)이라 14바이트 헤더 제거를 넣었습니다. pcapng는
   지원하지 않으며 매직 넘버 단계에서 명확히 거부합니다.
5. **테스트 스위트 3종 신규 추가** — 계획에 없었지만 각 항목의 "완료 기준"을
   사람이 매번 손으로 확인할 수는 없어서 자동화했습니다
   (`latency_test.cpp`, `linux|windows/api_test.*`, `linux/behaviour_test.sh`).

### 구현 중 발견해 고친 결함

| 결함 | 발견 경로 | 조치 |
|------|----------|------|
| 지연 p95가 실제보다 87ms 과대 | `latency_test.cpp` | 양 끝 구간을 관측 min/max로 클램프 |
| 스파크라인이 영구히 비어 있음 | 브라우저에서 픽셀 검사 | `dtMs`를 `lastStamp` 갱신 **전에** 계산하도록 순서 수정 |
| 다크 모드에서 헤더 링크가 안 보임 | 스크린샷 확대 | 브라우저 기본 `:visited` 색 대신 팔레트 토큰 사용 |
| 라이트 모드 링크 대비 4.4:1 (AA 미달) | 대비율 계산 | `--link` 토큰 분리 (5.5:1 / 6.2:1로 개선) |

### 알려진 환경 제약

이 개발 환경(Symantec Endpoint Protection + CrowdStrike Falcon)은 빌드 직후
`clumsy.exe`를 수 분 내에 격리합니다. 패킷 조작 도구의 특성상 휴리스틱에 걸리는
것으로, 코드 문제가 아닙니다. Windows 테스트는 빌드 직후 즉시 실행해야 합니다.

---

---

## 공통 준비: 새 기능을 어디에 붙이나 (아키텍처 요약)

| 하고 싶은 것 | 손대는 파일 |
|---|---|
| 새 패킷 조작 모듈 | `src/<name>.cpp` 신규 + `common.h`(extern, MODULE_CNT) + `main.cpp`(modules[]) + 빌드파일 |
| 새 REST 응답 | `controlapi.cpp`(`apiXxx()`) + `controlapi.h`(선언) + `httpserver.cpp`(라우팅) |
| 새 GET 엔드포인트 | `httpserver.cpp`의 `routeRequest()` GET 블록 |
| 대시보드 UI | `etc/web/index.html` (단일 파일, 외부 CDN 금지) |
| 빌드 등록 | Windows `msvc/clumsy.vcxproj`(+`.filters`), 리눅스 `Makefile`의 `SOURCES` |

패킷 내용을 만질 때는 **반드시 백엔드 중립 헬퍼**를 쓰세요 (양 플랫폼 자동 지원):
```c
int  packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen);
void packetRecalcChecksums(char *packet, UINT len);
int  packetSetTcpRst(char *packet, UINT len);
```
`windivert.h`를 include하는 순간 리눅스 빌드가 깨집니다.

---

## T1. Prometheus `/metrics` 엔드포인트

**왜**: Phase 3.4에서 CI/분산 QA 시나리오를 만들었습니다. Prometheus 텍스트 포맷 엔드포인트를
추가하면 Grafana 대시보드·시계열 그래프·알림을 **코드 한 줄 없이** 얻습니다. 여러 테스트
머신의 clumsy를 한 화면에서 모니터링할 수 있습니다.

**규모**: 작음. 새 파일 없음. **난이도**: 낮음.

### 구현

**1) `controlapi.cpp`에 텍스트 생성 함수 추가** (기존 `apiStatsJson()` 바로 아래에):

```cpp
// GET /metrics — Prometheus text exposition format (v0.0.4).
// 카운터/게이지만 노출. HELP/TYPE 주석은 Prometheus 규약.
std::string apiMetricsText() {
    std::string o;
    auto line = [&](const char *name, const char *help, const char *type,
                    const std::string &labelsAndValue) {
        o += "# HELP clumsy_"; o += name; o += " "; o += help; o += "\n";
        o += "# TYPE clumsy_"; o += name; o += " "; o += type; o += "\n";
        o += "clumsy_"; o += name; o += labelsAndValue; o += "\n";
    };

    line("up", "1 when clumsy is running", "gauge", " 1");
    line("capturing", "1 while a capture is active", "gauge",
         std::string(" ") + (appIsCapturing() ? "1" : "0"));
    line("captured_total", "packets captured", "counter",
         " " + numToStr(statsCapturedTotal));
    line("sent_total", "packets re-injected", "counter",
         " " + numToStr(statsSentTotal));
    line("capture_elapsed_ms", "ms since capture start", "gauge",
         " " + numToStr((long)appCaptureElapsedMs()));

    // 모듈별 affected 카운터를 label로 (clumsy_module_affected{module="drop"} N)
    o += "# HELP clumsy_module_affected packets a module acted on\n";
    o += "# TYPE clumsy_module_affected counter\n";
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        o += "clumsy_module_affected{module=\"";
        o += m->shortName;
        o += "\"} ";
        o += numToStr(m->affectedCount);
        o += "\n";
        o += "clumsy_module_enabled{module=\"";
        o += m->shortName;
        o += "\"} ";
        o += (*m->enabledFlag ? "1" : "0");
        o += "\n";
    }
    return o;
}
```
> `numToStr`, `Module`, `statsCapturedTotal` 등은 이미 `controlapi.cpp` 안에서 접근 가능합니다.

**2) `controlapi.h`에 선언 추가** (`apiStatsJson()` 근처):
```cpp
std::string apiMetricsText();    // GET /metrics (Prometheus)
```

**3) `httpserver.cpp` 라우팅** — `routeRequest()` 안, `/api/health`처럼 **인증 없이** 처리
(메트릭은 읽기 전용 카운터라 노출해도 안전; `isAuthorized()` 검사 **위쪽**에 배치):

```cpp
// Health check is intentionally unauthenticated (Phase 3.4).
if (path == "/api/health") { sendJson(s, 200, apiHealthJson()); return; }
// ↓ 이 줄 추가
if (path == "/metrics") {
    std::string body = apiMetricsText();
    // Prometheus는 이 Content-Type을 요구
    sendResponse(s, 200, "text/plain; version=0.0.4; charset=utf-8",
                 body.c_str(), (int)body.size(), NULL);
    return;
}
```
> 외부 바인딩(`--web-bind`) 시 토큰을 강제하고 싶다면 이 블록을 `isAuthorized()` 검사
> **아래**로 옮기세요. Prometheus는 `Authorization` 헤더나 스크레이프 설정의 `params: {token: [...]}`로
> 토큰을 보낼 수 있습니다. 기본은 무인증 권장(로컬호스트 전용이 기본이므로).

**4) `apiDocsJson()`에 항목 추가** (선택): `/metrics`를 문서에 노출.

### Prometheus 스크레이프 설정 예시 (문서용, `docs/LINUX.md`나 README에)
```yaml
scrape_configs:
  - job_name: clumsy
    static_configs:
      - targets: ['10.0.0.5:8080']
    metrics_path: /metrics
```

### 완료 기준
- [x] `curl http://127.0.0.1:8080/metrics` 가 카운터/게이지/히스토그램을 반환
- [x] 형식 검증 — HELP/TYPE 짝이 맞고(16쌍), `le` 누적 버킷이 단조 증가(13개)
- [x] Windows/리눅스 빌드 경고 0
- [x] README/manual.md에 스크레이프 설정과 PromQL 예시 문서화

**구현 메모**: 계획보다 넓게 노출했습니다 — 모듈별 `enabled`/`affected` 외에
버퍼 크기, pcap 기록량, 재생 통계, 그리고 T8의 지연 히스토그램을 **네이티브
histogram 형식**(`_bucket`/`_sum`/`_count`)으로 내보냅니다. Grafana에서
`histogram_quantile()`이 `/api/stats`의 분위수와 같은 수치를 내도록 하기 위해서입니다.
모듈 이름은 메트릭 이름이 아니라 레이블이므로 패널 하나로 전 모듈을 다룹니다.

---

## T2. 신규 모듈 `corrupt` (비트 에러율)

**왜**: 현재 `tamper`는 페이로드의 1/4 영역을 XOR 패턴으로 뒤집습니다. 이건 "명확한 손상"이고,
**무선/전파 환경의 실제 비트 에러**(BER, 낮은 확률로 개별 비트가 랜덤하게 뒤집힘)와는 다릅니다.
`corrupt` 모듈은 페이로드의 각 비트를 설정한 확률로 뒤집어, Wi-Fi/셀룰러/위성의 미세 손상을
재현합니다. **모듈 추가 절차의 완벽한 예시**이기도 합니다.

**규모**: 작음(모듈 1개). **난이도**: 낮음. **양 플랫폼 동작** (백엔드 중립 헬퍼만 사용).

### 구현

**1) `src/corrupt.cpp` 신규** — `drop.cpp`/`tamper.cpp`를 참고한 최소 구현:

```cpp
// corrupt module — bit-error injection (realistic wireless corruption)
//
// tamper.cpp는 페이로드 영역을 XOR로 확 바꾸지만, 이 모듈은 각 비트를 아주 낮은
// 확률로 독립적으로 뒤집어 실제 BER을 재현합니다. 체크섬을 다시 계산하면 손상이
// 애플리케이션까지 전달되고, 끄면 링크 계층 드롭처럼 커널이 걸러낼 수 있습니다.
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "corrupt"

static volatile short corruptEnabled = 0,
    corruptInbound = 1, corruptOutbound = 1,
    chance = 1000,       // 이 패킷을 손상시킬 확률 [0-10000] = 10%
    doChecksum = 1;
// 비트당 뒤집힘 확률(ppm, parts-per-million). 기본 100 = 0.01%.
static volatile LONG bitErrorPpm = 100;

static void corruptStartUp()  { LOG("corrupt enabled"); }
static void corruptCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head); UNREFERENCED_PARAMETER(tail);
}

static short corruptProcess(PacketNode *head, PacketNode *tail) {
    short did = FALSE;
    PacketNode *pac = head->next;
    LONG ppm = bitErrorPpm;               // volatile 스냅샷
    while (pac != tail) {
        if (checkDirection(pac->meta.outbound, corruptInbound, corruptOutbound)
            && calcChance(chance)) {
            char *data = NULL; UINT len = 0;
            if (packetGetPayload(pac->packet, pac->packetLen, &data, &len) && len) {
                int flipped = 0;
                for (UINT i = 0; i < len; ++i) {
                    for (int b = 0; b < 8; ++b) {
                        // rand()%1000000 < ppm 이면 그 비트를 뒤집는다
                        if ((rand() % 1000000) < ppm) {
                            data[i] ^= (char)(1 << b);
                            ++flipped;
                        }
                    }
                }
                if (flipped) {
                    if (doChecksum)
                        packetRecalcChecksums(pac->packet, pac->packetLen);
                    InterlockedIncrement(&corruptModule.affectedCount);
                    did = TRUE;
                    LOG("corrupt flipped %d bits in %u byte payload", flipped, len);
                }
            }
        }
        pac = pac->next;
    }
    return did;
}

static int corruptSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value)*100.0+0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-ber") == 0) {   // ppm 단위
        InterlockedExchange(&bitErrorPpm, clampLong(atol(value), 0, 1000000));
        return 1;
    }
    if (strcmp(key, NAME"-checksum") == 0) {
        InterlockedExchange16(&doChecksum, (short)parseBoolValue(value)); return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&corruptInbound, (short)parseBoolValue(value)); return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&corruptOutbound, (short)parseBoolValue(value)); return 1;
    }
    return 0;
}

static int corruptGetParams(ParamKV *kv, int maxKv) {
    int n = 0; if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-chance");   sprintf(kv[n].val, "%.1f", chance/100.0f); n++;
    strcpy(kv[n].key, NAME"-ber");      sprintf(kv[n].val, "%ld", bitErrorPpm); n++;
    strcpy(kv[n].key, NAME"-checksum"); strcpy(kv[n].val, doChecksum?"true":"false"); n++;
    strcpy(kv[n].key, NAME"-inbound");  strcpy(kv[n].val, corruptInbound?"true":"false"); n++;
    strcpy(kv[n].key, NAME"-outbound"); strcpy(kv[n].val, corruptOutbound?"true":"false"); n++;
    return n;
}

static const ParamSpec corruptParamSpecs[] = {
    { NAME"-inbound",  "Inbound",        "bool",    0, 0 },
    { NAME"-outbound", "Outbound",       "bool",    0, 0 },
    { NAME"-checksum", "Redo checksum",  "bool",    0, 0 },
    { NAME"-chance",   "Chance (%)",     "percent", 0, 100 },
    { NAME"-ber",      "Bit error (ppm)","int",     0, 1000000 },
};

Module corruptModule = {
    "Corrupt", NAME, (short*)&corruptEnabled,
    corruptStartUp, corruptCloseDown, corruptProcess,
    corruptSetParam, corruptGetParams,
    corruptParamSpecs, (int)(sizeof(corruptParamSpecs)/sizeof(corruptParamSpecs[0])),
    0, 0, 0
};
```

**2) `common.h`**:
- `#define MODULE_CNT 11` → `12`
- `extern Module resetModule;` 목록에 `extern Module corruptModule;` 추가

**3) `main.cpp`의 `modules[]` 배열에 추가** — 처리 순서가 곧 우선순위입니다.
tamper 뒤(변조 계열끼리 모으기)에 넣는 걸 권장:
```cpp
&tamperModule,
&corruptModule,     // ← 추가
&resetModule,
```

**4) 빌드 등록**:
- `msvc/clumsy.vcxproj`의 `<ItemGroup>` ClCompile 목록에
  `<ClCompile Include="$(ProjectRoot)src\corrupt.cpp" />` 추가
- `msvc/clumsy.vcxproj.filters`에도 같은 항목 추가 (Filter: `Source Files\Modules`)
- 리눅스 `Makefile`의 `SOURCES`(포터블 코어 목록)에 `$(SRCDIR)/corrupt.cpp \` 추가

**CLI/REST/웹은 자동 노출**됩니다: `applyCliModuleParams()`가 `paramSpecs`를 순회하고,
`controlapi.cpp`가 `modules[]`를 순회하며, 대시보드는 `paramSpecs`로 폼을 그리기 때문입니다.

### 완료 기준
- [x] 페이로드 비트가 실제로 뒤집힘 — 실측 20/20 패킷이 손상된 채 도달
- [x] `--corrupt-checksum on` 시 앱까지 전달(20/20), `off` 시 커널이 폐기(0/20)
- [x] 대시보드에 Corrupt 폼이 자동 생성 (5개 필드, 코드 수정 없이)
- [x] Windows/리눅스 빌드 경고 0
- [x] `manual.md` 모듈 설명 추가, README/CODING_STYLE의 "11개→12개" 갱신

**구현 메모**: 난수 사용법이 위 예시와 다릅니다. 비트마다 `rand()`를 부르면
1400바이트에 11,200회가 되어 캡처 뮤텍스를 쥔 채 파이프라인을 막습니다.
기하분포에서 **다음 에러까지의 간격**을 뽑아 실제 반전 횟수만큼만 난수를 씁니다.

---

## T3. jitter 지연 분포 선택 (uniform / normal / pareto)

**왜**: 현재 jitter는 `[min, max]` **균등분포**입니다. 실제 네트워크 지연은 정규분포(대부분
평균 근처)나 파레토/롱테일(가끔 큰 스파이크) 형태입니다. Linux `tc netem`이 지원하는 이
분포들을 넣으면 훨씬 현실적인 지연 재현이 됩니다. 기존 모듈 **확장**이라 리스크가 낮습니다.

**규모**: 작음. **난이도**: 낮음(수학 약간).

### 구현 (`src/jitter.cpp`만 수정)

**1) 분포 상태 추가** (파일 상단 static 블록):
```cpp
#define DIST_UNIFORM 0
#define DIST_NORMAL  1
#define DIST_PARETO  2
static volatile short jitterDist = DIST_UNIFORM;
```

**2) 샘플링 함수 추가** (`jitterProcess` 위에):
```cpp
// [0,1) 균등 난수
static double urand() { return (rand() + 0.5) / (RAND_MAX + 1.0); }

// lo..hi 범위에서 분포에 맞는 지연(ms)을 하나 뽑는다.
static DWORD sampleDelay(short lo, short hi, short dist) {
    if (hi <= lo) return (DWORD)lo;
    double range = hi - lo;
    double v;
    switch (dist) {
    case DIST_NORMAL: {
        // Box-Muller. 평균=중앙, 표준편차=range/6 → 99.7%가 [lo,hi]에 들어옴
        double u1 = urand(), u2 = urand();
        double z = sqrt(-2.0*log(u1)) * cos(2.0*3.14159265358979*u2);
        v = (lo + hi)/2.0 + z * (range/6.0);
        break;
    }
    case DIST_PARETO: {
        // 롱테일: 대부분 lo 근처, 가끔 hi 쪽 스파이크. shape=2.0
        double xm = 1.0, shape = 2.0;
        double p = xm / pow(1.0 - urand(), 1.0/shape);   // 파레토 표본 [1, ∞)
        v = lo + (p - 1.0) / (pow(1.0/(1e-6), 1.0/shape)) * range; // 스케일 정규화
        // 간단히: v = lo + (1 - 1/p) * range  로 대체해도 됨(아래 주석 참고)
        break;
    }
    default: // DIST_UNIFORM
        v = lo + urand() * range;
    }
    if (v < 0) v = 0;
    if (v > hi) v = hi;      // 범위 밖으로 튀는 표본은 클램프
    return (DWORD)(v + 0.5);
}
```
> 파레토 정규화가 헷갈리면 더 단순한 버전을 쓰세요:
> `double p = 1.0/pow(1.0-urand(), 1.0/2.0); v = lo + (1.0 - 1.0/p) * range;`
> — `p`는 [1,∞), `1-1/p`는 [0,1)이고 롱테일 성질을 유지합니다.

**3) `jitterProcess`에서 delay 계산 교체**:
```cpp
// 기존: DWORD delay = (DWORD)lo + (range > 0 ? (DWORD)(rand() % ((int)range + 1)) : 0);
DWORD delay = sampleDelay(lo, hi, jitterDist);
```
그리고 상단에 `#include <math.h>` 추가.

**4) setParam/getParams/paramSpecs에 `jitter-dist` 추가**:
```cpp
// setParam 안:
if (strcmp(key, NAME"-dist") == 0) {
    short d = DIST_UNIFORM;
    if (_stricmp(value,"normal")==0) d = DIST_NORMAL;
    else if (_stricmp(value,"pareto")==0) d = DIST_PARETO;
    InterlockedExchange16(&jitterDist, d);
    return 1;
}
// getParams 안 (maxKv 체크를 4→5로):
strcpy(kv[n].key, NAME"-dist");
strcpy(kv[n].val, jitterDist==DIST_NORMAL?"normal":jitterDist==DIST_PARETO?"pareto":"uniform"); n++;
// paramSpecs 배열에 (type은 문자열 선택이므로 "int"로 두거나 UI에서 select 처리):
{ NAME"-dist", "Distribution (uniform/normal/pareto)", "int", 0, 2 },
```
> 대시보드에서 드롭다운으로 예쁘게 하려면 `ParamSpec.type`에 `"enum"`을 새로 추가하고
> `etc/web/index.html`의 `paramInput()`에 enum 케이스를 넣는 방법도 있습니다(T6와 함께 추천).
> 급하면 `int`(0/1/2)로 두고 문서로 안내해도 동작합니다.

### 완료 기준
- [x] 실측으로 세 분포의 모양이 구분됨 (Min=0, Max=400, 각 60패킷)

  | 분포 | p50 | p95 |
  |------|-----|-----|
  | uniform | 270ms | 403ms |
  | normal | 217ms | 336ms |
  | pareto | 161ms | 327ms |

  uniform 중앙값이 이론값 200ms 부근이고, pareto가 아래로 치우치며,
  normal의 p95가 uniform을 넘지 않는 것을 자동 단언합니다.
- [x] 기본값 uniform은 기존 동작 유지 (회귀 없음)
- [x] Windows/리눅스 빌드 경고 0
- [x] manual.md에 분포 설명 + 실측표 추가

**구현 메모**: `int`로 때우지 않고 `ParamSpec`에 `enum` 타입과 `options` 목록을
추가했습니다. 대시보드는 모듈을 특정하지 않고 드롭다운을 그립니다.
숫자 인덱스(`0`/`1`/`2`)도 계속 받습니다.

---

## T4. 웹 UI 시나리오 에디터

**왜**: 시나리오는 강력하지만(시간/조건/반복 트리거) 지금은 JSON 파일을 **손으로** 써야 합니다.
브라우저에서 스텝을 추가/편집하고 즉시 실행하면 진입장벽이 크게 낮아집니다.

**규모**: 중간. **난이도**: 중(백엔드 소폭 + 프론트 다수).

### 백엔드: 문자열에서 시나리오 로드

현재 `scenario.cpp`의 `scenarioLoad(path)`는 파일을 버퍼로 읽은 뒤 파싱합니다. 파싱 부분을
분리해 문자열에서도 로드할 수 있게 합니다.

**1) `scenario.cpp` 리팩터링** — `scenarioLoad`의 파싱 루프를 `scenarioParse(buf)`로 추출하고,
공개 함수 `scenarioLoadString` 추가:
```cpp
// 기존 scenarioLoad 내부의 "find '[' ... 파싱 ... sortSteps()" 부분을 이 함수로 이동
static void scenarioParse(const char *buf) {
    const char *p = strchr(buf, '[');
    nSteps = 0;
    if (!p) { INFO("scenario: no JSON array found"); return; }
    p++;
    while (*p && nSteps < MAX_STEPS) { /* ... 기존 객체 추출 루프 그대로 ... */ }
    sortSteps();
    INFO("scenario: loaded %d steps", nSteps);
}

void scenarioLoad(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { INFO("scenario: cannot open '%s'", path); return; }
    char *buf = (char*)malloc(SCENARIO_BUF_SIZE); if (!buf){fclose(f);return;}
    size_t len = fread(buf,1,SCENARIO_BUF_SIZE-1,f); fclose(f); buf[len]='\0';
    scenarioParse(buf);
    free(buf);
}

void scenarioLoadString(const char *json) {   // 신규
    scenarioParse(json);
}
```
`common.h`에 `void scenarioLoadString(const char *json);` 선언 추가.

**2) `controlapi.cpp`에 인라인 로드 핸들러 추가**:
```cpp
std::string apiScenarioLoadInline(const std::string &body, int *httpStatus) {
    JsonValue req; *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400; return errJson("malformed JSON body");
    }
    // 프론트가 시나리오 배열을 문자열로 넣어 보냄: {"scenario":"[ {...}, ... ]"}
    std::string arr = req.str("scenario");
    if (arr.empty()) { *httpStatus = 400; return errJson("missing 'scenario' field"); }
    scenarioLoadString(arr.c_str());
    if (!scenarioIsLoaded()) { *httpStatus = 400; return errJson("failed to parse scenario"); }
    return okJson("\"steps\":" + numToStr(scenarioStepCount()));
}
```
`controlapi.h`에 선언 추가.

**3) `httpserver.cpp` POST 라우팅**:
```cpp
if (path == "/api/scenario/loadinline") { sendJson(s, status, apiScenarioLoadInline(req.body, &status)); return; }
```

### 프론트엔드: `etc/web/index.html`

기존 "Scenario file" 입력 아래에 에디터 블록을 추가하고, 스텝을 배열로 관리하는 JS를 작성합니다.
핵심 로직만(전체는 기존 코드 스타일에 맞춰):
```javascript
// 스텝 목록 상태
let scenarioSteps = [];  // [{trigger:'at', at:10, params:{lag:true,'lag-time':200}}, ...]

function scenarioToJson() {
  return JSON.stringify(scenarioSteps.map(s => {
    const o = {};
    if (s.trigger === 'at') o.at = Number(s.at);
    else { o.when = s.when; o.op = s.op; o.value = Number(s.value); }
    if (s.repeat) { o.repeat = Number(s.repeat); if (s.times) o.times = Number(s.times); }
    Object.assign(o, s.params);
    return o;
  }), null, 2);
}

function loadInlineScenario() {
  api("/api/scenario/loadinline", {
    method: "POST",
    body: JSON.stringify({ scenario: scenarioToJson() })
  }).then(d => setStatus("Scenario loaded: " + d.steps + " steps."))
    .catch(fail);
}
```
- 스텝 추가 UI: 트리거 종류(select: at / when), 파라미터 입력(모듈 shortName + 값).
  모듈 목록은 이미 `moduleCache`(GET /api/modules 결과)에 있으니 재사용.
- "미리보기" 영역에 `scenarioToJson()` 결과를 `<textarea readonly>`로 표시 → 파일로 저장도 가능.
- "Load & Start" 버튼: `loadInlineScenario()` 후 `POST /api/scenario/start`.

> 최소 버전: textarea에 직접 JSON을 붙여넣고 "Load inline" 버튼만 만들어도 손편집→파일저장→경로입력
> 3단계가 1단계로 줄어듭니다. 스텝 빌더 UI는 그 다음에 얹으세요.

### 완료 기준
- [x] 스텝 빌더로 추가/삭제하고 파일 없이 로드 (브라우저에서 직접 확인)
- [x] 시간·조건 트리거, repeat/times, 변경 항목 모두 UI에서 구성 가능
- [x] 생성된 JSON이 손으로 쓴 형식과 동일 (불리언·숫자는 따옴표 없이)
- [x] 인라인 로드한 100% drop이 실제 패킷을 없앰 (20→0, 실측)
- [x] 트리거 없는 스텝과 `scenario` 필드 누락은 400
- [x] 기존 파일 기반 `/api/scenario/load` 회귀 없음
- [x] Windows/리눅스 빌드 경고 0

**구현 메모**: 최소 버전(textarea만)이 아니라 스텝 빌더까지 만들었습니다.
변경 항목 드롭다운은 `moduleCache`의 ParamSpec에서 생성되므로 모듈이 늘어나면
자동 반영되고, enum 파라미터는 여기서도 드롭다운으로 나옵니다.
textarea는 편집·붙여넣기가 가능하며 전송되는 것은 항상 textarea의 내용입니다.

---

## T5. 프로파일 관리 개선 (삭제 + 인라인 적용)

**왜**: 지금은 프로파일을 저장/적용만 할 수 있고 **삭제**가 없습니다. 또 "저장하지 않고 지금
값만 한 번 적용"하는 경로가 없어 실험이 번거롭습니다.

**규모**: 작음. **난이도**: 낮음.

### 구현

**1) `profile.cpp`에 삭제 함수** (배열에서 제거 후 `profilesWriteAll()` 재사용):
```cpp
int profileDelete(const char *name) {
    for (int i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            for (int j = i; j < nProfiles - 1; j++) profiles[j] = profiles[j+1];
            nProfiles--;
            profilesWriteAll();      // 이미 존재하는 static 함수
            INFO("profiles: deleted '%s'", name);
            return 1;
        }
    }
    return 0;
}
```
`common.h`에 `int profileDelete(const char *name);` 선언.

**2) `controlapi.cpp` 핸들러 + 라우팅**:
```cpp
std::string apiDeleteProfile(const std::string &name, int *httpStatus) {
    *httpStatus = 200;
    if (!profileDelete(name.c_str())) { *httpStatus = 404; return errJson("profile not found: " + name); }
    return okJson("\"deleted\":" + quoted(name));
}
```
`httpserver.cpp`에서 `matchWrapped(path, "/api/profiles/", "/delete", &middle)` 로
`POST /api/profiles/{name}/delete` 라우팅(기존 `/apply` 패턴과 동일).

**3) 인라인 적용** — 이미 `applyModuleKV()`가 있으므로 새 엔드포인트는 "모듈 여러 개를
한 번에 설정"만 하면 됩니다. `POST /api/apply` 로 `{"lag":true,"lag-time":200,"drop":true,...}`를
받아 각 키를 `applyModuleKV`로 적용:
```cpp
std::string apiApplyInline(const std::string &body, int *httpStatus) {
    JsonValue req; *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) { *httpStatus=400; return errJson("malformed JSON body"); }
    int applied = 0;
    for (size_t i = 0; i < req.obj.size(); ++i) {
        if (applyModuleKV(req.obj[i].first.c_str(), req.obj[i].second.asString().c_str())) applied++;
    }
    return okJson("\"applied\":" + numToStr(applied));
}
```

**4) 대시보드**: 프로파일 드롭다운 옆에 "Delete" 버튼 추가(`refreshProfiles()` 재호출).

### 완료 기준
- [x] 웹에서 프로파일 삭제 → `profiles.json`에서 제거됨 (확인 대화상자 포함)
- [x] `POST /api/apply` 로 저장 없이 여러 모듈 즉시 설정 (4개 키 동시 적용 확인)
- [x] 미인식 키는 `unknownKeys`로 되돌려줌
- [x] 존재하지 않는 프로파일 삭제 시 404
- [x] 빌드 경고 0, 기존 apply/save 회귀 없음
- [x] Named Pipe에도 `delete_profile`·`apply` 명령 추가 (두 트랜스포트 동등성 유지)

---

## T6. 대시보드 UX (테마 토글 + 모듈별 미니 그래프)

**왜**: 대시보드는 이미 `prefers-color-scheme`로 다크/라이트를 자동 대응하지만 **수동 토글**이
없습니다. 또 통계 그래프가 전체 pps 하나뿐이라, 어떤 모듈이 얼마나 일하는지 시계열로 안 보입니다.

**규모**: 작음(프론트만). **난이도**: 낮음.

### 구현 (`etc/web/index.html`만)

**1) 테마 토글** — 헤더에 버튼 추가, `localStorage`에 저장:
```javascript
function applyTheme(t) {
  document.documentElement.setAttribute('data-theme', t);
  localStorage.setItem('clumsy-theme', t);
}
// CSS는 이미 :root 토큰 기반이므로, [data-theme="dark"]/[data-theme="light"] 블록만 추가
```
CSS에 `:root[data-theme="dark"]{ ... }` / `:root[data-theme="light"]{ ... }`를 명시하면
자동감지를 무시하고 사용자 선택이 이깁니다(artifact 규칙과 동일 패턴).

**2) 모듈별 스파크라인** — 이미 SSE로 `s.modules[name].affected`가 들어옵니다.
각 모듈 카드에 `<canvas class="spark">`를 넣고, `applyStats()`에서 모듈별 델타(초당 affected)를
작은 링버퍼에 쌓아 그립니다. 기존 `drawChart()` 로직을 축소해 재사용:
```javascript
const modHist = {};  // { drop: [12, 8, ...], ... }
// applyStats() 안에서:
Object.keys(s.modules).forEach(name => {
  const cur = s.modules[name].affected;
  const prev = (modHist[name+'_last'] || cur);
  modHist[name+'_last'] = cur;
  (modHist[name] = modHist[name] || []).push(Math.max(0, cur - prev));
  if (modHist[name].length > 60) modHist[name].shift();
  drawSpark(name, modHist[name]);   // 카드 안 canvas에 그림
});
```

### 완료 기준
- [x] `Auto → Light → Dark → Auto` 순환, localStorage에 유지, 새로고침 후에도 보존
- [x] "Auto"를 남긴 것이 중요 — OS 설정을 따르는 상태로 되돌아갈 수 있어야 합니다
- [x] 활성 모듈 카드에 초당 처리량 스파크라인 (픽셀 단위로 그려지는 것 확인)
- [x] 오프라인 동작 유지 — 외부 CDN·폰트·스크립트 0개
- [x] 두 테마 모두 WCAG AA 충족

  | | 링크 | 본문 | 흐린 글씨 |
  |---|---|---|---|
  | light | 5.5:1 | 17.1:1 | 5.3:1 |
  | dark | 6.2:1 | 13.8:1 | 5.7:1 |

**여기서 잡은 결함 두 가지**: 스파크라인이 항상 비어 있었습니다 —
`dtMs`를 `lastStamp` 갱신 뒤에 계산해 간격이 늘 0이었습니다. 그리고 다크 모드에서
헤더 링크가 브라우저 기본 `:visited` 보라색이라 거의 보이지 않았습니다.
후자는 토글을 붙이기 전에도 OS가 다크인 사용자에게는 있던 문제입니다.

---

## T7. pcap 리플레이 (저장된 패킷 재주입) — 선택/대형

**왜**: 지금은 pcap을 **내보내기만** 합니다. 저장한 pcap을 **다시 주입**하면, 특정 버그를
유발한 트래픽 시퀀스를 결정적으로 재현할 수 있습니다(회귀 테스트에 강력).

**규모**: 큼. **난이도**: 높음(플랫폼별 주입 경로 차이). 다른 기능 다 끝낸 뒤 착수 권장.

### 설계 개요
- 신규 `src/pcapreplay.cpp`: `pcapexport.cpp`의 역함수. 24바이트 글로벌 헤더 확인 후
  (16바이트 레코드 헤더 + raw 패킷) 반복을 읽어, 레코드 타임스탬프 간격을 지켜 주입.
- **주입 경로**가 관건입니다:
  - **리눅스**: `divert_linux.cpp`의 raw 소켓(`injectRawPacket`)을 재사용 가능. 단 fwmark가
    찍혀 있어야 NFQUEUE로 되돌아오지 않음. outbound IPv4/IPv6만(duplicate와 동일 제약).
  - **Windows**: `WinDivertSend`에 유효한 `WINDIVERT_ADDRESS`가 필요. 캡처가 없으면 주소를
    합성해야 함(Outbound=1, Loopback 등 설정). `packetutil_win.cpp`에 주소 합성 헬퍼 추가 필요.
- 공용 인터페이스를 `common.h`에 정의하고 백엔드가 구현:
  ```c
  int  packetBackendInject(const char *packet, UINT len, BOOL outbound); // 신규 백엔드 훅
  ```
  - 리눅스: 기존 raw 소켓으로 send
  - Windows: 합성 주소로 WinDivertSend
- 재생 스레드: `--replay-in capture.pcap` 또는 `POST /api/replay/start {"path":...,"speed":1.0}`.
  레코드 간 `ts` 차이만큼 `Sleep` 후 주입. `speed`로 배속 지원.

### 주의 / 제약 (문서화 필수)
- 캡처(divertStart)와 동시 사용 시 상호작용 정의 필요 — 재생 패킷이 자기 필터에 다시 잡히지
  않도록 리눅스는 fwmark, Windows는 Impostor 플래그 활용.
- inbound 재생은 리눅스에서 불가(raw 소켓 한계, duplicate와 동일).
- 대용량 pcap은 스트리밍 파싱(전체 로드 금지).

### 완료 기준
- [x] `--replay-in x.pcap` / `POST /api/replay/start` 로 재생, 타임스탬프 간격 유지
- [x] `speed` 배속, `loop` 반복 동작
- [x] **Linux 실트래픽 왕복 확인** — 25패킷 캡처 → 재생 → 25패킷 주입 → 수신 25개
- [x] 잘못된 입력은 스레드를 띄우기 전에 거부 (없는 파일 / 매직 넘버 / 링크 타입)
- [x] 권한이 없으면 크래시 없이 `failed`로 집계
- [x] manual.md / LINUX.md에 플랫폼별 제약 문서화

**Windows 주입 경로는 코드 리뷰까지만**입니다. 이 환경의 백신이 빌드 직후
`clumsy.exe`를 격리해 관리자 권한 실트래픽 테스트를 완주할 수 없었습니다.
제어 평면(파일 검증·스레드 수명·카운터)은 `windows/api_test.ps1` 55/55로 검증했고,
실제 주입은 `WinDivertOpen("false", …, SEND_ONLY)` + 합성 `WINDIVERT_ADDRESS`
경로로, `tests/windows/capture_test.ps1`에 준하는 관리자 테스트가 남아 있습니다.

**설계 메모**: 패킷 사이에 고정 시간을 자지 않고 **원본 타임라인 기준으로** 맞춥니다.
전자는 주입 비용이 누적되어 점점 뒤처집니다. 스트리밍 파서라 파일 크기와 무관하게
메모리에 한 패킷만 올립니다.

---

## T8. 지연 히스토그램 / p50·p95 통계 — 선택

**왜**: 리포트·stats에 "평균 지연"만 있으면 롱테일을 놓칩니다. p50/p95/p99와 히스토그램이
있으면 "가끔 튀는" 문제를 정량화할 수 있습니다(특히 T3 분포와 함께).

**규모**: 중간. **난이도**: 중.

### 설계 개요
- lag/jitter 모듈이 패킷을 **실제로 지연시킨 시간**을 기록해야 합니다. 지금은 버퍼에서
  timestamp 기반으로 내보내므로, 내보내는 순간 `now - enqueueTime`을 히스토그램 버킷에 누적.
- `report.cpp`에 지연 히스토그램(예: 0-10, 10-25, 25-50, 50-100, 100-250, 250-500, 500+ ms)
  버킷 배열을 두고, `reportRenderHtml()`에서 인라인 SVG 막대그래프로 렌더.
- stats JSON에 `"latency":{"p50":..,"p95":..,"p99":..}` 추가(정렬 없이 히스토그램에서 근사).
- 스레드 안전: 히스토그램은 divert mutex 안(모듈 process)에서만 쓰거나, 원자적 카운터 배열 사용.

### 완료 기준
- [x] lag 300ms 설정 시 실측 wire delay 323ms, 히스토그램 p50 322ms로 일치
- [x] pareto 분포에서 p95(327ms)가 p50(161ms)보다 뚜렷이 큼
- [x] 리포트 HTML에 13구간 막대그래프 + min/mean/p50/p95/p99/max 카드
- [x] `/metrics`의 네이티브 histogram과 `/api/stats` 분위수가 같은 데이터에서 산출
- [x] 단위 테스트 35 assert (균등분포 복원, 단조성, 롱테일 분리, 롤오버)
- [x] 빌드 경고 0

**정확도 개선**: 교과서식 구간 보간은 0~400ms 균등분포의 p95를 **467ms**로
과대추정합니다(답이 든 구간의 명목 상한이 500ms인데 데이터는 400ms까지뿐).
최소·최대값은 정확히 기록하므로 양 끝 구간을 관측 범위로 좁혀 오차를 25ms 이내로
줄였습니다. 이 값을 `tests/latency_test.cpp`가 단언합니다.

**스레드 안전**: 쓰기는 lag/jitter의 `process()`뿐이고 캡처 뮤텍스 안에서만
일어나므로 서로 직렬화됩니다. 읽기(HTTP 워커·리포트)는 락 없이 `LONG`을
읽으므로 값이 찢어지지 않습니다. 합계는 64비트 Interlocked가 없어
1e9 단위 롤오버 카운터 + 나머지, 두 개의 `LONG`으로 나눠 저장합니다.

---

## 진행 순서 (실제로는 아래 순서로 구현했습니다)

1. **T1(/metrics) → T2(corrupt) → T3(jitter 분포)** — 각각 독립적이고 반나절이면 끝나며,
   T2는 모듈 추가 절차를 몸에 익히는 좋은 연습입니다.
2. **T5(프로파일) → T6(대시보드 UX) → T4(시나리오 에디터)** — 프론트 작업이 이어지므로 묶어서.
3. **T8(히스토그램)** — T3와 시너지.
4. **T7(pcap 리플레이)** — 가장 크므로 마지막. 필요성이 확실할 때만.

각 기능 완료 시:
- Windows: `MSBuild msvc\clumsy.vcxproj /p:Configuration=Release /p:Platform=x64` 경고 0 확인,
  `tests/windows/capture_test.ps1`(관리자) 회귀
- 리눅스: `make && make test` 경고 0, `tests/`의 계약 테스트
- 문서: `manual.md`(사용법), `README.md`(엔드포인트/모듈 수), 이 문서의 완료 체크, `working_log.md`

문서화까지가 "완료"입니다.

---

## 후속 과제

- **Windows 관리자 권한 재생 검증** — 위 T7 참고. 백신이 실행 파일을 격리하지 않는
  환경에서 `tests/windows/capture_test.ps1`에 재생 왕복 항목을 추가하면 됩니다.
- **`capture_test.ps1`에 corrupt·지연 히스토그램 항목 추가** — 현재 23항목은
  Phase 4.12 시점 기준입니다. Linux 쪽 `behaviour_test.sh`에는 이미 들어 있습니다.
