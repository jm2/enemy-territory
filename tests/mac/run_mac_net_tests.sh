#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d -p "${TMPDIR:-/var/tmp}" q3-mac-net.XXXXXX)"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# Issues #277, #265, #20, #21 and #33: the Mac Open Transport receive,
# resolve, start-up and sleep paths.  mac_net.c needs the whole Mac Toolbox,
# so the fixture takes from it verbatim (with a #line so reports name that
# file) its globals, the start-up and shutdown functions with the helpers and
# notifiers before them, Sys_StringToAdr with its helper, Sys_GetPacket and
# NET_Sleep, and Com_EventLoop from common.c; tests/mac_ot_fake.h stands in
# for OT.
Q3_TEST_EXTRACT() {
    awk -v start="$1" -v file="$2" '
        $0 ~ start { printf "#line %d \"%s\"\n", NR, file; found = 1 }
        found { print }
        found && /^}/ { found = 0; count++ }
        END { exit count == 1 ? 0 : 1 }
    ' "$2" >> "$3" || { echo "run_mac_net_tests: no single $1 in $2" >&2; exit 1; }
}
# Everything from the first line matching $1 to the end of the function $2.
Q3_TEST_EXTRACT_SPAN() {
    awk -v first="$1" -v last="$2" -v file="$3" '
        !found && !done && $0 ~ first { printf "#line %d \"%s\"\n", NR, file; found = 1 }
        found { print }
        found && $0 ~ last { inlast = 1 }
        inlast && /^}/ { found = inlast = 0; done = 1 }
        END { exit done ? 0 : 1 }
    ' "$3" > "$4" || { echo "run_mac_net_tests: no $1 .. $2 in $3" >&2; exit 1; }
}
Q3_TEST_NET="$Q3_TEST_ROOT/src/mac9/mac_net.c"
# the globals: from gOTInited to the first function
awk -v file="$Q3_TEST_NET" '
    /^static[[:space:]]+qboolean[[:space:]]+gOTInited/ { printf "#line %d \"%s\"\n", NR, file; found = 1 }
    found && /^void[[:space:]]+RcvUDErr/ { found = 0; done = 1 }
    found { print }
    END { exit done ? 0 : 1 }
' "$Q3_TEST_NET" > "$Q3_TEST_DIR/mac_net_globals.c" ||
    { echo "run_mac_net_tests: no globals in mac_net.c" >&2; exit 1; }
Q3_TEST_EXTRACT_SPAN '^(static[[:space:]]+pascal[[:space:]]+void[[:space:]]+NET_EndpointNotify|static[[:space:]]+void[[:space:]]+NET_CloseOpenTransport|void[[:space:]]+Sys_InitNetworking)[[:space:]]*[(]' \
    '^void[[:space:]]+Sys_ShutdownNetworking[[:space:]]*[(]' "$Q3_TEST_NET" "$Q3_TEST_DIR/mac_net_init_extracted.c"
Q3_TEST_EXTRACT_SPAN '^(static[[:space:]]+qboolean[[:space:]]+NET_StringToHost|qboolean[[:space:]]+Sys_StringToAdr)[[:space:]]*[(]' \
    '^qboolean[[:space:]]+Sys_StringToAdr[[:space:]]*[(]' "$Q3_TEST_NET" "$Q3_TEST_DIR/mac_net_resolve_extracted.c"
Q3_TEST_EXTRACT_SPAN '^#define[[:space:]]+DROPPED_PACKET_REPORT_MSEC' \
    '^static[[:space:]]+void[[:space:]]+Sys_ReportDroppedPacket' "$Q3_TEST_NET" "$Q3_TEST_DIR/mac_net_extracted.c"
Q3_TEST_EXTRACT '^qboolean[[:space:]]+Sys_GetPacket[[:space:]]*[(]' \
    "$Q3_TEST_NET" "$Q3_TEST_DIR/mac_net_extracted.c"
Q3_TEST_EXTRACT '^void[[:space:]]+NET_Sleep[[:space:]]*[(]' \
    "$Q3_TEST_NET" "$Q3_TEST_DIR/mac_net_sleep_extracted.c"
Q3_TEST_EXTRACT '^int[[:space:]]+Com_EventLoop[[:space:]]*[(]' \
    "$Q3_TEST_ROOT/src/qcommon/common.c" "$Q3_TEST_DIR/com_event_loop_extracted.c"

"${CC:-cc}" \
    -std=gnu99 -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I"$Q3_TEST_DIR" \
    "$Q3_TEST_ROOT/tests/mac/mac_net_regression.c" \
    -Wl,--gc-sections -o "$Q3_TEST_DIR/mac_net"

# One process per case: AddressSanitizer stops at the first overflow.
for Q3_TEST_CASE in host-normal host-255 host-256 host-1023 \
        packet-normal packet-split packet-full packet-drain-error packet-dropped-reports event-oversize \
        init-default init-cvars init-port-busy init-port-reassigned init-port-exhausted \
        init-noudp init-fail-ot init-fail-config init-fail-open init-fail-nonblocking \
        init-fail-resolver-open init-fail-resolver-bind init-fail-net-ip \
        init-fail-notifier init-fail-resolver-notifier init-fail-resolver-async \
        host-dotted host-disabled resolve-async resolve-error resolve-ot-timeout \
        resolve-timeout resolve-cancel \
        sleep-timeout sleep-packet sleep-queued sleep-event sleep-client; do
    ASAN_OPTIONS=detect_leaks=${Q3_TEST_DETECT_LEAKS:-1}:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$Q3_TEST_DIR/mac_net" "$Q3_TEST_CASE"
done
