#!/usr/bin/env python3
"""
generate_dictionary.py — Convert translation-map.json into dictionary.txt
for the True Blue English patch hook DLL.

The dictionary file uses tab-separated Shift-JIS encoded lines:
    {Japanese text}\t{English text}

The hook DLL loads this at runtime and replaces matching Japanese text
with the English translation.

Usage:
    python generate_dictionary.py
    (reads ../translation-map.json, writes dictionary.txt)
"""

import json
import os
import sys


JP_BYTES_PER_ROW = 44   # game wraps at 44 CP932 bytes (22 fullwidth chars)
EN_CHARS_PER_ROW = 44   # ASCII chars are half-width, ~44 fit in same space


def _is_sjis_lead(b: int) -> bool:
    return (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC)


def split_entry(jp: str, en: str) -> list[tuple[str, str]]:
    """
    Split a long JP->EN entry into row-sized fragments that match
    what the game sends via TextOutA.

    Splits on CP932-encoded byte boundaries (44 bytes per row) to match
    the C++ continuation cache logic exactly.
    """
    jp_bytes = jp.encode('cp932')
    if len(jp_bytes) <= JP_BYTES_PER_ROW:
        return [(jp, en)]

    jp_chunks = []
    off = 0
    while off < len(jp_bytes):
        end = min(off + JP_BYTES_PER_ROW, len(jp_bytes))
        check = off
        while check < end:
            if _is_sjis_lead(jp_bytes[check]):
                if check + 2 > end:
                    end = check
                    break
                check += 2
            else:
                check += 1
        jp_chunks.append(jp_bytes[off:end].decode('cp932'))
        off = end

    en_chunks = _wrap_en_to_chunks(en, len(jp_chunks))

    pairs = []
    for i in range(len(jp_chunks)):
        en_part = en_chunks[i] if i < len(en_chunks) else ''
        if en_part:
            pairs.append((jp_chunks[i], en_part))

    return pairs if pairs else [(jp, en)]


def _wrap_en_to_chunks(text: str, num_chunks: int) -> list[str]:
    """
    Split English text into `num_chunks` pieces, word-wrapping at
    EN_CHARS_PER_ROW characters.
    """
    words = text.split(' ')
    chunks = []
    current = ''

    for word in words:
        if not current:
            current = word
        elif len(current) + 1 + len(word) <= EN_CHARS_PER_ROW:
            current += ' ' + word
        else:
            chunks.append(current)
            current = word

    if current:
        chunks.append(current)

    # If we have fewer chunks than needed, pad with empty
    while len(chunks) < num_chunks:
        chunks.append('')

    # If we have more chunks than JP rows, merge the extras into the last row
    if len(chunks) > num_chunks:
        merged = ' '.join(chunks[num_chunks - 1:])
        chunks = chunks[:num_chunks - 1] + [merged]

    return chunks


def main():
    map_path = os.path.join(os.path.dirname(__file__), '..', 'translation-map.json')
    out_path = os.path.join(os.path.dirname(__file__), 'dictionary.txt')
    full_path = os.path.join(os.path.dirname(__file__), 'dictionary_full.txt')

    print(f'Reading {map_path}...')
    with open(map_path, 'r', encoding='utf-8') as f:
        tmap = json.load(f)

    print(f'  {len(tmap)} entries in translation map')

    count = 0
    full_count = 0
    skipped = 0

    out = open(out_path, 'w', encoding='cp932', errors='replace')
    full_out = open(full_path, 'w', encoding='cp932', errors='replace')
    out.write('# True Blue English Patch Dictionary (split)\n\n')
    full_out.write('# True Blue Full Entries (for continuation cache)\n\n')

    for jp, en in tmap.items():
        if not jp or not en:
            skipped += 1
            continue

        if jp.startswith('[選択肢') or jp.startswith('  '):
            skipped += 1
            continue
        if jp.startswith('-----') or jp.startswith('*****'):
            skipped += 1
            continue

        jp_clean = jp
        en_clean = en.strip().replace('\n', ' ')

        en_clean = (en_clean
            .replace('\u2014', '-')
            .replace('\u2013', '-')
            .replace('\u2018', "'")
            .replace('\u2019', "'")
            .replace('\u201C', '"')
            .replace('\u201D', '"')
            .replace('\u2026', '...')
            .replace('\u00D7', 'x')
        )

        try:
            jp_clean.encode('cp932')
            en_clean.encode('cp932')
        except UnicodeEncodeError:
            skipped += 1
            continue

        # Write the full (unsplit) entry for continuation cache
        full_out.write(f'{jp_clean}\t{en_clean}\n')
        full_count += 1

        # Write split entries for per-row matching
        pairs = split_entry(jp_clean, en_clean)
        for jp_part, en_part in pairs:
            if not jp_part or not en_part:
                continue
            try:
                jp_part.encode('cp932')
                en_part.encode('cp932')
            except UnicodeEncodeError:
                continue
            out.write(f'{jp_part}\t{en_part}\n')
            count += 1

    out.close()
    full_out.close()

    print(f'  Wrote {count} split entries to {out_path}')
    print(f'  Wrote {full_count} full entries to {full_path}')
    print(f'  Skipped {skipped} entries')
    print(f'\nDone! Copy dictionary.txt AND dictionary_full.txt to the game folder.')


if __name__ == '__main__':
    main()
