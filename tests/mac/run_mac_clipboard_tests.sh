#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d -p "${TMPDIR:-/var/tmp}" q3-mac-clipboard.XXXXXX)"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# Issue #266: classic Mac paste.  mac_main.c needs the whole Mac Toolbox, so
# the fixture takes Sys_ClipboardText and Sys_GetClipboardData from it
# verbatim, with a #line so reports name that file.
Q3_TEST_EXTRACT() {
    awk -v start="$1" -v file="$2" '
        { sub(/\r$/, "") }
        $0 ~ start { printf "#line %d \"%s\"\n", NR, file; found = 1 }
        found { print }
        found && /^}/ { found = 0; count++ }
        END { exit count == 1 ? 0 : 1 }
    ' "$2" >> "$3" || { echo "run_mac_clipboard_tests: no single $1 in $2" >&2; exit 1; }
}
Q3_TEST_MAIN="$Q3_TEST_ROOT/src/mac9/mac_main.c"
Q3_TEST_EXTRACT '^static[[:space:]]+char[[:space:]]+[*]Sys_ClipboardText[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_clipboard_extracted.c"
Q3_TEST_EXTRACT '^char[[:space:]]+[*]Sys_GetClipboardData[[:space:]]*[(]' "$Q3_TEST_MAIN" "$Q3_TEST_DIR/mac_clipboard_extracted.c"

"${CC:-cc}" -std=gnu99 -fsigned-char -Wall -Wno-unused-function -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$Q3_TEST_DIR" \
    "$Q3_TEST_ROOT/tests/mac/mac_clipboard_regression.c" -o "$Q3_TEST_DIR/mac_clipboard"
"$Q3_TEST_DIR/mac_clipboard"
