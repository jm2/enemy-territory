#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d -p "${TMPDIR:-/var/tmp}" q3-mac-startup-parms.XXXXXX)"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# Issues #261 and #24: the classic Mac startup parameters.  mac_main.c needs
# the whole Mac Toolbox, so the fixture takes Sys_AppendStartupText and
# Sys_ReadStartupFile from it verbatim (with a #line so reports name that
# file), and Com_ParseCommandLine and Com_SafeMode from common.c and
# Cmd_TokenizeString, Cmd_Argc and Cmd_Argv from cmd.c, which consume the
# command line they build.
Q3_TEST_EXTRACT() {
    awk -v start="$1" -v file="$2" '
        $0 ~ start { printf "#line %d \"%s\"\n", NR, file; found = 1 }
        found { print }
        found && /^}/ { found = 0; count++ }
        END { exit count == 1 ? 0 : 1 }
    ' "$2" >> "$3" || { echo "run_mac_startup_parms_tests: no single $1 in $2" >&2; exit 1; }
}
Q3_TEST_EXTRACT '^static int[[:space:]]+Sys_AppendStartupText[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/mac9/mac_main.c" "$Q3_TEST_DIR/mac_startup_extracted.c"
Q3_TEST_EXTRACT '^static int[[:space:]]+Sys_ReadStartupFile[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/mac9/mac_main.c" "$Q3_TEST_DIR/mac_startup_extracted.c"
Q3_TEST_EXTRACT '^static void[[:space:]]+Sys_MaskStartupSecrets[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/mac9/mac_main.c" "$Q3_TEST_DIR/mac_startup_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Com_ParseCommandLine[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/common.c" "$Q3_TEST_DIR/com_command_line_extracted.c"
Q3_TEST_EXTRACT '^qboolean[[:space:]]+Com_SafeMode[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/common.c" "$Q3_TEST_DIR/com_command_line_extracted.c"
Q3_TEST_EXTRACT '^int[[:space:]]+Cmd_Argc[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/cmd.c" "$Q3_TEST_DIR/cmd_tokenize_extracted.c"
Q3_TEST_EXTRACT '^char[[:space:]]*[*]Cmd_Argv[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/cmd.c" "$Q3_TEST_DIR/cmd_tokenize_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+Cmd_TokenizeString[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/cmd.c" "$Q3_TEST_DIR/cmd_tokenize_extracted.c"

# -fsigned-char as on the PowerPC target (CMakeLists.txt): Cmd_TokenizeString
# treats Mac Roman bytes as whitespace outside quotes, as retail does.
"${CC:-cc}" \
    -std=gnu99 -fsigned-char -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$Q3_TEST_DIR" \
    "$Q3_TEST_ROOT/tests/mac/mac_startup_parms_regression.c" "$Q3_TEST_ROOT/src/game/q_shared.c" \
    -Wl,--gc-sections -lm -o "$Q3_TEST_DIR/mac_startup_parms"

ASAN_OPTIONS=detect_leaks=${Q3_TEST_DETECT_LEAKS:-1}:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$Q3_TEST_DIR/mac_startup_parms" "$Q3_TEST_DIR"
