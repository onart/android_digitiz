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
- [ ] Phase 5 — 통합 (지연 측정, 재연결/백그라운드 처리, 세팅 가이드)

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
