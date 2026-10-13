# Lua mod map — `re2coop_main.lua` (19,388 lines)

Plain Lua for REFramework. Entry point: file loaded from `reframework\autorun\`.
Data it persists in `reframework\data\`: `re2coop_hunk_config.json`,
`re2coop_p2_inventory.json`, `re2coop_loadouts.json`, `re2coop_p2_log.txt`, plus the two
helper-DLL files.

## Core idea (from the file header, lines 5-7)
The game reads input through **static `InputSystem` functions that always use the "active" pad**.
The mod **tags the update methods of player/weapon components with which character is running**;
when HUNK's code asks for input it answers from controller 2 instead. This "context tagging"
(`st.stacks`, `st.owners`, `hook_type_context`) is the heart of the mod.

## Subsystems (approx. source lines)

| Lines | Subsystem | Notes |
|-------|-----------|-------|
| 137-360 | helpers, component cache, **context tracking** | `comp`, `players`, `current_owner`, `hook_type_context`, `install_context_hooks`, `is_hunk_owner` |
| 366-1460 | **input routing / controllers** | controller brands, virtual pad `st.pad.vpad`, `refresh_unit`, `swap_controllers`, `route()`, `p2_move_stick`, keyboard+mouse pad |
| 1464-1755 | **HUNK aim** | sub-weapon stick aim, `vaim`, `PlayerActionOrderer` WatchDir/CharDir, spine pitch |
| 1588-1755 | split-screen flashlight / look-at | (we drop split-screen for per-monitor MP) |
| 1880-1990 | **raycast / crosshair target** | `cast_ray`, `aim_ray`, `target_point` |
| 1965-2840 | **projectile redirect** | rewrite shot matrices from P2's muzzle; tracers, flame/minigun fixes |
| 2842-3030 | **weapon / loadout** | `WEAPON_BY_WT`, `build_loadout`, `give_loadout`, `refill_ammo`, infinite kit |
| 3033-4310 | **P2 inventory** | `app.ropeway.survivor.Inventory`, save/restore JSON |
| 4313-4460 | **invulnerability / grab guard** | `keep_invulnerable`, `child_guard` |
| 4465-5390 | **downed/revive** | `p2_go_down`, `p2_revive`, `p1_go_down`, `p1_down_tick` |
| 5400-6210 | **spawn / model / rig / costume** | `spawn()`, `finish_spawn`, `model_costume`, `rig_for`, `voice_map_go`, `apply_model` |
| 6210+ | floor spot, teleport, HUD, co-op menu, AI partner | |
| 6552-6580 | **support abilities + SFX bridge** | `toast`, `play_sfx` → `re2coop_sfx.txt` |
| UI | `re.on_frame`, `imgui.*`, Script Generated UI | menu tabs Play/Player 2/AI Partner/Screen/Controls/Extras |

## Hook surface
- `sdk.hook(...)` used ~87 times; `sdk.hook` appears 54× as the dominant registration call.
- Types hooked most often: `via.SceneManager` (38), `via.GameObject` (9),
  `app.ropeway.survivor.Inventory` (5), `via.Application`, `via.hid.GamePad`,
  `app.ropeway.InputUnit`, `app.ropeway.InputSystem`, plus weapon/generator, gimmick
  (door/ladder), camera, reticle, HitPointController, enemy define types.
- `sdk.find_type_definition` used 127×; singletons via `sdk.get_managed_singleton` (73×) /
  `sdk.get_native_singleton` (42×): `via.SceneManager`, `app.ropeway.EnemyManager`,
  `app.ropeway.InputSystem`, `app.ropeway.camera.CameraSystem`,
  `app.ropeway.gamemastering.MainFlowManager`, `app.ropeway.gui.GUIMaster`,
  `app.ropeway.PlayerManager`, `app.ropeway.SurvivorManager`, `ItemManager`, etc.
- `call_native_func` (50×), `to_int/to_ptr` etc. for calling game internals.

## What we keep vs change for our networked mod
- **Keep (reuse as our module layer):** context tagging, spawn/model/rig/costume, weapon/
  loadout, inventory, aim/raycast/shot redirect, downed/revive, gimmicks, HUD.
- **Replace:** input routing (controller 2 → **network packets**), split-screen paths.
- **Add:** a net layer (host-authoritative), remote-player interpolation, world sync (enemies/
  doors/items/flags).
- **Rebrand:** UI strings/fonts and file names.
