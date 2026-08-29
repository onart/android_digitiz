# 서드파티 라이선스

Wired Phone Digitizer 는 아래 오픈소스를 사용합니다. 호스트(Windows)와
게스트(Android)가 쓰는 것이 다르므로 나누어 적습니다.

## 호스트 (Windows 실행 파일에 포함)

| 구성요소 | 버전 | 라이선스 |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.91.5 | MIT |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib/libpng |
| [Zstandard](https://github.com/facebook/zstd) | v1.5.6 | BSD 3-Clause |

## 게스트 (APK에 포함)

| 구성요소 | 버전 | 라이선스 |
|---|---|---|
| [androidx.games:games-activity](https://developer.android.com/games/agdk/game-activity) | 3.0.5 | Apache 2.0 |
| [androidx.appcompat](https://developer.android.com/jetpack/androidx) | 1.7.0 | Apache 2.0 |
| [Kotlin standard library](https://github.com/JetBrains/kotlin) | 1.9.24 | Apache 2.0 |
| [Zstandard](https://github.com/facebook/zstd) | v1.5.6 | BSD 3-Clause |

## 빌드에만 쓰이고 배포물에는 들어가지 않는 것

| 구성요소 | 버전 | 라이선스 |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) | v2.4.11 | MIT |

---

## Dear ImGui — MIT License

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## doctest — MIT License

Copyright (c) 2016-2023 Viktor Kirilov

(전문은 위 Dear ImGui 의 MIT 라이선스와 동일한 조문입니다.)

---

## GLFW — zlib/libpng License

Copyright (c) 2002-2006 Marcus Geelnard
Copyright (c) 2006-2019 Camilla Löwy

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use
of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.

---

## Zstandard — BSD 3-Clause License

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta Platforms, nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

zstd 는 BSD 3-Clause 와 GPLv2 의 이중 라이선스이며, 이 프로젝트는 BSD 3-Clause
조건으로 사용합니다.

---

## androidx (games-activity, appcompat) 및 Kotlin 표준 라이브러리 — Apache License 2.0

Copyright (c) The Android Open Source Project
Copyright (c) JetBrains s.r.o. and Kotlin Programming Language contributors

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
these files except in compliance with the License. You may obtain a copy of the
License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

**전문은 `licenses/Apache-2.0.txt` 에 포함되어 있습니다.** Apache 2.0 제4조는
배포물에 라이선스 사본을 함께 줄 것을 요구하므로, 릴리스 압축본에도 이 파일이
들어갑니다.

---

## 이 프로젝트 자체의 라이선스

**아직 정해지지 않았습니다.** 라이선스 파일이 없는 저장소는 기본적으로 모든 권리가
저작자에게 유보된 것으로 취급되므로, 배포할 계획이라면 루트에 `LICENSE` 를 두는
편이 낫습니다.
