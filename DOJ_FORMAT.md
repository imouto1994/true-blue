# DOJ Script Format Reference

This document describes the binary format of decoded **Lilim `.doj` script files** —
the dialogue/script bytecode used by Lilim visual novel games. It is intended as a
reference for engineers extending or debugging the extraction toolchain.

---

## Archive Pipeline

The data reaches us through two layers that must be unwrapped in order:

```
doj.dpk
  │  Container archive ("PA" magic, Lilim/DPK format)
  │  → decoded by arc_unpacker's  lilim/dpk  decoder
  │
  └─ *.doj  (one file per scene/chapter)
       │  Compressed script files ("CC" magic, LZSS payload)
       │  → automatically decoded by arc_unpacker's  lilim/doj  decoder
       │    (triggered via dpk's get_linked_formats() → {"lilim/doj", …})
       │
       └─ decoded binary script bytecode  ← this document describes this layer
            written to  doj~.dpk/
```

**The files in `doj~.dpk/` are already fully decoded.** Running `arc_unpacker --dec=lilim/doj`
on them again will fail (no `CC` magic) because they are the *output* of that decoder,
not the input. Use `decode_doj.py` to extract Japanese text from them.

---

## CC-Format (compressed layer) — handled by arc_unpacker

> You don't need to parse this yourself; arc_unpacker does it.
> This is documented for completeness only.

```
Offset  Size  Description
------  ----  -----------
0       2     Magic: "CC" (0x43 0x43)
2       2     u16le  entry_count
4       entry_count × 6 bytes  metadata table (meaning not fully decoded)
4 + entry_count*6     2     Magic: "DD" (0x44 0x44)
+2      2     unknown
+4      4     u32le  compressed_size
+8      4     u32le  original_size
+12     compressed_size bytes  LZSS-compressed payload (via sysd_decompress)
```

The decompressed payload is the decoded script bytecode documented below.

---

## Decoded Script Bytecode Format

The decoded file is a flat stream of **variable-length records**. Each record
begins with a 2-byte opcode field (u16le). The low byte is the meaningful opcode
value; the high byte is always `0x00`.

### Scanning Strategy

The parser performs a linear scan:
- Test each byte as a potential opcode low byte.
- If `data[i+1] == 0x00` AND `data[i]` is a known opcode → process record.
- Otherwise advance by 1 byte and continue.

There is no length prefix or record delimiter beyond the null terminator on text
strings, so correct handling of each known record type (advancing `i` by the right
amount) is essential to stay in sync.

---

## Record Types

### 0x0A — Narration / Inner Monologue

The most common record type. Carries a single line of narration or the protagonist's
internal thoughts. Speaker is implicit (unmarked / default voice).

```
Byte   Size   Field
----   ----   -----
+0     1      Opcode: 0x0A
+1     1      0x00  (high byte of u16 opcode)
+2     2      u16le  param  (varies; high-bit set → non-text command record, skip)
+4     16     Scene / character context flags  (not decoded; purpose unclear)
+20    1      "has text" marker: 0x01 = text follows; any other value = no text
+21    1      0x00  (second byte of marker)
+22    …      Null-terminated Shift-JIS string  (only present when [+20] == 0x01)
               ↓
             [Second-string sub-record — see below]
```

> **Key offsets:** marker at `+20`, text at `+22`.

**No-text variant:** When `data[i+20] != 0x01`, the record is a control/flow command
(e.g., sound cue, scene transition) carrying a null-terminated ASCII resource path
at `+4` (e.g., `"SE137"`, `"bg032a"`). However, a **second-string sub-record with
narration text may still follow** after the resource path's null terminator. The
parser scans forward for null bytes and probes each with the second-string logic.

---

### 0x2A / 0x22 — Character Dialogue (Form A, single line)

Used for a single spoken line attributed to a named character. The character name
is embedded in the Shift-JIS string itself using the game's conventional format,
e.g. `秋人「……うわっ！」`.

```
Byte   Size   Field
----   ----   -----
+0     1      Opcode: 0x2A or 0x22
+1     1      0x00
+2     2      u16le  param == 0x0000  ← distinguishes Form A from Form B
+4     14     Context flags  (not decoded)
+18    1      "has text" marker: 0x01
+19    1      0x00
+20    …      Null-terminated Shift-JIS string
               ↓
             [Second-string sub-record — see below]
```

> **Key offsets:** marker at `+18`, text at `+20`.

> Both `0x2A` and `0x22` use this layout; `0x22` was observed only in Form A
> (always `param == 0`). The difference in opcode value may encode speaker type
> (e.g., scene partner vs secondary character) but this has not been confirmed.

---

### Second-String Sub-Record (continuation text)

After the first text string's null terminator in 0x0A, 0x2A (Form A), and 0x22 records,
an optional **second text line** may follow in the same record. This accounts for roughly
**89% of text records** across all script files and was the source of the "missing
alternate lines" issue.

```
Byte   Size   Field
----   ----   -----
+0     2      u16le  sub_size  (byte distance from here to the next opcode record)
+2     16     Context / padding  (usually all zeros)
+18    1      "has text" marker: 0x01  (anything else → no second text)
+19    1      0x00
+20    …      Shift-JIS text  (NOT null-terminated — bounded by sub_size)
```

> **Key difference from the first string:** the second string is **not null-terminated**.
> It runs right up to the boundary defined by `sub_size`. Using a null-seeking reader
> would overshoot into the next record's opcode byte (typically `0x0A`, which is not
> `0x00`) and then consume the next record's `0x00` sub-byte as the false terminator,
> silently skipping the next record.

> **Read the text as:** `data[pos + 20 : pos + sub_size]`, decoding the raw slice as
> Shift-JIS. If a null byte appears within the slice, truncate there.
>
> **SJIS boundary fix:** the `sub_size` boundary occasionally lands in the middle of a
> 2-byte Shift-JIS character (the last byte of the slice is a lead byte `0x81–0x9F` /
> `0xE0–0xFC` without its trail byte). When detected, the decoder extends the slice by
> 1 byte to include the trail byte, preventing garbled output on the final character.

**When the second text is absent**, the bytes at `+0` may begin either a
resource-load sub-record (e.g., `40 00` followed by an ASCII resource path like
`prg\99vpf00002`) or a **choice sub-record** (see below). The marker at `+18` will
not be `0x01`, so the parser falls through to the choice sub-record check.

---

### Choice Sub-Record (embedded in text records)

After the first text string's null **or** after the second-string sub-record, an
optional **choice menu** may be embedded in the same record. This accounts for
**59 choice menus** across all script files — the majority of player-facing
choices in the game.

These are distinct from the standalone opcode-based choice formats (0x2A Form B,
0x06) documented below.

```
Byte   Size   Field
----   ----   -----
+0     2      u16le  sub_size  (byte distance to the next opcode record)
+2     2–6    Variable header fields  (u16le values; purpose not fully decoded)
+6/+8  …      Null-terminated Shift-JIS choice strings, one after another
```

> **Variable header size:** the choice strings start at either `+6` or `+8` from
> the sub-record start. The parser probes offsets `+6`, `+8`, `+10` for the first
> valid Shift-JIS lead byte.

> **Validation:** a real choice sub-record has **≥ 2 strings containing Japanese
> characters**. The `sub_size` field must point to a valid next-opcode boundary.

**Sub-record chain:** The full chain after a text string's null terminator can be
several levels deep:
```
text1 \0  →  [resource sub-record(s)]  →  [narration sub-record]  →  [choice sub-record]  →  next opcode
              e.g. "40 00 prg\…"            (marker 0x01 at +18)       (no 0x01 marker)
```
Any or all of the resource, narration, and choice sub-records may be absent.

> **Deep chain handling:** When neither narration text nor choices are found at the
> immediate position after text1's null, the parser scans forward for null bytes
> (up to 256 bytes) and probes each with the narration second-string logic. This
> follows chains where resource-path sub-records (image/sound loads) separate text1
> from text2 — recovering text that would otherwise be invisible to the opcode scanner.

> **Boundary alignment:** The narration sub-record's `sub_size` boundary sometimes
> lands on or near the text's null terminator rather than right after it. The parser
> probes positions `end_pos` through `end_pos + 3` when looking for a following
> choice sub-record, to handle this alignment variation.

**Example from `01c04.doj`** (after second narration string "……さて、どうしようかな？"):
```
48 00       ← sub_size = 72
36 00       ← header field (purpose unclear)
2E 00       ← header field
04 00       ← header field (possibly related to number of choices)
81 40 88 A8 82 C6 82 CD 81 41 96 BE 93 FA 82 E0 89 EF 82 A6 82 E9 00
            ^ 「葵とは、明日も会える」\0
81 40 83 68 83 89 83 7D 82 E6 82 E8 82 E0 88 A8 82 AA 91 E5 8E 96 00
            ^ 「ドラマよりも葵が大事」\0
11 00 …    ← control/jump data (within sub_size boundary)
```

---

### 0x2A — Choice Menu (Form B)

Presents the player with selectable dialogue choices. Identified by `param != 0x0000`.
Two structural sub-variants exist:

**Variant 1 — param=0x0022 (count-prefixed):**
```
Byte   Size   Field
----   ----   -----
+0     1      0x2A
+1     1      0x00
+2     2      u16le  0x0022
+4     1      u8  total_string_count  (includes trailing control-byte padding!)
+5     1      0x00
+6     …      total_string_count × null-terminated Shift-JIS strings
```
> The count includes non-choice padding entries (e.g. single bytes like `0x11`). Filter: keep only strings with no control characters (`< 0x20`) and length > 1.

**Variant 2 — param=0x0004 (no count byte):**
```
Byte   Size   Field
----   ----   -----
+0     1      0x2A
+1     1      0x00
+2     2      u16le  0x0004
+4     …      null-terminated Shift-JIS strings directly (no count prefix)
              terminated when a non-text byte (e.g. 0x11) is encountered
```

---

### 0x06 — Primary Choice Menu (main player-facing choices)

The most common choice format in the game. Presents the player with typically 2–3
options in Japanese. **No count byte, no header** — strings start immediately at `+2`.

```
Byte   Size   Field
----   ----   -----
+0     1      0x06
+1     1      0x00
+2     …      null-terminated Shift-JIS strings, one after another
              List ends when a non-text byte is encountered (e.g. 0x15, 0x11)
```

> [!IMPORTANT]
> Opcode `0x06` is **also** used for internal resource-routing blocks that look
> structurally identical but contain only short ASCII labels like `"99a04"`, `"@"`, or
> voice-file path strings (e.g. `"01_01\01v100463"`). These are **not** player-visible.
>
> **Distinguish**: a real choice block has **≥ 2 strings containing Japanese characters**
> (Shift-JIS 2-byte sequences). Routing blocks contain only ASCII strings.

**Example from `06c01.doj`:**
```
06 00  81 40 90 53 94 7A 82 C8 82 CC 82 C5 88 A8 82 F0 92 54 82 B7 00
       ^ ideographic space + 「心配なので葵を探す」\0
       81 40 8B 43 82 C9 82 B9 82 BA 8B 8F 96 B0 82 E8 00
       ^ 「気にせず居眠り」\0
       81 40 82 C6 82 E8 82 A0 82 A6 82 BA ... 00
       ^ 「とりあえずトイレに行く」\0
       15  ← non-text byte: end of list
```

---

## Other Opcodes (not text-bearing)

These opcodes were observed in the wild but carry no Japanese text. They control
game flow, resource loading, and audio/visual state:

| Opcode | Likely purpose          |
|--------|-------------------------|
| `0x11` | Flow control / jump     |
| `0x14` | Resource load (images, animations) |
| `0x33` | Scene-level command     |
| `0x08` | Audio / SE trigger (often followed by ASCII label like `"SE137"`) |
| `0x07` | Animation command       |

The first file-level record in most scripts is a special `0x0A` whose ASCII payload
is a sound effect key (e.g., `"SE137"`), not a Shift-JIS dialogue line. The
`looks_like_text` heuristic in `decode_doj.py` naturally skips these because their
bytes often fall outside normal Shift-JIS ranges.

---

## Shift-JIS Encoding Notes

All text strings are encoded in **Shift-JIS** (CP932). Key properties for correct parsing:

- **Lead bytes** `0x81–0x9F` and `0xE0–0xFC` introduce a 2-byte character; the
  trailing byte must be consumed before checking for `0x00` null terminator.
- **Half-width Katakana** (`0xA1–0xDF`) is a 1-byte character.
- **Ideographic space** `U+3000` → Shift-JIS `0x81 0x40`. It appears at the start
  of most narration lines as an indent.
- Lines ending in `\n` (`0x0A`) are **not** separate records — the `0x0A` here is
  inside the string, not the opcode. The `strip()` call in the decoder removes it.

---

## Text Validation

Decoded strings are filtered before inclusion in the output:

- **Control character rejection:** any string containing bytes `< 0x20` (C0 controls)
  or the Unicode replacement character `U+FFFD` is discarded. This prevents false
  positives from binary data in resource-only files (e.g., `trueblue.doj` which
  contains only animation/resource opcodes and no dialogue).
- **Minimum length:** second-string sub-record text must be at least 2 characters
  to avoid picking up stray single-byte values from record headers.

---

## Tooling

| Script | Purpose |
|--------|---------|
| `decode_doj.py` | Production text extractor. Reads decoded `.doj` → UTF-8 `.txt` |
| `analyze_doj.py` | Development tool. Inspects raw binary, confirms opcode layouts, discovers unknown record types |

### Current extraction stats (152 files)

| Metric | Count |
|--------|-------|
| Text lines (narration + dialogue) | 39,057 |
| Choice menus | 59 |
| Garbled characters (U+FFFD) | 0 |

### Typical workflow

```bash
# Extract doj.dpk (arc_unpacker auto-decodes the CC layer → doj~.dpk/)
arc_unpacker.exe doj.dpk

# Extract all Japanese text from the decoded scripts
python decode_doj.py doj~.dpk/ -o decoded_txt/ -v

# Inspect a specific file's binary structure during format research
python analyze_doj.py   # (edit SAMPLE_FILES inside the script)
```

---

## Extending the Decoder

To support a new text-bearing opcode:

1. **Run `analyze_doj.py`** targeting files where the opcode appears.  
   Confirm the marker offset and text offset from the frequency table.

2. **Add the opcode** to `DIALOGUE_OPCODES` in `decode_doj.py`.

3. **Add a branch** in `decode_doj()` with the correct `MARKER_OFFSET_*` and
   `TEXT_OFFSET_*` constants.

4. **Re-run** on the full `doj~.dpk/` directory and spot-check the new lines
   against the raw hex to confirm alignment.

---

## File Naming Convention

Scripts follow a consistent naming scheme:

| Pattern       | Meaning                                   |
|---------------|-------------------------------------------|
| `01c01.doj`   | Chapter 1, scene 1 (main route)          |
| `d09c03s02.doj` | Day 9, chapter 3, sub-scene 2          |
| `ED01A.doj`   | Ending 1, part A                         |
| `PRG.doj`     | Prologue                                 |
| `t13*.doj`    | Special/bonus scenes                     |
| `STAFF.doj`   | Staff roll (control-only, no dialogue)   |
| `debug.doj`   | Debug/test entry in CC format (not a decoded script) |
