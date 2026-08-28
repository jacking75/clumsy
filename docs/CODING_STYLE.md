# clumsy 코드 스타일 원칙

> Phase 1.4 산출물. 이 문서는 C++ 전환 이후 clumsy 소스에 적용되는 규칙을 정리합니다.
> 로드맵 전체는 [TODO.md](../TODO.md), 아키텍처 개요는 [CLAUDE.md](../CLAUDE.md)를 참고하세요.

## 1. 큰 원칙 — 기존 코드와 신규 코드를 구분한다

clumsy는 C11에서 C++23으로 **컴파일러만** 전환했습니다. 언어 전환이 곧 재설계는 아닙니다.

| 구분 | 대상 | 규칙 |
|---|---|---|
| **기존 코드** | `divert.cpp`, `packet.cpp`, 11개 모듈(`lag.cpp` 등), `utils.cpp`, `elevate.cpp`, `procfilter.cpp`, `statslog.cpp` | 로직을 그대로 유지. 클래스/템플릿으로 재설계하지 않는다. |
| **신규 코드** | `httpserver.cpp`, `controlapi.cpp`, `json.cpp`, `pcapexport.cpp`, `report.cpp`, 향후 리눅스 백엔드 | `std::string` / `std::vector` / `std::optional` / RAII를 적극 사용. |

**이유**: 패킷 처리 핫패스(`process()` 계열)는 이미 검증된 코드입니다. 회귀 리스크 대비 재설계 이득이 낮습니다.
반면 수제 파서·버퍼 관리 코드(옛 `pipe.c`의 flat-JSON 파서 등)는 표준 라이브러리로 대체할 때 코드량과 버그가 함께 줄어듭니다.

## 2. Module 구조체 아키텍처는 유지한다

`Module`은 함수 포인터 테이블입니다. 이것을 가상 함수를 가진 기반 클래스로 바꾸지 않습니다.

```cpp
Module lagModule = {
    "Lag", NAME, (short*)&lagEnabled,
    lagStartUp, lagCloseDown, lagProcess,
    lagSetParam, lagGetParams,
    lagParamSpecs, (int)(sizeof(lagParamSpecs)/sizeof(lagParamSpecs[0])),
    /* runtime */ 0, 0, 0
};
```

- 새 모듈 추가 절차(`CLAUDE.md`)와의 호환성을 Phase 전체에서 유지합니다.
- 모듈은 **정적 초기화만 하는 POD**여야 합니다. 생성자/소멸자를 넣지 마세요 — 정적 초기화 순서 문제가 생깁니다.

## 3. C++ 전환 시 실제로 고쳐야 했던 것들

전환 과정에서 발견된 항목입니다. 새 코드를 작성할 때도 같은 함정을 피하세요.

### 3.1 문자열 리터럴 → `const char*`

C에서는 `char *p = "literal";`이 허용되지만 C++에서는 오류입니다.

```cpp
// 잘못됨 (C++에서 컴파일 오류)
char *protocol = "TCP ";
// 올바름
const char *protocol = "TCP ";
```

문자열 리터럴을 담는 구조체 필드도 마찬가지입니다 (`filterRecord::filterName` 등).

### 3.2 네임스페이스 스코프의 `const` 객체는 내부 링키지를 가진다

C에서는 외부 링키지지만 C++에서는 기본이 내부 링키지입니다. `extern`을 명시해야 합니다.

```cpp
// packet.cpp
extern PacketNode * const head = &headNode, * const tail = &tailNode;
//^^^^^^ 없으면 divert.cpp의 extern 선언이 링크되지 않는다
```

### 3.3 축소 변환(narrowing)

중괄호 초기화에서 `int` → `char` 축소는 C++에서 경고/오류입니다.

```cpp
// 잘못됨: 0x88은 signed char 범위를 넘는다
static char patterns[] = { 0x64, 0x88, 0xAA };
// 올바름
static const unsigned char patterns[] = { 0x64, 0x88, 0xAA };
```

### 3.4 `void*` 암시적 변환

`malloc`/`calloc`/`realloc` 반환값은 반드시 캐스팅합니다. 기존 코드는 이미 캐스팅되어 있었지만, 새로 작성할 때는 애초에 `std::vector`/`std::string`을 쓰는 편이 낫습니다.

### 3.5 예약어

`new`, `delete`, `class`, `template`, `namespace`, `private`, `public`, `this`, `operator`를 식별자로 쓰지 않습니다. (현재 소스에는 충돌 없음 — 전수 확인 완료)

## 4. 스레드 안전성 규칙 (변경 없음)

C++로 바뀌어도 동시성 모델은 그대로입니다.

- 모듈 파라미터는 `volatile short` / `volatile LONG`이며, 쓰기는 항상 `InterlockedExchange16` / `InterlockedExchange`를 씁니다.
- `process()` 내부에서는 volatile 값을 **한 번만 읽어 지역 변수에 스냅샷**한 뒤 사용합니다. 같은 배치 안에서 값이 바뀌는 것을 막기 위함입니다.
- `head`/`tail` 패킷 리스트는 divert mutex 안에서만 조작합니다. HTTP 스레드는 이 리스트를 절대 건드리지 않고, 파라미터/플래그만 씁니다.
- `std::atomic`으로 교체하지 않습니다 — `platform.h`가 POSIX에서 같은 의미를 제공하므로
  이중 관리가 되고, 패킷 핫패스 전체를 건드리게 됩니다.

## 5. 신규 코드에서의 C++ 사용 범위

과하지 않게 씁니다. 다음은 권장:

- `std::string` — 문자열 조합/파싱 (수제 `snprintf` 누적 대신)
- `std::vector<T>` — 가변 길이 버퍼
- `std::optional<T>` — "값이 없을 수 있음"을 반환 타입으로 표현
- RAII 래퍼 — 소켓, 파일 핸들, Windows HANDLE

다음은 지양:

- 예외(exception) — 기존 코드가 반환 코드 기반이며, 캡처 스레드에서 예외를 던지면 처리 지점이 애매해집니다. 신규 코드도 반환 코드/`std::optional`로 통일합니다.
- 템플릿 메타프로그래밍 — 컴파일 시간과 가독성 대비 이득이 없습니다.
- 외부 라이브러리 의존 — [부록 A 원칙 2](../TODO.md) 참조. HTTP 서버·JSON 파서는 직접 구현 범위입니다.

## 6. 포맷

- 들여쓰기 4칸, 탭 없음.
- 중괄호는 K&R (`if (x) {`).
- 파일 인코딩은 UTF-8(BOM 없음). MSVC에는 `/utf-8`을 넘겨 코드페이지 경고(C4819)를 막습니다.
- 주석은 코드가 "왜" 그런지를 설명합니다. "무엇"은 코드가 말합니다.
