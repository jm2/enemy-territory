#!/usr/bin/env python3
"""Finish, verify and export Retro68-built Classic Mac OS applications.

Retro68's Rez combines the MakePEF output (the data fork) with the compiled
resources and writes the application in three host-independent containers:

  Name.bin              MacBinary: both forks and Finder info in one file
  Name.dsk              HFS disk image holding the application
  Name.ad + %Name.ad    AppleDouble pair (data fork, then resource fork and
                        Finder info), the layout genisoimage -hfs -double reads

Rez cannot set Finder flags. "finalize" sets kHasBundle, without which the
Finder ignores the application's BNDL/FREF/icon resources, in every container
and then verifies them. "verify" checks type/creator, flags, the data fork
against the PEF, and the resources a launchable PowerPC application needs.
"resource-fork" and "finder-info" export one container's resource fork and
Finder info for hosts that store real forks (macOS).
"""

import argparse
import binascii
import os
import struct
import sys
from dataclasses import dataclass

K_HAS_BUNDLE = 0x2000
PEF_MAGIC = b"Joy!peffpwpc"
DEFAULT_REQUIRED = ("cfrg", "SIZE", "BNDL", "FREF", "ICN#", "ics#", "icl8", "ics8")

APPLEDOUBLE_MAGIC = 0x00051607
APPLEDOUBLE_VERSION = 0x00020000
APPLEDOUBLE_RESOURCE_FORK = 2
APPLEDOUBLE_FINDER_INFO = 9

HFS_MDB_OFFSET = 1024
HFS_FILE_RECORD = 2
HFS_FILE_RECORD_SIZE = 102


class FormatError(ValueError):
    """A container or resource fork is malformed or unsupported."""


@dataclass
class MacFile:
    name: str
    file_type: bytes
    creator: bytes
    flags: int
    data: bytes
    resource_fork: bytes


def _unpack(fmt, buf, offset, what):
    try:
        return struct.unpack_from(fmt, buf, offset)
    except struct.error:
        raise FormatError(f"truncated {what}") from None


def _round128(length):
    return (length + 127) & ~127


def _mac_name(raw):
    return bytes(raw).decode("mac_roman")


# MacBinary ------------------------------------------------------------------

def _macbinary_header(path):
    with open(path, "rb") as stream:
        header = bytearray(stream.read(128))
    if len(header) < 128:
        raise FormatError(f"{path}: truncated MacBinary header")
    if header[0] != 0 or header[74] != 0 or not 1 <= header[1] <= 63:
        raise FormatError(f"{path}: not a MacBinary file")
    stored_crc = struct.unpack_from(">H", header, 124)[0]
    if stored_crc != binascii.crc_hqx(bytes(header[:124]), 0):
        raise FormatError(f"{path}: MacBinary header CRC mismatch")
    return header


def read_macbinary(path):
    header = _macbinary_header(path)
    data_length, resource_length = struct.unpack_from(">II", header, 83)
    secondary_length = struct.unpack_from(">H", header, 120)[0]
    data_offset = 128 + _round128(secondary_length)
    resource_offset = data_offset + _round128(data_length)
    with open(path, "rb") as stream:
        stream.seek(data_offset)
        data = stream.read(data_length)
        stream.seek(resource_offset)
        resource_fork = stream.read(resource_length)
    if len(data) != data_length or len(resource_fork) != resource_length:
        raise FormatError(f"{path}: MacBinary forks are truncated")
    return MacFile(
        name=_mac_name(header[2:2 + header[1]]),
        file_type=bytes(header[65:69]),
        creator=bytes(header[69:73]),
        flags=header[73] << 8 | header[101],
        data=data,
        resource_fork=resource_fork,
    )


def _set_macbinary_flags(path, mask):
    header = _macbinary_header(path)
    header[73] |= mask >> 8
    header[101] |= mask & 0xFF
    struct.pack_into(">H", header, 124, binascii.crc_hqx(bytes(header[:124]), 0))
    with open(path, "r+b") as stream:
        stream.write(header)


# AppleDouble ----------------------------------------------------------------

def _appledouble_data_path(path):
    directory, base = os.path.split(path)
    prefix = "%" if base.startswith("%") else "._"
    return os.path.join(directory, base[len(prefix):])


def _appledouble_entries(path, blob):
    magic, version = _unpack(">II", blob, 0, f"{path}: AppleDouble header")
    if magic != APPLEDOUBLE_MAGIC or version != APPLEDOUBLE_VERSION:
        raise FormatError(f"{path}: not an AppleDouble version 2 header")
    count = _unpack(">H", blob, 24, f"{path}: AppleDouble header")[0]
    entries = {}
    for index in range(count):
        entry_id, offset, length = _unpack(
            ">III", blob, 26 + 12 * index, f"{path}: AppleDouble entries")
        if offset + length > len(blob):
            raise FormatError(f"{path}: AppleDouble entry {entry_id} is truncated")
        entries[entry_id] = (offset, length)
    finder = entries.get(APPLEDOUBLE_FINDER_INFO)
    if finder is None or finder[1] < 16:
        raise FormatError(f"{path}: AppleDouble has no Finder info")
    return entries


def read_appledouble(path):
    with open(path, "rb") as stream:
        blob = stream.read()
    entries = _appledouble_entries(path, blob)
    finder_offset = entries[APPLEDOUBLE_FINDER_INFO][0]
    resource_fork = b""
    if APPLEDOUBLE_RESOURCE_FORK in entries:
        offset, length = entries[APPLEDOUBLE_RESOURCE_FORK]
        resource_fork = blob[offset:offset + length]
    data_path = _appledouble_data_path(path)
    with open(data_path, "rb") as stream:
        data = stream.read()
    return MacFile(
        name=os.path.basename(data_path),
        file_type=blob[finder_offset:finder_offset + 4],
        creator=blob[finder_offset + 4:finder_offset + 8],
        flags=struct.unpack_from(">H", blob, finder_offset + 8)[0],
        data=data,
        resource_fork=resource_fork,
    )


def _set_appledouble_flags(path, mask):
    with open(path, "rb") as stream:
        blob = stream.read()
    flags_offset = _appledouble_entries(path, blob)[APPLEDOUBLE_FINDER_INFO][0] + 8
    flags = struct.unpack_from(">H", blob, flags_offset)[0] | mask
    with open(path, "r+b") as stream:
        stream.seek(flags_offset)
        stream.write(struct.pack(">H", flags))


# HFS disk image -------------------------------------------------------------

class _HfsVolume:
    """Reads the catalog of an HFS volume written by Retro68/libhfs.

    Forks and the catalog must fit in their three MDB/catalog extents; a
    freshly written volume never needs the extents overflow file.
    """

    def __init__(self, path, stream):
        self.path = path
        self.stream = stream
        mdb = self._read(HFS_MDB_OFFSET, 162, "master directory block")
        if mdb[:2] != b"BD":
            raise FormatError(f"{path}: not an HFS volume")
        self.block_size = struct.unpack_from(">I", mdb, 0x14)[0]
        self.first_block = struct.unpack_from(">H", mdb, 0x1C)[0] * 512
        if self.block_size == 0 or self.block_size % 512:
            raise FormatError(f"{path}: invalid HFS allocation block size")
        catalog_length = struct.unpack_from(">I", mdb, 0x92)[0]
        self.catalog = self._chunks(mdb[0x96:0xA2], catalog_length, "catalog")

    def _read(self, offset, length, what):
        self.stream.seek(offset)
        blob = self.stream.read(length)
        if len(blob) != length:
            raise FormatError(f"{self.path}: truncated {what}")
        return blob

    def _chunks(self, extent_record, length, what):
        chunks = []
        remaining = length
        for start, count in struct.iter_unpack(">HH", extent_record):
            if remaining <= 0:
                break
            size = min(count * self.block_size, remaining)
            chunks.append((self.first_block + start * self.block_size, size))
            remaining -= size
        if remaining > 0:
            raise FormatError(f"{self.path}: {what} uses overflow extents (unsupported)")
        return chunks

    def _read_fork(self, extent_record, length, what):
        return b"".join(self._read(offset, size, what)
                        for offset, size in self._chunks(extent_record, length, what))

    def _catalog_offset(self, position):
        for offset, size in self.catalog:
            if position < size:
                return offset + position
            position -= size
        raise FormatError(f"{self.path}: catalog node lies outside the catalog")

    def _node(self, number, node_size):
        offset = self._catalog_offset(number * node_size)
        return offset, self._read(offset, node_size, "catalog node")

    def file_records(self):
        """Yield (image offset of record data, parent ID, name, record)."""
        _, header = self._node(0, 512)
        if header[8] != 1:
            raise FormatError(f"{self.path}: malformed catalog header node")
        first_leaf, _, node_size = struct.unpack_from(">IIH", header, 24)
        node_number, visited = first_leaf, set()
        while node_number:
            if node_number in visited:
                raise FormatError(f"{self.path}: catalog leaf chain loops")
            visited.add(node_number)
            node_offset, node = self._node(node_number, node_size)
            if node[8] != 0xFF:
                raise FormatError(f"{self.path}: catalog leaf chain reaches a non-leaf node")
            for index in range(struct.unpack_from(">H", node, 10)[0]):
                record = struct.unpack_from(">H", node, node_size - 2 * (index + 1))[0]
                key_length = node[record]
                parent_id = struct.unpack_from(">I", node, record + 2)[0]
                name = node[record + 7:record + 7 + node[record + 6]]
                data = record + 1 + key_length
                data += data & 1
                if node[data] == HFS_FILE_RECORD:
                    body = node[data:data + HFS_FILE_RECORD_SIZE]
                    if len(body) != HFS_FILE_RECORD_SIZE:
                        raise FormatError(f"{self.path}: truncated catalog file record")
                    yield node_offset + data, parent_id, _mac_name(name), body
            node_number = struct.unpack_from(">I", node, 0)[0]

    def find(self, name):
        try:
            matches = [(offset, record) for offset, _, record_name, record
                       in self.file_records() if record_name == name]
        except (IndexError, struct.error):
            raise FormatError(f"{self.path}: malformed catalog leaf node") from None
        if len(matches) != 1:
            state = "not found" if not matches else "ambiguous"
            raise FormatError(f"{self.path}: file {name!r} is {state} in the HFS catalog")
        return matches[0]

    def read_file(self, name):
        _, record = self.find(name)
        data_length = struct.unpack_from(">I", record, 26)[0]
        resource_length = struct.unpack_from(">I", record, 36)[0]
        return MacFile(
            name=name,
            file_type=record[4:8],
            creator=record[8:12],
            flags=struct.unpack_from(">H", record, 12)[0],
            data=self._read_fork(record[74:86], data_length, "data fork"),
            resource_fork=self._read_fork(record[86:98], resource_length, "resource fork"),
        )


def _hfs_default_name(path):
    return os.path.splitext(os.path.basename(path))[0][:31]


def read_hfs_file(path, name=None):
    with open(path, "rb") as stream:
        return _HfsVolume(path, stream).read_file(name or _hfs_default_name(path))


def _set_hfs_flags(path, mask, name=None):
    with open(path, "r+b") as stream:
        offset, record = _HfsVolume(path, stream).find(name or _hfs_default_name(path))
        flags = struct.unpack_from(">H", record, 12)[0] | mask
        stream.seek(offset + 12)
        stream.write(struct.pack(">H", flags))


# Containers -----------------------------------------------------------------

def container_kind(path):
    base = os.path.basename(path)
    if base.startswith("%") or base.startswith("._"):
        return "appledouble"
    extension = os.path.splitext(base)[1].lower()
    if extension == ".bin":
        return "macbinary"
    if extension in (".dsk", ".img", ".iso"):
        return "hfs"
    raise FormatError(f"{path}: unknown container (expected Name.bin, Name.dsk, "
                      "%Name or ._Name)")


def load(path, hfs_name=None):
    kind = container_kind(path)
    if kind == "macbinary":
        return read_macbinary(path)
    if kind == "appledouble":
        return read_appledouble(path)
    return read_hfs_file(path, hfs_name)


def set_flags(path, mask, hfs_name=None):
    kind = container_kind(path)
    if kind == "macbinary":
        _set_macbinary_flags(path, mask)
    elif kind == "appledouble":
        _set_appledouble_flags(path, mask)
    else:
        _set_hfs_flags(path, mask, hfs_name)


def finder_info(mac_file):
    """The 32-byte FInfo + FXInfo a real HFS file would carry."""
    return (mac_file.file_type + mac_file.creator
            + struct.pack(">H", mac_file.flags) + bytes(22))


# Resource forks -------------------------------------------------------------

def parse_resource_fork(fork):
    """Return {type: {id: (name, data)}} for a resource fork."""
    if not fork:
        return {}
    data_offset, map_offset, data_length, map_length = _unpack(
        ">IIII", fork, 0, "resource fork header")
    if (data_offset + data_length > len(fork) or map_offset + map_length > len(fork)
            or map_length < 30):
        raise FormatError("resource fork header points outside the fork")
    type_list_offset, name_list_offset = _unpack(
        ">HH", fork, map_offset + 24, "resource map")
    type_list = map_offset + type_list_offset
    name_list = map_offset + name_list_offset
    type_count = (_unpack(">H", fork, type_list, "resource type list")[0] + 1) & 0xFFFF
    resources = {}
    for type_index in range(type_count):
        raw_type, count, reference_offset = _unpack(
            ">4sHH", fork, type_list + 2 + 8 * type_index, "resource type list")
        by_id = resources.setdefault(_mac_name(raw_type), {})
        for reference_index in range(count + 1):
            resource_id, name_offset, attributes_and_offset = _unpack(
                ">hHI", fork, type_list + reference_offset + 12 * reference_index,
                "resource reference list")
            body_offset = data_offset + (attributes_and_offset & 0xFFFFFF)
            length = _unpack(">I", fork, body_offset, "resource data")[0]
            body = fork[body_offset + 4:body_offset + 4 + length]
            if len(body) != length:
                raise FormatError(f"resource '{_mac_name(raw_type)}' "
                                  f"({resource_id}) is truncated")
            name = None
            if name_offset != 0xFFFF:
                name_start = name_list + name_offset
                name_length = _unpack(">B", fork, name_start, "resource name")[0]
                name = _mac_name(fork[name_start + 1:name_start + 1 + name_length])
            by_id[resource_id] = (name, body)
    return resources


def _cfrg_is_powerpc_application(body):
    """True if a cfrg names a PowerPC application in the whole data fork."""
    if len(body) < 32 or struct.unpack_from(">I", body, 8)[0] != 1:
        return False
    offset = 32
    for _ in range(struct.unpack_from(">I", body, 28)[0]):
        if offset + 42 > len(body):
            return False
        architecture = body[offset:offset + 4]
        usage, location = body[offset + 22], body[offset + 23]
        fork_offset, fork_length = struct.unpack_from(">II", body, offset + 24)
        member_size = struct.unpack_from(">H", body, offset + 40)[0]
        if (architecture == b"pwpc" and usage == 1 and location == 1
                and fork_offset == 0 and fork_length == 0):
            return True
        if member_size < 42:
            return False
        offset += member_size
    return False


def verify(mac_file, pef=None, file_type=b"APPL", creator=b"WfET",
           required=DEFAULT_REQUIRED):
    """Return a list of reasons the application would not launch correctly."""
    problems = []
    if mac_file.file_type != file_type or mac_file.creator != creator:
        problems.append(
            f"type/creator is {_mac_name(mac_file.file_type)}/"
            f"{_mac_name(mac_file.creator)}, want {_mac_name(file_type)}/"
            f"{_mac_name(creator)}")
    if not mac_file.data.startswith(PEF_MAGIC):
        problems.append("data fork is not a PowerPC PEF (Joy!peffpwpc)")
    if pef is not None and mac_file.data != pef:
        problems.append("data fork differs from the PEF")
    try:
        resources = parse_resource_fork(mac_file.resource_fork)
    except FormatError as error:
        return problems + [f"resource fork: {error}"]
    for resource_type in required:
        if not any(body for _, body in resources.get(resource_type, {}).values()):
            problems.append(f"missing or empty '{resource_type}' resource")
    if "cfrg" in resources:
        cfrg = resources["cfrg"].get(0)
        if cfrg is None or not _cfrg_is_powerpc_application(cfrg[1]):
            problems.append("'cfrg' (0) does not describe a PowerPC application "
                            "in the data fork")
    if resources.get("SIZE") and not {-1, 0} & set(resources["SIZE"]):
        problems.append("no 'SIZE' (-1) or (0) resource")
    for resource_id, (_, body) in resources.get("BNDL", {}).items():
        if body[:4] != creator:
            problems.append(f"'BNDL' ({resource_id}) signature "
                            f"{_mac_name(body[:4])!r} does not match the creator")
    if resources.get("BNDL") and not mac_file.flags & K_HAS_BUNDLE:
        problems.append("kHasBundle Finder flag is clear, so the Finder "
                        "ignores the BNDL")
    return problems


def describe(path, mac_file):
    types = " ".join(sorted(parse_resource_fork(mac_file.resource_fork)))
    return (f"{path}: {_mac_name(mac_file.file_type)}/{_mac_name(mac_file.creator)}, "
            f"flags 0x{mac_file.flags:04x}, data fork {len(mac_file.data)} bytes, "
            f"resource fork {len(mac_file.resource_fork)} bytes [{types}]")


def _verify_paths(paths, pef, file_type, creator, required, hfs_name):
    problems, forks = [], {}
    for path in paths:
        try:
            mac_file = load(path, hfs_name)
        except (OSError, FormatError) as error:
            problems.append(str(error))
            continue
        label = path
        if hfs_name and container_kind(path) == "hfs":
            label = f"{path}:{hfs_name}"
        found = verify(mac_file, pef, file_type, creator, required)
        problems.extend(f"{label}: {problem}" for problem in found)
        if not found:
            print(describe(label, mac_file))
        forks.setdefault(mac_file.resource_fork, []).append(path)
    if len(forks) > 1:
        problems.append("resource forks differ between " + ", ".join(paths))
    return problems


def _remove_outputs(paths):
    for path in paths:
        companions = [path]
        if container_kind(path) == "appledouble":
            companions.append(_appledouble_data_path(path))
        for companion in companions:
            try:
                os.remove(companion)
            except FileNotFoundError:
                pass


def _os_type(value):
    raw = value.encode("mac_roman")
    if len(raw) != 4:
        raise argparse.ArgumentTypeError(f"{value!r} is not a four-character code")
    return raw


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def add_checks(command):
        command.add_argument("--pef", help="PEF that every data fork must equal")
        command.add_argument("--type", type=_os_type, default=b"APPL")
        command.add_argument("--creator", type=_os_type, default=b"WfET")
        command.add_argument("--require", default=",".join(DEFAULT_REQUIRED),
                             help="comma-separated resource types that must be present")
        command.add_argument("--hfs-name", help="file name inside HFS images "
                             "(default: the image name without its extension)")
        command.add_argument("apps", nargs="+", metavar="APP")

    add_checks(commands.add_parser(
        "finalize", help="set kHasBundle, verify, and delete the outputs on failure"))
    add_checks(commands.add_parser("verify", help="verify application containers"))
    export = commands.add_parser("resource-fork", help="write an app's resource fork")
    export.add_argument("app")
    export.add_argument("output")
    info = commands.add_parser("finder-info", help="print an app's Finder info as hex")
    info.add_argument("app")
    args = parser.parse_args(argv)

    try:
        if args.command == "resource-fork":
            resource_fork = load(args.app).resource_fork
            with open(args.output, "wb") as stream:
                stream.write(resource_fork)
            return 0
        if args.command == "finder-info":
            print(finder_info(load(args.app)).hex().upper())
            return 0
    except (OSError, FormatError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    pef = None
    if args.pef:
        try:
            with open(args.pef, "rb") as stream:
                pef = stream.read()
        except OSError as error:
            print(f"Classic application check FAILED: {error}", file=sys.stderr)
            return 1
    required = tuple(filter(None, args.require.split(",")))
    if args.command == "finalize":
        for path in args.apps:
            try:
                if "BNDL" in parse_resource_fork(load(path, args.hfs_name).resource_fork):
                    set_flags(path, K_HAS_BUNDLE, args.hfs_name)
            except (OSError, FormatError):
                pass  # The verification below reports it with context.
    problems = _verify_paths(args.apps, pef, args.type, args.creator,
                             required, args.hfs_name)
    for problem in problems:
        print(f"Classic application check FAILED: {problem}", file=sys.stderr)
    if problems and args.command == "finalize":
        # Never leave an incomplete application that the build tool would
        # consider up to date.
        _remove_outputs(args.apps)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
