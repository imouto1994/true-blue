#!/usr/bin/env python3
"""
analyze_doj.py - Low-level binary analysis tool for Lilim .doj script files.

PURPOSE
-------
This tool is intended for developers who want to inspect the raw decoded .doj
binary format — for example when extending decode_doj.py to handle new opcodes,
verifying parsing logic, or reverse-engineering fields that are not yet understood.

It is NOT required for normal text extraction (use decode_doj.py for that).

WHAT IT DOES
------------
- Counts all opcode occurrences across one or more .doj files
- Reports where the "has text" marker (0x01 0x00) sits within each record type
- Shows raw hex dumps alongside decoded text for any number of sample records
- Detects records that carry text to confirm marker/text offsets are correct

See DOJ_FORMAT.md for the full binary format specification.

USAGE
-----
    python analyze_doj.py                        - default: analyze three sample files
    (edit SAMPLE_FILES at the bottom to change targets)
"""

import sys
import struct

sys.stdout.reconfigure(encoding='utf-8')


# ── Shift-JIS string reader (same logic as decode_doj.py) ────────────────────

def decode_sjis(data: bytes, start: int) -> tuple[str, int]:
    """
    Read a null-terminated Shift-JIS string at `start`.
    Returns (decoded_unicode_str, offset_just_past_null).

    Shift-JIS is a variable-width encoding; lead bytes in 0x81–0x9F and
    0xE0–0xFC are followed by a second byte, so we must check each byte before
    deciding how far to advance to avoid false null detection mid-character.
    """
    end = start
    while end < len(data) and data[end] != 0:
        b = data[end]
        if (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC):
            end += 2   # 2-byte character
        else:
            end += 1
    return data[start:end].decode('shift-jis', errors='replace'), end + 1


# ── Marker-hunting helper ─────────────────────────────────────────────────────

def find_marker_offset(data: bytes, pos: int, search_range: range = range(14, 26)) -> int | None:
    """
    Given a record starting at `pos`, scan bytes in `search_range` (relative
    offsets) looking for the 0x01 0x00 "has text" marker followed by something
    that looks like the start of a Shift-JIS or ASCII string.

    Returns the relative offset of the 0x01 byte, or None if not found.

    This is a discovery helper — it is not used by the production decoder
    (which uses hard-coded, confirmed offsets).
    """
    for offset in search_range:
        if pos + offset + 2 >= len(data):
            break
        if data[pos + offset] == 0x01 and data[pos + offset + 1] == 0x00:
            # Sanity-check: the byte after 0x01 0x00 should look like text
            first_text_byte = data[pos + offset + 2]
            is_sjis_lead   = (0x81 <= first_text_byte <= 0x9F) or \
                              (0xE0 <= first_text_byte <= 0xFC)
            is_ascii_print = 0x20 <= first_text_byte <= 0x7E
            if is_sjis_lead or is_ascii_print:
                return offset
    return None


# ── Analysis functions ────────────────────────────────────────────────────────

def count_opcodes(data: bytes) -> dict[tuple[int, int], int]:
    """
    Scan `data` and count every adjacent (byte, next_byte) pair that matches
    opcode+sub format (i.e. any byte followed by 0x00).  Returns a dict
    mapping (opcode, 0x00) → occurrence count, sorted by frequency.

    This gives a quick picture of which opcodes dominate the file and how many
    text-bearing records there are (0x0A, 0x2A, 0x22).
    """
    counts: dict[tuple[int, int], int] = {}
    for i in range(len(data) - 1):
        if data[i + 1] == 0x00:   # only count opcode-like pairs
            key = (data[i], 0x00)
            counts[key] = counts.get(key, 0) + 1
    return dict(sorted(counts.items(), key=lambda x: -x[1]))


def analyze_file(filepath: str, max_samples: int = 5) -> None:
    """
    Full analysis of a single decoded .doj file.  Prints:
      1. File metadata (path, size)
      2. Top opcode frequencies
      3. For each text-bearing opcode: confirmed marker offset + sample records
    """
    with open(filepath, 'rb') as f:
        data = f.read()
    n = len(data)

    print(f'{"=" * 60}')
    print(f'FILE: {filepath}  ({n:,} bytes)')
    print(f'{"=" * 60}')

    # ── 1. Opcode frequency table ─────────────────────────────────────────────
    print('\n[Opcode frequency (byte, 0x00) pairs, top 15]')
    all_counts = count_opcodes(data)
    for (op, sub), cnt in list(all_counts.items())[:15]:
        label = {
            0x0A: 'narration/monologue',
            0x2A: 'dialogue or choices',
            0x22: 'dialogue (alt)',
            0x11: 'flow control',
            0x14: 'resource load',
            0x33: 'scene command',
        }.get(op, '')
        print(f'  0x{op:02X} 0x{sub:02X}: {cnt:5d}  {label}')

    # ── 2. Per-opcode text analysis ───────────────────────────────────────────
    for target_op in (0x0A, 0x2A, 0x22):
        records_with_text = 0
        marker_tally: dict[int, int] = {}  # relative offset → how many times seen
        sample_records: list[tuple[int, int, str]] = []  # (file_offset, marker_off, text)

        i = 0
        while i < n - 25:
            if data[i] == target_op and data[i + 1] == 0x00:
                mo = find_marker_offset(data, i)
                if mo is not None:
                    records_with_text += 1
                    marker_tally[mo] = marker_tally.get(mo, 0) + 1
                    if len(sample_records) < max_samples:
                        txt, _ = decode_sjis(data, i + mo + 2)
                        sample_records.append((i, mo, txt.strip()))
            i += 1

        if records_with_text == 0:
            # Opcode not present (or no text records found)
            continue

        print(f'\n[Opcode 0x{target_op:02X}: {records_with_text} records with text]')

        # Marker offset distribution — should be a single dominant value
        print(f'  "has text" marker offset distribution:')
        for off, cnt in sorted(marker_tally.items()):
            confidence = '✓ confirmed' if cnt == records_with_text else f'({cnt}/{records_with_text})'
            print(f'    +{off:2d}: {cnt:5d} records  {confidence}')

        # Sample records with hex dump up to the marker, then decoded text
        print(f'  Sample records (first {len(sample_records)}):')
        for file_off, mo, txt in sample_records:
            # Show bytes from record start up to the end of the marker
            hex_part = ' '.join(f'{data[file_off + j]:02X}' for j in range(mo + 4))
            print(f'    0x{file_off:05X}: [{hex_part}]')
            print(f'           text : {txt[:70]!r}')

    print()


# ── Entry point ───────────────────────────────────────────────────────────────

# Edit this list to analyze different files.
# Paths are relative to the directory where you run the script.
SAMPLE_FILES = [
    r'doj~.dpk\01c12.doj',      # small file, good for quick overview
    r'doj~.dpk\04c10.doj',      # largest file (~264 KB), most records
    r'doj~.dpk\d09c03s02.doj',  # medium scene file
]

for path in SAMPLE_FILES:
    try:
        analyze_file(path, max_samples=5)
    except FileNotFoundError:
        print(f'[SKIP] File not found: {path}')
    except Exception as e:
        print(f'[ERROR] {path}: {e}')
