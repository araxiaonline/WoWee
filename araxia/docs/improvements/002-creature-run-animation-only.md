# Story 002: Creatures always play Run animation regardless of walk/run mode or idle state

## Status
In Progress — **Part 1 (walk/run) reworked to SPEED INFERENCE** for WotLK/AzerothCore, implemented +
unit-tested (TDD red→green) on branch `fix/002-creature-run-animation` (branched from `master`,
independent of #3). Awaiting live verification. Part 2 (return-to-idle / "run in place") still deferred.

> ### ⚠️ Major course-correction (2026-07-09) — the original "Walkmode spline flag" premise was WRONG for this server
> The first implementation read a `Walkmode` spline-flag bit (assumed `0x1000`) from `SMSG_MONSTER_MOVE`.
> Live testing against the Araxia AzerothCore server disproved it: patrolling Orgrimmar orcs moved at
> **walk speed but played the run animation**, and a diagnostic showed **every** creature move arriving with
> `splineFlags = 0x0` (485/485 samples). Reading the **AzerothCore server source** (`~/github.com/araxiaonline/AzerothCore-wotlk-with-NPCBots`)
> plus **cmangos classic/TBC** clones settled it definitively:
>
> 1. **AzerothCore (WotLK) has no Walkmode spline flag at all.** In `MoveSplineFlag::Enum`, bit `0x1000` is
>    `CanSwim`, and there is no walk/run bit. `MoveSplineInit::SetWalk(enable)` only sets `args.walk`
>    (spline **velocity**), never a serialized flag or `MOVEMENTFLAG_WALKING`. `WaypointMovementGenerator`
>    uses `init.SetWalk(...)` (velocity), not `Unit::SetWalk(...)` (which would set `MOVEMENTFLAG_WALKING`).
> 2. **`splineFlags = 0x0` is CORRECT for ground creatures.** `WriteCommonMonsterMovePart` writes **linear**
>    paths (`WriteLinearPath`) for ground units and masks the flags with `~Mask_No_Monster_Move`, so a normal
>    ground move legitimately carries no flags. The client reads them at the right offset (byte order matches
>    the server serializer exactly). Not a parse bug.
> 3. **Walk vs run is therefore expressed ONLY by the spline's speed** (walk ≈ 2.5 yd/s, run ≈ 7.0 yd/s).
>    The fix must **infer** it from `pathLength / duration`, not read a flag.

### Cross-core spline-flag comparison (verified against source)
WoWee's `SplineFlag` table is the **mangos (Classic/TBC)** layout — it does **not** match WotLK.

| Bit | mangos Classic/TBC = **WoWee's table** | WotLK (AzerothCore/TrinityCore) |
|-----|----------------------------------------|--------------------------------|
| `0x100`  | `Runmode`/`Walkmode` (walk/run signal) | `Done` |
| `0x200`  | `Flying` | `Falling` |
| `0x800`  | (unknown) | `Parabolic` |
| `0x1000` | (unknown) | `CanSwim` |
| `0x8000` | (unknown) | `Final_Point` |
| `0x10000`| `Final_Point` | `Final_Target` |
| `0x40000`| `Final_Angle` | `Catmullrom` |
| `0x80000`| (unknown) | `Cyclic` |
| **walk/run** | **spline flag bit `0x100`** | **spline velocity — NO flag** |

**Implications:**
- Classic/TBC/Turtle DO have a walk/run flag (`0x100`); WotLK does not.
- WoWee parsing WotLK spline flags with the Classic/TBC table means flying/catmullrom/cyclic paths are
  **misinterpreted** (wrong uncompressed-vs-compressed waypoint branch). Ground creatures are unaffected
  (`0x0` either way). → **Separate movement-layer issue** (expansion-blind spline flags); filed apart from 002.

## Story
**As a** player, **I want** NPCs and creatures to walk when the server is walking them, run when they run, and stand idle when they stop, **so that** the world reads correctly (patrolling guards stroll, fleeing critters run, idle mobs stand still) instead of every creature sprinting in place.

## Context / Observed Behavior
Every creature plays RUN (anim 5) whenever it moves — even ones the server walks (patrol guards, ambient wanderers), which visibly move at **walk speed** while showing the **run** animation. Many creatures also keep looping RUN while standing perfectly still ("run in place").

## Expected Behavior
- A creature moving at walk speed plays **Walk** (4); at run speed plays **Run** (5).
- When a move spline ends and no follow-up arrives, the creature returns to **Stand** (0).
- Walk/run is derived from the **server-sent movement** (on WotLK: the spline's speed), never guessed from nothing.

## Acceptance Criteria
1. A creature the server is walking (walk-speed spline, e.g. a patrolling guard) plays Walk (4), not Run (5).
2. A creature the server is running plays Run (5).
3. When a creature's spline ends and no follow-up move is received, it returns to Stand (0) within a frame or two — no run-in-place. *(Part 2)*
4. Swimming/flying creatures still resolve to Swim/SwimIdle and Fly/FlyIdle (no regression).
5. Death animation is never overridden by locomotion selection (existing guard preserved).

## Root cause (corrected)
1. **Walk never selected (primary).** The creature-move path hardcoded RUN and never derived walk. On WotLK there is no flag to read, so walk must be inferred from spline speed.
2. **Per-frame override (why a one-shot fix didn't stick).** `application.cpp` re-selects Walk/Run **every frame** from `getCreatureWalkingState()` (aliased at `:1762` and `:2005`) and defaults to RUN when the guid is absent. That map is only fed by `MSG_MOVE_*` WALKING flags — which spline-driven NPCs never send — so it overrode any one-shot `playAnimation` in the move callback. The fix must feed that map.
3. **Run-in-place (secondary, Part 2).** RUN's only teardown to STAND is the edge-triggered per-frame sync, which is skippable when the unit is sync-culled (>320u). A `LocomotionFSM` with a WALK/RUN→IDLE grace timer already exists and is the right home.

## Approach (implemented for WotLK)
1. **Speed inference** — `isWalkingSpeed(pathLength, durationMs)` in `include/game/spline_packet.hpp`:
   `speed = pathLength / (duration/1000)`; walk if `speed <= WALK_RUN_SPEED_THRESHOLD` (4.0 yd/s, midway
   between base walk 2.5 and run 7.0). Universal signal — also covers Classic/TBC without their flag.
2. **Compute at the move sites** — `movement_handler.cpp` primary path sums the true path length
   (start→waypoints→dest) and passes `walk`; the transport path uses straight-line distance. Snap/stop
   sites (duration 0) pass `false`.
3. **Reconcile the two drivers** — the move callback (`entity_spawn_callback_handler.cpp`) feeds
   `getCreatureWalkingState()` (`walk` → set, else erase; same contract as `setUnitMoveFlagsCallback`), so
   the per-frame selector in `application.cpp` agrees instead of overriding. Also selects `WALK`/`RUN`
   directly and compares the "already playing" guard against the selected anim so walk↔run transitions restart.
4. **`bool walk` callback signature** carries the decision through all 13 `creatureMoveCallbackRef()` sites.

## Tasks / Subtasks
- [x] Determine how walk/run is conveyed on the target server → **spline speed** (WotLK/AC has no walk flag). Verified against AzerothCore + cmangos sources.
- [x] Add `isWalkingSpeed()` + `WALK_RUN_SPEED_THRESHOLD` (spline_packet.hpp); remove the wrong `WALKMODE`/`splineIsWalking`.
- [x] Compute `walk` from path length ÷ duration at the spline move sites; thread `bool walk` through `CreatureMoveCallback` (13 sites; 2 real, 11 snap→false).
- [x] Select `WALK` vs `RUN` in the callback and **feed `getCreatureWalkingState()`** so the per-frame `application.cpp` selector agrees (the override fix).
- [x] Unit test `isWalkingSpeed` (`tests/test_walk_speed.cpp`) — walk/run boundary + degenerate cases; TDD red→green.
- [ ] **Live-verify** on AzerothCore: patrolling orcs walk; running mobs run; tune `WALK_RUN_SPEED_THRESHOLD` from the temporary `[002dbg]` speed log if needed.
- [ ] **Part 2:** route creature stop→idle through `LocomotionFSM` (grace timer). Fixes "run in place" + Story 006 half.
- [ ] Remove the temporary `[002dbg]` speed logging before committing the final code.
- [ ] Correct swim/fly regression check once live.

## Testing
Live against AzerothCore 3.3.5a:
1. Patrolling Orgrimmar orc / city guard → Walk (4), not Run (5).
2. Aggro'd/fleeing mob → Run (5).
3. Read the temporary `[002dbg]` log: walkers should cluster near ~2.5 yd/s, runners near ~7 yd/s — confirms the 4.0 threshold separates them, tune if reality differs.
4. Swimming murloc / flying gryphon → Swim/Fly unaffected.
5. (Part 2) creature stops → returns to Stand, no run-in-place.

## Dev Agent Record
### Attempt 1 — Walkmode spline flag (ABANDONED)
Read `WALKMODE=0x1000` from the spline flags. Built, unit-tested, committed — then live testing showed
walk-speed/run-anim and `splineFlags=0x0` everywhere. Server source proved WotLK has no walkmode flag.
Branch history was reset to `master` and the approach removed. Lesson: the `SplineFlag` table was a
Classic/TBC (mangos) reference, not WotLK — verify wire specs against the actual target server's source.

### Attempt 2 — Speed inference (CURRENT)
- **Files:** `include/game/spline_packet.hpp` (`isWalkingSpeed` + threshold; corrected flag-table comment),
  `include/game/game_handler.hpp` (`bool walk` signature), `src/core/entity_spawn_callback_handler.cpp`
  (feed `getCreatureWalkingState()` + select WALK/RUN), `src/game/movement_handler.cpp` (path-length →
  `isWalkingSpeed` at spline sites; temp `[002dbg]` speed log), `src/game/entity_controller.cpp` (snap→false),
  `tests/test_walk_speed.cpp` + `tests/CMakeLists.txt`.
- **TDD:** stubbed `isWalkingSpeed → false` (old always-run) → walk cases FAILED (red); real impl → 11/11 pass (green).
- **What survived the pivot:** the per-frame-override reconciliation finding and the `bool walk` plumbing —
  only *how `walk` is computed* changed (speed instead of flag).

### Reference material
Server sources cloned locally for cross-core verification: `~/github.com/araxiaonline/AzerothCore-wotlk-with-NPCBots`
(WotLK), `~/github.com/cmangos/mangos-classic`, `~/github.com/cmangos/mangos-tbc`.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |
| 2026-07-09 | Attempt 1 (Walkmode flag) implemented, then INVALIDATED by live test + server source | dev |
| 2026-07-09 | Cross-core comparison (AC vs cmangos classic/TBC); confirmed WotLK has no walk flag | dev |
| 2026-07-09 | Pivoted to speed inference; implemented + unit-tested (red→green); branch history reset | dev |
