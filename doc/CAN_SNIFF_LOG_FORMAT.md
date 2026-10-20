# CanSniff Log format (`.CSL`) version 1.0

## Purpose and byte order

`.CSL` is the canonical lossless Classic CAN capture produced by the device.
All multi-byte integers are unsigned little-endian. Timestamps are expressed in
microseconds relative to the beginning of a logging configuration. A file is a
128-byte header followed by CRC-protected blocks and, after a clean close or
recovery, a 64-byte footer.

The device writes an active file with the `.TMP` extension. It truncates the
unused preallocated tail, appends a footer, calls `f_sync`, closes the file and
renames it to `.CSL`. Files rotate before 32 MiB and never split a block.

## File header — 128 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `CSLG` |
| 4 | 2 | format major version, currently `1` |
| 6 | 2 | format minor version, currently `0` |
| 8 | 2 | header size, `128` |
| 10 | 2 | format flags; bit 0 means little-endian |
| 12 | 4 | CAN bitrate in bit/s, or zero while auto detection is unresolved |
| 16 | 1 | logging mode: `1` Full, `2` Selected nodes |
| 17 | 1 | timestamp unit: `1` microsecond |
| 18 | 2 | reserved, zero |
| 20 | 4 | session number |
| 24 | 4 | file index within the session |
| 28 | 4 | maximum uncompressed block payload, currently 65536 |
| 32 | 16 | selected-node bitmap; bit `n` represents CANopen node `n` |
| 48 | 16 | zero-terminated ASCII firmware version |
| 64 | 60 | reserved, zero |
| 124 | 4 | IEEE CRC-32 of bytes 0…123 |

## Block header — 32 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `BLK1` |
| 4 | 4 | monotonically increasing block sequence number |
| 8 | 8 | timestamp of the first frame in the block, microseconds |
| 16 | 4 | number of frame records |
| 20 | 4 | payload size in bytes |
| 24 | 4 | IEEE CRC-32 of the complete payload |
| 28 | 4 | IEEE CRC-32 of block-header bytes 0…27 |

Sequence numbers continue across rotated files in one session. A reader must
verify both CRC values before accepting any records from a block.

## Frame record — 9 to 17 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | timestamp delta from the block timestamp, microseconds |
| 4 | 4 | CAN identifier and flags |
| 8 | 1 | DLC in bits 0…3 |
| 9 | 0…8 | payload bytes; absent for Remote frames |

Identifier/flag word:

- bits 0…28: CAN identifier;
- bit 29: Extended identifier;
- bit 30: Remote frame;
- bit 31: reserved and zero.

For a Data frame, the record contains exactly `DLC` data bytes. For a Remote
frame it contains no data bytes, while the DLC retains the requested length.

## Footer — 64 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `END1` |
| 4 | 4 | close reason: `0` stopped, `1` rotated, `2` recovered, `3` configuration changed |
| 8 | 4 | blocks in this file |
| 12 | 4 | reserved |
| 16 | 8 | frames in this file |
| 24 | 8 | logger-dropped frames observed during the session |
| 32 | 4 | write errors observed during the session |
| 36 | 24 | reserved, zero |
| 60 | 4 | IEEE CRC-32 of footer bytes 0…59 |

## Recovery

On mount, the storage task examines `.TMP` files in the most recent session.
It accepts the header and every consecutive block whose header and payload CRC
are valid, discards the preallocated or torn tail, adds a footer with close
reason `2`, truncates and renames the file to `.CSL`. A file with an invalid
header is retained with the `.BAD` extension and is never silently deleted.
If the destination name already exists, recovery selects a free `_R01`…`_R99`
suffix instead of replacing any existing capture.

Periodic `f_sync` bounds the amount of data that can remain only in filesystem
or card caches. No filesystem can guarantee survival if power is removed while
the card itself is updating internal flash translation metadata.

## Human-readable conversion

The dependency-free converter validates all sequence numbers and CRC values:

```text
python tools/cansniff_convert.py LOGS/S000001 --format csv -o capture.csv
python tools/cansniff_convert.py LOGS/S000001 --format jsonl -o capture.jsonl
python tools/cansniff_convert.py LOGS/S000001 --format candump -o capture.log
```

CSV and JSONL include the raw frame fields plus derived CANopen `node_id` and
message type. Derived fields may be empty and never replace the original CAN ID,
flags, DLC or payload.
