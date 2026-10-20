import importlib.util
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


PROJECT_DIR = Path(__file__).parents[1]
sys.path.insert(0, str(PROJECT_DIR))

from log_analyzer.analysis import (  # noqa: E402
    analyze_session,
    describe_frame,
    filter_frames,
    scan_sessions,
)
from log_analyzer.app import CanSniffAnalyzer  # noqa: E402


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def make_session(root: Path, *, close_reason: int = 0,
                 clean_close: bool = True, include_footer: bool = True,
                 recovered: bool = False) -> Path:
    session = root / "LOGS" / "S000007"
    session.mkdir(parents=True)
    (session / "SESSION.JSON").write_text(json.dumps({
        "firmware": "3.0.0", "bitrate": 500000, "mode": "Full",
        "selected_nodes": [], "frames_stored": 3,
        "recovered": recovered, "clean_close": clean_close,
    }), encoding="utf-8")

    header = bytearray(128)
    header[:4] = b"CSLG"
    struct.pack_into("<HHHHI", header, 4, 1, 0, 128, 1, 500_000)
    header[16] = 1
    header[17] = 1
    struct.pack_into("<III", header, 20, 7, 1, 65536)
    header[48:53] = b"3.0.0"
    struct.pack_into("<I", header, 124, crc32(header[:124]))

    records = (
        struct.pack("<IIB", 0, 0x607, 8) + bytes.fromhex("4009600000000000")
        + struct.pack("<IIB", 200, 0x587, 8) + bytes.fromhex("43096000BFEC623F")
        + struct.pack("<IIB", 100_000, 0x707, 1) + bytes.fromhex("05")
    )
    block = bytearray(32)
    block[:4] = b"BLK1"
    struct.pack_into("<IQIII", block, 4, 0, 1_000_000, 3, len(records), crc32(records))
    struct.pack_into("<I", block, 28, crc32(block[:28]))

    footer = bytearray(64)
    footer[:4] = b"END1"
    struct.pack_into("<IIQQI", footer, 4, close_reason, 1, 3, 0, 0)
    struct.pack_into("<I", footer, 60, crc32(footer[:60]))
    content = header + block + records + (footer if include_footer else b"")
    (session / "L000001.CSL").write_bytes(content)
    return session


class AnalyzerTests(unittest.TestCase):
    def test_table_sort_values_are_typed(self) -> None:
        parse = CanSniffAnalyzer._parse_sort_value
        self.assertEqual(parse("id", "0x607"), 0x607)
        self.assertEqual(parse("session", "S000203"), 203)
        self.assertEqual(parse("size", "10,5 МБ"), 10.5 * 1024**2)
        self.assertEqual(parse("time", "1.250000"), 1.25)
        self.assertIsNone(parse("node", "—"))

    def test_scan_and_analyze(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            session_path = make_session(Path(directory))
            sessions = scan_sessions(Path(directory))
            self.assertEqual(len(sessions), 1)
            self.assertEqual(sessions[0].session_id, 7)
            self.assertEqual(sessions[0].state, "Закрыта")
            result = analyze_session(sessions[0])
            self.assertTrue(result.valid)
            self.assertEqual(len(result.frames), 3)
            self.assertEqual(len(result.id_statistics), 3)
            self.assertEqual(result.duration_us, 100_000)
            self.assertIn("SDO чтение 6009:00", describe_frame(result.frames[0]))
            self.assertIn("float32=", describe_frame(result.frames[1]))
            self.assertIn("Operational", describe_frame(result.frames[2]))
            self.assertEqual(session_path, sessions[0].path)

    def test_filters_by_node_and_description(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            make_session(Path(directory))
            result = analyze_session(scan_sessions(directory)[0])
            self.assertEqual(len(list(filter_frames(result.frames, node_id=7))), 3)
            self.assertEqual(len(list(filter_frames(result.frames, message_type="SDO_TX"))), 1)
            self.assertEqual(len(list(filter_frames(result.frames, query="6009"))), 2)

    def test_session_state_uses_csl_footer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            make_session(Path(directory), close_reason=2, clean_close=False)
            session = scan_sessions(directory)[0]
            self.assertEqual(session.state, "Восстановлена")

        with tempfile.TemporaryDirectory() as directory:
            make_session(Path(directory), clean_close=False)
            session = scan_sessions(directory)[0]
            self.assertEqual(session.state, "Закрыта")

        with tempfile.TemporaryDirectory() as directory:
            make_session(Path(directory), clean_close=False, include_footer=False)
            session = scan_sessions(directory)[0]
            self.assertEqual(session.state, "Не закрыта")


if __name__ == "__main__":
    unittest.main()

