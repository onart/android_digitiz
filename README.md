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
- [ ] Phase 2 — 호스트 골격 (ImGui 셸, 화면 정보, 입력 주입)
- [ ] Phase 3 — ADB 전송
- [ ] Phase 4 — 게스트 (GameActivity + GLES3)
- [ ] Phase 5 — 통합

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

Phase 4에서 추가. Android Studio로 `guest/` 를 연다.
필요 항목은 NDK r27 (`27.3.13750724`), compileSdk 35, minSdk 24.

## 레이아웃

```
common/   호스트·게스트 공유 C++. 플랫폼 헤더 금지 — 표준 라이브러리만
host/     PC 호스트 (Phase 2)
guest/    Android 게스트 (Phase 4)
docs/     설계 문서
```

`common/` 은 데스크톱 CMake와 안드로이드 CMake 양쪽에서 그대로 빌드된다.
그래서 저기에는 `<windows.h>` 도, `<android/*.h>` 도, 소켓 API도 들어가지 않는다.
