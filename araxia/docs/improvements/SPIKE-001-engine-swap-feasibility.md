# Story SPIKE-001: Engine-swap feasibility — keep the WoW backend, replace the renderer?

## Status
Investigated

## Story
**As a** maintainer, **I want** a clear-eyed feasibility read on replacing WoWee's bespoke renderer with a mature engine while reusing the protocol/asset/game code, **so that** I can decide whether to swap or polish-in-place.

## Context

WoWee is (as far as anyone can tell) the only native, no-Rosetta, no-Wine client that plays WotLK 3.3.5a on Apple Silicon against AzerothCore / ChromieCraft, and it has been validated working by the owner. The mission is preservation: keep classic WoW playable on modern Macs, long-term, open-source, with **no proprietary runtime dependency** so the thing stays buildable and mirrorable (Internet-Archive-durable) for 10–15+ years.

The pain is not the network stack — it is the **engine polish**. The owner's own description is "feels like original EverQuest": choppy animation, no camera collision, broken/flat lights, and general presentation roughness. Those complaints all live in the renderer, not the protocol.

This matters because of what is valuable vs. what is engine-bound:

- **Valuable, hard-won, renderer-independent** — the WoW 3.3.5a **network protocol** (opcodes, packet handlers, SRP6a auth, RC4, movement/spells/inventory/quests/party, ~530 Lua API funcs), the **asset pipeline** (MPQ/StormLib, M2, WMO, ADT, BLP, DBC parsers), **Warden** anti-cheat via Unicorn x86 emulation, and **game logic/state**. This is years of work and it is the reason to keep the project alive.
- **Engine-bound, replaceable** — the bespoke Vulkan renderer, scene/camera, skeletal-anim-to-GPU, ImGui UI, and SDL2 windowing/input. This is the ~50k-LOC part the owner is unhappy with.

The tempting hypothesis: bolt the valuable backend onto a mature engine (Godot / Unreal / Unity / O3DE / Bevy) and inherit camera collision, shadows, upscaling, and tooling "for free." This spike stress-tests that hypothesis before any code is written. **It is a direction-setting spike, not a plan to build.**

## Key Questions

1. **Is the backend actually separable** from the renderer, or is Vulkan threaded through the protocol/asset/game code such that a swap means a rewrite?
2. **Which engine, if any,** satisfies the open-source / durable-preservation constraint AND has real native-C++ interop AND mature Apple-Silicon support?
3. **What does a general engine NOT give you for free** — how much WoW-specific rendering semantics must be re-implemented on any target?
4. **Has anyone actually shipped this** — a protocol-speaking WoW client on a general engine — and what happened?
5. **Does an engine swap serve or sabotage the preservation mission**, given that heavy engines are themselves a dependency to preserve?
6. **What is the honest recommendation** — swap or polish-in-place — and what first step is valuable regardless of the answer?

## Findings

### 1. Is the backend cleanly separable? — YES (this is the linchpin, and it is grounded in the code)

The backend separates unusually cleanly, and the code already did the hard decoupling work:

- **Parsers are 100% Vulkan-free.** `grep` for `vulkan`/`VkBuffer`/`VkImage`/`VkDevice` across all of `src/pipeline` and `include/pipeline` returns **zero hits**. `include/pipeline/m2_loader.hpp:5` includes only `<glm/glm.hpp>` (math, not a renderer) and emits `M2Model`/`M2Vertex`/`M2Bone`/`M2AnimationTrack` as `std::vector`-of-POD structs. Same for `wmo_loader.hpp`, `adt_loader.hpp`, `blp_loader.hpp`, `dbc_loader.hpp`, `terrain_mesh.hpp`. glm is trivially convertible to any engine's math. These ~4,500 LOC of true loaders port with near-zero change.
- **The parser → GPU boundary is a single, textbook seam.** `src/rendering/m2_renderer.cpp:1153` — `bool M2Renderer::loadModel(const pipeline::M2Model& model, uint32_t modelId)`. The producer (parser) emits plain data; **all** Vulkan lives on the consumer side of this one call. To retarget you reimplement this consumer per engine; the producer is untouched.
- **Network, auth, Warden are render-free and port verbatim.** No `rendering/`, `vulkan`, or `Vk[A-Z]` references anywhere in `src/network`+`include/network`, `src/auth`+`include/auth`, or `src/game/warden*`. Sizes: network ~1.3k, auth ~1.9k, Warden ~5.2k LOC (Unicorn x86 emulation).
- **Game state + DBC data are pure data.** `include/game/entity.hpp` carries **zero** GPU handles. The ~154 `wowee_*` DBC-backed data tables (~41k LOC) have zero Vulkan refs.
- **The ~530 Lua API funcs bind to GAME STATE, not the renderer** — hence reusable. `src/addons` (~8k LOC): 0 files reference `rendering/`; 9 reference `game`/`GameHandler`. (Caveat: WoW addons also *draw* frames/textures; that presentation side rebinds to the target engine's UI, but the API-to-game-state surface is portable.)
- **The total game→renderer entanglement is tiny:** ~35 call sites across 7 files, funneled through two injected pointers — `GameServices.renderer` (`include/game/game_services.hpp:16`, already DI, not a singleton) and `transport_manager`'s `WMORenderer*` (`include/game/transport_manager.hpp:135`) — for exactly three concerns: play-a-spell-visual (`src/game/spell_handler.cpp:164`, 8×), get-character-position (`src/game/game_handler_packets.cpp:1853`, 3×), and transport/platform geometry. None of these intrinsically needs a renderer; each is really "game logic wants a position, collision geometry, or to trigger an effect."
- **The decoupling pattern is half-built already.** `src/core/` (~12.4k LOC) is a composition root of `*_callback_handler.cpp` files binding game-layer `std::function` callbacks (e.g. `GameHandler::setCameraShakeCallback`, `game_handler.hpp:1062`) to Vulkan. `src/rendering/animation/` already defines `i_animator.hpp` / `i_anim_renderer.hpp` / `i_character_animator.hpp`.

**But the second half of the verdict is load-bearing and must travel with the first:** a swap still **rewrites roughly half the LOC**. Of ~210k total, ~110k ports clean (network + auth + pipeline + game + audio + math + addons); ~100k is rewritten or rebound — the Vulkan renderer (~50k, **discarded**), the ImGui UI (~37k, **5,291 `ImGui::` calls, rebuilt from scratch in the target's UI system, not ported**), and the `core/` wiring (~12.4k, rebound). None of that ~100k is protocol or game logic — but a reader who hears only "the backend is clean" underestimates the swap by ~100k LOC. Also note the animation **logic** (locomotion/combat/mount FSMs + pose math in `src/rendering/animation/`) is Vulkan-free and reusable, but is trapped under the `rendering` namespace, so extracting it is real (not free) work.

**Answer: the backend IS separable; a renderer swap still means rebuilding the entire presentation + composition layer.**

### 2. Candidate engines

Apply the project's own stated constraint **first** — open-source, mirrorable, no proprietary runtime, durable — and the field collapses before rendering quality is even weighed.

| Engine | Native / interop | License / preservation fit | Maturity (Apple Silicon) | Effort | Verdict |
|---|---|---|---|---|---|
| **Improve bespoke Vulkan** | Native C++, zero boundary | Perfect — zero proprietary runtime; source + a C++ compiler + Vulkan loader builds forever | Runs native on Apple Silicon today (MoltenVK), owner-validated | Bounded per-fix (weeks–months) | **Recommended default** |
| **Godot 4** | Real C++ via GDExtension/godot-cpp over a stable C ABI — backend links as a native lib, no marshalling tax | MIT — forkable/rebuildable forever | Native **Metal** backend since 4.4 ([PR #88199](https://github.com/godotengine/godot/pull/88199)), ≥ MoltenVK speed | Multi-person-year (~87k LOC rendering+UI rewrite + C++ live-parse plugins) | **Only credible swap** |
| **Unreal 5** | Strong C++ interop | Source-**available** under EULA (not OSI open), 5% royalty >$1M, Epic-controlled — cannot be mirrored/rebuilt from open source in 10 yr | Universal binary since 5.2, ongoing Metal RHI work | Multi-year (Turtle WoW: ~18 mo, funded team) | **TRAP** (best renderer, wrong license, the path prior art just died on) |
| **Unity** | C# main language; C++ reachable only across a P/Invoke boundary — no object reuse for the ~530 funcs | Proprietary closed runtime, seat-licensed, cannot mirror/rebuild | AS editor exists | High + permanent boundary tax | **TRAP** |
| **O3DE** | C++ | Apache-2.0 | macOS / Apple-Silicon support officially **experimental** per O3DE docs | High | **TRAP for now** (right license, wrong platform maturity) |
| **Bevy** | Rust — C++ only via C FFI, no object reuse; would mean rewriting parsers | MIT/Apache | Immature, ~3-month breaking releases, sparse docs | High + language rewrite | **TRAP** (language mismatch + immaturity) |

Net: the preservation filter **pre-eliminates Unreal and Unity** (proprietary/vendor-controlled) and **O3DE** (experimental on the one platform that must be rock-solid) and **Bevy** (Rust vs. a 100k-LOC C++ backend). Only **improve-in-place** and **Godot 4** survive as sane for a small team.

### 3. The asset / rendering glue cost — the WoW-specific 80% no engine does for free

Engines give you meshes, skinning, shadows, PBR, a scene graph, camera collision. They give you **none** of WoW's specific rendering semantics, all of which must be re-implemented on **any** target and thrown away if you later change engines:

- **Terrain (ADT):** chunks blend up to 4 texture layers via alpha/splat maps using WoW's specific single-pass WotLK blend formula — `finalColor = tex0*(1-Σα) + tex1*α1 + tex2*α2 + tex3*α3`. Unreal Landscape / Unity Terrain / Godot terrain will not ingest this; you author a custom splat material and feed the alpha maps yourself.
- **M2 materials:** per-batch WoW blend modes (opaque / alpha-key / alpha / additive / mod / mod2x), billboarding modes, two-UV combiners, animated `M2TextureTransform`, and color/alpha animation tracks — none map to a stock PBR material.
- **Particles / ribbons:** `M2ParticleEmitter` / `M2RibbonEmitter` are bespoke; Niagara / VFX Graph require you to translate emitter params, not import them.
- **WMO interiors:** baked vertex-color ambient + portal-based visibility culling, not the engine's dynamic GI.
- **Liquid + sky/fog:** animated liquid surfaces; DBC-driven (`Light.dbc`) per-zone atmosphere and fog.

Worse, **modern PBR/deferred pipelines actively mis-render WoW's flat-lit art.** WoW 3.3.5a is forward-rendered, vertex-lit, baked-ambient, hand-tuned additive. Dropping it into Unreal Lumen / modern Godot/Unity looks *wrong* unless you author **unlit / custom-lit** materials that replicate WoW's lighting — i.e., you opt **out** of the engine's headline feature to preserve the look. "Inherit the engine's lighting" is therefore partly illusory. WoWee's current renderer, whatever its faults, already owns a lighting model that matches WoW.

The measured ratio in-tree confirms where the cost sits: ~4,500 LOC of engine-neutral loaders vs. ~13k+ LOC of Vulkan-bound rendering *semantics* (`m2_renderer*` ~5.6k, `wmo_renderer` ~3.9k, `terrain_renderer` ~1.3k, `water_renderer` ~2.2k, plus lighting/sky/weather/particles). That ~13k is what you rebuild against a new API — **per engine.**

Prior art de-risks the *conversion step* but not the *runtime*: `wow.export` ([Kruithne](https://github.com/Kruithne/wow.export) / [Marlamin](https://github.com/Marlamin/wow.export)) does mature offline M2→glTF-with-armature, WMO, and ADT export; [Noggit3](https://github.com/wowdev/noggit3) is a live ADT/WMO/liquid renderer+editor (excellent terrain-splat reference). But these are **offline machinima/mapping tooling and editors, not live streaming clients** driven by network opcodes. Using `wow.export` as the client's asset *spine* is a trap: it breaks reading the user's own legally-obtained install, loses the durability property, bloats storage, and can't track server/patch assets — preservation mandates the harder **runtime live-parse** path, which is exactly what Godot's GDExtension makes viable and what C#/Rust boundaries make painful.

### 4. Prior art — has anyone done it?

**In ~20 years of open-source WoW work, no one has produced a maintained, polished, general-engine WoW client that plays against a real server.** The precedents are the most important evidence in this spike:

- **WowUnreal ([Clancey/WowUnreal](https://github.com/Clancey/WowUnreal))** — the decisive existence proof. A **live** UE5.7 client for 3.3.5a that connects to AzerothCore, macOS-first, C++, built by essentially **one developer** (~345 commits). Reuses StormLib/MPQ + ADT/M2/WMO/BLP/DBC parsers and converts to UE natives (BLP→UTexture2D, M2→USkeletalMesh with 150+ anim IDs + state machine, WMO→UStaticMesh, 4-layer terrain splat→UMaterial with 3-level LOD + async streaming), SRP6/ARC4, 121+ opcodes, movement/combat/casting, FrameXML/Lua UI. **Explicitly still missing:** mail/AH/trade, dungeons/raids, PvP, flying/transport, many spell visuals are placeholders, Windows untested. **Calibration: ~a dev-year on the most turnkey engine, still not feature-complete.** It proves the architecture works AND reveals the true scale.
- **Turtle WoW's Unreal 5 client (headline cautionary tale)** — announced July 2024 ([turtlecraft.gg/remastered](https://turtlecraft.gg/remastered), [forum thread](https://forum.turtle-wow.org/viewtopic.php?t=14454)), a Vanilla client reimplemented in UE5 by a **funded, multi-person team**, reached "Legacy graphics complete aside from weather" by early 2025 — then **permanently cancelled 19 Dec 2025** ([report](https://boosting-ground.com/wow-classic/news/turtle-wow-cancels-unreal-engine-client)) amid Blizzard legal exposure. Read precisely: a WoW-client engine-swap is **feasible, costs multiple person-years even when funded, and the risk that killed it (distributing a WoW client) attaches to *any* engine choice and to the bespoke renderer too.** It is not evidence the engine swap is technically doomed.
- **Glader / [HelloKitty](https://github.com/HelloKitty)'s Unity3D demo (2020)** — connected to a live 3.3.5 WotLK server and played alongside normal clients ([Unity forum](https://forum.unity.com/threads/world-of-warcraft-emulation-in-unity3d.914690)). Stalled as a tech demo. (Distinct from his "GladMMO," which is an original-content MMO, not a WoW client.)
- **Every other serious/playable/protocol-aware reimplementation uses a BESPOKE renderer:** [Wowser](https://github.com/wowserhq/wowser) (JS/WebGL2, 3.3.5a, POC, stalled), [idewave-cli](https://github.com/gtker/wow_messages) / gtker's Rust protocol libs (no renderer), WoWee itself.
- **Apparent counterexamples don't count:** [Reinisch/Warcraft-Arena-Unity](https://github.com/Reinisch/Warcraft-Arena-Unity) uses its own netcode, not the Blizzard protocol; [lisacvuk/godot-wow-srp](https://github.com/lisacvuk/godot-wow-srp) is only a Godot C++ SRP6a auth module (still, useful WoW+Godot prior art); Noggit / wow.export / WebWoWViewer are viewers/editors that speak no protocol.
- **The loudest signal of all:** the entire [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) / TrinityCore private-server ecosystem still runs **Blizzard's original binary client.** Engine-swapped reimplementation has no sustained precedent.

### 5. Preservation angle — bespoke-native vs. heavy-engine dependency

This is the decisive axis for *this* project, and it ranks the options unambiguously:

- **Bespoke native C++ + Vulkan/MoltenVK:** zero proprietary runtime; source + a C++ compiler + a Vulkan loader builds it indefinitely; fully archivable; no account, license server, or vendor. **Best possible durability.**
- **Godot (MIT):** the **only full engine** compatible with the mission — the engine source is itself archivable, and GDExtension lets the existing C++ loaders link in. Caveats: Godot churns hard (3→4 was a breaking rewrite), you still rewrite the entire asset-to-GPU path, and there is **no known live WoW-Godot client** to de-risk against.
- **Unreal:** source-available but EULA/royalty/Epic-account-gated, ~100GB+ to preserve and build — a proprietary dependency that may not build in 15 years.
- **Unity:** closed proprietary runtime, account/licensing, notorious version churn, and the 2023 runtime-fee episode proved acute vendor risk — **the worst fit** for 15-year preservation.
- **Bevy / O3DE:** preservation-OK on license, but Bevy forces a Rust rewrite of the pipeline and O3DE is experimental on Apple Silicon.

An under-discussed **middle path** deserves naming: swap only the low-level **rendering backend** to a permissive open-source rendering *library* — [bgfx](https://github.com/bkaradzic/bgfx) (BSD), [Google Filament](https://github.com/google/filament) (Apache, strong PBR), Diligent Engine (Apache), or sokol (zlib). These satisfy the preservation constraint exactly like Godot but as a small, C++-native, archivable library you link — **you keep your own scene/camera/asset model and replace only the GPU abstraction**, avoiding the scene/asset impedance mismatch and the UI rewrite. This best reconciles "the renderer needs polish / cross-platform is painful" with "zero proprietary runtime dependency."

**Bottom line for preservation:** an engine swap to Unity/Unreal directly **defeats the mission**. Godot is the only full engine that doesn't — and even it trades a young renderer you fully control for a mid-maturity one you partly control, plus a ~45%-of-LOC rewrite bill.

## Options (ranked)

| # | Option | What | Effort | Risk | Preservation | Verdict |
|---|---|---|---|---|---|---|
| **1** | **Polish the existing Vulkan renderer** | Fix the named gaps: camera collision (raycast vs. already-parsed WMO/terrain), animation interpolation/higher-rate skeletal sampling, lighting/`Light.dbc` wiring. Keep everything else. | Bounded, weeks–months **per fix** | **Low** — no interop, no vendor, no language boundary; owner-validated pipeline | **Perfect** (zero new dependency) | **Recommended default.** Best effort-to-value; the WoW-specific 80% already works and the lighting already matches WoW. The baseline any swap must beat. |
| **2** | **Backend-library swap (bgfx / Filament / Diligent)** | Replace only the low-level GPU abstraction; keep native C++ scene/camera/asset model and the whole backend. | Months–~1 yr **if** the render seam is clean | **Medium** — rewrite GPU abstraction, not the scene graph or UI | **Full** (permissive C++ lib, archivable) | **Strong hybrid** if cross-platform/maintenance pain (not polish) is the real driver. Keeps preservation; avoids the UI rewrite and scene mismatch. |
| **3** | **Full engine swap → Godot 4** | Link the C++ backend via GDExtension; rebuild ~87–100k LOC of rendering+UI as Godot importers/scenes; write C++ live-parse plugins. | **Multi-person-year** | **High** — no live WoW-Godot prior art; Godot churn; still must re-author all WoW rendering semantics AND fight PBR lighting | **Compatible** (MIT) — the only full engine that is | **Only defensible full-engine target,** and only if the team specifically wants Godot's editor/ecosystem — **not** as a shortcut to polish. |
| **4** | **Full engine swap → Unreal 5** (WowUnreal path) | Proven route; best renderer/tooling. | Multi-year (WowUnreal ~1 dev-yr, still incomplete) | **High** technical + **fatal** to mission | **Fails** — EULA/royalty/vendor-locked, non-mirrorable | **TRAP.** Best renderer, wrong license, the exact path Turtle WoW abandoned. Reject for a preservation project. |
| **5** | **Full engine swap → Unity / O3DE / Bevy** | — | High + language/platform tax | High | Unity fails durability; O3DE experimental on AS; Bevy = Rust rewrite | **Reject.** |

## Recommendation

**Do NOT swap to a full engine. Polish in place (Option 1), and if maintenance/cross-platform pain later dominates, consider a backend-library swap (Option 2) — never Unity/Unreal.**

The reasoning is threefold and honest that swapping is likely **not worth it**:

1. **The swap relieves the least-valuable code and pays full price for the privilege.** The renderer (~50k LOC) is the part the owner is unhappy with, but it is the *cheapest to fix and the least hard-won.* An engine swap discards it, then **still forces you to re-author every WoW-specific rendering behavior** (terrain splat, M2 blend/billboard/texture-transform materials, particles/ribbons, WMO portals, liquids, DBC lighting) on the new scene graph — because no general engine does any of that for free — **AND** rebuild ~37k LOC of UI from scratch **AND** fight the engine's PBR lighting to preserve WoW's flat-lit look. WowUnreal is the yardstick: ~a dev-year on the most turnkey engine and still missing whole subsystems.

2. **The owner's actual complaints are bounded, well-understood, missing-feature gaps — not architectural failures.** Camera collision is a raycast against geometry you already parse. Choppy animation is a temporal-resolution / interpolation fix. Broken lights are shader/`Light.dbc`-wiring bugs. Small teams *have* reached good-looking bespoke WoW renderers (Noggit Red, wow.export, WebWoWViewer). These are the lower-variance path to the owner's real goal — a playable, preservable client — and each one already has an investigated companion story (see `003-animation-not-fluid-interpolation.md`, `005-camera-collision-clipping.md`, `004-light-sources-flat-square-no-emission.md` in this directory).

3. **For a preservation mission, a proprietary engine is self-defeating and even Godot is a net loss here.** Unity/Unreal add a proprietary runtime that may not build in 15 years — the opposite of the mission. Godot is the only mission-compatible full engine, but it has no live-WoW prior art, weak large-world tooling, and still costs a multi-person-year ~45%-LOC rewrite. The one funded precedent (Turtle WoW) was killed by **legal** exposure that no engine choice mitigates.

**PHASED first step — valuable no matter what you decide (do this now):**

**Phase 0 — Harden and document the renderer-independent seam.** Regardless of swap-or-polish, the highest-leverage move is to make the boundary explicit and enforced:
- Replace `GameServices.renderer` (`game_services.hpp:16`) with two narrow interfaces: `IVisualEffectSink` (`playSpellVisual`) and `IWorldView` (`getCharacterPosition`).
- Replace `transport_manager`'s `WMORenderer*` (`transport_manager.hpp:135`) with an `IWorldGeometry` provider.
- Move the Vulkan-free animation FSMs + pose math out of the `rendering` namespace into a reusable module.
- This is **~35 call sites + one interface extraction — days-to-weeks, not months.**

Phase 0 is worth doing on its own merits: it kills the last renderer leaks in the game layer, makes the code cleaner today, and — crucially — makes **any** future decision (polish, bgfx/Filament backend swap, or a Godot experiment) dramatically cheaper and reversible. It protects the durable asset (protocol + parsers + Warden + game logic) from ever being entangled with a renderer choice again.

**Then:** proceed with the bounded renderer-polish stories (Option 1). Only if cross-platform maintenance — not polish — becomes the dominant pain should Option 2 (bgfx/Filament) be spiked. Treat a full Godot swap as a "we specifically want Godot's editor/ecosystem" decision, never as a shortcut to polish.

## Effort / Risk

- **Phase 0 (seam hardening):** small–medium · **low risk** · touches ~7 game files + `core/` wiring. **Recommended immediately.**
- **Option 1 (polish in place):** medium, bounded per-fix · **low risk** · cross-cuts only the renderer. Prior art proves achievable by small teams.
- **Option 2 (backend-library swap):** large (months–~1 yr) · **medium risk** · rewrites GPU abstraction only; preserves scene/UI/backend and durability.
- **Option 3 (Godot full swap):** very large (**multi-person-year**) · **high risk** · ~45% of LOC rewritten, no live-WoW-Godot prior art, must re-author all WoW rendering semantics + rebuild UI + fight PBR lighting. Only mission-compatible full engine.
- **Options 4–5 (Unreal/Unity/O3DE/Bevy):** very large · **high technical risk + fatal preservation/mission risk.** Reject.
- **Cross-cutting reality:** the risk that killed the only funded precedent (Turtle WoW) was **legal** — distributing a WoW client — and it attaches to every option, including the current bespoke renderer. Engine choice does not mitigate it.

## Change Log

| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Spike investigated | agent team |
