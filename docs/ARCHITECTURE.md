# re2r_mp architecture

Goal: turn the reference mod's **local** couch co-op (one PC, two pads, split control) into a
**networked** co-op where each player runs the game on their own machine and their own monitor.

We keep the reference mod's game-integration (the hard part: spawning and driving a second
survivor via the RE Engine scripting API) and replace *local input* with *networked input/state*.

## Pieces

```
            +---------------------------- your machine ----------------------------+
            |  re2.exe                                                                 |
            |   |- reframework/  (Non-RT REFramework, dinput8.dll)                     |
            |   |     |- plugins/re2r_mp.dll      <- owns socket + bridge RAM block    |
            |   |     |- plugins/re2r_mp_pads.dll <- reimplemented helper (gamepads)   |
            |   |     |- plugins/re2r_mp_sfx.dll  <- reimplemented helper (sounds)     |
            |   |     |- autorun/re2r_mp_main.lua <- gameplay: 2nd survivor, blend     |
            |   |                                     remote state, apply to net player|
            +----------------------------------------------------------------+
                         | UDP (LAN broadcast discovery, or direct IP for internet)
                         v
            +---------------------------- peer machine -----------------------------+
            |                          (same setup)                                   |
            +-------------------------------------------------------------------------+
```

### Why this split (native plugin + Lua)
- **Lua** is where the reference mod already reads/writes game objects (position, motion FSM,
  animations). Full readable source is available (`reference/re2coop_main.lua`, 19,388 lines),
  so we port gameplay there.
- **Native plugin** is where sockets belong. REFramework's C plugin API (`reframework/API.h`)
  gives us lifecycle hooks and logging; Winsock gives us UDP. The plugin runs its own thread.

### The bridge (Lua <-> plugin)
We reuse the **exact** pattern the reference helper DLLs use, so we know it works in this game:

1. Plugin allocates one `NetBlock` struct in-process and writes its address to
   `reframework/data/re2r_mp_net.txt` (`addr <decimal>`).
2. Lua reads the address, does `sdk.to_ptr(addr)`, then reads/writes fields.
3. Fields (see `src/plugin_main.cpp`, `struct NetBlock`):
   - control (Lua -> plugin): `cmd` (host/join/disconnect/scan), `port`, `ip`.
   - status (plugin -> Lua): `state`, `error`, `peer`, `tick`, `status[]`, `found[]`.
   - `local` `PlayerSnapshot` (Lua writes; plugin sends every ~16 ms).
   - `remote` `PlayerSnapshot` (plugin writes on receive; Lua reads to drive the net player).

`PlayerSnapshot` = `pos[3]`, `rot[4]` (quat), `motion` (FSM id/hash), `flags` (bitfield),
`health`.

### Protocol (MVP, v1)
Little-endian, `#pragma pack(1)`, no encryption yet. Header `{ magic 'RMP2', u16 version,
u8 type, u8 flags, u32 seq }`, then a payload for state packets.

| type | dir | meaning |
|------|-----|---------|
| HELLO (1) | client->host | request to join (resent every 250 ms until acked) |
| HELLO_ACK (2) | host->client | accept; both sides now in CONNECTED |
| STATE (3) | both | per-frame `PlayerSnapshot` (sent ~60 Hz, `SO_REUSEADDR` UDP) |
| BYE (4) | both | clean disconnect |
| DISCOVER (5) | client->broadcast | LAN scan on UDP :27100 |
| DISCOVER_ACK (6) | host->client | "I'm here" (host ip:port added to `found[]`) |
| DOOR (7) | both | a door was opened: `{u32 state (2=OPEN), char name[64]}` |
| ITEM (8) | both | a world item was taken: `{float pos[3], char name[64]}` |

Default game port `27099`, discovery port `27100`. Host-authoritative: world events/entities are
validated by the host in later milestones (not in the MVP).

### Internet vs LAN
- **LAN**: broadcast discovery (`DISCOVER`) finds hosts automatically; then a normal HELLO/ACK.
- **Internet**: connect by direct `ip:port` (port-forward on the host). Later milestone: relay via
  Steam P2P / small UDP relay so no port-forward is needed. Both end up as the same HELLO/ACK +
  STATE stream, so only the peer-resolution step differs.

## Milestones
1. **MVP (this scaffold)**: three DLLs build; plugin hosts/joins and streams a player snapshot;
   Lua bridge documented; mod manager installs/enables mods.
2. **Drive the net player**: apply `remote` snapshot to the second survivor (position/rotation/
   motion) in `re2r_mp_main.lua`. **DONE** - see "Lua net API" below.
3. **Input parity**: the second pad's input poll is replaced with `remote`-driven input so the net
   player animates/shoots correctly (not just teleports). **DONE** - see "Net input" below.
4. **World sync**: doors, items, enemies, damage events (host-authoritative). **IN PROGRESS** -
   door, world-item and damage/health sync done, see "Net world sync" below. Enemies pending.
5. **Transport hardening**: reliability layer for events, interpolation/extrapolation for
   position, NAT punchthrough/relay for internet play.

## Lua net API (`scripts/re2r_mp_main.lua`)
The module `RE2R_MP_NET` (built with an IIFE so it keeps its own locals out of the 200-slot main
chunk) talks to `re2r_mp.dll` over the RAM block and is driven from the frame loop
(`pcall(RE2R_MP_NET.tick)` inside `re.on_frame`). The menu (`RE2R_MP_MENU`, drawn from the co-op
menu's Experimental section) exposes Host / Join / Scan LAN / Stop plus an IP field.

Memory access uses the same calls the reference plugins use: `fs.read("re2r_mp_net.txt")` for the
address + field offsets, and `sdk.to_valuetype(addr, "via.mat4")` + `read_float` / `write_dword` /
`write_float` / `read_dword` for the fields. `local` is this machine's live P1 transform; `remote`
is applied to `st.hunk` via `st.warp_go` each frame (lerped at `NET.smooth`, snapped when >5 m,
skipped while HUNK is climbing).

Public API: `RE2R_MP_NET.start_host([port])`, `.start_join([ip],[port])`, `.scan()`, `.stop()`,
`.status_text()`, `.found_count()`, `.found(i)`, `.tick()`. Settings: `.ip`, `.port`, `.smooth`.

## Net input (milestone 3)
The net player is wired in as **player 2's virtual pad** (`st.pad.vpad`, `st.p2_dev`). The mod's
whole P2 input path - `route()`'s InputSystem hooks, `update_vaim`'s right-stick aim, the co-op
combos (`pad2_buttons` -> heal/resupply/reload/teleport), the footrest `V.raw()` - already reads
only that virtual pad, so feeding it the remote input gives locomotion *and* all support actions
with no changes to those systems.

- **Send side**: every frame `NET.tick` -> `NET.send_input` maps player 1's real pad
  (`st.pad.raw` + `st.pad.maps_default.game` + `st.pad.level`, exactly `PD.update`'s mapping but
  for P1) into canonical virtual-pad state (buttons bitfield + lx/ly/rx/ry/lt/rt) and writes
  `local_input` in the block. Using the default layout keeps button *identity* peer-to-peer even
  across different controllers; custom "Map player 2" remaps do not apply to the net player.
- **Wire-up**: `NET.ensure_p2` (called from `start_host/join` and every `refresh_unit`) runs
  `st.set_p2(is, pads, {kind="game",key="game",uid="net"})`, so `st.unit2` (the game's button
  layout, taken from the active input unit) and `st.p2_dev` (= the virtual pad) stay live even
  though there is no second physical controller. `NET.stop` hands player 2 back to `choose_p2`.
- **Receive side**: a one-line injection in `st.pad.update` (right after `V.prev = V.b`) calls
  `NET.apply_remote_input(V)`, which fills the virtual pad from `remote_input` when a peer is
  connected (once per frame; `V.frame` caches it) and skips the physical pad.
- **Position**: with remote input driving HUNK's own locomotion (real feet/hitboxes), the state
  sync only corrects drift: snap > 8 m (loads/warps), converge 2-8 m, gentle pull < 2 m.

Known limits: capture is gamepad-only (a keyboard+mouse P1 sends no input, so the net HUNK holds
still). Per-player aiming is synced: each machine sends the world point under its own player-1
crosshair (pumped alongside the snapshot, `local_aim`), and a small override in `target_point()`
makes every HUNK aim behavior - vaim, muzzle-fired projectiles, the flashlight, tank aim - face
the *remote* crosshair point instead of the local one. (`st.net_local_ask` lets the sending
machine lift its own genuine point without recursing.) Once world sync exists, that remote point
will line up exactly with the peer's world; today it is the point in *your* world under the
crosshair direction they sent.


## Net world sync (milestone 4)
World entities are synced as small events the peer replays locally (peer-to-peer, not
host-authoritative yet). Doors are the first slice.

**Doors.** A door opening on one machine is replayed on the other. Doors are
`app.ropeway.gimmick.action.GimmickDoorBase`; the stable id is the GameObject *name* (same map
asset on both machines). Progress = `_PeekedWork.GetValue(0).Value` (0..1, >0.5 open).

- **Capture** (in `RE2R_MP_NET`, observe-only): a passive `sdk.hook` on the two managed open
  methods the game itself calls - `requestOpenByLever` (lever/push doors) and `setPeeked`
  (peek-routine doors) - catches both player 1's native opens and the mod's own opens, with the
  hook touching nothing (it only records the door's address). A polling fallback watches the
  peek `_PeekedWork` rising edge for any door the hooks miss.
- **Transport**: `DoorPacket` (type 7), `{u32 state, char name[64]}`. Lua queues a name into the
  block's `door_out_*` slot; the plugin drains it into a packet at the next loop turn (a slot
  frees up every ~16 ms). Incoming packets land in `door_in_*`.
- **Apply**: on receipt the door is looked up by name (a module-scoped `findComponents` cache,
  refreshed every 60 frames - the main chunk's `door_cache` is a local we can't reach). It is
  opened with `requestOpenByLever(calcSide(remotePos))` - i.e. from the *mirrored remote player's*
  side, the same path the mod's own `door_open` uses - then `setOpenSpeed(1.0)` and the contact
  timer (`_TimerContactPl = 1.0`) hold it ajar ~2 s. For ~2.5 s after applying, that door is
  muted in our own capture so the open is not echoed back.

Known limits: both peers must have the door's area loaded (same general location); a door only
exists in the cache once its scene is streamed in. Room pre-loading on a remote open is mirrored
via `st.area_ready` (the door's `get_MyRooms`), same as the mod's own `door_open`. Event delivery
is best-effort (no reliability layer until milestone 5).

**World items.** A `SetItem` gimmick (`app.ropeway.gimmick.action.SetItem`) taken by *either*
player is removed on the other machine, so an item can't be collected twice. The id is the
GameObject name *plus* position - prefab names are not unique, so the nearest same-named item is
the one removed.

- **Capture**: an observe-only `sdk.hook` on `SetItem.setVanish` records the item as it leaves the
  world, and only when it was within 3.5 m of player 1 or HUNK - so streaming an area out can't be
  mistaken for a pick-up. This covers both player 1's native pick-up and the mod's own HUNK
  pick-up (which calls `setVanish` from its pick-up screen hook).
- **Transport**: `ItemPacket` (type 8), `{float pos[3], char name[64]}`, over the same
  `item_out_*`/`item_in_*` block slots.
- **Apply**: the matching `SetItem` (same name, nearest position) is `setVanish`-ed, then that name
  is muted ~2 s in our own capture so the removal is not echoed back.

Known limits: only items whose scene is loaded on the receiving side can be removed. Inventories
are still per-machine (there is no cross-machine inventory transfer yet), so a pickup removes the
world item but the *other* player's mirrored inventory is unaffected.

**Damage / health.** Each machine sends its own player-1 hit points in the snapshot (the
`PlayerSnapshot.health` field, plus a "health valid" bit in `PlayerSnapshot.flags`), read from
`app.ropeway.HitPointController.get_CurrentHitPoint`. The receiver mirrors the remote value onto
its HUNK: every 10th frame it reads the puppet's `CurrentHitPoint` and, beyond a deadband
(-1 / +2), calls `addDamage(delta)` clamped to +/-150 - so the peer's real damage (and healing)
shows on our HUNK, and downing the peer brings him down here. Skipped while the puppet is
downed/immune/a child. Toggleable in the net menu ("sync health").

## Reference material
- `docs/RE/` - reverse-engineering of the shipped Lua + both DLLs (protocols, button maps).
- `reference/re2coop_main.lua` - the readable source we port gameplay from.
- `reference/README` and fonts/WAVs are the reference mod's assets - **do not redistribute**.
