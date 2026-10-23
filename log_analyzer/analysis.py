"""High-level, dependency-free analysis of CanSniff sessions."""

from __future__ import annotations

import json
import math
import statistics
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator, Sequence

from tools.cansniff_convert import (
    FILE_HEADER_SIZE,
    FOOTER_SIZE,
    CSLFormatError,
    FileHeader,
    FileSummary,
    Frame,
    canopen_decode,
    crc32,
    iter_file,
    parse_header,
    read_exact,
)


CLOSE_REASONS = {
    0: "Остановлено штатно",
    1: "Ротация файла",
    2: "Восстановлено после сбоя",
    3: "Изменена конфигурация",
}

HEARTBEAT_STATES = {
    0: "Boot-up",
    4: "Stopped",
    5: "Operational",
    127: "Pre-operational",
}

NMT_COMMANDS = {
    1: "Start remote node",
    2: "Stop remote node",
    128: "Enter pre-operational",
    129: "Reset node",
    130: "Reset communication",
}

SDO_ABORT_CODES = {
    0x05030000: "Toggle bit not alternated",
    0x05040000: "SDO protocol timed out",
    0x05040001: "Invalid command specifier",
    0x06010000: "Unsupported access",
    0x06010001: "Attempt to read a write-only object",
    0x06010002: "Attempt to write a read-only object",
    0x06020000: "Object does not exist",
    0x06040041: "Object cannot be mapped to PDO",
    0x06070010: "Data type/length mismatch",
    0x06090011: "Sub-index does not exist",
    0x06090030: "Value range exceeded",
    0x08000000: "General error",
    0x08000020: "Data cannot be transferred/stored",
}


@dataclass(frozen=True)
class SessionInfo:
    path: Path
    name: str
    session_id: int | None
    bitrate: int
    mode: str
    selected_nodes: tuple[int, ...]
    firmware: str
    file_count: int
    total_bytes: int
    metadata_frames: int | None
    clean_close: bool | None
    recovered: bool
    state: str
    bad_files: int
    metadata_error: str = ""


@dataclass(frozen=True)
class IdStatistics:
    can_id: int
    extended: bool
    frame_type: str
    node_id: int | None
    canopen_type: str
    count: int
    percent: float
    first_us: int
    last_us: int
    rate_hz: float
    average_period_ms: float | None
    min_period_ms: float | None
    max_period_ms: float | None
    dlcs: str
    changes: int
    last_data: str


@dataclass
class AnalysisResult:
    session: SessionInfo
    headers: list[FileHeader] = field(default_factory=list)
    file_summaries: list[FileSummary] = field(default_factory=list)
    frames: list[Frame] = field(default_factory=list)
    id_statistics: list[IdStatistics] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    duration_us: int = 0
    estimated_bus_load_percent: float | None = None

    @property
    def valid(self) -> bool:
        return not self.errors


def normalize_logs_root(path: str | Path) -> Path:
    """Accept a card root, CANSNIFF directory, LOGS directory, or a session."""
    candidate = Path(path).expanduser()
    if not candidate.exists():
        raise FileNotFoundError(f"Путь не найден: {candidate}")
    if candidate.is_file():
        return candidate.parent
    direct_logs = candidate / "LOGS"
    nested_logs = candidate / "CANSNIFF" / "LOGS"
    if direct_logs.is_dir():
        return direct_logs
    if nested_logs.is_dir():
        return nested_logs
    return candidate


def _read_metadata(session_path: Path) -> tuple[dict, str]:
    metadata_path = session_path / "SESSION.JSON"
    if not metadata_path.exists():
        return {}, "SESSION.JSON отсутствует"
    try:
        return json.loads(metadata_path.read_text(encoding="utf-8-sig")), ""
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return {}, f"Ошибка SESSION.JSON: {error}"


def _session_number(name: str) -> int | None:
    if len(name) == 7 and name[:1].upper() == "S" and name[1:].isdigit():
        return int(name[1:])
    return None


def _session_state(csl_files: Sequence[Path], bad_files: Sequence[Path],
                   metadata_recovered: bool) -> str:
    if bad_files:
        return "BAD"
    if not csl_files:
        return "Не закрыта"

    reasons: list[int] = []
    for path in csl_files:
        try:
            if path.stat().st_size < FOOTER_SIZE:
                return "Не закрыта"
            with path.open("rb") as stream:
                stream.seek(-FOOTER_SIZE, 2)
                footer = read_exact(stream, FOOTER_SIZE, "footer")
            if footer[:4] != b"END1" or crc32(footer[:60]) != struct.unpack_from("<I", footer, 60)[0]:
                return "Не закрыта"
            reasons.append(struct.unpack_from("<I", footer, 4)[0])
        except (OSError, CSLFormatError, ValueError):
            return "Не закрыта"
    if metadata_recovered or 2 in reasons:
        return "Восстановлена"
    return "Закрыта"


def scan_sessions(path: str | Path) -> list[SessionInfo]:
    source = Path(path).expanduser()
    if source.is_file():
        if source.suffix.upper() != ".CSL":
            raise ValueError(f"Ожидается файл .CSL: {source}")
        try:
            with source.open("rb") as stream:
                header = parse_header(
                    source, read_exact(stream, FILE_HEADER_SIZE, "file header")
                )
        except (OSError, CSLFormatError) as error:
            raise CSLFormatError(f"{source.name}: {error}") from error
        selected_nodes = tuple(
            node
            for node in range(1, 128)
            if header.node_filter[node // 8] & (1 << (node % 8))
        )
        state = _session_state([source], [], False)
        return [
            SessionInfo(
                path=source,
                name=f"S{header.session_id:06d}",
                session_id=header.session_id,
                bitrate=header.bitrate,
                mode="Full" if header.mode == 1 else "Selected" if header.mode == 2 else str(header.mode),
                selected_nodes=selected_nodes,
                firmware=header.firmware,
                file_count=1,
                total_bytes=source.stat().st_size,
                metadata_frames=None,
                clean_close=state in ("Закрыта", "Восстановлена"),
                recovered=state == "Восстановлена",
                state=state,
                bad_files=0,
                metadata_error="SESSION.JSON отсутствует: открыт отдельный файл, скачанный через web",
            )
        ]

    root = normalize_logs_root(path)
    if root.name.upper().startswith("S") and any(root.glob("*.CSL")):
        directories = [root]
    else:
        directories = [item for item in root.iterdir() if item.is_dir()]

    sessions: list[SessionInfo] = []
    for directory in directories:
        csl_files = sorted(directory.glob("*.CSL"))
        bad_files = list(directory.glob("*.BAD"))
        if not csl_files and not bad_files and not (directory / "SESSION.JSON").exists():
            continue
        metadata, metadata_error = _read_metadata(directory)
        session_id = _session_number(directory.name)
        bitrate = int(metadata.get("bitrate") or 0)
        firmware = str(metadata.get("firmware") or "")
        mode = str(metadata.get("mode") or "Неизвестно")
        selected_nodes = tuple(
            int(value) for value in metadata.get("selected_nodes", []) if str(value).isdigit()
        )
        clean_close = metadata.get("clean_close")
        metadata_frames = metadata.get("frames_stored")
        recovered = metadata.get("recovered") is True
        state = _session_state(csl_files, bad_files, recovered)

        # Header values are authoritative when old/incomplete metadata contains zeros.
        if csl_files:
            try:
                with csl_files[0].open("rb") as stream:
                    header = parse_header(
                        csl_files[0], read_exact(stream, FILE_HEADER_SIZE, "file header")
                    )
                session_id = header.session_id
                bitrate = header.bitrate
                firmware = header.firmware
                if mode == "Неизвестно":
                    mode = "Full" if header.mode == 1 else "Selected" if header.mode == 2 else str(header.mode)
            except (OSError, CSLFormatError) as error:
                metadata_error = "; ".join(filter(None, (metadata_error, str(error))))

        sessions.append(
            SessionInfo(
                path=directory,
                name=directory.name,
                session_id=session_id,
                bitrate=bitrate,
                mode=mode,
                selected_nodes=selected_nodes,
                firmware=firmware,
                file_count=len(csl_files),
                total_bytes=sum(item.stat().st_size for item in csl_files),
                metadata_frames=int(metadata_frames) if isinstance(metadata_frames, int) else None,
                clean_close=clean_close if isinstance(clean_close, bool) else None,
                recovered=recovered,
                state=state,
                bad_files=len(bad_files),
                metadata_error=metadata_error,
            )
        )
    return sorted(sessions, key=lambda item: (item.session_id is None, item.session_id or 0))


def session_files(session: SessionInfo) -> list[Path]:
    if session.path.is_file():
        return [session.path]
    return sorted(session.path.glob("*.CSL"), key=lambda item: item.name.upper())


def _estimate_frame_bits(frame: Frame) -> int:
    """Conservative Classic CAN frame size including average bit stuffing."""
    base_bits = 67 if frame.extended else 47
    if not frame.remote:
        base_bits += frame.dlc * 8
    return math.ceil(base_bits * 1.20) + 3


def analyze_session(session: SessionInfo) -> AnalysisResult:
    result = AnalysisResult(session=session)
    previous_global_sequence: int | None = None

    for path in session_files(session):
        try:
            header, iterator, summary_box = iter_file(path)
            result.headers.append(header)
            try:
                file_frames = list(iterator)
            except (OSError, CSLFormatError, ValueError) as error:
                result.errors.append(f"{path.name}: {error}")
                continue
            result.frames.extend(file_frames)
            result.file_summaries.extend(summary_box)
            sequences = list(dict.fromkeys(frame.block_sequence for frame in file_frames))
            if previous_global_sequence is not None and sequences:
                expected = (previous_global_sequence + 1) & 0xFFFFFFFF
                if sequences[0] != expected:
                    result.warnings.append(
                        f"{path.name}: разрыв последовательности блоков {previous_global_sequence} → {sequences[0]}"
                    )
            if sequences:
                previous_global_sequence = sequences[-1]
        except (OSError, CSLFormatError, ValueError) as error:
            result.errors.append(f"{path.name}: {error}")

    if not result.frames:
        if not result.errors:
            result.warnings.append("В сессии нет CAN-кадров")
        return result

    result.frames.sort(key=lambda frame: (frame.timestamp_us, frame.file_index, frame.block_sequence))
    result.duration_us = max(0, result.frames[-1].timestamp_us - result.frames[0].timestamp_us)
    result.id_statistics = build_id_statistics(result.frames)

    bitrate = result.headers[0].bitrate if result.headers else session.bitrate
    if result.duration_us > 0 and bitrate > 0:
        bits = sum(_estimate_frame_bits(frame) for frame in result.frames)
        result.estimated_bus_load_percent = bits / (bitrate * (result.duration_us / 1_000_000)) * 100

    for summary in result.file_summaries:
        if not summary.clean_close:
            result.warnings.append(f"{summary.path.name}: нет штатного завершающего блока")
    if session.bad_files:
        result.warnings.append(f"Повреждённых/не начатых файлов .BAD: {session.bad_files}")
    if session.metadata_frames not in (None, 0, len(result.frames)):
        result.warnings.append(
            f"SESSION.JSON сообщает {session.metadata_frames} кадров, фактически прочитано {len(result.frames)}"
        )
    return result


def build_id_statistics(frames: Sequence[Frame]) -> list[IdStatistics]:
    groups: dict[tuple[int, bool, bool], list[Frame]] = defaultdict(list)
    for frame in frames:
        groups[(frame.can_id, frame.extended, frame.remote)].append(frame)
    total = len(frames)
    rows: list[IdStatistics] = []
    for (can_id, extended, remote), items in groups.items():
        node_id, message_type = canopen_decode(items[0])
        timestamps = [item.timestamp_us for item in items]
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:]) if b >= a]
        elapsed = timestamps[-1] - timestamps[0]
        rate = ((len(items) - 1) * 1_000_000 / elapsed) if elapsed > 0 else 0.0
        changes = sum(a.data != b.data for a, b in zip(items, items[1:]))
        rows.append(
            IdStatistics(
                can_id=can_id,
                extended=extended,
                frame_type="RTR" if remote else "DATA",
                node_id=node_id,
                canopen_type=message_type,
                count=len(items),
                percent=len(items) * 100 / total if total else 0.0,
                first_us=timestamps[0],
                last_us=timestamps[-1],
                rate_hz=rate,
                average_period_ms=(statistics.fmean(intervals) / 1000) if intervals else None,
                min_period_ms=(min(intervals) / 1000) if intervals else None,
                max_period_ms=(max(intervals) / 1000) if intervals else None,
                dlcs=", ".join(str(value) for value in sorted({item.dlc for item in items})),
                changes=changes,
                last_data=items[-1].data.hex(" ").upper(),
            )
        )
    return sorted(rows, key=lambda item: (-item.count, item.can_id))


def describe_frame(frame: Frame) -> str:
    """Return a useful CANopen interpretation while preserving raw data."""
    node_id, message_type = canopen_decode(frame)
    data = frame.data
    if frame.remote:
        return f"Remote request, DLC={frame.dlc}"
    if message_type == "NMT":
        if len(data) >= 2:
            command = NMT_COMMANDS.get(data[0], f"command 0x{data[0]:02X}")
            target = "все узлы" if data[1] == 0 else f"узел {data[1]}"
            return f"{command}; {target}"
        return "NMT"
    if message_type == "HEARTBEAT_GUARD" and data:
        state = data[0] & 0x7F
        toggle = " toggle" if data[0] & 0x80 else ""
        return f"Узел {node_id}: {HEARTBEAT_STATES.get(state, f'состояние 0x{state:02X}')}{toggle}"
    if message_type == "EMCY" and len(data) >= 3:
        error_code = int.from_bytes(data[:2], "little")
        return f"Узел {node_id}: EMCY 0x{error_code:04X}, регистр 0x{data[2]:02X}"
    if message_type in ("SDO_RX", "SDO_TX") and len(data) >= 4:
        command = data[0]
        index = int.from_bytes(data[1:3], "little")
        subindex = data[3]
        direction = "запрос" if message_type == "SDO_RX" else "ответ"
        if command == 0x80 and len(data) >= 8:
            code = int.from_bytes(data[4:8], "little")
            text = SDO_ABORT_CODES.get(code, "неизвестная причина")
            return f"SDO abort {index:04X}:{subindex:02X}, 0x{code:08X} ({text})"
        if command == 0x40:
            return f"SDO чтение {index:04X}:{subindex:02X} ({direction})"
        if command in (0x43, 0x4B, 0x4F) and len(data) >= 8:
            size = {0x43: 4, 0x4B: 2, 0x4F: 1}[command]
            raw_value = data[4 : 4 + size]
            value = int.from_bytes(raw_value, "little")
            if size == 4:
                signed = int.from_bytes(raw_value, "little", signed=True)
                float_value = struct.unpack("<f", raw_value)[0]
                float_text = f", float32={float_value:.7g}" if math.isfinite(float_value) else ""
                return (
                    f"SDO {index:04X}:{subindex:02X}: raw=0x{value:08X}, "
                    f"uint={value}, int={signed}{float_text}"
                )
            return f"SDO {index:04X}:{subindex:02X} = {value} (0x{value:0{size * 2}X})"
        if command in (0x23, 0x2B, 0x2F) and len(data) >= 8:
            size = {0x23: 4, 0x2B: 2, 0x2F: 1}[command]
            raw_value = data[4 : 4 + size]
            value = int.from_bytes(raw_value, "little")
            if size == 4:
                signed = int.from_bytes(raw_value, "little", signed=True)
                float_value = struct.unpack("<f", raw_value)[0]
                float_text = f", float32={float_value:.7g}" if math.isfinite(float_value) else ""
                return (
                    f"SDO запись {index:04X}:{subindex:02X}: raw=0x{value:08X}, "
                    f"uint={value}, int={signed}{float_text}"
                )
            return f"SDO запись {index:04X}:{subindex:02X} = {value} (0x{value:0{size * 2}X})"
        if command == 0x60:
            return f"SDO подтверждение записи {index:04X}:{subindex:02X}"
        return f"SDO {direction} {index:04X}:{subindex:02X}, cmd 0x{command:02X}"
    if message_type:
        suffix = f", узел {node_id}" if node_id is not None else ""
        return f"{message_type}{suffix}"
    return ""


def frame_id_text(frame: Frame) -> str:
    return f"0x{frame.can_id:08X}" if frame.extended else f"0x{frame.can_id:03X}"


def filter_frames(
    frames: Iterable[Frame],
    query: str = "",
    message_type: str = "Все",
    node_id: int | None = None,
    data_changed_only: bool = False,
) -> Iterator[Frame]:
    normalized = query.strip().upper().replace("0X", "")
    previous_data: dict[tuple[int, bool], bytes] = {}
    for frame in frames:
        decoded_node, decoded_type = canopen_decode(frame)
        changed = previous_data.get((frame.can_id, frame.extended)) != frame.data
        previous_data[(frame.can_id, frame.extended)] = frame.data
        if normalized:
            haystack = " ".join(
                (
                    f"{frame.can_id:X}",
                    frame.data.hex().upper(),
                    decoded_type.upper(),
                    describe_frame(frame).upper(),
                )
            )
            if normalized not in haystack:
                continue
        if message_type != "Все" and decoded_type != message_type:
            continue
        if node_id is not None and decoded_node != node_id:
            continue
        if data_changed_only and not changed:
            continue
        yield frame


def available_message_types(frames: Iterable[Frame]) -> list[str]:
    return sorted({canopen_decode(frame)[1] for frame in frames if canopen_decode(frame)[1]})


def available_nodes(frames: Iterable[Frame]) -> list[int]:
    return sorted({node for frame in frames if (node := canopen_decode(frame)[0]) is not None})


def integrity_text(result: AnalysisResult) -> str:
    if result.errors:
        return f"ОШИБКА ({len(result.errors)})"
    if result.warnings:
        return f"ПРЕДУПРЕЖДЕНИЕ ({len(result.warnings)})"
    return "CRC OK"


def close_reason_text(summary: FileSummary) -> str:
    if not summary.clean_close:
        return "Нет footer"
    return CLOSE_REASONS.get(summary.close_reason, f"Код {summary.close_reason}")

