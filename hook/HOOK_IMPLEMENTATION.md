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
| Resolution | 640x480 |
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

### Imported GDI32 Functions (relevant to text)

```
CreateFontA          — Font creation (game creates fonts at init, before our hook)
TextOutA             — TEXT RENDERING FUNCTION (the hook target)
SetTextColor         — Text color
SetBkMode            — Background mode
SetBkColor           — Background color
GetCharABCWidthsA    — Character width queries (used for pixel-based wrapping)
GetTextMetricsA      — Font metrics (we read this to match font height)
```

---

## Text Rendering: TextOutA

The game renders text on screen via **`TextOutA`** (GDI32.dll).

### Calling Pattern

```c
BOOL TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c);
```

- **Full lines** — Each call receives a complete text row (not char-by-char)
- **Shadow rendering** — Each row is drawn TWICE per frame:
  - Shadow: (x-2, y-2) — black text offset
  - Main: (x, y) — colored text on top
- **Every-frame redraw** — Text is rendered ~30fps (same line repeated)
- **Encoding** — Text is CP932 (Shift-JIS) with leading ideographic space `U+3000` = `0x81 0x40`

### Line Wrapping

The game wraps text by **pixel width**, not a fixed character count. For fullwidth
Japanese text, this typically results in ~22-23 characters per row (~44-46 CP932 bytes).
Each row gets its own `TextOutA` call. The exact split point varies per line because
different characters have different pixel widths.

---

## Hook Architecture

### Approach: winmm.dll Proxy + IAT Patch + Font Swap

```
Game .exe
  ├─ loads winmm.dll from game folder (our proxy)
  │    ├─ forwards all 25 winmm functions to real system winmm.dll
  │    ├─ starts HookThread:
  │    │    ├─ loads dictionary.txt → g_dict (pre-split fragments)
  │    │    ├─ loads dictionary_full.txt → g_fullDict (prefix lookup)
  │    │    │                            + g_dict (full entries for direct match)
  │    │    ├─ waits for main.bin to appear in memory
  │    │    └─ patches main.bin's IAT: TextOutA → HookedTextOutA
  │    │
  │    ├─ HookedTextOutA (called for every text draw):
  │    │    ├─ Step 0: shadow/repeat detection (same text → same EN)
  │    │    ├─ Step 1: continuation cache (rows 2+ of multi-row lines)
  │    │    │    ├─ 1a: exact match
  │    │    │    ├─ 1b: forward prefix (game row wider than predicted)
  │    │    │    └─ 1c: reverse prefix (game row narrower than predicted)
  │    │    ├─ Step 2: exact dictionary match (short lines, first-row fragments)
  │    │    │    └─ if 44-byte match + g_fullDict entry → build continuation cache
  │    │    ├─ Step 3: prefix fallback (first row wider than 44 bytes)
  │    │    │    └─ match first 44 bytes in g_fullDict → build continuation cache
  │    │    └─ Step 4: log miss (hex dump for debugging)
  │    │
  │    └─ RenderEnglish: swaps in Arial Narrow font for translated text
  │
  └─ loads main.bin (dynamically, temporary file)
       └─ calls TextOutA → redirected to our hook
```

### main.bin Detection

Since `main.bin` is created at runtime and may not be loadable via `GetModuleHandleA`,
the hook thread scans process memory:

1. Try `GetModuleHandleA("main.bin")` first
2. Fall back to memory scanning via `VirtualQuery`:
   - Walk memory regions from 0x00400000 to 0x10000000
   - Use `SafeProbeModule` (SEH in separate function from C++ objects) to safely
     check for MZ header + PE signature + matching code section size
   - Confirmed base address: `0x00400000`

### IAT Hooking

IAT patching overwrites a function pointer in main.bin's import table:
1. Parse main.bin's PE headers to find the import directory
2. Walk import descriptors to find GDI32.dll
3. Walk the thunk array to find the TextOutA entry
4. Save the original function pointer (`g_origTextOutA`)
5. Overwrite with our `HookedTextOutA` address

### Font Handling

The game creates its fonts via `CreateFontA` during init, before our hook is installed.
Instead of hooking `CreateFontA`, we swap the font at render time:

1. On the first translated TextOutA call, `GetEnglishFont` reads the current font
   metrics (height, weight) from the HDC via `GetTextMetricsA`
2. Creates an Arial Narrow font at **60% of the original character width**
   (`tmAveCharWidth * 3 / 5`), matching the game's font height
3. `RenderEnglish` temporarily selects this font into the HDC before calling
   the original `TextOutA`, then restores the game's font
4. Untranslated Japanese text bypasses `RenderEnglish` and uses the game's
   original font directly

---

## Multi-Row Text Matching

The game wraps long lines by pixel width, producing 2-4+ TextOutA calls per sentence.
Our dictionary pre-splits entries at predicted 44-byte boundaries, but the game's actual
split points vary by +/- a few bytes. The matching logic handles this through a
multi-step fallback chain with cache rebuilding.

### Data Structures

| Structure | Key | Value | Purpose |
|-----------|-----|-------|---------|
| `g_dict` | JP fragment (CP932 bytes) | EN text | Pre-split fragments + full entries |
| `g_fullDict` | First 44 bytes of long JP | `"fullJP\tfullEN"` | Lookup full entry from first-row fragment |
| `g_contCache` | Predicted JP fragment | EN row text | Continuation rows for active multi-row line |
| `g_activeFullJP/EN` | — | Full entry text | Reference for cache rebuilds |
| `g_activeEnRow` | — | EN row counter | Tracks which EN word-wrap row is next |
| `g_lastMatchedJP/EN` | — | Last match pair | Shadow/repeat detection |

### Matching Flow (HookedTextOutA)

**Step 0 — Shadow/Repeat Detection**

The game draws each row twice (shadow + main) and redraws at ~30fps. If the same
text arrives again, return the cached EN immediately without touching any state.

**Step 1a — Exact Continuation Match**

If the text exactly matches a predicted continuation cache key, serve the cached EN.
Increment `g_activeEnRow` to track which EN row we're on. No cache rebuild needed
(alignment is correct).

**Step 1b — Forward Prefix Match (wider game row)**

The game sent more bytes than the predicted cache key. The cache key is a prefix
of the received text. This happens when the game fits one extra character on the row.
Serve the cache key's EN, combining with the next cache entry's EN if the extra bytes
match. Rebuild the cache from the actual next byte offset with the advanced EN row index.

**Step 1c — Reverse Prefix Match (narrower game row)**

The game sent fewer bytes than the predicted cache key. The received text is a prefix
of the cache key. Serve the cache key's EN. Rebuild from actual next offset.

**Step 2 — Exact Dictionary Match**

Handles single-row lines (<=44 bytes) and predicted first-row fragments (exactly 44 bytes).
When a 44-byte match is found and `g_fullDict` has the full entry, build the continuation
cache for subsequent rows.

**Step 3 — Prefix Fallback (wider first row)**

The game's first row is wider than 44 bytes (e.g., 46). Take the first 44 bytes and
look them up in `g_fullDict`. If the full entry starts with the received text, serve
EN row 1 and build the continuation cache from the actual row 1 size.

**Step 4 — Miss Logging**

Log unmatched SJIS text with hex dump and raw bytes to `patch_log.txt` for debugging.

### Cache Rebuild on Alignment Shift

When a fuzzy match (step 1b/1c) detects that the game split at a different boundary
than predicted, the continuation cache is rebuilt from the actual byte offset where
the next row begins. The EN row counter (`g_activeEnRow`) is passed through so the
rebuild assigns the correct EN word-wrap rows (not duplicating earlier rows).

---

## Dictionary Format

### File: dictionary.txt

Tab-separated values, CP932 encoded. Contains pre-split row fragments:

```
{Japanese fragment in CP932}\t{English text}
```

Lines starting with `#` are comments. For multi-row entries, the JP text is split
at 44-byte boundaries (respecting SJIS character boundaries), and the EN text is
word-wrapped at 92 characters per row.

### File: dictionary_full.txt

Tab-separated values, CP932 encoded. Contains full (unsplit) entries:

```
{Full Japanese text in CP932}\t{Full English text}
```

Used at runtime to build the continuation cache and for direct matching when the
game sends a full line without splitting.

### Generation: generate_dictionary.py

Reads `translation-map.json` (Unicode JP->EN mapping) and writes both dictionary files:

1. Skips meta-lines (choice labels, file headers, separators)
2. Replaces Unicode characters that CP932 can't encode:
   - `U+2014` (em dash) -> `-`, `U+2013` (en dash) -> `-`
   - `U+2018/U+2019` (curly quotes) -> `'`
   - `U+201C/U+201D` (curly double quotes) -> `"`
   - `U+2026` (ellipsis) -> `...`, `U+00D7` (multiplication) -> `x`
3. Splits JP text at 44-byte CP932 boundaries (SJIS-aware, matching C++ logic)
4. Word-wraps EN text at 92 characters per row
5. Reports overflow (EN needs more rows than JP) to `overflow_report.txt`

### Critical Encoding Note

**Always use `cp932`**, never `shift-jis`, for encoding/decoding. Python's `shift-jis`
codec is the strict JIS standard, while `cp932` is Microsoft's superset.

### Stats

| Metric | Count |
|--------|-------|
| translation-map.json entries | 31,623 |
| dictionary.txt entries (split fragments) | ~34K |
| dictionary_full.txt entries (full) | ~31K |
| Skipped (meta-lines + encoding failures) | 140 |

---

## Confirmed Working

- [x] Proxy DLL loads successfully
- [x] main.bin detected at 0x00400000
- [x] IAT hook installed on TextOutA
- [x] TextOutA receives full rows in CP932
- [x] Dictionary lookup matches single-row lines
- [x] Multi-row continuation matching (exact, forward prefix, reverse prefix)
- [x] Cache rebuild on alignment shift preserves correct EN row assignment
- [x] Shadow/repeat detection prevents cache corruption
- [x] English text rendered with Arial Narrow at 60% width
- [x] Empty EN continuations prevent fragment collision (e.g., `た。` matching wrong entry)
- [x] Full entries loaded into g_dict for direct matching

## Known Limitations

### 1. Pixel-Based Wrapping Variance

The game wraps by pixel width, so the exact row boundary varies per line. The hook
handles +/- a few bytes via fuzzy matching, but extreme variance could still miss.
The miss log (`patch_log.txt`) captures any unmatched text with hex for investigation.

### 2. Fragment Collision

Common JP fragments like `た。`, `る。`, `い。` appear as row-2 remnants of many
different sentences. The dictionary can only store one EN per JP key. The continuation
cache takes priority over `g_dict` to prevent wrong matches, and empty EN continuations
are stored to block fallthrough.

### 3. Font Availability

Arial Narrow ships with Microsoft Office but may not be on bare Windows installations.
If absent, `CreateFontA` returns NULL and `RenderEnglish` falls back to the game's
original font (text will overflow).

---

## Failed Approaches (for reference)

### Inline Hook at main.bin+0x33794

- **What**: 5-byte JMP patch replacing `shr ecx,2 / rep movsd`
- **Why it failed**: The function fires during script LOADING (copying resource paths,
  opcodes), not during text DISPLAY. After initialization, it stops firing.
- **Lesson**: LunaTranslator's text extraction works from the loading phase, but text
  REPLACEMENT needs to target the rendering phase.

### CreateFontA IAT Hook

- **What**: Hook `CreateFontA` to change the font face at creation time
- **Why it failed**: The game creates fonts during `main.bin` init, before our hook
  thread gets to patch the IAT. By the time we hook, fonts are already created.
- **Solution**: Swap fonts at render time in `RenderEnglish` via `SelectObject`.

### Fixed Character-Count Wrapping

- **What**: Assume the game always wraps at exactly 22 fullwidth characters (44 bytes)
- **Why it failed**: The game wraps by pixel width, producing rows of 44-46 bytes
  depending on character widths. Fixed predictions miss by 1-2 characters.
- **Solution**: Multi-step fuzzy matching (forward/reverse prefix) with cache rebuild.

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
# Writes dictionary.txt, dictionary_full.txt, overflow_report.txt
```

### Install

1. Copy `winmm.dll` to game folder (same folder as game .exe)
2. Copy `dictionary.txt` and `dictionary_full.txt` to game folder
3. Launch game normally

### Uninstall

Delete `winmm.dll`, `dictionary.txt`, and `dictionary_full.txt` from game folder.

### Debug

- **DebugView** (Sysinternals): Shows `[TrueBluePatch]` messages for hook status
- **patch_log.txt** (game folder): Hex dump of unmatched TextOutA calls

---

## File Inventory

```
hook/
  winmm_proxy.cpp        — Main source: proxy DLL + IAT hook + font swap + dictionary lookup
  winmm.def              — Module definition (export name mapping)
  build.bat              — Build script for MSVC x86
  generate_dictionary.py — Converts translation-map.json -> dictionary.txt + dictionary_full.txt
  dictionary.txt         — Generated split JP->EN lookup (CP932 TSV)
  dictionary_full.txt    — Generated full JP->EN entries (CP932 TSV)
  overflow_report.txt    — Generated report of EN translations needing more rows than JP
  README.md              — User-facing install instructions
  HOOK_IMPLEMENTATION.md — This file (technical reference)
```
