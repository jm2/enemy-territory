#!/bin/bash
# globalservers against two fake masters on loopback, and a third host that answers
# unasked. Only the two masters' lists may reach the client's list.
# Usage: CLIENT=./et_host tests/compat/master.sh   (et_host copied into run/base)
HERE=$(cd "$(dirname "$0")" && pwd)
ET_DATA=${ET_DATA:-$HOME/et-data}
R=$ET_DATA/run; O=$ET_DATA/compat-out; rm -rf "$O"; mkdir -p "$O/clhome/.etwolf/legacy"
cat > "$O/clhome/.etwolf/legacy/master.cfg" <<'C'
set sv_master1 127.0.0.1:27950
set sv_master2 127.0.0.1:27951
set sv_master3 ""
globalservers 0 84
globalservers 1 84
globalservers 2 84
globalservers 7 84
wait 300
quit
C
cat > "$O/masters.py" <<'P'
import socket, struct, sys, time
def entry(a, b, c, d, port):
    return b'\\' + bytes([a, b, c, d]) + struct.pack('>H', port)
def reply(entries):
    return b'\xff\xff\xff\xffgetserversResponse' + b''.join(entries) + b'\\EOT\0\0\0'
lists = {27950: [entry(10, 0, 0, i, 27960) for i in range(1, 4)],
         27951: [entry(10, 0, 1, i, 27960) for i in range(1, 3)]}
socks = {}
for port in lists:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1', port)); s.settimeout(0.1); socks[port] = s
rogue = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rogue.bind(('127.0.0.1', 27999))
log = open('/out/masters.log', 'w'); end = time.time() + float(sys.argv[1])
while time.time() < end:
    for port, s in socks.items():
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        log.write('%d asked by %s: %r\n' % (port, addr, data)); log.flush()
        s.sendto(reply(lists[port]), addr)
        # a host nobody asked, answering as the client waits
        rogue.sendto(reply([entry(6, 6, 6, i, 27960) for i in range(1, 51)]), addr)
        log.write('rogue answered %s\n' % (addr,)); log.flush()
P
exec env CLIENT="${CLIENT:-./et_host}" \
 bwrap --unshare-all --die-with-parent --new-session \
 --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
 --ro-bind /etc/alternatives /etc/alternatives --ro-bind-try /etc/ld.so.cache /etc/ld.so.cache \
 --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
 --ro-bind "$R/base" /base --bind "$O/clhome" /clhome --bind "$O" /out \
 --setenv PATH /usr/bin --setenv TMPDIR /tmp \
 /bin/bash -c '
cd /base
Xvfb :9 -screen 0 800x600x24 +extension GLX +render -noreset >/out/xvfb.log 2>&1 &
sleep 2
export DISPLAY=:9 LIBGL_ALWAYS_SOFTWARE=1 HOME=/clhome
python3 -I /out/masters.py 90 &
M=$!
sleep 1
timeout 90 $CLIENT +set fs_basepath /base +set fs_homepath /clhome/.etwolf +set com_hunkMegs 128 +set com_maxfps 30 \
  +set r_mode 3 +set r_fullscreen 0 +set s_initsound 0 +set in_dgamouse 0 +set in_mouse 0 +set ttycon 0 +set com_introplayed 1 \
  +set fs_game legacy +set pb_cl_enable 0 +set net_port 27970 +set developer 1 +exec master.cfg >/out/client.log 2>&1 </dev/null
echo "client exit $?" >>/out/client.log
kill $M 2>/dev/null; wait $M 2>/dev/null
exit 0'
