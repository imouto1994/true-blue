// winmm_proxy.cpp - Proxy DLL for True Blue English patch
//
// This DLL masquerades as winmm.dll. When the game loads it, it:
//   1. Forwards all winmm API calls to the real system winmm.dll
//   2. Waits for main.bin to be loaded into memory
//   3. Hooks the text rendering function at main.bin+0x33794
//   4. Replaces Japanese text with English from dictionary.txt
//
// Build (Visual Studio x86 Developer Command Prompt):
//   cl /LD /O2 /EHsc winmm_proxy.cpp /Fe:winmm.dll /link /DEF:winmm.def

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <fstream>

// ---- Forward declarations for hook machinery --------------------------------

static void* __cdecl TextHookHandler(const char* srcText);

// Global variables that the naked ASM detour references (must be declared
// before the naked function so the compiler knows their addresses).
static DWORD g_trampolineAddr = 0;
static void* (__cdecl *g_handlerPtr)(const char*) = &TextHookHandler;
static char g_replaceBuffer[4096] = {};

// ---- Translation dictionary -------------------------------------------------

static std::unordered_map<std::string, std::string> g_dict;
static bool g_dictLoaded = false;
static bool g_hookInstalled = false;

static void LoadDictionary() {
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
    sprintf_s(msg, "[TrueBluePatch] Loaded %d translation entries.", count);
    OutputDebugStringA(msg);
}

// ---- Safe pointer check (SEH can't coexist with C++ objects) ----------------

static bool IsBadReadPointer(const void* ptr) {
    __try {
        volatile char c = *(const char*)ptr;
        (void)c;
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// ---- C++ hook handler (called from naked detour via function pointer) --------

static int g_logCount = 0;  // limit debug output to first N calls

static void* __cdecl TextHookHandler(const char* srcText) {
    if (!g_dictLoaded || !srcText) return (void*)srcText;
    if (IsBadReadPointer(srcText)) return (void*)srcText;
    if (srcText[0] == '\0') return (void*)srcText;

    // Log the first 50 texts we see so we can debug matching
    if (g_logCount < 50) {
        g_logCount++;
        char logBuf[512];

        // Show the raw bytes (first 40 bytes) for encoding diagnosis
        char hexPart[128] = {};
        int len = (int)strlen(srcText);
        int show = len < 40 ? len : 40;
        for (int i = 0; i < show; i++) {
            sprintf_s(hexPart + i * 3, sizeof(hexPart) - i * 3,
                      "%02X ", (unsigned char)srcText[i]);
        }

        sprintf_s(logBuf, "[TrueBluePatch] Text #%d (len=%d): %s",
                  g_logCount, len, hexPart);
        OutputDebugStringA(logBuf);

        // Also log as string (may be Shift-JIS)
        sprintf_s(logBuf, "[TrueBluePatch] Text #%d str: %.100s",
                  g_logCount, srcText);
        OutputDebugStringA(logBuf);
    }

    auto it = g_dict.find(std::string(srcText));
    if (it != g_dict.end()) {
        strncpy_s(g_replaceBuffer, it->second.c_str(), sizeof(g_replaceBuffer) - 1);
        OutputDebugStringA("[TrueBluePatch] >>> MATCH FOUND - replacing!");
        return g_replaceBuffer;
    }

    return (void*)srcText;
}

// ---- Register dump handler (for debugging) ----------------------------------

static void __cdecl DumpRegisters(DWORD edi, DWORD esi, DWORD ebp,
                                   DWORD esp_val, DWORD ebx, DWORD edx,
                                   DWORD ecx, DWORD eax) {
    static int dumpCount = 0;
    if (dumpCount >= 20) return;
    dumpCount++;

    char buf[512];
    sprintf_s(buf, "[TrueBluePatch] HOOK FIRED #%d  "
              "EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESI=%08X EDI=%08X",
              dumpCount, eax, ecx, edx, ebx, esi, edi);
    OutputDebugStringA(buf);

    // Try to read ESI as string
    if (!IsBadReadPointer((void*)esi) && *(char*)esi != 0) {
        sprintf_s(buf, "[TrueBluePatch]   ESI str: %.80s", (char*)esi);
        OutputDebugStringA(buf);
    }
    // Try EDI as string
    if (!IsBadReadPointer((void*)edi) && *(char*)edi != 0) {
        sprintf_s(buf, "[TrueBluePatch]   EDI str: %.80s", (char*)edi);
        OutputDebugStringA(buf);
    }
    // Try EAX as string
    if (!IsBadReadPointer((void*)eax) && *(char*)eax != 0) {
        sprintf_s(buf, "[TrueBluePatch]   EAX str: %.80s", (char*)eax);
        OutputDebugStringA(buf);
    }
    // Try EBX as string
    if (!IsBadReadPointer((void*)ebx) && *(char*)ebx != 0) {
        sprintf_s(buf, "[TrueBluePatch]   EBX str: %.80s", (char*)ebx);
        OutputDebugStringA(buf);
    }
}

static void (__cdecl *g_dumpPtr)(DWORD, DWORD, DWORD, DWORD,
                                  DWORD, DWORD, DWORD, DWORD) = &DumpRegisters;

// ---- Naked ASM detour -------------------------------------------------------
//
// The hook target at main.bin+0x33794 is mid-function:
//   C1 E9 02    shr ecx, 2     ; prepare dword count
//   F3 A5       rep movsd       ; copy dwords from ESI to EDI
//
// pushad pushes registers in order: EAX ECX EDX EBX ESP EBP ESI EDI
// Last pushed (EDI) is at top of stack.
// After pushad:
//   [ESP+00]=EDI  [ESP+04]=ESI  [ESP+08]=EBP  [ESP+0C]=ESP(orig)
//   [ESP+10]=EBX  [ESP+14]=EDX  [ESP+18]=ECX  [ESP+1C]=EAX

static __declspec(naked) void TextHookDetour() {
    __asm {
        pushad

        // Pass all registers to DumpRegisters for diagnosis
        // Args pushed right-to-left: eax, ecx, edx, ebx, esp_val, ebp, esi, edi
        push [esp + 0x1C]  // EAX
        push [esp + 0x1C]  // ECX (offset shifts by 4 due to push)
        push [esp + 0x1C]  // EDX
        push [esp + 0x1C]  // EBX
        push [esp + 0x1C]  // ESP
        push [esp + 0x1C]  // EBP
        push [esp + 0x1C]  // ESI
        push [esp + 0x1C]  // EDI
        call [g_dumpPtr]
        add esp, 32

        // Now call the text handler with ESI (source text)
        // ESI is at [ESP+0x04] in the pushad frame
        push [esp + 0x04]       // ESI
        call [g_handlerPtr]
        add esp, 4

        // Update saved ESI with handler's return value
        mov [esp + 0x04], eax

        popad

        // Jump to trampoline (original 5 bytes + return)
        jmp [g_trampolineAddr]
    }
}

// ---- 5-byte JMP hook installer ----------------------------------------------

static BYTE g_origBytes[5] = {};
static BYTE g_trampoline[32] = {};
static DWORD g_hookAddr = 0;

static bool InstallHook(DWORD targetAddr, void* detourFunc) {
    // Save original bytes
    memcpy(g_origBytes, (void*)targetAddr, 5);

    // Build trampoline: original 5 bytes + JMP back to target+5
    memcpy(g_trampoline, g_origBytes, 5);
    g_trampoline[5] = 0xE9; // JMP rel32
    DWORD backTarget = (targetAddr + 5) - ((DWORD)&g_trampoline[6] + 4);
    memcpy(&g_trampoline[6], &backTarget, 4);

    // Make trampoline executable
    DWORD oldProt;
    VirtualProtect(g_trampoline, sizeof(g_trampoline),
                   PAGE_EXECUTE_READWRITE, &oldProt);

    g_trampolineAddr = (DWORD)g_trampoline;

    // Overwrite target with JMP to our detour
    VirtualProtect((void*)targetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    BYTE patch[5];
    patch[0] = 0xE9; // JMP rel32
    DWORD rel = (DWORD)detourFunc - (targetAddr + 5);
    memcpy(&patch[1], &rel, 4);
    memcpy((void*)targetAddr, patch, 5);
    VirtualProtect((void*)targetAddr, 5, oldProt, &oldProt);

    FlushInstructionCache(GetCurrentProcess(), (void*)targetAddr, 5);

    g_hookAddr = targetAddr;
    g_hookInstalled = true;
    return true;
}

// ---- Main.bin detection and hook thread -------------------------------------

static const DWORD HOOK_OFFSET = 0x33794;

// Expected bytes at the hook point (shr ecx,2 / rep movsd)
static const BYTE HOOK_SIGNATURE[] = { 0xC1, 0xE9, 0x02, 0xF3, 0xA5 };

static DWORD WINAPI HookThread(LPVOID) {
    LoadDictionary();
    if (!g_dictLoaded || g_dict.empty()) return 0;

    OutputDebugStringA("[TrueBluePatch] Waiting for main.bin...");

    HMODULE hMainBin = nullptr;
    for (int attempt = 0; attempt < 300; attempt++) { // 30 seconds max
        Sleep(100);

        // Try GetModuleHandle (works if loaded via LoadLibrary)
        hMainBin = GetModuleHandleA("main.bin");
        if (hMainBin) break;

        // Scan memory for main.bin by looking for our signature bytes
        MEMORY_BASIC_INFORMATION mbi;
        DWORD addr = 0x00400000;
        while (addr < 0x10000000) {
            if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY | PAGE_READONLY |
                                    PAGE_READWRITE)) &&
                    mbi.RegionSize >= HOOK_OFFSET + sizeof(HOOK_SIGNATURE)) {
                    __try {
                        BYTE* base = (BYTE*)mbi.AllocationBase;
                        BYTE* p = base + HOOK_OFFSET;
                        if (memcmp(p, HOOK_SIGNATURE, sizeof(HOOK_SIGNATURE)) == 0) {
                            hMainBin = (HMODULE)base;
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
    sprintf_s(msg, "[TrueBluePatch] main.bin found at 0x%08X",
              (DWORD)hMainBin);
    OutputDebugStringA(msg);

    DWORD hookTarget = (DWORD)hMainBin + HOOK_OFFSET;
    if (InstallHook(hookTarget, &TextHookDetour)) {
        sprintf_s(msg, "[TrueBluePatch] Hook installed at 0x%08X", hookTarget);
        OutputDebugStringA(msg);
    } else {
        OutputDebugStringA("[TrueBluePatch] Failed to install hook!");
    }

    return 0;
}

// ---- Winmm proxy infrastructure ---------------------------------------------

static HMODULE g_realWinmm = nullptr;
static FARPROC g_origFuncs[64] = {};

static const char* g_funcNames[] = {
    "mmioOpenA",              //  0
    "timeBeginPeriod",        //  1
    "timeSetEvent",           //  2
    "timeKillEvent",          //  3
    "timeEndPeriod",          //  4
    "mmioGetInfo",            //  5
    "mmioAdvance",            //  6
    "mmioSetInfo",            //  7
    "mmioSeek",               //  8
    "mmioDescend",            //  9
    "mmioRead",               // 10
    "mmioAscend",             // 11
    "mmioClose",              // 12
    "mixerGetNumDevs",        // 13
    "mixerOpen",              // 14
    "mixerGetDevCapsA",       // 15
    "mixerGetLineInfoA",      // 16
    "mixerGetLineControlsA",  // 17
    "mixerSetControlDetails", // 18
    "mixerGetControlDetailsA",// 19
    "mixerClose",             // 20
    "midiOutGetNumDevs",      // 21
    "mciSendCommandA",        // 22
    "mciGetErrorStringA",     // 23
    "timeGetTime",            // 24
    nullptr
};

// ---- DLL entry point --------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        // Load real winmm.dll from system directory
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\winmm.dll");
        g_realWinmm = LoadLibraryA(sysDir);

        if (!g_realWinmm) {
            MessageBoxA(nullptr, "Failed to load real winmm.dll!",
                        "True Blue EN Patch", MB_OK | MB_ICONERROR);
            return FALSE;
        }

        for (int i = 0; g_funcNames[i]; i++) {
            g_origFuncs[i] = GetProcAddress(g_realWinmm, g_funcNames[i]);
        }

        // Start hook thread
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_realWinmm) FreeLibrary(g_realWinmm);
    }
    return TRUE;
}

// ---- Exported function trampolines ------------------------------------------

#define TRAMPOLINE(idx) \
    extern "C" __declspec(naked, dllexport) void __stdcall proxy_##idx() { \
        __asm { jmp dword ptr [g_origFuncs + idx * 4] } \
    }

// Generate trampolines for indices 0..24
TRAMPOLINE(0)   TRAMPOLINE(1)   TRAMPOLINE(2)   TRAMPOLINE(3)
TRAMPOLINE(4)   TRAMPOLINE(5)   TRAMPOLINE(6)   TRAMPOLINE(7)
TRAMPOLINE(8)   TRAMPOLINE(9)   TRAMPOLINE(10)  TRAMPOLINE(11)
TRAMPOLINE(12)  TRAMPOLINE(13)  TRAMPOLINE(14)  TRAMPOLINE(15)
TRAMPOLINE(16)  TRAMPOLINE(17)  TRAMPOLINE(18)  TRAMPOLINE(19)
TRAMPOLINE(20)  TRAMPOLINE(21)  TRAMPOLINE(22)  TRAMPOLINE(23)
TRAMPOLINE(24)
