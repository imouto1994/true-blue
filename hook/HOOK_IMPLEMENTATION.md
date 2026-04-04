# True Blue English Patch — Hook Implementation Details

This document captures all technical findings, implementation decisions, and debugging
results from building the runtime hook English patch for True Blue (LiLiM DARKNESS, 2002).
It serves as a knowledge base for any future agent continuing this work.

---

## Game Technical Profile

| Property | Value |
|----------|-------|
| Title | TRUE BLUE (LiLiM DARKNESS) |
| Release | 2002-09-13 |
| Resolution | 640×480 |
| Architecture | 32-bit x86 Windows |
| Engine | Sysd (Lilim's pre-AOS2 engine) |
| Script format | DOJ bytecode in DPK archives (see `DOJ_FORMAT.md`) |
| Text encoding | CP932 (Microsoft Shift-JIS) |

## Game Executable Structure

The game uses a **two-stage loader**:

1. **Game .exe** — A launcher/loader stub
2. **main.bin** — The actual game engine, created as a **temporary file** at runtime

`main.bin` is extracted/unpacked when the game starts and **deleted when the game closes**.
It is a standard 32-bit PE executable (MZ header, 792 KB).

### main.bin PE Analysis

```
Image base:     0x00400000
Sections:       .text (0x1000, 0x57000), .rdata, .data, .rsrc
Type:           EXE (not DLL)
Machine:        x86 (32-bit)
```

### Imported DLLs (from main.bin)

```
DSOUND.dll      — DirectSound (ordinal 1 only)
WINMM.dll       — Multimedia timers, MIDI, mixer ← PROXY TARGET
COMCTL32.dll    — Common controls
MSACM32.dll     — Audio compression
KERNEL32.dll    — Core OS
USER32.dll      — Window management
GDI32.dll       — Graphics (includes TextOutA) ← TEXT RENDERING
ADVAPI32.dll    — Registry/security
```

### Imported WINMM Functions (25 total)

```
mmioOpenA, timeBeginPeriod, timeSetEvent, timeKillEvent, timeEndPeriod,
mmioGetInfo, mmioAdvance, mmioSetInfo, mmioSeek, mmioDescend, mmioRead,
mmioAscend, mmioClose, mixerGetNumDevs, mixerOpen, mixerGetDevCapsA,
mixerGetLineInfoA, mixerGetLineControlsA, mixerSetControlDetails,
mixerGetControlDetailsA, mixerClose, midiOutGetNumDevs, mciSendCommandA,
mciGetErrorStringA, timeGetTime
```

### Imported GDI32 Functions (relevant to text)

```
CreateFontA          — Font creation
TextOutA             — TEXT RENDERING FUNCTION (the hook target)
SetTextColor         — Text color
SetBkMode            — Background mode
SetBkColor           — Background color
GetCharABCWidthsA    — Character width queries
GetTextMetricsA      — Font metrics
```

---

## LunaTranslator Hook Code

LunaTranslator successfully extracts text from the game using:

```
Sysd HSXN12+-1C:8@33794:main.bin 0:4468c0
```

### H-code Breakdown

- **Sysd** — Engine identifier (Sysd/Lilim engine)
- **@33794** — Hook address: offset `0x33794` in `main.bin`
- **:main.bin** — Module name
- **0:4468c0** — Additional context (possibly absolute address when base=0x413000)

### Function at 0x33794

This is a **generic string copy function** (memcpy with Shift-JIS awareness):

```asm
; At 0x33794:
C1 E9 02    shr ecx, 2      ; dword count = byte_count / 4
F3 A5       rep movsd        ; copy dwords (ESI → EDI)
8B CA       mov ecx, edx
83 E1 03    and ecx, 3
F3 A4       rep movsb        ; copy remaining bytes
5F          pop edi
B8 01 00 00 00  mov eax, 1
5E          pop esi
C2 08 00    ret 8            ; stdcall, 2 args
```

**Important finding**: This function is used during **script loading** (copies resource
paths, opcode data, etc.) but does NOT fire during **dialogue display**. LunaTranslator
extracts text from the loading phase using the H-code's offset parameters (`12+-1C:8`)
to read the text from a specific stack location.

**This function is NOT suitable for text replacement** — it fires during init only,
and modifying ESI at this point would corrupt the script loading, not change displayed text.

---

## Text Rendering: TextOutA

The game renders text on screen via **`TextOutA`** (GDI32.dll).

### Calling Pattern

```c
BOOL TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c);
```

- **Full lines** — Each call receives a complete text line (not char-by-char)
- **Shadow rendering** — Each line is drawn TWICE per frame:
  - Shadow: (x=20, y=28) — black text offset
  - Main:   (x=22, y=30) — colored text on top
- **Every-frame redraw** — Text is rendered ~30fps (same line repeated)
- **Encoding** — Text is CP932 (Shift-JIS) with leading ideographic space `U+3000` = `0x81 0x40`

### Example TextOutA Call

```
TextOut (len=16, x=22, y=30): [　――都内某所。]
Hex: 81 40 81 5C 81 5C 93 73 93 E0 96 5E 8F 8A 81 42
```

---

## Hook Architecture

### Approach: winmm.dll Proxy + IAT Patch

```
Game .exe
  ├─ loads winmm.dll from game folder (our proxy)
  │    ├─ forwards all 25 winmm functions to real system winmm.dll
  │    ├─ starts HookThread
  │    │    ├─ loads dictionary.txt (JP→EN translation map)
  │    │    ├─ waits for main.bin to appear in memory
  │    │    ├─ patches main.bin's IAT entry for TextOutA
  │    │    └─ redirects TextOutA → HookedTextOutA
  │    └─ HookedTextOutA:
  │         ├─ receives (hdc, x, y, lpString, c)
  │         ├─ builds std::string from (lpString, c)
  │         ├─ looks up in g_dict (unordered_map)
  │         ├─ if found: calls real TextOutA with English text
  │         └─ if not found: calls real TextOutA with original Japanese
  └─ loads main.bin (dynamically, temporary file)
       └─ calls TextOutA → redirected to our hook
```

### main.bin Detection

Since `main.bin` is created at runtime and may not be loadable via `GetModuleHandleA`,
the hook thread scans process memory:

1. Try `GetModuleHandleA("main.bin")` first
2. Fall back to memory scanning:
   - Walk memory regions from 0x00400000 to 0x10000000
   - Look for MZ header + PE signature + matching section sizes
   - Confirmed base address: `0x00400000`

### IAT Hooking (vs Inline Hooking)

**IAT patching** was chosen over inline hooking because:
- Simpler: just overwrite a function pointer in main.bin's import table
- Safer: no risk of corrupting mid-function code
- TextOutA is imported by name from GDI32.dll

The `HookIAT` function:
1. Parses main.bin's PE headers to find the import directory
2. Walks import descriptors to find GDI32.dll
3. Walks the thunk array to find the TextOutA entry
4. Saves the original function pointer
5. Overwrites with our `HookedTextOutA` address

---

## Dictionary Format

### File: dictionary.txt

Tab-separated values, CP932 encoded:

```
{Japanese text in CP932}\t{English text in CP932-safe ASCII}
```

Lines starting with `#` are comments.

### Generation: generate_dictionary.py

Reads `translation-map.json` (Unicode JP→EN mapping) and writes `dictionary.txt`:

1. Skips meta-lines (choice labels, file headers, separators)
2. Replaces Unicode characters that CP932 can't encode:
   - `U+2014` (em dash) → `-`
   - `U+2013` (en dash) → `-`
   - `U+2018/U+2019` (curly quotes) → `'`
   - `U+201C/U+201D` (curly double quotes) → `"`
   - `U+2026` (ellipsis) → `...`
3. Encodes both sides as CP932 (NOT `shift-jis` — different codec!)
4. No line wrapping (wrapping inserts `\n` which breaks TSV parsing)

### Critical Encoding Note

**Always use `cp932`**, never `shift-jis`**, for encoding/decoding. Python's `shift-jis`
codec is the strict JIS standard, while `cp932` is Microsoft's superset. The game uses
CP932. Characters that differ:

| Bytes | shift-jis | cp932 |
|-------|-----------|-------|
| 81 60 | U+301C (〜) | U+FF5E (～) |
| 81 5C | U+2015 (―) | U+2015 (―) |
| 81 61 | U+2016 (‖) | U+2225 (∥) |
| 81 7C | U+2212 (−) | U+FF0D (－) |

The em dash issue (`U+2014` in English translations → can't encode in CP932) caused
~400 dictionary entries to be silently dropped until fixed.

### Stats

| Metric | Count |
|--------|-------|
| translation-map.json entries | 31,623 |
| dictionary.txt entries (after filtering) | 31,483 |
| Skipped (meta-lines + encoding failures) | 140 |

---

## Confirmed Working

- [x] Proxy DLL loads successfully
- [x] main.bin detected at 0x00400000
- [x] IAT hook installed on TextOutA
- [x] TextOutA receives full lines in CP932
- [x] Dictionary lookup matches (e.g., `少女「……っ」` → `Girl: "……ngh"`)
- [x] English text rendered in-game (TextOutA draws it)

## Line Splitting (key finding)

The game wraps text at **22 fullwidth characters** (44 bytes) per visual row.
Each row gets its own `TextOutA` call. Long sentences are split into multiple rows.

Example: "　突然の灯された光に、少女は眩しそうに表情を歪めた。" (26 chars) becomes:
- Row 1 (y=30): "　突然の灯された光に、少女は眩しそうに表情を" (22 chars, 44 bytes)
- Row 2 (y=58): "歪めた。" (4 chars, 8 bytes)

The dictionary has the full sentence, so the generator **pre-splits** long entries
into row-sized fragments. Additionally, `dictionary_full.txt` stores the unsplit
entries so the hook can build a **continuation cache** at runtime: when row 1 matches,
it predicts what rows 2+ will look like and caches their translations.

## Dictionary Files

| File | Content | Entries |
|------|---------|--------|
| `dictionary.txt` | Split per-row fragments (CP932 TSV) | ~47K |
| `dictionary_full.txt` | Full unsplit entries (CP932 TSV) | ~31K |

## Known Issues (to fix)

### 1. Text Overflow / Clipping

English text is often longer than Japanese. The game's text box has a fixed width.
Long English lines get clipped at the right edge.  English rows are wrapped at
~44 ASCII characters to approximate the same visual width as 22 SJIS characters.

### 2. Continuation Row Accuracy

The continuation cache predicts what the game's row 2+ fragments will be by
splitting the full JP text at 22-char boundaries. If the game uses slightly
different wrapping (e.g., avoiding mid-word breaks for certain character types),
the prediction may not match exactly.

### 3. Every-Frame Overhead

The dictionary lookup runs ~60 times per second per visible line (30fps × 2
for shadow). With 47K entries in an `unordered_map` plus the continuation cache,
each lookup is O(1) amortized.  Performance has not been an issue in testing.

---

## Failed Approaches (for reference)

### Inline Hook at main.bin+0x33794

- **What**: 5-byte JMP patch replacing `shr ecx,2 / rep movsd`
- **Why it failed**: The function fires during script LOADING (copying resource paths,
  opcodes), not during text DISPLAY. After initialization, it stops firing.
- **Lesson**: LunaTranslator's text extraction works from the loading phase, but text
  REPLACEMENT needs to target the rendering phase.

### HOOK_Lilim (ssynn's tool)

- **What**: AOS2 engine inline hook for Chinese translation
- **Why it doesn't apply**: Only supports AOS2 (2012+). True Blue is 2002 / pre-AOS2.

---

## Build Instructions

### Requirements

- Visual Studio 2019/2022/2026 with **C++ desktop development** workload
- Python 3 (for dictionary generation)

### Build

```cmd
:: Open x86 Native Tools Command Prompt for VS
cd hook
build.bat
:: Output: winmm.dll
```

### Generate Dictionary

```bash
python generate_dictionary.py
# Reads ../translation-map.json
# Writes dictionary.txt
```

### Install

1. Copy `winmm.dll` to game folder (same folder as game .exe)
2. Copy `dictionary.txt` to game folder
3. Launch game normally

### Uninstall

Delete `winmm.dll` and `dictionary.txt` from game folder.

### Debug

- **DebugView** (Sysinternals): Shows `[TrueBluePatch]` messages for hook status
- **patch_log.txt** (game folder): Detailed TextOutA call log with match/miss info

---

## File Inventory

```
hook/
  winmm_proxy.cpp      — Main source: proxy DLL + IAT hook + dictionary lookup
  winmm.def            — Module definition (export name mapping)
  build.bat            — Build script for MSVC x86
  generate_dictionary.py — Converts translation-map.json → dictionary.txt
  dictionary.txt       — Generated JP→EN lookup (CP932 TSV)
  README.md            — User-facing install instructions
  HOOK_IMPLEMENTATION.md — This file (technical reference)
```

---

## Next Steps

1. **Fix remaining dictionary misses** — Compare runtime text (from patch_log.txt)
   against dictionary keys to find encoding/formatting mismatches
2. **Handle text overflow** — Either pre-wrap English text intelligently or hook
   the font/layout to accommodate longer text
3. **Test extensively** — Play through the full game checking all routes
4. **Choice text** — Verify that choice menu text also goes through TextOutA
5. **Performance** — Profile the dictionary lookup under heavy rendering
