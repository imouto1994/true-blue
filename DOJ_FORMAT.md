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
```

> **Key offsets:** marker at `+20`, text at `+22`.

**No-text variant:** When `data[i+20] != 0x01`, the record is a control/flow command
(e.g., sound cue, scene transition). The parser advances past the 2-byte opcode only
and rescans from `i+2`.

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
```

> **Key offsets:** marker at `+18`, text at `+20`.

> Both `0x2A` and `0x22` use this layout; `0x22` was observed only in Form A
> (always `param == 0`). The difference in opcode value may encode speaker type
> (e.g., scene partner vs secondary character) but this has not been confirmed.

---

### 0x2A — Choice Menu (Form B)

Presents the player with one or more selectable dialogue choices. Identified by
`param != 0x0000` (unlike Form A).

```
Byte   Size   Field
----   ----   -----
+0     1      0x2A
+1     1      0x00
+2     2      u16le  flags  (non-zero; exact meaning unknown)
+4     1      u8  number_of_choices
+5     1      0x00
+6     …      number_of_choices × null-terminated Shift-JIS strings (no separator)
```

> **No "has text" marker** — the strings follow directly after `+6`.
> The count field doubles as the record length indicator.

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

## Tooling

| Script | Purpose |
|--------|---------|
| `decode_doj.py` | Production text extractor. Reads decoded `.doj` → UTF-8 `.txt` |
| `analyze_doj.py` | Development tool. Inspects raw binary, confirms opcode layouts, discovers unknown record types |

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
