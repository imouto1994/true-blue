#!/usr/bin/env python3
"""
analyze_doj.py - Low-level binary analysis tool for decoded Lilim .doj script files.

PURPOSE
-------
This tool is intended for developers who want to inspect the raw decoded .doj
binary format — for example when extending decode_doj.py to handle new opcodes,
verifying parsing logic, or reverse-engineering fields that are not yet understood.

It is NOT required for normal text extraction. Use decode_doj.py for that.

See DOJ_FORMAT.md for the full binary format specification.

USAGE
-----
    python analyze_doj.py
"""

import sys
import os
import struct

sys.stdout.reconfigure(encoding='utf-8')

DOJ_DIR = os.path.join('doj~.dpk')


# ── Shift-JIS string reader ───────────────────────────────────────────────────

def decode_sjis(data: bytes, start: int) -> tuple[str, int]:
    """
    Read a null-terminated Shift-JIS string starting at `start`.
    Returns (decoded_str, offset_after_null).
    """
    end = start
    while end < len(data) and data[end] != 0:
        b = data[end]
        if (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC):
            end += 2
        else:
            end += 1
    return data[start:end].decode('shift-jis', errors='replace'), end + 1


def is_text_byte(b: int) -> bool:
    """Return True if byte `b` can start a Shift-JIS or printable ASCII string."""
    return (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC) or (0x20 < b <= 0x7E)


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 1 — Investigate missing alternate lines in text records
#
# Hypothesis: each 0x0A / 0x2A / 0x22 text record might contain TWO
# consecutive null-terminated strings, and our decoder only reads the first.
# This section reads each record's first string, then checks if a second
# valid Shift-JIS string immediately follows.
# ══════════════════════════════════════════════════════════════════════════════

SAMPLE_FILE = os.path.join(DOJ_DIR, 'PRG.doj')

print('═' * 70)
print(f'SECTION 1 — Post-string analysis for: {os.path.basename(SAMPLE_FILE)}')
print('═' * 70)

with open(SAMPLE_FILE, 'rb') as f:
    data = f.read()

n = len(data)
record_num = 0
i = 0
while i < n - 1:
    op = data[i]
    sub = data[i + 1]

    if sub != 0x00 or op not in (0x0A, 0x2A, 0x22):
        i += 1
        continue

    param = struct.unpack_from('<H', data, i + 2)[0] if i + 4 <= n else 0xFFFF

    # Determine marker and text offsets based on opcode
    if op == 0x0A:
        mo, to = 20, 22
    elif op in (0x2A, 0x22) and param == 0x0000:
        mo, to = 18, 20
    else:
        i += 2
        continue

    # Check the "has text" marker
    if (i + to <= n
            and data[i + mo] == 0x01
            and data[i + mo + 1] == 0x00
            and i + to < n
            and is_text_byte(data[i + to])):

        text1, after1 = decode_sjis(data, i + to)
        text1 = text1.strip()

        # Now check what's right after the first string's null terminator
        bytes_after = data[after1:after1 + 30] if after1 + 30 <= n else data[after1:]
        has_second = after1 < n and is_text_byte(data[after1])

        text2 = ''
        after2 = after1
        if has_second:
            text2, after2 = decode_sjis(data, after1)
            text2 = text2.strip()

        record_num += 1
        if record_num <= 60:
            hex_preview = ' '.join(f'{b:02X}' for b in bytes_after[:16])
            print(f'\n  Record #{record_num:3d}  op=0x{op:02X} @ 0x{i:04X}')
            print(f'    String 1: {text1[:70]!r}')
            print(f'    After null (0x{after1:04X}): {hex_preview}')
            if has_second and text2:
                print(f'    String 2: {text2[:70]!r}')
                print(f'    After str2 null (0x{after2:04X}): '
                      + ' '.join(f'{b:02X}' for b in data[after2:after2 + 10]))
            else:
                print(f'    String 2: (none / not text)')

        i = after1
    else:
        i += 2

print(f'\n  Total text records found: {record_num}')
print()


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 2 — Find skipped 0x0A/0x2A/0x22 records (marker != 0x01)
#
# The current decoder requires data[i+20]==0x01 (for 0x0A) or data[i+18]==0x01
# (for 0x2A/0x22). Records that don't match are skipped with i+=2.
# This section finds those SKIPPED records and checks if they contain text
# at some other offset, or if they have a different marker byte.
# ══════════════════════════════════════════════════════════════════════════════

print('═' * 70)
print(f'SECTION 2 — Skipped records analysis for: {os.path.basename(SAMPLE_FILE)}')
print('═' * 70)

skipped_count = 0
skipped_with_text = 0
marker_byte_dist: dict[int, int] = {}

i = 0
while i < n - 22:
    op = data[i]
    sub = data[i + 1]

    if sub != 0x00 or op not in (0x0A, 0x2A, 0x22):
        i += 1
        continue

    param = struct.unpack_from('<H', data, i + 2)[0] if i + 4 <= n else 0xFFFF

    if op == 0x0A:
        mo, to = 20, 22
    elif op in (0x2A, 0x22) and param == 0x0000:
        mo, to = 18, 20
    else:
        i += 2
        continue

    marker_val = data[i + mo] if i + mo < n else 0xFF
    marker_byte_dist[marker_val] = marker_byte_dist.get(marker_val, 0) + 1

    if marker_val == 0x01 and data[i + mo + 1] == 0x00:
        # Normal text record — skip past the string
        if i + to < n and is_text_byte(data[i + to]):
            _, next_i = decode_sjis(data, i + to)
            i = next_i
        else:
            i += 2
    else:
        # SKIPPED record — inspect it
        skipped_count += 1
        # Check if there's text at the expected text offset anyway
        has_text_at_to = i + to < n and is_text_byte(data[i + to])
        # Also scan nearby offsets for potential text
        found_text_offset = None
        for probe in range(to - 4, to + 8):
            if i + probe < n and is_text_byte(data[i + probe]):
                try_txt, _ = decode_sjis(data, i + probe)
                try_txt = try_txt.strip()
                if len(try_txt) > 3 and not any(ord(c) < 0x20 for c in try_txt):
                    found_text_offset = probe
                    break

        if found_text_offset is not None:
            skipped_with_text += 1
        if skipped_count <= 30:
            hex_context = ' '.join(f'{data[i+k]:02X}' for k in range(min(30, n - i)))
            print(f'\n  Skipped #{skipped_count:3d}  op=0x{op:02X} @ 0x{i:04X}  '
                  f'marker@+{mo}=0x{marker_val:02X}  param=0x{param:04X}')
            print(f'    Raw: {hex_context}')
            if found_text_offset is not None:
                txt, _ = decode_sjis(data, i + found_text_offset)
                print(f'    Text at +{found_text_offset}: {txt.strip()[:70]!r}')
            else:
                print(f'    No text found nearby')
        i += 2

print(f'\n  Total skipped records: {skipped_count}')
print(f'  Skipped records WITH text nearby: {skipped_with_text}')
print(f'\n  Marker byte distribution at expected offset: {dict(sorted(marker_byte_dist.items()))}')
print()


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 3 — Hex dump gaps between consecutive text records
#
# Shows the bytes between the end of one text record and the start of the next.
# Helps identify what structure exists in the inter-record gap.
# ══════════════════════════════════════════════════════════════════════════════

print('═' * 70)
print(f'SECTION 3 — Gaps between text records in: {os.path.basename(SAMPLE_FILE)}')
print('═' * 70)

# First pass: collect all text records with their end positions
text_records = []
i = 0
while i < n - 1:
    op = data[i]
    sub = data[i + 1]
    if sub != 0x00 or op not in (0x0A, 0x2A, 0x22):
        i += 1
        continue
    param = struct.unpack_from('<H', data, i + 2)[0] if i + 4 <= n else 0xFFFF
    if op == 0x0A:
        mo, to = 20, 22
    elif op in (0x2A, 0x22) and param == 0x0000:
        mo, to = 18, 20
    else:
        i += 2
        continue
    if (i + to <= n and data[i + mo] == 0x01 and data[i + mo + 1] == 0x00
            and i + to < n and is_text_byte(data[i + to])):
        txt, next_i = decode_sjis(data, i + to)
        text_records.append({
            'op': op, 'start': i, 'str_end': next_i,
            'text': txt.strip()
        })
        i = next_i
    else:
        i += 2

for idx in range(min(15, len(text_records) - 1)):
    r1 = text_records[idx]
    r2 = text_records[idx + 1]
    gap_start = r1['str_end']
    gap_end = r2['start']
    gap_size = gap_end - gap_start

    print(f'\n  Gap #{idx+1}: 0x{gap_start:04X}–0x{gap_end:04X} ({gap_size} bytes)')
    print(f'    Prev: {r1["text"][:50]!r}')
    print(f'    Next: {r2["text"][:50]!r}')

    # Dump the gap bytes
    gap_bytes = data[gap_start:gap_end]
    # Show in rows of 16
    for row_off in range(0, min(len(gap_bytes), 80), 16):
        chunk = gap_bytes[row_off:row_off + 16]
        hex_str = ' '.join(f'{b:02X}' for b in chunk)
        ascii_str = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in chunk)
        print(f'    0x{gap_start + row_off:04X}: {hex_str:<48s} {ascii_str}')

print()
