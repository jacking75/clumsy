# clumsy — CLAUDE.md

## 아키텍처

### 핵심 구조

```
main.c          ← IUP GUI 초기화, 이벤트 루프, 모듈 UI 조합
divert.c        ← WinDivert 래퍼: 패킷 캡처/전송, 스레드 관리
packet.c        ← PacketNode 연결 리스트 (패킷 큐)
common.h        ← 전역 타입/매크로/Module 구조체 선언
utils.c         ← 확률 계산, 타이머 유틸리티
elevate.c       ← UAC 관리자 권한 상승
```

### 모듈 시스템

각 기능은 `Module` 구조체로 구현됩니다 (`common.h:122`):

```c
typedef struct {
    const char *displayName;   // UI 표시 이름
    const char *shortName;     // CLI 인수 접두사 (예: "lag", "drop")
    short *enabledFlag;        // 활성화 상태 (volatile)
    Ihandle* (*setupUIFunc)(); // IUP 컨트롤 생성
    void (*startUp)();         // 모듈 활성화 시 호출
    void (*closeDown)(head, tail); // 비활성화 시 호출 (버퍼 플러시)
    short (*process)(head, tail);  // 매 클럭 틱마다 패킷 처리
    // 런타임 필드
    short lastEnabled;
    short processTriggered;
    Ihandle *iconHandle;
} Module;
```

현재 모듈 목록 (`main.c:10`, 처리 순서 = 우선순위):

| 파일 | 모듈 | 기능 |
|------|------|------|
| `lag.c` | Lag | 고정 지연(ms) |
| `drop.c` | Drop | 확률적 패킷 드롭 |
| `throttle.c` | Throttle | 일시적 패킷 억제 (버스트 후 일괄 전송/드롭) |
| `duplicate.c` | Duplicate | 패킷 복제 |
| `ood.c` | Out of Order | 패킷 순서 뒤섞기 |
| `tamper.c` | Tamper | 패킷 페이로드 변조 |
| `reset.c` | Reset | TCP RST 강제 전송 |
| `bandwidth.c` | Bandwidth | 대역폭 상한 제한 (KB/s) |

### 스레드 구조

```
메인 스레드 (IUP 이벤트 루프)
    ├─ divertReadLoop   ← WinDivertRecv() 블로킹 수신
    └─ divertClockLoop  ← 40ms 주기로 패킷 처리 트리거
```

두 스레드는 단일 mutex로 동기화되며, `divertConsumeStep()`에서 모든 모듈의 `process()`를 순서대로 호출합니다.

### 패킷 흐름

```
WinDivertRecv()
    → PacketNode 생성 → appendNode() (tail 앞에 삽입)
    → divertConsumeStep()
        → 각 모듈 process(head, tail)  [순서대로]
        → sendAllListPackets()          [남은 패킷 전송]
    → WinDivertSend()
```

---

## 빌드 주의사항 (개발자)

- `MinGW32` 빌드 시 `InterlockedXxx16` 함수를 GCC atomic builtin으로 대체 (`common.h:27-57`)
- Release 빌드는 WindowedApp으로 빌드됨 → stdout/stderr 미출력, `LOG()` 매크로는 `OutputDebugString` 사용

---

## 새 모듈 추가 방법

1. `src/<name>.c` 생성
2. `Module` 구조체 인스턴스 정의 (`lag.c` 참고)
3. `common.h` 에 `extern Module <name>Module;` 선언 추가
4. `main.c` 의 `modules[]` 배열에 추가 (처리 순서 결정)
5. `MODULE_CNT` 값 증가 (`common.h:12`)
