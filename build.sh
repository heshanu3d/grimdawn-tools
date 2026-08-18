#!/usr/bin/env bash
# Build dpyes-ext as a Windows x86 or x64 DLL with llvm-mingw/MinGW.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")" && pwd)}"
MH="$ROOT/third_party/minhook"
ARCH="${ARCH:-x64}"

case "$ARCH" in
  x64|x86_64|amd64)
    OUTPUT_ARCH="x64"
    TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-w64-mingw32}"
    HDE_SOURCE="$MH/src/hde/hde64.c"
    ;;
  x86|x32|i686)
    OUTPUT_ARCH="x86"
    TARGET_TRIPLE="${TARGET_TRIPLE:-i686-w64-mingw32}"
    HDE_SOURCE="$MH/src/hde/hde32.c"
    ;;
  *)
    echo "error: unsupported ARCH=$ARCH (expected x86 or x64)" >&2
    exit 1
    ;;
esac

# Keep local output names consistent with the GitHub Actions artifacts.
# An explicit OUT value still takes precedence for callers that need another path.
OUT="${OUT:-$ROOT/dpyes_ext-${OUTPUT_ARCH}.dll}"

if [[ -n "${CC:-}" ]]; then
  COMPILER="$CC"
elif command -v "${TARGET_TRIPLE}-clang" >/dev/null 2>&1; then
  COMPILER="$(command -v "${TARGET_TRIPLE}-clang")"
elif command -v "${TARGET_TRIPLE}-gcc" >/dev/null 2>&1; then
  COMPILER="$(command -v "${TARGET_TRIPLE}-gcc")"
elif command -v "${TARGET_TRIPLE}-gcc-posix" >/dev/null 2>&1; then
  COMPILER="$(command -v "${TARGET_TRIPLE}-gcc-posix")"
else
  LLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-/d/SDK/llvm-mingw/llvm-mingw-20260616-ucrt-x86_64}"
  if [[ -x "$LLVM_MINGW_ROOT/bin/${TARGET_TRIPLE}-clang.exe" ]]; then
    COMPILER="$LLVM_MINGW_ROOT/bin/${TARGET_TRIPLE}-clang.exe"
  elif [[ -x "$LLVM_MINGW_ROOT/bin/${TARGET_TRIPLE}-gcc.exe" ]]; then
    COMPILER="$LLVM_MINGW_ROOT/bin/${TARGET_TRIPLE}-gcc.exe"
  else
    echo "error: no compiler for $TARGET_TRIPLE found; set CC or LLVM_MINGW_ROOT" >&2
    exit 1
  fi
fi

mkdir -p "$(dirname "$OUT")"

"$COMPILER" -shared -O2 -Wall -Wextra \
  ${EXTRA_CFLAGS:-} \
  -I"$ROOT/src" -I"$MH/include" \
  "$ROOT/src/dllmain.c" \
  "$MH/src/buffer.c" "$MH/src/hook.c" "$MH/src/trampoline.c" "$HDE_SOURCE" \
  -o "$OUT" \
  -static -lkernel32 -luser32 -lgdi32 \
  ${EXTRA_LDFLAGS:-} \
  -Wl,--kill-at -Wl,-subsystem,windows

echo "built: $OUT  arch=$ARCH target=$TARGET_TRIPLE ($(stat -c%s "$OUT") bytes)"
