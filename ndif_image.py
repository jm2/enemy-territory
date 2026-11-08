#!/usr/bin/env python3
"""Wrap a raw volume as a Disk Copy 6 read/write image (NDIF), in MacBinary.

Usage: ndif_image.py VOLUME OUTPUT.bin "Volume Name"

Mac OS 9 ships Disk Copy 6.3.3, which mounts only its own formats. It refuses a
bare HFS or HFS/ISO hybrid volume however the file is typed: 'rohd' gives -39,
'dImg' gives -8819, and it does not list type 'iso ' at all. A read/write NDIF
image is the volume as its data fork, plus a 'bcem' resource describing it,
typed 'dimg' with creator 'ddsk' (Disk Copy's own creator; 'dCpy' is not).

The layout here is the one Disk Copy 6.3.3 wrote for its own 2.5 MB and 10 MB
read/write images, read back from the files it made:

  bcem 128, named after the volume, big-endian:
    0x00  u16    10 (version)
    0x02  u16    0
    0x04  Str63  volume name (length byte and 63 bytes)
    0x44  u32    sectors of 512 bytes
    0x48  zeros  (no checksum)
    0x7c  u32    chunk count, the end marker included
    0x80  chunks of 12 bytes: u24 first sector, u8 kind, u32 data fork offset,
          u32 length. Kind 2 copies from the data fork; kind 0xff ends the
          table at the sector count, with offset sectors * 512 and length 0.

Disk Copy used one chunk for its own images. This splits the volume into 1 MB
chunks, which is what was mounted under ClassicMac at 279 MB.

The volume is not changed: the data fork is the input file, byte for byte.
"""
import os
import struct
import sys
import tempfile

import macbinary_encode

FILE_TYPE = "dimg"
CREATOR = "ddsk"
SECTOR = 512
CHUNK_SECTORS = 0x800
BCEM_VERSION = 10
KIND_COPY = 2
KIND_END = 0xFF
MAX_SECTORS = 1 << 24


def volume_name_bytes(name):
    try:
        raw = name.encode("mac_roman")
    except UnicodeEncodeError as exc:
        raise ValueError(f"volume name is not representable in MacRoman: {name!r}") from exc
    if not 1 <= len(raw) <= 27:
        raise ValueError(f"an HFS volume name is 1 to 27 bytes: {name!r}")
    if b":" in raw:
        raise ValueError(f"an HFS volume name cannot contain ':': {name!r}")
    return raw


def bcem(name, sectors, chunk_sectors=CHUNK_SECTORS):
    """Return the 'bcem' resource for a volume of sectors 512-byte sectors.

    chunk_sectors=None gives one chunk, as Disk Copy writes for itself.
    """
    raw_name = volume_name_bytes(name)
    if not 0 < sectors < MAX_SECTORS:
        raise ValueError(f"a volume of {sectors} sectors does not fit NDIF's 24-bit sector field")
    step = sectors if chunk_sectors is None else chunk_sectors
    if step <= 0:
        raise ValueError("chunk_sectors must be positive")
    chunks = []
    for first in range(0, sectors, step):
        count = min(step, sectors - first)
        chunks.append(struct.pack(">I", first)[1:] + bytes([KIND_COPY])
                      + struct.pack(">II", first * SECTOR, count * SECTOR))
    chunks.append(struct.pack(">I", sectors)[1:] + bytes([KIND_END])
                  + struct.pack(">II", sectors * SECTOR, 0))
    header = (struct.pack(">HH", BCEM_VERSION, 0)
              + bytes([len(raw_name)]) + raw_name.ljust(63, b"\0")
              + struct.pack(">I", sectors))
    header += bytes(0x7C - len(header))
    return header + struct.pack(">I", len(chunks)) + b"".join(chunks)


def resource_fork(resources):
    """Build a resource fork from [(type, id, name or None, data)]."""
    by_type = {}
    for res_type, res_id, name, data in resources:
        by_type.setdefault(res_type, []).append((res_id, name, data))
    data_area = b""
    names = b""
    references = b""
    type_list = struct.pack(">H", len(by_type) - 1)
    reference_base = 2 + 8 * len(by_type)
    for res_type, entries in by_type.items():
        type_list += res_type + struct.pack(">HH", len(entries) - 1,
                                            reference_base + len(references))
        for res_id, name, data in entries:
            name_offset = 0xFFFF
            if name is not None:
                name_offset = len(names)
                names += bytes([len(name)]) + name
            references += struct.pack(">hHII", res_id, name_offset, len(data_area), 0)
            data_area += struct.pack(">I", len(data)) + data
    type_list += references
    data_offset = 256
    map_offset = data_offset + len(data_area)
    map_length = 28 + len(type_list) + len(names)
    header = struct.pack(">IIII", data_offset, map_offset, len(data_area), map_length)
    resource_map = (header + struct.pack(">IHH", 0, 0, 0)
                    + struct.pack(">HH", 28, 28 + len(type_list)) + type_list + names)
    return header + bytes(data_offset - len(header)) + data_area + resource_map


def ndif_resource_fork(name, sectors, chunk_sectors=CHUNK_SECTORS):
    return resource_fork([
        (b"bcem", 128, volume_name_bytes(name), bcem(name, sectors, chunk_sectors)),
        # What the Finder names when Disk Copy is missing.
        (b"STR ", -16396, None, b"\x09Disk Copy"),
    ])


def write_image(volume_path, output_path, name, chunk_sectors=CHUNK_SECTORS):
    size = os.path.getsize(volume_path)
    if size == 0 or size % SECTOR:
        raise ValueError(f"{volume_path}: {size} bytes is not a whole number of "
                         f"{SECTOR}-byte sectors")
    fork = ndif_resource_fork(name, size // SECTOR, chunk_sectors)
    with tempfile.TemporaryDirectory() as scratch:
        fork_path = os.path.join(scratch, "bcem.rsrc")
        with open(fork_path, "wb") as stream:
            stream.write(fork)
        macbinary_encode.encode_macbinary(volume_path, output_path, FILE_TYPE,
                                          CREATOR, fork_path)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 3:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        write_image(argv[0], argv[1], argv[2])
    except (OSError, ValueError) as exc:
        print(f"ndif_image.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
