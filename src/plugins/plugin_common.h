// Shared helpers for our re2r_mp helper plugins.
// Reimplemented clean-room from the reverse-engineered behaviour of the reference
// re2coop_sfx.dll / re2coop_pads.dll (see docs/RE/02-dlls.md). No third-party code.
#ifndef RE2R_MP_PLUGIN_COMMON_H
#define RE2R_MP_PLUGIN_COMMON_H

#include <windows.h>
#include <stdio.h>

// Resolve the game root folder (the folder containing re2.exe) from our own module path.
// Our DLL lives at <game>\reframework\plugins\<name>.dll, so the game root is three levels up.
// Returns the base with a trailing backslash, e.g. L"C:\...\RESIDENT EVIL 2 BIOHAZARD RE2\".
static void re2rmp_game_root(wchar_t* out, size_t cap)
{
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&re2rmp_game_root, &self);
    out[0] = L'\0';
    if (self == NULL || GetModuleFileNameW(self, out, (DWORD)cap) == 0)
        return;

    // Prefer to cut right before "\reframework\"; fall back to removing three path segments.
    wchar_t* p = wcsstr(out, L"\\reframework\\");
    if (p != NULL) {
        *(p + 1) = L'\0';
        return;
    }
    wchar_t* s = out;
    int cuts = 3;
    while (cuts > 0) {
        wchar_t* last = wcsrchr(out, L'\\');
        if (last == NULL || last == out)
            break;
        *last = L'\0';
        --cuts;
    }
    size_t n = wcslen(out);
    if (n + 1 < cap) {
        out[n] = L'\\';
        out[n + 1] = L'\0';
    }
    (void)s;
}

// Append a relative suffix to a base dir into out.
static void re2rmp_path(wchar_t* out, size_t cap, const wchar_t* base, const wchar_t* suffix)
{
    swprintf(out, cap, L"%ls%ls", base, suffix);
}

#endif // RE2R_MP_PLUGIN_COMMON_H
