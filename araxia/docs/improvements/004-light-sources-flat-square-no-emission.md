# Story 004: Light sources render as a flat square and emit no light

## Status
Investigated

## Story
**As a** player, **I want** in-world light sources (lamps, lanterns, torches, braziers, candles, magical glows) to appear as soft camera-facing glows AND to actually illuminate nearby surfaces, **so that** lit areas read correctly and the world matches retail WoW 3.3.5a lighting.

## Context / Observed Behavior
In-world light sources display as a flat (billboard-looking) square and cast/emit no actual light. Two distinct sub-problems:
- **(a) Visual proxy is a flat square.** The light's glow "card" renders as a hard-edged, often untextured/white quad instead of a soft additive glow.
- **(b) No illumination.** The light contributes nothing to scene lighting — surfaces near a lamp/torch are lit exactly as if the light did not exist.

## Expected Behavior
In real WoW 3.3.5a:
- M2 light-source doodads draw their glow via **billboarded, additively-blended glow cards** that face the camera and read as a soft halo — never a hard opaque square.
- WMO interior lights (MOLT chunk) and M2 light emitters (omni/point lights) contribute **local dynamic illumination** to nearby M2 and WMO surfaces (warm pools of light around torches, lamps, forges), attenuated by distance.

## Acceptance Criteria
1. Common light-source doodads (lamps, lanterns, torches, braziers, candles, generic glows) render as a soft camera-facing glow, not a hard/opaque square, regardless of whether the model name contains "lantern"/"lamp"/"light".
2. Glow cards are never routed onto the opaque/alpha-test pipeline; additive glow textures without an alpha channel or with a black color-key still blend additively.
3. WMO MOLT lights and M2 light emitters produce visible local illumination on nearby surfaces, attenuated to their `attenuationStart`/`attenuationEnd` (WMO) or emitter radius (M2).
4. Texture-load failure on a glow card must not leave a white 1×1-fallback square visible in the world.
5. No regression to sky/fog/ambient time-of-day lighting (LightingManager path) or to existing particle/ribbon effects.

## Investigation (Dev Notes)

### Where it lives
- **Environmental (time-of-day) lighting — NOT the culprit, but easily confused with it:** `src/rendering/lighting_manager.cpp`, `include/rendering/lighting_manager.hpp`. This drives sky/fog/ambient/sun from `Light.dbc`/`LightParams.dbc`. It is a global directional-sun + ambient system and has no concept of per-object point lights.
- **Per-frame lighting UBO (what the shaders actually get):** `include/rendering/vk_frame_data.hpp:12` `struct GPUPerFrameData` — fields are only `lightDir` (16), `lightColor` (17), `ambientColor` (18). One directional light + ambient. No point-light array.
- **Shaders confirm the same:** `assets/shaders/wmo.frag.glsl:3-13` and `assets/shaders/m2.frag.glsl:6-8` declare a `PerFrame` UBO with a single `lightDir`/`lightColor`/`ambientColor`; `m2.frag.glsl:118` computes `ldir = normalize(-lightDir.xyz)` — a single global light. No point-light loop, no attenuation.
- **WMO lights are parsed but never consumed:** `src/pipeline/wmo_loader.cpp:239-269` reads the `MOLT` chunk into `WMOLight` (`include/pipeline/wmo_loader.hpp:44-56`: `type`, `useAttenuation`, `color`, `position`, `intensity`, `attenuationStart/End`) and pushes into `model.lights` (`wmo_loader.cpp:267`). Grep shows `model.lights` / `WMOLight` are referenced **only** in the loader and the header — `src/rendering/wmo_renderer.cpp` never reads them. `wmo_renderer.cpp:1140` even notes "setLighting is now a no-op (lighting is in the per-frame UBO)."
- **M2 light emitters are never decoded:** `src/pipeline/m2_loader.cpp:814-815` reads `header.nLights` / `header.ofsLights` but there is no `M2Light` struct and no parse loop — the emitters are dropped on the floor.
- **The glow-card / fake-glow-sprite system (the "square"):**
  - Classifier: `src/rendering/m2_model_classifier.cpp:71-74` — `isLanternLike = name has "lantern" | "lamp" | "light"`, `isElvenLike = "elf"|"elven"|"quel"`. Batch-texture glow heuristics at `m2_model_classifier.cpp:242-289` (glow/flame/lantern-family token tables).
  - Per-batch hint computation: `src/rendering/m2_renderer.cpp:1556-1563` (`lanternGlowHint`, `glowCardLike`, `glowTint`) and glow center/size at `m2_renderer.cpp:1597-1622`.
  - Glow-sprite substitution + card-skip logic (opaque pass): `src/rendering/m2_renderer_render.cpp:1074-1118`; duplicated for the transparent pass at `1310-1331`.
  - `forceCutout` pipeline selection: `src/rendering/m2_renderer_render.cpp:1163-1187`.
  - Glow-sprite draw (soft radial texture, additive, but **screen sprite only**): `m2_renderer_render.cpp:1409-1441`; radial texture generated at `m2_renderer.cpp:769-807`; white 1×1 fallback at `m2_renderer.cpp:763-766` (assigned to failed batches at `m2_renderer.cpp:1384`).

### Root-cause hypothesis
One underlying root cause with two visible symptoms: **WoWee has no real light-source subsystem.** It never turns M2/WMO light emitters into scene illumination, and it substitutes a *name-gated heuristic* screen-space glow sprite for the glow card instead of rendering the card correctly.

- **(b) Emits no light — HIGH confidence.** There is literally no code path from WMO MOLT lights or M2 light emitters to any shader. The only lighting inputs the shaders receive are one directional sun + ambient (`vk_frame_data.hpp:12-18`; `wmo.frag.glsl:3-13`; `m2.frag.glsl:6-8`). Parsed WMO lights are dead data (`wmo_loader.cpp:267` is the only write, zero reads); M2 lights aren't even parsed (`m2_loader.cpp:814-815`).
- **(a) Flat square — MEDIUM confidence** (cannot run the Vulkan client to confirm the exact pixels). Most likely chain: a light-source model **fails the glow-sprite heuristic** (its name lacks "lantern"/"lamp"/"light"/"elf", or its glow-card texture name doesn't match the token tables at `m2_model_classifier.cpp:257-284`), so `shouldUseGlowSprite` is false (`m2_renderer_render.cpp:1080-1087`) and the card mesh is **not** skipped. That card then hits `forceCutout` (`m2_renderer_render.cpp:1163-1175`) via `(batch.blendMode >= 2 && !batch.hasAlpha)` or `batch.colorKeyBlack` — glow-card textures are commonly RGB-intensity with no alpha or black-keyed — forcing it onto `opaquePipeline_`/`alphaTestPipeline_` (`effectiveBlendMode = 1`, lines 1175/1178-1187). The result is a **hard-edged opaque quad = the square.** If the glow texture also failed to load, it is drawn with the white 1×1 fallback (`m2_renderer.cpp:763-766`, `1384`) → a white square. Non-billboarding of the card is a *secondary* contributor (fixed orientation makes the quad more obviously card-shaped) but does not by itself explain an opaque square, since a correctly-additive card would be edge-on invisible.

### Evidence
- Single-light UBO: `include/rendering/vk_frame_data.hpp:12-18`; shader mirrors `assets/shaders/wmo.frag.glsl:3-13`, `assets/shaders/m2.frag.glsl:6-8,118,154-160`.
- Dead WMO lights: parse at `src/pipeline/wmo_loader.cpp:239-269`; struct `include/pipeline/wmo_loader.hpp:44-56,208`; no reader in `wmo_renderer.cpp` (grep for `model.lights`/`WMOLight` returns only loader+header); `wmo_renderer.cpp:1140` "setLighting is now a no-op."
- Undecoded M2 lights: `src/pipeline/m2_loader.cpp:814-815` (header fields read, no parse).
- Name-gated glow heuristic: `src/rendering/m2_model_classifier.cpp:71-72`; token tables `242-289`.
- `shouldUseGlowSprite` gate depends on model being elven/lantern-like AND texture/blend hints: `src/rendering/m2_renderer_render.cpp:1080-1087` (and `1315-1321`). When false, the card is not skipped (`1088-1118` / `1322-1331`).
- `forceCutout` pushes no-alpha/color-keyed transparent batches to opaque/alpha-test: `src/rendering/m2_renderer_render.cpp:1163-1187`.
- White fallback square on tex-load failure: `src/rendering/m2_renderer.cpp:763-766` (create), `1384` (assign to failed batch).
- Glow sprite is a screen sprite only (additive billboard, radial texture) and does not illuminate anything: draw at `m2_renderer_render.cpp:1409-1441`, texture at `m2_renderer.cpp:769-807`.
- Built-in diagnostic that will confirm which gate a specific model falls through: `WOWEE_M2_GLOW_DIAG` logs per lantern-like batch `blend`, `matFlags`, `colorKey`, `hasAlpha`, `unlit`, `lanternHint`, `glowSize`, `tex` at `src/rendering/m2_renderer.cpp:1626-1637`.

### Suggested approach
Two independent workstreams; (a) is the cheaper visible win, (b) is the larger correctness fix.

**(a) Fix the square (glow-card rendering):**
1. Stop routing glow cards onto the opaque pipeline: in `forceCutout` (`m2_renderer_render.cpp:1163-1175`), exclude batches identified as glow cards (`batch.glowCardLike` / `lanternGlowHint`, or additive `blendMode>=3`) so a no-alpha/color-keyed glow card stays additive instead of becoming an opaque/alpha-test quad.
2. Broaden the glow-sprite substitution beyond the name gate: `isLanternLike`/`isElvenLike` (`m2_model_classifier.cpp:71-72`) miss most torches/braziers/candles (note `isTorch`, `isBrazierOrFire` already exist at `82`/`81` but aren't used to drive glow substitution). Drive `shouldUseGlowSprite` off the batch/texture glow signal (additive + glow token / color-key + unlit) rather than model-name family, and fold `isTorch`/`isBrazierOrFire` in.
3. Ensure a failed glow-card texture does not fall back to opaque white (`m2_renderer.cpp:1384`): treat glow-card batches with failed textures as zero-opacity (like the `groundDetail` guard at `m2_renderer.cpp:1577`) so nothing hard renders.
4. (Secondary) Add billboard-bone support to the M2 vertex path so any remaining glow cards face the camera; low priority once (1)-(3) land.

**(b) Add real light-source illumination (larger):**
1. Extend `GPUPerFrameData` (`vk_frame_data.hpp:12`) — or add a separate SSBO — with a small array of point lights `{vec4 posRadius, vec4 color}` plus a count.
2. Collect lights: decode M2 emitters in `m2_loader.cpp` (parse `ofsLights`) and forward the already-parsed WMO `model.lights` (`wmo_loader.cpp:267`) into a per-frame light list, transformed to world space, culled to the N nearest to the camera/player.
3. Accumulate in `m2.frag.glsl` and `wmo.frag.glsl`: loop the point lights, add `color * attenuation(dist, start/end)` to the lit term next to the existing directional contribution.

**Unknowns / risks:** exact M2 light record layout for 3.3.5a (`ofsLights`) must be confirmed against wowdev.wiki; light-count budget and per-frame culling need a perf pass (see `docs/perf_baseline.md`); interior vs. exterior light selection (WMO lights should mostly light interiors) needs the indoor flag already tracked by `LightingManager::setIndoors`.

### Effort / risk
- Part (a): **medium effort · medium risk** (touches hot per-batch render loop and pipeline selection; risk of regressing other transparent/cutout batches — gate changes tightly on glow-card flags).
- Part (b): **large effort · medium-high risk** (new GPU data path + shader changes + loader work + perf).
- Overall: **large · med/high.**

## Tasks / Subtasks
- [ ] Reproduce and identify a failing model: run with `WOWEE_M2_GLOW_DIAG=1` near a torch/brazier/lamp and capture `blend/colorKey/hasAlpha/lanternHint` for the square batch (`m2_renderer.cpp:1626`).
- [ ] (a1) Exclude glow-card batches from `forceCutout` so additive glow cards stay additive (`m2_renderer_render.cpp:1163-1175`).
- [ ] (a2) Drive `shouldUseGlowSprite` from batch/texture glow signals + `isTorch`/`isBrazierOrFire`, not just model name (`m2_renderer_render.cpp:1080-1087`, `1315-1321`; `m2_model_classifier.cpp:71-82`).
- [ ] (a3) Zero-opacity failed glow-card textures instead of white fallback (`m2_renderer.cpp:1384`, `1577`).
- [ ] (a4, optional) Billboard-bone support in `m2.vert.glsl` for remaining glow cards.
- [ ] (b1) Add point-light array + count to `GPUPerFrameData` / new SSBO (`vk_frame_data.hpp:12`).
- [ ] (b2) Forward parsed WMO `model.lights` into the per-frame collector (`wmo_loader.cpp:267`; consumer in `wmo_renderer.cpp`).
- [ ] (b3) Parse M2 light emitters from `ofsLights` (`m2_loader.cpp:814-815`) and add to the collector.
- [ ] (b4) Point-light culling (N nearest, interior/exterior aware via `LightingManager::setIndoors`).
- [ ] (b5) Add attenuated point-light accumulation to `m2.frag.glsl` and `wmo.frag.glsl`.
- [ ] Perf check vs. `docs/perf_baseline.md`; confirm no regression to sky/fog/ambient and particle/ribbon paths.

## Testing
- **Diagnostic (root-cause confirmation):** launch against a live 3.3.5a server, stand next to a lamp/torch/brazier with `WOWEE_M2_GLOW_DIAG=1`; confirm the square batch fails the glow heuristic and/or shows `colorKey=Y` or `hasAlpha=N` (which is what triggers `forceCutout`). This validates the (a) hypothesis before coding.
- **(a) Visual:** after the fix, orbit the camera around the same doodads — the glow must read as a soft, camera-facing halo from all angles with no hard opaque/white quad, and must not disappear or turn opaque at any blend mode.
- **(a) Regression:** verify unrelated transparent/cutout M2s (foliage, ghosts, spell effects) are unchanged.
- **(b) Illumination:** enter a torch-lit interior (e.g., a Stormwind/inn WMO or a cave with braziers); confirm nearby walls/floor (WMO) and props (M2) show a warm local pool of light that falls off with distance and disappears when the emitter is out of range. Compare against a reference retail/private-server screenshot of the same location.
- **(b) Regression:** confirm outdoor day/night sky, fog, and sun shading (LightingManager) are unaffected.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent team |

## Dev Agent Record
_(populated during implementation)_
