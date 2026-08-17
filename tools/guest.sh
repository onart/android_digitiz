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
#  2. JAVA_HOME is given in Windows form. Note this script deliberately does
#     NOT set MSYS_NO_PATHCONV: every path here is a host path that Git Bash
#     should translate. Only scripts that pass device paths like /data/local/tmp
#     need to suppress that (see test-tunnel.sh).

set -euo pipefail

# Every path this script hands to adb is a host path that Git Bash should
# translate. A caller that exported MSYS_NO_PATHCONV for its own device-path
# work would otherwise break `adb install` here, and adb reports that as a
# plain "failed to stat" that is easy to scroll past.
unset MSYS_NO_PATHCONV

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUEST="$HERE/guest"

# Overridden unconditionally, not defaulted: the inherited JAVA_HOME on this
# machine points at a Java 8 from emsdk, and Gradle fails with a confusing
# "no matching variant ... compatible with Java 8" rather than saying so.
# Set DIGITIZ_JAVA_HOME to use a different JDK.
JAVA_HOME="${DIGITIZ_JAVA_HOME:-C:/Program Files/Android/Android Studio/jbr}"
export JAVA_HOME

if [ ! -x "$JAVA_HOME/bin/java.exe" ] && [ ! -x "$JAVA_HOME/bin/java" ]; then
    echo "no JDK at JAVA_HOME=$JAVA_HOME" >&2
    echo "set DIGITIZ_JAVA_HOME to a JDK 17+ installation" >&2
    exit 1
fi

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

    # adb reports success loudly and staleness not at all, so confirm the APK
    # on the device is byte-for-byte the one just built. Chasing a phantom bug
    # in code that was never installed is an expensive afternoon.
    local remote local_sum remote_sum
    remote="$("$ADB" shell pm path "$PKG" | tr -d '\r' | sed 's/package://')"
    local_sum="$(md5sum "$APK" | cut -d' ' -f1)"
    remote_sum="$("$ADB" shell md5sum "$remote" | tr -d '\r' | cut -d' ' -f1)"

    if [ "$local_sum" != "$remote_sum" ]; then
        echo "installed APK does not match the build output" >&2
        echo "  local  $local_sum  $APK" >&2
        echo "  device $remote_sum  $remote" >&2
        exit 1
    fi
    echo "verified on device: $local_sum"
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
