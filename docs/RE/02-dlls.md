# The two helper DLLs

Both are x64 REFramework plugins (export `reframework_plugin_initialize`) built with
**MinGW-w64 GCC 13-win32** (see debug strings `GCC: (GNU) 13-win32`,
`/usr/src/mingw-w64-11.0.1-3build1/...`). They ignore the REFramework API and run their own
worker thread. The Lua mod coordinates with them purely through files in `reframework\data\`.

---

## `re2coop_pads.dll` (42,496 bytes)

**Exports:** `reframework_plugin_initialize` only.

**Imports**
- `WINMM.dll`: `joyGetNumDevs`, `joyGetDevCapsW`, `joyGetPosEx`, `joyConfigChanged`
- `ADVAPI32.dll`: `RegGetValueW`
- `KERNEL32.dll`: `CreateThread`, `Sleep`, `GetModuleFileNameW`, `MoveFileExW`,
  `CreateFileW` (via CRT `_wfopen`), critical sections, `VirtualProtect/VirtualQuery`
- `msvcrt.dll`: file IO + `_snprintf`/`_snwprintf`

**What it does**
1. `reframework_plugin_initialize` spawns a worker thread (no game hooks).
2. The thread enumerates legacy joysticks with `joyGetNumDevs`/`joyGetDevCapsW`, reads state
   with `joyGetPosEx`.
3. For each device it looks up a friendly name in the registry:
   `HKLM\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_%04X&PID_%04X`
   value `OEMName` (`RegGetValueW`).
4. It locates the game folder from `GetModuleFileNameW`, then writes
   `reframework\data\re2coop_pads.txt` (atomically: write `.tmp`, then `MoveFileExW`).
5. It also publishes raw per-pad state into a **memory block** at an address it reports as
   `addr <n>`; the Lua reads it with `sdk.to_valuetype(addr + 64*(idx+1), "via.mat4")`.

> Note: it uses the *legacy* WINMM joystick API, not XInput. XInput pads are already visible to
> the game; this plugin exists to expose *DirectInput/legacy* pads (PS3 pads, arcade sticks,
> generic USB) that the game itself can't see.

Strings that pin the behaviour:
```
addr %llu
alive %u
pad %u %04X %04X %d %u %u %s
Controller %u
%lsreframework\data\re2coop_pads.tmp
%lsreframework\data\re2coop_pads.txt
System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_%04X&PID_%04X
OEMName
```

---

## `re2coop_sfx.dll` (93,428 bytes; `.text` only 6.2 KB — rest is debug/pseudo-reloc)

**Exports:** `reframework_plugin_initialize` only.

**Imports**
- `WINMM.dll`: **`PlaySoundW`** (the only real API used)
- `KERNEL32.dll`: `CreateFileW`, `ReadFile`, `CreateThread`, `Sleep`, `GetModuleFileNameW`, ...
- `msvcrt.dll`: `_snwprintf`, `strchr`, `strcmp`, `strcpy`, ...

**What it does**
1. Worker thread polls `reframework\data\re2coop_sfx.txt` for changes.
2. On a new request it plays `reframework\data\re2coop_sfx\<name>.wav` with
   `PlaySoundW(path, NULL, SND_FILENAME | SND_ASYNC)` (fire-and-forget, non-blocking).

Strings that pin the behaviour:
```
%lsreframework\data\re2coop_sfx.txt
%lsreframework\data\re2coop_sfx\%ls.wav
PlaySoundW
```

---

## Implication for our mod
Both are ~200 lines of C. We reimplement them cleanly as
`src/plugins/re2r_mp_sfx` and `src/plugins/re2r_mp_pads` (see `03-protocols.md` for the exact
formats they must produce). We deliberately keep the **same file names and formats** so the
Lua side of the ported mod keeps working unchanged during development.
