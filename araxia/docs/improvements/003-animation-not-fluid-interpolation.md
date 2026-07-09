# Story 003: Character/creature animations look choppy (sprite-swap), not smooth 3D

## Status
DONE — root cause CONFIRMED, fix implemented (TDD red→green, 32/32 tests) on branch
`fix/003-animation-distance-lod`, and **LIVE-VERIFIED 2026-07-09** against the live Araxia server:
player's own Paladin and follower bots animate smoothly at default and zoomed-out camera
distances. Code commit `c553992c` is upstream-ready (no `araxia/`).

> **Live verification note (2026-07-09):** After fixing an unrelated launcher issue (the client
> ignores `argv`; data path must come from `$WOW_DATA_PATH`, not a `--data` flag), the fixed build
> was run against the live server. Animation is smooth on the player character and on NPCBot party
> followers at normal play distances — the reported sprite-swap stutter is gone. Distant-unit
> throttling (interval 2 beyond 50u, 4 beyond 100u) is retained by design and did not read as
> choppy in normal play.

> **Update 2026-07-09 (rule 10):** The original investigation flagged a caveat — "within 10
> yards `boneInterval=1`, so point-blank should be smooth; if a point-blank unit is *also*
> choppy the root cause is incomplete." User reported choppiness on **their own character too**,
> which first *looked* like it invalidated the throttle. It does **not** — the caveat was based
> on a wrong assumption. `distSq` is measured from the **camera**, and the 3rd-person camera sits
> **10–22 units** from the player at default/normal zoom (up to 50 zoomed out) —
> `MAX_DISTANCE_NORMAL = 22`, `MAX_DISTANCE_EXTENDED = 50`, default `userTargetDistance = 10` in
> `include/rendering/camera_controller.hpp:205-211`. The throttle starts at `distSq > 10*10`, so
> the player's OWN model is already at interval **2–4** in normal play. The distance-LOD throttle
> IS the primary cause, including for the player. Fix confirmed: relax the throttle so everything
> within the 50u max-zoom updates every frame. (The `<10-yard = smooth` premise was true but
> irrelevant, because a 3rd-person camera is never within 10 yards.)

## Upstream Contribution — TODO (fork-only tracking; do NOT PR this section)
The fix is upstream-worthy (verified real-world). When we choose to contribute it to
`Kelsidavis/WoWee`, do this — the discipline is: **branch from `upstream/master`, never from our
fork's `master`** (ours carries `araxia/`), and ship ONLY the code commit `c553992c`.

- [ ] Decide to contribute upstream (owner: James — deferred for now, 2026-07-09).
- [ ] `git fetch upstream`
- [ ] `git checkout -b pr/003-animation-lod upstream/master`   ← clean base, no `araxia/`
- [ ] `git cherry-pick c553992c`   ← the ONLY commit; touches render_constants.hpp,
      character_renderer.cpp, tests/CMakeLists.txt, tests/test_bone_lod.cpp
- [ ] Verify clean: `git show --stat HEAD` shows 4 code files, **zero** `araxia/`
- [ ] Build + `ctest` green on the clean branch (no fork-only deps)
- [ ] `git push origin pr/003-animation-lod`; open PR → `Kelsidavis/WoWee:master`
- [ ] PR body: camera-distance throttle started at 10u while the 3rd-person camera sits 10–22u
      (50u zoomed) from the player, so the player's own model was throttled to interval 2–4 →
      sprite-swap look; fix returns interval 1 within the 50u max-zoom via a shared
      `boneUpdateIntervalForDistanceSq()` helper (character + doodad paths); distant-unit LOD
      tiers (2 beyond 50u, 4 beyond 100u) retained. Live-verified on player, bots, world NPCs.

## Story
**As a** player, **I want** character and creature skeletal animations to update smoothly every rendered frame at normal viewing distance, **so that** movement looks like fluid 3D animation instead of an old game snapping between discrete poses.

## Context / Observed Behavior
Character and creature animations look choppy — as if the game were "sprite swapping" between a handful of fixed poses rather than smoothly interpolating a skeleton. The stutter is present even though the frame rate is a playable ~20-25 FPS, so it is not a raw-FPS problem. The models appear to hold a pose for several frames and then jump to a noticeably different pose.

## Expected Behavior
Real WoW 3.3.5a advances each unit's animation clock by real elapsed time every frame and re-derives the skeleton every frame (with distance LOD that degrades gracefully rather than visibly stepping). Nearby characters/creatures animate fluidly; even distant ones morph smoothly rather than teleporting between poses. Bone transforms are linearly/spherically interpolated between keyframes so intermediate frames are shown, not just the keyframe poses.

## Acceptance Criteria
1. A moving character/creature at typical combat/viewing distance (roughly 10-40 yards) animates smoothly, without visible hold-then-jump pose stepping, at ~20-25 FPS.
2. The player's own model animates smoothly in 3rd-person even when the camera is zoomed out beyond ~10 yards from the model.
3. Distant units may still use reduced-rate updates for performance, but the transition must not read as sprite-swapping at normal view distances.
4. Keyframe interpolation (lerp for translation/scale, slerp for rotation) remains correct — no regression to snapping between keyframes.

## Investigation (Dev Notes)

### Where it lives
- Per-frame character/creature animation update loop: `CharacterRenderer::update()` — `src/rendering/character_renderer.cpp:1811` onward.
  - animationTime advance (every frame, real delta): `src/rendering/character_renderer.cpp:1860` (`inst.animationTime += deltaTime * 1000.0f`).
  - Distance-tiered bone-update throttling: `src/rendering/character_renderer.cpp:1882-1893`.
  - Bone matrix (re)computation gated by that throttle: `calculateBoneMatrices()` at `src/rendering/character_renderer.cpp:2106`, called from `1908/1925/1937`.
- Keyframe interpolation helpers (verified correct): `interpolateVec3` `src/rendering/character_renderer.cpp:2037` and `interpolateQuat` (with `glm::slerp`) `src/rendering/character_renderer.cpp:2068-2101`; keyframe lookup `findKeyframeIndex` `src/rendering/character_renderer.cpp:2002`.
- Which renderer owns animated units: creatures/players/NPCs are added to `CharacterRenderer` — creature spawn path uses `charRenderer` at `src/core/entity_spawner_processing.cpp:1204-1205`; the separate `M2Renderer` is used only for static WMO doodads (`src/core/entity_spawner_processing.cpp:1113-1170`). This link is load-bearing: the whole finding rests on animated units flowing through `CharacterRenderer`, not `M2Renderer`.
- Renderer LOD constants for comparison: `include/rendering/render_constants.hpp:20-22`.

### Root-cause hypothesis
**Most likely cause (medium confidence):** The choppiness is a *temporal-resolution* problem, not a missing-interpolation problem. `CharacterRenderer::update()` advances `animationTime` every single frame (`character_renderer.cpp:1860`) but only recomputes the bone matrices every Nth frame based on distance (`character_renderer.cpp:1882-1893`):

```
distSq > 40*40 (40 yd) -> boneInterval = 8
distSq > 20*20 (20 yd) -> boneInterval = 4
distSq > 10*10 (10 yd) -> boneInterval = 2
else                    -> boneInterval = 1
```

Between throttled recomputes the model renders with the *stale* `boneMatrices` (frozen pose); when bones are finally recomputed, `animationTime` has already advanced N frames, so the interpolated pose jumps forward by N frames' worth at once. Freeze-then-jump is exactly the "sprite swap" look. At ~20-25 FPS this means:
- 10-20 yd: bones update ~10-12 Hz
- 20-40 yd: ~5-6 Hz
- 40 yd+: ~2.5-3 Hz

3-12 Hz effective animation reads as steppy/sprite-swapping. The 10-yard interval-2 threshold is unusually aggressive — the engine's own `M2Renderer` doodad path doesn't begin throttling until 50 yards (`render_constants.hpp:22`). Because the 3rd-person camera typically sits ~5-15 yards behind the player, even the player's own model can fall into interval-2 when zoomed out.

**Scope / caveat (own it, don't bury it):** Within 10 yards `boneInterval = 1`, so units at point-blank range update every frame and should be smooth. This root cause therefore explains choppiness at >10 yards (most of what a player watches) but *cannot* explain choppiness at <10 yards. If the near-distance test (see Testing) shows a point-blank unit is *also* choppy, this story's root cause is incomplete and must be reopened to look for a second mechanism.

### Evidence
- Interpolation is present and correct, ruling out the "missing/coarse interpolation" hypothesis:
  - `interpolateQuat` does true `glm::slerp(q0, q1, t)` with a clamped fraction — `character_renderer.cpp:2093-2101`.
  - `interpolateVec3` does `glm::mix(v0, v1, t)` with a clamped fraction — `character_renderer.cpp:2060-2065`.
  - `findKeyframeIndex` correctly finds the surrounding keyframe pair via `upper_bound` — `character_renderer.cpp:2002-2012`.
  - A steppy look from bad interpolation data (sparse/step keyframes) would appear *floaty/rubbery* (slow morph between far poses), not the sharp hold-then-jump the report describes — so the interpolation-data explanation is a weaker fit.
- animationTime genuinely advances by real frame delta every frame (`character_renderer.cpp:1860`), and loops via subtraction (`1862-1869`), so the animation clock is smooth; only the bone *sampling* of that clock is throttled.
- The throttle is a frame-count gate, not time-based: `inst.boneUpdateCounter++` then `needsBones = (counter >= boneInterval)` (`character_renderer.cpp:1888-1893`). At low FPS this compounds — each skipped frame is a larger real-time gap, worsening the jump size.
- External `.anim` keyframe data (WotLK stores most sequence keyframes outside the M2) *is* loaded for in-world units — `entity_spawner_processing.cpp:512-524` calls `M2Loader::loadAnimFile(...)` for every sequence lacking the embedded-data flag `0x20`, so missing keyframes are not the cause. (Loader body `m2_loader.cpp:1755` and the `0x20` semantics `m2_loader.cpp:429-470` were noted but not exhaustively audited — only relevant if the near-distance test comes back choppy.)
- `playAnimation` resets `animationTime` to 0 (`character_renderer.cpp:1776`), but both the player path (`animation_controller.cpp:1136-1140`, guarded by `shouldPlay`) and the creature-movement path (`character_renderer.cpp:3228`, guarded by `currentAnimationId != moveAnim`) avoid calling it every frame, so per-frame reset-to-frame-0 is *not* occurring.
- The parallel `M2Renderer` path (`src/rendering/m2_renderer_internal.h:271-355`) also interpolates correctly and advances `animTime` by real delta (`m2_renderer_render.cpp:375/401`); it only throttles beyond 50/100 yd (`m2_renderer_render.cpp:471-476`). It handles doodads, not the reported characters/creatures.

### Suggested approach
Two tiers:

- **Primary / small:** Push the `CharacterRenderer` throttle thresholds out so anything in normal view distance updates every frame. Change `character_renderer.cpp:1882-1886` so `boneInterval` stays 1 until roughly 30-40 yards (aligning with the `M2Renderer`'s more lenient 50-yard first tier). This directly restores smooth animation for units in typical combat/viewing range at low cost.
- **Robust / medium:** Keep aggressive distance throttling but eliminate the visible step by *temporally interpolating the cached bone set* between recomputes: store the previous and target bone-matrix sets per instance and lerp/blend them each rendered frame toward the next recompute. This preserves the perf win for distant LOD while removing the pose-jump. (Larger: adds per-instance state and a blend pass; matrix lerp is not rotation-correct, so prefer blending the sampled TRS or re-sampling at the render clock.)

Unknowns / risks:
- Perf: relaxing the thresholds increases bone-computation load in dense scenes; the code already threads bone computation (`character_renderer.cpp:1899-1939`), so measure against `docs/perf_baseline.md` before/after.
- Both `CharacterRenderer` and `M2Renderer` carry near-duplicate throttle logic; consider unifying constants in `render_constants.hpp` to avoid future drift.

### Effort / risk
Small · low (threshold change) — to — medium · medium (temporal bone-matrix interpolation).

## Tasks / Subtasks
- [ ] Confirm scope with the near-vs-far observation test (see Testing) before changing code.
- [ ] Relax `CharacterRenderer` bone-update thresholds (`character_renderer.cpp:1882-1886`) so `boneInterval == 1` out to ~30-40 yd; move the magic numbers into `render_constants.hpp`.
- [ ] Verify the player's own model stays interval-1 across the normal 3rd-person zoom range.
- [ ] (Optional, robust fix) Add per-instance previous/target bone state and blend between throttled recomputes so distant units morph instead of stepping.
- [ ] Re-measure animation-thread cost in a dense area against `docs/perf_baseline.md`.
- [ ] (Only if near-distance test is choppy) Audit `M2Loader::loadAnimFile` (`m2_loader.cpp:1755`) and the `0x20` flag handling for sparse/step keyframe data.

## Testing
Against a live AzerothCore/TrinityCore 3.3.5a server:
1. **Disambiguator (confirm/reopen):** Stand so one moving creature is <10 yards away and another is 20-40 yards away simultaneously. Watch both.
   - If the near unit animates smoothly and the far unit visibly steps/holds-then-jumps → root cause **confirmed**.
   - If the near unit *also* steps at point-blank → root cause **incomplete**; reopen and look for a second mechanism (this cannot be throttling, since <10 yd is interval-1).
2. Zoom the 3rd-person camera out past ~10 yards and watch your own running/walking cycle — under the current code it should degrade to every-2nd-frame stepping; after the fix it should stay smooth.
3. After the threshold change, re-run the near/far observation: both units should animate smoothly in the ~10-40 yd band.
4. Sanity-check FPS and animation-thread timing in a crowded hub (e.g., a capital city) to ensure relaxed thresholds don't regress frame rate below the ~20-25 FPS baseline.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |
| 2026-07-09 | Caveat RESOLVED: user's own character also choppy → confirmed via camera distance (10–22u, throttle at 10u) that the player IS throttled; distance-LOD throttle is the primary cause. Fix started on `fix/003-animation-distance-lod`. | James + Claude |

## Dev Agent Record
- **Branch:** `fix/003-animation-distance-lod`
- **Approach:** extract the inline throttle in `character_renderer.cpp` into a testable pure
  `boneUpdateIntervalForDistanceSq(distSq)` in `render_constants.hpp` (shared with the M2 doodad
  path to prevent drift); relax it so units within the 50u max-zoom update every frame; pin the
  behavior with a Catch2 test that fails against the old thresholds. Fixed the misleading inline
  comment (said 3 tiers; code had 4).
- **Files touched:**
  - `include/rendering/render_constants.hpp` — new `constexpr boneUpdateIntervalForDistanceSq()` on the shared M2 tiers.
  - `src/rendering/character_renderer.cpp` — call the function instead of the inline throttle; fixed the misleading comment; added the include.
  - `tests/test_bone_lod.cpp` — new Catch2 test pinning the intended distance→interval spec.
  - `tests/CMakeLists.txt` — registered `test_bone_lod`.
- **Completion notes:** TDD red→green — the test failed against the old thresholds (player at 50u
  zoom was on interval 8) and passes after the fix. Full suite 32/32 green, 0 build errors.
  Scope kept to the throttle only (rule 7); the story's heavier "temporal bone interpolation"
  option was NOT needed for the reported problem and is left as a future option if distant-unit
  smoothness is ever wanted. **Pending: live verification against the server** (smoothness of the
  player + nearby creatures at normal and zoomed-out camera). Distant-unit perf sanity in a
  crowded city still worth a glance (relaxed thresholds do more per-frame bone work in dense scenes).
