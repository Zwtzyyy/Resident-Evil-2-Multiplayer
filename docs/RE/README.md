# Reverse Engineering: "RE2 Co-op: HUNK" v1.4.3-NonRT-Alpha

Target: `reference/` inside this repo.
- `re2coop_main.lua` — 19,388 lines, **plain readable Lua** (not obfuscated). This is the whole mod.
- `re2coop_pads.dll` — controller-helper plugin (x64).
- `re2coop_sfx.dll` — sound-helper plugin (x64).

Tooling used: a dependency-free PE parser (`tools/pe_analyze.py`) for exports/imports/strings,
plus the Lua source itself (which documents the DLL contracts in comments).

## Headline findings

1. **The reference mod is LOCAL couch co-op, not networked.** One game instance, a second
   survivor driven by a 2nd controller. Our job is to keep its game-integration tech and
   replace the input source with packets (LAN/internet).
2. **The game-integration lives entirely in Lua.** It hooks 54 game methods (~87 `sdk.hook`
   sites) across ~19 singletons — see `01-lua-map.md`.
3. **Both DLLs are tiny, self-contained helpers** that do not touch the game at all:
   - `re2coop_sfx.dll` (`.text` = 6.2 KB): polls a text file, plays WAVs via `PlaySoundW`.
   - `re2coop_pads.dll` (`.text` = 29.7 KB): polls legacy joysticks via WINMM `joy*`, writes a
     text file **and** a shared-memory block the Lua reads by address.
   See `02-dlls.md`.
4. Both DLLs are built with **MinGW-w64 GCC 13** (cross-compiled on Linux) and export only
   `reframework_plugin_initialize`. Neither needs the REFramework API; they run their own thread.
5. Exact file/RAM protocols are in `03-protocols.md`. These are what our replacements must match.

## Files
- `01-lua-map.md` — structure of the Lua mod (hooks, singletons, subsystems).
- `02-dlls.md` — per-DLL PE/imports/strings analysis and behaviour.
- `03-protocols.md` — the two on-disk/RAM protocols in exact detail.
- `pads_pe.txt`, `sfx_pe.txt` — raw analyzer output.
