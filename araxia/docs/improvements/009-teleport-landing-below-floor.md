# Story 009: Player lands below the WMO floor after a cross-map teleport (instance exit)

_(Discovered while verifying Story 007. Story authored per Rule 11 — story before code.)_

## Status
**✅ DONE — FIXED + LIVE-VERIFIED 2026-07-11 (branch `fix/007-instance-exit`, commit `b02d202a`).**
Attempt 2 (data-driven one-shot floor snap from the server target Z) works. User exited Ragefire Chasm and
**"appeared in the correct place… definitely NOT below the floor or above this time."** A diagnostic gave
the exact Z stack (below), which made the correct rule obvious. 34/34 tests pass.

> **Note — the descriptor-pool crash (issue #8) is a separate blocker.** After the (correct) landing, the
> user hit a hard SIGSEGV in `CharacterRenderer::prepareRender` (Vulkan descriptor-pool exhaustion) on the
> dense Orgrimmar re-spawn. That is byte-for-byte the same pre-existing crash seen before this fix, does
> **not** touch the landing/movement path, and is tracked separately as issue #8 / Story 010. Story 009's
> landing behavior itself is verified correct.

> ### ✅ Diagnostic data (RFC exit → Orgrimmar, 2026-07-11) — the numbers that fixed it
> Logged the Z stack at the landing (map 1):
> - **server target Z = −18.57** (the intended Cleft of Shadow floor)
> - **player placed at −25.08 == terrain (−25.076)** → the continent load dropped the player onto raw
>   terrain, ~6.7u **below** the WMO floor.
> - `getFloorHeight` probe ladder revealed the surfaces at that (x,y):
>   - probe from **+40** (14.92): wmoFloor = **+2.24**  ← the CEILING/roof (why Attempt 1 snapped onto the roof)
>   - probe from **+10** (−15.08): wmoFloor = **−18.345**  ← the actual Cleft FLOOR (≈ server target −18.57)
>   - probe from **+3 and below**: none (nothing below the floor)
>
> **Correct rule:** probe from just above the **server target Z**, not the player's fallen-to-terrain Z.
> `getFloorHeight(x, y, serverTargetZ + 5)` = `getFloorHeight(−13.57)` → returns the highest floor ≤ −13.57
> = **−18.345** (the floor), and the ceiling (+2.24 > −13.57) is excluded. Attempt 1 probed from
> `playerZ + 40` = +14.92, which is above the ceiling, so it caught +2.24.

### Attempt 2 — one-shot floor snap from the server target Z (CURRENT)
At the cross-map continent landing (`world_loader.cpp`, final map `IN_GAME`, guarded
`isMapTransfer && !isWMOOnlyMap`): probe WMO/M2 `getFloorHeight(rp.x, rp.y, z + 5)` (z = server target Z ==
render Z), and if a floor is found **above** the current (terrain) Z and **within 15u** of the server
target (sanity), lift the player onto it (`floor + 0.1`) and sync to the server. One-shot — the Cleft WMO
collision is loaded by `IN_GAME` (the diagnostic found −18.345 there). No freeze-clamp, no ceiling
overshoot, no per-frame cost.

> ### ⚠️ Attempt 1 — arm the taxi landing clamp (REVERTED)

> ### ⚠️ Attempt 1 — arm the taxi landing clamp on cross-map transfer (REVERTED)
> Idea: arm the existing landing clamp (`application.cpp:1475-1514`, probe terrain/WMO/M2, snap to the
> HIGHEST, freeze until loaded) on cross-map transfers. Two regressions killed it:
> 1. **Froze instance ENTRY.** Arming on *every* transfer (`isMapTransfer`) armed it for WMO-only
>    instances too — which have no terrain, so the clamp could not resolve a floor and **froze the player
>    on entry** ("can't get into Ragefire"). Narrowed the guard to `isMapTransfer && !isWMOOnlyMap`
>    (continents only, since instances already floor-snap at `world_loader.cpp:688-705`). That fixed entry.
> 2. **Snapped the exit onto the CEILING.** With the continent-only guard, exiting RFC then placed the
>    player **above the Cleft of Shadow roof**, not on its floor. Root of the overshoot: `getFloorHeight(x,
>    y, glZ)` accepts the highest floor **at or below `glZ`**, and the clamp probes from `p.z + 40`. When
>    the player lands *below* the walkable floor, `p.z + 40` reaches above the floor and the "highest floor
>    below the probe" is the WMO **ceiling** → snap onto the roof. The "snap to highest floor" heuristic
>    cannot distinguish floor from ceiling in a multi-surface WMO like the Cleft.
>
> Both story-009 code commits were reverted. State restored to: **entry works, exit teleports out but lands
> a bit below the floor** (the pre-009 behavior after the 007 fix).

### Re-approach (diagnostic first — do NOT guess again)
The landing Z resolution is geometry-sensitive; guessing has cost two regressions. Before another fix,
**observe the actual Z values** at an RFC exit landing with a temporary diagnostic logging, at the
landing point (server target ≈ (1813.49, −4418.58, −18.57)):
- the server-target Z the player is placed at,
- the client terrain height at (x,y),
- the WMO floor(s) `getFloorHeight` returns probing from a few reference Zs (target Z + small margin, and
  from high above), and any ceiling hit,
- the player's final render Z.

Then choose the correct rule. Likely the right one is **not** "highest floor" but "the walkable floor at or
just below the server's target Z" — i.e. probe `getFloorHeight(x, y, serverTargetZ + small)` so the search
finds the Cleft floor beneath the intended landing, never the ceiling above it. That also explains why the
server Z alone leaves the player slightly low (client WMO floor sits a little above the server target),
which a downward-from-target probe corrects without overshooting.

### (superseded) Attempt-1 plan — kept for the record

## Story
**As a** player, **I want** to land on the floor after teleporting between maps (e.g. exiting an
instance), **so that** I arrive standing in the world instead of stuck below the ground.

## Context / Observed Behavior
After the Story 007 fix, exiting Ragefire Chasm teleports the player to Orgrimmar — but the player lands
**below the Orgrimmar floor**, on the terrain under the city (X/Y correct, Z wrong). Same visual family as
issue #6 (below-floor), but on the **local-player teleport-landing** path, which the #6 fix (creature/bot
sync loops) did not touch.

## Expected Behavior
The player lands on the correct surface — the Cleft of Shadow WMO floor at the exit point — not on the
raw terrain below it.

## Acceptance Criteria
1. Exiting RFC lands the player standing on the Cleft of Shadow floor in Orgrimmar.
2. Generalizes to other cross-map teleports that land inside a WMO (other instance exits, portals).
3. No regression to normal continent-to-continent teleports that land on open terrain (still land on terrain).
4. No regression to initial login spawn.

## Root cause (CONFIRMED)
Two load paths place the player differently:
- **Instance maps** (`world_loader.cpp:688-705`): after loading the instance's single root WMO, the code
  **snaps the player to the WMO floor** (`wmoRenderer->getFloorHeight(x, y, z+50)` → set Z). Works.
- **Continent maps** (Orgrimmar = map 1): loaded via the terrain-ADT path, which does **not** snap the
  player to a WMO floor. The Cleft of Shadow is a WMO placed on the Orgrimmar terrain; the server's exit
  target Z (−18.57, from `areatrigger_teleport` 2226) is below the Cleft floor. So the player lands under it.

The floor-recovery machinery already exists and is reusable:
- The **landing clamp** (`application.cpp:1475-1514`) runs each frame while `taxiLandingClampTimer_ > 0`:
  probe terrain + WMO + M2 floors at the player's X/Y, snap Z to the **highest**, and freeze the player
  (externalFollow) until a floor is found (handles async terrain/WMO load). Currently armed **only for taxi
  landings** (`application.cpp:1469`, gated on `getLastTaxiFlight()`).
- `WorldEntryCallbackHandler::sampleBestFloorAt(x, y, probeZ)` (`world_entry_callback_handler.cpp:39`) —
  the same "highest of terrain/WMO/M2" picker, used by `/unstuck` to "snap up to WMO floors when fallen below".

The cross-map load path explicitly **clears** the clamp (`world_loader.cpp:1095`,
`world_entry_callback_handler.cpp` various), so nothing snaps the player on a continent landing.

## Fix (approach)
Arm the existing landing clamp after a **cross-map** world-transfer load completes, so the generic
floor-recovery snaps the player onto the WMO floor (and freezes until it loads). In
`WorldLoader::loadOnlineWorldTerrain`:
1. Capture `isMapTransfer = (loadedMapId_ != 0xFFFFFFFF && loadedMapId_ != mapId)` at function entry, before
   `loadedMapId_` is updated — true only for real map changes, **false on initial login** (AC #4) and
   same-map (AC handled by instance branch / same-map teleport branch).
2. When the final map reaches `IN_GAME` (`world_loader.cpp:1104`), if `isMapTransfer`, arm
   `worldEntryCallbacks_->setTaxiLandingClampTimer(2.0f)`. The clamp then probes floors and snaps the
   player up to the WMO floor (Cleft), releasing once grounded. The clamp already picks `max(terrain, wmo,
   m2)`, so open-terrain landings snap to terrain (AC #3, no regression).

**Why the clamp over a one-shot snap:** continent WMOs can finish loading a frame or two after the blocking
load; the clamp freezes the player and keeps probing until a floor is found, avoiding a fall-through race.

## Rule-2 note (test skipped — decide together, Rule 9)
This fix arms a timer in the async world-load/render/collision integration path; the actual floor-selection
logic (`sampleBestFloorAt` / the clamp's `max(terrain,wmo,m2)`) already exists and is exercised by taxi
landings and `/unstuck`. There is no isolated pure unit worth a new test here (a unit test would be theater
per Rule 6). Verification is inherently live: exit RFC → land on the Cleft floor. Flagging the skip.

## Tasks / Subtasks
- [x] Capture `isMapTransfer` at `loadOnlineWorldTerrain` entry (before `loadedMapId_` changes).
- [x] (Attempt 1, reverted) Arm the landing clamp — froze entry, then snapped onto the ceiling.
- [x] Diagnostic: log the Z stack at the RFC-exit landing (server target / terrain / WMO floor+ceiling).
- [x] Attempt 2: one-shot floor snap probing `getFloorHeight(x, y, serverTargetZ + 5)`, guarded
      `isMapTransfer && !isWMOOnlyMap`, lift onto floor+0.1 and sync to server.
- [x] Live-verify: exit RFC → **stood on the Cleft floor, correct spot** (user confirmed). Login/entry
      unaffected (guarded on map change + `!isWMOOnlyMap`).

## Testing
Live against AzerothCore: exit RFC → land on the Orgrimmar Cleft floor (not below). Regression: a normal
open-world teleport lands on terrain; logging in spawns correctly.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-11 | Story created + researched: continent-load path lacks the WMO floor-snap the instance path has | dev |
| 2026-07-11 | Attempt 1 (arm taxi landing clamp) REVERTED — froze instance entry, then snapped exit onto the WMO ceiling (getFloorHeight `p.z+40` probe catches the roof). Re-approach: diagnostic-first, probe floor at/below the server target Z instead of "highest floor" | dev |
| 2026-07-11 | Attempt 2 (one-shot floor snap from `serverTargetZ + 5`, commit `b02d202a`) — **LIVE-VERIFIED**: user exited RFC and appeared in the correct spot, not below/above the floor. Story DONE. (Descriptor-pool crash on re-spawn is separate → issue #8 / Story 010.) | dev |
