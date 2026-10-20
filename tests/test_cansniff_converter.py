import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "tools" / "cansniff_convert.py"
SPEC = importlib.util.spec_from_file_location("cansniff_convert", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
converter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = converter
SPEC.loader.exec_module(converter)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def make_capture(path: Path, corrupt_payload: bool = False) -> None:
    header = bytearray(128)
    header[:4] = b"CSLG"
    struct.pack_into("<HHHHI", header, 4, 1, 0, 128, 1, 500_000)
    header[16] = 1
    header[17] = 1
    struct.pack_into("<III", header, 20, 7, 2, 64 * 1024)
    header[48:53] = b"3.0.0"
    struct.pack_into("<I", header, 124, crc32(header[:124]))

    standard = struct.pack("<IIB", 0, 0x181, 2) + bytes.fromhex("1122")
    extended_remote = struct.pack(
        "<IIB", 25, 0x1ABCDE | converter.FLAG_EXTENDED | converter.FLAG_REMOTE, 4
    )
    payload = bytearray(standard + extended_remote)
    payload_crc = crc32(payload)
    if corrupt_payload:
        payload[-1] ^= 0x01

    block = bytearray(32)
    block[:4] = b"BLK1"
    struct.pack_into("<IQIII", block, 4, 3, 1_000_000, 2, len(payload), payload_crc)
    struct.pack_into("<I", block, 28, crc32(block[:28]))

    footer = bytearray(64)
    footer[:4] = b"END1"
    struct.pack_into("<IIQQI", footer, 4, 0, 1, 2, 0, 0)
    struct.pack_into("<I", footer, 60, crc32(footer[:60]))
    path.write_bytes(header + block + payload + footer)


class ConverterTests(unittest.TestCase):
    def test_decodes_standard_and_extended_remote_frames(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "L000002.CSL"
            make_capture(path)
            header, frames, summary_box = converter.iter_file(path)
            decoded = list(frames)

            self.assertEqual(header.bitrate, 500_000)
            self.assertEqual(header.session_id, 7)
            self.assertEqual(len(decoded), 2)
            self.assertEqual(decoded[0].timestamp_us, 1_000_000)
            self.assertEqual(decoded[0].can_id, 0x181)
            self.assertEqual(decoded[0].data, bytes.fromhex("1122"))
            self.assertEqual(converter.canopen_decode(decoded[0]), (1, "TPDO1"))
            self.assertTrue(decoded[1].extended)
            self.assertTrue(decoded[1].remote)
            self.assertEqual(decoded[1].dlc, 4)
            self.assertEqual(decoded[1].data, b"")
            self.assertTrue(summary_box[0].clean_close)

    def test_rejects_payload_crc_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "BROKEN.CSL"
            make_capture(path, corrupt_payload=True)
            _header, frames, _summary_box = converter.iter_file(path)
            with self.assertRaises(converter.CSLFormatError):
                list(frames)


if __name__ == "__main__":
    unittest.main()
