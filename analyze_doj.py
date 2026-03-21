#!/usr/bin/env python3
"""
analyze_doj.py - Low-level binary analysis tool for decoded Lilim .doj script files.

PURPOSE
-------
This tool is intended for developers who want to inspect the raw decoded .doj
binary format — for example when extending decode_doj.py to handle new opcodes,
verifying parsing logic, or reverse-engineering fields that are not yet understood.

It is NOT required for normal text extraction. Use decode_doj.py for that.

This script has three sections — edit whichever is useful for your task:
  Section 1: Opcode frequency count across all files
  Section 2: Choice block discovery and verification (0x06, 0x2A:Form B)
  Section 3: Marker offset confirmation (0x0A, 0x2A, 0x22 records)

See DOJ_FORMAT.md for the full binary format specification.

USAGE
-----
    python analyze_doj.py
"""

import sys
import os
import struct

sys.stdout.reconfigure(encoding='utf-8')


# ── Shift-JIS string reader ───────────────────────────────────────────────────

def decode_sjis(data: bytes, start: int) -> tuple[str, int]:
    """
    Read a null-terminated Shift-JIS string starting at `start`.
    Returns (decoded_str, offset_after_null).

    Shift-JIS uses 2-byte sequences for lead bytes 0x81-0x9F and 0xE0-0xFC,
    so we advance by 2 for those to avoid false null detection mid-character.
    """
    end = start
    while end < len(data) and data[end] != 0:
        b = data[end]
        if (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC):
            end += 2   # 2-byte character
        else:
            end += 1
    return data[start:end].decode('shift-jis', errors='replace'), end + 1


def is_text_byte(b: int) -> bool:
    """Return True if byte `b` can start a Shift-JIS or printable ASCII string."""
    return (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC) or (0x20 < b <= 0x7E)


# ── Marker offset scanner ─────────────────────────────────────────────────────

def find_marker_offset(data: bytes, pos: int) -> int | None:
    """
    Scan a record starting at `pos` for the 0x01 0x00 'has text' marker.
    Searches relative offsets 14-25. Returns offset of 0x01 byte, or None.

    Only used in Section 3 (discovery helper for 0x0A / 0x2A / 0x22 records).
    decode_doj.py uses hard-coded confirmed offsets instead.
    """
    for offset in range(14, 26):
        if pos + offset + 2 >= len(data):
            break
        if data[pos + offset] == 0x01 and data[pos + offset + 1] == 0x00:
            if is_text_byte(data[pos + offset + 2]):
                return offset
    return None


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 1 — Opcode frequency across all files
# ══════════════════════════════════════════════════════════════════════════════

def count_all_opcodes(doj_dir: str) -> dict[int, int]:
    """Count how many times each (byte, 0x00) opcode pair appears across all files."""
    counts: dict[int, int] = {}
    for fn in sorted(os.listdir(doj_dir)):
        if not fn.lower().endswith('.doj'):
            continue
        with open(os.path.join(doj_dir, fn), 'rb') as f:
            data = f.read()
        for i in range(len(data) - 1):
            if data[i + 1] == 0x00:
                counts[data[i]] = counts.get(data[i], 0) + 1
    return counts


OPCODE_LABELS = {
    0x06: 'PRIMARY choice menu (Japanese text options)',
    0x0A: 'narration / monologue',
    0x2A: 'dialogue (Form A) or choice menu (Form B)',
    0x22: 'dialogue alt',
    0x11: 'flow control / jump',
    0x14: 'resource load',
    0x08: 'audio / SE trigger',
    0x07: 'animation command',
    0x33: 'scene-level command',
}

print('═' * 65)
print('SECTION 1 — Opcode frequency across all doj~.dpk files')
print('═' * 65)
for op, cnt in sorted(count_all_opcodes(r'doj~.dpk').items(), key=lambda x: -x[1])[:20]:
    print(f'  0x{op:02X}: {cnt:6d}  {OPCODE_LABELS.get(op, "")}')
print()


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 2 — Choice block discovery and verification
#
# Two choice formats exist:
#   0x06: Main player choices. Strings at +2, no count, end at non-text byte.
#         Also used for internal routing labels — filter by Japanese content.
#   0x2A param=0x0022: count byte at +4, strings from +6, includes control padding.
#   0x2A param=0x0004: no count byte, strings from +4, end at non-text byte.
# ══════════════════════════════════════════════════════════════════════════════

print('═' * 65)
print('SECTION 2 — 0x06 choice blocks')
print('═' * 65)

# Scan all files for 0x06 blocks that look like real player choices
# (at least 2 strings containing Japanese characters)
doj_dir = r'doj~.dpk'
total_real = total_routing = 0
for fn in sorted(os.listdir(doj_dir)):
    if not fn.lower().endswith('.doj'):
        continue
    with open(os.path.join(doj_dir, fn), 'rb') as f:
        d = f.read()
    n = len(d)
    idx = 0
    while idx < n - 4:
        if d[idx] == 0x06 and d[idx + 1] == 0x00 and is_text_byte(d[idx + 2]):
            j = idx + 2
            all_strings = []
            for _ in range(15):
                if j >= n or not is_text_byte(d[j]):
                    break
                txt, j = decode_sjis(d, j)
                txt = txt.strip()
                if txt and not any(ord(c) < 0x20 for c in txt):
                    all_strings.append(txt)
            # Real choices contain Japanese Shift-JIS characters
            def has_jp(s: str) -> bool:
                return any('\u3000' <= c or '\u4e00' <= c <= '\u9fff' for c in s)
            jp_choices = [s for s in all_strings if has_jp(s)]
            if len(jp_choices) >= 2:
                total_real += 1
                choices_str = ' | '.join(jp_choices)
                print(f'  CHOICE  {fn} @ 0x{idx:04X}: {choices_str}')
            elif all_strings:
                total_routing += 1
                # Uncomment the next line to see routing label blocks too:
                # print(f'  routing {fn} @ 0x{idx:04X}: {all_strings}')
        idx += 1
print(f'\n  Real choice blocks: {total_real}, Routing label blocks (skipped): {total_routing}')
print()


# ══════════════════════════════════════════════════════════════════════════════
# SECTION 3 — Marker offset confirmation for 0x0A / 0x2A / 0x22
# ══════════════════════════════════════════════════════════════════════════════

print('═' * 65)
print('SECTION 3 — Marker offset confirmation (sample file: 01c12.doj)')
print('═' * 65)

# Edit this path to analyze a different file:
SAMPLE_FILE = r'doj~.dpk\01c12.doj'

with open(SAMPLE_FILE, 'rb') as f:
    data = f.read()

for target_op in (0x0A, 0x2A, 0x22):
    tally: dict[int, int] = {}
    samples: list[tuple[int, int, str]] = []
    i = 0
    while i < len(data) - 25:
        if data[i] == target_op and data[i + 1] == 0x00:
            mo = find_marker_offset(data, i)
            if mo is not None:
                tally[mo] = tally.get(mo, 0) + 1
                if len(samples) < 3:
                    txt, _ = decode_sjis(data, i + mo + 2)
                    samples.append((i, mo, txt.strip()))
        i += 1
    if not tally:
        continue
    print(f'Opcode 0x{target_op:02X}: "has text" marker offset distribution: {dict(sorted(tally.items()))}')
    for (file_off, mo, txt) in samples:
        print(f'  0x{file_off:04X}: marker@+{mo} text: {txt[:60]!r}')
print()
