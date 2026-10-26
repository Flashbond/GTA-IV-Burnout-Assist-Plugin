// LaunchAssist.cpp - v0.4.1 quiet diagnostics held-intent automatic-transmission burnout assist
// No force-duration timer and no clutch writes. The game's automatic gearbox owns shifts.
// Target: GTA IV Complete Edition 1.2.0.59, x86; no ScriptHook dependency.
// Build as a 32-bit DLL and rename output to LaunchAssist v0.4.1.asi.
//
// Strategy:
// - Validate and detour the known CALL site to FUN_00C2FDB0 inside FUN_00C2F9A0.
// - At standstill, W+Left Shift starts a persistent assist. The force multiplier
//   remains active while W+Shift stays held, including across automatic upshifts.
// - If activated while already rolling, do not inject extra torque immediately:
//   wait until native wheel telemetry shows sustained driven-wheel slip.
// - Do not write the clutch. GTA's native automatic transmission owns the shift.
// - No fixed-duration force burst; releasing W or Left Shift ends the assist.
// - No handling data or wheel-output fields are written.
// - Debug=0 disables CSV creation, CSV sampling, and verbose diagnostic logging.
//
// Safety: the call-site is patched only after the actual E8 target is verified.
// If it does not match the expected function, the hook is not installed and no
// clutch/force assist is run. This build is specific to GTAIV.exe 1.2.0.59 x86.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
    
// Portable finite-float check for both MSVC and MinGW/GCC.
// NaN fails (value != value); +/-infinity exceeds FLT_MAX.
static bool IsFiniteFloat(float value)
{
    return (value == value) && (value <= FLT_MAX) && (value >= -FLT_MAX);
}

static_assert(sizeof(void*) == 4, "Build this plugin as 32-bit (x86)");
typedef uintptr_t uptr;

// ---- GTA IV Complete Edition 1.2.0.59 addresses (preferred VA, image base 0x400000) ----
static const uptr IMAGE_BASE_PREFERRED  = 0x00400000;
static const uptr VA_LOCAL_PLAYER_INDEX = 0x01036F14;
static const uptr VA_PLAYER_INFO_ARRAY  = 0x011A8808;

// ---- Vehicle fields (offsets relative to vehicle pointer) ----
static const uptr V_TRANSMISSION = 0x1090; // short gear +0; RPM-like state +4; clutch +0x10
static const uptr V_RPM_STATE     = 0x1094; // smoothed drivetrain state candidate; not proven physical RPM
static const uptr V_CLUTCH        = 0x10A0; // float, native clutch/control state candidate
static const uptr V_GAS           = 0x1078; // native gas input candidate
static const uptr V_TYPE          = 0x1304; // existing filter: 0 = passenger car
static const uptr V_WHEELS        = 0x0F80;
static const uptr V_WHEEL_COUNT   = 0x0F84;
static const uptr V_HANDLING      = 0x0DC8;
static const uptr V_MATRIX        = 0x20;   // pointer; position at matrix + 0x30/34/38
static const uptr WHEEL_STRIDE    = 0x170;
static const uptr W_FLAGS         = 0x164;  // bit 15 is used as driven-wheel flag in current analysis
static const uptr H_MASS          = 0x10;

struct Config
{
    bool enabled;
    bool burnoutAssist;
    int key;
    bool allGears;
    bool debug;
    bool strictVersion;
    float forceMultiplier;
    float standstillSpeedThreshold;
    float standstillScalarFloor;
    float debugSampleMs;
    float telemetryTailMs;
};

static Config g_cfg;
static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_csvPath[MAX_PATH];
static uptr g_delta = 0;
static HANDLE g_thread = NULL;
static volatile bool g_stop = false;
static float g_speedLongitudinalRaw = -999.0f; // projected position-delta speed; -999 = unavailable
static unsigned long g_motionSampleSerial = 0; // increments only when a fresh position-speed sample is computed

// Native scalar hook state. These are 32-bit values because this plugin is x86.
static volatile LONG g_forceActive = 0;
static volatile LONG g_forceVehicle32 = 0;
static volatile LONG g_forceStandstill = 0;
static volatile LONG g_launchMotionStarted = 0;
static volatile LONG g_lastFloorApplied = 0;
static volatile LONG g_lastPreMultiplierScalarBits = 0;
static volatile LONG g_hookCalls = 0;
static volatile LONG g_hookMatchedCalls = 0;
static volatile LONG g_lastNativeScalarBits = 0;
static volatile LONG g_lastBoostedScalarBits = 0;
static volatile LONG g_lastHookTick = 0;
static volatile LONG g_hookInstallState = 0; // 0=not attempted, 1=installed, -1=failed
static bool g_hookInstalled = false;
static uptr g_callSite = 0;
static BYTE g_originalCallBytes[5] = {};

// GTAIV.exe 1.2.0.59 preferred VAs; addresses are relocated by g_delta.
static const uptr VA_C2FDB0 = 0x00C2FDB0;
static const uptr VA_C2F9A0_CALL_C2FDB0 = 0x00C2FB6B;

// Experimental Control Mapping UI patch.
// Makes the existing hidden 0x88 row eligible for display.
static const uptr VA_MAPPING_FILTER_88_BRANCH = 0x00B1ACC3;

// Experimental: read the key assigned to the hidden control row.
static const uptr VA_CONTROL_MAP_A = 0x016334C8;
static const uptr VA_INPUT_STATE_BASE = 0x018B7A90;
static const uptr VA_INPUT_STATE_BANK = 0x018B7DA0;

static const int BURNOUT_MAPPING_ROW = 49;

static BYTE g_originalMappingFilter88Bytes[2] = {};
static uptr g_mappingFilter88Address = 0;
static bool g_mappingFilter88Installed = false;

typedef float (__thiscall *PFN_C2FDB0)(short*, int*, int, float, float, float);
static PFN_C2FDB0 g_originalC2FDB0 = NULL;
static float g_forceMultiplier = 2.35f;

using LocalizationLookupFn = wchar_t* (__thiscall*)(void* self, const char* key);

static LocalizationLookupFn g_originalLocalizationLookup = nullptr;
static wchar_t g_burnoutLabel[] = L"Burnout Assist";

static wchar_t* __fastcall HookControlLabel(
    void* self,
    void* edxDummy,
    const char* key)
{
    (void)edxDummy;

    if (key && strcmp(key, "CTRL_136") == 0)
        return g_burnoutLabel;

    return g_originalLocalizationLookup
        ? g_originalLocalizationLookup(self, key)
        : nullptr;
}

// Legacy diagnostic callsites are compiled to no-ops: they must not format
// strings, inspect telemetry for log messages, or open files during gameplay.
#define Log(...) ((void)0)

// One concise status line per load. Overwrite instead of appending so the log
// never grows across sessions. Called only for a final status or an important error.
static void LogStatus(const char* message)
{
    if (!g_logPath[0] || !message) return;
    FILE* f = fopen(g_logPath, "w");
    if (!f) return;
    fputs(message, f);
    fputc('\n', f);
    fclose(f);
}

static bool BuildSiblingPath(
    char* out, size_t capacity,
    const char* modulePath, const char* filename)
{
    if (!out || !capacity || !modulePath || !filename)
        return false;

    const size_t pathLength = strlen(modulePath);
    if (pathLength >= capacity)
        return false;

    memcpy(out, modulePath, pathLength + 1);

    char* backslash = strrchr(out, '\\');
    char* slash = strrchr(out, '/');
    char* lastSeparator = backslash;
    if (!lastSeparator || (slash && slash > lastSeparator))
        lastSeparator = slash;

    if (lastSeparator)
        lastSeparator[1] = '\0';
    else
        out[0] = '\0';

    const size_t directoryLength = strlen(out);
    const size_t filenameLength = strlen(filename);
    if (directoryLength + filenameLength + 1 > capacity)
        return false;

    memcpy(out + directoryLength, filename, filenameLength + 1);
    return true;
}

static bool CreateDefaultIniIfMissing()
{
    const DWORD attributes = GetFileAttributesA(g_iniPath);
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;

    static const char defaults[] =
        "[BurnoutAssist]\r\n"
        "; GTA IV Complete Edition 1.2.0.59 x86 only. No ScriptHook dependency.\r\n"
        "Enabled=1\r\n"
        "EnableBurnoutAssist=1\r\n"
        "\r\n"
        "; The in-game Control Mapping assignment is used when available.\r\n"
        "; 160 = VK_LSHIFT; fallback only if the mapped input state cannot be read.\r\n"
        "TriggerKey=160\r\n"
        "; 1 = allow all forward gears (1+); 0 = first gear only for initial activation.\r\n"
        "; A brief automatic-shift gear transition does not cancel an active assist.\r\n"
        "AllGears=1\r\n"
        "\r\n"
        "; Scalar multiplier applied to the game's native drive scalar.\r\n"
        "; Starting value tuned to 2.35; adjust to taste.\r\n"
        "ForceMultiplier=2.35\r\n"
        "\r\n"
        "; Initial standstill/launch classification threshold (telemetry-derived game units).\r\n"
        "StandstillSpeedThreshold=0.75\r\n"
        "; Minimum native scalar until the vehicle starts moving; 0 disables this floor.\r\n"
        "StandstillScalarFloor=0.20\r\n"
        "\r\n"
        "; Rolling entry requires native wheel slip at high throttle; no duration timer.\r\n"
        "; Source thresholds: throttle >= 0.85, longitudinal speed >= 3.0, slip >= 0.25,\r\n"
        "; confirmed across 2 consecutive fresh movement samples.\r\n"
        "DebugSampleMs=25\r\n"
        "TelemetryTailMs=1500\r\n"
        "Debug=0\r\n"
        "\r\n"
        "; Require exact GTAIV.exe file version 1.2.0.59.\r\n"
        "StrictVersion=1\r\n";

    HANDLE file = CreateFileA(
        g_iniPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
    }

    DWORD written = 0;
    const DWORD expected = (DWORD)(sizeof(defaults) - 1);
    const BOOL writeOk = WriteFile(file, defaults, expected, &written, NULL);
    const BOOL closeOk = CloseHandle(file);

    if (!writeOk || !closeOk || written != expected) {
        DeleteFileA(g_iniPath);
        return false;
    }

    return true;
}

static float IniF(const char* key, float def)
{
    char buf[64] = {};
    GetPrivateProfileStringA("BurnoutAssist", key, "", buf,
                             sizeof(buf), g_iniPath);

    if (!buf[0]) {
        char fallback[32];
        sprintf(fallback, "%g", (double)def);
        GetPrivateProfileStringA("BurnoutAssist v0.4.1", key, fallback,
                                 buf, sizeof(buf), g_iniPath);
    }

    return (float)atof(buf);
}

static int IniI(const char* key, int def)
{
    char buf[32] = {};
    GetPrivateProfileStringA("BurnoutAssist", key, "", buf,
                             sizeof(buf), g_iniPath);

    if (buf[0])
        return atoi(buf);

    return (int)GetPrivateProfileIntA(
        "BurnoutAssist v0.4.1", key, def, g_iniPath);
}

static void LoadConfig()
{
    g_cfg.enabled                  = IniI("Enabled", 1) != 0;
    g_cfg.burnoutAssist            = IniI("EnableBurnoutAssist", 1) != 0;
    g_cfg.key                      = IniI("TriggerKey", 160); // VK_LSHIFT
    g_cfg.allGears                 = IniI("AllGears", 1) != 0;
    g_cfg.debug                    = IniI("Debug", 0) != 0;
    g_cfg.strictVersion            = IniI("StrictVersion", 1) != 0;
    g_cfg.forceMultiplier          = IniF("ForceMultiplier", 2.35f);
    g_cfg.standstillSpeedThreshold = IniF("StandstillSpeedThreshold", 0.75f);
    g_cfg.standstillScalarFloor    = IniF("StandstillScalarFloor", 0.20f);
    // Diagnostic settings are not read or used when Debug=0.
    g_cfg.debugSampleMs            = g_cfg.debug ? IniF("DebugSampleMs", 25.0f) : 0.0f;
    g_cfg.telemetryTailMs          = g_cfg.debug ? IniF("TelemetryTailMs", 1500.0f) : 0.0f;

    if (g_cfg.forceMultiplier < 1.0f) g_cfg.forceMultiplier = 1.0f;
    if (g_cfg.forceMultiplier > 5.0f) g_cfg.forceMultiplier = 5.0f;
    if (g_cfg.standstillSpeedThreshold < 0.05f) g_cfg.standstillSpeedThreshold = 0.05f;
    if (g_cfg.standstillSpeedThreshold > 5.0f) g_cfg.standstillSpeedThreshold = 5.0f;
    if (g_cfg.standstillScalarFloor < 0.0f) g_cfg.standstillScalarFloor = 0.0f;
    if (g_cfg.standstillScalarFloor > 1.0f) g_cfg.standstillScalarFloor = 1.0f;
    if (g_cfg.debug) {
        if (g_cfg.debugSampleMs < 10.0f) g_cfg.debugSampleMs = 10.0f;
        if (g_cfg.debugSampleMs > 250.0f) g_cfg.debugSampleMs = 250.0f;
        if (g_cfg.telemetryTailMs < 0.0f) g_cfg.telemetryTailMs = 0.0f;
        if (g_cfg.telemetryTailMs > 5000.0f) g_cfg.telemetryTailMs = 5000.0f;
    } else {
        g_cfg.debugSampleMs = 0.0f;
        g_cfg.telemetryTailMs = 0.0f;
    }
    g_forceMultiplier = g_cfg.forceMultiplier;
}

// ---- Protected memory access ----
static bool RdMem(uptr address, void* out, size_t size)
{
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, out, size, &got)) return false;
    return got == size;
}

static const uptr VA_LABEL_CALL_ROW  = 0x00B1A2AC;
static const uptr VA_LABEL_CALL_LIST = 0x00B1BB30;
static const uptr VA_LOCALIZATION_LOOKUP = 0x008C5BE0;

struct LabelCallPatch
{
    uptr site;
    BYTE original[5];
    bool installed;
};

static LabelCallPatch g_labelPatches[2] = {};

static bool DecodeRelativeCall(
    uptr site,
    const BYTE bytes[5],
    uptr& target)
{
    if (bytes[0] != 0xE8)
        return false;

    int32_t rel = 0;
    memcpy(&rel, bytes + 1, sizeof(rel));
    target = (uptr)((intptr_t)(site + 5) + (intptr_t)rel);
    return true;
}

static bool RestoreLabelCallPatch(LabelCallPatch& patch)
{
    if (!patch.installed)
        return true;

    BYTE current[5] = {};
    uptr target = 0;

    if (!RdMem(patch.site, current, sizeof(current)) ||
        !DecodeRelativeCall(patch.site, current, target) ||
        target != (uptr)&HookControlLabel)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect((LPVOID)patch.site, sizeof(patch.original),
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    memcpy((void*)patch.site, patch.original, sizeof(patch.original));
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)patch.site,
                          sizeof(patch.original));

    DWORD ignored = 0;
    VirtualProtect((LPVOID)patch.site, sizeof(patch.original), oldProtect, &ignored);
    patch.installed = false;
    return true;
}

static bool InstallOneLabelCallPatch(LabelCallPatch& patch, uptr site)
{
    BYTE original[5] = {};
    if (!RdMem(site, original, sizeof(original)))
        return false;

    uptr actualTarget = 0;
    if (!DecodeRelativeCall(site, original, actualTarget))
        return false;

    const uptr expectedTarget = VA_LOCALIZATION_LOOKUP + g_delta;
    if (actualTarget != expectedTarget)
        return false;

    BYTE replacement[5] = { 0xE8, 0, 0, 0, 0 };
    const int32_t newRel = (int32_t)(
        (uint32_t)(uptr)&HookControlLabel - (uint32_t)(site + 5));
    memcpy(replacement + 1, &newRel, sizeof(newRel));

    DWORD oldProtect = 0;
    if (!VirtualProtect((LPVOID)site, sizeof(replacement),
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    patch.site = site;
    memcpy(patch.original, original, sizeof(original));
    memcpy((void*)site, replacement, sizeof(replacement));
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, sizeof(replacement));

    DWORD ignored = 0;
    VirtualProtect((LPVOID)site, sizeof(replacement), oldProtect, &ignored);
    patch.installed = true;

    BYTE verify[5] = {};
    uptr verifyTarget = 0;
    if (!RdMem(site, verify, sizeof(verify)) ||
        !DecodeRelativeCall(site, verify, verifyTarget) ||
        verifyTarget != (uptr)&HookControlLabel) {
        RestoreLabelCallPatch(patch);
        return false;
    }

    return true;
}

static bool InstallLocalizationLabelHook()
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe)
        return false;

    g_delta = (uptr)exe - IMAGE_BASE_PREFERRED;
    g_originalLocalizationLookup =
        reinterpret_cast<LocalizationLookupFn>(VA_LOCALIZATION_LOOKUP + g_delta);

    if (!InstallOneLabelCallPatch(
            g_labelPatches[0], VA_LABEL_CALL_ROW + g_delta)) {
        g_originalLocalizationLookup = nullptr;
        return false;
    }

    if (!InstallOneLabelCallPatch(
            g_labelPatches[1], VA_LABEL_CALL_LIST + g_delta)) {
        RestoreLabelCallPatch(g_labelPatches[0]);
        g_originalLocalizationLookup = nullptr;
        return false;
    }

    return true;
}

static void UninstallLocalizationLabelHook()
{
    const bool listRestored = RestoreLabelCallPatch(g_labelPatches[1]);
    const bool rowRestored = RestoreLabelCallPatch(g_labelPatches[0]);

    if (listRestored && rowRestored)
        g_originalLocalizationLookup = nullptr;
}

static bool WrMem(uptr address, const void* data, size_t size)
{
    SIZE_T put = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, data, size, &put)) return false;
    return put == size;
}

template <typename T> static bool Rd(uptr address, T& value)
{
    return RdMem(address, &value, sizeof(T));
}

template <typename T> static bool Wr(uptr address, const T& value)
{
    return WrMem(address, &value, sizeof(T));
}

// ---- Call-site hook: C2F9A0 calls C2FDB0 at VA 0x00C2FB6B ----
static LONG FloatToBits(float value)
{
    LONG bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float BitsToFloat(LONG bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool ForceWindowLive()
{
    // Lifetime is input/state-driven, not timer-driven.
    return InterlockedCompareExchange(&g_forceActive, 0, 0) != 0;
}

// __fastcall mirrors the original __thiscall ABI: ECX keeps param_1 and EDX is
// a dummy register argument, so the original stack argument layout is retained.
static float __fastcall HookC2FDB0(short* trans, void* edxDummy, int* vehicle,
                                   int param3, float param4, float param5, float param6)
{
    (void)edxDummy;
    if (g_cfg.debug) InterlockedIncrement(&g_hookCalls);

    PFN_C2FDB0 original = g_originalC2FDB0;
    if (!original) return 0.0f;

    float nativeScalar = original(trans, vehicle, param3, param4, param5, param6);
    uptr targetVehicle = (uptr)(DWORD)InterlockedCompareExchange(&g_forceVehicle32, 0, 0);

    if (vehicle && (uptr)vehicle == targetVehicle && ForceWindowLive()) {
        // At a standstill Burnout, keep a small scalar floor only until actual
        // vehicle motion begins. It is state-based, not a duration timer.
        float scalarForMultiplier = nativeScalar;
        bool floorApplied = false;
        LONG standstill = InterlockedCompareExchange(&g_forceStandstill, 0, 0);
        LONG motionStarted = InterlockedCompareExchange(&g_launchMotionStarted, 0, 0);
        float currentGas = 0.0f;
        bool throttleOpen = Rd((uptr)vehicle + V_GAS, currentGas) &&
            IsFiniteFloat(currentGas) && currentGas >= 0.5f && currentGas <= 1.1f;

        // Do not synthesize drive force if the game has not registered throttle yet.
        if (standstill != 0 && motionStarted == 0 && throttleOpen &&
            g_cfg.standstillScalarFloor > 0.0f &&
            IsFiniteFloat(nativeScalar) && nativeScalar >= 0.0f &&
            nativeScalar < g_cfg.standstillScalarFloor) {
            scalarForMultiplier = g_cfg.standstillScalarFloor;
            if (g_cfg.debug) floorApplied = true;
        }

        float boosted = scalarForMultiplier * g_forceMultiplier;
        if (g_cfg.debug) {
            InterlockedIncrement(&g_hookMatchedCalls);
            InterlockedExchange(&g_lastNativeScalarBits, FloatToBits(nativeScalar));
            InterlockedExchange(&g_lastPreMultiplierScalarBits, FloatToBits(scalarForMultiplier));
            InterlockedExchange(&g_lastFloorApplied, floorApplied ? 1 : 0);
            InterlockedExchange(&g_lastBoostedScalarBits, FloatToBits(boosted));
            InterlockedExchange(&g_lastHookTick, (LONG)GetTickCount());
        }
        return boosted;
    }

    return nativeScalar;
}

static bool InstallScalarHook()
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        Log("SCALAR HOOK FAILED: GetModuleHandleA(NULL) returned null");
        InterlockedExchange(&g_hookInstallState, -1);
        return false;
    }

    g_delta = (uptr)exe - IMAGE_BASE_PREFERRED;
    g_callSite = VA_C2F9A0_CALL_C2FDB0 + g_delta;
    uptr expectedTarget = VA_C2FDB0 + g_delta;

    BYTE callBytes[5] = {};
    if (!RdMem(g_callSite, callBytes, sizeof(callBytes))) {
        Log("SCALAR HOOK FAILED: cannot read call site 0x%08X", (unsigned)g_callSite);
        InterlockedExchange(&g_hookInstallState, -1);
        return false;
    }
    if (callBytes[0] != 0xE8) {
        Log("SCALAR HOOK FAILED: call site opcode is 0x%02X, expected E8", callBytes[0]);
        InterlockedExchange(&g_hookInstallState, -1);
        return false;
    }

    int32_t oldRel = 0;
    memcpy(&oldRel, callBytes + 1, sizeof(oldRel));
    uptr actualTarget = g_callSite + 5 + (intptr_t)oldRel;
    if (actualTarget != expectedTarget) {
        Log("SCALAR HOOK FAILED: call target=0x%08X expected=0x%08X; no patch applied",
            (unsigned)actualTarget, (unsigned)expectedTarget);
        InterlockedExchange(&g_hookInstallState, -1);
        return false;
    }

    g_originalC2FDB0 = (PFN_C2FDB0)expectedTarget;
    uintptr_t hookAddress = (uintptr_t)&HookC2FDB0;
    int32_t newRel = (int32_t)((uint32_t)hookAddress - (uint32_t)(g_callSite + 5));
    BYTE patch[5] = { 0xE8, 0, 0, 0, 0 };
    memcpy(patch + 1, &newRel, sizeof(newRel));

    DWORD oldProtect = 0;
    if (!VirtualProtect((LPVOID)g_callSite, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("SCALAR HOOK FAILED: VirtualProtect failed, error=%lu", GetLastError());
        g_originalC2FDB0 = NULL;
        InterlockedExchange(&g_hookInstallState, -1);
        return false;
    }

    memcpy(g_originalCallBytes, callBytes, sizeof(callBytes));
    memcpy((void*)g_callSite, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)g_callSite, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect((LPVOID)g_callSite, sizeof(patch), oldProtect, &ignored);

    g_hookInstalled = true;
    InterlockedExchange(&g_hookInstallState, 1);
    Log("SCALAR HOOK INSTALLED: call=0x%08X native=0x%08X hook=0x%08X",
        (unsigned)g_callSite, (unsigned)expectedTarget, (unsigned)hookAddress);
    return true;
}

static void UninstallScalarHook()
{
    InterlockedExchange(&g_forceActive, 0);
    if (!g_hookInstalled || !g_callSite) return;

    DWORD oldProtect = 0;
    if (VirtualProtect((LPVOID)g_callSite, sizeof(g_originalCallBytes), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)g_callSite, g_originalCallBytes, sizeof(g_originalCallBytes));
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)g_callSite, sizeof(g_originalCallBytes));
        DWORD ignored = 0;
        VirtualProtect((LPVOID)g_callSite, sizeof(g_originalCallBytes), oldProtect, &ignored);
        g_hookInstalled = false;
    }
}

// ---- Experimental Control Mapping filter patch ----
// FUN_00B1ACA0: bypass only the JZ that filters control ID 0x88.
// Expected bytes: 74 03
// Patch bytes:    90 90
//
// This changes UI eligibility only. It does not register a new game action
// or change the native meaning of control ID 0x88.

static bool InstallMappingFilter88Patch()
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return false;

    g_delta = (uptr)exe - IMAGE_BASE_PREFERRED;
    g_mappingFilter88Address = VA_MAPPING_FILTER_88_BRANCH + g_delta;

    BYTE current[2] = {};
    if (!RdMem(g_mappingFilter88Address, current, sizeof(current)))
        return false;

    // Fail closed if the target differs from the expected GTA IV build.
    if (current[0] != 0x74 || current[1] != 0x03)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            (LPVOID)g_mappingFilter88Address,
            sizeof(current),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        return false;
    }

    memcpy(g_originalMappingFilter88Bytes, current, sizeof(current));

    const BYTE patch[2] = { 0x90, 0x90 };
    memcpy((void*)g_mappingFilter88Address, patch, sizeof(patch));

    g_mappingFilter88Installed = true;

    FlushInstructionCache(
        GetCurrentProcess(),
        (LPCVOID)g_mappingFilter88Address,
        sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(
        (LPVOID)g_mappingFilter88Address,
        sizeof(patch),
        oldProtect,
        &ignored);

    return true;
}

static void UninstallMappingFilter88Patch()
{
    if (!g_mappingFilter88Installed || !g_mappingFilter88Address)
        return;

    BYTE current[2] = {};
    if (!RdMem(g_mappingFilter88Address, current, sizeof(current)))
        return;

    // Do not overwrite another mod's change if the bytes no longer match ours.
    if (current[0] != 0x90 || current[1] != 0x90) {
        g_mappingFilter88Installed = false;
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            (LPVOID)g_mappingFilter88Address,
            sizeof(current),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        return;
    }

    memcpy(
        (void*)g_mappingFilter88Address,
        g_originalMappingFilter88Bytes,
        sizeof(g_originalMappingFilter88Bytes));

    FlushInstructionCache(
        GetCurrentProcess(),
        (LPCVOID)g_mappingFilter88Address,
        sizeof(g_originalMappingFilter88Bytes));

    DWORD ignored = 0;
    VirtualProtect(
        (LPVOID)g_mappingFilter88Address,
        sizeof(current),
        oldProtect,
        &ignored);

    g_mappingFilter88Installed = false;
}

// ---- Runtime executable version check (loaded dynamically; no version.lib dependency) ----
typedef DWORD (WINAPI *PFN_GFVISA)(LPCSTR, LPDWORD);
typedef BOOL  (WINAPI *PFN_GFVIA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI *PFN_VQVA)(LPCVOID, LPCSTR, LPVOID*, PUINT);

static bool CheckVersion()
{
    HMODULE hv = LoadLibraryA("version.dll");
    if (!hv) {
    Log("version.dll load failed");
        return !g_cfg.strictVersion;
    }

    PFN_GFVISA pSize = (PFN_GFVISA)GetProcAddress(hv, "GetFileVersionInfoSizeA");
    PFN_GFVIA pInfo = (PFN_GFVIA)GetProcAddress(hv, "GetFileVersionInfoA");
    PFN_VQVA pQuery = (PFN_VQVA)GetProcAddress(hv, "VerQueryValueA");
    if (!pSize || !pInfo || !pQuery) {
        Log("version.dll exports missing");
        FreeLibrary(hv);
        return !g_cfg.strictVersion;
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    DWORD dummy = 0;
    DWORD size = pSize(exePath, &dummy);
    if (!size) {
        Log("could not read GTAIV.exe version resource");
        FreeLibrary(hv);
        return !g_cfg.strictVersion;
    }

    BYTE* buffer = (BYTE*)malloc(size);
    bool ok = false;
    if (buffer && pInfo(exePath, 0, size, buffer)) {
        VS_FIXEDFILEINFO* info = NULL;
        UINT len = 0;
        if (pQuery(buffer, "\\", (LPVOID*)&info, &len) && info && len >= sizeof(VS_FIXEDFILEINFO)) {
            unsigned a = HIWORD(info->dwFileVersionMS);
            unsigned b = LOWORD(info->dwFileVersionMS);
            unsigned c = HIWORD(info->dwFileVersionLS);
            unsigned d = LOWORD(info->dwFileVersionLS);
            Log("GTAIV.exe file version: %u.%u.%u.%u", a, b, c, d);
            if (a == 1 && b == 2 && c == 0 && d == 59) ok = true;
        }
    }
    if (buffer) free(buffer);
    FreeLibrary(hv);

    if (!ok && g_cfg.strictVersion) Log("strict check failed; feature will not start");
    return ok || !g_cfg.strictVersion;
}

static bool GameFocused()
{
    HWND window = GetForegroundWindow();
    if (!window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

// Finds the local player's current vehicle, following the same pointer chain as the old working prototype.
static uptr GetPlayerVehicle()
{
    int index = -1;
    if (!Rd(VA_LOCAL_PLAYER_INDEX + g_delta, index) || index < 0 || index >= 0x20) return 0;

    uptr info = 0;
    if (!Rd(VA_PLAYER_INFO_ARRAY + g_delta + (uptr)index * 4, info) || info < 0x10000) return 0;

    uptr ped = 0;
    if (!Rd(info + 0x598, ped) || ped < 0x10000) return 0;

    unsigned char flags = 0;
    if (!Rd(ped + 0x26C, flags) || (flags & 4) == 0) return 0;

    uptr vehicle = 0;
    if (!Rd(ped + 0xB30, vehicle) || vehicle < 0x10000) return 0;
    return vehicle;
}

static bool ReadPos(uptr vehicle, float& x, float& y, float& z)
{
    uptr matrix = 0;
    if (!Rd(vehicle + V_MATRIX, matrix) || matrix < 0x10000) return false;
    return Rd(matrix + 0x30, x) && Rd(matrix + 0x34, y) && Rd(matrix + 0x38, z);
}

// CMatrix forward-axis layout (+0x10) is used only for telemetry projection.
// The displacement-based speed remains an estimate, not the game's internal velocity.
static bool ReadForwardAxis(uptr vehicle, float& fx, float& fy, float& fz)
{
    uptr matrix = 0;
    if (!Rd(vehicle + V_MATRIX, matrix) || matrix < 0x10000) return false;
    if (!Rd(matrix + 0x10, fx) || !Rd(matrix + 0x14, fy) || !Rd(matrix + 0x18, fz)) return false;
    if (!IsFiniteFloat(fx) || !IsFiniteFloat(fy) || !IsFiniteFloat(fz)) return false;
    float length = sqrtf(fx * fx + fy * fy + fz * fz);
    if (!IsFiniteFloat(length) || length < 0.5f || length > 1.5f) return false;
    fx /= length; fy /= length; fz /= length;
    return true;
}

static bool ReadMappedBurnoutKey(
    bool& held,
    WORD& scanCode,
    BYTE& rawState)
{
    held = false;
    scanCode = 0;
    rawState = 0;

    // Row 49: mapping record + 2 contains the DIK scan code.
    const uptr mappingAddress =
        VA_CONTROL_MAP_A + g_delta
        + (uptr)BURNOUT_MAPPING_ROW * 4 + 2;

    if (!Rd(mappingAddress, scanCode))
        return false;

    // Zero and out-of-range values are not usable keyboard scan codes.
    // An unassigned 0xFFFF binding is rejected by this check as well.
    if (scanCode == 0 || scanCode > 0xFF)
        return false;

    DWORD bank = 0;

    if (!Rd(VA_INPUT_STATE_BANK + g_delta, bank)
        || bank > 1)
        return false;

    const uptr stateAddress =
        VA_INPUT_STATE_BASE + g_delta
        + (uptr)bank * 0x100 + scanCode;

    if (!Rd(stateAddress, rawState))
        return false;

    held = rawState != 0;
    return true;
}

struct VehInfo
{
    uptr handling;
    float mass;
    int drivenWheels;
    int wheelCount;
};

static bool Classify(uptr vehicle, VehInfo& info, char* why, size_t whySize)
{
    memset(&info, 0, sizeof(info));
    int type = -1;
    int wheelCount = -1;
    uptr wheels = 0;
    uptr handling = 0;

    if (!Rd(vehicle + V_TYPE, type) || type != 0) {
        if (why && whySize) snprintf(why, whySize, "vehicle type=%d (expected 0/passenger car)", type);
        return false;
    }
    if (!Rd(vehicle + V_WHEEL_COUNT, wheelCount) || wheelCount < 2 || wheelCount > 6) {
        if (why && whySize) snprintf(why, whySize, "wheel count=%d", wheelCount);
        return false;
    }
    if (!Rd(vehicle + V_WHEELS, wheels) || wheels < 0x10000) {
        if (why && whySize) snprintf(why, whySize, "invalid wheel-array pointer");
        return false;
    }

    int driven = 0;
    for (int i = 0; i < wheelCount; ++i) {
        int flags = 0;
        if (!Rd(wheels + (uptr)i * WHEEL_STRIDE + W_FLAGS, flags)) {
            if (why && whySize) snprintf(why, whySize, "could not read wheel %d", i);
            return false;
        }
        if ((flags & 0x8000) != 0) ++driven;
    }
    if (driven != 2 && driven != 4) {
        if (why && whySize) snprintf(why, whySize, "driven-wheel count=%d (expected 2 or 4)", driven);
        return false;
    }

    if (!Rd(vehicle + V_HANDLING, handling) || handling < 0x10000) {
        if (why && whySize) snprintf(why, whySize, "invalid handling pointer");
        return false;
    }

    float mass = 0.0f;
    if (!Rd(handling + H_MASS, mass) || !(mass >= 100.0f && mass <= 100000.0f)) {
        if (why && whySize) snprintf(why, whySize, "handling mass sanity check failed");
        return false;
    }

    info.handling = handling;
    info.mass = mass;
    info.drivenWheels = driven;
    info.wheelCount = wheelCount;
    if (why && whySize) snprintf(why, whySize, "mass=%.0f wheels=%d driven=%d", (double)mass, wheelCount, driven);
    return true;
}

static void InitCsv()
{
    if (!g_cfg.debug || !g_csvPath[0]) return;
    FILE* f = fopen(g_csvPath, "w");
    if (!f) return;
    fprintf(f, "tick_ms,event,phase,vehicle,gear,gas,state1094,clutch10A0,speed_filtered,speed_raw,longitudinal_speed,elapsed_ms,motion_ms,handling_driveforce,force_multiplier,assist_active,hook_calls,hook_matched,last_native_scalar,last_boosted_scalar,last_floor_applied,last_scalar_for_multiplier,assist_mode,driven_wheel_count,valid_driven_wheel_count,driven_surface_avg,driven_surface_max,driven_slip_avg,driven_slip_max");
    for (int i = 0; i < 6; ++i)
        fprintf(f, ",wheel%d_driven,wheel%d_omega,wheel%d_radius,wheel%d_surface,wheel%d_slip", i, i, i, i, i);
    fputc('\n', f);
    fclose(f);
}

enum AssistPhase
{
    PHASE_IDLE = 0,
    PHASE_WAITING_FOR_NATIVE_SLIP,
    PHASE_ACTIVE_LAUNCH,
    PHASE_ACTIVE_ROLLING,
    PHASE_DONE
};

static AssistPhase g_phase = PHASE_IDLE;
static uptr g_assistVehicle = 0;
static DWORD g_assistStartTick = 0;
static short g_assistStartGear = -1;
static bool g_motionStartLogged = false;
static DWORD g_motionStartTick = 0;
static DWORD g_lastCsvSample = 0;
static unsigned long g_lastRollingSampleSerial = 0;
static int g_rollingSlipSamples = 0;
static bool g_hookReady = false;
static DWORD g_telemetryUntilTick = 0;

// Rolling activation is based on observed native slip, not elapsed time.
static const float ROLLING_THROTTLE_THRESHOLD = 0.85f;
static const float ROLLING_MIN_LONGITUDINAL_SPEED = 3.0f;
static const float ROLLING_SLIP_THRESHOLD = 0.25f;
static const int ROLLING_CONFIRM_SAMPLES = 2;

static const char* PhaseName(AssistPhase phase)
{
    switch (phase) {
    case PHASE_WAITING_FOR_NATIVE_SLIP: return "WAIT_NATIVE_SLIP";
    case PHASE_ACTIVE_LAUNCH: return "ACTIVE_LAUNCH";
    case PHASE_ACTIVE_ROLLING: return "ACTIVE_ROLLING";
    case PHASE_DONE: return "DONE";
    default: return "IDLE";
    }
}

static int AssistModeCode()
{
    switch (g_phase) {
    case PHASE_WAITING_FOR_NATIVE_SLIP: return 2;
    case PHASE_ACTIVE_LAUNCH: return 1;
    case PHASE_ACTIVE_ROLLING: return 3;
    default: return 0;
    }
}

static float ReadDriveForce(uptr vehicle)
{
    uptr handling = 0;
    float driveForce = -1.0f;
    if (Rd(vehicle + V_HANDLING, handling) && handling >= 0x10000)
        Rd(handling + 0x44, driveForce);
    return driveForce;
}

// Wheel records are sampled read-only. +0x80 is the omega candidate and +0x08
// the radius candidate from the current static analysis; their in-game units are
// not yet independently validated. Surface speed is abs(omega * radius).
struct WheelSample
{
    int driven;
    float omega;
    float radius;
    float surfaceSpeed;
    float slipEstimate;
    bool valid;
    bool slipValid;
};

struct WheelTelemetry
{
    int wheelCount;
    int drivenCount;
    int validDrivenCount;
    float surfaceAverage;
    float surfaceMaximum;
    float slipAverage;
    float slipMaximum;
    WheelSample wheel[6];
};

static void ReadWheelTelemetry(uptr vehicle, float longitudinalSpeed, WheelTelemetry& out)
{
    memset(&out, 0, sizeof(out));
    out.surfaceAverage = out.surfaceMaximum = -999.0f;
    out.slipAverage = out.slipMaximum = -999.0f;
    for (int i = 0; i < 6; ++i) {
        out.wheel[i].omega = -999.0f;
        out.wheel[i].radius = -999.0f;
        out.wheel[i].surfaceSpeed = -999.0f;
        out.wheel[i].slipEstimate = -999.0f;
    }

    uptr wheelBase = 0;
    int count = 0;
    if (!Rd(vehicle + V_WHEELS, wheelBase) || wheelBase < 0x10000 ||
        !Rd(vehicle + V_WHEEL_COUNT, count) || count < 2 || count > 6) return;
    out.wheelCount = count;

    float surfaceSum = 0.0f;
    float slipSum = 0.0f;
    float surfaceMax = -999.0f;
    float slipMax = -999.0f;
    float bodySpeed = IsFiniteFloat(longitudinalSpeed) && longitudinalSpeed > -900.0f
        ? fabsf(longitudinalSpeed) : -999.0f;

    for (int i = 0; i < count; ++i) {
        uptr wheel = wheelBase + (uptr)i * WHEEL_STRIDE;
        int flags = 0;
        float omega = -999.0f;
        float radius = -999.0f;
        bool flagsOk = Rd(wheel + W_FLAGS, flags);
        bool omegaOk = Rd(wheel + 0x80, omega);
        bool radiusOk = Rd(wheel + 0x08, radius);
        WheelSample& sample = out.wheel[i];
        sample.driven = (flagsOk && (flags & 0x8000) != 0) ? 1 : 0;
        if (sample.driven) ++out.drivenCount;

        if (!omegaOk || !radiusOk || !IsFiniteFloat(omega) || !IsFiniteFloat(radius) ||
            fabsf(omega) > 100000.0f || radius <= 0.001f || radius > 20.0f) continue;

        float surface = fabsf(omega * radius);
        if (!IsFiniteFloat(surface) || surface > 100000.0f) continue;
        sample.omega = omega;
        sample.radius = radius;
        sample.surfaceSpeed = surface;
        sample.valid = true;

        if (bodySpeed > -900.0f) {
            // A stable low-speed denominator avoids an unbounded ratio at rest.
            // This is an estimate: (wheel surface speed - |vehicle longitudinal speed|)
            // / max(|vehicle longitudinal speed|, 1.0 game unit/s).
            float denominator = (bodySpeed < 1.0f) ? 1.0f : bodySpeed;
            float slip = (surface - bodySpeed) / denominator;
            if (IsFiniteFloat(slip) && fabsf(slip) < 10000.0f) {
                sample.slipEstimate = slip;
                sample.slipValid = true;
            }
        }

        if (sample.driven) {
            ++out.validDrivenCount;
            surfaceSum += surface;
            if (surface > surfaceMax) surfaceMax = surface;
            if (sample.slipValid) {
                slipSum += sample.slipEstimate;
                if (out.validDrivenCount == 1 || sample.slipEstimate > slipMax) slipMax = sample.slipEstimate;
            }
        }
    }

    if (out.validDrivenCount > 0) {
        out.surfaceAverage = surfaceSum / (float)out.validDrivenCount;
        out.surfaceMaximum = surfaceMax;
        int slipCount = 0;
        for (int i = 0; i < count; ++i)
            if (out.wheel[i].driven && out.wheel[i].slipValid) ++slipCount;
        if (slipCount > 0) {
            out.slipAverage = slipSum / (float)slipCount;
            out.slipMaximum = slipMax;
        }
    }
}

static DWORD TelemetryRemainingMs(DWORD now)
{
    LONG remaining = (LONG)(g_telemetryUntilTick - now);
    return remaining > 0 ? (DWORD)remaining : 0;
}

static void CsvSample(DWORD now, const char* event, uptr vehicle, short gear,
                      float gas, float state, float clutch, float speedFiltered,
                      float speedRaw, int assistMode)
{
    // Never create a file, read wheel telemetry, or append a sample in quiet mode.
    if (!g_cfg.debug || !g_csvPath[0]) return;
    FILE* f = fopen(g_csvPath, "a");
    if (!f) return;
    DWORD elapsed = (g_assistStartTick != 0) ? now - g_assistStartTick : 0;
    DWORD motionMs = g_motionStartLogged ? g_motionStartTick - g_assistStartTick : 0;
    float assistActive = (float)(InterlockedCompareExchange(&g_forceActive, 0, 0) != 0 ? 1 : 0);
    float nativeScalar = BitsToFloat(InterlockedCompareExchange(&g_lastNativeScalarBits, 0, 0));
    float boostedScalar = BitsToFloat(InterlockedCompareExchange(&g_lastBoostedScalarBits, 0, 0));
    float scalarForMultiplier = BitsToFloat(InterlockedCompareExchange(&g_lastPreMultiplierScalarBits, 0, 0));
    int floorApplied = (int)InterlockedCompareExchange(&g_lastFloorApplied, 0, 0);
    WheelTelemetry wheelData;
    ReadWheelTelemetry(vehicle, g_speedLongitudinalRaw, wheelData);
    fprintf(f, "%lu,%s,%s,0x%08X,%d,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%lu,%lu,%.5f,%.3f,%.0f,%ld,%ld,%.7g,%.7g,%d,%.7g,%d,%d,%d,%.5f,%.5f,%.5f,%.5f",
        (unsigned long)now, event ? event : "", PhaseName(g_phase), (unsigned)vehicle,
        (int)gear, (double)gas, (double)state, (double)clutch,
        (double)speedFiltered, (double)speedRaw, (double)g_speedLongitudinalRaw,
        (unsigned long)elapsed, (unsigned long)motionMs, (double)ReadDriveForce(vehicle),
        (double)g_forceMultiplier, (double)assistActive,
        (long)InterlockedCompareExchange(&g_hookCalls, 0, 0),
        (long)InterlockedCompareExchange(&g_hookMatchedCalls, 0, 0),
        (double)nativeScalar, (double)boostedScalar, floorApplied,
        (double)scalarForMultiplier, assistMode,
        wheelData.drivenCount, wheelData.validDrivenCount,
        (double)wheelData.surfaceAverage, (double)wheelData.surfaceMaximum,
        (double)wheelData.slipAverage, (double)wheelData.slipMaximum);
    for (int i = 0; i < 6; ++i) {
        const WheelSample& w = wheelData.wheel[i];
        if (i < wheelData.wheelCount)
            fprintf(f, ",%d,%.6f,%.6f,%.6f,%.6f", w.driven,
                (double)w.omega, (double)w.radius, (double)w.surfaceSpeed, (double)w.slipEstimate);
        else
            fprintf(f, ",-1,-999,-999,-999,-999");
    }
    fputc('\n', f);
    fclose(f);
}

static void ReadVehicleSample(uptr vehicle, float& gas, float& state, float& clutch)
{
    gas = state = clutch = -1.0f;
    Rd(vehicle + V_GAS, gas);
    Rd(vehicle + V_RPM_STATE, state);
    Rd(vehicle + V_CLUTCH, clutch);
}

static void StopForceAssist(const char* reason, DWORD now)
{
    LONG wasActive = InterlockedExchange(&g_forceActive, 0);
    (void)wasActive;
    (void)reason;
    (void)now;
    InterlockedExchange(&g_forceVehicle32, 0);
    InterlockedExchange(&g_forceStandstill, 0);
    InterlockedExchange(&g_launchMotionStarted, 0);
}

static void ResetAssistState()
{
    g_phase = PHASE_IDLE;
    g_assistVehicle = 0;
    g_assistStartTick = 0;
    g_assistStartGear = -1;
    g_motionStartLogged = false;
    g_motionStartTick = 0;
    g_lastCsvSample = 0;
    g_lastRollingSampleSerial = g_motionSampleSerial;
    g_rollingSlipSamples = 0;
    g_telemetryUntilTick = 0;
}

static void AbortAssist(const char* reason, DWORD now)
{
    StopForceAssist(reason, now);
    ResetAssistState();
}

static void BeginAssist(uptr vehicle, short gear, DWORD now, float speedFiltered,
                        float speedRaw, bool comboHeld)
{
    if (!g_hookReady) {
        Log("ASSIST NOT STARTED: native scalar hook is not installed; telemetry-only mode");
        return;
    }

    g_assistVehicle = vehicle;
    g_assistStartGear = gear;
    g_assistStartTick = g_cfg.debug ? now : 0;
    g_telemetryUntilTick = 0;
    g_motionStartLogged = speedRaw >= g_cfg.standstillSpeedThreshold;
    g_motionStartTick = (g_cfg.debug && g_motionStartLogged) ? now : 0;
    g_lastCsvSample = 0;
    g_lastRollingSampleSerial = g_motionSampleSerial;
    g_rollingSlipSamples = 0;

    bool fromStandstill = speedRaw < g_cfg.standstillSpeedThreshold;
    g_phase = fromStandstill ? PHASE_ACTIVE_LAUNCH : PHASE_WAITING_FOR_NATIVE_SLIP;

    // Clear previous-hook telemetry, then publish the selected vehicle and mode.
    InterlockedExchange(&g_forceActive, 0);
    if (g_cfg.debug) {
        InterlockedExchange(&g_hookMatchedCalls, 0);
        InterlockedExchange(&g_lastNativeScalarBits, FloatToBits(0.0f));
        InterlockedExchange(&g_lastPreMultiplierScalarBits, FloatToBits(0.0f));
        InterlockedExchange(&g_lastFloorApplied, 0);
        InterlockedExchange(&g_lastBoostedScalarBits, FloatToBits(0.0f));
    }
    InterlockedExchange(&g_forceStandstill, fromStandstill ? 1 : 0);
    InterlockedExchange(&g_launchMotionStarted, fromStandstill ? 0 : 1);
    InterlockedExchange(&g_forceVehicle32, (LONG)(DWORD)vehicle);

    // A standstill burnout is an intentional launch request. In rolling conditions,
    // wait for native wheel slip first so a key press alone cannot manufacture it.
    if (fromStandstill && comboHeld)
        InterlockedExchange(&g_forceActive, 1);

    if (g_cfg.debug) {
        float gas = -1.0f, state = -1.0f, clutch = -1.0f;
        ReadVehicleSample(vehicle, gas, state, clutch);
        const char* mode = fromStandstill ? "STANDSTILL_LAUNCH" : "ROLLING_WAIT_FOR_NATIVE_SLIP";
        Log("ASSIST START: mode=%s vehicle=0x%08X gear=%d WShift=%d gas=%.3f state1094=%.3f clutchReadOnly=%.3f K=%.2f noForceTimer=1 floor=%.3f untilMotion=1 rollingSlipThreshold=%.2f minLongSpeed=%.2f confirmSamples=%d speedFiltered=%.3f speedRaw=%.3f longitudinalSpeed=%.3f",
            mode, (unsigned)vehicle, (int)gear, comboHeld ? 1 : 0,
            (double)gas, (double)state, (double)clutch,
            (double)g_forceMultiplier, (double)g_cfg.standstillScalarFloor,
            (double)ROLLING_SLIP_THRESHOLD, (double)ROLLING_MIN_LONGITUDINAL_SPEED,
            ROLLING_CONFIRM_SAMPLES,
            (double)speedFiltered, (double)speedRaw, (double)g_speedLongitudinalRaw);
    }
}

static bool NativeRollingSlipDetected(uptr vehicle, float& slipMaximum, float& gas)
{
    // This is the minimum live read needed for rolling activation, not diagnostic
    // telemetry: no arrays, history, files, or per-wheel samples are retained.
    slipMaximum = -999.0f;
    gas = -1.0f;
    if (!Rd(vehicle + V_GAS, gas) || !IsFiniteFloat(gas) || gas < ROLLING_THROTTLE_THRESHOLD)
        return false;
    if (!IsFiniteFloat(g_speedLongitudinalRaw) || g_speedLongitudinalRaw <= -900.0f ||
        fabsf(g_speedLongitudinalRaw) < ROLLING_MIN_LONGITUDINAL_SPEED)
        return false;

    uptr wheelBase = 0;
    int count = 0;
    if (!Rd(vehicle + V_WHEELS, wheelBase) || wheelBase < 0x10000 ||
        !Rd(vehicle + V_WHEEL_COUNT, count) || count < 2 || count > 6)
        return false;

    const float bodySpeed = fabsf(g_speedLongitudinalRaw);
    const float denominator = bodySpeed < 1.0f ? 1.0f : bodySpeed;
    bool foundDrivenWheel = false;
    float maxSlip = -999.0f;
    for (int i = 0; i < count; ++i) {
        const uptr wheel = wheelBase + (uptr)i * WHEEL_STRIDE;
        int flags = 0;
        float omega = 0.0f, radius = 0.0f;
        if (!Rd(wheel + W_FLAGS, flags) || (flags & 0x8000) == 0) continue;
        if (!Rd(wheel + 0x80, omega) || !Rd(wheel + 0x08, radius)) continue;
        if (!IsFiniteFloat(omega) || !IsFiniteFloat(radius) || fabsf(omega) > 100000.0f ||
            radius <= 0.001f || radius > 20.0f)
            continue;

        const float surfaceSpeed = fabsf(omega * radius);
        const float slip = (surfaceSpeed - bodySpeed) / denominator;
        if (!IsFiniteFloat(surfaceSpeed) || surfaceSpeed > 100000.0f ||
            !IsFiniteFloat(slip) || fabsf(slip) >= 10000.0f)
            continue;
        if (!foundDrivenWheel || slip > maxSlip) maxSlip = slip;
        foundDrivenWheel = true;
    }

    if (!foundDrivenWheel) return false;
    slipMaximum = maxSlip;
    return maxSlip >= ROLLING_SLIP_THRESHOLD;
}

static void TickAssist(uptr vehicle, short gear, DWORD now, float speedFiltered,
                       float speedRaw, bool comboHeld, bool gearOk)
{
    if (g_phase == PHASE_IDLE) return;

    if (vehicle != g_assistVehicle) {
        AbortAssist("vehicle changed", now);
        return;
    }

    // DONE is only the telemetry tail after the key combo has been released.
    if (g_phase == PHASE_DONE) {
        if (g_cfg.debug && TelemetryRemainingMs(now) > 0 &&
            (g_lastCsvSample == 0 || now - g_lastCsvSample >= (DWORD)g_cfg.debugSampleMs)) {
            float gas = -1.0f, state = -1.0f, clutch = -1.0f;
            ReadVehicleSample(vehicle, gas, state, clutch);
            g_lastCsvSample = now;
        }
        return;
    }

    if (!comboHeld) {
        if (g_cfg.debug) {
            float gas = -1.0f, state = -1.0f, clutch = -1.0f;
            ReadVehicleSample(vehicle, gas, state, clutch);
        }
        StopForceAssist("W+LeftShift released", now);
        g_phase = PHASE_DONE;
        g_telemetryUntilTick = g_cfg.debug ? now + (DWORD)g_cfg.telemetryTailMs : 0;
        g_lastCsvSample = 0;
        return;
    }

    // This is a moving-state indicator, not a timer. Once motion begins, the
    // launch scalar floor is removed, but the held assist remains active.
    if (!g_motionStartLogged && speedRaw >= g_cfg.standstillSpeedThreshold) {
        g_motionStartLogged = true;
        if (g_cfg.debug) g_motionStartTick = now;
        InterlockedExchange(&g_launchMotionStarted, 1);
        if (g_cfg.debug) {
            Log("MOTION START: elapsed=%lu ms threshold=%.3f rawSpeed=%.3f filteredSpeed=%.3f",
                (unsigned long)(now - g_assistStartTick),
                (double)g_cfg.standstillSpeedThreshold,
                (double)speedRaw, (double)speedFiltered);
            float gas = -1.0f, state = -1.0f, clutch = -1.0f;
            ReadVehicleSample(vehicle, gas, state, clutch);
        }
    }

    // During a rolling activation, only engage the multiplier after native physics
    // has produced slip at high throttle. Two consecutive fresh movement samples debounce noise.
    if (g_phase == PHASE_WAITING_FOR_NATIVE_SLIP &&
        g_motionSampleSerial != g_lastRollingSampleSerial) {
        // Only evaluate after a fresh longitudinal-speed sample; no elapsed-time trigger.
        g_lastRollingSampleSerial = g_motionSampleSerial;
        float slipMaximum = -999.0f, gas = -1.0f;
        bool slipping = NativeRollingSlipDetected(vehicle, slipMaximum, gas);
        if (slipping) ++g_rollingSlipSamples;
        else g_rollingSlipSamples = 0;

        if (g_rollingSlipSamples >= ROLLING_CONFIRM_SAMPLES) {
            g_phase = PHASE_ACTIVE_ROLLING;
            InterlockedExchange(&g_forceStandstill, 0);
            InterlockedExchange(&g_launchMotionStarted, 1);
            InterlockedExchange(&g_forceActive, 1);
            if (g_cfg.debug) {
                Log("ROLLING SLIP CONFIRMED: assist engaged gear=%d gas=%.3f longitudinal=%.3f maxSlip=%.3f; stays active while W+LeftShift held",
                    (int)gear, (double)gas, (double)g_speedLongitudinalRaw, (double)slipMaximum);
                float state = -1.0f, clutch = -1.0f;
                ReadVehicleSample(vehicle, gas, state, clutch);
            }
        }
    }

    // No gear-change reset. The native automatic gearbox controls its own shift;
    // our scalar assist is held across the shift as long as W+LeftShift stays held.
    // gearOk is intentionally not used here because the gear field may be transient
    // during the automatic shift. It is checked only when starting a new assist.
    (void)gearOk;

    if (g_cfg.debug && (g_lastCsvSample == 0 || now - g_lastCsvSample >= (DWORD)g_cfg.debugSampleMs)) {
        float gas = -1.0f, state = -1.0f, clutch = -1.0f;
        ReadVehicleSample(vehicle, gas, state, clutch);
        g_lastCsvSample = now;
    }
}

static DWORD WINAPI Worker(LPVOID)
{
    Log("worker started (no ScriptHook dependency)");
    LARGE_INTEGER frequency, last, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);

    uptr currentVehicle = 0;
    VehInfo vehicleInfo;
    memset(&vehicleInfo, 0, sizeof(vehicleInfo));
    bool eligible = false;
    bool havePrevPos = false;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float sampleTime = 0.0f, speedFiltered = 0.0f, speedRaw = 0.0f;
    bool prevWantLaunch = false;

    while (!g_stop) {
        Sleep(2);
        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - last.QuadPart) / (double)frequency.QuadPart);
        last = now;
        if (dt <= 0.0f || dt > 0.5f) {
            havePrevPos = false;
            sampleTime = 0.0f;
            speedRaw = 0.0f;
            continue;
        }

        uptr vehicle = GameFocused() ? GetPlayerVehicle() : 0;
        if (vehicle != currentVehicle) {
            if (g_phase != PHASE_IDLE) AbortAssist("vehicle changed", GetTickCount());
            currentVehicle = vehicle;
            eligible = false;
            havePrevPos = false;
            sampleTime = 0.0f;
            speedFiltered = 0.0f;
            speedRaw = 0.0f;
            g_speedLongitudinalRaw = -999.0f;
            prevWantLaunch = false;
            if (currentVehicle) {
                eligible = Classify(currentVehicle, vehicleInfo, NULL, 0);
            }
        }

        DWORD t = GetTickCount();
        if (!currentVehicle || !eligible) {
            if (g_phase != PHASE_IDLE) AbortAssist("no eligible player vehicle", t);
            prevWantLaunch = false;
            continue;
        }

        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (ReadPos(currentVehicle, x, y, z)) {
            if (!havePrevPos) {
                px = x; py = y; pz = z;
                sampleTime = 0.0f;
                havePrevPos = true;
            } else {
                sampleTime += dt;
                if (sampleTime >= 0.03f) {
                    float dx = x - px, dy = y - py, dz = z - pz;
                    float measured = sqrtf(dx * dx + dy * dy + dz * dz) / sampleTime;
                    if (measured > 150.0f || !IsFiniteFloat(measured)) measured = 0.0f;
                    speedRaw = measured;
                    speedFiltered = speedFiltered * 0.5f + measured * 0.5f;
                    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                    if (ReadForwardAxis(currentVehicle, fx, fy, fz)) {
                        float longitudinal = (dx * fx + dy * fy + dz * fz) / sampleTime;
                        if (IsFiniteFloat(longitudinal) && fabsf(longitudinal) <= 150.0f)
                            g_speedLongitudinalRaw = longitudinal;
                        else
                            g_speedLongitudinalRaw = -999.0f;
                    } else {
                        g_speedLongitudinalRaw = -999.0f;
                    }
                    px = x; py = y; pz = z;
                    sampleTime = 0.0f;
                    ++g_motionSampleSerial;
                }
            }
        } else {
            havePrevPos = false;
            g_speedLongitudinalRaw = -999.0f;
        }

        bool wHeld = (GetAsyncKeyState('W') & 0x8000) != 0;

		bool mappedHeld = false;
		WORD mappedScanCode = 0;
		BYTE mappedState = 0;
		
		bool mappingAvailable = ReadMappedBurnoutKey(
		    mappedHeld, mappedScanCode, mappedState);
		
		bool keyHeld = mappingAvailable
		    ? mappedHeld
		    : (GetAsyncKeyState(g_cfg.key) & 0x8000) != 0;
		
		bool comboHeld = g_cfg.enabled && wHeld && keyHeld;

        short gear = -1;
        Rd(currentVehicle + V_TRANSMISSION, gear);
        bool gearOk = g_cfg.allGears ? (gear >= 1) : (gear == 1);
        bool wantAssist = comboHeld && gearOk;

        if (g_cfg.burnoutAssist && g_hookReady && wantAssist && !prevWantLaunch &&
            (g_phase == PHASE_IDLE || g_phase == PHASE_DONE)) {
            BeginAssist(currentVehicle, gear, t, speedFiltered, speedRaw, comboHeld);
        }

        TickAssist(currentVehicle, gear, t, speedFiltered, speedRaw, comboHeld, gearOk);

        // A new assist may be started only after the eligible combo has been released.
        if (!wantAssist && g_phase == PHASE_DONE &&
            (!g_cfg.debug || TelemetryRemainingMs(t) == 0)) {
            ResetAssistState();
        }
        prevWantLaunch = wantAssist;

    }

    AbortAssist("worker stopping", GetTickCount());
    StopForceAssist("worker stopping", GetTickCount());
    Log("worker stopped");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);

        if (!BuildSiblingPath(g_iniPath, sizeof(g_iniPath), path, "BurnoutAssist.ini"))
            g_iniPath[0] = '\0';

        strncpy(g_logPath, path, MAX_PATH - 1);
        g_logPath[MAX_PATH - 1] = '\0';
        char* dot = strrchr(g_logPath, '.');
        if (dot) strcpy(dot, ".log");

        const bool iniReady = g_iniPath[0] && CreateDefaultIniIfMissing();
        LoadConfig();

        HMODULE exe = GetModuleHandleA(NULL);
        g_delta = (uptr)exe - IMAGE_BASE_PREFERRED;

        if (g_cfg.debug) {
            strncpy(g_csvPath, path, MAX_PATH - 1);
            g_csvPath[MAX_PATH - 1] = '\0';
            dot = strrchr(g_csvPath, '.');
            if (dot) strcpy(dot, ".csv");
        }

        if (!g_cfg.enabled || !g_cfg.burnoutAssist) {
            LogStatus("BurnoutAssist v0.4.1: mod disabled.");
            return TRUE;
        }
        if (!CheckVersion()) {
            LogStatus("BurnoutAssist v0.4.1: compatible game version not found; mod inactive.");
            return TRUE;
        }

        g_hookReady = InstallScalarHook();
        if (!g_hookReady) {
            LogStatus("BurnoutAssist v0.4.1: hook installation failed; mod inactive.");
            return TRUE;
        }

        const bool labelHookReady = InstallLocalizationLabelHook();
        InstallMappingFilter88Patch();

        if (g_cfg.debug) InitCsv();
        g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (!g_thread) {
            UninstallMappingFilter88Patch();
            UninstallLocalizationLabelHook();
            UninstallScalarHook();
            LogStatus("BurnoutAssist v0.4.1: initialization failed; mod inactive.");
            return TRUE;
        }

        char status[256];
        snprintf(status, sizeof(status),
            "BurnoutAssist v0.4.1: active; label=%s; Control Mapping row 0x88=%s%s",
            labelHookReady ? "renamed" : "unchanged",
            g_mappingFilter88Installed ? "exposed" : "not exposed",
            iniReady ? "" : "; warning: default INI could not be created");
        LogStatus(status);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_stop = true;
        InterlockedExchange(&g_forceActive, 0);
        UninstallLocalizationLabelHook();
        UninstallMappingFilter88Patch();
        // Do not manually unload this ASI while the game is running; normal usage is
        // process-lifetime. Restoring the call site here is a best-effort safety net.
        UninstallScalarHook();
    }
    return TRUE;
}