// winmm_proxy.cpp - Proxy DLL for True Blue English patch
// Hooks TextOutA to replace Japanese text with English at render time.
// Build: cl /LD /O2 /EHsc /D_CRT_SECURE_NO_WARNINGS winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def /MACHINE:X86 user32.lib gdi32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>

// ---- Translation dictionary -------------------------------------------------

static std::unordered_map<std::string, std::string> g_dict;
// Second map: full JP key -> full EN value (for building continuation cache)
static std::unordered_map<std::string, std::string> g_fullDict;
static bool g_dictLoaded = false;

static const int JP_CHARS_PER_ROW = 22;  // game wraps at 22 fullwidth chars
static const int JP_BYTES_PER_ROW = 44;  // 22 * 2 bytes per SJIS char
static const int EN_CHARS_PER_ROW = 44;  // ASCII half-width, ~44 fit per row

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

    // Also build the full-entry dictionary from the same file.
    // Entries where the JP key is exactly JP_BYTES_PER_ROW bytes are
    // potential first-row fragments. Find the full entry by checking
    // if the next entry starts where this one ends.
    // (Simpler: the full entries are those <= JP_BYTES_PER_ROW that are
    // also in g_dict. The split entries are those == JP_BYTES_PER_ROW.)

    g_dictLoaded = true;
    char msg[128];
    sprintf_s(msg, "[TrueBluePatch] Loaded %d translation entries.", count);
    OutputDebugStringA(msg);
}

// ---- Continuation cache -----------------------------------------------------
// When a first-row match is found for a long line, we predict what the
// continuation rows will look like and cache their translations.

static std::unordered_map<std::string, std::string> g_contCache;

static void BuildContinuationCache(const std::string& fullJP, const std::string& fullEN) {
    g_contCache.clear();

    if ((int)fullJP.size() <= JP_BYTES_PER_ROW) return;

    // Split EN into word-wrapped rows
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

    // Split JP into rows of JP_BYTES_PER_ROW, respecting SJIS boundaries
    int jpOff = JP_BYTES_PER_ROW; // first row already matched
    int enIdx = 1;
    while (jpOff < (int)fullJP.size()) {
        int jpEnd = jpOff + JP_BYTES_PER_ROW;
        if (jpEnd > (int)fullJP.size()) jpEnd = (int)fullJP.size();
        // Ensure we don't split a 2-byte SJIS character
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
        if (!jpFrag.empty())
            g_contCache[jpFrag] = enFrag;
        jpOff = jpEnd;
        enIdx++;
    }
}

// ---- English font -----------------------------------------------------------
// Created lazily on first translated TextOutA call, matching the game's font
// height but using Arial Narrow for narrower English glyphs.

static HFONT g_enFont = nullptr;

static HFONT GetEnglishFont(HDC hdc) {
    if (g_enFont) return g_enFont;

    TEXTMETRICA tm;
    if (GetTextMetricsA(hdc, &tm)) {
        g_enFont = CreateFontA(
            tm.tmHeight, 0, 0, 0, tm.tmWeight,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS,
            "Arial Narrow");
    }
    return g_enFont;
}

// ---- TextOutA hook ----------------------------------------------------------

typedef BOOL (WINAPI *TextOutA_t)(HDC, int, int, LPCSTR, int);
static TextOutA_t g_origTextOutA = nullptr;

static FILE* g_logFile = nullptr;
static int g_logCount = 0;

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

BOOL WINAPI HookedTextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) {
    if (!g_dictLoaded || !lpString || c <= 0)
        return g_origTextOutA(hdc, x, y, lpString, c);

    std::string text(lpString, c);

    // 1. Check continuation cache (rows 2+ of a multi-row line)
    auto cont = g_contCache.find(text);
    if (cont != g_contCache.end()) {
        const std::string& en = cont->second;
        return RenderEnglish(hdc, x, y, en.c_str(), (int)en.size());
    }

    // 2. Exact dictionary match (short lines or first-row fragments)
    auto it = g_dict.find(text);
    if (it != g_dict.end()) {
        const std::string& en = it->second;

        // If this was a first-row fragment of a longer entry,
        // build the continuation cache for subsequent rows.
        // We detect this by checking if the text is exactly JP_BYTES_PER_ROW
        // and the dictionary value has a "full entry" marker.
        // (Actually, just check if the original full key is longer.)
        // For now, we build continuations for all matches with row-length keys.
        if (c == JP_BYTES_PER_ROW) {
            // Search for a full entry that starts with this text
            // (stored with a special prefix in the dictionary)
            auto fullIt = g_fullDict.find(text);
            if (fullIt != g_fullDict.end()) {
                // fullIt->second format: "fullJP\tfullEN"
                size_t tab = fullIt->second.find('\t');
                if (tab != std::string::npos) {
                    std::string fullJP = fullIt->second.substr(0, tab);
                    std::string fullEN = fullIt->second.substr(tab + 1);
                    BuildContinuationCache(fullJP, fullEN);
                }
            }
        }

        return RenderEnglish(hdc, x, y, en.c_str(), (int)en.size());
    }

    // 3. Prefix fallback for rows wider than the predicted 44-byte split.
    //    The game wraps by pixel width, so some first rows are wider than 22
    //    fullwidth chars. Match by checking if the first 44 bytes correspond
    //    to a known multi-row entry.
    if (c > JP_BYTES_PER_ROW && c < JP_BYTES_PER_ROW * 2) {
        std::string prefix = text.substr(0, JP_BYTES_PER_ROW);
        auto fullIt = g_fullDict.find(prefix);
        if (fullIt != g_fullDict.end()) {
            size_t tab = fullIt->second.find('\t');
            if (tab != std::string::npos) {
                std::string fullJP = fullIt->second.substr(0, tab);
                std::string fullEN = fullIt->second.substr(tab + 1);
                if ((int)fullJP.size() >= c &&
                    fullJP.compare(0, c, text) == 0) {
                    // Get EN row 1 from the 44-byte fragment in g_dict
                    auto dictIt = g_dict.find(prefix);
                    std::string enRow1;
                    if (dictIt != g_dict.end()) {
                        enRow1 = dictIt->second;
                    } else {
                        enRow1 = fullEN;
                    }

                    // Build continuation for the ACTUAL remaining JP bytes
                    std::string remainJP = fullJP.substr(c);
                    if (!remainJP.empty()) {
                        std::string remainEN;
                        if (fullEN.size() > enRow1.size()) {
                            size_t pos = enRow1.size();
                            while (pos < fullEN.size() && fullEN[pos] == ' ')
                                pos++;
                            remainEN = fullEN.substr(pos);
                        }
                        g_contCache.clear();
                        g_contCache[remainJP] = remainEN;
                    }

                    return RenderEnglish(hdc, x, y,
                                        enRow1.c_str(), (int)enRow1.size());
                }
            }
        }
    }

    // 4. Log misses for debugging (include hex dump to identify the text)
    unsigned char first = (unsigned char)lpString[0];
    bool isSJIS = (first >= 0x81 && first <= 0x9F) ||
                  (first >= 0xE0 && first <= 0xFC);
    if (isSJIS && c >= 4 && g_logCount < 50) {
        g_logCount++;
        char logBuf[512];
        sprintf_s(logBuf, "MISS #%d (len=%d, x=%d, y=%d)", g_logCount, c, x, y);
        LogToFile(logBuf);

        // Log hex bytes (up to 64 bytes)
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

        // Log raw text (safe to write CP932 bytes to file)
        char rawBuf[300];
        int copyLen = (c < 250) ? c : 250;
        memcpy(rawBuf + 7, lpString, copyLen);
        memcpy(rawBuf, "  RAW: ", 7);
        rawBuf[7 + copyLen] = '\0';
        LogToFile(rawBuf);
    }

    return g_origTextOutA(hdc, x, y, lpString, c);
}

// ---- IAT hook ---------------------------------------------------------------

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

// ---- Hook thread ------------------------------------------------------------

// Separated from HookThread so SEH (__try) doesn't coexist with C++ objects.
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
    LoadDictionary();
    if (!g_dictLoaded || g_dict.empty()) return 0;

    // Build the full-entry lookup table for continuation cache.
    // For each dictionary entry whose JP key is exactly JP_BYTES_PER_ROW bytes,
    // we need to know the FULL original entry it came from.
    // We rebuild this from translation-map by reading dictionary_full.txt
    // (generated alongside dictionary.txt with full entries).
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

                    // Also add the full (unsplit) entry to g_dict so it matches
                    // when the game sends the whole line without splitting.
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

    if (!HookIAT(hMainBin, "GDI32.dll", "TextOutA",
                 (void*)HookedTextOutA, (void**)&g_origTextOutA)) {
        HookIAT(hMainBin, "gdi32.dll", "TextOutA",
                 (void*)HookedTextOutA, (void**)&g_origTextOutA);
    }

    return 0;
}

// ---- Winmm proxy ------------------------------------------------------------

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
