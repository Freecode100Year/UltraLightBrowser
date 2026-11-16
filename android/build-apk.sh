#!/usr/bin/env bash
# Builds the Android app: the Go network helper for each ABI, then the APKs.
#   GRADLE=/path/to/gradle android/build-apk.sh [debug|release]
# Needs Go (version in warp/go.mod), JDK 17+, the Android SDK (ANDROID_HOME) and Gradle 9.
# Release signing uses android/keystore.properties; without it the APKs are unsigned.
set -euo pipefail
cd "$(dirname "$0")"
TYPE=${1:-release}
LIBS=app/src/main/jniLibs
rm -rf "$LIBS"
build() { # abi goarch [goarm]
    mkdir -p "$LIBS/$1"
    # Plain Linux binaries run on Android; no NDK needed. See warp/dns.go.
    CGO_ENABLED=0 GOOS=linux GOARCH=$2 GOARM=${3:-} go build -C ../warp -trimpath -ldflags "-s -w -buildid=" -o "$PWD/$LIBS/$1/libulbwarp.so" .
}
build arm64-v8a arm64
build armeabi-v7a arm 7
build x86_64 amd64
"${GRADLE:-gradle}" --no-daemon -q "assemble${TYPE^}"
ls -l app/build/outputs/apk/"$TYPE"/*.apk
