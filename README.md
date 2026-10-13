# Resident Evil 2 (2019) - Multiplayer co-op mod

A from-scratch networked co-op mod for **RE2R (Non-RT / DirectX 11)**. Each player
runs the game on their own PC and monitor (non-split-screen). Reverse-engineered
from, and gameplay-ported from, the readable Lua of the reference mod *"RE2 Coop
HUNK v1.4.3 NonRT Alpha"*.

> Not affiliated with Capcom. The reference mod's assets (fonts, WAVs) are not
> redistributed by this project.

## What it does

- **`re2r_mp.dll`** - UDP networking plugin (host / join / LAN discovery) that
  publishes a RAM-bridge (`re2r_mp_net.txt`) with the Lua-side block address and
  field offsets.
- **`re2r_mp_pads.dll` / `re2r_mp_sfx.dll`** - clean reimplementations of the
  reference helper plugins.
- **`re2r_mp_main.lua`** - gameplay mod + `RE2R_MP_NET` bridge: hosts/joins,
  drives the second survivor from the remote snapshot, feeds **remote input** into
  player 2's virtual pad so the net HUNK walks/shoots/acts from the peer's
  controller, and syncs **door opens**, **world-item pickups** and
  **damage/health** between peers.
- **`tools/modmanager/`** - minimal open mod manager (Fluffy replacement).

## Play (quick)

1. Use the **Non-RT / DX11** build of RE2 - Steam -> RE2 -> Properties -> Betas ->
   `dx11_non-rt`. `re2_config.ini` should read `TargetPlatform=DirectX11`.
2. Install the mod: copy the plugins to `<game>/reframework/plugins/` and
   `scripts/re2r_mp_main.lua` to `<game>/reframework/autorun/`.
3. Host opens the co-op tab and hosts; the friend joins on the host's IP.
   UDP port **27099** must be reachable on the host.
4. Both players need the same build + mod. In-game: press **Insert** for the
   REFramework overlay, open the Co-op Mod tab.

## Build

Requires Visual Studio 2022 (MSVC) + CMake.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs land in `../dist/plugins/`. Copy them to `<game>/reframework/plugins/`,
and `scripts/re2r_mp_main.lua` to `<game>/reframework/autorun/`.

### Live log watcher

```
tools\watch-logs.cmd            # only plugin / loader / error lines
tools\watch-logs.cmd -All       # every line
```

Tails `<game>\re2_framework_log.txt`, highlights `re2r_mp` / `PluginLoader` /
errors, and prints the `re2r_mp_net.txt` bridge address + version when the net
plugin publishes it.

### REFramework compatibility

REFramework's `PluginLoader` rejects any plugin whose declared
`reframework_plugin_required_version` has a **minor greater than the host
framework's**. Our plugin only uses `param->functions` + `log_info` (present since
API 1.0.0), so `reframework/API.h` deliberately declares **1.0.0** to load on any
1.x build (e.g. the installed v1.5.9, whose plugin API is 1.10.0). Keep the
declared minor low; do not bump it to match the header's feature set.

## Layout

- `src/` - native plugin sources (main plugin + helper plugins).
- `scripts/` - Lua gameplay mod.
- `reframework/` - REFramework C/C++ plugin API headers.
- `docs/ARCHITECTURE.md` - how the netcode, bridge, and milestones fit together.
- `docs/RE/` - reverse-engineering notes and raw PE analysis.
- `assets/fonts/` - UI font (Rajdhani, OFL).
- `tools/modmanager/` - the mod manager.

## Mod manager

```
node tools/modmanager/modmanager.mjs config --game "D:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2"
node tools/modmanager/modmanager.mjs install <mod.zip>
node tools/modmanager/modmanager.mjs list
node tools/modmanager/modmanager.mjs enable <slug>
```

## License

MIT - see `LICENSE`. Third-party: Rajdhani font (OFL), REFramework headers (MIT).