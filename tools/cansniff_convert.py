#!/usr/bin/env python3
"""Convert and validate CanSniff .CSL Classic CAN capture files."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, TextIO

FILE_HEADER_SIZE = 128
BLOCK_HEADER_SIZE = 32
FOOTER_SIZE = 64
FORMAT_MAJOR = 1
ID_MASK = 0x1FFFFFFF
FLAG_EXTENDED = 1 << 29
FLAG_REMOTE = 1 << 30


class CSLFormatError(RuntimeError):
    """Raised when a capture is truncated or fails an integrity check."""


@dataclass(frozen=True)
class FileHeader:
    path: Path
    major: int
    minor: int
    bitrate: int
    mode: int
    session_id: int
    file_index: int
    node_filter: bytes
    firmware: str


@dataclass(frozen=True)
class Frame:
    source: Path
    session_id: int
    file_index: int
    block_sequence: int
    timestamp_us: int
    can_id: int
    extended: bool
    remote: bool
    dlc: int
    data: bytes


@dataclass(frozen=True)
class FileSummary:
    path: Path
    blocks: int
    frames: int
    clean_close: bool
    close_reason: int | None


def crc32(data: bytes, seed: int = 0) -> int:
    return zlib.crc32(data, seed) & 0xFFFFFFFF


def read_exact(stream: BinaryIO, size: int, description: str) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise CSLFormatError(f"truncated {description}: expected {size}, got {len(data)}")
    return data


def parse_header(path: Path, raw: bytes) -> FileHeader:
    if raw[:4] != b"CSLG":
        raise CSLFormatError("invalid file magic")
    major, minor, header_size, _flags = struct.unpack_from("<HHHH", raw, 4)
    if major != FORMAT_MAJOR:
        raise CSLFormatError(f"unsupported major version {major}")
    if header_size != FILE_HEADER_SIZE:
        raise CSLFormatError(f"unsupported header size {header_size}")
    expected_crc = struct.unpack_from("<I", raw, 124)[0]
    if crc32(raw[:124]) != expected_crc:
        raise CSLFormatError("file header CRC mismatch")
    bitrate = struct.unpack_from("<I", raw, 12)[0]
    mode = raw[16]
    timestamp_unit = raw[17]
    if timestamp_unit != 1:
        raise CSLFormatError(f"unsupported timestamp unit {timestamp_unit}")
    session_id, file_index = struct.unpack_from("<II", raw, 20)
    firmware = raw[48:64].split(b"\0", 1)[0].decode("ascii", errors="replace")
    return FileHeader(
        path=path,
        major=major,
        minor=minor,
        bitrate=bitrate,
        mode=mode,
        session_id=session_id,
        file_index=file_index,
        node_filter=raw[32:48],
        firmware=firmware,
    )


def iter_file(path: Path) -> tuple[FileHeader, Iterator[Frame], list[FileSummary]]:
    stream = path.open("rb")
    try:
        header = parse_header(path, read_exact(stream, FILE_HEADER_SIZE, "file header"))
    except Exception:
        stream.close()
        raise
    summary_box: list[FileSummary] = []

    def frames() -> Iterator[Frame]:
        block_count = 0
        frame_count = 0
        previous_sequence: int | None = None
        clean_close = False
        close_reason: int | None = None
        try:
            while True:
                magic = stream.read(4)
                if magic == b"":
                    break
                if len(magic) != 4:
                    raise CSLFormatError("truncated record magic")
                if magic == b"\0\0\0\0":
                    # Expected for a preallocated .TMP after the last committed block.
                    break
                if magic == b"END1":
                    footer = magic + read_exact(stream, FOOTER_SIZE - 4, "footer")
                    if crc32(footer[:60]) != struct.unpack_from("<I", footer, 60)[0]:
                        raise CSLFormatError("footer CRC mismatch")
                    close_reason = struct.unpack_from("<I", footer, 4)[0]
                    clean_close = True
                    break
                if magic != b"BLK1":
                    raise CSLFormatError(f"unknown record magic {magic!r}")

                block_header = magic + read_exact(
                    stream, BLOCK_HEADER_SIZE - 4, "block header"
                )
                if crc32(block_header[:28]) != struct.unpack_from("<I", block_header, 28)[0]:
                    raise CSLFormatError("block header CRC mismatch")
                sequence = struct.unpack_from("<I", block_header, 4)[0]
                first_timestamp, frames_in_block, payload_size, payload_crc = struct.unpack_from(
                    "<QIII", block_header, 8
                )
                if payload_size == 0 or payload_size > 64 * 1024:
                    raise CSLFormatError(f"invalid block payload size {payload_size}")
                if previous_sequence is not None and sequence != ((previous_sequence + 1) & 0xFFFFFFFF):
                    raise CSLFormatError(
                        f"block sequence discontinuity: {previous_sequence} -> {sequence}"
                    )
                previous_sequence = sequence
                payload = read_exact(stream, payload_size, "block payload")
                if crc32(payload) != payload_crc:
                    raise CSLFormatError(f"payload CRC mismatch in block {sequence}")

                offset = 0
                decoded_in_block = 0
                while offset < len(payload):
                    if len(payload) - offset < 9:
                        raise CSLFormatError(f"truncated frame in block {sequence}")
                    delta_us, id_flags = struct.unpack_from("<II", payload, offset)
                    dlc = payload[offset + 8] & 0x0F
                    if dlc > 8:
                        raise CSLFormatError(f"invalid DLC {dlc} in block {sequence}")
                    remote = bool(id_flags & FLAG_REMOTE)
                    data_size = 0 if remote else dlc
                    record_size = 9 + data_size
                    if offset + record_size > len(payload):
                        raise CSLFormatError(f"truncated CAN data in block {sequence}")
                    frame = Frame(
                        source=path,
                        session_id=header.session_id,
                        file_index=header.file_index,
                        block_sequence=sequence,
                        timestamp_us=first_timestamp + delta_us,
                        can_id=id_flags & ID_MASK,
                        extended=bool(id_flags & FLAG_EXTENDED),
                        remote=remote,
                        dlc=dlc,
                        data=payload[offset + 9 : offset + record_size],
                    )
                    yield frame
                    offset += record_size
                    decoded_in_block += 1
                    frame_count += 1
                if decoded_in_block != frames_in_block:
                    raise CSLFormatError(
                        f"block {sequence} declares {frames_in_block} frames, decoded {decoded_in_block}"
                    )
                block_count += 1
        finally:
            stream.close()
            summary_box.append(
                FileSummary(path, block_count, frame_count, clean_close, close_reason)
            )

    return header, frames(), summary_box


def canopen_decode(frame: Frame) -> tuple[int | None, str]:
    if frame.extended:
        return None, ""
    can_id = frame.can_id
    if can_id == 0:
        if len(frame.data) >= 2 and 1 <= frame.data[1] <= 127:
            return frame.data[1], "NMT"
        return None, "NMT"
    globals_by_id = {0x080: "SYNC", 0x100: "TIME", 0x7E4: "LSS_TX", 0x7E5: "LSS_RX"}
    if can_id in globals_by_id:
        return None, globals_by_id[can_id]
    ranges = (
        (0x080, "EMCY"),
        (0x180, "TPDO1"),
        (0x200, "RPDO1"),
        (0x280, "TPDO2"),
        (0x300, "RPDO2"),
        (0x380, "TPDO3"),
        (0x400, "RPDO3"),
        (0x480, "TPDO4"),
        (0x500, "RPDO4"),
        (0x580, "SDO_TX"),
        (0x600, "SDO_RX"),
        (0x700, "HEARTBEAT_GUARD"),
    )
    for base, message_type in ranges:
        node_id = can_id - base
        if 1 <= node_id <= 127:
            return node_id, message_type
    return None, ""


def collect_inputs(values: Iterable[str]) -> list[Path]:
    paths: list[Path] = []
    for value in values:
        path = Path(value)
        if path.is_dir():
            paths.extend(sorted(path.rglob("*.CSL")))
        elif path.is_file():
            paths.append(path)
        else:
            raise FileNotFoundError(value)
    if not paths:
        raise FileNotFoundError("no .CSL files found")
    return paths


CSV_FIELDS = (
    "session_id",
    "file_index",
    "block_sequence",
    "timestamp_us",
    "timestamp_s",
    "can_id",
    "id_format",
    "frame_type",
    "dlc",
    "data_hex",
    "node_id",
    "canopen_type",
)


def frame_mapping(frame: Frame) -> dict[str, object]:
    node_id, message_type = canopen_decode(frame)
    width = 8 if frame.extended else 3
    return {
        "session_id": frame.session_id,
        "file_index": frame.file_index,
        "block_sequence": frame.block_sequence,
        "timestamp_us": frame.timestamp_us,
        "timestamp_s": f"{frame.timestamp_us / 1_000_000:.6f}",
        "can_id": f"0x{frame.can_id:0{width}X}",
        "id_format": "EXT" if frame.extended else "STD",
        "frame_type": "REMOTE" if frame.remote else "DATA",
        "dlc": frame.dlc,
        "data_hex": frame.data.hex().upper(),
        "node_id": "" if node_id is None else node_id,
        "canopen_type": message_type,
    }


def write_csv(frames: Iterable[Frame], output: TextIO) -> int:
    writer = csv.DictWriter(output, fieldnames=CSV_FIELDS, lineterminator="\n")
    writer.writeheader()
    count = 0
    for frame in frames:
        writer.writerow(frame_mapping(frame))
        count += 1
    return count


def write_jsonl(frames: Iterable[Frame], output: TextIO) -> int:
    count = 0
    for frame in frames:
        output.write(json.dumps(frame_mapping(frame), ensure_ascii=False) + "\n")
        count += 1
    return count


def write_candump(frames: Iterable[Frame], output: TextIO) -> int:
    count = 0
    for frame in frames:
        width = 8 if frame.extended else 3
        payload = f"R{frame.dlc}" if frame.remote else frame.data.hex().upper()
        output.write(
            f"({frame.timestamp_us / 1_000_000:.6f}) can0 "
            f"{frame.can_id:0{width}X}#{payload}\n"
        )
        count += 1
    return count


def convert(paths: list[Path], output: TextIO, output_format: str) -> tuple[int, list[FileSummary]]:
    summaries: list[FileSummary] = []

    def all_frames() -> Iterator[Frame]:
        for path in paths:
            _header, frames, summary_box = iter_file(path)
            yield from frames
            summaries.extend(summary_box)

    writers = {"csv": write_csv, "jsonl": write_jsonl, "candump": write_candump}
    return writers[output_format](all_frames(), output), summaries


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help=".CSL file(s) or session directories")
    parser.add_argument("--format", choices=("csv", "jsonl", "candump"), default="csv")
    parser.add_argument("-o", "--output", help="output file; stdout if omitted")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    try:
        paths = collect_inputs(args.inputs)
        output: TextIO
        if args.output:
            output = Path(args.output).open("w", encoding="utf-8", newline="")
        else:
            output = sys.stdout
        try:
            count, summaries = convert(paths, output, args.format)
        finally:
            if args.output:
                output.close()
        incomplete = sum(not item.clean_close for item in summaries)
        print(
            f"Converted {count} frames from {len(summaries)} file(s); "
            f"incomplete files: {incomplete}",
            file=sys.stderr,
        )
        return 0
    except (OSError, CSLFormatError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
