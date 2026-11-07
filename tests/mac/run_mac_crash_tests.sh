#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d -p "${TMPDIR:-/var/tmp}" q3-mac-crash.XXXXXX)"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# src/mac9/mac_crash.c, included whole by the regression; its Toolbox headers
# resolve to tests/mac_crash_fake.h.
for Q3_TEST_HEADER in MacTypes.h MacMemory.h MachineExceptions.h; do
    printf '#include "%s/tests/mac/mac_crash_fake.h"\n' "$Q3_TEST_ROOT" > "$Q3_TEST_DIR/$Q3_TEST_HEADER"
done

"${CC:-cc}" \
    -std=gnu99 -Wall -Wno-pointer-to-int-cast -Wno-unused-function -fno-omit-frame-pointer \
    -fsanitize=address,undefined -I"$Q3_TEST_DIR" \
    "$Q3_TEST_ROOT/tests/mac/mac_crash_regression.c" \
    -o "$Q3_TEST_DIR/mac_crash"

ASAN_OPTIONS=detect_leaks=${Q3_TEST_DETECT_LEAKS:-1}:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$Q3_TEST_DIR/mac_crash"

# mac_symbolize.py against a stand-in for addr2line, which prints a function
# and a line for each address it is given.
cat > "$Q3_TEST_DIR/addr2line" <<'TOOL'
#!/bin/sh
shift 3
for address in "$@"; do
    printf '.func_%s\n/src/file.c:%s\n' "$address" "$address"
done
TOOL
chmod +x "$Q3_TEST_DIR/addr2line"
: > "$Q3_TEST_DIR/fake.xcoff"
printf '[     1.000] an ordinary line\n[     2.000] *** PC 10001000 code+0x1000\n[     2.000] *** frame  0 10002000 code+0x2000\n' > "$Q3_TEST_DIR/q3boot.log"
Q3_TEST_OUTPUT="$(ADDR2LINE="$Q3_TEST_DIR/addr2line" python3 "$Q3_TEST_ROOT/mac_symbolize.py" "$Q3_TEST_DIR/q3boot.log" "$Q3_TEST_DIR/fake.xcoff")"
case "$Q3_TEST_OUTPUT" in
    *'*** PC 10001000 func_0x1000 at /src/file.c:0x1000'*'*** frame  0 10002000 func_0x2000 at /src/file.c:0x2000'*) ;;
    *) printf 'mac_symbolize.py gave:\n%s\n' "$Q3_TEST_OUTPUT" >&2; exit 1 ;;
esac
case "$Q3_TEST_OUTPUT" in
    *ordinary*) echo 'mac_symbolize.py printed a line with no address' >&2; exit 1 ;;
esac
printf '[     1.000] an ordinary line\n' > "$Q3_TEST_DIR/q3boot.log"
ADDR2LINE="$Q3_TEST_DIR/addr2line" python3 "$Q3_TEST_ROOT/mac_symbolize.py" "$Q3_TEST_DIR/q3boot.log" "$Q3_TEST_DIR/fake.xcoff" | grep -q 'No crash addresses'
echo "mac_symbolize.py turns log offsets into source lines"
