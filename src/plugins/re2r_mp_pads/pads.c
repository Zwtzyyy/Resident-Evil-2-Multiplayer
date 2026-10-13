// re2r_mp_pads.dll
// Clean-room reimplementation of the reference "re2coop_pads.dll" behaviour
// (legacy DirectInput/joystick polling exposed to the Lua mod).
//
// Contract (see docs/RE/03-protocols.md):
//   * writes  <game>\reframework\data\reunited_pads.txt  (atomically via .tmp)
//       addr <uint64>
//       alive <uint32>
//       pad <id> <vid:%04X> <pid:%04X> <xinput:0|1> <nbuttons> <naxes> <name>\r\n
//   * publishes a RAM block at <addr>: 64-byte header, then one 64-byte entry per pad:
//       [0]=flags DWORD  [4]=buttons DWORD  [8]=pov DWORD  [12..]=6x float axes
//
// The reference file/format names were ported (see docs/RE/01-lua-map.md); the
// filenames are ours now: reunited_pads.txt.
#include "../plugin_common.h"

#define RE2RMP_MAX_PADS 16
#define RE2RMP_ENTRY 64

typedef struct {
    volatile unsigned int flags;
    volatile unsigned int buttons;
    volatile unsigned int pov;
    volatile float axes[6];
    unsigned char pad[RE2RMP_ENTRY - 12 - 24];
} PadEntry;

typedef struct {
    unsigned char header[RE2RMP_ENTRY];
    PadEntry pads[RE2RMP_MAX_PADS];
} PadBlock;

static PadBlock g_block;                 // published to the Lua via its address
static volatile unsigned int g_alive = 0; // heartbeat counter
static volatile LONG g_run = 0;
static HANDLE g_thread = NULL;

// Microsoft & partner vendor IDs whose pads are seen as XInput by the game already.
static int is_xinput_vendor(WORD vid)
{
    switch (vid) {
    case 0x045E: // Microsoft
    case 0x0738: // Mad Catz
    case 0x0F0D: // HORI
    case 0x24C6: // PDP / PowerA
    case 0x1430: // RedOctane
        return 1;
    default:
        return 0;
    }
}

static void oem_name(WORD vid, WORD pid, wchar_t* out, size_t cap)
{
    wchar_t sub[256];
    swprintf(sub, 256,
             L"SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties"
             L"\\Joystick\\OEM\\VID_%04X&PID_%04X",
             vid, pid);
    DWORD sz = (DWORD)(cap * sizeof(wchar_t));
    out[0] = L'\0';
    RegGetValueW(HKEY_LOCAL_MACHINE, sub, L"OEMName", RRF_RT_REG_SZ, NULL, out, &sz);
}

static int to_utf8(const wchar_t* in, char* out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    return n - 1;
}

static float axis_norm(DWORD pos)
{
    return ((float)((int)pos - 32767)) / 32767.0f;
}

static void poll_once(void)
{
    UINT n = joyGetNumDevs();
    if (n > RE2RMP_MAX_PADS)
        n = RE2RMP_MAX_PADS;

    wchar_t base[1024];
    re2rmp_game_root(base, 1024);
    wchar_t tmp[1152], final[1152];
    re2rmp_path(tmp, 1152, base, L"reframework\\data\\reunited_pads.tmp");
    re2rmp_path(final, 1152, base, L"reframework\\data\\reunited_pads.txt");

    FILE* f = _wfopen(tmp, L"wb");
    if (f != NULL) {
        fprintf(f, "addr %llu\n", (unsigned long long)(uintptr_t)&g_block);
        fprintf(f, "alive %u\n", g_alive);

        UINT reported = 0;
        for (UINT i = 0; i < n; ++i) {
            JOYCAPSW caps;
            if (joyGetDevCapsW(i, &caps, sizeof(caps)) != JOYERR_NOERROR)
                continue;

            JOYINFOEX info;
            info.dwSize = sizeof(info);
            info.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &info) != JOYERR_NOERROR)
                continue;

            wchar_t name[64];
            oem_name(caps.wMid, caps.wPid, name, 64);
            if (name[0] == L'\0')
                wcsncpy(name, caps.szPname, 63), name[63] = L'\0';

            char name8[256];
            to_utf8(name, name8, 256);

            fprintf(f, "pad %u %04X %04X %d %u %u %s\r\n", i, caps.wMid, caps.wPid,
                    is_xinput_vendor(caps.wMid), caps.wNumButtons, caps.wNumAxes, name8);

            if (reported < RE2RMP_MAX_PADS) {
                PadEntry* e = &g_block.pads[reported];
                e->flags = 1;
                e->buttons = info.dwButtons;
                e->pov = (info.dwPOV == 0xFFFF || info.dwPOV > 35999u) ? 0xFFFFu : info.dwPOV;
                e->axes[0] = axis_norm(info.dwXpos);
                e->axes[1] = axis_norm(info.dwYpos);
                e->axes[2] = axis_norm(info.dwZpos);
                e->axes[3] = axis_norm(info.dwRpos);
                e->axes[4] = axis_norm(info.dwUpos);
                e->axes[5] = axis_norm(info.dwVpos);
                ++reported;
            }
        }
        // Mark unreported entries dead so the Lua never reads stale state.
        for (UINT k = reported; k < RE2RMP_MAX_PADS; ++k)
            g_block.pads[k].flags = 0;

        fclose(f);
        MoveFileExW(tmp, final, MOVEFILE_REPLACE_EXISTING);
    }

    ++g_alive;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_run, 1, 1) == 1) {
        poll_once();
        Sleep(100);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(h);
    return TRUE;
}

// REFramework plugin entry point (same export name as the reference plugin).
__declspec(dllexport) int reframework_plugin_initialize(void* param)
{
    (void)param;
    if (InterlockedExchange(&g_run, 1) == 0 && g_thread == NULL)
        g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    return 1;
}
