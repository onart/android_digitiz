#!/usr/bin/env bash
# Build / install / run the Android guest.
#
#   ./tools/guest.sh build      assemble the debug APK
#   ./tools/guest.sh install    build, then install on the connected device
#   ./tools/guest.sh run        install, restart the app, tail its log
#   ./tools/guest.sh log        tail the guest log
#
# Two environment traps this script exists to avoid:
#
#  1. JAVA_HOME on this machine points at a Java 8 from emsdk, which Gradle
#     rejects. Android Studio's bundled JBR 17 is used instead.
#  2. Git Bash rewrites /data/... arguments into Windows paths, so adb needs
#     MSYS_NO_PATHCONV=1 — and once that is set, JAVA_HOME must already be in
#     Windows form or Gradle cannot find it either.

set -euo pipefail
export MSYS_NO_PATHCONV=1

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUEST="$HERE/guest"

: "${JAVA_HOME:=C:/Program Files/Android/Android Studio/jbr}"
if [ ! -d "$JAVA_HOME" ] || [ ! -x "$JAVA_HOME/bin/java.exe" ]; then
    JAVA_HOME="C:/Program Files/Android/Android Studio/jbr"
fi
export JAVA_HOME

ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
APK="$GUEST/app/build/outputs/apk/debug/app-debug.apk"
PKG="com.onart.digitiz"
ACTIVITY="$PKG/.MainActivity"

build() {
    (cd "$GUEST" && ./gradlew.bat :app:assembleDebug "$@")
}

install() {
    build "$@"
    "$ADB" install -r "$APK"
}

case "${1:-run}" in
build)
    shift || true
    build "$@"
    ;;
install)
    shift || true
    install "$@"
    ;;
run)
    shift || true
    install "$@"
    "$ADB" shell am force-stop "$PKG"
    "$ADB" logcat -c
    "$ADB" shell am start -n "$ACTIVITY" >/dev/null
    echo "== tailing guest log, Ctrl-C to stop =="
    "$ADB" logcat -s digitiz:V AndroidRuntime:E
    ;;
log)
    "$ADB" logcat -s digitiz:V AndroidRuntime:E
    ;;
*)
    echo "usage: $0 {build|install|run|log}" >&2
    exit 2
    ;;
esac
