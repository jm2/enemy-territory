#!/bin/bash
# Check that the Retro68 toolchain runs on this host (issue #269).
#
#   bash check_retro68.sh [--tools-only] [INSTALL_DIR]
#
# INSTALL_DIR defaults to tools/Retro68-build next to this script. Finding the
# programs is not enough: after a host OS upgrade `powerpc-apple-macos-gcc
# --version` still works while cc1, ar, Rez and MakeImport can no longer load
# their shared libraries (libisl, libfl, Boost), and CMake then reports only
# that the compiler identification is unknown. So this script runs the tools
# the build runs, in a scratch directory under ${TMPDIR:-/var/tmp}:
#   - powerpc-apple-macos-gcc --version, then gcc -c and g++ -c with the
#     CMakeLists.txt compile flags (cc1, cc1plus, as);
#   - powerpc-apple-macos-ar and -ranlib on the object, as CMake archives it;
#   - powerpc-apple-macos-ld --version, and MakePEF, MakeImport and Rez with
#     no input, which only has to get past the host loader;
# and, unless --tools-only is given:
#   - a C file that includes <MacTypes.h> and <math.h> is compiled, archived
#     and linked with --whole-archive, -lm and -lInterfaceLib as CMakeLists.txt
#     links Quake3 (ld, libretrocrt, the import libraries);
#   - MakePEF converts the XCOFF image, which must start with Joy!peff/pwpc;
#   - Rez combines that PEF with a resource into an APPL/IDQ3 MacBinary.
# --tools-only leaves out the steps that need the Toolbox headers and target
# libraries, which a --skip-thirdparty rebuild recreates. setup_retro68.sh
# uses it to decide whether resuming a build is safe.
#
# Exit status:
#   0  the toolchain runs;
#   1  the toolchain is broken: the message names the failed step and quotes
#      the tool's output (a library the loader cannot find is named there, and
#      on hosts with ldd every missing library of the toolchain is listed);
#   2  bad usage;
#   3  the check itself could not run, so nothing is known about the
#      toolchain: the scratch directory could not be created or written (a
#      missing, read-only or full TMPDIR, or one out of inodes), a step failed
#      for lack of space or temporary files, or a signal from outside (the OOM
#      killer, a timeout, Ctrl-C) stopped a step.
# Callers must treat only status 1 as a broken toolchain.
set -u

usage() {
    echo "Usage: $(basename "$0") [--tools-only] [INSTALL_DIR]"
}

TOOLS_ONLY=0
INSTALL_DIR=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --tools-only) TOOLS_ONLY=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *)
            if [ -n "$INSTALL_DIR" ]; then
                usage >&2
                exit 2
            fi
            INSTALL_DIR="$1"
            ;;
    esac
    shift
done
if [ -z "$INSTALL_DIR" ]; then
    INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)/tools/Retro68-build"
fi
BIN="$INSTALL_DIR/bin"
TARGET=powerpc-apple-macos
SCRATCH_PARENT="${TMPDIR:-/var/tmp}"

# Keep in step with CMakeLists.txt, cmake/Retro68.toolchain.cmake and
# check_retro68.ps1.
COMPILE_FLAGS="-std=gnu99 -fgnu89-inline -O0 -g -fno-strict-aliasing -fsigned-char -Wformat=2 -Wno-format-nonliteral -Werror=format -Werror=format-security -Wno-error=format-overflow -Wno-error=format-truncation -D__MACOS__ -D__POWERPC__"
CXX_FLAGS="-fsigned-char"

# Messages from a tool that failed because of its surroundings: no space,
# inodes or temporary files in the scratch directory, or the gcc driver
# reporting that something outside killed cc1 or as (a crash such as
# "Segmentation fault signal terminated program" is the toolchain's own).
ENVIRONMENT_ERRORS="No space left on device|Disk quota exceeded|Read-only file system|[Cc]ould not create temporary file|[Cc]annot create temporary file|(Killed|Terminated|Interrupt|Hangup|CPU time limit exceeded|File size limit exceeded) signal terminated program"

WORK=""
cleanup() {
    if [ -n "$WORK" ]; then
        chmod -R u+w "$WORK" 2> /dev/null
        rm -rf -- "$WORK"
    fi
}
trap cleanup EXIT

# Prints the first 20 lines of a tool's output, quoted.
show_output() {
    if [ -n "$1" ]; then
        printf '%s\n' "$1" | sed -n '1,20p' | sed 's/^/  | /'
    fi
}

# Status 3: the check could not run. Says nothing about the toolchain.
cannot_check() {
    local problem="$1" output="${2:-}"

    {
        echo "Error: could not check the Retro68 toolchain in $INSTALL_DIR:"
        echo "  $problem"
        show_output "$output"
        echo "The toolchain itself was not judged. Make TMPDIR ($SCRATCH_PARENT)"
        echo "an existing, writable directory with free space and run the check again."
    } >&2
    exit 3
}

# Succeeds when the scratch directory still takes a SIZE KiB file and 16
# more files (the tools need inodes for temporary and output files). BSD wc
# pads its count with spaces, so compare numbers, not strings.
scratch_writable() {
    local size="$1" probe="$WORK/space.probe" status=0 count=0

    dd if=/dev/zero of="$probe" bs=1024 count="$size" > /dev/null 2>&1 &&
        [ "$(wc -c < "$probe" 2> /dev/null)" -eq "$((size * 1024))" ] 2> /dev/null ||
        status=1
    while [ "$status" -eq 0 ] && [ "$count" -lt 16 ]; do
        : 2> /dev/null > "$WORK/inode.probe.$count" || status=1
        count=$((count + 1))
    done
    rm -f -- "$probe" "$WORK"/inode.probe.*
    return "$status"
}

# Lists the host libraries that the toolchain's executables cannot find.
report_missing_libraries() {
    local file missing

    command -v ldd > /dev/null 2>&1 || return 0
    missing=$(
        for file in "$BIN"/* "$INSTALL_DIR/libexec/gcc/$TARGET"/*/*; do
            case "${file##*/}" in m68k-*) continue ;; esac
            [ -f "$file" ] && [ -x "$file" ] || continue
            ldd "$file" < /dev/null 2> /dev/null |
                awk -v tool="${file##*/}" '/=> not found/ { print $1, tool }'
        done | sort -u |
            awk '$1 != library { if (line != "") print line; library = $1; line = "  " $1 " (needed by " $2; next }
                 { line = line ", " $2 }
                 END { if (line != "") print line }' |
            sed 's/$/)/'
    )
    if [ -n "$missing" ]; then
        echo "Host shared libraries the toolchain cannot find (ldd):"
        echo "$missing"
    fi
}

# Status 1: the toolchain is broken. A failure that its surroundings explain
# is status 3: a signal from outside stopped the tool (STATUS 129 HUP, 130 INT,
# 137 KILL, 143 TERM, 152 XCPU or 153 XFSZ), the tool reports no space or
# temporary files, or the scratch directory no longer takes writes.
fail() {
    local step="$1" output="${2:-}" status="${3:-}"

    if [ -n "$WORK" ]; then
        case "$status" in
            129|130|137|143|152|153)
                cannot_check "$step was stopped by signal $((status - 128)) from outside (exit status $status), for example by the OOM killer or a timeout." "$output"
                ;;
        esac
        if [ -n "$output" ] && printf '%s\n' "$output" | grep -q -E "$ENVIRONMENT_ERRORS"; then
            cannot_check "$step failed because of its surroundings (space, temporary files or a signal), not the toolchain:" "$output"
        fi
        if ! scratch_writable 64; then
            cannot_check "$step failed, and the scratch directory $WORK no longer takes writes." "$output"
        fi
    fi
    {
        echo "Error: Retro68 toolchain check failed in $INSTALL_DIR:"
        echo "  $step"
        show_output "$output"
        report_missing_libraries
        echo "If a host shared library is missing (for example after an OS"
        echo "upgrade), install it or rebuild the toolchain: setup_retro68.sh"
        echo "moves a toolchain that cannot run aside and rebuilds it. See"
        echo "\"Checking and rebuilding the toolchain\" in docs/building-mac-os9.md."
    } >&2
    exit 1
}

require_tool() {
    [ -f "$BIN/$1" ] && [ -x "$BIN/$1" ] ||
        fail "$1 is missing or not executable in $BIN."
}

# Writes the remaining arguments, one per line, to FILE and reads them back.
write_scratch_file() {
    local file="$1"
    shift
    { printf '%s\n' "$@" > "$file"; } 2> /dev/null &&
        [ "$(cat "$file" 2> /dev/null)" = "$(printf '%s\n' "$@")" ] ||
        cannot_check "could not write $file."
}

# Runs a tool that must succeed. Its output is kept in memory, not in the
# scratch directory: when that directory fills up, a log file there loses the
# tool's "No space left on device", and the failure would be blamed on the
# toolchain.
run_step() {
    local step="$1" output status
    shift
    output=$("$@" < /dev/null 2>&1)
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "$step failed (exit status $status):" "$output" "$status"
    fi
}

# Runs a tool without input: its own usage error is fine, but the host loader
# must not stop it (status 126/127, a signal, or a loader message).
run_probe() {
    local tool="$1" output status
    shift
    output=$("$BIN/$tool" "$@" < /dev/null 2>&1)
    status=$?
    if [ "$status" -ge 126 ] ||
       printf '%s\n' "$output" |
       grep -q -E "error while loading shared libraries|symbol lookup error|Library not loaded|not found \(required by"; then
        fail "$tool could not start (exit status $status):" "$output" "$status"
    fi
}

for tool in "$TARGET-gcc" "$TARGET-g++" "$TARGET-ar" "$TARGET-ranlib" \
            "$TARGET-ld" MakePEF MakeImport Rez; do
    require_tool "$tool"
done

WORK="$(mktemp -d "$SCRATCH_PARENT/q3-retro68-check.XXXXXX" 2> /dev/null)" || {
    WORK=""
    cannot_check "could not create a scratch directory under $SCRATCH_PARENT."
}
# The check writes well under 1 MiB in a few files; make sure that much fits
# before any tool runs, so a full TMPDIR is not mistaken for a broken compiler.
scratch_writable 1024 ||
    cannot_check "the scratch directory $WORK does not take 1 MiB and 16 files (TMPDIR full, out of inodes or read-only)."
export PATH="$BIN:$PATH"

if [ "$TOOLS_ONLY" -eq 1 ]; then
    write_scratch_file "$WORK/check.c" 'int retro68_check(void)' '{' '	return 0;' '}'
else
    write_scratch_file "$WORK/check.c" '#include <MacTypes.h>' '#include <math.h>' '' \
        'int main(int argc, char **argv)' '{' \
        '	Boolean ok = argc > 0 && argv != NULL;' \
        '	return ok && floor(1.5) == 1.0 ? 0 : 1;' '}'
fi
write_scratch_file "$WORK/check_cxx.cc" 'int retro68_check_cxx(int value)' '{' \
    '	return value + 1;' '}'

run_step "$TARGET-gcc --version" "$BIN/$TARGET-gcc" --version
# shellcheck disable=SC2086
run_step "compiling a C file ($TARGET-gcc -c)" \
    "$BIN/$TARGET-gcc" $COMPILE_FLAGS -c "$WORK/check.c" -o "$WORK/check.o"
# shellcheck disable=SC2086
run_step "compiling a C++ file ($TARGET-g++ -c)" \
    "$BIN/$TARGET-g++" $CXX_FLAGS -c "$WORK/check_cxx.cc" -o "$WORK/check_cxx.o"
run_step "archiving the object ($TARGET-ar qc)" \
    "$BIN/$TARGET-ar" qc "$WORK/libcheck.a" "$WORK/check.o"
run_step "indexing the archive ($TARGET-ranlib)" \
    "$BIN/$TARGET-ranlib" "$WORK/libcheck.a"
run_step "$TARGET-ld --version" "$BIN/$TARGET-ld" --version
run_probe MakePEF
run_probe MakeImport
run_probe Rez --help

if [ "$TOOLS_ONLY" -eq 0 ]; then
    # shellcheck disable=SC2086
    run_step "linking an XCOFF image ($TARGET-gcc)" \
        "$BIN/$TARGET-gcc" $COMPILE_FLAGS \
        -Wl,--whole-archive "$WORK/libcheck.a" -Wl,--no-whole-archive \
        -lm -lInterfaceLib -o "$WORK/check.xcoff"
    run_step "converting the XCOFF image to PEF (MakePEF)" \
        "$BIN/MakePEF" "$WORK/check.xcoff" -o "$WORK/check.pef"
    # "Joy!" "peff" and the "pwpc" architecture: a PowerPC PEF container.
    if [ "$(head -c 12 "$WORK/check.pef" 2> /dev/null)" != "Joy!peffpwpc" ]; then
        fail "MakePEF did not write a PowerPC PEF (no Joy!peff/pwpc header)."
    fi
    write_scratch_file "$WORK/check.r" "data 'Q3ck' (128) {" '	$"00"' '};'
    run_step "building an application with Rez" \
        "$BIN/Rez" "$WORK/check.r" -t APPL -c IDQ3 --data "$WORK/check.pef" \
        -o "$WORK/check.bin"
    if [ ! -s "$WORK/check.bin" ]; then
        fail "Rez did not write an application."
    fi
fi

if [ "$TOOLS_ONLY" -eq 1 ]; then
    echo "Retro68 tools run: $INSTALL_DIR"
else
    echo "Retro68 toolchain check passed: $INSTALL_DIR"
fi
exit 0
