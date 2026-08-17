# 구현 계획

## 1차 마일스톤 (최소사양)

목표: **폰을 터치하면 PC에서 클릭되고, 슬라이드하면 드래그된다.**
게스트는 PC 절대 픽셀에 대응하는 그리드 + 핀치 조정 + 빈 사이드 메뉴.
호스트는 중계 + on/off + 콘솔 로거.

### 순서 원칙

ADB 전송으로 확정하면서 **프로젝트-킬러급 리스크는 사라졌다.**
남은 순서 원칙은 두 가지다.

1. **작업 10(입력 주입)을 단독 검증 가능하게 만든다.** 나중에 "클릭이 안 된다"가
   주입 문제인지 전송 문제인지 즉시 갈라진다.
2. **작업 3(폰에서 빈 앱 실행)과 작업 12(adb 기기 인식)를 Phase 0에서 확인한다.**
   기기·OEM별 드라이버 이슈(R3)는 있어도 조기에 드러나는 편이 낫다.

---

### Phase 0 — 리포/툴체인

| # | 작업 | 산출물 |
|---|---|---|
| 1 | 리포 스캐폴딩. 최상위 `CMakeLists.txt`, `CMakePresets.json`(msvc-x64 debug/release, ninja), `.gitignore`, `.clang-format` | 빈 타깃이 빌드됨 |
| 2 | third_party 조달 방침 확정 후 glfw / imgui 확보 | `cmake --build` 통과 |
| 3 | NDK r27 설치. `guest/` gradle 프로젝트 생성, GameActivity 빈 화면 | **`adb devices`에 폰이 뜨고** 검은 화면 앱이 설치·실행됨 |

### Phase 1 — common (양쪽 공유, OS 의존 0)

| # | 작업 | 검증 |
|---|---|---|
| 4 | `wire.hpp` 헤더/MsgType, `messages.hpp` encode/decode | `static_assert(sizeof)` + 왕복 테스트 |
| 5 | `Framer` — 스트림에서 메시지 경계 복원 | **분할 수신 / 합쳐진 수신 / 쓰레기 데이터 리싱크** 단위 테스트 |
| 6 | `geometry.hpp` `ViewTransform` | pc↔surface 왕복 오차 테스트 |
| 7 | `core/log.hpp` 레벨 + 싱크 추상 | 호스트=ImGui, 게스트=logcat+LOG 메시지 |

### Phase 2 — 호스트 골격 (전송 없이 단독 동작)

| # | 작업 | 상태 |
|---|---|---|
| 8 | ImGui + GLFW 창, Log 패널(스레드 안전), on/off 토글 | ✅ |
| 9 | `Win32DisplayInfo` — 가상 화면/모니터 열거 + `SetProcessDpiAwarenessContext` | ✅ |
| 10 | `Win32InputInjector` — SendInput 절대좌표 + 자체 테스트 | ✅ |
| 11 | `PointerPipeline` — POINTER→주입 상태머신 | ✅ |

작업 10을 단독 검증 가능하게 만든 게 바로 값을 했다. 좌표 정규화 공식이
1픽셀 틀려 있었고, `--selftest` 의 실측(반올림 1.00 px vs 올림 0.00 px)이
그걸 잡아냈다. 이후 `AbsoluteCoord.hpp` 로 분리해 21개 해상도의 모든 픽셀에 대해
왕복 항등을 전수 테스트로 고정했다 (약 45만 assertion).

파이프라인 테스트는 정상 경로가 아니라 **버튼이 눌린 채 남는 모든 경로**를 겨냥한다:
중복 DOWN, DOWN 없는 UP, 버튼이 어긋난 UP, 스트로크 도중 비활성화, 세션 강제 종료.
사용자 PC의 마우스가 눌린 채 남는 게 이 프로그램 최악의 실패 모드다.

### Phase 3 — ADB 전송

| # | 작업 | 상태 |
|---|---|---|
| 12 | `AdbClient` — adb 탐색(**시스템 PATH 우선**), `start-server`, 기기 목록/상태 | ✅ |
| 13 | `AdbTransport` — 루프백 **임시 포트** listen + `adb reverse tcp:27183 tcp:<port>` | ✅ |
| 14 | `adb shell am start`로 게스트 앱 자동 실행 | ✅ (앱은 Phase 4) |
| 15 | accept → **`TCP_NODELAY`** → RX 스레드 + Framer + 재연결 루프 | ✅ |
| 16 | PING/PONG RTT + 하트비트 타임아웃 | ✅ |

**호스트 측 포트는 임시 포트(bind 0)** 로 잡는다. 디바이스 측만 27183으로 고정하면
되므로(게스트가 하드코딩), 호스트끼리 포트를 다툴 일이 없어진다.

`TCP_NODELAY` 를 빠뜨리면 Nagle이 포인터 이벤트를 뭉쳐 원인 찾기 어려운 지연이
생긴다. 세션 수립 직후 설정한다.

### Phase 3 검증 방법 (게스트 앱 없이)

디바이스의 `nc` 로 실제 HELLO를 터널에 흘려보내 전 구간을 확인한다.
`tools/test-tunnel.sh` 가 이걸 자동화한다.

```bash
./tools/test-tunnel.sh          # digitiz_host 를 먼저 띄워둘 것
```

기대 결과: HELLO_ACK(모니터 정보 포함) → HOST_STATE → 1초 간격 PING,
그리고 netcat은 PONG을 안 보내므로 **3회 미응답 후 세션 드롭**.

### Phase 4 — 게스트

| # | 작업 | 상태 |
|---|---|---|
| 17 | `MainActivity`(GameActivity 서브클래스), **`INTERNET` 권한**, `KEEP_SCREEN_ON` | ✅ |
| 18 | `TcpTransport` — `connect(127.0.0.1:27183)` + 백오프 + `TCP_NODELAY` + RX 스레드, HELLO 핸드셰이크 | ✅ |
| 19 | 프래그먼트 셰이더 그리드 (PC px 기준, LOD, 데스크톱 경계 표시) | ✅ |
| 20 | `ViewTransform` + 핀치 zoom / 다지점 pan | ✅ (실기 손가락 검증 남음) |
| 21 | `TouchRouter` — 1지점=디지타이저, 2지점+=뷰 조작(+`CANCEL`) | ✅ (`CANCEL` 경로는 실기 검증 남음) |
| 22 | 엣지 핸들 + 사이드 메뉴 드로어 (내용 없음) | ✅ |

### Phase 4에서 걸린 것들

빌드 셋업이 구현보다 오래 걸렸다. 재발 방지를 위해 기록한다.

- `games-activity` 3.0.5는 **POM에 의존성을 선언하지 않는데** `GameActivity`는
  `AppCompatActivity`를 상속한다 → `androidx.appcompat` 직접 추가 + 테마도
  `Theme.AppCompat` 계열이어야 함
- 그 결과 kotlin-stdlib 중복 클래스 → jdk7/jdk8 아티팩트를 1.8 이상으로 제약
- JNI 심볼이 링커에 제거됨 → `--whole-archive` 필요
- `local.properties`에 백슬래시 금지 (Java Properties의 `\u` 이스케이프)
- **Android 15의 16KB 페이지**: 정렬 안 되면 시스템 경고 다이얼로그가 앱을 덮고
  포커스를 뺏는다 → `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`
- 메인 루프에서 poll 타임아웃을 루프 밖에서 한 번만 정하면, 포커스를 잃는 순간
  영원히 블록된다 (복귀 명령이 루프 안에서 오기 때문) → 매 반복 재계산

### Phase 4 검증 방법

실기에 손을 대지 않고도 전 구간을 확인할 수 있다.

```bash
adb exec-out screencap -p > screen.png     # 렌더링 확인
adb shell input swipe 150 550 500 800 500  # 스트로크 주입
```

720x1544 화면에서 1920x1080 데스크톱에 맞춘 상태(scale 0.33)로
`(150,550) → (500,800)` 을 그으면 호스트가 `DOWN (455, 148)` / `UP (1515, 906)` 을
받아야 한다. 계산값과 실측값이 일치하는지가 좌표계 전체의 검증이다.

### Phase 5 — 통합

| # | 작업 |
|---|---|
| 23 | 종단 지연 측정, 드래그 연속성 / `CANCEL` 동작 검증 |
| 24 | 연결 해제 · 재연결 · 앱 백그라운드 복귀 · 화면 꺼짐 처리 |
| 25 | README + 세팅 가이드 (개발자 옵션 / USB 디버깅 / RSA 승인 / OEM 드라이버 절차) |

---

## 2차 마일스톤 (사용성)

M1 완료 후 착수. M1에서 예약해둔 메시지 번호와 확장 지점을 사용한다.

| 요구 | 주요 작업 | 난이도 |
|---|---|---|
| 게스트가 해당 영역의 PC 화면 표시 | 호스트: Desktop Duplication(DXGI) 캡처 → 크롭/스케일 → **H.264 하드웨어 인코딩**. 게스트: `AMediaCodec` 디코딩 → GL 텍스처 → 그리드 아래 합성 | **높음** |
| 좌표 최소 간격 (시간/공간 개별) | `TouchRouter`의 `min_dt_us` / `min_dist_px` 활성화. 게스트 UI 슬라이더 | 낮음 |
| PC 좌표/영역/단축키를 게스트 버튼으로 등록 | 사이드 메뉴 위젯 시스템, 수치 입력(GameTextInput), 인앱 영역 선택, 기기 저장(JSON), 프리셋 | 중간 |
| 호스트의 CR 스플라인 보정 | `PointerPipeline`에 필터 삽입. **지연 vs 부드러움 트레이드오프** — 스플라인은 다음 점을 알아야 하므로 1샘플 지연이 생긴다 | 중간 |
| 영역/해상도 비율 대응 캡처 전송 | `VIEWPORT_REQ` 처리, 폴링 레이트, 흐름 제어(입력 우선) | 높음 |
| 활성 창 프로세스명 전달 | Windows `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` → `ACTIVE_WINDOW`. 게스트가 프리셋 자동 전환 | 낮음 |

### M2의 지배적 제약

USB 2.0 실효 대역폭은 **20~35 MB/s** 수준이다 (전송 계층과 무관하게 USB가 상한).
1080p BGRA 30fps = 250 MB/s로 **한 자릿수 배 초과**.
따라서 화면 전송은 압축이 선택이 아니라 필수이며, 해상도 비율 옵션이
사용자가 조절할 수 있는 유일한 완충 장치다.

흐름 제어에서 **입력 지연은 화면 지연보다 항상 우선한다.**
프레임이 큐를 막아 포인터 이벤트가 밀리면 디지타이저로서 실격이다.

---

## 착수 전 확정 사항

전부 확정되었다. 상세는 [ARCHITECTURE.md §11](ARCHITECTURE.md#11-확정된-빌드-결정).

- C++20 / third_party는 **FetchContent** / 테스트는 **doctest**
- minSdk **24**, compileSdk 35, NDK **r27(LTS)** — `ndkVersion` 명시
- adb는 M1에서 **시스템 adb 요구** (번들은 배포 시점에 결정)

**유일한 선행 조건**: NDK r27 설치 (SDK Manager → SDK Tools → `Show Package Details`).
