# Digitiz 설계 문서

안드로이드 폰을 PC의 **디지타이저(펜 태블릿)** 로 쓰는 프로그램.
폰 화면을 터치하면 PC에서 클릭이, 슬라이드하면 드래그가 발생한다.

- **PC = 호스트(서버)**: 좌표를 받아 OS에 마우스 입력으로 주입한다.
- **Android = 게스트(클라이언트)**: 터치를 받아 PC 좌표로 변환해 전송한다.
- **연결 = USB 유선, ADB reverse TCP 터널** (AOA는 보조 백엔드로 후속 추가)

---

## 1. 기술 스택 결정

| 영역 | 선택 | 근거 |
|---|---|---|
| 전송 계층 | **ADB reverse TCP** (기본) / AOA + libusb (보조, 후속) | 배포 대상이므로 §4.1 참조. 드라이버가 **추가적**이고 MTP를 깨지 않음 |
| 게스트 UI | GameActivity + GLES 3.0 | 어차피 전부 커스텀 드로잉. 소켓은 NDK 직접이라 **Java 코드가 거의 남지 않는다** |
| 호스트 GUI | Dear ImGui + GLFW + OpenGL 3 | on/off + 로그 콘솔이 수십 줄. Win/Linux/mac 동일 코드 |
| 공용 코드 | C++20, OS 의존 0 | `common/`을 양쪽 CMake에서 공유 |
| 바이트 순서 | 리틀 엔디언 고정 | x86/ARM 모두 LE, 변환 코드 불필요 |

호스트 OS 지원: **Windows 우선**, Linux/macOS는 인터페이스만 정의하고 M1 이후 구현.
게스트: Android 단일.

---

## 2. 전체 구조

```
┌─────────────────────────── PC (호스트/서버) ───────────────────────────┐
│                                                                        │
│   ImGuiShell            HostApp              PointerPipeline           │
│   ├ StatusPanel   ◄───  상태머신      ────►  DOWN/MOVE/UP/CANCEL       │
│   ├ EnableToggle        Disconnected                 │                 │
│   └ LogPanel            → Probing                    ▼                 │
│        ▲                → Tunneling            IInputInjector          │
│        │                → Connected            └ Win32InputInjector    │
│        │                      │                     (SendInput)        │
│     LogSink                   ▼                                        │
│                          ITransport                                    │
│                          └ AdbTransport (TCP 서버 + adb reverse)       │
└───────────────────────────────┬────────────────────────────────────────┘
                                │  USB (adb 터널)  길이 프리픽스 프레이밍
┌───────────────────────────────┴──────── Android (게스트/클라이언트) ───┐
│                          TcpTransport                                  │
│                          connect(127.0.0.1:27183)                      │
│                                                                        │
│   App (android_main)                                                   │
│   ├ TouchRouter    1지점 → 디지타이저 / 2지점+ → 뷰 조작              │
│   ├ ViewTransform  PC px ↔ 화면 px (scale, pan)                       │
│   └ GridRenderer   프래그먼트 셰이더 그리드 (PC px 기준, LOD)         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 리포지토리 레이아웃

```
digitiz/
├─ CMakeLists.txt              # common + host (게스트는 gradle이 별도 진입)
├─ CMakePresets.json           # msvc-x64-{debug,release}, ninja
├─ docs/
│  ├─ ARCHITECTURE.md          # 이 문서
│  ├─ PROTOCOL.md              # 와이어 프로토콜 사양
│  └─ MILESTONES.md            # 작업 분해
├─ common/                     # ★ 호스트/게스트 공유. OS API 의존 금지
│  ├─ CMakeLists.txt           #   → digitiz_common (STATIC)
│  ├─ include/digitiz/
│  │  ├─ proto/wire.hpp        #   MsgType, Writer/Reader, MessageBuilder
│  │  ├─ proto/messages.hpp    #   메시지 타입 + encode/decode
│  │  ├─ proto/framer.hpp      #   바이트스트림 → 메시지 경계 복원
│  │  ├─ core/geometry.hpp     #   Vec2/Recti/ViewTransform/grid_step_pc
│  │  └─ core/log.hpp          #   레벨 + 싱크 추상
│  ├─ src/                     #   log.cpp, messages.cpp
│  └─ tests/                   #   doctest
├─ host/
│  ├─ CMakeLists.txt
│  ├─ src/
│  │  ├─ main.cpp
│  │  ├─ app/HostApp.{hpp,cpp}
│  │  ├─ transport/Transport.hpp          # ITransport + 상태/통계 타입
│  │  ├─ transport/AdbTransport.{hpp,cpp} # TCP 서버 + adb reverse + 세션 루프
│  │  ├─ transport/AdbClient.{hpp,cpp}    # adb 탐색 / 기기 목록 / reverse
│  │  ├─ platform/Process.hpp             # 자식 프로세스 실행 + 출력 캡처
│  │  ├─ platform/Win32Process.cpp
│  │  ├─ input/InputInjector.hpp          # IInputInjector + 팩토리
│  │  ├─ input/AbsoluteCoord.hpp          # 순수 좌표 정규화 (전수 테스트됨)
│  │  ├─ input/Win32InputInjector.cpp
│  │  ├─ input/PointerPipeline.{hpp,cpp}
│  │  ├─ display/DisplayInfo.hpp          # IDisplayInfo + 팩토리
│  │  ├─ display/Win32DisplayInfo.cpp
│  │  └─ ui/{ImGuiShell.{hpp,cpp}, LogStore.hpp}
│  ├─ tests/                              # AbsoluteCoord, PointerPipeline
│  └─ bin/                                # 번들 adb (배포 시 결정)
├─ guest/                                 # Android Studio 프로젝트 루트
│  ├─ settings.gradle.kts
│  └─ app/
│     ├─ build.gradle.kts
│     └─ src/main/
│        ├─ AndroidManifest.xml
│        ├─ java/com/onart/digitiz/
│        │  └─ MainActivity.kt            # GameActivity 서브클래스
│        └─ cpp/
│           ├─ CMakeLists.txt             # add_subdirectory(../../../../common)
│           ├─ App.{hpp,cpp}              # android_main 루프
│           ├─ net/TcpTransport.{hpp,cpp} # connect + 재시도 + RX 스레드
│           ├─ view/ViewTransform.cpp
│           ├─ input/TouchRouter.{hpp,cpp}
│           ├─ ui/{EdgeHandle,SideMenu}.{hpp,cpp}
│           └─ render/{GlContext,GridRenderer,ShapeRenderer}.{hpp,cpp}
```

게스트에 JNI 브리지가 없다는 점에 주목. 소켓은 NDK에서 직접 열 수 있으므로
Kotlin은 GameActivity 서브클래스 한 줄짜리만 남는다. AOA였다면 액세서리 fd를
넘기는 브리지가 필요했다.

`common/`은 게스트 CMake에서도 `add_subdirectory`로 그대로 빌드된다. 따라서
`<windows.h>`, `<android/*.h>`, 소켓 API 등 **어떤 플랫폼 헤더도 포함하지 않는다.**

---

## 4. 전송 계층: ADB reverse TCP

### 4.1 왜 AOA가 아니라 ADB인가

**이 프로그램은 배포용이다.** 그 전제에서 두 방식의 Windows 드라이버 성격이 갈린다.

| | ADB | AOA |
|---|---|---|
| 대상 인터페이스 | ADB 인터페이스 (**별개**) | MTP 인터페이스 (**점유 중**) |
| 드라이버 작업 | 대부분 자동. 안 되면 서명된 OEM/구글 드라이버를 **추가** | WPD 드라이버를 WinUSB로 **교체** |
| MTP 파일 전송 | 계속 동작 | **깨짐** (§10.1) |
| 되돌리기 | 불필요 | 장치 관리자에서 수동 |

**파괴적이냐 아니냐**가 결정적이다. 배포 대상에게 "드라이버를 갈아끼우고 파일 전송을
포기하라"는 성립하지 않는다.

부수적으로 개발 효율에서도 ADB가 크게 유리하다. AOA는 USB 디버깅이 꺼진 상태로
개발해야 해서 **logcat도 Android Studio 디버거도 쓸 수 없다.**

**ADB를 택하는 대가**

- 사용자가 개발자 옵션 + USB 디버깅을 켜야 하고, 최초 1회 RSA 지문 승인이 필요하다
- 일부 앱(국내 금융앱, 게임 안티치트)이 USB 디버깅 켜진 기기를 거부/경고한다
- `adb` 바이너리 번들 필요 (Apache 2.0, 재배포 가능)
- adb 서버 싱글톤 충돌 가능성 (§4.4)
- 백신 오탐 가능성

### 4.2 호스트 상태머신

```
[WaitingForDevice]
  │  adb 탐색 / start-server
  │  adb devices -l 폴링 (1초 백오프)
  │    unauthorized 만 있으면 → [Unauthorized] (폰에서 RSA 승인 필요)
  ▼
[Preparing]
  │  ① 127.0.0.1:0 에 listen  → 커널이 임시 포트 P 배정
  │  ② adb -s <serial> reverse --remove tcp:27183   (이전 크래시 잔재 정리)
  │  ③ adb -s <serial> reverse tcp:27183 tcp:P
  │  ④ adb -s <serial> shell am start -n com.onart.digitiz/.MainActivity
  ▼
[WaitingForClient]  타임아웃 15초
  │  accept 성공 → TCP_NODELAY → [Connected]
  ▼
[Connected]
  │  전송 스레드: select(250ms) → recv() → Framer → 핸들러
  │  UI 스레드: 1초마다 PING, 3회 미응답이면 drop_session()
  │  소켓 종료 / 기기 분리 → reverse --remove 후 [WaitingForDevice]
```

`adb reverse tcp:27183 tcp:P` 는 **폰에서** 27183을 listen 하여 PC의 P로 터널링한다.
따라서 게스트는 폰의 `127.0.0.1:27183` 에 connect 하는 평범한 TCP 클라이언트가 되고,
**PC=서버 / 폰=클라이언트 구조가 그대로 보존된다.**

**호스트 측 포트는 임시 포트(bind 0)** 다. 디바이스 측만 고정이면 충분하므로
(게스트가 하드코딩), 호스트를 두 번 띄워도 포트를 다투지 않는다.

앱 자동 실행을 `am start`로 우리가 직접 제어한다는 점도 이득이다.
AOA였다면 시스템의 "이 앱으로 열까요?" 팝업에 의존해야 했다.

### 4.3 게스트 측

NDK에서 소켓을 직접 열 수 있으므로 **JNI 브리지가 필요 없다.**

- `connect(127.0.0.1:27183)` 실패 시 지수 백오프 재시도.
  `am start`로 앱이 뜨는 타이밍과 `adb reverse` 설정 순서가 어긋날 수 있다
- **`TCP_NODELAY` 필수** — 아래 참조
- 전용 RX 스레드 (블로킹 `recv`)
- `AndroidManifest.xml` 에 **`android.permission.INTERNET` 선언 필요.**
  루프백이라도 Android는 이 권한 없이 소켓 생성을 막는다. 정상 권한이라 다이얼로그는 없다
- `FLAG_KEEP_SCREEN_ON`

### 4.4 adb 운용 주의점

| 항목 | 내용 |
|---|---|
| **`TCP_NODELAY`** | **양쪽 모두 필수.** Nagle 알고리즘이 작은 포인터 이벤트를 뭉쳐 지연을 만든다. 디지타이저에서 이건 치명적이다 |
| adb 바이너리 선택 | **시스템 PATH의 adb를 먼저 탐색**하고 없을 때만 번들 사용 |
| 서버 싱글톤 | adb 서버는 5037 포트에 단일 인스턴스다. 버전이 다르면 기존 서버가 kill·재시작되어 **사용자의 Android Studio / scrcpy 연결이 끊긴다.** 위의 시스템 adb 우선 정책이 이 완화책이다 |
| 기기 감시 | 5037 소켓에 `host:track-devices` 를 보내면 이벤트 푸시로 받을 수 있다. M1은 500 ms 폴링으로 시작해도 무방 |
| 다중 기기 | 모든 adb 호출에 `-s <serial>` 명시 |
| reverse 정리 | 종료 시 `adb reverse --remove tcp:27183`. 안 하면 다음 실행에서 포트가 물린다 |
| `unauthorized` | 최초 연결 시 폰에서 RSA 지문 승인 필요. 이 상태를 UI에 명확히 안내한다 |
| `offline` | USB 재연결 직후 잠깐 나타난다. 재시도 필요 |
| 지연 | 실측 PING/PONG 왕복 1.6~4.2 ms (S23 Ultra, USB 2.0) |
| **게스트 수신 타임아웃** | 호스트가 비정상 종료하면 adb가 터널을 남기고 소켓이 **닫히지 않은 채 조용해질** 수 있다. 5초 무음이면 끊고 재연결한다 |
| 세션 성립 판정 | adb는 PC쪽에 아무도 없어도 기기 쪽 연결을 받아준다. `connect()` 성공은 의미가 없고, **호스트가 한 바이트라도 보냈는지**로 판정해야 재연결 백오프가 제대로 늘어난다 |

### 4.5 `ITransport` 추상화와 AOA 후속 백엔드

```cpp
struct ITransport {
    virtual bool   send(std::span<const std::byte>) = 0;
    virtual size_t recv(std::span<std::byte>)       = 0;
    virtual State  state() const                    = 0;
};
```

ADB reverse TCP와 AOA는 **둘 다 신뢰성 있는 순서 보장 바이트 스트림**이다.
따라서 프레이밍·프로토콜·좌표변환·입력주입 전부가 백엔드와 무관하게 재사용된다.

두 백엔드의 실패 모드는 상호 보완적이다.

| | Windows | Linux / macOS |
|---|---|---|
| ADB | **양호** — 드라이버 자동/추가적 | 양호 |
| AOA | 나쁨 — MTP 드라이버 교체 필요 | **매우 양호** — udev 룰 한 줄, USB 디버깅 불필요 |

M1은 ADB 단독으로 간다. AOA는 후속 옵트인 백엔드로 추가하며,
**Linux/macOS에서는 오히려 AOA가 기본값이 될 수 있다.**
그때 참고할 AOA 구현 세부는 §10.1과 아래에 남겨둔다.

<details>
<summary>AOA 백엔드 구현 메모 (후속용)</summary>

- 제어 요청: `getProtocol`=51 (`0xC0`), `sendString`=52 (`0x40`, wIndex 0~5:
  manufacturer/model/description/version/uri/serial), `startAccessory`=53 (`0x40`)
- 전환 후 재열거: VID `0x18D1`, PID `0x2D00`(accessory) / `0x2D01`(accessory+adb)
- `manufacturer`/`model` 은 게스트 `res/xml/accessory_filter.xml` 과 한 글자도
  달라선 안 된다 (앱 자동 실행 조건)
- 게스트 `read()` 버퍼가 들어온 USB transfer보다 작으면 **나머지가 유실된다.**
  양쪽 16 KiB 고정, 송신도 16 KiB 이하로 청크 분할
- 페이로드가 `wMaxPacketSize` 배수면 ZLP 필요 (`LIBUSB_TRANSFER_ADD_ZERO_PACKET`).
  단 길이 프리픽스 프레이밍이라 스트림으로 취급하면 무해
- `UsbAccessory` 는 NDK API가 없다. Kotlin에서 `openAccessory(...).detachFd()` 로
  raw fd를 뽑아 JNI로 1회 넘기고 이후는 POSIX I/O
- USB 2.0 high-speed bulk 실측 20~30 MB/s

</details>

---

## 5. 좌표계

**정본 좌표계는 PC 가상 데스크톱 픽셀(i32)** 이다. 원점은 OS가 보고하는 가상 화면
좌상단이며 멀티모니터 배치에 따라 **음수가 될 수 있다** (Windows `SM_XVIRTUALSCREEN`).

게스트는 `ViewTransform { double scale; dvec2 pan; }` 하나만 들고 있다.

```
surface_px = (pc_px - pan) * scale
pc_px      = surface_px / scale + pan
```

- **핀치**: 제스처 중심점을 고정한 채 `scale` 변경
- **2지점 드래그**: `pan` 변경
- **그리드**: PC px 기준 간격으로 그린다. `scale`에 따라 주선/보조선 간격을
  1·2·5·10·20·50·100… 로 LOD 전환한다.

호스트는 접속 시 `HELLO_ACK`로 가상 화면 rect와 모니터 목록을 보내고, 게스트는
초기 `pan`/`scale`을 "가상 화면 전체가 보이도록" 맞춘다.

### 5.1 시간축

게스트가 보내는 `t_us` 는 안드로이드 모션 이벤트의 타임스탬프이고, 호스트는 이걸
자기 시계로 옮겨야 지연을 계산할 수 있다. 두 `CLOCK_MONOTONIC` 은 부팅 시점이
달라 epoch가 무관하므로 오프셋을 **측정**한다 (`host/src/app/LatencyStats.hpp`).

PONG 왕복 중 **가장 빠른 것**을 채택한다. 빠른 왕복은 양쪽 다리가 대칭에 가깝지만,
느린 왕복은 어느 쪽에서 지연됐는지 알 수 없어 평균을 내면 추정이 오히려 나빠진다.

⚠️ **`GameActivityMotionEvent::eventTime` 은 밀리초다.** 기저 `AMotionEvent` API가
나노초를 쓰기 때문에 헤더만 보고는 틀리기 쉽고, 틀려도 아무것도 실패하지 않는다 —
지연 수치만 조용히 버려질 뿐이다. `TouchRouter::emit()` 에 세션당 1회 정합성
검사를 상시 남겨뒀다.

### 5.2 터치 라우팅 규칙 (핵심)

> **1지점 = 디지타이저 입력, 2지점 이상 = 뷰 조작**

첫 손가락이 닿는 순간에는 드로잉인지 핀치 시작인지 알 수 없다. 지연을 두면
드로잉 응답성이 죽으므로 **즉시 `POINTER_DOWN`을 보내고**, 두 번째 손가락이 닿으면
`POINTER_CANCEL`을 보낸 뒤 뷰 조작으로 전환한다. 호스트는 `CANCEL` 수신 시
눌린 버튼을 릴리스하고 해당 스트로크를 종료한다. (Android 자체 제스처 처리와 동일한 방식)

---

## 6. 호스트 입력 주입 (Windows)

`SendInput` + `INPUT_MOUSE`, 플래그 `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE`.
좌표는 가상 데스크톱 기준 **0..65535 정규화**.

**정규화 공식은 올림 나눗셈이다** (`input/AbsoluteCoord.hpp`):

```
n = ceil(offset * 65536 / extent) = (offset * 65536 + extent - 1) / extent
```

Windows는 받은 값을 **절삭**해서 픽셀로 되돌린다 (`pixel = n * extent / 65536`).
따라서 그 절삭을 통과해 목표 픽셀에 착지하는 최소값을 골라야 한다.
흔히 쓰이는 `n = v * 65535 / (extent - 1)` 과 반올림 방식은 둘 다 참몫이 정수 바로
아래에 놓이는 지점에서 **1픽셀 미달**한다.

이건 추측이 아니라 측정 결과다. `digitiz_host --selftest` 로 1920x1080에서
반올림은 max 1.00 px 오차, 올림은 0.00 px. 이후 21개 해상도의 **모든 픽셀**에 대해
왕복 항등을 단위 테스트로 고정했다.

| 항목 | 처리 |
|---|---|
| DPI | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 를 창 생성 전에 호출. 아니면 `GetSystemMetrics`가 논리 픽셀을 돌려줘 좌표가 어긋난다. 매니페스트 대신 API 호출을 쓰는 이유는 Ninja 빌드에서 매니페스트 임베딩이 번거롭기 때문 |
| 이벤트 순서 | 이동과 버튼을 **하나의 `INPUT` 이벤트**로 합친다(플래그 OR). 실제 마우스가 둘 사이에 끼어들 여지 자체가 없어진다 |
| UIPI | 관리자 권한 창에는 주입 불가. 호스트를 승격 실행해야 함 (M1은 로그로 안내만) |
| 액션 매핑 | DOWN→`LEFTDOWN`, MOVE→`MOVE`, UP→`LEFTUP`, CANCEL→`LEFTUP` |
| 범위 밖 좌표 | 가상 데스크톱으로 클램프하고 횟수를 센다 (UI에 노출) |

크로스플랫폼 계획: Linux는 `/dev/uinput` 가상 절대좌표 포인터(X11/Wayland 양쪽 동작),
macOS는 `CGEventCreateMouseEvent` + `CGEventPost`. M1은 인터페이스만 두고 Windows만 구현.

---

## 7. 호스트 GUI (M1 범위)

ImGui 창 3개.

- **Status**: 연결 상태, 기기명, 가상 화면 rect, 패킷/초, PING RTT
- **Enable**: 큰 on/off 토글. off일 때 전송은 유지하되 **주입만 드롭**하고 로그에 남긴다(안전장치)
- **Log**: 링 버퍼 N줄, 레벨 필터, 자동 스크롤, 복사. `common/core/log.hpp` 싱크로 연결

게스트가 보내는 `LOG` 메시지도 같은 콘솔에 합쳐 출력한다. logcat이 따로 있지만
**양쪽 이벤트를 하나의 타임라인에서 보는 것**이 좌표·지연 문제 추적에 훨씬 빠르다.

유휴 시 `glfwWaitEventsTimeout(0.05)`로 CPU 사용을 억제한다.

---

## 8. 게스트 렌더링 (M1 범위)

- GameActivity + EGL + GLES 3.0
- **그리드는 프래그먼트 셰이더로** 그린다. 전체화면 쿼드 1개에서 `gl_FragCoord`를
  역변환해 PC px 좌표를 얻고, `fwidth` 기반 안티에일리어싱으로 선을 그린다.
  라인 배칭 대비 코드가 훨씬 적고 LOD는 `log2(scale)` 한 줄이다.
- `ShapeRenderer`: 사각형/둥근 사각형 배치 1개면 엣지 핸들과 사이드 메뉴가 커버된다.
- 엣지 핸들: 우측 벽면 탭. 탭 또는 드래그로 사이드 메뉴 드로어 토글. **M1의 메뉴 내용은 비움.**
- 텍스트는 M1에 사실상 불필요하다. 좌표 표시가 필요해지면 stb_truetype 아틀라스를 추가한다.
- `FLAG_KEEP_SCREEN_ON` 설정. 화면이 꺼지면 세션이 끊긴다.

---

## 9. 2차 마일스톤을 위한 설계 여지

M1 시점에 미리 잡아두는 것들. 나중에 재작업을 막는 게 목적이다.

| M2 요구 | M1에서 미리 할 것 |
|---|---|
| PC 화면 전송 | 메시지 타입 `0x20~0x22` 예약. 청크 분할 전제로 프레이밍 설계 |
| 좌표 최소 간격(시공간) | `TouchRouter`에 `min_dt_us` / `min_dist_px` 필드만 뚫어두고 M1은 0 |
| 커스텀 버튼/프리셋 | `SideMenu`를 위젯 리스트 컨테이너로. 저장은 M2 |
| CR 스플라인 보정 | `PointerPipeline`을 "이벤트 in → 이벤트 out" 필터 체인으로 설계 |
| 활성 창 전달 | 메시지 `0x23` 예약 |
| 단축키 전송 | 메시지 `0x24` 예약, `IInputInjector`에 `sendKey()` 선언만 |

**M2 대역폭 결론을 미리 박아둔다**: USB 2.0 실효 대역폭은 어느 전송 계층을 쓰든
20~35 MB/s 수준인데 1080p BGRA 30fps는 250 MB/s로 한 자릿수 배 초과다.
따라서 화면 전송은 **압축 필수**이며,
PC측 하드웨어 H.264 인코딩(NVENC/QSV/AMF) → 게스트 `AMediaCodec` 디코딩 → GL 텍스처가
현실적인 유일한 경로다. JPEG는 해상도·fps를 크게 낮출 때만 성립한다.

---

## 10. 리스크

| # | 리스크 | 심각도 | 완화 |
|---|---|---|---|
| **R1** | **adb 서버 싱글톤 충돌.** 버전이 다르면 기존 서버를 kill·재시작해 사용자의 Android Studio / scrcpy 연결을 끊는다 | 중간 | **시스템 PATH의 adb를 먼저 탐색**하고 없을 때만 번들 사용. 사용 중인 adb 경로·버전을 로그에 남긴다 |
| R2 | USB 디버깅 요구가 진입장벽. 일부 앱(국내 금융앱, 게임 안티치트)이 켜진 기기를 거부/경고 | 중간 | 문서화. 장기적으로 **AOA 백엔드가 이 사용자층을 커버**한다 (§4.5) |
| R3 | 일부 OEM은 Windows에서 ADB 드라이버 수동 설치가 필요 | 중간 | 안내 문서. `unauthorized` / `offline` / 미인식 상태를 UI에 **구분해서** 표시. 원인 파악이 사용자 몫이 되면 안 된다 |
| R4 | `TCP_NODELAY` 누락 시 입력 지연 | 낮음 | 양쪽 소켓 생성 직후 설정. Phase 5에 지연 측정 포함 |
| R5 | 화면 꺼짐·백그라운드 전환 시 소켓 끊김 | 중간 | `KEEP_SCREEN_ON`, `onPause` 정리 후 복귀 시 재연결 |
| R6 | SendInput UIPI — 관리자 권한 창에 주입 불가 | 낮음 | M1은 문서화. 필요 시 매니페스트 `requireAdministrator` |
| R7 | adb 번들로 인한 백신 오탐 | 낮음 | 시스템 adb 우선 정책이 완화책. 배포 시 코드 서명 |
| R8 | M2 화면 전송 대역폭 | 중간 | §9 결론대로 H.264 전제. 해상도 비율이 사용자 옵션인 게 완충 |
| R9 | 설치된 NDK가 21.4 / 23.1로 구버전 | 낮음 | **NDK r27(LTS) 설치**, compileSdk 35 (android-35은 이미 설치됨) |

ADB 전환으로 최대 리스크였던 Windows 드라이버 문제(구 R1)가 해소되었다.
남은 항목은 대부분 "운용상 짜증" 등급이다.

---

### 10.1 참고 — AOA 백엔드를 후속으로 미룬 이유 (Windows 드라이버)

§4.5의 AOA 백엔드를 나중에 도입할 때 다시 마주칠 문제다. **M1 범위 밖이다.**

**메커니즘.** 폰을 꽂으면 Windows는 USB 복합 장치로 인식하고 `usbccgp.sys`가
인터페이스별로 자식 장치를 쪼갠다.

```
Android 폰 (예: 04E8:6860)
└─ usbccgp.sys (복합 장치 부모)
   ├─ Interface 0: MTP  → wpdusb.sys / WPD 스택   ← 탐색기에 뜨는 주체
   └─ Interface 1: ADB  → WinUSB (구글 USB 드라이버)  ← USB 디버깅 켠 경우만
```

libusb는 Windows에서 **WinUSB / libusbK / libusb-win32** 가 바인딩된 장치만 열 수 있다.
WPD 스택은 그중 어느 것도 아니다. 그런데 accessory 전환 요청(51/52/53)은 폰이
**아직 기본 모드일 때** 보내야 하므로, MTP 인터페이스의 WPD 드라이버를
WinUSB로 교체해야 하고 그 순간 WPD 스택이 인터페이스를 놓는다.

**깨지는 것**

- 탐색기에서 폰이 사라진다 (드래그&드롭 파일 복사 불가)
- WPD 기반 앱 전부 — Windows 사진 가져오기, Lightroom import, Smart Switch 등
- 장치 관리자에서 `휴대용 장치` → `범용 직렬 버스 장치` 로 이동
- **바인딩이 영속적이다.** 드라이버 저장소 + 레지스트리에 VID/PID 기준으로 박혀
  재부팅·재연결해도 유지된다. Digitiz를 안 켜도 계속 깨져 있다

**안 깨지는 것**

- 폰 자체는 무영향. 순수하게 Windows 드라이버 DB 문제이고 데이터 손실 0
- 다른 PC에 꽂으면 정상. 그 PC 하나 · 그 폰 하나에 국한
- ADB / scrcpy는 멀쩡 (별개 인터페이스)
- accessory 모드의 `18D1:2D00`은 Windows 입장에서 완전히 다른 장치라 무관
- 무선 파일 전송(구글 포토, Nearby Share 등) 무관

**되돌리기.** 브릭 아님. 장치 관리자 → 드라이버 업데이트 → 컴퓨터에서 찾아보기 →
사용 가능한 드라이버 목록에서 직접 선택 → 원래 MTP/WPD 드라이버.

**AOA 도입 시 검증할 탈출구.** 위 메커니즘까지는 확정이고 아래는 미검증이다.
하나라도 통하면 Windows에서도 AOA가 성립한다.

1. **USB 모드를 "충전만 / 데이터 전송 안 함"으로 두고 전환 시도.** 이 모드에서 폰은
   기능 인터페이스가 거의/전혀 없다. 깨뜨릴 MTP가 애초에 없으니 WinUSB를 붙여도
   잃는 게 없을 수 있다. AOA 제어 요청 처리는 현재 function 설정과 독립된 계층이라
   응답할 가능성이 있다. **통하면 Windows AOA 문제가 사실상 해소된다.**
2. **PTP 모드** — 인터페이스 구성이 달라 결과가 다를 수 있음
3. **기존 WinUSB 핸들 재활용** — 구글 USB 드라이버가 ADB 인터페이스에 이미 WinUSB를
   붙여둔다. 51/52/53은 recipient=Device라 그 핸들로도 전달될 수 있다. 통하면
   드라이버 교체 불필요. 단 USB 디버깅을 켜야 해서 AOA 선택 이유가 반쯤 무색해짐

**Linux/macOS에는 이 문제가 없다** (Linux는 udev 룰 한 줄).
순수 Windows 문제이며, 그래서 §4.5대로 **Linux/macOS에서는 AOA가 기본값이 될 수 있다.**

---

## 11. 확정된 빌드 결정

| 항목 | 결정 | 비고 |
|---|---|---|
| C++ 표준 | C++20 | `common/`은 표준 라이브러리만 사용 |
| third_party 조달 | **CMake FetchContent** | 의존성이 imgui + glfw 둘뿐이라 vcpkg/submodule은 과하다 |
| 테스트 프레임워크 | **doctest** | 헤더 1개. `common/tests/` 에서만 사용 |
| 최소 Android API | **24** | GameActivity + M2의 `AMediaCodec` 요구 |
| compileSdk | 35 | `android-35` 설치 완료 |
| NDK | **r27 (LTS)** | `build.gradle.kts` 에 `ndkVersion` 을 **명시**해 재현성 확보 |
| adb | **M1은 시스템 adb 요구** | 번들 여부는 배포 시점에 결정 (§4.4, R7) |

### 남은 환경 준비

- Android Studio SDK Manager → SDK Tools 탭 → **`Show Package Details` 체크** →
  `NDK (Side by side)` 아래에서 r27 선택 → Apply.
  (NDK는 side-by-side 패키지라 in-place 업데이트가 아니라 추가 설치다.
  접힌 목록에서는 버전을 지정할 수 없어 "Update available" 문구만 보인다.)
- 같은 화면에서 `Android SDK Command-line Tools (latest)` 도 설치하면
  `sdkmanager` CLI를 쓸 수 있다. 현재는 `cmdline-tools/` 가 없다.
- 기존 NDK 21.4 / 23.1 은 사용처가 없으므로 제거해도 무방하다.
