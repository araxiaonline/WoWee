# Story 005: Camera collision is WMO-only — no auto-zoom near terrain/M2, and the camera dips under sloped terrain

## Status
Investigated

## Story
**As a** player, **I want** the camera to auto-pull-in when world geometry (walls, cliffs, trees, ceilings) comes between it and my character, and to never sink below the ground, **so that** I always see my character and never see through walls or under the world.

## Context / Observed Behavior
When zoomed out in third-person, the camera ignores most world collision:
- Backing up against a **terrain cliff/hillside** or an **M2 doodad** (tree, rock, fence) does **not** pull the camera in — it clips straight through the geometry.
- On slopes / at cliff edges the camera swings behind/below the terrain and **dips under the ground**, letting you see below the world.

Only solid WMO building **walls** reliably pull the camera in; everything else is ignored.

## Expected Behavior
Real WoW 3.3.5a: the chase camera continuously collides with the world. When anything (WMO wall, terrain, doodad, ceiling) is between the camera and the player it auto-pulls-**in** along the camera→pivot ray so the character stays visible, and the camera is **pushed up** so it never penetrates the ground. Zoom-out resumes smoothly once the obstruction clears.

## Acceptance Criteria
1. Backing the camera into a **terrain cliff/steep hill** auto-pulls the camera in; it never clips through terrain.
2. Backing the camera into an **M2 doodad** (large tree/rock/structure) auto-pulls the camera in.
3. Standing on a slope / cliff edge and orbiting the camera never lets it drop **below the terrain surface at the camera's own XY** — no "see under the world."
4. Existing **WMO wall** auto-zoom and **WMO interior** behavior are preserved (no regressions).
5. Pull-in stays smooth (fast in, slow recover) and does not jitter on flat ground.

## Investigation (Dev Notes)

### Where it lives
- Orbit-camera update + collision: `src/rendering/camera_controller.cpp`, `CameraController::update()` third-person branch, specifically the "WoW-style orbit camera" block `camera_controller.cpp:1457-1661`.
  - Auto-zoom pull-in raycast: `camera_controller.cpp:1528-1557` (only `wmoRenderer->raycastBoundingBoxes(...)` at `:1536`).
  - Terrain floor helper: `getTerrainFloorAt` lambda `camera_controller.cpp:1559-1565` (calls only `terrainManager->getHeightAt`).
  - "Final floor clearance" clamp: `camera_controller.cpp:1586-1647`, using `selectReachableFloor(..., 0.5f)` at `:1639-1640` and the feet-relative clamp at `:1647`.
  - `selectReachableFloor` definition: `camera_controller.cpp:23-41` (rejects floors above `refZ + maxStepUp`).
  - Terrain-clearance pivot lift (partial mitigation): `camera_controller.cpp:1461-1496` (capped at 1.4m, throttled).
- WMO camera raycast: `WMORenderer::raycastBoundingBoxes` `src/rendering/wmo_renderer.cpp:3624-3743`.
- M2 camera raycast that exists but is **never called** by the camera: `M2Renderer::raycastBoundingBoxes` `include/rendering/m2_renderer.hpp:375`.
- Terrain has **no** ray intersection at all — only vertical sampling: `TerrainManager::getHeightAt` `src/rendering/terrain_manager.cpp:2249`.
- Collision deps are correctly wired to the camera (so this is not a null-pointer wiring bug): `src/rendering/renderer.cpp:2041-2049`, `:2218`, `:2302-2308`.

### Root-cause hypothesis
The task's premise ("camera→world raycast missing entirely") is **incorrect** — a WMO camera raycast and a terrain floor clamp both exist. The real defect is that camera collision is **WMO-only and asymmetric**, producing two distinct failures:

**A — "clips through walls/cliffs/trees" (confidence: high).** The auto-zoom pull-in at `camera_controller.cpp:1536` raycasts **only** WMO wall triangles. It never raycasts terrain (no terrain ray API exists) and never calls `M2Renderer::raycastBoundingBoxes` (which exists at `m2_renderer.hpp:375`). So terrain cliffs/hillsides and M2 doodads never shorten `collisionDistance`, and the camera passes through them.

**B — "dips under terrain" (confidence: high).** There is no ray pull-in against terrain; the only protection is a post-hoc vertical nudge (`camera_controller.cpp:1586-1647`). Both of its clamps fail on uneven ground:
1. The push-up-to-floor clamp (`:1642-1643`) is gated by `selectReachableFloor(camTerrainH, camWmoH, camZ, 0.5f)` (`:1639-1640`). Per `selectReachableFloor` (`:23-41`, line `:30`), terrain is only considered when `*terrainH <= camZ + 0.5`. Terrain more than 0.5m **above** the camera — precisely the deep-clip case — is discarded as "unreachable," so no clamp is applied and the camera stays under the ground. The smoothed camera position (`:1584`) can overshoot the 0.5m band in one frame and is then never rescued.
2. The fallback clamp `smoothedCamPos.z = max(smoothedCamPos.z, targetPos.z + 0.15f)` (`:1647`) is relative to the **player's feet**, not the terrain under the **camera's XY**. On a slope, when the player is downhill of where the camera orbits, terrain-at-camera-XY is above player-feet-Z, so this clamp does not lift the camera high enough.

This also explains the **intermittency**: on flat ground the feet-relative clamp (`:1647`) keeps the camera above ground incidentally, so the bug is invisible; it only manifests on slopes / cliff edges.

### Evidence
- `camera_controller.cpp:1535-1536`: `if (wmoRenderer && currentDistance > MIN_DISTANCE) { float rawHitDist = wmoRenderer->raycastBoundingBoxes(...); }` — the sole pull-in ray; no terrain/M2 equivalent anywhere in the block.
- `grep raycastBoundingBoxes src/rendering/camera_controller.cpp` → single hit at `:1536` (WMO). `M2Renderer::raycastBoundingBoxes` (`m2_renderer.hpp:375`) is defined but has zero call sites in the camera.
- `terrain_manager.cpp:2249` `getHeightAt(glX, glY)` is the only terrain query — a vertical column sample, not a ray; no ray-vs-heightmap function exists.
- `camera_controller.cpp:1639-1640` passes `maxStepUp = 0.5f`; `selectReachableFloor` `:30` `if (terrainH && *terrainH <= refZ + maxStepUp) reachTerrain = terrainH;` → deep-below-terrain camera gets `reachTerrain = nullopt` → `camFloorH` nullopt → no clamp at `:1642`.
- `camera_controller.cpp:1647` clamps against `targetPos.z` (player feet), not terrain-at-camera-XY.
- Partial existing mitigation acknowledged: pivot lift `camera_controller.cpp:1461-1496` raises the pivot for terrain clearance but is capped at 1.4m (`:1487`) and throttled, so it cannot cover large slope deltas.
- Minor contributor: even for real WMO walls, `raycastBoundingBoxes` filters aggressively — hits only counted within ~0.8–0.9m above/below origin Z (`wmo_renderer.cpp:3631-3632`) and only near-vertical faces (`MAX_WALKABLE_ABS_NORMAL_Z=0.20`, `:3630`) — so tall/angled WMO walls can also be missed. This is a secondary effect, not the headline.

### Suggested approach
Two independent fixes, matching the two symptoms:

1. **Broaden the auto-zoom pull-in ray (Symptom A).** In the collision block around `camera_controller.cpp:1533-1557`, after the WMO raycast, also:
   - Call `m2Renderer->raycastBoundingBoxes(pivot, camDir, currentDistance)` (guard on `m2Renderer && !externalFollow_`) and fold its hit into `rawLimit` via `std::min`.
   - Add a **terrain ray-march**: step along `camDir` from `pivot` out to `currentDistance` sampling `terrainManager->getHeightAt(p.x, p.y)`; the first step whose `p.z < terrainHeight` gives the terrain hit distance. (Or add a proper `TerrainManager::raycast`/`intersect` to `terrain_manager.cpp` beside `getHeightAt:2249` for a clean API.) Fold into `rawLimit`.
   - Keep the existing asymmetric smoothing (`:1550-1554`) so pull-in stays fast, recovery slow.

2. **Make the terrain floor clamp unconditional for the camera (Symptom B).** For the camera clamp at `camera_controller.cpp:1638-1647`, do not reuse the player-locomotion `selectReachableFloor(..., 0.5f)` "reachability/step-up" gate — that semantic is for walkable ground, not the camera. Instead unconditionally push the camera above the terrain (and WMO floor when applicable) at the camera's own XY, e.g. `smoothedCamPos.z = max(smoothedCamPos.z, terrainAtCamXY + MIN_FLOOR_CLEARANCE)` whenever `camTerrainH` is present, independent of how far below it the camera currently is. Preserve the interior-WMO special-casing at `:1589-1645`.

Unknowns / risks:
- Terrain ray-march step size vs cost — reuse the existing throttle/cache pattern (e.g. `pivotLift` cache at `:1461-1496`, floor caches at `:271-279`) to keep it off the per-frame hot path.
- Adding M2 pull-in may over-pull on decorative foliage; may need to restrict to collision-flagged/large M2 or reuse the same filters `M2Renderer::raycastBoundingBoxes` applies internally.
- Interior/tunnel WMO transitions are already delicately handled (`:1497-1500`, `:1604-1637`, `:1624-1637`) — verify the unconditional terrain clamp stays gated by `!cachedInsideWMO` / `!cachedInsideInteriorWMO` as it is today.

### Effort / risk
Medium · Medium. The terrain-raycast path is the bulk of the work; the clamp fix is small but touches subtle interior-WMO seam logic, so regression risk is moderate.

## Tasks / Subtasks
- [ ] Add an M2 pull-in raycast to the camera collision block (`camera_controller.cpp:~1536`), reusing `M2Renderer::raycastBoundingBoxes` (`m2_renderer.hpp:375`); fold into `rawLimit`.
- [ ] Add a terrain ray-march (or new `TerrainManager::raycast`) and fold its hit into `rawLimit`; throttle/cache like the existing pivot-lift/floor caches.
- [ ] Replace the camera floor clamp's `selectReachableFloor(..., 0.5f)` gate (`camera_controller.cpp:1639-1640`) with an unconditional "camera above terrain-at-camera-XY + clearance" clamp; keep the interior-WMO guards.
- [ ] Verify feet-relative clamp (`:1647`) and pivot-lift (`:1461-1496`) still cooperate without double-lifting or jitter.
- [ ] (Optional) Widen/relax WMO raycast Z-band filters (`wmo_renderer.cpp:3630-3632`) if tall/angled WMO walls are still missed.
- [ ] Add tunables (constants) for terrain ray step size and camera floor clearance; document them near `CAM_SPHERE_RADIUS`/`CAM_EPSILON` (`camera_controller.hpp:225-226`).

## Testing
Against a live AzerothCore/TrinityCore 3.3.5a server:
1. **Terrain cliff:** stand at the base of a steep hill (e.g. Elwynn/Redridge), zoom out fully, back into the slope — camera must auto-pull-in, never clip into terrain.
2. **M2 doodad:** stand with a large tree/rock directly behind the camera; zoom out — camera must pull in.
3. **Slope under-clip:** stand on a ridge/cliff edge (e.g. Thousand Needles / Stranglethorn cliffs), orbit the camera around and below; confirm it never drops below the ground surface and you never see under the world.
4. **WMO regression:** inside/near a building (Goldshire Inn, Stormwind), confirm walls still pull the camera in and interior max-zoom still applies.
5. **Flat-ground smoothness:** on open flat terrain, confirm no new zoom jitter and normal zoom-out is preserved.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |

## Dev Agent Record
_(populated during implementation)_
