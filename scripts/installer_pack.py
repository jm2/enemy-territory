#!/usr/bin/env python3
"""Add the game to the one-file installer. The format is in src/mac9/et_installer_format.h.

  installer_pack.py pef-length PEF OUTPUT.r
  installer_pack.py pack INSTALLER.bin OUTPUT_DATA OUTPUT_RSRC ROOT APP.bin [PARENT/FILE ...]
  installer_pack.py verify INSTALLER.bin SUMS APP.bin [EXTRA_NAME ...]
"""
import hashlib
import os
import struct
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from macbinary_encode import calc_crc  # noqa: E402

MAX_ENTRIES = 16
FOLDER = b"fold"
# what the image's HFS map gives these files
TYPES = {".pk3": (b"Stak", b"WfET"), ".cfg": (b"TEXT", b"WfET"), ".txt": (b"TEXT", b"ttxt")}


def macbinary(path):
    """Fork offsets and Finder flags. Only a checked header is believed."""
    with open(path, "rb") as stream:
        head = stream.read(128)
    if len(head) != 128 or head[0] or head[74] or head[82] or calc_crc(head[:124]) != struct.unpack(">H", head[124:126])[0]:
        sys.exit("%s is not MacBinary" % path)
    data, rsrc = struct.unpack(">II", head[83:91])
    rsrc_at = 128 + (data + 127) // 128 * 128
    if rsrc_at + rsrc > os.path.getsize(path):
        sys.exit("%s is shorter than its forks" % path)
    return {"type": head[65:69], "creator": head[69:73], "flags": head[73] << 8 | head[101],
            "data": (128, data), "rsrc": (rsrc_at, rsrc)}


def read_at(path, where):
    with open(path, "rb") as stream:
        stream.seek(where[0])
        return stream.read(where[1])


def entry(name, parent, type_, creator, flags, data, rsrc, data_crc, rsrc_crc):
    raw = name.encode("mac_roman")
    if not 0 < len(raw) < 32 or b":" in raw:
        sys.exit("bad Mac file name %r" % name)
    return struct.pack(">32si4s4sIIIII", bytes([len(raw)]) + raw, parent, type_, creator, flags,
                       data, rsrc, data_crc, rsrc_crc)


def crc_file(path):
    crc, size = 0, 0
    with open(path, "rb") as stream:
        while chunk := stream.read(1 << 20):
            crc = zlib.crc32(chunk, crc)
            size += len(chunk)
    return crc, size


def pack(installer, out_data, out_rsrc, root, app, files):
    """The application first, then each file named relative to root, one folder deep."""
    pef = read_at(installer, macbinary(installer)["data"])
    if pef[:8] != b"Joy!peff":
        sys.exit("%s does not hold a PEF" % installer)
    entries, payload = [], []
    info = macbinary(app)
    app_data, app_rsrc = read_at(app, info["data"]), read_at(app, info["rsrc"])
    entries.append(entry("ET", -1, info["type"], info["creator"], info["flags"], len(app_data), len(app_rsrc),
                         zlib.crc32(app_data), zlib.crc32(app_rsrc)))
    payload += [app_data, app_rsrc]
    folders = {}
    for name in files:
        folder, _, leaf = name.rpartition("/")
        if "/" in folder:
            sys.exit("%s is too deep" % name)
        if folder and folder not in folders:
            folders[folder] = len(entries)
            entries.append(entry(folder, -1, FOLDER, b"\0\0\0\0", 0, 0, 0, 0, 0))
        type_, creator = TYPES[os.path.splitext(leaf)[1]]
        crc, size = crc_file(os.path.join(root, name))
        entries.append(entry(leaf, folders[folder] if folder else -1, type_, creator, 0, size, 0, crc, 0))
        payload.append(os.path.join(root, name))
    if len(entries) > MAX_ENTRIES:
        sys.exit("too many files for the installer")
    if len(set(files)) != len(files):
        sys.exit("a file is named twice")
    total = sum(len(part) if isinstance(part, bytes) else os.path.getsize(part) for part in payload)
    if total >= 1 << 31:
        sys.exit("the Mac installer holds under 2 GiB")
    header = b"ETIN" + struct.pack(">II", 1, len(entries)) + b"".join(entries)
    with open(out_data, "wb") as out:
        out.write(pef)
        out.write(header)
        for part in payload:
            if isinstance(part, bytes):
                out.write(part)
            else:
                with open(part, "rb") as stream:
                    while chunk := stream.read(1 << 20):
                        out.write(chunk)
        out.write(b"ETIT" + struct.pack(">III", len(pef), len(header), zlib.crc32(header)))
    with open(out_rsrc, "wb") as out:
        out.write(read_at(installer, macbinary(installer)["rsrc"]))


def verify(installer, sums, app, extra):
    """Read every file back out of the finished MacBinary by the format alone."""
    info = macbinary(installer)
    start, length = info["data"]
    with open(installer, "rb") as stream:
        def at(offset, size):
            stream.seek(start + offset)
            return stream.read(size)
        magic, header_at, header_len, header_crc = struct.unpack(">4sIII", at(length - 16, 16))
        if magic != b"ETIT" or at(0, 8) != b"Joy!peff":
            sys.exit("installer_pack: no trailer or PEF")
        header = at(header_at, header_len)
        if zlib.crc32(header) != header_crc or header[:4] != b"ETIN":
            sys.exit("installer_pack: bad header")
        version, count = struct.unpack(">II", header[4:12])
        if version != 1 or not 0 < count <= MAX_ENTRIES or header_len != 12 + count * 64:
            sys.exit("installer_pack: the Mac would refuse this header")
        names, where = [], header_at + header_len
        files = {}
        for i in range(count):
            raw = header[12 + i * 64:12 + (i + 1) * 64]
            name = raw[1:1 + raw[0]].decode("mac_roman")
            parent = struct.unpack(">i", raw[32:36])[0]
            data, rsrc, data_crc, rsrc_crc = struct.unpack(">IIII", raw[48:64])
            names.append(name)
            path = names[parent] + "/" + name if parent >= 0 else name
            if raw[36:40] == FOLDER:
                continue
            content = at(where, data)
            fork = at(where + data, rsrc)
            where += data + rsrc
            if zlib.crc32(content) != data_crc or zlib.crc32(fork) != rsrc_crc:
                sys.exit("installer_pack: %s fails its CRC" % path)
            files[path] = (raw[36:44], content, fork)
        if where != length - 16:
            sys.exit("installer_pack: forks do not reach the trailer")
    app_info = macbinary(app)
    expected = (app_info["type"] + app_info["creator"], read_at(app, app_info["data"]), read_at(app, app_info["rsrc"]))
    if files.pop("ET", None) != expected:
        sys.exit("installer_pack: the application differs")
    for line in open(sums):
        digest, path = line.split()
        if path not in files or hashlib.sha256(files.pop(path)[1]).hexdigest() != digest:
            sys.exit("installer_pack: %s differs or is missing" % path)
    if sorted(files) != sorted(extra):
        sys.exit("installer_pack: unexpected files: %s" % sorted(files))
    print("installer verified")


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "pef-length" and len(sys.argv) == 4:
        with open(sys.argv[3], "w") as out:
            out.write("#define ET_INSTALLER_PEF_BYTES %d\n" % os.path.getsize(sys.argv[2]))
    elif command == "pack" and len(sys.argv) >= 7:
        pack(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6], sys.argv[7:])
    elif command == "verify" and len(sys.argv) >= 5:
        verify(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
