# True Blue English Patch (Runtime Hook)

This patch replaces Japanese text with English translations at runtime using a
proxy DLL. No game files are modified — the patch is fully reversible by
deleting the added files.

## How it works

The game dynamically creates `main.bin` (the actual engine) at startup. Our
`winmm.dll` proxy is loaded by the game automatically. It:

1. Forwards all `winmm.dll` API calls to the real system DLL
2. Detects when `main.bin` is loaded into memory
3. Patches `main.bin`'s IAT to redirect `TextOutA` (GDI32) to our hook
4. Looks up each Japanese text line in `dictionary.txt`
5. Replaces matching lines with English translations

## Building

**Requirements**: Visual Studio 2019+ with C++ desktop workload (x86 target)

1. Open a **x86 Native Tools Command Prompt for VS**
2. `cd` to this directory
3. Run `build.bat`
4. Output: `winmm.dll`

## Generating the dictionary

**Requirements**: Python 3

```bash
python generate_dictionary.py
```

This reads `../translation-map.json` and produces `dictionary.txt` and
`dictionary_full.txt`.

## Installation

1. Copy `winmm.dll` to the game folder (same folder as the game .exe)
2. Copy `dictionary.txt` and `dictionary_full.txt` to the game folder
3. Launch the game normally

The patch logs debug messages via `OutputDebugString` — view them with
[DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)
if troubleshooting.

## Uninstallation

Delete `winmm.dll`, `dictionary.txt`, and `dictionary_full.txt` from the
game folder. The game returns to its original Japanese state.

## Technical Notes

- Hook method: IAT patch on `main.bin`'s import of `TextOutA` (GDI32.dll)
- The game is 32-bit (x86), so the DLL must be compiled as 32-bit
- `main.bin` is a packed PE that exists only while the game is running
- The proxy DLL scans memory to find `main.bin`'s base address at runtime

## Known limitations

- **Font rendering**: The game's font must support ASCII characters. If English
  text appears garbled, a font hook (`CreateFontA`) may be needed.
- **Text box width**: Long English lines may overflow. The dictionary generator
  pre-wraps lines at ~38 characters, but some manual adjustment may be needed.
- **Choices**: Choice text replacement depends on how the game passes choice
  strings to the same text function. May need a separate hook.
