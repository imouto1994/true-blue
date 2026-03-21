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

# All three of the above can carry text.  Other opcodes (0x11, 0x14, 0x33 …)
# are flow-control / resource-load commands and carry no Japanese text.
DIALOGUE_OPCODES = (OPCODE_0A, OPCODE_2A, OPCODE_22)


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


# ── String helpers ────────────────────────────────────────────────────────────

def read_sjis_string(data: bytes, offset: int) -> tuple[str, int]:
    """
    Read a null-terminated Shift-JIS string starting at `offset`.

    Returns (unicode_string, offset_after_null).
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
        text = raw.decode('shift-jis', errors='replace')
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
    b = data[offset]
    is_sjis_lead   = (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC)
    is_ascii_print = 0x20 <= b <= 0x7E
    return is_sjis_lead or is_ascii_print


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

        # Skip anything that isn't one of the three text-bearing opcodes with
        # a 0x00 sub-byte.  Non-text opcodes advance by 1 so we don't skip over
        # the next real record.
        if sub != 0x00 or op not in DIALOGUE_OPCODES:
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
                text = text.strip()
                if text:
                    entries.append({'type': 'narration', 'offset': i, 'text': text})
                i = next_i   # jump past the null-terminated string
            else:
                i += 2   # advance past the 2-byte opcode and re-scan

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
                    text = text.strip()
                    if text:
                        entries.append({'type': 'dialogue', 'offset': i, 'text': text})
                    i = next_i
                else:
                    i += 2

            elif op == OPCODE_2A:
                # ── Form B: choice menu ────────────────────────────────────
                # byte[4] = number of choices; strings follow immediately at +6
                count = data[i + 4] if i + 5 < n else 0
                j = i + 6   # first choice string begins here
                choices = []
                for _ in range(min(count, 20)):   # cap at 20 as a sanity guard
                    if j >= n:
                        break
                    text, j = read_sjis_string(data, j)
                    text = text.strip()
                    # Filter out garbage bytes that slipped through: only keep
                    # strings that contain at least one printable character
                    # and no raw control bytes (< 0x20).
                    if (text
                            and len(text) > 1
                            and not any(ord(c) < 0x20 for c in text)):
                        choices.append(text)
                if choices:
                    entries.append({'type': 'choices', 'offset': i, 'choices': choices})
                i = j   # jump to byte after the last choice string's null

            else:
                # 0x22 with non-zero param: unknown variant, skip the opcode
                i += 2

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
