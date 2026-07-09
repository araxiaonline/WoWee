# Story 002: Creatures always play Run animation regardless of walk/run mode or idle state

## Status
Investigated

## Story
**As a** player, **I want** NPCs and creatures to walk when the server tells them to walk, run when they run, and stand idle when they stop, **so that** the world reads correctly (patrolling guards stroll, fleeing critters run, idle mobs stand still) instead of every creature sprinting in place.

## Context / Observed Behavior
Every creature plays its RUN animation (M2 AnimationData id 5) whenever it moves, even creatures the server is walking (patrol guards, ambient wanderers). In addition, many creatures continue to play the looping RUN animation while standing perfectly still (the classic "run in place" bug).

## Expected Behavior
Real WoW 3.3.5a drives NPC locomotion animation from the movement the server sends:
- A `SMSG_MONSTER_MOVE` spline carrying the **Walkmode** spline flag → play **Walk** (id 4).
- A normal (run) spline → play **Run** (id 5).
- When a spline completes and no new movement arrives → return to **Stand** (id 0) (or SwimIdle/FlyIdle as appropriate).
Walk vs run is a server-authoritative property of the move, not something inferred from raw client-side speed.

## Acceptance Criteria
1. A creature the server is walking (Walkmode spline flag, e.g. a patrolling guard) plays Walk (4), not Run (5).
2. A creature the server is running plays Run (5).
3. When any creature's movement spline ends and no follow-up move is received, it returns to Stand (0) within a frame or two and stays there — no run-in-place.
4. Swimming and flying creatures still resolve to Swim/SwimIdle and Fly/FlyIdle respectively (no regression).
5. Death animation is never overridden by locomotion selection (existing guard preserved).

## Investigation (Dev Notes)

### Where it lives
- `src/core/entity_spawn_callback_handler.cpp:131-164` — `setCreatureMoveCallback`, the primary driver of creature locomotion animation on every server move. Line **158** hardcodes `rendering::anim::RUN`. Line **160** sets `_creatureWasMoving[guid] = true`.
- `include/game/spline_packet.hpp:50-65` — `namespace SplineFlag` constant table. **No Walkmode bit is defined.**
- `src/game/spline_packet.cpp:32-` (`parseMonsterMoveSplineBody`) — parses the spline body; `out.splineFlags` is stored but the walk bit is never extracted or surfaced.
- `src/game/movement_handler.cpp:1482-1486` and `:1616-1617` — `SMSG_MONSTER_MOVE` handlers invoke `creatureMoveCallbackRef()(guid, x, y, z, duration)`; the callback signature (`include/game/game_handler.hpp`, `CreatureMoveCallback = void(uint64_t,float,float,float,uint32_t)`) has **no walk flag parameter**.
- `src/core/animation_callback_handler.cpp:306-319` — `setUnitMoveFlagsCallback` is the *only* writer of `creatureWalkingState_` (via `getCreatureWalkingState()[guid]=true` when `MovementFlags::WALKING` is set). It is fed from `MSG_MOVE_*` movement-info flags (`movement_handler.cpp:1110-1111`) and object-update blocks (`entity_controller.cpp:1409-1411,1478-1480`) — packets that **players/other players emit, but ordinary NPC spline patrols do not**.
- `src/core/application.cpp:1934-1969` — the per-frame "authoritative" locomotion re-selection (`isWalkingNow ? WALK : RUN`, `STAND` when idle). This is the code that *could* pick Walk, but see below why it doesn't.
- `src/core/application.cpp:1799` — creature sync loop early `continue` when `canonDistSq > syncRadiusSq` (320u), *above* the animation block.
- `src/rendering/character_renderer.cpp:1833-1842` — completes move interpolation (`inst.isMoving=false`) with **no** animation reset. `moveInstanceTo` only resets to Stand on an explicit zero-distance stop (`:3198-3201`).
- `include/rendering/animation/animation_ids.hpp:23-28` — confirms STAND=0, WALK=4, RUN=5.

### Root-cause hypothesis
**Primary ("always run regardless of speed") — HIGH confidence.** Walk-mode is never available on the creature path, so Run is always chosen:
1. The server's per-move Walkmode signal lives in the `SMSG_MONSTER_MOVE` spline flags, but `SplineFlag` (`spline_packet.hpp:50`) defines no Walkmode bit and the parser never extracts it.
2. The `creatureMoveCallback` signature carries no walk flag, and its implementation (`entity_spawn_callback_handler.cpp:158`) unconditionally plays `anim::RUN` for any spline with `duration>0`.
3. The one place that *can* select Walk (`application.cpp:1957-1961`) keys off `creatureWalkingState_`, which is only populated from `MSG_MOVE_*` WALKING flags — packets NPCs on spline patrols don't send — so `isWalkingNow` is effectively always false for creatures.

**Secondary ("run while standing still") — MED confidence.** RUN is a looping animation whose only teardown to STAND is the edge-triggered per-frame sync in `application.cpp:1944-1967`. That reset is skippable: the loop early-`continue`s at `:1799` for creatures beyond 320u (a creature that stops while sync-culled keeps looping RUN), and the block is also skipped when the entity is momentarily absent/type-mismatched (`:1770`). Crucially, `character_renderer.cpp:1836-1838` completes the interpolation without any fallback STAND, so nothing in the renderer itself corrects a stuck looping locomotion anim.

### Evidence
- `entity_spawn_callback_handler.cpp:157-159`:
  ```cpp
  if (!gotState || (curAnimId != rendering::anim::DEATH && curAnimId != rendering::anim::RUN)) {
      cr->playAnimation(instanceId, rendering::anim::RUN, /*loop=*/true);
  }
  ```
  Hardcoded RUN; no walk/run branch; comment at `:144-145` even says "Play Run animation (anim 5) for the duration of the spline move."
- `spline_packet.hpp:50-65`: flags FINAL_POINT/TARGET/ANGLE, CATMULLROM, CYCLIC, ENTER_CYCLE, ANIMATION, PARABOLIC — **no Walkmode**. Grep for `WALKMODE`/`Walkmode`/`0x00001000` across `src/game` returns nothing.
- **Pre-empting the obvious objection** ("doesn't `application.cpp:1957` override RUN with WALK?"): No. (a) `isWalkingNow = _creatureWalkingState.count(guid) > 0` is ~never true for spline-driven NPCs because that map is only fed by `MSG_MOVE`/object-update WALKING flags (`animation_callback_handler.cpp:306-316`). (b) The re-selection is gated on `stateChanged`, but the callback pre-sets `_creatureWasMoving[guid]=true` (`:160`), suppressing the moving rising-edge. So the hardcoded RUN wins for the duration of the move.
- Standing-still teardown gap: `application.cpp:1799` `if (canonDistSq > syncRadiusSq) continue;` sits above the STAND-reset block (`:1962-1966`); `character_renderer.cpp:1836-1838` sets `isMoving=false` with no `playAnimation(..., STAND)` fallback (contrast `:3199-3201` which only fires on an explicit zero-distance `moveInstanceTo`).

### Suggested approach
1. **Plumb Walkmode end-to-end (fixes the primary bug):**
   - Add the Walkmode constant to `SplineFlag` (`spline_packet.hpp`). **Verify the exact bit against the server's `MoveSplineFlag::Walkmode` in AzerothCore/TrinityCore 3.3.5a** before committing a value — do not assume `0x00001000`.
   - Extract it in `parseMonsterMoveSplineBody` into `SplineBlockData` (e.g. `bool walkmode`).
   - Extend `CreatureMoveCallback` to carry the walk flag (or a small enum: walk/run/swim/fly), and pass it from every `creatureMoveCallbackRef()` call site in `movement_handler.cpp`/`entity_controller.cpp`.
   - In `entity_spawn_callback_handler.cpp:setCreatureMoveCallback`, select `anim::WALK` vs `anim::RUN` from that flag instead of hardcoding RUN. Keep the existing swim/fly resolution.
2. **Guarantee return-to-idle (fixes the secondary bug):** in `character_renderer.cpp` update loop where interpolation completes (`:1836-1838`), if the just-finished animation is a looping locomotion anim (WALK/RUN/SWIM/FLY_FORWARD) and no new move is queued, reset to the matching idle (STAND/SWIM_IDLE/FLY_IDLE) — so the renderer self-heals independent of the `application.cpp` sync edge-trigger. Alternatively/additionally, ensure the STAND-reset block in `application.cpp` runs before the `:1799` distance cull (or is re-evaluated on re-entry) for creatures that stop while culled.
3. Reconcile the two competing drivers (the `creatureMoveCallback` immediate play vs the `application.cpp:1934-1969` per-frame re-selection) so they agree on walk/run source of truth; prefer the server spline flag as authoritative.

**Unknowns / risks:** exact Walkmode bit value (must be server-verified); whether some cores additionally send `SMSG_SPLINE_MOVE_SET_WALK_MODE` (already synth-handled at `movement_handler.cpp:59`) so that persistent walk state should also be honored between splines; ensuring the idle-reset heuristic doesn't stomp emote/combat/one-shot animations (guard on looping locomotion ids only, keep the existing Death guard).

### Effort / risk
Medium · Medium. Touches packet parsing, a cross-module callback signature, and two animation drivers, but each change is localized and well-bounded. Main risk is the unverified spline bit and not regressing swim/fly/emote handling.

## Tasks / Subtasks
- [ ] Confirm the Walkmode spline-flag bit value against AzerothCore/TrinityCore 3.3.5a `MoveSplineFlag`.
- [ ] Add Walkmode to `SplineFlag` (`include/game/spline_packet.hpp`) and extract it into `SplineBlockData` in `parseMonsterMoveSplineBody` (`src/game/spline_packet.cpp`).
- [ ] Extend `CreatureMoveCallback` to carry walk (and ideally swim/fly) state; update all `creatureMoveCallbackRef()` call sites in `movement_handler.cpp` and `entity_controller.cpp`.
- [ ] Replace the hardcoded `anim::RUN` in `entity_spawn_callback_handler.cpp:158` with walk/run selection from the new flag.
- [ ] Add a return-to-idle fallback in `character_renderer.cpp` when a looping locomotion animation's move interpolation completes (`:1836-1838`).
- [ ] Ensure the `application.cpp` STAND-reset is not permanently skipped by the `:1799` distance cull for creatures that stop while culled.
- [ ] Verify swim/fly and emote/combat animations are unaffected.

## Testing
Against a live AzerothCore/TrinityCore 3.3.5a server:
1. Stand near a patrolling guard that the server walks — confirm Walk (4), not Run (5). (Enable `[AnimDbg]` logging / inspect `getAnimationState` to read the played id.)
2. Aggro a fleeing critter / mob commanded to run — confirm Run (5).
3. Let a patrolling creature reach a waypoint and idle — confirm it returns to Stand (0) within ~1-2 frames and does not run in place.
4. Repeat #3 at >320u then walk toward the creature — confirm it is standing (not running) on approach (validates the cull-gap fix).
5. Test a swimming murloc and a flying gryphon/bat — confirm Swim/SwimIdle and Fly/FlyIdle still resolve correctly.
6. Kill a moving creature mid-spline — confirm Death (1) plays and is not overridden by locomotion selection.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |

## Dev Agent Record
_(populated during implementation)_
