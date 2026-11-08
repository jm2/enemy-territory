"""ndif_image.py, as Quake III's tests/test_packaging_tools.py NdifImageTests (833d8eb0)."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO))
import mac_app  # noqa: E402
import ndif_image  # noqa: E402

# The 'bcem' resource Disk Copy 6.3.3 wrote for its own 2.5 MB read/write
# image named "Untitled", read back from the file it made under ClassicMac.
DISK_COPY_BCEM_2_5_MB = bytes.fromhex(
    "000a000008556e7469746c6564" + "00" * 55 + "00001400" + "00" * 52
    + "00000002" "00000002" "00000000" "00280000" "001400ff" "00280000"
    "00000000")


class NdifImageTests(unittest.TestCase):
    """Images Disk Copy 6.3.3 mounts without Toast."""

    def write_volume(self, sectors):
        temp_path = Path(self.enterContext(tempfile.TemporaryDirectory()))
        volume = temp_path / "ET_Install.img"
        volume.write_bytes(bytes(range(256)) * (2 * sectors))
        return volume, temp_path / "ET_Install.img.bin"

    def test_one_chunk_matches_disk_copys_own_image(self):
        self.assertEqual(len(DISK_COPY_BCEM_2_5_MB), 152)
        self.assertEqual(ndif_image.bcem("Untitled", 5120, None), DISK_COPY_BCEM_2_5_MB)

    def test_chunks_cover_the_volume_in_order(self):
        sectors = 2 * ndif_image.CHUNK_SECTORS + 5
        resource = ndif_image.bcem("Enemy Territory", sectors)
        self.assertEqual(struct.unpack_from(">I", resource, 0x44)[0], sectors)
        count = struct.unpack_from(">I", resource, 0x7C)[0]
        self.assertEqual(count, 4)
        self.assertEqual(len(resource), 0x80 + 12 * count)
        expected_first = 0
        for index in range(count):
            entry = resource[0x80 + 12 * index:0x80 + 12 * (index + 1)]
            first = int.from_bytes(entry[:3], "big")
            kind = entry[3]
            offset, length = struct.unpack(">II", entry[4:])
            self.assertEqual(first, expected_first)
            self.assertEqual(offset, first * 512)
            if index == count - 1:
                self.assertEqual((kind, length), (0xFF, 0))
                self.assertEqual(first, sectors)
            else:
                self.assertEqual(kind, 2)
                expected_first += length // 512
        self.assertEqual(expected_first, sectors)

    def test_image_keeps_the_volume_and_is_typed_for_disk_copy(self):
        volume, output = self.write_volume(ndif_image.CHUNK_SECTORS + 1)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ndif_image.main([str(volume), str(output), "Enemy Territory"]), 0)
        image = mac_app.read_macbinary(str(output))
        self.assertEqual(image.name, "ET_Install.img")
        self.assertEqual((image.file_type, image.creator), (b"dimg", b"ddsk"))
        self.assertEqual(image.data, volume.read_bytes())
        resources = mac_app.parse_resource_fork(image.resource_fork)
        self.assertEqual(sorted(resources), ["STR ", "bcem"])
        name, body = resources["bcem"][128]
        self.assertEqual(name, "Enemy Territory")
        self.assertEqual(body, ndif_image.bcem("Enemy Territory", ndif_image.CHUNK_SECTORS + 1))
        self.assertEqual(resources["STR "][-16396], (None, b"\x09Disk Copy"))

    def test_refuses_what_disk_copy_could_not_describe(self):
        volume, output = self.write_volume(1)
        cases = {
            "not whole sectors": (lambda: volume.write_bytes(b"x" * 513), "Enemy Territory", "whole number"),
            "empty": (lambda: volume.write_bytes(b""), "Enemy Territory", "whole number"),
            "long name": (lambda: volume.write_bytes(bytes(512)), "x" * 28, "1 to 27 bytes"),
            "colon": (lambda: volume.write_bytes(bytes(512)), "a:b", "':'"),
        }
        for label, (prepare, name, message) in cases.items():
            with self.subTest(label):
                prepare()
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    status = ndif_image.main([str(volume), str(output), name])
                self.assertEqual(status, 1)
                self.assertIn(message, stderr.getvalue())
                self.assertFalse(output.exists())
        with self.assertRaisesRegex(ValueError, "24-bit"):
            ndif_image.bcem("Enemy Territory", 1 << 24)

    def test_package_writes_the_ndif_image(self):
        script = (REPO / "scripts" / "package_mac.sh").read_text()
        self.assertIn('ndif_image.py "$RELEASE/ET_Install.img" "$RELEASE/ET_Install.img.bin" '
                      '"Enemy Territory"', script)
        self.assertNotIn('"iso " "dCpy"', script)
        self.assertNotIn(".dsk", script)


if __name__ == "__main__":
    unittest.main()
