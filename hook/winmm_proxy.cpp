// winmm_proxy.cpp — Proxy DLL for True Blue English patch
//
// This DLL masquerades as winmm.dll. When the game loads it, it:
//   1. Forwards all winmm API calls to the real system winmm.dll
//   2. Waits for main.bin to be loaded into memory
//   3. Hooks the text rendering function at main.bin+0x33794
//   4. Replaces Japanese text with English from dictionary.txt
//
// Build (Visual Studio 2019+ x86 Developer Command Prompt):
//   cl /LD /O2 /EHsc winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <fstream>

// ── Forwarded winmm functions ───────────────────────────────────────────────
// We load the real winmm.dll and forward all imported functions via
// GetProcAddress at runtime.

static HMODULE g_realWinmm = nullptr;

#define FORWARD_FUNC(name) \
    static decltype(&name) p_##name = nullptr; \
    extern "C" __declspec(dllexport) void __stdcall proxy_##name() { \
        if (!p_##name) p_##name = (decltype(&name))GetProcAddress(g_realWinmm, #name); \
        /* This stub just jumps to the real function via inline asm */ \
    }

// Instead of the macro approach (which doesn't handle args), we use
// a .def file with forwarding + runtime load.  See winmm.def.
// Each export is a naked trampoline to the real function.

// Pointers to real winmm functions
static FARPROC g_origFuncs[64] = {};

// Function names imported by main.bin (in order matching .def ordinals)
static const char* g_funcNames[] = {
    "mmioOpenA",
    "timeBeginPeriod",
    "timeSetEvent",
    "timeKillEvent",
    "timeEndPeriod",
    "mmioGetInfo",
    "mmioAdvance",
    "mmioSetInfo",
    "mmioSeek",
    "mmioDescend",
    "mmioRead",
    "mmioAscend",
    "mmioClose",
    "mixerGetNumDevs",
    "mixerOpen",
    "mixerGetDevCapsA",
    "mixerGetLineInfoA",
    "mixerGetLineControlsA",
    "mixerSetControlDetails",
    "mixerGetControlDetailsA",
    "mixerClose",
    "midiOutGetNumDevs",
    "mciSendCommandA",
    "mciGetErrorStringA",
    "timeGetTime",
    nullptr
};

// ── Translation dictionary ──────────────────────────────────────────────────

static std::unordered_map<std::string, std::string> g_dict;
static bool g_dictLoaded = false;
static bool g_hookInstalled = false;

// Original bytes at hook point (for unhooking)
static BYTE g_origBytes[5] = {};
static DWORD g_hookAddr = 0;

// Buffer for the English replacement text
static char g_replaceBuffer[4096] = {};

static void LoadDictionary() {
    // Dictionary format: tab-separated, one pair per line
    // {Japanese Shift-JIS}\t{English Shift-JIS}
    // Lines starting with # are comments
    std::ifstream f("dictionary.txt");
    if (!f.is_open()) {
        MessageBoxA(nullptr,
            "dictionary.txt not found!\n"
            "Place it in the game folder.",
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
    sprintf_s(msg, "Loaded %d translation entries.", count);
    OutputDebugStringA(msg);
}

// ── Hook implementation ─────────────────────────────────────────────────────
//
// The text function at main.bin+0x33794 is a Shift-JIS string copy.
// Based on the LunaTranslator H-code (HSXN12+-1C:8), the source string
// pointer is accessible from the function's context.
//
// Strategy: We hook BEFORE the string copy.  When our hook is called,
// we inspect the source text, look it up in the dictionary, and if found,
// redirect the source pointer to our English replacement buffer.
//
// The function at +0x33794 is mid-function (it's the memcpy part of a
// larger text-processing function).  We'll hook a few bytes before where
// the copy begins.  The exact register/stack state depends on the calling
// convention, so we use a naked hook to preserve everything.

// The hook target: the instruction at main.bin+0x33794 is:
//   C1 E9 02    shr ecx, 2    (preparing byte count for rep movsd)
//   F3 A5       rep movsd      (copy dwords)
//   8B CA       mov ecx, edx
//   83 E1 03    and ecx, 3
//   F3 A4       rep movsb      (copy remaining bytes)
//
// At this point: ESI = source text, EDI = destination, ECX/EDX = length.
// We can intercept here and modify ESI (source) to point to our buffer.

typedef void (*OrigTextFunc)();
static OrigTextFunc g_origTextFunc = nullptr;

// Our detour — called instead of the original code at the hook point.
// We read ESI (source string), look it up, and replace if found.
static __declspec(naked) void TextHookDetour() {
    __asm {
        // Save all registers
        pushad
        pushfd

        // ESI has the source Shift-JIS string pointer
        push esi
        call TextHookHandler
        // EAX = new source pointer (or original ESI if no match)
        mov [esp + 0x04], eax  // overwrite saved ESI on stack

        popfd
        popad

        // Execute the original instructions we overwrote (5 bytes):
        //   C1 E9 02    shr ecx, 2
        //   F3 A5       rep movsd
        // Wait — we can't just replay these because they depend on ECX/ESI/EDI.
        // Instead, jump back to hook_addr + 5 to continue original code.
        // But first we replaced ESI, so the copy will use our buffer.

        // Actually, for a JMP hook we need to jump back.
        // Let's use a different approach — trampoline.
        jmp [g_trampolineAddr]
    }
}

// C++ handler called from the detour
static DWORD g_trampolineAddr = 0;

extern "C" __declspec(noinline)
void* __cdecl TextHookHandler(const char* srcText) {
    if (!g_dictLoaded || !srcText) return (void*)srcText;

    // Try exact match in dictionary
    auto it = g_dict.find(std::string(srcText));
    if (it != g_dict.end()) {
        // Copy English text to replacement buffer
        strncpy_s(g_replaceBuffer, it->second.c_str(), sizeof(g_replaceBuffer) - 1);
        return g_replaceBuffer;
    }

    return (void*)srcText;
}

// ── Simple 5-byte JMP hook ──────────────────────────────────────────────────
// Overwrites the first 5 bytes at the target with:  JMP <detour>
// Saves original bytes for the trampoline.

static BYTE g_trampoline[32] = {};

static bool InstallHook(DWORD targetAddr, void* detourFunc) {
    // Save original bytes
    memcpy(g_origBytes, (void*)targetAddr, 5);

    // Build trampoline: original 5 bytes + JMP back to target+5
    memcpy(g_trampoline, g_origBytes, 5);
    g_trampoline[5] = 0xE9; // JMP rel32
    DWORD jmpBack = (targetAddr + 5) - ((DWORD)&g_trampoline[6] + 4);
    memcpy(&g_trampoline[6], &jmpBack, 4);

    // Make trampoline executable
    DWORD oldProtect;
    VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);

    g_trampolineAddr = (DWORD)g_trampoline;

    // Overwrite target with JMP to detour
    VirtualProtect((void*)targetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    BYTE jmpPatch[5];
    jmpPatch[0] = 0xE9; // JMP rel32
    DWORD rel = (DWORD)detourFunc - (targetAddr + 5);
    memcpy(&jmpPatch[1], &rel, 4);
    memcpy((void*)targetAddr, jmpPatch, 5);
    VirtualProtect((void*)targetAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)targetAddr, 5);

    g_hookAddr = targetAddr;
    g_hookInstalled = true;
    return true;
}

// ── Main.bin detection and hook installation ────────────────────────────────

static const DWORD HOOK_OFFSET = 0x33794;

static DWORD WINAPI HookThread(LPVOID) {
    // Load dictionary first
    LoadDictionary();
    if (!g_dictLoaded || g_dict.empty()) return 0;

    // Wait for main.bin to be loaded into memory.
    // Since main.bin is an EXE loaded dynamically, we scan for it.
    OutputDebugStringA("[TrueBluePatch] Waiting for main.bin...");

    HMODULE hMainBin = nullptr;
    for (int attempt = 0; attempt < 300; attempt++) { // 30 seconds max
        Sleep(100);

        // Try GetModuleHandle first (works if loaded via LoadLibrary)
        hMainBin = GetModuleHandleA("main.bin");
        if (hMainBin) break;

        // Also try scanning for the file-backed mapping
        // by checking if our known offset has the expected bytes
        // The game might map main.bin at a fixed address
        MEMORY_BASIC_INFORMATION mbi;
        DWORD addr = 0x00400000; // typical base
        while (addr < 0x10000000) {
            if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
                    mbi.RegionSize >= HOOK_OFFSET + 16) {
                    // Check if the bytes at offset match what we expect
                    __try {
                        BYTE* p = (BYTE*)mbi.AllocationBase + HOOK_OFFSET;
                        if (p[0] == 0xC1 && p[1] == 0xE9 && p[2] == 0x02 &&
                            p[3] == 0xF3 && p[4] == 0xA5) {
                            hMainBin = (HMODULE)mbi.AllocationBase;
                            break;
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
        OutputDebugStringA("[TrueBluePatch] main.bin not found in memory!");
        return 0;
    }

    char msg[256];
    sprintf_s(msg, "[TrueBluePatch] main.bin found at 0x%08X", (DWORD)hMainBin);
    OutputDebugStringA(msg);

    // Install hook
    DWORD hookTarget = (DWORD)hMainBin + HOOK_OFFSET;
    if (InstallHook(hookTarget, &TextHookDetour)) {
        sprintf_s(msg, "[TrueBluePatch] Hook installed at 0x%08X", hookTarget);
        OutputDebugStringA(msg);
    } else {
        OutputDebugStringA("[TrueBluePatch] Failed to install hook!");
    }

    return 0;
}

// ── DLL entry point ─────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        // Load the real winmm.dll from the system directory
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\winmm.dll");
        g_realWinmm = LoadLibraryA(sysDir);

        if (!g_realWinmm) {
            MessageBoxA(nullptr, "Failed to load real winmm.dll!",
                        "True Blue EN Patch", MB_OK | MB_ICONERROR);
            return FALSE;
        }

        // Resolve all forwarded function pointers
        for (int i = 0; g_funcNames[i]; i++) {
            g_origFuncs[i] = GetProcAddress(g_realWinmm, g_funcNames[i]);
        }

        // Start the hook installation thread
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_realWinmm) FreeLibrary(g_realWinmm);
    }
    return TRUE;
}

// ── Exported function trampolines ───────────────────────────────────────────
// Each exported function is a naked jump to the real winmm function.
// The .def file maps the export names.

#define DEFINE_TRAMPOLINE(index, name) \
    extern "C" __declspec(naked, dllexport) void name() { \
        __asm { jmp [g_origFuncs + index * 4] } \
    }

DEFINE_TRAMPOLINE(0,  proxy_mmioOpenA)
DEFINE_TRAMPOLINE(1,  proxy_timeBeginPeriod)
DEFINE_TRAMPOLINE(2,  proxy_timeSetEvent)
DEFINE_TRAMPOLINE(3,  proxy_timeKillEvent)
DEFINE_TRAMPOLINE(4,  proxy_timeEndPeriod)
DEFINE_TRAMPOLINE(5,  proxy_mmioGetInfo)
DEFINE_TRAMPOLINE(6,  proxy_mmioAdvance)
DEFINE_TRAMPOLINE(7,  proxy_mmioSetInfo)
DEFINE_TRAMPOLINE(8,  proxy_mmioSeek)
DEFINE_TRAMPOLINE(9,  proxy_mmioDescend)
DEFINE_TRAMPOLINE(10, proxy_mmioRead)
DEFINE_TRAMPOLINE(11, proxy_mmioAscend)
DEFINE_TRAMPOLINE(12, proxy_mmioClose)
DEFINE_TRAMPOLINE(13, proxy_mixerGetNumDevs)
DEFINE_TRAMPOLINE(14, proxy_mixerOpen)
DEFINE_TRAMPOLINE(15, proxy_mixerGetDevCapsA)
DEFINE_TRAMPOLINE(16, proxy_mixerGetLineInfoA)
DEFINE_TRAMPOLINE(17, proxy_mixerGetLineControlsA)
DEFINE_TRAMPOLINE(18, proxy_mixerSetControlDetails)
DEFINE_TRAMPOLINE(19, proxy_mixerGetControlDetailsA)
DEFINE_TRAMPOLINE(20, proxy_mixerClose)
DEFINE_TRAMPOLINE(21, proxy_midiOutGetNumDevs)
DEFINE_TRAMPOLINE(22, proxy_mciSendCommandA)
DEFINE_TRAMPOLINE(23, proxy_mciGetErrorStringA)
DEFINE_TRAMPOLINE(24, proxy_timeGetTime)
