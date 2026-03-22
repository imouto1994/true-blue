#!/usr/bin/env python3
"""
decode_doj.py - Extract Japanese text from decoded Lilim .doj script files.

BACKGROUND
----------
The game's dialogue data is stored in a two-layer archive:

  doj.dpk  (container archive, "PA" magic)
      └─ *.doj  (per-scene script files, "CC" magic, LZSS-compressed)

arc_unpacker's `lilim/dpk` decoder extracts the archive and its built-in
`linked_formats` list causes it to automatically apply the `lilim/doj` decoder
to each extracted file. The `lilim/doj` decoder strips the "CC" header and
LZSS-decompresses the payload, writing the result to `doj~.dpk/`.

The FILES IN `doj~.dpk/` ARE ALREADY FULLY DECODED — they are the binary
script bytecode — and this script is what reads those decoded files.

See DOJ_FORMAT.md for a full description of the decoded binary format.

USAGE
-----
    python decode_doj.py <file.doj>            - single file, print to stdout
    python decode_doj.py <dir/>                - all .doj files in dir, stdout
    python decode_doj.py <file.doj> -o out/    - single file, save as .txt
    python decode_doj.py <dir/> -o out/ -v     - all files, save, verbose log
"""

import sys
import os
import struct
import argparse


# ── Opcode constants ──────────────────────────────────────────────────────────
# All opcodes are single bytes; the following byte is always 0x00 (part of the
# u16 opcode field, stored little-endian).

OPCODE_0A = 0x0A   # Narration / inner monologue line
OPCODE_2A = 0x2A   # Character dialogue (single line) OR choice menu
OPCODE_22 = 0x22   # Character dialogue (alternate form, same layout as 0x2A)
OPCODE_06 = 0x06   # Choice menu (primary format, 3-option player choices)

# All four of the above can carry text.  Other opcodes are flow-control or
# resource-load commands — they carry no text in their primary payload, but
# a text sub-record may follow their resource path's null terminator.
DIALOGUE_OPCODES = (OPCODE_0A, OPCODE_2A, OPCODE_22, OPCODE_06)
NONTXT_OPCODES = (0x07, 0x08, 0x11, 0x14, 0x33)


# ── Record layouts (all offsets relative to the start of the record) ──────────
#
# 0x0A (narration):
#   +0   u8   opcode  (0x0A)
#   +1   u8   0x00
#   +2   u16le  param  (varies; high bit set → non-text command record)
#   +4   16 bytes  scene / character context flags (not decoded here)
#   +20  u8   0x01  = "has text" marker  (anything else → no text, skip record)
#   +21  u8   0x00
#   +22  …    null-terminated Shift-JIS text string
#
# 0x2A / 0x22 — Form A (single dialogue line, param == 0x0000):
#   +0   u8   opcode  (0x2A or 0x22)
#   +1   u8   0x00
#   +2   u16le  0x0000  (distinguishes from Form B below)
#   +4   14 bytes  context flags
#   +18  u8   0x01  = "has text" marker
#   +19  u8   0x00
#   +20  …    null-terminated Shift-JIS text string
#
# 0x2A — Form B (choice menu, param != 0x0000):
#   +0   u8   0x2A
#   +1   u8   0x00
#   +2   u16le  flags  (non-zero; exact meaning unknown)
#   +4   u8   number_of_choices
#   +5   u8   0x00
#   +6   …    number_of_choices × null-terminated Shift-JIS strings

# Byte offset at which the "has text" marker (0x01 0x00) lives:
MARKER_OFFSET_0A      = 20   # for 0x0A records
MARKER_OFFSET_2A_22   = 18   # for 0x2A / 0x22 Form-A records

# Byte offset at which the null-terminated text string starts:
TEXT_OFFSET_0A        = 22
TEXT_OFFSET_2A_22     = 20

# After the first string's null terminator, a second text line may follow
# with its own sub-record header.  The marker is always at +18 from the byte
# after the null, and the text starts at +20.
SECOND_STRING_MARKER_OFFSET = 18
SECOND_STRING_TEXT_OFFSET   = 20


# ── String helpers ────────────────────────────────────────────────────────────

# Characters to strip from decoded text.  Only ASCII controls are removed —
# NOT the ideographic space U+3000 (Shift-JIS 0x81 0x40), which is used as
# a leading indent in narration lines.  Python's str.strip() treats U+3000
# as whitespace and would silently remove it.
_STRIP_CHARS = ''.join(chr(c) for c in range(0x20)) + '\x7f'


def read_sjis_string(data: bytes, offset: int) -> tuple[str, int]:
    """
    Read a null-terminated Shift-JIS string starting at `offset`.

    Returns (unicode_string, offset_after_null).
    Decoded with CP932 (Microsoft's Shift-JIS superset) rather than the
    JIS standard codec, so that characters like the wave dash (0x81 0x60)
    map to U+FF5E (～) instead of U+301C (〜).

    Shift-JIS uses two bytes for lead byte ranges 0x81–0x9F and 0xE0–0xFC,
    so we must advance by 2 for those to avoid mistaking the trailing byte
    of a 2-byte character for a null terminator.
    """
    end = offset
    while end < len(data) and data[end] != 0x00:
        b = data[end]
        if (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC):
            end += 2   # 2-byte Shift-JIS character
        else:
            end += 1   # 1-byte ASCII or Katakana half-width
    raw = data[offset:end]
    try:
        text = raw.decode('cp932', errors='replace')
    except Exception:
        text = raw.decode('latin-1', errors='replace')
    return text, end + 1   # +1 to skip the null terminator


def looks_like_text(data: bytes, offset: int) -> bool:
    """
    Heuristic: does the byte at `offset` look like the start of a valid
    Shift-JIS string?  We check for:
      - Shift-JIS 2-byte lead bytes  (0x81–0x9F, 0xE0–0xFC)
      - Printable ASCII              (0x20–0x7E)
    This is used to reject false-positive "text" records that contain
    raw binary data or control bytes where text is not actually present.
    """
    if offset >= len(data):
        return False
    # Skip leading 0x0A (newline) bytes — they appear at the start of some
    # text strings and are stripped from the final output.
    while offset < len(data) and data[offset] == 0x0A:
        offset += 1
    if offset >= len(data):
        return False
    b = data[offset]
    is_sjis_lead   = (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC)
    is_ascii_print = 0x20 <= b <= 0x7E
    return is_sjis_lead or is_ascii_print


def _is_clean_text(text: str) -> bool:
    """Reject decoded strings that are too short, contain control chars, or U+FFFD."""
    return (len(text) > 1
            and not any(ord(c) < 0x20 or c == '\ufffd' for c in text))


# ── Chain concatenation ──────────────────────────────────────────────────────

_MAX_CHAIN_DEPTH = 20


def _concat_chain(
    data: bytes, n: int,
    chain_pos: int,
    text: str, end_pos: int,
    entries: list[dict], entry_type: str,
) -> tuple[str, int]:
    """
    After reading a null-terminated second-string, check for continuation
    sub-records that should be concatenated into one line.

    The game engine chains multiple short sub-records for repeating effects
    (e.g. "グラグラ" × 5 + "グラグラッ！").  Each has the standard layout:
    sub_size, 16 context bytes, 0x01 0x00 marker, text.

    Only concatenates chunks that are repetitions of the existing text
    (substring match).  Non-matching chunks are added as separate entries
    so they aren't lost between the chain and the next opcode record.

    Returns (text, end_pos).  Appended entries go directly into `entries`.
    """
    smo = SECOND_STRING_MARKER_OFFSET  # 18
    had_short_chunk = False  # tracks typewriter mode

    for _ in range(_MAX_CHAIN_DEPTH):
        # Probe a small window for the next sub-record header
        found = False
        for probe in range(4):
            p = chain_pos + probe
            if p + 22 > n:
                break
            ss = struct.unpack_from('<H', data, p)[0]
            if not (22 <= ss <= 512 and p + ss <= n):
                continue
            if data[p + smo] != 0x01 or data[p + smo + 1] != 0x00:
                continue

            txt_off = p + 20
            if txt_off >= n or not looks_like_text(data, txt_off):
                continue

            raw2 = data[p + 20 : p + ss]
            null2 = raw2.find(b'\x00')
            if null2 >= 0:
                raw2 = raw2[:null2]
            elif (raw2
                  and (0x81 <= raw2[-1] <= 0x9F or 0xE0 <= raw2[-1] <= 0xFC)
                  and p + ss < n):
                raw2 = data[p + 20 : p + ss + 1]

            try:
                chunk = raw2.decode('cp932', errors='replace').strip(_STRIP_CHARS)
            except Exception:
                break

            if not chunk or any(ord(c) < 0x20 or c == '\ufffd' for c in chunk):
                break

            # A leading ideographic space always means a new independent line.
            if chunk[0] == '\u3000':
                break

            is_short = len(chunk) <= 3

            if is_short:
                # Short chunks are always part of a typewriter/reveal effect.
                had_short_chunk = True
            elif had_short_chunk:
                # In typewriter mode: accept longer final chunks (the reveal
                # completes with a longer tail like "ろーーーーーっ！！！」").
                pass
            else:
                # Not in typewriter mode: only accept repetitions.
                base = text.replace('\u3000', '')
                if not (chunk in base or base in chunk
                        or chunk.rstrip('ッ！!') in base):
                    break

            text += chunk
            end_pos = p + ss
            chain_pos = p + 20 + (null2 + 1 if null2 >= 0 else len(raw2))
            found = True
            break

        if not found:
            break

    return text, end_pos


# ── Second-string reader ─────────────────────────────────────────────────────

def try_read_second_string(
    data: bytes, after_null: int, n: int,
    entries: list[dict], entry_type: str,
) -> int:
    """
    After the first text string's null terminator, check for an optional second
    text line embedded in the same record.

    The sub-record layout (relative to `after_null`):
        +0   u16le  sub_size  (byte distance to the next opcode record)
        +2   16 bytes  context/padding
        +18  0x01 0x00  "has text" marker
        +20  Shift-JIS text  (NOT null-terminated; bounded by sub_size)

    The second string is NOT null-terminated in the traditional sense — it
    runs right up to the boundary of the next opcode record.  Using
    read_sjis_string() would overshoot into the next record's opcode byte.
    Instead we use the sub_size field to slice the exact text bytes.

    Returns the new scan position (start of the next opcode record if text2
    was found, or `after_null` unchanged so the main loop byte-scans normally).
    """
    smo = SECOND_STRING_MARKER_OFFSET  # 18
    sto = SECOND_STRING_TEXT_OFFSET    # 20
    pos = after_null

    if pos + 2 > n:
        return pos

    sub_size = struct.unpack_from('<H', data, pos)[0]
    if sub_size < sto + 2:
        return pos

    end_pos = pos + sub_size
    if end_pos > n:
        return pos

    if (data[pos + smo]     == 0x01
            and data[pos + smo + 1] == 0x00
            and looks_like_text(data, pos + sto)):

        raw = data[pos + sto : end_pos]
        null_idx = raw.find(b'\x00')
        if null_idx >= 0:
            raw = raw[:null_idx]
        elif (raw
              and (0x81 <= raw[-1] <= 0x9F or 0xE0 <= raw[-1] <= 0xFC)
              and end_pos < n):
            # sub_size boundary split a 2-byte SJIS character; include trail byte
            raw = data[pos + sto : end_pos + 1]

        try:
            text = raw.decode('cp932', errors='replace').strip(_STRIP_CHARS)
        except Exception:
            text = ''

        if len(text) > 1 and _is_clean_text(text):
            # Check for chained continuation sub-records (e.g. repeating
            # sound effects like "グラグラ" × N + "グラグラッ！").
            if null_idx >= 0:
                chain_start = pos + sto + null_idx + 1
            else:
                chain_start = pos + sto + len(raw)
            if chain_start < n and data[chain_start] == 0x00:
                orig_text = text
                text, end_pos = _concat_chain(
                    data, n, chain_start + 1, text, end_pos,
                    entries, entry_type)
                if text != orig_text:
                    entries.append({'type': 'chain_concat', 'offset': pos, 'text': text})
                    return end_pos
            entries.append({'type': entry_type, 'offset': pos, 'text': text})
            return end_pos

    return pos


# ── Choice sub-record reader ─────────────────────────────────────────────────

def try_read_choice_subrecord(
    data: bytes, pos: int, n: int, entries: list[dict],
) -> int:
    """
    Check for a choice sub-record at `pos`.  These appear after a text or
    narration sub-record and have a variable-length header followed by
    multiple null-terminated Shift-JIS choice strings.

    Layout (relative to `pos`):
        +0   u16le  sub_size
        +2   2–4 u16le header fields  (variable; meaning not fully decoded)
        +6/+8  null-terminated choice strings

    Returns the position past the sub-record if choices were found, or
    `pos` unchanged.
    """
    if pos + 8 >= n:
        return pos

    sub_size = struct.unpack_from('<H', data, pos)[0]
    if sub_size < 8 or sub_size > 512:
        return pos

    end_pos = pos + sub_size
    if end_pos > n:
        return pos

    for text_off in (6, 8, 10):
        if pos + text_off >= n or not looks_like_text(data, pos + text_off):
            continue
        j = pos + text_off
        choices: list[str] = []
        while j < end_pos and j < n and looks_like_text(data, j):
            c, j = read_sjis_string(data, j)
            c = c.strip(_STRIP_CHARS)
            if c and not any(ord(ch) < 0x20 for ch in c):
                choices.append(c)

        def _has_japanese(s: str) -> bool:
            return any('\u3000' <= c <= '\u9FFF' or '\uFF00' <= c <= '\uFFEF'
                       or '\u4E00' <= c <= '\u9FFF' for c in s)

        real = [c for c in choices if _has_japanese(c)]
        if len(real) >= 2:
            entries.append({'type': 'choices', 'offset': pos, 'choices': real})
            return end_pos

    return pos


_SUBRECORD_PROBE = 4


def _complete_short_brackets(
    data: bytes, pos: int, n: int, entries: list[dict],
) -> int:
    """
    When the most recent entry is very short (≤5 chars) and has unmatched
    「…」 brackets, scan forward for continuation sub-records and concatenate
    them until brackets balance.  This handles typewriter/reveal effects
    where text is split into single-character sub-records.

    The short-text restriction prevents false triggers on normal multi-page
    dialogue where 「 and 」 are in separate records by design.

    Uses relaxed sub_size minimum (21) to capture single-SJIS-character
    sub-records that the standard threshold (22) would reject.
    """
    if not entries:
        return pos

    last = entries[-1]
    text = last.get('text', '')
    if len(text) > 5 or text.count('「') <= text.count('」'):
        return pos

    smo = SECOND_STRING_MARKER_OFFSET  # 18

    for _ in range(50):
        if text.count('「') <= text.count('」'):
            break

        found = False
        for probe in range(4):
            pp = pos + probe
            if pp + 21 > n:
                break
            ss = struct.unpack_from('<H', data, pp)[0]
            if not (21 <= ss <= 512 and pp + ss <= n):
                continue
            if data[pp + smo] != 0x01 or data[pp + smo + 1] != 0x00:
                continue

            txt_off = pp + 20
            while txt_off < n and data[txt_off] == 0x0A:
                txt_off += 1
            if txt_off >= n or not looks_like_text(data, txt_off):
                continue

            raw = data[pp + 20 : pp + ss]
            null_idx = raw.find(b'\x00')
            if null_idx >= 0:
                raw = raw[:null_idx]
            elif (raw
                  and (0x81 <= raw[-1] <= 0x9F or 0xE0 <= raw[-1] <= 0xFC)
                  and pp + ss < n):
                raw = data[pp + 20 : pp + ss + 1]

            try:
                chunk = raw.decode('cp932', errors='replace').strip(_STRIP_CHARS)
            except Exception:
                break

            if not chunk or any(ord(c) < 0x20 or c == '\ufffd' for c in chunk):
                break

            text += chunk
            last['text'] = text
            pos = pp + ss
            found = True
            break

        if not found:
            break

    return pos


def _try_subrecords(
    data: bytes, pos: int, n: int, entries: list[dict], entry_type: str,
) -> int:
    """
    After a text string's null terminator, try the chain of possible
    sub-records: narration second-string, then choice sub-record.

    When neither is found at the exact position, a one-time null-scan
    (up to 256 bytes) bridges resource-path sub-records that separate
    text1 from text2.

    Returns the final scan position.
    """
    # 1. Narration second-string (exact position)
    entries_before = len(entries)
    new_pos = try_read_second_string(data, pos, n, entries, entry_type)

    # Detect if chain concatenation happened (tagged as 'chain_concat')
    chain_did_concat = (len(entries) > entries_before
                        and entries[-1].get('type') == 'chain_concat')
    if chain_did_concat:
        entries[-1]['type'] = entry_type  # normalize back to narration

    # 2. Choice sub-record (probe a small window for alignment)
    for probe in range(_SUBRECORD_PROBE):
        p = new_pos + probe
        result = try_read_choice_subrecord(data, p, n, entries)
        if result > p:
            return result

    if new_pos > pos:
        # 2b. After a chain concatenation, the sub-record right at the
        #     boundary was rejected (non-repeating text).  A tiny null-scan
        #     captures it.  Only runs when the chain actually merged chunks,
        #     to avoid consuming unrelated content in non-chain records.
        if chain_did_concat:
            for j in range(new_pos, min(new_pos + 10, n)):
                if data[j] == 0x00:
                    nxt = try_read_second_string(
                        data, j + 1, n, entries, entry_type)
                    if nxt > j + 1:
                        new_pos = nxt
                        break
        return new_pos

    # 3. Neither found at exact position. One-time null-scan forward
    #    to bridge resource-path sub-records.
    scan_end = min(pos + _NONTXT_SCAN_LIMIT, n - 1)
    j = pos + 2
    while j < scan_end:
        if data[j] == 0x00:
            nxt = try_read_second_string(
                data, j + 1, n, entries, entry_type)
            if nxt > j + 1:
                for probe in range(_SUBRECORD_PROBE):
                    p = nxt + probe
                    result = try_read_choice_subrecord(data, p, n, entries)
                    if result > p:
                        return result
                return nxt
        j += 1

    return pos


# ── Non-text record scanner ──────────────────────────────────────────────────

_NONTXT_SCAN_LIMIT = 256

def _scan_nontxt_for_text(
    data: bytes, rec_start: int, n: int, entries: list[dict],
) -> int:
    """
    For a non-text 0x0A record (marker at +20 != 0x01), scan forward for
    null-terminated resource paths and check for a text sub-record after
    each null terminator.

    Non-text records often contain a resource path (e.g. "SE137", "bg032a")
    starting at +4.  After the path's null, a second-string sub-record may
    carry narration text that would otherwise be missed.

    Returns the new scan position.
    """
    scan_end = min(rec_start + _NONTXT_SCAN_LIMIT, n - 1)
    j = rec_start + 4
    while j < scan_end:
        if data[j] == 0x00:
            new_i = _try_subrecords(data, j + 1, n, entries, 'narration')
            if new_i > j + 1:
                return new_i
        j += 1
    return rec_start + 2


# ── Main parser ───────────────────────────────────────────────────────────────

def decode_doj(filepath: str) -> list[dict]:
    """
    Parse a decoded .doj binary file and return a list of script entries.

    Each entry is a dict with:
        {'type': 'narration',  'offset': int, 'text': str}
        {'type': 'dialogue',   'offset': int, 'text': str}
        {'type': 'choices',    'offset': int, 'choices': list[str]}

    The parser is a simple linear scan; it relies on the sub-byte (always 0x00
    for the opcodes we care about) plus opcode value to decide what to do.
    Unknown opcodes advance by 1 byte, which is safe because the 0x00 sub-byte
    serves as a natural resync point.
    """
    with open(filepath, 'rb') as f:
        data = f.read()

    entries = []
    i = 0
    n = len(data)

    while i < n - 1:
        op  = data[i]
        sub = data[i + 1]

        if sub != 0x00:
            i += 1
            continue

        # Non-text opcodes (0x07, 0x08, 0x14, etc.) can have text sub-records
        # following their resource path.  Scan them the same way as non-text
        # 0x0A records.
        if op in NONTXT_OPCODES:
            i = _scan_nontxt_for_text(data, i, n, entries)
            continue

        if op not in DIALOGUE_OPCODES:
            i += 1
            continue

        # Read the param field (u16le at bytes +2..+3); used to distinguish
        # 0x2A dialogue lines from 0x2A choice menus.
        param = struct.unpack_from('<H', data, i + 2)[0] if i + 4 <= n else 0xFFFF

        # ── 0x0A: Narration / inner monologue ─────────────────────────────
        if op == OPCODE_0A:
            mo = MARKER_OFFSET_0A     # 20
            to = TEXT_OFFSET_0A       # 22

            if (i + to <= n
                    and data[i + mo]     == 0x01   # "has text" marker byte 1
                    and data[i + mo + 1] == 0x00   # "has text" marker byte 2
                    and looks_like_text(data, i + to)):

                text, next_i = read_sjis_string(data, i + to)
                text = text.strip(_STRIP_CHARS)
                if _is_clean_text(text):
                    entries.append({'type': 'narration', 'offset': i, 'text': text})
                    next_i = _complete_short_brackets(data, next_i, n, entries)
                i = _try_subrecords(data, next_i, n, entries, 'narration')
            else:
                # Non-text 0x0A record (resource load, sound cue, etc.).
                # These records contain null-terminated resource paths, and
                # a text sub-record may follow after the path's null.  Scan
                # for null bytes and probe each with try_read_second_string.
                i = _scan_nontxt_for_text(data, i, n, entries)

        # ── 0x2A / 0x22: Dialogue or choice menu ──────────────────────────
        elif op in (OPCODE_2A, OPCODE_22):

            if param == 0x0000:
                # ── Form A: single dialogue line ───────────────────────────
                mo = MARKER_OFFSET_2A_22  # 18
                to = TEXT_OFFSET_2A_22    # 20

                if (i + to <= n
                        and data[i + mo]     == 0x01
                        and data[i + mo + 1] == 0x00
                        and looks_like_text(data, i + to)):

                    text, next_i = read_sjis_string(data, i + to)
                    text = text.strip(_STRIP_CHARS)
                    if _is_clean_text(text):
                        entries.append({'type': 'dialogue', 'offset': i, 'text': text})
                        next_i = _complete_short_brackets(data, next_i, n, entries)
                    i = _try_subrecords(data, next_i, n, entries, 'narration')
                else:
                    i += 2

            elif op == OPCODE_2A:
                # ── Form B: choice menu (two sub-variants) ─────────────────
                choices = []

                if param == 0x0022:
                    # Variant 1 (param=0x0022): count byte at [+4], strings from [+6].
                    # The count includes trailing control-code entries (e.g. 0x11
                    # jump targets placed as padding) — filter those out.
                    count = data[i + 4] if i + 5 < n else 0
                    j = i + 6
                    for _ in range(min(count, 20)):
                        if j >= n:
                            break
                        text, j = read_sjis_string(data, j)
                        text = text.strip(_STRIP_CHARS)
                        # Keep only genuine multi-character text with no control bytes.
                        if (text and len(text) > 1
                                and not any(ord(c) < 0x20 for c in text)):
                            choices.append(text)

                elif param == 0x0004:
                    # Variant 2 (param=0x0004): NO count byte.
                    # Strings start directly at [+4] and continue until a
                    # non-text byte (typically 0x11, a flow-control opcode).
                    j = i + 4
                    for _ in range(20):   # sanity cap
                        if j >= n or not looks_like_text(data, j):
                            break
                        text, j = read_sjis_string(data, j)
                        text = text.strip(_STRIP_CHARS)
                        if text and not any(ord(c) < 0x20 for c in text):
                            choices.append(text)

                else:
                    # Unknown param variant — skip the 2-byte opcode and rescan.
                    i += 2
                    continue

                if choices:
                    entries.append({'type': 'choices', 'offset': i, 'choices': choices})
                i = j   # advance past all choice strings

            else:
                # 0x22 with non-zero param: unknown variant, skip the opcode
                i += 2

        # ── 0x06: Main choice menu ────────────────────────────────────────────
        # Strings start immediately at +2 with no count or header bytes.
        # The list ends at the first byte that cannot start a Shift-JIS / ASCII string.
        #
        # This opcode is also used for internal resource-routing blocks that look
        # similar but contain only short ASCII labels (e.g. "99a04", "@", path refs).
        # We treat the block as a real choice menu only when at least 2 strings
        # contain a Japanese character (Shift-JIS 2-byte lead byte).  Otherwise
        # the whole block is silently skipped.
        elif op == OPCODE_06:
            j = i + 2   # strings start right after the 2-byte opcode
            candidates = []
            for _ in range(20):   # sanity cap
                if j >= n or not looks_like_text(data, j):
                    break
                text, j = read_sjis_string(data, j)
                text = text.strip(_STRIP_CHARS)
                if text and not any(ord(c) < 0x20 for c in text):
                    candidates.append(text)

            # Keep the block only if at least 2 options contain Japanese characters.
            # Japanese strings contain Shift-JIS lead bytes (0x81-0x9F or 0xE0-0xFC).
            def has_japanese(s: str) -> bool:
                return any('\u3000' <= c <= '\u9FFF' or '\uFF00' <= c <= '\uFFEF'
                           or '\u4E00' <= c <= '\u9FFF' for c in s)

            real_choices = [c for c in candidates if has_japanese(c)]
            if len(real_choices) >= 2:
                entries.append({'type': 'choices', 'offset': i, 'choices': real_choices})
            i = j   # advance past all strings (whether we kept them or not)

    return entries


# ── Output formatting ─────────────────────────────────────────────────────────

def format_output(entries: list[dict], filename: str) -> str:
    """
    Render a list of script entries as a human-readable UTF-8 text block.

    Layout:
      - Narration and dialogue lines appear verbatim, one per output line.
      - Choice menus appear as a labelled numbered list, separated by blank
        lines from surrounding dialogue.
      - The file header (=== filename ===) appears at the top.
    """
    lines = [f'=== {os.path.basename(filename)} ===', '']
    prev_type = None
    for e in entries:
        if e['type'] in ('narration', 'dialogue'):
            if prev_type == 'choices':
                lines.append('')   # blank line after a choice block
            lines.append(e['text'])
        elif e['type'] == 'choices':
            if prev_type is not None:
                lines.append('')   # blank line before choice block
            lines.append('[選択肢 / Choices:]')
            for idx, choice in enumerate(e['choices'], 1):
                lines.append(f'  {idx}. {choice}')
        prev_type = e['type']
    lines.append('')   # trailing newline
    return '\n'.join(lines)


# ── File I/O ──────────────────────────────────────────────────────────────────

def process_file(path: str, output_dir: str | None, verbose: bool) -> tuple[int, int]:
    """
    Decode one .doj file.  If `output_dir` is given, write a UTF-8 .txt file
    there; otherwise print to stdout.  Returns (n_text_lines, n_choice_menus).
    """
    try:
        entries = decode_doj(path)
    except Exception as ex:
        print(f'[ERROR] {path}: {ex}', file=sys.stderr)
        return 0, 0

    text = format_output(entries, path)

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        out_name = os.path.splitext(os.path.basename(path))[0] + '.txt'
        out_path = os.path.join(output_dir, out_name)
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(text)
        n_text = sum(1 for e in entries if e['type'] in ('narration', 'dialogue'))
        n_ch   = sum(1 for e in entries if e['type'] == 'choices')
        if verbose:
            print(f'  {os.path.basename(path):25s}  {n_text:4d} lines  {n_ch:2d} choice menus')
        return n_text, n_ch
    else:
        # Force UTF-8 output so Japanese characters render correctly even on
        # Windows where the console code page is often not UTF-8.
        sys.stdout.reconfigure(encoding='utf-8')
        print(text)
        return 0, 0


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Extract Japanese text from decoded Lilim .doj script files')
    parser.add_argument('input', help='.doj file or directory of .doj files')
    parser.add_argument('-o', '--out', default=None,
                        help='Output directory for UTF-8 .txt files (default: stdout)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Print per-file stats when using -o')
    args = parser.parse_args()

    if os.path.isdir(args.input):
        doj_files = sorted(
            os.path.join(args.input, f)
            for f in os.listdir(args.input)
            if f.lower().endswith('.doj')
        )
        if not doj_files:
            print(f'No .doj files found in {args.input}')
            sys.exit(1)
        print(f'Processing {len(doj_files)} .doj files...')
        total_lines = total_menus = 0
        for path in doj_files:
            nl, nm = process_file(path, args.out, verbose=bool(args.out) or args.verbose)
            total_lines += nl
            total_menus += nm
        if args.out:
            print(f'\nTotal: {total_lines} text lines, {total_menus} choice menus')
            print(f'Output written to: {os.path.abspath(args.out)}')
    elif os.path.isfile(args.input):
        process_file(args.input, args.out, verbose=args.verbose)
    else:
        print(f'Error: {args.input!r} not found', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
