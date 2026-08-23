# Digitiz 와이어 프로토콜 v1

전송 계층은 **신뢰성 있는 순서 보장 바이트 스트림**으로만 가정한다
(M1은 ADB reverse TCP, 후속으로 AOA bulk). 메시지 경계는 이 프로토콜이 직접 복원한다.

- 바이트 순서: **리틀 엔디언** 고정 (x86/ARM 모두 LE)
- 모든 구조체는 명시적 패딩을 넣어 정렬을 맞춘다. `static_assert(sizeof(T) == N)`로 고정.
- 문자열: UTF-8, 고정 배열은 NUL 패딩, 가변 길이는 헤더의 `payload_len`으로 결정
- `H` = 호스트(PC), `C` = 클라이언트/게스트(Android)

---

## 1. 프레이밍

```c
struct Header {           // 8 bytes
    uint16_t magic;       // 0x4449  ("DI")
    uint8_t  type;        // MsgType
    uint8_t  flags;       // 예약, 0
    uint32_t payload_len; // 헤더 뒤 바이트 수. 최대 4 MiB
};
```

`Header` 뒤에 `payload_len` 바이트의 페이로드가 이어진다.

**Framer 규칙**

1. 수신 바이트를 링 버퍼에 누적한다.
2. 8바이트 이상이면 헤더를 파싱한다.
3. `magic != 0x4449` 이거나 `payload_len > 4 MiB` 이면 **디싱크**로 판정하고,
   버퍼를 1바이트씩 전진시키며 다음 `magic`을 재탐색한다. 이 사건은 반드시 로그에 남긴다.
4. `payload_len` 만큼 모이면 메시지를 방출하고 소비분을 버린다.

TCP(및 하위 USB)가 무결성을 보장하므로 실제 손상은 드물다. magic은
**구현 버그로 인한 디싱크를 조용히 넘기지 않기 위한 장치**다.

---

## 2. 메시지 타입

| 값 | 이름 | 방향 | 마일스톤 |
|---|---|---|---|
| `0x01` | `HELLO` | C→H | M1 |
| `0x02` | `HELLO_ACK` | H→C | M1 |
| `0x03` | `PING` | 양방향 | M1 |
| `0x04` | `PONG` | 양방향 | M1 |
| `0x10` | `POINTER` | C→H | M1 |
| `0x11` | `HOST_STATE` | H→C | M1 |
| `0x20` | `VIEWPORT_REQ` | C→H | M2 (예약) |
| `0x21` | `FRAME_INFO` | H→C | M2 (예약) |
| `0x22` | `FRAME_DATA` | H→C | M2 (예약) |
| `0x23` | `ACTIVE_WINDOW` | H→C | M2 |
| `0x24` | `KEY` | C→H | M2 |
| `0x25` | `WHEEL` | C→H | M2 (예약) |
| `0x26` | `SMOOTHING` | C→H | M2 (예약) |
| `0x7F` | `LOG` | 양방향 | M1 |

M2 값을 M1의 enum에 미리 넣어둔다. 나중에 번호를 재배치하지 않기 위해서다.

---

## 3. M1 메시지 정의

### `0x01 HELLO` (C→H)

세션 시작. 게스트가 fd를 얻은 직후 **가장 먼저** 보낸다.

```c
struct Hello {            // 80 bytes
    uint16_t proto_ver;   // = 1
    uint16_t reserved;
    int32_t  screen_w;    // 게스트 서피스 픽셀
    int32_t  screen_h;
    float    density;     // dpi / 160.0
    char     device[64];  // UTF-8, NUL 패딩
};
```

### `0x02 HELLO_ACK` (H→C)

```c
struct Monitor {          // 24 bytes
    int32_t  x, y, w, h;  // 가상 데스크톱 좌표. x/y는 음수 가능
    uint32_t dpi;
    uint8_t  primary;
    uint8_t  pad[3];
};

struct HelloAck {         // 24 bytes + Monitor[monitor_count]
    uint16_t proto_ver;   // 호스트가 지원하는 버전
    uint8_t  host_os;     // 0=Windows 1=Linux 2=macOS
    uint8_t  monitor_count;
    int32_t  vx, vy, vw, vh;   // 가상 데스크톱 전체 rect
    uint32_t reserved;
    // Monitor monitors[monitor_count];
};
```

`proto_ver`가 다르면 호스트는 로그를 남기고 세션을 닫는다. M1에서 협상은 하지 않는다.
게스트는 이 rect를 받아 초기 `pan`/`scale`을 "가상 화면 전체가 보이도록" 맞춘다.

### `0x03 PING` / `0x04 PONG`

```c
struct Ping { uint64_t t_send_us; };
struct Pong { uint64_t t_send_us;    // PING의 값을 그대로 반사
              uint64_t t_reply_us; };
```

호스트가 1초 주기로 PING. RTT를 Status 패널에 표시하고, 3회 연속 무응답이면
세션을 끊는다.

### `0x10 POINTER` (C→H)

M1의 핵심 메시지.

```c
struct Pointer {          // 24 bytes
    uint64_t t_us;        // 게스트 단조 시계 (이벤트 발생 시각)
    int32_t  x, y;        // PC 가상 데스크톱 픽셀. 음수 가능
    uint8_t  action;      // 0=DOWN 1=MOVE 2=UP 3=CANCEL 4=HOVER(예약)
    uint8_t  button;      // 0=LEFT 1=RIGHT 2=MIDDLE
    uint8_t  pointer_id;  // M1은 항상 0
    uint8_t  flags;       // 예약, 0
    float    pressure;    // 0.0~1.0, 미지원이면 1.0
};
```

**액션 시맨틱**

| action | 게스트 | 호스트 |
|---|---|---|
| `DOWN` | 첫 손가락 접촉 | 이동 + 버튼 다운을 한 `SendInput` 배열로 |
| `MOVE` | 이동 (스로틀링은 M2) | 이동 |
| `UP` | 손가락 뗌 | 버튼 업 |
| `CANCEL` | 두 번째 손가락 접촉 → 뷰 조작 전환 | 눌린 버튼 릴리스, 스트로크 종료 |

호스트는 버튼 상태를 직접 추적한다. `DOWN` 없이 온 `UP`, 중복 `DOWN`은
무시하고 로그에 남긴다. 세션이 끊길 때 눌린 버튼이 있으면 **반드시 릴리스**한다.
(안 하면 사용자 PC의 마우스가 눌린 채로 남는다.)

### `0x11 HOST_STATE` (H→C)

```c
struct HostState {
    uint8_t enabled;      // on/off 토글 상태
    uint8_t injecting;    // 현재 버튼이 눌려 있는가
    uint8_t pad[2];
};
```

토글이 바뀔 때마다 전송. 게스트는 이걸로 그리드 색을 바꿔 상태를 표시한다.

### `0x23 ACTIVE_WINDOW` (H→C)

```c
struct ActiveWindow {     // 68 bytes
    uint32_t pid;
    char     process[64]; // 실행 파일 이름만, NUL 패딩. "krita.exe"
};
```

PC의 포커스가 다른 프로그램으로 옮겨갈 때마다 전송. 게스트가 프로그램별
버튼 프리셋을 자동으로 올리는 데 쓴다.

**창 제목이 아니라 실행 파일 이름이다.** 제목은 열어둔 문서를 따라 바뀌므로
파일을 하나 열 때마다 프리셋이 흔들린다. 프리셋이 대응하는 대상은 프로그램이다.

`process` 가 비어 있으면 호스트가 창을 식별하지 못한 것이다(권한이 없는
프로세스 등). 게스트는 마지막 프리셋을 유지하지 말고 기본값으로 돌아간다.

호스트는 **포커스가 잠시 머무른 뒤에** 보낸다. 알트탭 한 번은 거의 항상 셸을
거쳐 가는데(작업 표시줄이 실제로 0.1초쯤 포커스를 가져간다) 그대로 흘리면
전환할 때마다 프리셋이 기본값으로 떨어졌다 돌아온다. 정착 대기는 200 ms.

### `0x24 KEY` (C→H)

```c
struct Key {              // 20 bytes
    uint8_t modifiers;    // bit0 Ctrl, bit1 Shift, bit2 Alt, bit3 Meta(Win)
    uint8_t action;       // 0=Press(눌렀다 뗌), 1=Down, 2=Up
    uint8_t pad[2];
    char    key[16];      // 소문자 키 이름, NUL 패딩
};
```

게스트의 단축키 버튼이 보낸다. 호스트는 조합키를 누른 채로 키를 입력하고
반대 순서로 뗀다. 한 번의 `SendInput` 배치로 나가므로 사용자의 실제 입력이
조합키와 키 사이에 끼어들 수 없다.

**코드가 아니라 이름이 간다.** 가상 키 번호는 호스트 OS의 속성이고 폰이 그걸
알 이유가 없다. 폰은 사용자가 입력한 것을 저장하고, 호스트가 자기 입력 API가
원하는 것으로 바꾼다. 리눅스·macOS 호스트가 게스트를 고치지 않고 들어올 수
있는 것도 그 덕이다.

이름은 `a`~`z`, `0`~`9`, `f1`~`f24`, `numpad0`~`numpad9`, 그리고 `escape` `tab`
`enter` `space` `backspace` `delete` `insert` `home` `end` `pageup` `pagedown`
`up` `down` `left` `right` 등의 표에서 찾는다. 모르는 이름은 **비슷한 것으로
추측하지 않고** 거부하고 호스트 콘솔에 남긴다 — 사용자 PC에서 엉뚱한 키가
눌리는 것보다 아무것도 안 눌리는 편이 낫다.

### `0x7F LOG` (양방향)

```c
struct LogMsg {
    uint8_t level;        // 0=Trace 1=Debug 2=Info 3=Warn 4=Error
    uint8_t pad[3];
    // char text[payload_len - 4];   // UTF-8, NUL 종료 없음
};
```

게스트 로그를 호스트 콘솔에 합쳐 출력한다. logcat이 따로 있지만
**양쪽 이벤트를 하나의 타임라인에서 보는 것**이 좌표·지연 문제 추적에 훨씬 빠르다.

---

## 4. M2 예약 메시지 (스케치)

확정 아님. 번호만 선점하고 필드는 M2에서 결정한다.

```c
// 0x20 VIEWPORT_REQ (C→H) — 게스트가 보고 싶은 PC 영역과 출력 해상도
struct ViewportReq {
    int32_t  x, y, w, h;     // 요청 영역 (PC px)
    uint16_t out_w, out_h;   // 인코딩 해상도 (해상도 비율은 사용자 옵션)
    uint8_t  fps;            // 폴링 레이트
    uint8_t  codec;          // 0=RAW_BGRA 1=JPEG 2=H264
    uint8_t  quality;
    uint8_t  flags;          // bit0: 커서 포함
};

// 0x21 FRAME_INFO (H→C) — 프레임 메타. 뒤이어 FRAME_DATA 청크가 온다
// 0x22 FRAME_DATA (H→C) — {u32 seq; u32 offset; bytes} 로 16 KiB 이하 분할

// 0x25 WHEEL (C→H)
// 0x26 SMOOTHING (C→H) — CR 스플라인 보정 파라미터
```

### 흐름 제어 (M2 과제)

M1은 이벤트가 작아 흐름 제어가 불필요하다. M2의 프레임 전송은 다르다.
TCP는 백프레셔가 있어 느린 수신자가 송신을 막는다. 화면 전송이
입력 경로를 막으면 안 되므로 다음 중 하나가 필요하다.

- 게스트가 프레임 ACK를 보내고 호스트가 in-flight 1~2 프레임으로 제한
- 또는 호스트 송신 큐에서 오래된 프레임을 드롭

**입력 지연이 화면 지연보다 항상 우선한다.** 송신 큐를 우선순위 큐로 만들지,
아니면 `adb reverse` 로 두 번째 포트를 열어 채널 자체를 분리할지는 M2에서 결정한다.
(채널 분리는 ADB 전송에서는 포트 하나 추가로 쉽지만, 후속 AOA 백엔드에는 bulk
엔드포인트 쌍이 하나뿐이라 적용할 수 없다. 이식성을 지키려면 우선순위 큐가 낫다.)
