#!/bin/bash
# Loopback only, with no access to $HOME beyond the test tree.
HERE=$(cd "$(dirname "$0")" && pwd)
ET_DATA=${ET_DATA:-$HOME/et-data}
R=$ET_DATA/run; O=$ET_DATA/compat-out; rm -rf "$O"; mkdir -p "$O"
exec env SPOOF="${SPOOF:-}" CLIENT="${CLIENT:-./et.x86}" CLGUID="${CLGUID:-unknown}" GUIDCHECK="${GUIDCHECK:-1}" PURE="${PURE:-1}" DL="${DL:-0}" DEV="${DEV:-0}" LIMIT="${LIMIT:-240}" \
 bwrap --unshare-all --die-with-parent --new-session \
 --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
 --ro-bind /etc/alternatives /etc/alternatives --ro-bind-try /etc/ld.so.cache /etc/ld.so.cache \
 --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
 --ro-bind "$R/base" /base --bind "$R/svhome" /svhome --bind "$R/clhome" /clhome --bind "$O" /out \
 --ro-bind "$HERE/inner.sh" /inner.sh --setenv PATH /usr/bin --setenv TMPDIR /tmp \
 /bin/bash /inner.sh
