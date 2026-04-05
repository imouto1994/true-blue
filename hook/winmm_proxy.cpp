// winmm_proxy.cpp - Proxy DLL for True Blue English patch
// Hooks TextOutA via IAT patching to replace Japanese text with English at render time.
// Uses Arial Narrow font for English text to fit more characters per row.
// Build: cl /LD /O2 /EHsc /D_CRT_SECURE_NO_WARNINGS winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def /MACHINE:X86 user32.lib gdi32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>

// =============================================================================
// Translation Dictionary
// =============================================================================
//
// Three lookup structures work together:
//
// g_dict: JP fragment (CP932 bytes) -> EN text
//   Contains both pre-split row fragments from dictionary.txt AND full unsplit
//   entries from dictionary_full.txt. Pre-split fragments are keyed by the
//   predicted 44-byte row boundaries. Full entries handle cases where the game
//   sends the entire text in one TextOutA call.
//
// g_fullDict: first 44 bytes of a long JP entry -> "fullJP\tfullEN"
//   Used to look up the complete entry when only the first row (44 bytes) is
//   matched. Enables building the continuation cache for subsequent rows.
//
// g_contCache: predicted JP fragment -> EN text for that row
//   Built at runtime when a first-row match is found. Caches the predicted
//   JP fragments for rows 2+ of the current multi-row line, along with
//   their corresponding EN text from the word-wrapped full translation.

static std::unordered_map<std::string, std::string> g_dict;
static std::unordered_map<std::string, std::string> g_fullDict;
static bool g_dictLoaded = false;

static const int JP_CHARS_PER_ROW = 22;  // game wraps at ~22 fullwidth chars
static const int JP_BYTES_PER_ROW = 44;  // 22 * 2 bytes per CP932 fullwidth char
static const int EN_CHARS_PER_ROW = 92;  // Arial Narrow at 60% width fits ~92 chars

static void LoadDictionary() {
    std::ifstream f("dictionary.txt");
    if (!f.is_open()) {
        MessageBoxA(nullptr, "dictionary.txt not found!\nPlace it in the game folder.",
                    "True Blue EN Patch", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string jp = line.substr(0, tab);
        std::string en = line.substr(tab + 1);
        if (!jp.empty() && !en.empty()) {
            g_dict[jp] = en;
            count++;
        }
    }

    g_dictLoaded = true;
    char msg[128];
    sprintf_s(msg, "[TrueBluePatch] Loaded %d translation entries.", count);
    OutputDebugStringA(msg);
}

// =============================================================================
// Continuation Cache
// =============================================================================
//
// When a multi-row JP line is being rendered, the game sends each row as a
// separate TextOutA call. The continuation cache predicts what those rows
// will look like and maps them to the corresponding EN text.
//
// The cache is rebuilt when alignment shifts (the game wraps at a different
// byte boundary than predicted). Key state variables:
//
// g_activeFullJP/EN: the full entry currently being rendered across rows
// g_activeEnRow:     which EN word-wrap row to assign next (incremented per row)
// g_lastMatchedJP/EN: the most recently matched JP/EN pair, used to handle
//                     shadow rendering (each row drawn twice) and 30fps redraws

static std::unordered_map<std::string, std::string> g_contCache;
static std::string g_activeFullJP;
static std::string g_activeFullEN;
static int g_activeEnRow;
static std::string g_lastMatchedJP;
static std::string g_lastMatchedEN;

// Builds the continuation cache for rows after `firstRowBytes`.
// The full EN text is word-wrapped at EN_CHARS_PER_ROW, and each EN row
// is paired with the corresponding predicted JP fragment (at 44-byte boundaries).
// `enRowStart` indicates which EN row index to begin assigning from, so that
// rebuilds after fuzzy matches don't repeat earlier EN rows.
static void BuildContinuationCache(const std::string& fullJP,
                                   const std::string& fullEN,
                                   int firstRowBytes = JP_BYTES_PER_ROW,
                                   int enRowStart = 1) {
    g_contCache.clear();
    g_activeFullJP = fullJP;
    g_activeFullEN = fullEN;
    g_activeEnRow = enRowStart;

    if ((int)fullJP.size() <= firstRowBytes) return;

    // Word-wrap full EN into rows of EN_CHARS_PER_ROW characters.
    // Breaks at word boundaries (last space before the limit).
    std::vector<std::string> enRows;
    {
        int off = 0, len = (int)fullEN.size();
        while (off < len) {
            int end = off + EN_CHARS_PER_ROW;
            if (end >= len) { enRows.push_back(fullEN.substr(off)); break; }
            int lastSpace = -1;
            for (int i = end; i > off; i--)
                if (fullEN[i] == ' ') { lastSpace = i; break; }
            if (lastSpace > off) {
                enRows.push_back(fullEN.substr(off, lastSpace - off));
                off = lastSpace + 1;
            } else {
                enRows.push_back(fullEN.substr(off, EN_CHARS_PER_ROW));
                off += EN_CHARS_PER_ROW;
            }
        }
    }

    // Split remaining JP (from firstRowBytes onwards) into predicted fragments
    // of JP_BYTES_PER_ROW bytes, respecting 2-byte SJIS character boundaries.
    // Each fragment is paired with the corresponding EN row.
    int jpOff = firstRowBytes;
    int enIdx = enRowStart;
    while (jpOff < (int)fullJP.size()) {
        int jpEnd = jpOff + JP_BYTES_PER_ROW;
        if (jpEnd > (int)fullJP.size()) jpEnd = (int)fullJP.size();
        // Walk forward to ensure we don't split mid-SJIS character
        int check = jpOff;
        while (check < jpEnd) {
            unsigned char b = (unsigned char)fullJP[check];
            if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
                if (check + 2 > jpEnd) { jpEnd = check; break; }
                check += 2;
            } else {
                check += 1;
            }
        }
        std::string jpFrag = fullJP.substr(jpOff, jpEnd - jpOff);
        std::string enFrag = (enIdx < (int)enRows.size()) ? enRows[enIdx] : "";
        // Store even empty EN -- prevents fallthrough to g_dict where a
        // fragment like "た。" could match the wrong entry from another sentence.
        if (!jpFrag.empty())
            g_contCache[jpFrag] = enFrag;
        jpOff = jpEnd;
        enIdx++;
    }
}

// =============================================================================
// English Font
// =============================================================================
//
// Created lazily on the first translated TextOutA call. Reads the game's
// current font height/weight via GetTextMetrics, then creates an Arial Narrow
// font at 60% of the original character width. This gives much narrower
// English glyphs so more text fits per row (~92 chars vs ~22 fullwidth JP chars).

static HFONT g_enFont = nullptr;

static HFONT GetEnglishFont(HDC hdc) {
    if (g_enFont) return g_enFont;

    TEXTMETRICA tm;
    if (GetTextMetricsA(hdc, &tm)) {
        g_enFont = CreateFontA(
            tm.tmHeight, tm.tmAveCharWidth * 3 / 5, 0, 0, tm.tmWeight,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS,
            "Arial Narrow");
    }
    return g_enFont;
}

// =============================================================================
// TextOutA Hook
// =============================================================================

typedef BOOL (WINAPI *TextOutA_t)(HDC, int, int, LPCSTR, int);
static TextOutA_t g_origTextOutA = nullptr;

static FILE* g_logFile = nullptr;
static int g_logCount = 0;

// Renders English text by temporarily selecting the Arial Narrow font into
// the device context, calling the original TextOutA, then restoring the
// game's original font. Untranslated JP text bypasses this and uses the
// game's font directly via g_origTextOutA.
static BOOL RenderEnglish(HDC hdc, int x, int y, LPCSTR str, int len) {
    HFONT enFont = GetEnglishFont(hdc);
    if (enFont) {
        HFONT oldFont = (HFONT)SelectObject(hdc, enFont);
        BOOL result = g_origTextOutA(hdc, x, y, str, len);
        SelectObject(hdc, oldFont);
        return result;
    }
    return g_origTextOutA(hdc, x, y, str, len);
}

static void LogToFile(const char* msg) {
    if (!g_logFile) {
        g_logFile = fopen("patch_log.txt", "w");
        if (!g_logFile) return;
    }
    fprintf(g_logFile, "%s\n", msg);
    fflush(g_logFile);
}

// Main hook function. Intercepts every TextOutA call from main.bin.
// The matching logic handles variable-width game wrapping through a
// multi-step fallback chain:
//
//   Step 0: Shadow/repeat detection (same text as last match)
//   Step 1a: Exact continuation cache match
//   Step 1b: Forward prefix match on cache (game row wider than predicted)
//   Step 1c: Reverse prefix match on cache (game row narrower than predicted)
//   Step 2: Exact dictionary match (short lines or predicted first-row fragments)
//   Step 3: Prefix fallback (first row wider than predicted 44 bytes)
//   Step 4: Miss logging
//
// The game wraps text by pixel width, not a fixed character count, so the
// actual row boundaries vary by +/- a few bytes from our 44-byte prediction.
// Steps 1b, 1c, and 3 handle this variance. When a fuzzy match detects
// misalignment, the continuation cache is rebuilt from the actual byte offset.
BOOL WINAPI HookedTextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) {
    if (!g_dictLoaded || !lpString || c <= 0)
        return g_origTextOutA(hdc, x, y, lpString, c);

    std::string text(lpString, c);

    // --- Step 0: Shadow/repeat detection ---
    // The game draws each row twice per frame (shadow pass at offset -2,-2,
    // then main pass). It also redraws every frame at ~30fps. If the same
    // text arrives again, serve the cached result without touching the
    // continuation cache state.
    if (text == g_lastMatchedJP) {
        return RenderEnglish(hdc, x, y,
                             g_lastMatchedEN.c_str(), (int)g_lastMatchedEN.size());
    }

    // --- Step 1: Continuation cache (rows 2+ of a multi-row line) ---
    if (!g_contCache.empty() && !g_activeFullJP.empty()) {

        // Step 1a: Exact match -- the game split at the same boundary we predicted.
        auto cont = g_contCache.find(text);
        if (cont != g_contCache.end()) {
            g_activeEnRow++;
            g_lastMatchedJP = text;
            g_lastMatchedEN = cont->second;
            return RenderEnglish(hdc, x, y,
                                 g_lastMatchedEN.c_str(), (int)g_lastMatchedEN.size());
        }

        // Step 1b/1c: Fuzzy match -- the game split at a different boundary.
        // 1b (forward): game sent MORE bytes than the predicted cache key.
        //     The cache key is a prefix of the received text. This happens when
        //     the game merges what we predicted as two rows into one wider row.
        //     We combine EN from both predicted rows.
        // 1c (reverse): game sent FEWER bytes than the predicted cache key.
        //     The received text is a prefix of the cache key. This happens when
        //     the game wraps narrower than our prediction.
        // In both cases, the alignment has shifted, so we rebuild the cache
        // from the actual next byte offset and advance the EN row counter.
        for (auto& kv : g_contCache) {
            bool forwardHit = ((int)text.size() > (int)kv.first.size() &&
                               text.compare(0, kv.first.size(), kv.first) == 0);
            bool reverseHit = ((int)kv.first.size() > (int)text.size() &&
                               kv.first.compare(0, text.size(), text) == 0);
            if (forwardHit || reverseHit) {
                std::string en = kv.second;
                if (forwardHit) {
                    // Check if the extra bytes match a subsequent cache entry
                    std::string remaining = text.substr(kv.first.size());
                    auto nextIt = g_contCache.find(remaining);
                    if (nextIt != g_contCache.end() && !nextIt->second.empty()) {
                        if (!en.empty()) en += " ";
                        en += nextIt->second;
                    }
                }
                g_activeEnRow++;
                g_lastMatchedJP = text;
                g_lastMatchedEN = en;
                // Rebuild cache from actual next offset with correct EN row index
                size_t pos = g_activeFullJP.find(kv.first);
                if (pos != std::string::npos) {
                    int nextOff = (int)pos + c;
                    if (nextOff < (int)g_activeFullJP.size())
                        BuildContinuationCache(g_activeFullJP, g_activeFullEN,
                                               nextOff, g_activeEnRow);
                }
                return RenderEnglish(hdc, x, y, en.c_str(), (int)en.size());
            }
        }
    }

    // --- Step 2: Exact dictionary match ---
    // Handles single-row lines (<=44 bytes) and predicted first-row fragments
    // of multi-row lines (exactly 44 bytes). When a 44-byte match is found
    // and g_fullDict has the corresponding full entry, we build the continuation
    // cache for the subsequent rows.
    auto it = g_dict.find(text);
    if (it != g_dict.end()) {
        const std::string& en = it->second;

        if (c == JP_BYTES_PER_ROW) {
            auto fullIt = g_fullDict.find(text);
            if (fullIt != g_fullDict.end()) {
                size_t tab = fullIt->second.find('\t');
                if (tab != std::string::npos) {
                    std::string fullJP = fullIt->second.substr(0, tab);
                    std::string fullEN = fullIt->second.substr(tab + 1);
                    BuildContinuationCache(fullJP, fullEN);
                }
            }
        }

        g_lastMatchedJP = text;
        g_lastMatchedEN = en;
        return RenderEnglish(hdc, x, y, en.c_str(), (int)en.size());
    }

    // --- Step 3: Prefix fallback for wider first rows ---
    // The game wraps by pixel width, so some first rows are wider than 44 bytes
    // (e.g., 46 bytes = 23 fullwidth chars). We check if the first 44 bytes
    // match a g_fullDict entry, verify the full JP starts with the received text,
    // then serve EN row 1 and build the continuation cache from the actual offset.
    if (c > JP_BYTES_PER_ROW) {
        std::string prefix = text.substr(0, JP_BYTES_PER_ROW);
        auto fullIt = g_fullDict.find(prefix);
        if (fullIt != g_fullDict.end()) {
            size_t tab = fullIt->second.find('\t');
            if (tab != std::string::npos) {
                std::string fullJP = fullIt->second.substr(0, tab);
                std::string fullEN = fullIt->second.substr(tab + 1);
                if ((int)fullJP.size() >= c &&
                    fullJP.compare(0, c, text) == 0) {
                    auto dictIt = g_dict.find(prefix);
                    std::string enRow1;
                    if (dictIt != g_dict.end()) {
                        enRow1 = dictIt->second;
                    } else {
                        enRow1 = fullEN;
                    }

                    // Build cache from actual row 1 size, not predicted 44
                    BuildContinuationCache(fullJP, fullEN, c);

                    g_lastMatchedJP = text;
                    g_lastMatchedEN = enRow1;
                    return RenderEnglish(hdc, x, y,
                                        enRow1.c_str(), (int)enRow1.size());
                }
            }
        }
    }

    // --- Step 4: Miss logging ---
    // Logs unmatched SJIS text with hex dump for debugging. Only logs
    // the first 50 misses to avoid flooding the log file.
    unsigned char first = (unsigned char)lpString[0];
    bool isSJIS = (first >= 0x81 && first <= 0x9F) ||
                  (first >= 0xE0 && first <= 0xFC);
    if (isSJIS && c >= 4 && g_logCount < 50) {
        g_logCount++;
        char logBuf[512];
        sprintf_s(logBuf, "MISS #%d (len=%d, x=%d, y=%d)", g_logCount, c, x, y);
        LogToFile(logBuf);

        char hexBuf[256] = "  HEX: ";
        int hexOff = 7;
        int limit = (c < 64) ? c : 64;
        for (int i = 0; i < limit; i++) {
            sprintf_s(hexBuf + hexOff, sizeof(hexBuf) - hexOff, "%02X ",
                      (unsigned char)lpString[i]);
            hexOff += 3;
        }
        if (c > 64) strcat_s(hexBuf, "...");
        LogToFile(hexBuf);

        char rawBuf[300];
        int copyLen = (c < 250) ? c : 250;
        memcpy(rawBuf + 7, lpString, copyLen);
        memcpy(rawBuf, "  RAW: ", 7);
        rawBuf[7 + copyLen] = '\0';
        LogToFile(rawBuf);
    }

    return g_origTextOutA(hdc, x, y, lpString, c);
}

// =============================================================================
// IAT Hook
// =============================================================================
//
// Patches the Import Address Table of a PE module to redirect calls to a
// specific imported function. Used to redirect main.bin's TextOutA import
// to our HookedTextOutA.

static bool HookIAT(HMODULE hModule, const char* dllName,
                     const char* funcName, void* hookFunc, void** origFunc) {
    BYTE* base = (BYTE*)hModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* imports = (IMAGE_IMPORT_DESCRIPTOR*)(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imports->Name; imports++) {
        const char* name = (const char*)(base + imports->Name);
        if (_stricmp(name, dllName) != 0) continue;

        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + imports->OriginalFirstThunk);
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + imports->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, thunk++) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* imp = (IMAGE_IMPORT_BY_NAME*)(
                base + origThunk->u1.AddressOfData);
            if (strcmp((const char*)imp->Name, funcName) == 0) {
                DWORD oldProt;
                VirtualProtect(&thunk->u1.Function, sizeof(DWORD),
                               PAGE_EXECUTE_READWRITE, &oldProt);
                *origFunc = (void*)thunk->u1.Function;
                thunk->u1.Function = (DWORD)hookFunc;
                VirtualProtect(&thunk->u1.Function, sizeof(DWORD),
                               oldProt, &oldProt);
                char msg[256];
                sprintf_s(msg, "[TrueBluePatch] IAT hooked %s!%s", dllName, funcName);
                OutputDebugStringA(msg);
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// Hook Thread
// =============================================================================

// Separated from HookThread so SEH (__try/__except) doesn't coexist with
// C++ objects that require destructor unwinding (MSVC error C2712).
static bool SafeProbeModule(void* base, HMODULE* outModule) {
    __try {
        BYTE* p = (BYTE*)base;
        if (p[0] == 'M' && p[1] == 'Z') {
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)p;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(p + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                nt->OptionalHeader.SizeOfCode > 0x50000) {
                *outModule = (HMODULE)p;
                return true;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static DWORD WINAPI HookThread(LPVOID) {
    // 1. Load the pre-split dictionary (dictionary.txt)
    LoadDictionary();
    if (!g_dictLoaded || g_dict.empty()) return 0;

    // 2. Load full (unsplit) entries from dictionary_full.txt.
    //    For entries longer than 44 bytes, store two things:
    //    a) g_fullDict[first 44 bytes] = "fullJP\tfullEN" (for continuation lookup)
    //    b) g_dict[fullJP] = fullEN (for direct matching when game doesn't split)
    {
        std::ifstream ff("dictionary_full.txt");
        int fullAdded = 0;
        if (ff.is_open()) {
            std::string line;
            while (std::getline(ff, line)) {
                if (line.empty() || line[0] == '#') continue;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string fullJP = line.substr(0, tab);
                std::string fullEN = line.substr(tab + 1);
                if ((int)fullJP.size() > JP_BYTES_PER_ROW) {
                    std::string firstRow = fullJP.substr(0, JP_BYTES_PER_ROW);
                    g_fullDict[firstRow] = fullJP + "\t" + fullEN;

                    if (g_dict.find(fullJP) == g_dict.end()) {
                        g_dict[fullJP] = fullEN;
                        fullAdded++;
                    }
                }
            }
            char msg[128];
            sprintf_s(msg, "[TrueBluePatch] Loaded %d full entries (%d new) for continuations.",
                      (int)g_fullDict.size(), fullAdded);
            OutputDebugStringA(msg);
        }
    }

    // 3. Wait for main.bin to appear in memory.
    //    The game extracts it at runtime as a temp file. Try GetModuleHandle
    //    first, then fall back to scanning memory for an MZ/PE header.
    OutputDebugStringA("[TrueBluePatch] Waiting for main.bin...");

    HMODULE hMainBin = nullptr;
    for (int attempt = 0; attempt < 300; attempt++) {
        Sleep(100);
        hMainBin = GetModuleHandleA("main.bin");
        if (hMainBin) break;

        MEMORY_BASIC_INFORMATION mbi;
        DWORD addr = 0x00400000;
        while (addr < 0x10000000) {
            if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x40000) {
                    if (SafeProbeModule((void*)mbi.AllocationBase, &hMainBin))
                        break;
                }
                addr = (DWORD)mbi.BaseAddress + mbi.RegionSize;
            } else {
                addr += 0x10000;
            }
        }
        if (hMainBin) break;
    }

    if (!hMainBin) {
        OutputDebugStringA("[TrueBluePatch] main.bin not found!");
        return 0;
    }

    char msg[256];
    sprintf_s(msg, "[TrueBluePatch] main.bin at 0x%08X", (DWORD)hMainBin);
    OutputDebugStringA(msg);

    // 4. Patch main.bin's IAT to redirect TextOutA to our hook.
    //    Try both "GDI32.dll" and "gdi32.dll" casing.
    if (!HookIAT(hMainBin, "GDI32.dll", "TextOutA",
                 (void*)HookedTextOutA, (void**)&g_origTextOutA)) {
        HookIAT(hMainBin, "gdi32.dll", "TextOutA",
                 (void*)HookedTextOutA, (void**)&g_origTextOutA);
    }

    return 0;
}

// =============================================================================
// Winmm Proxy
// =============================================================================
//
// The game imports winmm.dll. By placing our DLL named winmm.dll in the game
// folder, Windows loads ours instead of the system one. We load the real
// system winmm.dll and forward all 25 imported functions via naked trampolines.
// The .def file maps each export name (e.g., mmioOpenA) to proxy_N.

static HMODULE g_realWinmm = nullptr;
static FARPROC g_origFuncs[64] = {};
static const char* g_funcNames[] = {
    "mmioOpenA", "timeBeginPeriod", "timeSetEvent", "timeKillEvent",
    "timeEndPeriod", "mmioGetInfo", "mmioAdvance", "mmioSetInfo",
    "mmioSeek", "mmioDescend", "mmioRead", "mmioAscend", "mmioClose",
    "mixerGetNumDevs", "mixerOpen", "mixerGetDevCapsA",
    "mixerGetLineInfoA", "mixerGetLineControlsA", "mixerSetControlDetails",
    "mixerGetControlDetailsA", "mixerClose", "midiOutGetNumDevs",
    "mciSendCommandA", "mciGetErrorStringA", "timeGetTime", nullptr
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\winmm.dll");
        g_realWinmm = LoadLibraryA(sysDir);
        if (!g_realWinmm) {
            MessageBoxA(nullptr, "Failed to load real winmm.dll!",
                        "True Blue EN Patch", MB_OK | MB_ICONERROR);
            return FALSE;
        }
        for (int i = 0; g_funcNames[i]; i++)
            g_origFuncs[i] = GetProcAddress(g_realWinmm, g_funcNames[i]);
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_realWinmm) FreeLibrary(g_realWinmm);
    }
    return TRUE;
}

// Naked trampolines: each just jumps to the real winmm function.
// The .def file maps export names to these (e.g., mmioOpenA = proxy_0).
#define TRAMPOLINE(idx) \
    extern "C" __declspec(naked, dllexport) void __stdcall proxy_##idx() { \
        __asm { jmp dword ptr [g_origFuncs + idx * 4] } \
    }

TRAMPOLINE(0)   TRAMPOLINE(1)   TRAMPOLINE(2)   TRAMPOLINE(3)
TRAMPOLINE(4)   TRAMPOLINE(5)   TRAMPOLINE(6)   TRAMPOLINE(7)
TRAMPOLINE(8)   TRAMPOLINE(9)   TRAMPOLINE(10)  TRAMPOLINE(11)
TRAMPOLINE(12)  TRAMPOLINE(13)  TRAMPOLINE(14)  TRAMPOLINE(15)
TRAMPOLINE(16)  TRAMPOLINE(17)  TRAMPOLINE(18)  TRAMPOLINE(19)
TRAMPOLINE(20)  TRAMPOLINE(21)  TRAMPOLINE(22)  TRAMPOLINE(23)
TRAMPOLINE(24)
