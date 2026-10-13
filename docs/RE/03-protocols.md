# On-disk / RAM protocols

These are the contracts between the Lua mod and the two helper DLLs. Our replacements must
match them byte-for-byte so the Lua side keeps working.

Root data dir = `<game>\reframework\data\`.

---

## 1. Pads: `re2coop_pads.txt` (written by the DLL, read by Lua)

Text, UTF-8/ASCII. Line order in the reference is `addr`, `alive`, then one `pad` per device:

```
addr <uint64 decimal>          # base address of the shared pad block (see below)
alive <uint32 decimal>         # a counter the DLL bumps every tick (liveness heartbeat)
pad <id:u> <vid:%04X> <pid:%04X> <xinput:0|1> <nbuttons:u> <naxes:u> <name>\r\n
pad ...
```

Lua parse (from `reference/re2coop_main.lua:456-461`):
```lua
local addr  = tonumber(txt:match("addr (%d+)"))
local alive = tonumber(txt:match("alive (%d+)"))
for id, vid, pid, xi, nb, na, name in
    txt:gmatch("pad (%d+) (%x+) (%x+) (%d) (%d+) (%d+) ([^\r\n]*)") do ... end
```

Rules the Lua relies on:
- `alive` changing between reads = the plugin is ticking (else it distrusts after 4 s).
- `addr` must stay stable, otherwise the plugin is considered restarted/`trusted=false`.

### Shared pad block at `addr`
The DLL allocates a block in the game's address space and reports its base as `addr`.
Header is 64 bytes; then **one 64-byte entry per pad**. For pad index `idx` (0-based) the entry
starts at `addr + 64 * (idx + 1)` and is interpreted as a `via.mat4` (Lua reads dwords/floats):

| off | type  | meaning                                   |
|-----|-------|-------------------------------------------|
| 0   | dword | flags (bit0 = present/alive)              |
| 4   | dword | buttons bitmask                           |
| 8   | dword | POV / hat (0xFFFF = centred)              |
| 12  | float | axis[1] X                                 |
| 16  | float | axis[2] Y                                 |
| 20  | float | axis[3] Rx                                |
| 24  | float | axis[4] Ry                                |
| 28  | float | axis[5] Z / analog L                      |
| 32  | float | axis[6] Rz / analog R                     |

Read by the Lua in `PD.ext_raw()` (`reference/re2coop_main.lua:475-488`).

---

## 2. Sound: `re2coop_sfx.txt` (written by Lua, watched by the DLL)

Lua writes a monotonically increasing sequence number followed by the clip name
(`reference/re2coop_main.lua:6562-6566`):

```
<seq> <name>          e.g.  "7 heal"
```

The DLL plays `<game>\reframework\data\re2coop_sfx\<name>.wav` via `PlaySoundW`. It only plays
when `<seq>` increases, so re-writing the same line does nothing. Available clips shipped by the
reference: `heal.wav`, `resupply.wav`.

---

## 3. Our naming
Our plugins keep the **same file/format names** (`re2coop_pads.txt`, `re2coop_sfx.txt`,
`re2coop_pads.tmp`, `re2coop_sfx\*.wav`) while porting, so the Lua side works unchanged. We can
rename to `re2r_mp_*` later once the whole mod is rebranded.
