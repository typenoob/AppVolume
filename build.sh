#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="$ROOT/dist"
mkdir -p "$OUTPUT"

if [[ "$(uname -s)" == Darwin ]]; then
    SDKROOT="${SDKROOT:-$(xcrun --sdk iphoneos --show-sdk-path)}"
    CLANG="${CLANG:-$(xcrun --find clang)}"
    LINKER_FLAGS=()
else
    LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-19/bin}"
    CLANG="${CLANG:-$LLVM_BIN/clang}"
    SDKROOT="${SDKROOT:-$ROOT/.toolchain/iPhoneOS15.6.sdk}"
    LINKER_FLAGS=("--ld-path=$LLVM_BIN/ld64.lld" -Wl,-adhoc_codesign -Wl,-platform_version,ios,15.0,15.5)
fi

if [[ ! -d "$SDKROOT/System/Library/Frameworks/UIKit.framework" ]]; then
    printf 'Set SDKROOT to an extracted iPhoneOS SDK (15.0 or newer). Current: %s\n' "$SDKROOT" >&2
    exit 1
fi

"${HOST_CC:-cc}" -std=c11 -Wall -Wextra -Werror "$ROOT/tests/volume_policy_test.c" -lm -o "$OUTPUT/volume_policy_test"
"$OUTPUT/volume_policy_test"

"$CLANG" -target arm64-apple-ios15.0 -isysroot "$SDKROOT" \
    -fobjc-arc -fblocks -fobjc-exceptions -fvisibility=hidden \
    -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations \
    -dynamiclib "${LINKER_FLAGS[@]}" \
    -Wl,-install_name,@rpath/AppVolume.dylib \
    "$ROOT/src/AppVolume.m" "$ROOT/src/AudioQueueVolume.c" "$ROOT/vendor/fishhook/fishhook.c" \
    -framework Foundation -framework UIKit -framework AVFoundation -framework CoreMedia \
    -framework AudioToolbox -framework CoreFoundation -lobjc -lpthread \
    -o "$OUTPUT/AppVolume.dylib"

if [[ "$(uname -s)" == Darwin ]]; then
    codesign --force --sign - "$OUTPUT/AppVolume.dylib"
fi
printf 'Built: %s\n' "$OUTPUT/AppVolume.dylib"
