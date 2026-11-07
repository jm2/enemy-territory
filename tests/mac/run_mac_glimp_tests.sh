#!/usr/bin/env bash
set -euo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
Q3_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Q3_TEST_DIR="$(mktemp -d -p "${TMPDIR:-/var/tmp}" q3-mac-glimp.XXXXXX)"
trap 'rm -rf -- "$Q3_TEST_DIR"' EXIT

# Issues #3, #34, #15, #16, #6 and #7: the classic Mac renderer's GLimp_Init and
# GLimp_Shutdown.  The fixture compiles the real src/mac9/mac_glimp.c and
# src/mac9/MacGamma.c against a fake AGL, DrawSprocket, Window Manager,
# video driver and Memory Manager.  Their Toolbox headers resolve to
# tests/mac_glimp_fake.h.  mac_glimp.c's own #includes pull in the whole
# renderer and Mac Toolbox, so the runner blanks them (keeping the line
# numbers) and the fixture includes q_shared.h and tr_public.h instead; the
# macGlInfo struct comes from mac_local.h and glconfigExt_t from tr_local.h.
for Q3_TEST_HEADER in MacTypes.h Quickdraw.h QDOffscreen.h Devices.h Files.h Video.h; do
    printf '#include "%s/tests/mac/mac_glimp_fake.h"\n' "$Q3_TEST_ROOT" > "$Q3_TEST_DIR/$Q3_TEST_HEADER"
done

Q3_TEST_OUT="$Q3_TEST_DIR/mac_glimp_extracted.c"
Q3_TEST_TYPE() {
    awk -v start="$1" -v end="$2" -v file="$3" '
        { sub(/\r$/, "") }
        $0 ~ start { buf = sprintf("#line %d \"%s\"\n", NR, file); collecting = 1 }
        collecting { buf = buf $0 "\n" }
        collecting && $0 ~ end { printf "%s", buf; collecting = 0; count++ }
        END { exit count == 1 ? 0 : 1 }
    ' "$3" >> "$Q3_TEST_OUT" || { echo "run_mac_glimp_tests: no single $2 in $3" >&2; exit 1; }
}
: > "$Q3_TEST_OUT"
# ET has no glconfigExt_t: anisotropy is glConfig.anisotropicAvailable
Q3_TEST_TYPE '^#define[[:space:]]+MAX_DEVICES[[:space:]]' '^[}][[:space:]]*macGlInfo;' "$Q3_TEST_ROOT/src/mac9/mac_local.h"
awk -v file="$Q3_TEST_ROOT/src/mac9/mac_glimp.c" '
    NR == 1 { printf "#line 1 \"%s\"\n", file }
    { sub(/\r$/, "") }
    /^#include[[:space:]]/ { print ""; count++; next }
    { print }
    END { exit count > 0 ? 0 : 1 }
' "$Q3_TEST_ROOT/src/mac9/mac_glimp.c" >> "$Q3_TEST_OUT" ||
    { echo "run_mac_glimp_tests: no #include lines in src/mac9/mac_glimp.c" >&2; exit 1; }

# -Wno-pointer-to-int-cast: mac_glimp.c prints the pixel format with
# (int)sys_gl.fmt, which only fits on the 32-bit target.
"${CC:-cc}" \
    -std=gnu99 -fsigned-char -Wall -Wno-unused-function -Wno-pointer-to-int-cast \
    -fno-omit-frame-pointer -fsanitize=address,undefined -I"$Q3_TEST_DIR" \
    "$Q3_TEST_ROOT/tests/mac/mac_glimp_regression.c" "$Q3_TEST_ROOT/src/game/q_shared.c" \
    -lm -o "$Q3_TEST_DIR/mac_glimp"

ASAN_OPTIONS=detect_leaks=${Q3_TEST_DETECT_LEAKS:-1}:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$Q3_TEST_DIR/mac_glimp"

# The fixture runs only GLimp_Init, but R_Register registers the anisotropy cvar first, so
# tr_init.c's default is the one a Mac gets. Preprocess its registration as the Mac build does.
Q3_TEST_ANISO="$(awk '{ sub(/\r$/, "") } /^#ifdef __MACOS__/ { buf = ""; keep = 1 } keep { buf = buf $0 "\n" }
    keep && /^#endif/ { if ( buf ~ /r_ext_texture_filter_anisotropic/ ) printf "%s", buf; keep = 0 }' \
    "$Q3_TEST_ROOT/src/renderer/tr_init.c" | "${CC:-cc}" -E -P -D__MACOS__ - |
    grep -o 'Cvar_Get( "r_ext_texture_filter_anisotropic", "[^"]*"' || true)"
if [ "$Q3_TEST_ANISO" != 'Cvar_Get( "r_ext_texture_filter_anisotropic", "1"' ]; then
    echo "FAIL: tr_init.c does not default r_ext_texture_filter_anisotropic to 1 on Mac (got: $Q3_TEST_ANISO)" >&2
    exit 1
fi
echo "tr_init.c defaults r_ext_texture_filter_anisotropic to 1 on Mac"
