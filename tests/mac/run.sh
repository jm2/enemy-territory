#!/usr/bin/env bash
# The Quake III port's host regressions of the Mac layer, run against ET's src/mac9.
# They extract the real functions and run them against fake Toolbox headers under ASan/UBSan.
set -uo pipefail
export TMPDIR="${TMPDIR:-/var/tmp}"
cd "$(dirname "$0")"
fails=0
for test in crash event_queue glimp net startup_parms toolbox_init; do
    log=$(mktemp)
    if bash "run_mac_${test}_tests.sh" > "$log" 2>&1; then
        echo "mac $test: passed"
    else
        echo "FAIL: mac $test"; tail -20 "$log"; fails=1
    fi
    rm -f "$log"
done
exit "$fails"
