#!/bin/bash
set -euo pipefail

# Work from the repository root whatever the caller's directory (issue #231):
# tools/, retro68-versions.txt and check_retro68.sh are all found from there.
cd "$(dirname "${BASH_SOURCE[0]}")"

# Detect OS
OS_NAME=$(uname -s)

# Homebrew/Path setup for macOS
if [ "$OS_NAME" = "Darwin" ]; then
    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
fi

INSTALL_DIR="$(pwd)/tools/Retro68-build"
SOURCE_DIR="$(pwd)/tools/Retro68-src"
BUILD_WORK_DIR="$(pwd)/tools/Retro68-work"

# The Retro68 commit, its submodules and the SDK archive digests are pinned
# in retro68-versions.txt, which setup_retro68.ps1 reads too (issue #227).
# macintoshgarden.org has been intermittently unreachable; if you can't fetch
# fresh copies you can drop the originals at the repo root or in tools/ and
# this script will pick them up. The URLs are only used as a last resort.
VERSIONS_FILE="$(pwd)/retro68-versions.txt"
RETRO68_URL=""
RETRO68_COMMIT=""
RETRO68_SUBMODULES=()
MPW_FILE=""
MPW_URL=""
MPW_SHA256=""
OPENGL_FILE=""
OPENGL_URL=""
OPENGL_SHA256=""

versions_error() {
    echo "Error: $VERSIONS_FILE: $1"
    exit 1
}

read_versions() {
    local line key value

    [ -f "$VERSIONS_FILE" ] || versions_error "missing; it pins the Retro68 commit and the SDK archives."
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            ''|'#'*) continue ;;
            *=*) ;;
            *) versions_error "unknown line: $line" ;;
        esac
        key="${line%%=*}"
        value="${line#*=}"
        case "$key" in
            RETRO68_URL|MPW_FILE|MPW_URL|OPENGL_FILE|OPENGL_URL)
                [ -n "$value" ] || versions_error "$key is empty."
                ;;
            RETRO68_COMMIT)
                [[ $value =~ ^[0-9a-f]{40}$ ]] || versions_error "$key is not a full commit hash: $value"
                ;;
            RETRO68_SUBMODULE)
                [[ $value =~ ^[^[:space:]]+\ [0-9a-f]{40}$ ]] ||
                    versions_error "$key is not \"<path> <commit>\": $value"
                RETRO68_SUBMODULES+=("$value")
                continue
                ;;
            MPW_SHA256|OPENGL_SHA256)
                [[ $value =~ ^[0-9a-f]{64}$ ]] || versions_error "$key is not a SHA-256: $value"
                ;;
            *) versions_error "unknown line: $line" ;;
        esac
        printf -v "$key" '%s' "$value"
    done < "$VERSIONS_FILE"
    for key in RETRO68_URL RETRO68_COMMIT MPW_FILE MPW_URL MPW_SHA256 \
               OPENGL_FILE OPENGL_URL OPENGL_SHA256; do
        [ -n "${!key}" ] || versions_error "$key is missing."
    done
    [ "${#RETRO68_SUBMODULES[@]}" -gt 0 ] || versions_error "RETRO68_SUBMODULE is missing."
}
read_versions

echo "=========================================="
echo "Retro68 Setup Script"
echo "=========================================="
echo "This will:"
echo "1. Clone Retro68 at the commit pinned in retro68-versions.txt"
echo "2. Locate or download MPW & OpenGL SDKs (.sit) and check their SHA-256"
echo "3. Inject them into Retro68-src/InterfacesAndLibraries"
echo "4. Move old build directories aside (unless resuming)"
echo "5. Rebuild Retro68 completely"
echo "=========================================="
echo "Press Ctrl+C to cancel in 5 seconds..."
sleep 5

# Dependency Checking
# Note: ruby is required by Retro68's build-toolchain.bash to generate the
# Multiversal Interfaces (it runs make-multiverse.rb even when Universal
# Interfaces are present). Failing late in the toolchain build for a missing
# ruby is painful, so check up front.
REQUIRED_CMDS="cmake git bison flex makeinfo unar ruby"
MISSING_DEPS=0

for cmd in $REQUIRED_CMDS; do
    if ! command -v $cmd &> /dev/null; then
        echo "Error: Required command '$cmd' not found."
        MISSING_DEPS=1
    fi
done
# The SDK archives are checked against their pinned SHA-256.
if ! command -v sha256sum &> /dev/null && ! command -v shasum &> /dev/null; then
    echo "Error: Required command 'sha256sum' (or 'shasum') not found."
    MISSING_DEPS=1
fi

if [ $MISSING_DEPS -eq 1 ]; then
    echo "--------------------------------------------------------"
    echo "Please install missing dependencies manually."
    exit 1
fi

mkdir -p tools

# Decide before changing anything whether step 4 can resume an existing
# toolchain. A full Retro68 build takes 30+ minutes; if we already have the
# host tools (ConvertDiskImage is the last one built and installed), step 4
# keeps the existing binutils/gcc/host-tool artifacts and passes
# --skip-thirdparty to build-toolchain.bash so it just runs the I&L +
# multiversal + target-lib steps. The installed tools must also run: after a
# host OS upgrade they can fail to load their shared libraries, and
# --skip-thirdparty never rebuilds gcc or binutils (issue #269).
# check_retro68.sh exits 1 only for tools that cannot run. Any other status
# means the check itself could not run (for example a missing, read-only or
# full TMPDIR), which says nothing about the toolchain: stop and change nothing.
SKIP_FLAGS=()
ASIDE_SUFFIX=previous
if [ -x "$INSTALL_DIR/bin/ConvertDiskImage" ] && [ -d "$BUILD_WORK_DIR" ]; then
    CHECK_STATUS=0
    bash ./check_retro68.sh --tools-only "$INSTALL_DIR" || CHECK_STATUS=$?
    case "$CHECK_STATUS" in
        0) SKIP_FLAGS=(--skip-thirdparty) ;;
        1) ASIDE_SUFFIX=broken ;;
        *)
            echo "Error: check_retro68.sh could not check the existing toolchain"
            echo "(exit status $CHECK_STATUS). Nothing was changed; fix the problem"
            echo "above and run setup_retro68.sh again."
            exit 1
            ;;
    esac
fi

# 1. Retro68 at the pinned commit. A fresh clone checks out RETRO68_COMMIT and
# its submodules. An existing checkout is never pulled, switched or updated,
# its submodules included (issue #406): this script makes in-tree edits
# (Boost patch, InterfacesAndLibraries population) that block a checkout, and
# the toolchain was built from that commit. So a checkout at another commit,
# or with a submodule that is missing, at another commit or in conflict,
# stops setup before anything changes.
echo "Step 1: Ensuring Retro68 source is at commit $RETRO68_COMMIT..."

# " <commit> <path>" for every submodule of the checkout at $1, as `git
# submodule status` flags it: ' ' checked out at the commit Retro68 records,
# '-' not checked out, '+' at another commit, 'U' in conflict.
checked_out_submodules() {
    git -C "$1" submodule status --recursive |
        awk '{ path = substr($0, 43); sub(/ \(.*\)$/, "", path); print substr($0, 1, 41), path }' |
        LC_ALL=C sort
}
pinned_submodules() {
    local pin
    for pin in "${RETRO68_SUBMODULES[@]}"; do
        echo " ${pin#* } ${pin%% *}"
    done | LC_ALL=C sort
}

# Succeeds when the checkout at $1 is at the pinned commit with the pinned
# submodules; otherwise says why.
at_pinned_checkout() {
    local dir="$1" commit="not a git checkout"
    if [ -e "$dir/.git" ]; then
        commit=$(git -C "$dir" rev-parse --verify HEAD) || commit="unknown"
    fi
    if [ "$commit" != "$RETRO68_COMMIT" ]; then
        echo "Error: $dir is at $commit, but"
        echo "retro68-versions.txt pins Retro68 $RETRO68_COMMIT."
        return 1
    fi
    if [ "$(checked_out_submodules "$dir")" != "$(pinned_submodules)" ]; then
        echo "Error: the submodules of $dir are not the ones retro68-versions.txt pins."
        echo "  Checked out (git submodule status --recursive):"
        checked_out_submodules "$dir" | sed 's/^/    /' || true
        echo "  Pinned:"
        pinned_submodules | sed 's/^/    /'
        return 1
    fi
}

if [ ! -e "$SOURCE_DIR" ]; then
    # Clone next to tools/Retro68-src and rename the clone only once it is
    # at the pinned commit and submodules, so a clone or checkout that fails
    # leaves nothing behind that would stop the next run (issue #406).
    PARTIAL_SOURCE_DIR="$SOURCE_DIR.partial"
    fresh_clone_failed() {
        rm -rf "$PARTIAL_SOURCE_DIR"
        echo "Error: could not check out Retro68 $RETRO68_COMMIT and its pinned"
        echo "submodules (see above). The partial clone was removed; nothing else changed."
        exit 1
    }
    rm -rf "$PARTIAL_SOURCE_DIR"
    git clone --no-checkout "$RETRO68_URL" "$PARTIAL_SOURCE_DIR" || fresh_clone_failed
    git -C "$PARTIAL_SOURCE_DIR" -c advice.detachedHead=false checkout --detach "$RETRO68_COMMIT" ||
        fresh_clone_failed
    git -C "$PARTIAL_SOURCE_DIR" submodule update --init --recursive || fresh_clone_failed
    at_pinned_checkout "$PARTIAL_SOURCE_DIR" || fresh_clone_failed
    mv "$PARTIAL_SOURCE_DIR" "$SOURCE_DIR"
else
    echo "Retro68 source already present at $SOURCE_DIR (not pulling)."
    if ! at_pinned_checkout "$SOURCE_DIR"; then
        echo "Nothing was changed: setup never pulls, switches or updates an existing"
        echo "checkout or its submodules. To build the pinned toolchain, move tools/Retro68-src,"
        echo "tools/Retro68-build and tools/Retro68-work aside and run setup_retro68.sh again."
        exit 1
    fi
fi

# 2. Prepare InterfacesAndLibraries
echo "Step 2: Preparing SDKs..."
SDK_DEST="$SOURCE_DIR/InterfacesAndLibraries"

# Prints the SHA-256 of a file.
sha256_of() {
    if command -v sha256sum &> /dev/null; then
        sha256sum < "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 < "$1" | cut -d ' ' -f 1
    fi
}

# Succeeds when FILE's SHA-256 is WANT; otherwise says why on stderr.
sit_matches() {
    local file="$1" want="$2" got
    got=$(sha256_of "$file" 2> /dev/null) || got=""
    [ "$got" = "$want" ] && return 0
    echo "  $file does not match the SHA-256 pinned in retro68-versions.txt" >&2
    echo "  (truncated, damaged or another file); it is not used." >&2
    echo "    pinned: $want" >&2
    echo "    found:  ${got:-(could not read it)}, $(wc -c 2> /dev/null < "$file" | tr -d ' ') bytes" >&2
    return 1
}

# Resolve a SIT file whose SHA-256 matches the pin: tools/<name>, then
# repo-root <name>, else download. The repo-root fallback is the friendly path
# for offline setups when macintoshgarden is down — drop the file alongside
# the script and re-run. Nothing is extracted from a copy that does not match,
# and no local copy is deleted or overwritten. A download is written to
# tools/<name>.part and renamed to tools/<name> only once it matches.
locate_or_fetch_sit() {
    # Status messages go to stderr; only the resolved path goes to stdout, so
    # `VAR=$(locate_or_fetch_sit ...)` captures just the path.
    local fname="$1"
    local url="$2"
    local want="$3"
    local candidate

    for candidate in "tools/$fname" "$fname"; do
        [ -e "$candidate" ] || continue
        if sit_matches "$candidate" "$want"; then
            echo "  Using $candidate (SHA-256 matches retro68-versions.txt)" >&2
            echo "$candidate"
            return 0
        fi
    done
    if [ -e "tools/$fname" ]; then
        echo "Error: no copy of $fname matches its pinned SHA-256." >&2
        echo "Replace tools/$fname with a good copy, or move it away so setup can download one." >&2
        return 1
    fi
    echo "  No local copy of $fname; attempting download..." >&2
    if ! command -v wget &> /dev/null; then
        echo "Error: wget is required only when $fname is not cached." >&2
        echo "Place it at the repo root or in tools/, or install wget." >&2
        return 1
    fi
    rm -f "tools/$fname.part"
    if ! wget --quiet --show-progress -O "tools/$fname.part" "$url"; then
        rm -f "tools/$fname.part"
        echo "Error: could not obtain $fname (and download failed)." >&2
        echo "Place it at the repo root or in tools/ and re-run." >&2
        return 1
    fi
    if ! sit_matches "tools/$fname.part" "$want"; then
        rm -f "tools/$fname.part"
        echo "Error: the download of $fname from $url was discarded." >&2
        echo "Place a copy that matches at the repo root or in tools/ and re-run." >&2
        return 1
    fi
    mv "tools/$fname.part" "tools/$fname"
    echo "tools/$fname"
}

# Check both archives before extracting either.
MPW_SIT=$(locate_or_fetch_sit "$MPW_FILE" "$MPW_URL" "$MPW_SHA256") || exit 1
OPENGL_SIT=$(locate_or_fetch_sit "$OPENGL_FILE" "$OPENGL_URL" "$OPENGL_SHA256") || exit 1
mkdir -p "$SDK_DEST"

# extract_sit ARCHIVE SHA256 DEST: hash the archive again right before
# extracting it, since it was checked before the other one (issue #406).
extract_sit() {
    if ! sit_matches "$1" "$2"; then
        echo "Error: $1 changed after it was checked; it was not extracted."
        exit 1
    fi
    rm -rf "$3"
    unar -q -f "$1" -o "$3"
}

# Extract MPW
echo "Extracting MPW..."
extract_sit "$MPW_SIT" "$MPW_SHA256" "tools/temp_mpw"

# Move Interfaces&Libraries content from MPW to Retro68 src
echo "Injecting MPW Interfaces&Libraries..."
MPW_I_AND_L=$(find tools/temp_mpw -type d -name "Interfaces&Libraries" -print -quit)

if [ -n "$MPW_I_AND_L" ] && [ -d "$MPW_I_AND_L" ]; then
    echo "Found MPW I&L at: $MPW_I_AND_L"
    cp -r "$MPW_I_AND_L/"* "$SDK_DEST/"
else
    echo "Error: Could not find Interfaces&Libraries in extracted MPW"
    ls -R tools/temp_mpw | head -n 20 || true
    exit 1
fi

# Extract OpenGL
echo "Extracting OpenGL SDK..."
extract_sit "$OPENGL_SIT" "$OPENGL_SHA256" "tools/temp_opengl"

echo "Injecting OpenGL headers/libs..."
mkdir -p "$SDK_DEST/Libraries"
mkdir -p "$SDK_DEST/Interfaces/CIncludes"

# OpenGL SDK lays out 'Libraries' (PEF stubs) and 'Headers' (gl.h, glu.h,
# agl.h, glm.h, ...) at the top level. -maxdepth 2 because the SDK also
# contains nested e.g. Source/Libraries/ that we do not want; without the
# constraint, find's traversal order is filesystem-dependent and we
# silently picked the wrong one on some runs.
OGL_LIBS=$(find tools/temp_opengl -maxdepth 2 -type d -name "Libraries" -print -quit)
if [ -n "$OGL_LIBS" ]; then
    echo "Copying OpenGL Libs from $OGL_LIBS..."
    mkdir -p "$SDK_DEST/SharedLibraries"
    cp -r "$OGL_LIBS/"* "$SDK_DEST/SharedLibraries/"
else
    echo "Warning: Could not find Libraries in OpenGL SDK"
fi

OGL_HEADERS=$(find tools/temp_opengl -maxdepth 2 -type d -name "Headers" -print -quit)
if [ -n "$OGL_HEADERS" ]; then
    echo "Copying OpenGL Headers from $OGL_HEADERS..."
    cp -r "$OGL_HEADERS/"* "$SDK_DEST/Interfaces/CIncludes/"
else
    echo "Warning: Could not find Headers in OpenGL SDK"
fi

rm -rf "tools/temp_mpw" "tools/temp_opengl"

echo "Pruning conflicting MPW headers..."
# Remove standard C headers from MPW that conflict with the host GCC's libc.
for header in assert.h ctype.h errno.h float.h limits.h locale.h math.h \
              setjmp.h signal.h stdarg.h stddef.h stdio.h stdlib.h string.h \
              time.h; do
    find "$SDK_DEST/Interfaces/CIncludes" -name "$header" -delete 2>/dev/null || true
done

# 3. Patching
echo "Step 3: Patching CMakeLists for modern Boost..."
if [ "$OS_NAME" = "Darwin" ]; then
    find "$SOURCE_DIR" -name "CMakeLists.txt" -exec sed -i '' '/find_package(Boost/s/ system//g' {} +
else
    find "$SOURCE_DIR" -name "CMakeLists.txt" -exec sed -i '/find_package(Boost/s/ system//g' {} +
fi

# 4. Clean / resume (decided above). A full build needs an empty prefix and a
# fresh work tree. Never delete the toolchain: move it aside every time, so
# one that only lacks a host library, or was set aside by mistake, can be
# restored. The work tree holds only build intermediates (several GB), so keep
# a single moved-aside copy of it: an older one is removed when a newer one is
# moved aside. To force a full rebuild, move tools/Retro68-build away yourself.
if [ "${#SKIP_FLAGS[@]}" -ne 0 ]; then
    echo "Step 4: Existing toolchain detected — resuming with --skip-thirdparty."
else
    if [ "$ASIDE_SUFFIX" = broken ]; then
        echo "Step 4: The existing toolchain cannot run (see above); rebuilding it all."
    fi
    echo "Step 4: Moving previous builds aside..."
    ASIDE_STAMP=$(date -u +%Y%m%dT%H%M%SZ)
    MOVED_ASIDE=0
    for previous in "$INSTALL_DIR" "$BUILD_WORK_DIR"; do
        [ -e "$previous" ] || continue
        aside="$previous.$ASIDE_SUFFIX-$ASIDE_STAMP"
        [ -e "$aside" ] && aside="$aside-$$"
        if [ "$previous" = "$BUILD_WORK_DIR" ]; then
            for older in "$BUILD_WORK_DIR".broken-* "$BUILD_WORK_DIR".previous-*; do
                [ -d "$older" ] || continue
                echo "  Removing the older moved-aside work tree $older"
                echo "  (build intermediates only; one moved-aside work tree is kept)."
                rm -rf "$older"
            done
        fi
        mv "$previous" "$aside"
        MOVED_ASIDE=1
        echo "  Moved $previous"
        echo "     to $aside"
        echo "  To restore it: rm -rf \"$previous\" && mv \"$aside\" \"$previous\""
    done
    if [ "$MOVED_ASIDE" -eq 1 ]; then
        echo "  Delete the moved directories once the new toolchain works."
    fi
fi

# 5. Build
echo "Step 5: Building Retro68..."
mkdir -p "$BUILD_WORK_DIR"
cd "$BUILD_WORK_DIR"

if [ "$OS_NAME" = "Darwin" ] && command -v brew &> /dev/null; then
    export BOOST_ROOT="$(brew --prefix boost)"
    export Boost_INCLUDE_DIR="$(brew --prefix boost)/include"
    export Boost_LIBRARY_DIR="$(brew --prefix boost)/lib"
elif [ "$OS_NAME" = "Linux" ]; then
    echo "Using system Boost."
fi

# GCC 16 defaults to C++20, which the host build of Retro68's GCC 12.2.0
# fails under (issue #387): libcody's u8"" literals become char8_t (its
# configure adds -std=c++11 to CXX, but GCC's top-level make drops it), and
# <memory>, which gcc/system.h includes after safe-ctype.h, now pulls in
# <locale> and clashes with its toupper/isalpha macros. build-toolchain.bash
# overwrites CC and CXX but not CXXFLAGS, so pin the dialect there: C++11,
# because libcody's configure accepts no other -std. Retro68's own CMake
# projects set C++17 after it, and GCC's target libraries ignore CXXFLAGS.
host_cxx_dialect() {
    local cxx
    for cxx in g++ c++; do
        command -v "$cxx" &> /dev/null || continue
        printf '__cplusplus\n' | "$cxx" -x c++ -E -P - 2> /dev/null | tr -d ' L\r' | tail -n 1
        return
    done
}
HOST_CXX_DIALECT=$(host_cxx_dialect) || HOST_CXX_DIALECT=""
if [[ $HOST_CXX_DIALECT =~ ^[0-9]+$ ]] && [ "$HOST_CXX_DIALECT" -gt 201703 ]; then
    case " ${CXXFLAGS:-} " in
        *" -std="*)
            echo "The host C++ compiler defaults to C++ $HOST_CXX_DIALECT; keeping CXXFLAGS=$CXXFLAGS."
            ;;
        *)
            export CXXFLAGS="${CXXFLAGS:--O2} -std=gnu++11"
            echo "The host C++ compiler defaults to C++ $HOST_CXX_DIALECT, which GCC 12.2 does not"
            echo "build under; building with CXXFLAGS=$CXXFLAGS."
            ;;
    esac
fi

# ${a[@]+...}: bash 3.2 (macOS) treats an empty array as unset under set -u.
bash "$SOURCE_DIR/build-toolchain.bash" --prefix="$INSTALL_DIR" ${SKIP_FLAGS[@]+"${SKIP_FLAGS[@]}"}

# 6. Post-Build Fixes
echo "Step 6: Post-Build Fixes..."
SDK_DEST="$SOURCE_DIR/InterfacesAndLibraries"

# Copy the OpenGL SDK headers into the PREPARED toolchain include dir.
# The project must compile ONLY against prepared headers: the raw CIncludes
# use '#pragma options align=mac68k', which powerpc-apple-macos GCC silently
# ignores, so every Toolbox struct (EventRecord, FSSpec, ...) gets natural
# alignment instead of the 68k packing InterfaceLib expects — corrupting all
# Toolbox calls. Retro68's prepare-headers rewrites Apple interfaces to
# '#pragma pack(push,2)' but does not know about the injected OpenGL SDK, so
# we copy those (pragma-free, alignment-safe) headers in here ourselves.
echo "Installing OpenGL headers into prepared include dir..."
PPC_INC="$INSTALL_DIR/powerpc-apple-macos/include"
if [ -d "$PPC_INC" ]; then
    for h in gl.h glu.h glm.h agl.h aglContext.h aglMacro.h aglRenderers.h \
             glext.h GL_gl.h GL_glext.h GL_glut.h gliContext.h gliDispatch.h glut.h; do
        [ -f "$SDK_DEST/Interfaces/CIncludes/$h" ] && cp "$SDK_DEST/Interfaces/CIncludes/$h" "$PPC_INC/"
    done
fi
STUB_SRC="$SDK_DEST/SharedLibraries/OpenGLLibraryStub"
STUB_RSRC="$SDK_DEST/SharedLibraries/OpenGLLibraryStub.rsrc"
STUB_AD="$SDK_DEST/SharedLibraries/%OpenGLLibraryStub"
STUB_LIB="$SDK_DEST/SharedLibraries/libOpenGLLibraryStub.a"

if [ ! -f "$STUB_SRC" ] || [ ! -f "$STUB_RSRC" ]; then
    echo "Error: OpenGL import-library inputs are incomplete in $SDK_DEST/SharedLibraries."
    exit 1
fi

echo "Generating OpenGLLibraryStub import library..."
cp "$STUB_RSRC" "$STUB_AD"
export PATH="$INSTALL_DIR/bin:$PATH"
if ! command -v MakeImport &> /dev/null; then
    echo "Error: MakeImport not found in $INSTALL_DIR/bin"
    exit 1
fi
MakeImport "$STUB_SRC" "$STUB_LIB"

for required in "$PPC_INC/gl.h" "$PPC_INC/agl.h" "$STUB_LIB"; do
    if [ ! -s "$required" ]; then
        echo "Error: Retro68 setup is incomplete; missing $required"
        exit 1
    fi
done

echo "=========================================="
echo "Retro68 Re-Build Complete!"
echo "Check $INSTALL_DIR for results."
echo "=========================================="
