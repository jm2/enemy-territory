#!/bin/bash
# Runs a sanitized et_host (cmake -DET_SANITIZE=..., see README.md) through the
# compat scenarios, one run.sh each, and keeps every scenario's logs.
# Usage: tests/compat/sanitize.sh <et_host> <evidence dir> [scenario...]
# Scenarios: menus maps listen dl restart (default: all). STRICT=1 halts at the
# first report. et_host is copied into run/base and the old one put back after;
# the cfgs and zz_dltest.pk3 it adds to run/clhome and run/svhome are removed.
# Markers are "set san_mark" and "cvarlist san_mark": echo needs cgame, and a
# pure server forces developer 0, which hides the cvar_set line (and show_menu,
# so the in-game menus are opened on the listen server's devmap).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ET_DATA=${ET_DATA:-$HOME/et-data}
R=$ET_DATA/run; O=$ET_DATA/compat-out
BIN=${1:?et_host}; EV=${2:?evidence dir}; shift 2
SCEN=${*:-menus maps listen dl restart}
CL=$R/clhome/.etwolf/legacy; SVL=$R/svhome/legacy
mkdir -p "$EV"
SAVED=$(mktemp -p "${TMPDIR:-/var/tmp}")
cp -p "$R/base/et_host" "$SAVED"
cleanup() {
    cp -p "$SAVED" "$R/base/et_host"; rm -f "${SAVED:?}"
    rm -f "$CL"/san_*.cfg "$CL/san_ubsan.supp" "$SVL/zz_dltest.pk3" "$CL/zz_dltest.pk3"
}
trap cleanup EXIT
cp "$BIN" "$R/base/et_host"
if [ "${STRICT:-0}" = 1 ]; then
    cp "$HERE/ubsan-legacy.supp" "$CL/san_ubsan.supp"
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0:log_path=/out/asan
    export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:log_path=/out/ubsan:suppressions=/clhome/.etwolf/legacy/san_ubsan.supp
else
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:log_path=/out/asan
    export UBSAN_OPTIONS=print_stacktrace=1:log_path=/out/ubsan
fi

MENUS="main options options_system options_system_extended options_system_gamma options_controls
 options_controls_advanced options_customise_game options_customise_hud options_customise_hud_extended
 options_customise_hudeditor_fui options_homepath playonline playonline_connecttoip playonline_serverinfo
 hostgame hostgame_advanced hostgame_extended profile profile_create profile_rename mods viewreplay
 credits_etlegacy credits_et260 credits_splashdamage credits_idsoftware credits_activision credits_additional
 etlegacy_halloffame changelog_popup main_quickconnect popupError popupPassword vid_confirm"
INGAME="ingame_main ingame_serverinfo ingame_vote ingame_vote_map ingame_vote_misc ingame_vote_players ingame_messagemode"

{ echo "wait 300"; echo "ui_report"
  for m in $MENUS; do echo "set san_mark menu_$m"; echo "cvarlist san_mark"; echo "show_menu $m"; echo "wait 45"; done
  echo "ui_cache"; echo "wait 60"; echo "ui_load"; echo "wait 150"; echo "set san_mark menus_done"; echo "cvarlist san_mark"; echo "quit"; } >"$CL/san_menus.cfg"
cat >"$CL/san_map.cfg" <<'C'
connect 127.0.0.1:27960
wait 1500
team b 1 3 12
wait 600
set san_mark "in game"
cvarlist san_mark
wait 300
disconnect
wait 90
quit
C
{ cat <<'C'
set sv_pure 0
set g_gametype 2
map goldrush
wait 900
team r 1 3 12
wait 450
set san_mark "hosting goldrush"
cvarlist san_mark
map radar
wait 900
team b 1 3 12
wait 450
set san_mark "changed to radar"
cvarlist san_mark
map_restart
wait 900
set san_mark "restarted"
cvarlist san_mark
devmap battery
wait 900
team b 1 3 12
wait 300
set san_mark "devmap battery"
cvarlist san_mark
C
  for m in $INGAME; do echo "show_menu $m"; echo "wait 30"; done
  cat <<'C'
killserver
wait 150
quit
C
} >"$CL/san_listen.cfg"
{ cat <<'C'
connect 127.0.0.1:27960
wait 1500
team b 1 3 12
wait 600
set san_mark "in game"
cvarlist san_mark
vid_restart
wait 600
set san_mark "after vid_restart"
cvarlist san_mark
snd_restart
wait 450
set san_mark "after snd_restart"
cvarlist san_mark
disconnect
wait 150
set san_mark "connecting again"
cvarlist san_mark
connect 127.0.0.1:27960
wait 1500
set san_mark "reconnected"
cvarlist san_mark
team r 1 3 12
wait 600
disconnect
wait 90
connect 127.0.0.1:27960
wait 1200
set san_mark "connected again"
cvarlist san_mark
disconnect
wait 90
quit
C
} >"$CL/san_restart.cfg"

run() {   # name, then env for run.sh
    local name=$1; shift
    echo "== $name"
    env "$@" GUIDCHECK=0 CLIENT=./et_host "$HERE/run.sh"
    rm -rf "${EV:?}/$name"; cp -r "$O" "$EV/$name"
    grep -a -E "^client exit|CLIENT_TIMEOUT" "$EV/$name/client.log"
    ls "$EV/$name" | grep -E "^(asan|ubsan)" | tr '\n' ' '; echo
}
for s in $SCEN; do
    case $s in
        menus) run menus SV=0 DEV=1 SND=1 DRIVE=san_menus.cfg LIMIT=300 ;;
        maps) for m in battery fueldump goldrush oasis radar railgun; do
                  run "map_$m" MAP=$m PURE=1 SND=1 DEV=1 DRIVE=san_map.cfg LIMIT=300
              done ;;
        listen) run listen SV=0 DEV=1 SND=1 DRIVE=san_listen.cfg LIMIT=600 ;;
        dl) D=$(mktemp -d -p "${TMPDIR:-/var/tmp}")
            unzip -q -d "$D" "$R/base/legacy/legacy_v2.86.2.pk3" maps/oasis.script
            (cd "$D" && zip -q -X zz_dltest.pk3 maps/oasis.script)
            cp "$D/zz_dltest.pk3" "$SVL/zz_dltest.pk3"; rm -rf "${D:?}"
            run dl DL=1 MAP=oasis PURE=1 SND=1 DEV=1 DRIVE=san_map.cfg LIMIT=300
            if cmp -s "$SVL/zz_dltest.pk3" "$CL/zz_dltest.pk3"; then echo "download matches"; else echo "DOWNLOAD MISSING OR DIFFERENT"; fi
            rm -f "$SVL/zz_dltest.pk3" "$CL/zz_dltest.pk3" ;;
        restart) run restart PURE=1 SND=1 DEV=1 DRIVE=san_restart.cfg LIMIT=600 ;;
        *) echo "unknown scenario $s" >&2; exit 2 ;;
    esac
done
