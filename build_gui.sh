#!/usr/bin/env bash
# Build the GUI and its matching engine from the current local source tree.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_type=Release
build_dir=""
qt_prefix=""
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-}"

usage() {
    cat <<'HELP'
Usage: ./build_gui.sh [options]

Build the current checkout's GUI, engine and AI Markdown instructions.
No git pull, source deletion or version change is performed.

  --debug             Use Debug configuration (default: Release)
  --build-dir PATH    Output directory (default: repo/build or repo/build-debug)
  --qt-prefix PATH    Qt installation prefix; macOS Homebrew Qt is auto-detected
  --jobs N, -j N      Parallel build jobs (default: CPU count, capped at 8)
  --help, -h         Show this help

Examples:
  ./build_gui.sh
  ./build_gui.sh --debug
  ./build_gui.sh --qt-prefix /path/to/Qt/6.8.3/macos --jobs 8
HELP
}
fail() { printf 'Error: %s\n' "$*" >&2; exit 2; }
value_required() { [[ $# -ge 2 && -n "$2" ]] || fail "$1 requires a value"; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) build_type=Debug; shift ;;
        --build-dir) value_required "$@"; build_dir="$2"; shift 2 ;;
        --qt-prefix) value_required "$@"; qt_prefix="$2"; shift 2 ;;
        --jobs|-j) value_required "$@"; jobs="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
command -v cmake >/dev/null 2>&1 || fail 'CMake 3.26+ is required. On macOS: brew install cmake qtbase'
if [[ -z "$jobs" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4')"
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || jobs=4
    if (( jobs > 8 )); then jobs=8; fi
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail '--jobs must be a positive integer'
if [[ -z "$build_dir" ]]; then
    build_dir="$repo_dir/build"
    if [[ "$build_type" == Debug ]]; then build_dir="$repo_dir/build-debug"; fi
fi
mkdir -p -- "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
[[ "$build_dir" != "$repo_dir" ]] || fail 'Use a separate build directory, not the source directory'

# Honor explicit Qt/CMake environment settings before probing Homebrew.
if [[ -z "$qt_prefix" && "$(uname -s)" == Darwin && -z "${CMAKE_PREFIX_PATH:-}" && -z "${Qt6_DIR:-}" && -z "${Qt6_ROOT:-}" ]]; then
    if command -v brew >/dev/null 2>&1; then
        for formula in qtbase qt; do
            candidate="$(brew --prefix "$formula" 2>/dev/null || true)"
            if [[ -f "$candidate/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
                qt_prefix="$candidate"; break
            fi
        done
    fi
fi
configure_args=(-S "$repo_dir" -B "$build_dir" "-DCMAKE_BUILD_TYPE=$build_type" -DGARLIC_BUILD_GUI=ON)
if [[ -n "$qt_prefix" ]]; then
    [[ -f "$qt_prefix/lib/cmake/Qt6/Qt6Config.cmake" ]] || fail "Qt6Config.cmake not found under $qt_prefix/lib/cmake/Qt6"
    configure_args+=("-DCMAKE_PREFIX_PATH=$qt_prefix" "-DQt6_DIR=$qt_prefix/lib/cmake/Qt6")
fi
if [[ -z "$qt_prefix" && -n "${Qt6_DIR:-}" ]]; then configure_args+=("-DQt6_DIR=$Qt6_DIR"); fi
if [[ -z "$qt_prefix" && -n "${Qt6_ROOT:-}" ]]; then configure_args+=("-DQt6_ROOT=$Qt6_ROOT"); fi
# Reuse an existing build's generator; select Ninja only for a new build.
if [[ ! -f "$build_dir/CMakeCache.txt" && -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null 2>&1; then
    configure_args+=(-G Ninja)
fi
printf 'Building %s GUI from %s\nOutput: %s\n' "$build_type" "$repo_dir" "$build_dir"
cmake "${configure_args[@]}"
cmake --build "$build_dir" --config "$build_type" --target garlic-gui --parallel "$jobs"

# Handle single- and multi-configuration generators.
gui_path=""
for candidate in "$build_dir/gui/$build_type/garlic-gui.app/Contents/MacOS/garlic-gui" \
                 "$build_dir/gui/garlic-gui.app/Contents/MacOS/garlic-gui" \
                 "$build_dir/gui/$build_type/garlic-gui.exe" "$build_dir/gui/garlic-gui.exe" \
                 "$build_dir/gui/$build_type/garlic-gui" "$build_dir/gui/garlic-gui"; do
    if [[ -f "$candidate" ]]; then gui_path="$candidate"; break; fi
done
[[ -n "$gui_path" ]] || fail 'Build finished but the GUI executable was not found'
"$gui_path" --version
"$gui_path" --headless --version
printf '\nGUI: %s\nAI instructions: %s/gui/AI-HEADLESS.md\n' "$gui_path" "$build_dir"
if [[ "$gui_path" == *.app/Contents/MacOS/garlic-gui ]]; then
    printf 'Launch: open "%s"\n' "${gui_path%/Contents/MacOS/garlic-gui}"
else
    printf 'Launch: "%s"\n' "$gui_path"
fi
