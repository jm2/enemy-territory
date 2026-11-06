#!/bin/bash
# Inside the et-ppc image: an x86-64 ETL server and a big-endian client under qemu-user.
# usage (from the repo): scripts/ppc.sh bash tests/host/ppc_connect.sh [ETL server args]
O=/src/build-ppc/connect; rm -rf $O; mkdir -p $O /tmp/sv /tmp/cl/legacy
cp /data/run/clhome/.etwolf/legacy/drive.cfg /tmp/cl/legacy/drive.cfg
sed -i 's/^wait 1200$/wait 300/; s/^wait 900$/wait 200/' /tmp/cl/legacy/drive.cfg
BASE=${BASE:-/data/run/base}
cd /data/run/base
${SERVER:-./etlded.x86_64} +set fs_basepath $BASE +set fs_homepath /tmp/sv +set fs_game legacy +set dedicated 1 \
  +set net_ip 127.0.0.1 +set net_port 27960 +set g_guidCheck 1 +set sv_pure 1 +set sv_maxclients 8 +set g_doWarmup 0 \
  +set sv_master1 "" +set sv_master2 "" +set sv_master3 "" +set sv_master4 "" +set sv_master5 "" \
  "$@" +map oasis > $O/server.log 2>&1 </dev/null &
SV=$!
sleep 20
timeout ${LIMIT:-400} qemu-ppc-static /src/build-ppc/et_host +set fs_basepath /data/run/base +set fs_homepath /tmp/cl \
  +set com_hunkMegs 128 +set com_maxfps 30 +set com_introplayed 1 +set ttycon 0 +set net_port 27970 +set cl_timeout 200 +exec drive.cfg > $O/client.log 2>&1 </dev/null
echo "client exit $?" >> $O/client.log
kill $SV; wait $SV 2>/dev/null
