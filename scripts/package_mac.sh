#!/usr/bin/env bash
# Stage the application and game data and make the images that go to the Mac. Run it through
# ./build_mac.sh package, so that the build it packages is the commit it records. --check only
# looks for what a package needs. --bringup adds the stage 1 METParms.txt and bringup.cfg of
# docs/target-run.md.
#
# Game data is found locally and never committed: ET_DATA_DIR holds etmain/ and legacy/, and
# defaults to ~/et-data/run/base. Every file must match mac-data/game-data.sha256, because a
# pure server checks these exact paks and a mod from another release is refused.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA=${ET_DATA_DIR:-$HOME/et-data/run/base}
SUMS=mac-data/game-data.sha256
RELEASE=release_mac
APP=build-mac/ET

fail() { echo "package_mac: $1" >&2; exit 1; }

check() {
    local mkisofs line sum file actual
    mkisofs=$(command -v genisoimage || command -v mkisofs) || fail "genisoimage or mkisofs is needed"
    while read -r sum file; do
        [ -f "$DATA/$file" ] || fail "$DATA/$file is missing.
Paks come from https://mirror.etlegacy.com/etmain/ and the mod pk3 from the ET: Legacy v2.86.2
release. Set ET_DATA_DIR to the folder holding etmain/ and legacy/."
        actual=$(sha256sum "$DATA/$file" | cut -d' ' -f1)
        [ "$actual" = "$sum" ] || fail "$DATA/$file is not the expected file (SHA-256 $actual).
A demo pak or another mod release will not work: a pure server checks these exact files."
    done < "$SUMS"
    MKISOFS=$mkisofs
}

BRINGUP=0
case "${1:-}" in
    --check) check; exit 0 ;;
    --from-build) ;;
    *) fail "run this through ./build_mac.sh package" ;;
esac
case "${2:-}" in
    --bringup) BRINGUP=1 ;;
    "") ;;
    *) fail "unknown option $2" ;;
esac
check
COMMIT=$(git rev-parse HEAD) || fail "a package records its commit, so it needs a git checkout"
OPT=$(sed -n 's/^ET_OPT:[A-Z]*=//p' build-mac/CMakeCache.txt)

for container in "$APP.pef" "$APP.bin" "$APP.ad" "build-mac/%ET.ad" "$APP.xcoff" "$APP.manifest.txt"; do
    [ -s "$container" ] || fail "$container is missing; build first"
done

# staging trees are rebuilt each time, so nothing left by another configuration gets in
CONTENT=$RELEASE/content
BIN_CONTENT=$RELEASE/bin_content
TEMP=$RELEASE/temp
SYMBOLS=$RELEASE/symbols
rm -rf "$RELEASE"
mkdir -p "$CONTENT/Enemy Territory/etmain" "$CONTENT/Enemy Territory/legacy" "$BIN_CONTENT" "$TEMP" "$SYMBOLS"

while read -r sum file; do
    cp "$DATA/$file" "$CONTENT/Enemy Territory/$file"
done < "$SUMS"
if [ "$BRINGUP" = 1 ]; then
    cp mac-data/METParms.txt "$CONTENT/Enemy Territory/"
    cp mac-data/bringup.cfg "$CONTENT/Enemy Territory/etmain/"
fi

# genisoimage -double merges %ET into ET's forks
cp "$APP.ad" "$CONTENT/Enemy Territory/ET"
cp "build-mac/%ET.ad" "$CONTENT/Enemy Territory/%ET"
cp "$APP.ad" "$BIN_CONTENT/ET"
cp "build-mac/%ET.ad" "$BIN_CONTENT/%ET"

# EXTN XLate CREATOR TYPE Comment. Raw keeps the text files byte for byte what was tested.
MAPPING=$TEMP/hfs_mapping.txt
cat > "$MAPPING" <<'M'
.pk3 Raw 'WfET' 'Stak' "Enemy Territory Data"
.cfg Raw 'WfET' 'TEXT' "Enemy Territory Config"
.txt Raw 'ttxt' 'TEXT' "Enemy Territory Parameters"
M

image() { # volume, staging tree, output
    "$MKISOFS" -quiet -hfs -double -map "$MAPPING" -o "$3" -V "$1" "$2"
    python3 mac_app.py verify --creator WfET --hfs-name ET --pef "$APP.pef" "$3"
}
image "Enemy Territory" "$CONTENT" "$RELEASE/ET_Install.img"
image "ET Binaries" "$BIN_CONTENT" "$RELEASE/ET_Bin.img"

# the application check says nothing of the data, so read it back out of the image too
python3 -I -B - "$RELEASE/ET_Install.img" "$SUMS" "$BRINGUP" <<'P'
import hashlib, os, sys
sys.path.insert(0, os.getcwd())
import mac_app
image, sums, bringup = sys.argv[1:]
with open(image, "rb") as stream:
    volume = mac_app._HfsVolume(image, stream)
    names = {}
    for _, _, name, _ in volume.file_records():
        names[name] = names.get(name, 0) + 1
    expected = [line.split()[1].rsplit("/", 1)[1] for line in open(sums)]
    bringup_files = ["METParms.txt", "bringup.cfg"]
    extra = bringup_files if bringup == "1" else []
    for name in expected + extra:
        if names.get(name) != 1:
            sys.exit("package_mac: %s is not in the image once: %s" % (name, sorted(names)))
    for line in open(sums):
        sum_, path = line.split()
        data = volume.read_file(path.rsplit("/", 1)[1]).data
        if hashlib.sha256(data).hexdigest() != sum_:
            sys.exit("package_mac: %s changed in the image" % path)
    if bringup != "1" and any(name in names for name in bringup_files):
        sys.exit("package_mac: a bring-up file got into a plain package")
print("image data verified: %d files" % len(expected))
P

python3 macbinary_encode.py "$RELEASE/ET_Install.img" "$RELEASE/ET_Install.img.bin" "iso " "dCpy"
python3 macbinary_encode.py "$RELEASE/ET_Bin.img" "$RELEASE/ET_Bin.img.bin" "iso " "dCpy"
rm -f "$RELEASE/ET_Install.img" "$RELEASE/ET_Bin.img"
rm -rf "$CONTENT" "$BIN_CONTENT" "$TEMP"

# stays here: mac_symbolize.py needs the matching XCOFF for a crash report
cp "$APP.xcoff" "$APP.pef" "$APP.manifest.txt" "$SYMBOLS/"
{
    echo "commit=$COMMIT"
    if [ -n "$(git status --porcelain)" ]; then echo "tree=modified"; else echo "tree=clean"; fi
    echo "opt=$OPT"
    echo "bringup=$BRINGUP"
    sed -n 's/^pef_sha256=/&/p' "$APP.manifest.txt"
    (cd "$RELEASE" && sha256sum ET_Install.img.bin ET_Bin.img.bin)
} > "$RELEASE/package.txt"
cat "$RELEASE/package.txt"
ls -l "$RELEASE"
