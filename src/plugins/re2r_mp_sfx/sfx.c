// re2r_mp_sfx.dll
// Clean-room reimplementation of the reference "re2coop_sfx.dll" behaviour: play a
// WAV when the Lua bumps a sequence number in a text file (Lua can't play audio).
//
// Contract (see docs/RE/03-protocols.md):
//   * watches  <game>\reframework\data\reunited_sfx.txt  for  "<seq> <name>"
//   * plays     <game>\reframework\data\reunited_sfx\<name>.wav
//   * sequence number only plays on change (re-writing the same line is a no-op)
#include "../plugin_common.h"
#include <stdlib.h>

static volatile LONG g_run = 0;
static HANDLE g_thread = NULL;
static unsigned long g_last_seq = 0;
static int g_have_last = 0;

static void poll_once(void)
{
    wchar_t base[1024];
    re2rmp_game_root(base, 1024);
    wchar_t cmd_path[1152];
    re2rmp_path(cmd_path, 1152, base, L"reframework\\data\\reunited_sfx.txt");

    FILE* f = _wfopen(cmd_path, L"rb");
    if (f == NULL)
        return;

    char buf[512];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';

    // parse "<seq> <name>"
    char* p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p < '0' || *p > '9')
        return;
    unsigned long seq = strtoul(p, &p, 10);
    while (*p == ' ' || *p == '\t')
        ++p;
    char* name = p;
    char* end = name;
    while (*end && *end != '\r' && *end != '\n' && *end != ' ' && *end != '\t')
        ++end;
    *end = '\0';
    if (*name == '\0')
        return;

    if (g_have_last && seq == g_last_seq)
        return;
    g_last_seq = seq;
    g_have_last = 1;

    wchar_t wname[256];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256) <= 0)
        return;

    wchar_t rel[512];
    swprintf(rel, 512, L"reframework\\data\\reunited_sfx\\%ls.wav", wname);
    wchar_t wav[1152];
    re2rmp_path(wav, 1152, base, rel);

    PlaySoundW(wav, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_run, 1, 1) == 1) {
        poll_once();
        Sleep(75);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(h);
    return TRUE;
}

__declspec(dllexport) int reframework_plugin_initialize(void* param)
{
    (void)param;
    if (InterlockedExchange(&g_run, 1) == 0 && g_thread == NULL)
        g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    return 1;
}
