// winmm_proxy.cpp - Proxy DLL for True Blue English patch
//
// Hooks TextOutA (GDI32) to replace Japanese text with English at render time.
//
// Build: cl /LD /O2 /EHsc winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def /MACHINE:X86 user32.lib gdi32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <fstream>

// ---- Translation dictionary -------------------------------------------------

static std::unordered_map<std::string, std::string> g_dict;
static bool g_dictLoaded = false;

static void LoadDictionary() {
    std::ifstream f("dictionary.txt");
    if (!f.is_open()) {
        MessageBoxA(nullptr,
            "dictionary.txt not found!\nPlace it in the game folder.",
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

// ---- TextOutA hook ----------------------------------------------------------
//
// BOOL TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c);
//
// The game calls this to draw each line of dialogue text on screen.
// We intercept it, look up the Japanese string in our dictionary,
// and if found, replace it with the English translation.

typedef BOOL (WINAPI *TextOutA_t)(HDC, int, int, LPCSTR, int);
static TextOutA_t g_origTextOutA = nullptr;

static char g_replaceBuffer[4096] = {};
static int g_sjisLogCount = 0;
static int g_matchCount = 0;

// Log file for detailed debugging (less spammy than OutputDebugString)
static FILE* g_logFile = nullptr;

static void LogToFile(const char* msg) {
    if (!g_logFile) {
        g_logFile = fopen("patch_log.txt", "w");
        if (!g_logFile) return;
    }
    fprintf(g_logFile, "%s\n", msg);
    fflush(g_logFile);
}

BOOL WINAPI HookedTextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) {
    if (g_dictLoaded && lpString && c > 0) {
        std::string text(lpString, c);

        // Log first 200 SJIS text fragments to file (not DebugView)
        unsigned char first = (unsigned char)lpString[0];
        bool isSJIS = (first >= 0x81 && first <= 0x9F) ||
                      (first >= 0xE0 && first <= 0xFC);

        if (isSJIS && c >= 2 && g_sjisLogCount < 200) {
            g_sjisLogCount++;
            char logBuf[512];
            sprintf_s(logBuf, "TextOut #%d (len=%d, x=%d, y=%d): [%.100s]",
                      g_sjisLogCount, c, x, y, text.c_str());
            LogToFile(logBuf);

            // Also show hex of first 20 bytes
            char hexPart[80] = {};
            int show = c < 20 ? c : 20;
            for (int i = 0; i < show; i++)
                sprintf_s(hexPart + i * 3, sizeof(hexPart) - i * 3,
                          "%02X ", (unsigned char)lpString[i]);
            LogToFile(hexPart);
        }

        // Dictionary lookup (exact match)
        auto it = g_dict.find(text);
        if (it != g_dict.end()) {
            g_matchCount++;
            const std::string& en = it->second;
            char logBuf[256];
            sprintf_s(logBuf, ">>> MATCH #%d: [%.60s] -> [%.60s]",
                      g_matchCount, text.c_str(), en.c_str());
            LogToFile(logBuf);
            return g_origTextOutA(hdc, x, y, en.c_str(), (int)en.size());
        }
    }

    return g_origTextOutA(hdc, x, y, lpString, c);
}

// ---- Simple IAT hook (Import Address Table patching) ------------------------
//
// Instead of inline hooks, we patch the game's IAT entry for TextOutA
// to point to our function. This is simpler and safer.

static bool HookIAT(HMODULE hModule, const char* dllName,
                     const char* funcName, void* hookFunc, void** origFunc) {
    // Get the PE headers
    BYTE* base = (BYTE*)hModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* imports = (IMAGE_IMPORT_DESCRIPTOR*)(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    // Find the target DLL
    for (; imports->Name; imports++) {
        const char* name = (const char*)(base + imports->Name);
        if (_stricmp(name, dllName) != 0) continue;

        // Walk the thunk array
        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + imports->OriginalFirstThunk);
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + imports->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, thunk++) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;

            IMAGE_IMPORT_BY_NAME* imp = (IMAGE_IMPORT_BY_NAME*)(
                base + origThunk->u1.AddressOfData);

            if (strcmp((const char*)imp->Name, funcName) == 0) {
                // Found it! Save original and patch
                DWORD oldProt;
                VirtualProtect(&thunk->u1.Function, sizeof(DWORD),
                               PAGE_EXECUTE_READWRITE, &oldProt);
                *origFunc = (void*)thunk->u1.Function;
                thunk->u1.Function = (DWORD)hookFunc;
                VirtualProtect(&thunk->u1.Function, sizeof(DWORD),
                               oldProt, &oldProt);

                char msg[256];
                sprintf_s(msg, "[TrueBluePatch] IAT hooked %s!%s at %p -> %p",
                          dllName, funcName, *origFunc, hookFunc);
                OutputDebugStringA(msg);
                return true;
            }
        }
    }
    return false;
}

// ---- Hook installation thread -----------------------------------------------
// Waits for main.bin to be loaded, then patches its IAT.

static DWORD WINAPI HookThread(LPVOID) {
    LoadDictionary();
    if (!g_dictLoaded || g_dict.empty()) return 0;

    OutputDebugStringA("[TrueBluePatch] Waiting for main.bin...");

    HMODULE hMainBin = nullptr;
    for (int attempt = 0; attempt < 300; attempt++) {
        Sleep(100);

        // Try GetModuleHandle
        hMainBin = GetModuleHandleA("main.bin");
        if (hMainBin) break;

        // Scan memory for main.bin's PE signature at a known offset
        // Check offset 0x00 for MZ and offset 0x33794 region for known bytes
        MEMORY_BASIC_INFORMATION mbi;
        DWORD addr = 0x00400000;
        while (addr < 0x10000000) {
            if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x40000) {
                    __try {
                        BYTE* base = (BYTE*)mbi.AllocationBase;
                        if (base[0] == 'M' && base[1] == 'Z') {
                            // Verify it's main.bin by checking the import table
                            // has TextOutA (already confirmed in our analysis)
                            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
                            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
                            if (nt->Signature == IMAGE_NT_SIGNATURE &&
                                nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                                nt->OptionalHeader.SizeOfCode > 0x50000) {
                                hMainBin = (HMODULE)base;
                                break;
                            }
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
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

    // Hook TextOutA in main.bin's IAT
    if (!HookIAT(hMainBin, "GDI32.dll", "TextOutA",
                 (void*)HookedTextOutA, (void**)&g_origTextOutA)) {
        // Try lowercase
        if (!HookIAT(hMainBin, "gdi32.dll", "TextOutA",
                     (void*)HookedTextOutA, (void**)&g_origTextOutA)) {
            OutputDebugStringA("[TrueBluePatch] Failed to hook TextOutA!");
        }
    }

    return 0;
}

// ---- Winmm proxy infrastructure ---------------------------------------------

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

// ---- Exported trampolines ---------------------------------------------------

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
