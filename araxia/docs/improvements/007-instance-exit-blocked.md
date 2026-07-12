# Story 007: Cannot exit an instance — player blocked at the entrance portal

_(GitHub issue [#1](https://github.com/araxiaonline/WoWee/issues/1). Story authored per Rule 11 — story before code.)_

## Status
**FIXED + LIVE-VERIFIED 2026-07-11 (commit on branch `fix/007-instance-exit`).** The exit teleport now
fires and the server ports the player out of Ragefire Chasm. Root cause was a **transposed area-trigger
box** (client swapped the trigger X/Y to canonical frame but not box length/width). Fixed by testing the
box in server frame to match AzerothCore's `Position::IsWithinBox` (`include/game/area_trigger_box.hpp`,
`tests/test_area_trigger_box.cpp`, red→green). 34/34 tests pass.

> **Follow-up bug discovered on landing (separate story):** the exit teleport works, but the local
> **player lands BELOW the Orgrimmar WMO floor** (X/Y correct, Z wrong) — same family as issue #6 but on
> the *local-player teleport-landing* path, which the #6 fix did not cover. `handleNewWorld`
> (`movement_handler.cpp:~1875`) places the player at the server's raw exit Z (−18.57) via the world-entry
> callback with **no WMO/M2 floor clamp**. The floor-clamp infra already exists (`application.cpp:1475-1509`,
> `landingClampActive` — probes terrain+WMO+M2, snaps to the highest) but is armed **only for taxi
> landings** (`setTaxiLandingClampTimer`). Fix likely = arm the same clamp after an instance-exit /
> world-transfer teleport. Tracked as Story 009.

> ### ✅ CONFIRMED ROOT CAUSE (2026-07-11) — transposed area-trigger box
> The second diagnostic (`[007dbg-move]`) disproved the position-desync theory and revealed the real bug:
>
> **Movement IS streaming correctly.** 7 `[007dbg-move]` lines in RFC (map 389) show the client sending
> movement with correct server-frame positions, tracking the player right up to the exit (last sent
> server pos (3.14, 0.65, −13.16) — inside the exit box). So the server has the right position. The
> "server doesn't know where I am" theory is **FALSE**.
>
> **The box test is transposed.** At the fire event the client was at canonical (−14.39, 1.63, −17.44)
> → server frame ≈ (1.63, −14.39, −17.44). The exit box (server) is center (2.58, −0.0136, −13.37),
> half-widths **length/2=15.34, width/2=6.10**, yaw 0:
> - Client fired because its check treats the canonical-X offset (−14.38) against **length/2=15.34** → inside.
> - Server `IsInAreaTriggerRadius`→`IsWithinBox` treats the same point's server-Y offset (−14.38) against
>   **width/2=6.10** → **OUTSIDE** → "too far" → silent reject (no `SMSG_NEW_WORLD` follows). Confirmed.
>
> **Why:** the DBC loader (`movement_handler.cpp:2599-2606`) swaps the trigger position
> `at.x=field3(Y_wire)`, `at.y=field2(X_wire)` into canonical frame, but loads `boxLength=field6`,
> `boxWidth=field7`, `boxYaw=field9` **straight, with no matching swap**. Since canonical-X = wire-Y and
> canonical-Y = wire-X, `boxLength` (a wire-X extent) ends up applied along canonical-X (= wire-Y), i.e.
> length and width are **transposed**. The box-check at `movement_handler.cpp:2646-2664` then tests
> `|localX| ≤ boxLength/2` and `|localY| ≤ boxWidth/2` in canonical frame — wrong axes.
>
> **Why it's worst on the exit and not the entrance:** the RFC exit box is elongated (30.69×12.19,
> ≈2.5:1), so the transposition creates a large region (6.1–15.3 units off the short axis) that is
> client-inside but server-outside. The client fires there, the server rejects, and the edge-triggered
> `activeAreaTriggers_` flag then **suppresses the re-fire** once the player walks to a spot the server
> *would* accept (log shows the player at dist 0.89 / server-Y 0.66 < 6.1 at 21:05:39, but `active=1`
> from the premature fire, so no re-fire). The entrance box (2230, 21.69×11.83, ≈1.8:1, and approached
> from open ground) happens to land the player in the overlap of both orientations, so it works.
>
> **This is a general movement/area-trigger bug, not RFC-specific** — every elongated or rotated area
> trigger (many instance portals, some world triggers) is mis-tested. It likely also causes wrong-place
> area-trigger *misfires* elsewhere.

## Fix plan (ready to implement — Rule 11 satisfied; Rule 2 failing test first)
Make the client's box test **exactly match the server's `Position::IsWithinBox`** by testing in
**server/wire frame** instead of the half-swapped canonical frame:
1. Store the trigger box in **raw server/wire frame** (keep `boxLength` along wire-X, `boxWidth` along
   wire-Y, `boxYaw` as-is) — or keep the struct but stop swapping only position while leaving the box.
2. In the check, convert the player's canonical position to server coords with the existing
   `core::coords::canonicalToServer(...)` (the same transform movement already uses), then replicate the
   server's `IsWithinBox`: `rotation = 2π − yaw`, rotate the player offset about the center, test
   `|dx| ≤ length/2`, `|dy| ≤ width/2`, `|dz| ≤ height/2`. Sphere triggers (`radius>0`) already work
   (distance is swap-invariant) — leave them.
3. **Failing test (Rule 2):** a pure box-in-server-frame predicate. Feed the real RFC 2226 box and the
   logged fire position (server (1.63, −14.39, −17.44)) → assert **outside** (current transposed logic
   returns inside — that's the red). Add an in-box point (server (3.14, 0.65, −13.16)) → assert inside.
   Also a rotated-box case (e.g. 2230's yaw 0.576) to guard the yaw handling.
4. Removes the premature fire, so the trigger fires only when the player is genuinely in the server box,
   and the server accepts → teleport out.

### Old server-side step (kept for reference — unavailable on this character)

> ### Diagnostic findings — the client side works; the server rejects (2026-07-10)
> Ran the `[007dbg]` probe in RFC (map 389) and cross-checked the AzerothCore server source + SQL.
>
> **What the client did — all correct:**
> - The exit trigger **is loaded and detected**: `trigsForMap=1`, `AT2226`, box `30.69×12.19×25.56`
>   centered where the player stood (`dist≈1.3`, deep inside). So **H3 (not detected) is FALSE**.
> - The player **reaches and is inside** the volume (`active=1`, cooldown expired, `suppressFirst=0`).
>   So **H2 (collision blocks reaching it) is FALSE**.
> - The client **fired `CMSG_AREATRIGGER id=2226` twice** (`Fired CMSG_AREATRIGGER: id=2226`). So the
>   detect-and-send path works — **H1 (never re-fires) is FALSE** (it did fire; it just needs a
>   leave+re-enter to fire again, which is correct behavior).
> - **No `SMSG_TRANSFER_PENDING` / `SMSG_NEW_WORLD` came back** after either fire → the server did
>   NOT initiate the teleport. (The only NEW_WORLD in the log was the *entry* into RFC.)
>
> **Server data — the trigger and teleport are correctly configured:**
> - `areatrigger_teleport` id **2226** = "Ragefire Chasm - Ogrimmar Instance (Inside)" → target map 1
>   (Kalimdor), pos (1813.49, -4418.58, -18.57). It IS the real exit-to-Orgrimmar teleport.
> - **No script** eats it: no `areatrigger_scripts` row and no C++ `OnAreaTrigger` for 2226.
> - The **entrance** trigger **AT2230** ("Ragefire Chasm - Ogrimmar Instance", map 1 → 389) fired at
>   16:29:53/58 and `SMSG_NEW_WORLD` immediately followed — **entering the instance WORKS**.
>
> **Conclusion — asymmetry points at server-side position:** entrance (2230) and exit (2226) use the
> *identical* server path: `WorldSession::HandleAreaTriggerOpcode` (MiscHandler.cpp) →
> `player->IsInAreaTriggerRadius(atEntry, 0.f)` → `TeleportTo(target_map…)`. The entrance fired while
> the player was **outdoors in Orgrimmar (position well-synced)** and worked; the exit fires **inside
> the instance** and is silently dropped. With the trigger known and no script, the only silent-reject
> gate is **`IsInAreaTriggerRadius`**, which validates the **server's** stored position for the player
> against the box with **zero tolerance** (`IsWithinBox(center, l/2, w/2, h/2)`). No transfer packet
> returns → the server rejected there. **Therefore the server's idea of the player's position does not
> match the client's inside the instance** — i.e. WoWee is not keeping the server's position in sync
> after the world-enter (movement/heartbeat reporting), so the server thinks the player is "too far"
> from the exit trigger and ignores it. This is a **client movement-position-reporting** bug, NOT an
> area-trigger detection bug.
>
> Supporting detail: the RFC entry teleport lands the player at server pos ≈ (3.81, -14.82, -17.84)
> (`areatrigger_teleport` 2230 target). The 2226 exit box (center (2.58, -0.0136, -13.37), half-widths
> 15.3/6.1/12.8, no rotation) does **not** contain that entry point in Y (|−14.82 − (−0.01)| ≈ 14.8 >
> 6.1). So if the server's position is **stale at the entry point** (because client movement isn't
> updating it), the player is genuinely outside the box server-side → "too far" → reject. Consistent.

> ### Refinement (2026-07-10, later) — narrowed to "does the client stream movement inside instances?"
> - GM commands are **unavailable** (test character has no GM access): `.debug areatrigger` and `.gps`
>   both return nothing. So server-side confirmation is out — must confirm from the client.
> - Checked the two "near-origin position" guards (they'd suppress movement/trigger for small coords):
>   both are **map-0-only** (`getCurrentMapId() == 0`, movement_handler.cpp:620 and :2614). RFC is map
>   389, so **neither applies** — not the cause.
> - Did the coordinate math: client canonical (0.65, 3.69, −13.16) → `canonicalToServer` ≈ server
>   (3.69, 0.65, −13.16), which **is inside** the 2226 server box (center (2.58, −0.0136, −13.37),
>   half-widths 15.3/6.1/12.8). So the position *math* is correct — IF a heartbeat carrying it reaches
>   the server, `IsInAreaTriggerRadius` should PASS.
> - **Therefore the remaining open question is purely: does WoWee actually SEND movement heartbeats to
>   the server while inside the instance?** If it stops streaming after `SMSG_NEW_WORLD` (or the sends
>   carry a stale/wrong server position), the server's stored position lags at the entry landing point
>   (server ≈ (3.81, −14.82, −17.84), which is OUTSIDE the exit box in Y) → "too far" → silent reject.
> - **Added a client diagnostic** (`[007dbg-move]`, movement_handler.cpp before the packet send): logs,
>   rate-limited, each outgoing movement opcode + server-frame position while on an instance map
>   (not 0/1/530/571). Next test: walk around RFC and to the exit, then read the log:
>   - **`[007dbg-move]` lines flowing with in-box server coords right up to the exit** → heartbeats are
>     fine; the server reject is elsewhere (reopen — unexpected).
>   - **No `[007dbg-move]` lines (or they stop / carry a stale position)** → confirms WoWee isn't
>     keeping the server position synced inside the instance = the root cause. Fix = ensure movement
>     streaming resumes after world-enter.

### Decisive next step (server-side — unavailable here; kept for reference / other servers)
Confirm the "too far" hypothesis directly, one of:
1. **`.debug areatrigger`** (GM command) on the character, then walk into the exit. It prints
   "AreaTrigger N reached" ONLY after `IsInAreaTriggerRadius` passes. If **no message** appears when
   the client fires 2226 → server position failed the box test (confirms desync). If it **does**
   print → position is fine and the problem is downstream (unexpected; would reopen).
2. **`.gps`** at the exit portal → compare the server's reported X/Y/Z for the player to where the
   client thinks it is (client `[007dbg]` pos). A large mismatch confirms the desync.
3. Grep the worldserver log for `too far` / `Area Trigger ID: 2226` (needs `network` debug logging).

If confirmed as position desync, the fix moves to **WoWee's movement/heartbeat reporting inside
instances** (a new/renamed story — this stops being "area-trigger exit" and becomes "client does not
keep server position synced"). Verify WoWee sends `CMSG_MOVE_*` / `MSG_MOVE_HEARTBEAT` with correct
server-frame coordinates after `SMSG_NEW_WORLD`, and that the server applies them.

## Story
**As a** player, **I want** walking into a dungeon's exit portal to teleport me back out to the world,
**so that** I can leave instances I have entered instead of being trapped inside.

## Context / Observed Behavior
Entering an instance works and mobs spawn (so instance map load + inbound transfer are fine). But walking
back through the entrance "portal" to leave **blocks** the player — movement stops at the portal and no exit
transfer happens, so the player is stuck inside the instance.

## Expected Behavior
Walking into the instance's exit area trigger should send `CMSG_AREATRIGGER`; the server teleports the player
out (`SMSG_TRANSFER_PENDING` → `SMSG_NEW_WORLD` → client `MSG_MOVE_WORLDPORT_ACK`), loading the outside world
at the entrance — the same round trip that already works for *entering*.

## Acceptance Criteria
1. Walking into a dungeon exit portal teleports the player out to the world entrance.
2. Works repeatably (enter → exit → re-enter → exit) with no stuck state.
3. Does not regress the *entry* transfer, and does not cause a rogue teleport (the existing
   suppress-first / cooldown guards must still prevent instant bounce-back on entry).

## Investigation (Dev Notes)

### Where it lives
- `src/game/movement_handler.cpp:2600` `checkAreaTriggers()` — the client-side detector. Loads
  `AreaTrigger.dbc`, tests the player against each trigger's sphere/box volume **filtered to the current
  map** (`:2636` `at.mapId != currentMapId → continue`), and fires `CMSG_AREATRIGGER` (`:2675-2680`) on first
  entry into a volume. Called each tick from `game_handler.cpp:883`.
- **Suppress/cooldown guards** (`:2626-2633`, `:2671`): after a map transfer, the first check marks triggers
  the player is already inside as "active" **without firing** (prevents the entrance trigger from instantly
  sending you back out), and an `areaTriggerCooldown` suppresses all firing for a while.
- **Re-fire requires leaving the volume:** a trigger only fires when it transitions from not-inside to inside
  (`activeAreaTriggersRef().count(at.id) == 0`, `:2668`); it is only cleared when the player is *outside* it
  (`:2685`).
- Transfer handlers: `SMSG_TRANSFER_PENDING` is a **no-op** (`game_handler_packets.cpp:799-806` — reads and
  discards the map id); `SMSG_NEW_WORLD` does the real work (`movement_handler.cpp:1741`) and sends
  `MSG_MOVE_WORLDPORT_ACK` (`:1859`). Entry proves this path works.
- Collision (portal geometry) lives in the WMO/M2 collision system used for player movement.

### Root-cause hypotheses (ranked; each needs live confirmation — DO NOT assume)
1. **Exit trigger never re-fires (HIGH).** On entry the player spawns *on* the exit trigger, so it is marked
   active (suppressed). To leave, the player must fully exit the trigger volume and re-enter it to re-fire.
   If the volume is large / the player never cleanly leaves it, `count(at.id) != 0` stays true forever and
   `CMSG_AREATRIGGER` never fires. The `areaTriggerCooldown` could also still be active on the way out.
2. **Portal collision blocks reaching the trigger (MED).** WMO/M2 collision at the portal physically stops
   the player short of the trigger volume ("blocked"). Retail exit portals are walk-through; if WoWee has
   solid collision there, the trigger is unreachable.
3. **Exit trigger not detected (MED).** The instance's exit trigger is missing from `AreaTrigger.dbc`, or its
   `mapId`/volume don't match, so the `:2636` map filter or the volume test skips it. (Open-world triggers do
   fire — logs show `CMSG_AREATRIGGER id=4801` — so the machinery works *outdoors*; instances are unverified.)
4. **`SMSG_TRANSFER_PENDING` no-op (LOW).** Unlikely, since entry uses the same no-op and works.

### Verification plan (do this BEFORE writing a fix — Rule 11 + observe-don't-assume)
Add a temporary diagnostic (WARNING-level, but NOT per-tick — gate it) that, while inside an instance and
near the exit, logs: current `mapId`, the count of `areaTriggersRef()` entries for this map, for the nearest
exit trigger its id / volume / distance / whether the player is `inside`, whether it is in
`activeAreaTriggers_` (stuck-active), and the `areaTriggerCooldown` value. Then walk into the portal and read
the log to see WHICH of H1–H3 is true:
- Trigger present + inside=true but in `activeAreaTriggers_` (never fired) → **H1**.
- Player can't get `inside` (blocked short of the volume) → **H2** (collision).
- No trigger for this mapId at all → **H3** (DBC/detection).

### Suggested approach (contingent on which hypothesis confirms)
- **H1:** allow the exit trigger to re-fire when the player *walks into* it after standing still, e.g. clear
  the suppressed-active state once the cooldown expires, or distinguish "spawned inside, suppressed" from
  "walked into" so a deliberate walk-in fires even without a full exit-and-re-enter. Preserve the anti-bounce
  guard on *entry*.
- **H2:** make instance exit-portal trigger volumes non-blocking, or fire the trigger on contact with the
  portal even if collision stops movement.
- **H3:** ensure instance-map area triggers are loaded and matched (mapId, volume) from `AreaTrigger.dbc`.

## Tasks / Subtasks
- [ ] Add the temporary exit-trigger diagnostic and reproduce an instance exit; capture the log.
- [ ] Confirm which of H1–H3 is the actual cause (record it here — Rule 10).
- [ ] Write a failing test for the confirmed logic seam where reasonable (Rule 2) — e.g. the trigger
      re-fire state machine for H1.
- [ ] Implement the fix for the confirmed cause; keep the entry anti-bounce guard intact.
- [ ] Remove the temporary diagnostic before committing.

## Testing
Live against AzerothCore: enter a dungeon (mobs spawn), walk to the exit portal → teleported to the world
entrance. Repeat enter/exit twice. Confirm no rogue teleport on entry.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-10 | Story created + researched (exit machinery mapped, hypotheses ranked, verification plan) | dev |
| 2026-07-10 | Ran `[007dbg]` probe in Ragefire Chasm: H1/H2/H3 all disproven — client detects + fires `CMSG_AREATRIGGER 2226` (verified via server SQL as the real RFC exit teleport, no script); server ignores it. Localized to server-side reject at `IsInAreaTriggerRadius` (position). Entrance 2230 works (outdoors, synced); exit fails (in-instance) → likely WoWee position desync inside instances. Decisive next step: `.debug areatrigger`/`.gps` server-side | dev |
| 2026-07-11 | GM cmds unavailable → added `[007dbg-move]` client probe. Result: movement streams fine (desync theory dead). **ROOT CAUSE CONFIRMED**: area-trigger box is transposed — loader swaps trigger X/Y to canonical but not boxLength/boxWidth, so client fires from server-rejected positions on elongated boxes. Fix plan: test box in server frame matching `Position::IsWithinBox`. Ready to implement | dev |
| 2026-07-11 | FIXED + verified: server-frame box test (red→green, 34/34). Exit teleports out. Discovered follow-up: player lands below WMO floor on teleport landing → Story 009 | dev |
