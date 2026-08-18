#!/usr/bin/env bash
# Build dpyes-ext DLL + injector for Windows x86 and x64 with llvm-mingw.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")" && pwd)}"
MH="$ROOT/third_party/minhook"
IMGUI="$ROOT/third_party/imgui"

# With no ARCH argument, build both Windows architectures in one invocation.
# ARCH remains supported so CI and advanced callers can build only one target.
if [[ -z "${ARCH:-}" ]]; then
  echo "building x64 and x86 DLLs + injectors..."
  ARCH=x64 \
    TARGET_TRIPLE="${TARGET_TRIPLE_X64:-x86_64-w64-mingw32}" \
    CC="${CC_X64:-}" CXX="${CXX_X64:-}" \
    OUT="${OUT_X64:-$ROOT/dpyes_ext-x64.dll}" \
    INJECTOR_OUT="${INJECTOR_OUT_X64:-$ROOT/dpyes_injector-x64.exe}" \
    "$0"
  ARCH=x86 \
    TARGET_TRIPLE="${TARGET_TRIPLE_X86:-i686-w64-mingw32}" \
    CC="${CC_X86:-}" CXX="${CXX_X86:-}" \
    OUT="${OUT_X86:-$ROOT/dpyes_ext-x86.dll}" \
    INJECTOR_OUT="${INJECTOR_OUT_X86:-$ROOT/dpyes_injector-x86.exe}" \
    "$0"
  echo "built both architectures:"
  echo "  $ROOT/dpyes_ext-x64.dll"
  echo "  $ROOT/dpyes_injector-x64.exe"
  echo "  $ROOT/dpyes_ext-x86.dll"
  echo "  $ROOT/dpyes_injector-x86.exe"
  exit 0
fi

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
# Explicit output values still take precedence.
OUT="${OUT:-$ROOT/dpyes_ext-${OUTPUT_ARCH}.dll}"
INJECTOR_OUT="${INJECTOR_OUT:-$(dirname "$OUT")/dpyes_injector-${OUTPUT_ARCH}.exe}"
BUILD_DIR="${BUILD_DIR:-$ROOT/.build/$OUTPUT_ARCH}"

find_c_compiler() {
  if [[ -n "${CC:-}" ]]; then
    printf '%s\n' "$CC"
  elif command -v "${TARGET_TRIPLE}-clang" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-clang"
  elif command -v "${TARGET_TRIPLE}-gcc" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-gcc"
  elif command -v "${TARGET_TRIPLE}-gcc-posix" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-gcc-posix"
  else
    local llvm_root="${LLVM_MINGW_ROOT:-/d/SDK/llvm-mingw/llvm-mingw-20260616-ucrt-x86_64}"
    if [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-clang" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-clang"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-clang.exe" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-clang.exe"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-gcc" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-gcc"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-gcc.exe" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-gcc.exe"
    else
      return 1
    fi
  fi
}

find_cxx_compiler() {
  if [[ -n "${CXX:-}" ]]; then
    printf '%s\n' "$CXX"
  elif command -v "${TARGET_TRIPLE}-clang++" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-clang++"
  elif command -v "${TARGET_TRIPLE}-g++" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-g++"
  elif command -v "${TARGET_TRIPLE}-g++-posix" >/dev/null 2>&1; then
    command -v "${TARGET_TRIPLE}-g++-posix"
  else
    local llvm_root="${LLVM_MINGW_ROOT:-/d/SDK/llvm-mingw/llvm-mingw-20260616-ucrt-x86_64}"
    if [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-clang++" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-clang++"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-clang++.exe" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-clang++.exe"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-g++" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-g++"
    elif [[ -x "$llvm_root/bin/${TARGET_TRIPLE}-g++.exe" ]]; then
      printf '%s\n' "$llvm_root/bin/${TARGET_TRIPLE}-g++.exe"
    else
      return 1
    fi
  fi
}

if ! COMPILER="$(find_c_compiler)"; then
  echo "error: no C compiler for $TARGET_TRIPLE found; set CC or LLVM_MINGW_ROOT" >&2
  exit 1
fi
if ! CXX_COMPILER="$(find_cxx_compiler)"; then
  echo "error: no C++ compiler for $TARGET_TRIPLE found; set CXX or LLVM_MINGW_ROOT" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR" "$(dirname "$OUT")" "$(dirname "$INJECTOR_OUT")"

CFLAGS=(
  -O2 -Wall -Wextra
  -I"$ROOT/src" -I"$MH/include"
)
CXXFLAGS=(
  -std=c++17 -O2 -Wall -Wextra -Wno-unknown-pragmas
  -I"$ROOT/src" -I"$MH/include" -I"$IMGUI"
)
# Intentional word splitting lets callers pass multiple additional flags.
# shellcheck disable=SC2206
EXTRA_CFLAGS_ARRAY=(${EXTRA_CFLAGS:-})
# shellcheck disable=SC2206
EXTRA_CXXFLAGS_ARRAY=(${EXTRA_CXXFLAGS:-})

C_SOURCES=(
  "$ROOT/src/dllmain.c"
  "$MH/src/buffer.c"
  "$MH/src/hook.c"
  "$MH/src/trampoline.c"
  "$HDE_SOURCE"
)
C_OBJECTS=()
for source in "${C_SOURCES[@]}"; do
  object="$BUILD_DIR/c_$(basename "${source%.*}").o"
  "$COMPILER" -c "${CFLAGS[@]}" "${EXTRA_CFLAGS_ARRAY[@]}" \
    "$source" -o "$object"
  C_OBJECTS+=("$object")
done

CXX_SOURCES=(
  "$ROOT/src/imgui_overlay.cpp"
  "$IMGUI/imgui.cpp"
  "$IMGUI/imgui_draw.cpp"
  "$IMGUI/imgui_tables.cpp"
  "$IMGUI/imgui_widgets.cpp"
  "$IMGUI/backends/imgui_impl_win32.cpp"
  "$IMGUI/backends/imgui_impl_dx9.cpp"
  "$IMGUI/backends/imgui_impl_dx11.cpp"
)
CXX_OBJECTS=()
for source in "${CXX_SOURCES[@]}"; do
  object="$BUILD_DIR/cxx_$(basename "${source%.*}").o"
  "$CXX_COMPILER" -c "${CXXFLAGS[@]}" "${EXTRA_CXXFLAGS_ARRAY[@]}" \
    "$source" -o "$object"
  CXX_OBJECTS+=("$object")
done

"$CXX_COMPILER" -shared "${C_OBJECTS[@]}" "${CXX_OBJECTS[@]}" \
  -o "$OUT" -static \
  -lkernel32 -luser32 -lgdi32 -limm32 -ldwmapi -lole32 -luuid \
  -ld3d9 -ld3d11 -ldxgi -ld3dcompiler \
  ${EXTRA_LDFLAGS:-} \
  -Wl,--kill-at -Wl,-subsystem,windows

"$CXX_COMPILER" "${CXXFLAGS[@]}" "${EXTRA_CXXFLAGS_ARRAY[@]}" \
  "$ROOT/src/injector.cpp" -o "$INJECTOR_OUT" \
  -static -municode -lkernel32 \
  ${EXTRA_INJECTOR_LDFLAGS:-}

echo "built: $OUT  arch=$ARCH target=$TARGET_TRIPLE ($(stat -c%s "$OUT") bytes)"
echo "built: $INJECTOR_OUT  arch=$ARCH target=$TARGET_TRIPLE ($(stat -c%s "$INJECTOR_OUT") bytes)"
