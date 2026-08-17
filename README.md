# Digitiz

안드로이드 폰을 PC의 **디지타이저(펜 태블릿)** 로 쓴다.
폰 화면을 터치하면 PC에서 클릭이, 슬라이드하면 드래그가 발생한다.

- **PC = 호스트(서버)** — 좌표를 받아 OS에 마우스 입력으로 주입
- **Android = 게스트(클라이언트)** — 터치를 PC 좌표로 변환해 전송
- **연결 = USB 유선**, ADB reverse TCP 터널

## 문서

| | |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 구조, 전송 계층, 좌표계, 리스크, 빌드 결정 |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | 와이어 프로토콜 v1 |
| [docs/MILESTONES.md](docs/MILESTONES.md) | 작업 분해 (1차 / 2차 마일스톤) |

## 현재 상태

1차 마일스톤 진행 중.

- [x] Phase 0 — 리포/툴체인
- [x] Phase 1 — `common/` (프로토콜, Framer, 좌표변환, 로그)
- [x] Phase 2 — 호스트 골격 (ImGui 셸, 화면 정보, 입력 주입, 포인터 파이프라인)
- [x] Phase 3 — ADB 전송 (reverse 터널, 세션 루프, PING/PONG)
- [x] Phase 4 — 게스트 (GameActivity + GLES3, 그리드, 핀치, 터치 라우팅, 사이드 메뉴)
- [x] Phase 5 — 통합 (지연 측정, 재연결/백그라운드 처리, 세팅 가이드)

**1차 마일스톤 완료.** 실측 지연: PING/PONG 왕복 1.6~4.2 ms, 터치 이벤트가
호스트에 도달하기까지 평균 11.3 ms, 주입은 측정 한계 이하.

## 처음 쓰는 법

**폰에서 (한 번만)**

1. 설정 → 휴대전화 정보 → 소프트웨어 정보 → **빌드번호를 7번 탭** (개발자 옵션 활성화)
2. 설정 → 개발자 옵션 → **USB 디버깅** 켜기
3. USB 케이블로 PC에 연결
4. 폰에 뜨는 **"USB 디버깅을 허용하시겠습니까?"** 에서 「이 컴퓨터에서 항상 허용」 체크 후 허용

**PC에서**

```bash
adb devices          # 폰이 device 상태로 보이면 준비 완료
digitiz_host.exe
```

호스트가 알아서 터널을 세우고 폰의 앱을 실행한 뒤 접속을 기다린다.
앱을 미리 설치해 두어야 한다 (`./tools/guest.sh install`).

마지막으로 호스트 창에서 **INJECTION 토글을 켜야** 폰 터치가 PC에 전달된다.
꺼져 있을 때는 좌표를 받아 세기만 하고 주입하지 않는다.

**폰 화면 읽는 법** — 그리드 색이 상태 표시다.

| 색 | 뜻 |
|---|---|
| 회색 | 호스트와 연결되지 않음 |
| 호박색 | 연결됨, 주입은 꺼짐 |
| 초록색 | 입력이 실제로 PC에 전달되는 중 |

주황색 사각형은 PC 화면의 경계다. 한 손가락은 디지타이저 입력, 두 손가락은
확대·이동이며, 그리는 도중 두 번째 손가락이 닿으면 그 획은 취소된다.

## 문제가 생기면

| 증상 | 원인과 해결 |
|---|---|
| 호스트가 `Unauthorized` | 폰의 RSA 허용 대화상자를 수락한다. 케이블을 다시 꽂거나 재부팅하면 다시 물어볼 수 있다 |
| 호스트가 `Waiting for device` 에서 멈춤 | `adb devices` 로 확인. 비어 있으면 USB 디버깅이 꺼졌거나 OEM 드라이버가 필요하다 |
| `No adb` | adb를 PATH에 두거나 `ANDROID_HOME` 을 설정한다 |
| 다른 프로그램의 adb 연결이 끊김 | adb 서버는 5037 포트에 하나뿐이라 버전이 다르면 서로 재시작시킨다. 호스트는 PATH의 adb를 먼저 쓰므로, Android Studio와 같은 adb를 PATH에 두면 충돌하지 않는다 |
| 특정 창에서만 클릭이 안 먹음 | 관리자 권한으로 실행 중인 창에는 주입할 수 없다(UIPI). 호스트도 관리자로 실행해야 한다 |
| 커서가 엉뚱한 곳으로 감 | 호스트의 **Self-test → coordinate accuracy sweep** 을 돌려본다. 0.00 px 가 정상이다 |

## 실행

```bash
./build/debug/host/digitiz_host.exe
```

전송 계층 없이도 동작한다. 창에는 주입 on/off 토글, 모니터 목록, 파이프라인 통계,
콘솔이 있고 **Self-test** 섹션에서 폰 없이 입력 주입을 검증할 수 있다.

폰이 USB로 연결돼 있고 USB 디버깅이 켜져 있으면 자동으로 reverse 터널을 세우고
게스트 앱을 실행한 뒤 접속을 기다린다. 게스트가 아직 없어도 문제없이 대기한다.

좌표 정확도 검사는 헤드리스로도 돌아간다 (커서를 25지점 훑고 원위치, 클릭 없음):

```bash
./build/debug/host/digitiz_host.exe --selftest
```

게스트 앱 없이 터널 전 구간을 확인하려면 (디바이스의 netcat을 씀):

```bash
./tools/test-tunnel.sh
```

## 빌드 (PC)

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

### Windows 주의

Ninja + MSVC 조합은 **컴파일러 환경이 셋업된 셸**을 요구한다.
"Developer Command Prompt for VS" 에서 실행하거나, 일반 셸이라면 먼저:

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
```

`Visual Studio` 제너레이터를 쓰지 않는 이유: 이 개발 환경에는 VS 18 Community가
설치돼 있는데 CMake 3.30에는 VS 18 제너레이터가 없다. CMake를 올리면 쓸 수 있지만
Ninja가 어차피 더 빠르고 크로스플랫폼이라 그대로 간다.

## 빌드 (Android)

```bash
./tools/guest.sh run      # 빌드 → 설치 → 실행 → 로그 tail
```

`build` / `install` / `log` 하위 명령도 있다. NDK r27 (`27.3.13750724`),
compileSdk 35, minSdk 24가 필요하다.

이 스크립트가 존재하는 이유는 이 개발 환경의 함정 두 개 때문이다.
`JAVA_HOME` 이 emsdk의 Java 8을 가리켜서 Gradle이 거부하고(Android Studio 번들
JBR 17로 덮어쓴다), Git Bash가 `/data/...` 인자를 Windows 경로로 바꿔버린다.

Android Studio로 열려면 `guest/` 를 프로젝트 루트로 연다. `local.properties` 는
gitignore 되어 있으므로 각자 만들어야 한다 (`sdk.dir=C:/path/to/Sdk`,
**백슬래시 금지** — Java Properties가 `\U` 를 유니코드 이스케이프로 읽는다).

## 레이아웃

```
common/   호스트·게스트 공유 C++. 플랫폼 헤더 금지 — 표준 라이브러리만
host/     PC 호스트 (Phase 2)
guest/    Android 게스트 (Phase 4)
docs/     설계 문서
```

`common/` 은 데스크톱 CMake와 안드로이드 CMake 양쪽에서 그대로 빌드된다.
그래서 저기에는 `<windows.h>` 도, `<android/*.h>` 도, 소켓 API도 들어가지 않는다.
