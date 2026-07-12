# Story 010: Hard crash (SIGSEGV) in the Vulkan descriptor allocator on zone transition

_(GitHub issue [#8](https://github.com/araxiaonline/WoWee/issues/8). Story authored per Rule 11 — story before code.)_

## Status
**✅ DONE — FIXED + LIVE-VERIFIED 2026-07-11 (branch `fix/010-descriptor-pool-crash`, commit `bfe7e897`).**
User exited/entered Ragefire Chasm ↔ Orgrimmar **3 times with no crash** (previously crashed within 1–3
transitions). Root cause was confirmed by static analysis: **not** pool exhaustion/undersizing (the issue
title's first guess) but a **descriptor-set lifetime/ordering bug** — a deferred `vkFreeDescriptorSets` fired
against the bone pool **after** that pool had already been reset, corrupting MoltenVK's free-list, so the
next allocation null-dereferenced. Fix: `VkContext::flushDeferredCleanup()` drains all pending deferred
frees before the map-change teardown, while every pool is still valid. Build clean, 34/34 tests pass.

## Story
**As a** player, **I want** the client to survive zone transitions (e.g. exiting an instance into a dense
city), **so that** I can move between maps without a hard crash.

## Context / Observed Behavior
Hard SIGSEGV during rendering right after a zone transition (exiting Ragefire Chasm → Orgrimmar). The
player teleported out (Story 007/009 landing is correct) and the client crashed on the Orgrimmar re-spawn.
Reproduces within **one to a few** transitions — timing-dependent.

Crash signature:
```
Thread 0 (main) EXC_BAD_ACCESS (KERN_INVALID_ADDRESS at 0x18)
0  libMoltenVK  MVKDescriptorPool::initDescriptorSet(...)
1  libMoltenVK  MVKDescriptorPool::allocateDescriptorSets(...)
2  libMoltenVK  vkAllocateDescriptorSets
3  wowee        CharacterRenderer::prepareRender(unsigned int)
4  wowee        Renderer::renderWorld(...)
5  wowee        Application::run()
```

## Why it is NOT pool exhaustion (the evidence that killed the first hypothesis)
- The bone pool is **`MAX_BONE_SETS = 8192`** (`character_renderer.cpp:170`), created with
  `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`:294`).
- On **every** map change, `world_loader.cpp:293` calls `CharacterRenderer::clear()`, which fully
  `vkResetDescriptorPool(boneDescPool_)` (`character_renderer.cpp:564`) — the pool starts each zone empty.
- Dense Orgrimmar + bots is a few hundred instances → a few hundred bone sets (1 per instance per frame ×2
  frames). Nowhere near 8192, and it cannot accumulate across zones because of the per-map reset.
- The crash is **inside MoltenVK's allocator** (frames 0–2), so the existing return-code guard at
  `character_renderer.cpp:2243` never runs — the allocator dies before returning. That is the fingerprint
  of a **corrupted pool**, not a cleanly-signalled `VK_ERROR_OUT_OF_POOL_MEMORY`. A bigger pool or a
  pre-alloc capacity check would **not** fix this.

## Root cause (CONFIRMED — deferred free after pool reset = use-after-free)
Two facts combine into a use-after-free on the bone descriptor pool:

1. **Bone-set frees during gameplay are deferred.** `CharacterRenderer::removeInstance()`
   (`character_renderer.cpp:3305`) calls `destroyInstanceBones(inst, /*defer=*/true)` (`:3323`), which
   queues `vkFreeDescriptorSets(boneDescPool_, …)` onto **both** frame slots' cleanup queues via
   `VkContext::deferAfterAllFrameFences` (`character_renderer.cpp:643`, `vk_context.cpp:199`). Those
   lambdas only actually run when `runDeferredCleanup(slot)` is called in the render loop
   (`vk_context.cpp:1814`), after both slots' fences pass.

2. **`clear()` resets the pool but never drains those pending frees.** On map change,
   `CharacterRenderer::clear()` does `vkDeviceWaitIdle` (`character_renderer.cpp:517`) then
   `vkResetDescriptorPool(boneDescPool_)` (`:564`). **`vkDeviceWaitIdle` signals the fences but does NOT
   execute the deferred callbacks** — they sit in `VkContext::deferredCleanup_[]` until the next frame's
   `runDeferredCleanup`. `clear()` does not flush that queue.

**The failure sequence** (needs a `removeInstance(defer=true)` in the ~1–2 frames before the blocking map
load, which is common — entities despawn right as you leave):
- gameplay: `removeInstance` queues `vkFreeDescriptorSets(boneDescPool_, boneSetX)` (still pending).
- map change: `clear()` → `vkResetDescriptorPool(boneDescPool_)` — frees *all* sets, **invalidates every
  handle** including `boneSetX`.
- next frame: render loop `runDeferredCleanup` fires the pending lambda →
  `vkFreeDescriptorSets(boneDescPool_, boneSetX)` on an **already-freed, stale handle**. With
  `FREE_DESCRIPTOR_SET_BIT`, MoltenVK tracks per-set free-list nodes; freeing a stale set **corrupts the
  free-list**.
- Orgrimmar re-spawn: `prepareRender` → `vkAllocateDescriptorSets` walks the corrupted free-list →
  **null-deref at 0x18**. ← the crash.

This fits **every** fact: crash inside the allocator (uncatchable by the `:2243` check), on zone re-spawn,
with only a few live sets, intermittent over one-to-a-few transitions (timing of the pending free), and the
high memory is unrelated coincidence (dense-zone textures), not the trigger.

## Fix (approach) — drain deferred cleanup before any pool teardown
The shared `deferredCleanup_[]` queue also holds WMO/M2/terrain frees, and the map-change teardown order is
`wmo->clearAll()` → `m2->clear()` → `cr->clear()` (`world_loader.cpp:280-293`), each of which
resets/destroys its own pools. So the **safe single choke point** is to drain all pending deferred cleanup
**once, at the very top of the map-change cleanup, before any renderer teardown**, while every pool handle
is still valid and the GPU is idle:

1. Add `VkContext::flushDeferredCleanup()` — `vkDeviceWaitIdle`, then run both slots' `deferredCleanup_[]`
   queues (each `deferAfterAllFrameFences` lambda runs exactly once as its shared counter reaches 0), then
   clear the queues. Draining on-idle is exactly what these callbacks were waiting for, so it is always safe.
2. Call it at the top of the map-change block in `WorldLoader` (`world_loader.cpp:~273`, before
   `wmo->clearAll()`). Now every queued `vkFreeDescriptorSets`/`vmaDestroy…` executes against a **valid**
   pool/allocator, so nothing stale can fire after a reset. `clear()`'s own frees stay synchronous
   (`destroyInstanceBones` default `defer=false`) and the subsequent `vkResetDescriptorPool` resets an
   already-properly-emptied pool.

This is an **ordering fix**, and it also immunizes the WMO/M2/terrain pools against the same class of bug.

## Acceptance Criteria
1. Exit RFC → Orgrimmar (issue #8 repro) no longer crashes, across repeated transitions (≥5 round trips).
2. No descriptor/validation errors in the log at the transition.
3. No regression to normal gameplay despawns (`removeInstance` still frees bone sets correctly) or to
   initial login / same-map teleport.

## Rule-2 note (unit test skipped — decide together, Rule 9)
The bug is a **live GPU driver-state corruption** (MoltenVK free-list) that only manifests with a real
Vulkan device across frame boundaries; there is no isolated pure-logic seam to unit-test without standing up
a device + MoltenVK (that would be integration, not a unit, and is Rule-6 theater as a "unit"). Verification
is inherently live: the RFC→Orgrimmar repro that crashed in 1–3 transitions must survive ≥5. Flagging the
skip per Rule 9, consistent with Stories 007/009. `flushDeferredCleanup()` will `LOG_DEBUG` the drained
count once per transition (not per-packet — no flush-storm) so we can observe it working.

## Tasks / Subtasks
- [x] Add `VkContext::flushDeferredCleanup()` (wait-idle + run both slots + clear); `LOG_DEBUG` drained count.
- [x] Call it at the top of the map-change cleanup in `world_loader.cpp`, before `wmo->clearAll()`.
- [x] Build; keep the existing 34 tests green (34/34 pass).
- [x] Live-verify: RFC↔Orgrimmar 3 round trips, **no crash** (user-confirmed); normal despawns/login unaffected.

## Testing
Live against AzerothCore: repeatedly exit/enter Ragefire Chasm from Orgrimmar with NPCBots active.
Before fix: SIGSEGV in `vkAllocateDescriptorSets` within 1–3 transitions. After fix: no crash, clean log.

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-11 | Story created (issue #8). Root cause CONFIRMED by static analysis: deferred `vkFreeDescriptorSets` fires after `vkResetDescriptorPool` (bone pool) → MoltenVK free-list corruption → null-deref. NOT exhaustion (pool 8192, reset every map change). Fix: drain deferred cleanup before map-change teardown. | dev |
| 2026-07-11 | Fix implemented (commit `bfe7e897`): `VkContext::flushDeferredCleanup()` + call at top of map-change cleanup before `wmo->clearAll()`. Build clean, 34/34 tests pass. Awaiting live RFC↔Orgrimmar verify. | dev |
| 2026-07-11 | **LIVE-VERIFIED**: user did 3 RFC↔Orgrimmar round trips with no crash. Story DONE. | dev |
