#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d "${TMPDIR:-/var/tmp}/q3-mac-toolbox-init.XXXXXX")"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# Issues #263 and #10: main initializes the Toolbox before anything else, and
# the Retro68 console window opens only when output is wanted.  mac_main.c and
# mac_console.c need the whole Mac Toolbox, so the fixture takes main and the
# functions it runs from them verbatim (with a #line so reports name those
# files) and fakes the Toolbox calls around them.  The real console hooks,
# mac_consolehooks.cc, are compiled as C++ against mac_console_window_fake.h
# in place of Retro68's retro/ConsoleWindow.h.
Q3_TEST_EXTRACT() {
    awk -v start="$1" -v file="$2" '
        $0 ~ start { printf "#line %d \"%s\"\n", NR, file; found = 1 }
        found { print }
        found && /^}/ { found = 0; count++ }
        END { exit count == 1 ? 0 : 1 }
    ' "$2" >> "$3" || { echo "run_mac_toolbox_init_tests: no single $1 in $2" >&2; exit 1; }
}
Q3_TEST_MAIN="$Q3_TEST_ROOT/src/mac9/mac_main.c"
Q3_TEST_CONSOLE="$Q3_TEST_ROOT/src/mac9/mac_console.c"
awk '/^#define[[:space:]]+MAC_(PARMS_FILE|COMMAND_LINE_SIZE)[[:space:]]/' "$Q3_TEST_MAIN" \
    > "$Q3_TEST_DIR/mac_main_extracted.c"
[ "$(wc -l < "$Q3_TEST_DIR/mac_main_extracted.c")" -eq 2 ] ||
    { echo "run_mac_toolbox_init_tests: no MAC_PARMS_FILE/MAC_COMMAND_LINE_SIZE in $Q3_TEST_MAIN" >&2; exit 1; }
Q3_TEST_EXTRACT '^static void[[:space:]]+Sys_InitToolbox[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Sys_LogPrintf[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^static int[[:space:]]+Sys_AppendStartupText[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^static int[[:space:]]+Sys_ReadStartupFile[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^static int[[:space:]]+Sys_StartupError[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^static void[[:space:]]+Sys_JoinHFSPath[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^int[[:space:]]+main[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Sys_Error[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_main_extracted.c"
Q3_TEST_EXTRACT '^int[[:space:]]+Sys_ConsoleWanted[[:space:]]*[(]' "$Q3_TEST_CONSOLE" "$Q3_TEST_DIR/mac_console_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Sys_ShowConsole[[:space:]]*[(]' "$Q3_TEST_CONSOLE" "$Q3_TEST_DIR/mac_console_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Sys_Print[[:space:]]*[(]' "$Q3_TEST_CONSOLE" "$Q3_TEST_DIR/mac_console_extracted.c"

# The C++ compiler that goes with $CC: clang++ for clang, g++ for gcc,
# be32_cxx.sh for be32_cc.sh (the ppc32 BE job), c++ for cc.  An exported CXX
# wins.  Either way it must target the same machine as $CC.  Behind
# tests/format_cc.sh (run_host_regressions.sh), $Q3_FORMAT_CC is the compiler.
if [ -z "${CXX:-}" ]; then
    Q3_TEST_REAL_CC="${Q3_FORMAT_CC:-${CC:-cc}}"
    case "$(basename -- "$Q3_TEST_REAL_CC")" in
        be32_cc.sh) CXX="$(dirname -- "$Q3_TEST_REAL_CC")/be32_cxx.sh" ;;
        clang*) CXX="$(dirname -- "$Q3_TEST_REAL_CC")/clang++${Q3_TEST_REAL_CC##*clang}" ;;
        *gcc*) CXX="$(dirname -- "$Q3_TEST_REAL_CC")/$(basename -- "$Q3_TEST_REAL_CC" | sed 's/gcc/g++/')" ;;
        cc) CXX=c++ ;;
        *) echo "run_mac_toolbox_init_tests: no C++ compiler known for CC=$Q3_TEST_REAL_CC; set CXX" >&2; exit 1 ;;
    esac
    case "$CXX" in ./*) CXX="${CXX#./}" ;; esac
fi
Q3_TEST_CC_MACHINE="$("${CC:-cc}" -dumpmachine 2>/dev/null)" ||
    { echo "run_mac_toolbox_init_tests: ${CC:-cc} -dumpmachine failed" >&2; exit 1; }
Q3_TEST_CXX_MACHINE="$("$CXX" -dumpmachine 2>/dev/null)" ||
    { echo "run_mac_toolbox_init_tests: C++ compiler $CXX (for CC=${CC:-cc}) is not available; install it or set CXX" >&2; exit 1; }
[ "$Q3_TEST_CC_MACHINE" = "$Q3_TEST_CXX_MACHINE" ] ||
    { echo "run_mac_toolbox_init_tests: CXX=$CXX targets $Q3_TEST_CXX_MACHINE, but CC=${CC:-cc} targets $Q3_TEST_CC_MACHINE; set CXX to a matching C++ compiler" >&2; exit 1; }
mkdir -p "$Q3_TEST_DIR/retro"
cp "$Q3_TEST_ROOT/tests/mac/mac_console_window_fake.h" "$Q3_TEST_DIR/retro/ConsoleWindow.h"
# -w: the hooks' "\pRetro68 Console" is a Retro68 Pascal string, which host
# GCC reads (with a warning) as plain "pRetro68 Console"; only the PPC build
# checks this file's warnings.
"$CXX" -std=gnu++11 -w -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I"$Q3_TEST_DIR" -c "$Q3_TEST_ROOT/src/mac9/mac_consolehooks.cc" \
    -o "$Q3_TEST_DIR/mac_consolehooks.o"
"${CC:-cc}" \
    -std=gnu99 -fsigned-char -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$Q3_TEST_DIR" -I"$Q3_TEST_ROOT/tests" \
    -c "$Q3_TEST_ROOT/tests/mac/mac_toolbox_init_regression.c" -o "$Q3_TEST_DIR/mac_toolbox_init.o"
"${CC:-cc}" \
    -std=gnu99 -fsigned-char -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined \
    -c "$Q3_TEST_ROOT/src/game/q_shared.c" -o "$Q3_TEST_DIR/q_shared.o"
"$CXX" -fsanitize=address,undefined \
    "$Q3_TEST_DIR/mac_toolbox_init.o" "$Q3_TEST_DIR/q_shared.o" "$Q3_TEST_DIR/mac_consolehooks.o" \
    -Wl,--gc-sections -lm -o "$Q3_TEST_DIR/mac_toolbox_init"

ASAN_OPTIONS=detect_leaks=${Q3_TEST_DETECT_LEAKS:-1}:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$Q3_TEST_DIR/mac_toolbox_init" "$Q3_TEST_DIR"

# The hooks stay in the link only because XCOFF ld ignores a later archive
# member's redefinition.  If mac_consolehooks.cc ever left the link,
# libRetroConsole's InitConsole.cc would quietly come back, with its
# InitWindows/InitMenus/InitCursor on the first console write.  The Mac build's
# static module check (cmake/EtMac.cmake) must refuse that: run
# static_modules.py with that file's --override list on maps in the Retro68 ld
# format.
# ET: the overrides are in cmake/EtMac.cmake, several to a line
mapfile -t Q3_TEST_OVERRIDES < <(grep -o -- '--override [^[:space:]]*' "$Q3_TEST_ROOT/cmake/EtMac.cmake" | tr ' ' '\n')
[ "${#Q3_TEST_OVERRIDES[@]}" -gt 0 ] ||
    { echo "run_mac_toolbox_init_tests: no --override in cmake/EtMac.cmake" >&2; exit 1; }
Q3_TEST_SYSCALLS='CMakeFiles/ET.dir/src/mac9/mac_syscalls.c.obj'
Q3_TEST_HOOKS='CMakeFiles/ET.dir/src/mac9/mac_consolehooks.cc.obj'
Q3_TEST_RETROCONSOLE='/opt/Retro68/powerpc-apple-macos/lib/libRetroConsole.a(InitConsole.cc.obj)'
q3_test_map() {
    printf 'Linker script and memory map\n\n.text           0x0000000000000000   0x400000\n'
    printf ' .pr            0x%016x       0x40 %s\n                0x%016x                ._open_r\n' 4096 "$Q3_TEST_SYSCALLS" 4096
    printf ' .pr            0x%016x       0x40 %s\n                0x%016x                ._rename_r\n' 4160 "$Q3_TEST_SYSCALLS" 4160
    printf ' .pr            0x%016x       0x40 %s\n                0x%016x                ._unlink_r\n' 4224 "$Q3_TEST_SYSCALLS" 4224
    while [ $# -gt 0 ]; do
        printf ' .pr            0x%016x       0x40 %s\n' "$3" "$2"
        printf '                0x%016x                %s\n' "$3" "$1"
        shift 3
    done
}
q3_test_overrides() {
    python3 "$Q3_TEST_ROOT/cmake/static_modules.py" overrides --map "$Q3_TEST_DIR/link.map" \
        "${Q3_TEST_OVERRIDES[@]}" > /dev/null 2>&1
}
q3_test_map ._consolewrite "$Q3_TEST_HOOKS" 8192 ._consoleread "$Q3_TEST_HOOKS" 8256 > "$Q3_TEST_DIR/link.map"
q3_test_overrides || { echo "the Mac build's override check refused mac_consolehooks.cc.obj's hooks" >&2; exit 1; }
for Q3_TEST_BAD in write read both; do
    case "$Q3_TEST_BAD" in
        write) q3_test_map ._consolewrite "$Q3_TEST_RETROCONSOLE" 12288 ._consoleread "$Q3_TEST_HOOKS" 8256 ;;
        read) q3_test_map ._consolewrite "$Q3_TEST_HOOKS" 8192 ._consoleread "$Q3_TEST_RETROCONSOLE" 12352 ;;
        both) q3_test_map ._consolewrite "$Q3_TEST_RETROCONSOLE" 12288 ._consoleread "$Q3_TEST_RETROCONSOLE" 12352 ;;
    esac > "$Q3_TEST_DIR/link.map"
    if q3_test_overrides; then
        echo "the Mac build's override check accepted libRetroConsole's console hooks ($Q3_TEST_BAD)" >&2
        exit 1
    fi
done
echo "console hook override check passed: 1 good and 3 bad maps"
