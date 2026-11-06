#!/bin/bash
# Runs inside the sandbox: Xvfb + ETL v2.86.2 server + retail ET 2.60b client.
cd /base
Xvfb :9 -screen 0 800x600x24 +extension GLX +render -noreset >/out/xvfb.log 2>&1 &
sleep 2
export DISPLAY=:9 LIBGL_ALWAYS_SOFTWARE=1 HOME=/clhome
./etlded.x86_64 +set fs_basepath /base +set fs_homepath /svhome +set fs_game legacy +set dedicated 1 \
  +set net_ip 127.0.0.1 +set net_port 27960 +set g_guidCheck ${GUIDCHECK:-1} +set sv_pure ${PURE:-1} +set sv_allowDownload 1 +set sv_maxclients 8 \
  +set g_warmup 5 +set g_doWarmup 0 +set match_warmupDamage 1 +set g_gametype 2 +set logfile 2 \
  +set sv_master1 "" +set sv_master2 "" +set sv_master3 "" +set sv_master4 "" +set sv_master5 "" \
  +map oasis >/out/server.log 2>&1 </dev/null &
SV=$!
sleep ${SVWAIT:-20}
${CLIENT:-./et.x86} +set fs_basepath /base +set fs_homepath /clhome/.etwolf +set com_hunkMegs 128 +set com_maxfps 30 \
  +set r_mode 3 +set r_fullscreen 0 +set s_initsound 0 +set in_dgamouse 0 +set in_mouse 0 +set ttycon 0 +set com_introplayed 1 +set fs_game legacy \
  +set cl_allowDownload ${DL:-0} +set pb_cl_enable 0 +set cl_guid ${CLGUID:-unknown} +set net_port 27970 +set logfile 2 +set developer ${DEV:-0} \
  +exec drive.cfg >/out/client.log 2>&1 </dev/null &
CL=$!
for i in $(seq 1 ${LIMIT:-240}); do kill -0 $CL 2>/dev/null || break; sleep 1; done
if kill -0 $CL 2>/dev/null; then echo CLIENT_TIMEOUT >>/out/client.log; DISPLAY=:9 xwd -root -silent -out /out/timeout.xwd 2>/dev/null; kill $CL; fi
wait $CL; echo "client exit $?" >>/out/client.log
kill $SV; wait $SV 2>/dev/null
exit 0
