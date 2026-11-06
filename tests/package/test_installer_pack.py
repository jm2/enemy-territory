#!/usr/bin/env python3
"""Pack a small installer and read it back; verify must notice any changed byte."""
import hashlib
import os
import subprocess
import sys

root, scratch = sys.argv[1:3]
sys.path.insert(0, root)
import macbinary_encode  # noqa: E402

work = os.path.join(scratch, "pack")
os.makedirs(work + "/tree/etmain")
pack = os.path.join(root, "scripts/installer_pack.py")


def run(*args):
    return subprocess.run([sys.executable, "-I", "-B", pack, *args], capture_output=True, text=True)


def macbinary(name, data, rsrc, type_):
    with open(work + "/" + name, "wb") as out:
        out.write(data)
    with open(work + "/" + name + ".r", "wb") as out:
        out.write(rsrc)
    macbinary_encode.encode_macbinary(work + "/" + name, work + "/" + name + ".bin", type_, "WfET", work + "/" + name + ".r")


macbinary("inst", b"Joy!peff" + b"p" * 500, b"I" * 40, "APPL")
macbinary("app", b"Joy!peff" + b"a" * 700, b"R" * 90, "APPL")
pak = b"pak" * 1000
with open(work + "/tree/etmain/pak0.pk3", "wb") as out:
    out.write(pak)
with open(work + "/tree/METParms.txt", "wb") as out:
    out.write(b"+set x 1\n")
with open(work + "/sums", "w") as out:
    out.write("%s etmain/pak0.pk3\n" % hashlib.sha256(pak).hexdigest())

made = run("pack", work + "/inst.bin", work + "/out", work + "/out.r", work + "/tree", work + "/app.bin",
           "etmain/pak0.pk3", "METParms.txt")
assert made.returncode == 0, made.stderr
macbinary_encode.encode_macbinary(work + "/out", work + "/ET Installer.bin", "APPL", "WfEI", work + "/out.r")
installer = work + "/ET Installer.bin"
good = run("verify", installer, work + "/sums", work + "/app.bin", "METParms.txt")
assert good.returncode == 0, good.stderr
assert run("verify", installer, work + "/sums", work + "/app.bin").returncode != 0, "an extra file was missed"
with open(installer, "rb") as stream:
    whole = bytearray(stream.read())
at = whole.index(b"pakpak") + 100
whole[at] ^= 1
with open(installer, "wb") as out:
    out.write(whole)
assert run("verify", installer, work + "/sums", work + "/app.bin", "METParms.txt").returncode != 0, "a changed byte was missed"
print("installer pack tests passed")
