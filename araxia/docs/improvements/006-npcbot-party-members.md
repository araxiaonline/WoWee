# Story 006: NPCBot party members disappear from UI and keep running when stopped

## Status
Investigated

## Story
**As a** player using the NPCBots AzerothCore module, **I want** my bot party members to stay visibly and stably in the party/unit-frame UI and to stand idle when they (and I) stop moving, **so that** the party frames are a reliable readout of my group and the bots don't sprint in place while standing still.

## Context / Observed Behavior
Running the trickerer/liyunfan1223 **NPCBots** server module (server-side creatures that join the player's group and follow), two distinct symptoms appear:

- **6a — Bots randomly vanish from the party/unit-frame UI, then reappear.** The bots are still present in the world; only their party-frame rows blink out and come back. It reads as "random," not tied to a specific action.
- **6b — Bots keep playing the Run animation when the player stops.** While following, when the player halts the bots mostly continue the looping run animation instead of returning to idle. The user suspects this is the same root cause as Story 002 (creatures run-in-place / always-run). It is.

## Expected Behavior
- **6a:** Once a bot is in the group, its party-frame row stays present as long as it is in the group. A single missing/late `SMSG_PARTY_MEMBER_STATS`, a member going out of range, an entity despawn, or one transient roster packet must not blank the row. If a member's data is temporarily unavailable, the row should persist (greyed / "out of range" / stale) rather than disappear.
- **6b:** When a bot's movement spline completes and no new move arrives, it returns to Stand (id 0) within a frame or two and stays there — exactly the behavior demanded by Story 002 AC #3.

## Acceptance Criteria
1. A bot that is a group member remains rendered in the party frame across missing stat updates, out-of-range transitions, and entity despawn/respawn (no flicker from client-side handling).
2. The client does not drop trailing group members when parsing an `SMSG_GROUP_LIST` whose per-member layout diverges slightly (parser robustness / no silent truncation), and does not blank the whole roster on a single parse failure.
3. Bots (creature-GUID party members) are tracked identically to player members in the roster — no player/high-GUID assumption drops them. (Confirmed already true; guard against regression.)
4. When a following bot and the player stop, the bot returns to Stand within ~1–2 frames and does not run in place — same behavior/fix as Story 002.
5. 6b is explicitly dispositioned relative to Story 002 (same root cause vs. distinct follower stop-packet bug).

## Investigation (Dev Notes)

### Where it lives

**6a — party/group roster tracking + party unit-frame UI**
- `src/game/social_handler.cpp:1705-1740` — `handleGroupList` (the only builder of the roster). Line **1710** `partyData = GroupListData{};` clears the roster, then **1711** `if (!GroupListParser::parse(...)) return;` — a full replace; on parse failure the roster is left **empty**.
- `src/game/world_packets_world.cpp:360-448` — `GroupListParser::parse`. Member loop **407-425** reads `name/guid/online/subGroup/flags(/roles)` per member and **early-`break`s** on any short read (`:408 rem()==0`, `:411 rem()<8`, `:413 rem()<3`, `:419 rem()<1`), silently truncating the member list. WotLK roles byte + LFG block (`:374-390`) use size heuristics that can misalign.
- `include/game/group_defines.hpp:13-54` — `GroupMember` / `GroupListData`. `isEmpty()` is `memberCount == 0`; there is **no** membership persistence beyond the last packet.
- `src/ui/social_panel.cpp:45-69` — `renderPartyFrames`: `:51` early-returns when `!isInGroup()` (i.e. `memberCount==0`); `:53-69` shows a "waiting for roster" placeholder when `members` is empty.
- `src/ui/social_panel.cpp:400-540` — party (5-man) frame loop: iterates **every** `partyData.members` entry **unconditionally** (`:402`). A member row is never skipped for a missing entity, missing stats, offline, or out-of-range — it is only greyed/labeled (`(offline)`, `(dead)`, `OOR`). So a row disappearing ⇒ the member left `partyData.members`.
- Health/name fall back to the entity only for cosmetics (`:426-428`, `:487-499`); entity despawn does **not** remove the row.
- `src/game/social_handler.cpp:1785-1894` — `handlePartyMemberStats`. WotLK reads the member GUID as a **packed GUID** (`:1793-1794`); unknown GUIDs are skipped (`:1798-1802 if (!member) { packet.skipAll(); return; }`). This only affects a bot's **stats** (blank bar), never its roster row.
- `src/game/social_handler.cpp:495-505` (`SMSG_GROUP_DESTROYED`) and `:1742-1752` (`SMSG_GROUP_UNINVITE`) — the only other roster mutators; both are explicit disband/kick, not "flicker."
- Negative result: grep of the roster path for `IS_PLAYER` / high-GUID / GUID-type gating found **none** (`social_handler.cpp`, `group_defines.hpp`, `game_handler.cpp`). Bots (creature GUIDs) are treated exactly like players in the roster.

**6b — creature/bot locomotion animation**
- `src/core/entity_spawn_callback_handler.cpp:131-164` — `setCreatureMoveCallback`, the network-driven locomotion driver for all creatures (incl. bots). **`:158` hardcodes `anim::RUN`** for any spline with `duration>0`; **`:160` sets `_creatureWasMoving[guid]=true`**.
- `src/core/application.cpp:1789-1972` — the per-frame creature render/animation sync loop. `:1912` `entityIsMoving = entity->isActivelyMoving()`; `:1944-1968` edge-triggered `stateChanged` → selects `RUN/WALK/SWIM/...` when moving, **`STAND`** when idle. `:1799` early-`continue` when `canonDistSq > syncRadiusSq` (**320u**, `:1754`). A near-identical second loop for player-typed instances is at `:2008-2085`.
- `include/game/entity.hpp:187-231` — `updateMovement` runs interpolation then a **dead-reckoning overrun** window (`:206-221`); `isActivelyMoving()` (`:229-231`) is `isMoving_ && moveElapsed_ < moveDuration_` (false once the spline window ends — this is the signal the STAND reset keys on).
- `src/game/game_handler.cpp:542-571` — `updateEntityInterpolation` only advances `updateMovement` for the player, current target, and entities within `ENTITY_UPDATE_RADIUS` (**150u**, `include/game/protocol_constants.hpp:99`). Following bots are within 150u, so they are updated (not a factor here).
- `src/rendering/character_renderer.cpp:1811-1842` — the render-instance interpolation completes (`:1836-1838 inst.isMoving=false`) with **no animation reset**. `moveInstanceTo` (`:3168-3231`) only resets to `STAND` on an **explicit zero-distance stop** (`:3194-3202`), and otherwise (re)plays a move anim (`:3227-3230`).
- Cross-reference: `araxia/docs/improvements/002-creature-run-animation-only.md` — the canonical Story 002. 6b is its **secondary** ("run while standing still") symptom applied to bot creatures.

### Root-cause hypothesis

**6a — MED confidence on client being causal; HIGH confidence on the mechanism.** The client keeps **no** party membership beyond the last `SMSG_GROUP_LIST` and rebuilds the roster by full replace (`social_handler.cpp:1710-1711`); the UI then renders that list verbatim (`social_panel.cpp:402`). There is **no debounce, retention, or reconciliation**. Therefore *any* transient roster packet that omits a bot — or a single parse hiccup — makes the row vanish until the next list re-adds it. The "random… then comes back" intermittency most plausibly **originates server-side** (NPCBots re-issuing group rosters as bots are summoned/unsummoned/adjusted); the defensible **client** defect is the lack of resilience, plus a real fragility: the parser's early-`break` (`world_packets_world.cpp:408-424`) silently truncates the member list on any byte misalignment, and bots sit at the **tail** of the member list, so truncation drops **bots first**. The task's "assume members are players / high-GUID check drops bots" hypothesis is **ruled out** — no such gating exists in the roster path.
Secondary reading of "disappears from the unit-frame": if the user means the bot's **data** goes blank (health bar empty) rather than the row vanishing, that is the packed-GUID stats path (`social_handler.cpp:1793-1802`) dropping stats for a mis-decoded creature GUID — the row stays, the bars blank. Default interpretation is row-gone (roster).

**6b — resolved-by Story 002. MED-HIGH confidence it is the same root cause; NOT a distinct follower stop-packet bug.** Bots are ordinary creatures driven by the exact same path (`creatureMoveCallback` + the `application.cpp` sync loop); there is **no follower-specific animation or stop handling** anywhere. This is Story 002's *secondary* bug ("looping RUN whose only teardown to STAND is the edge-triggered, skippable per-frame sync"): 
- The renderer never self-heals — `character_renderer.cpp:1836-1838` ends interpolation with no STAND fallback (only the explicit zero-distance `moveInstanceTo` at `:3199-3201` resets).
- The only STAND reset is the edge-triggered `application.cpp:1944-1968` block, and its rising "moving" edge is **suppressed** because the callback pre-sets `_creatureWasMoving[guid]=true` (`entity_spawn_callback_handler.cpp:160`), so the falling edge is what must fire the STAND — which it can, once `isActivelyMoving()` cleanly goes false.
This also **disposes of the task's alternative hypothesis**: the STAND reset is **completion-driven** (`isActivelyMoving()` false), not stop-packet-driven, so a follower simply *not receiving an explicit `MSG_MOVE_STOP`* would not by itself cause run-in-place. The residual trigger for bots specifically is narrower than "always runs": a following bot keeps `isActivelyMoving()==true` because the NPCBots follow generator **streams short `SMSG_MONSTER_MOVE` splines** — each new `startMoveTo` resets `moveElapsed_=0` (`entity.hpp:146-184`) and the callback re-forces RUN (`:158`) — so if micro-adjust splines keep arriving after the player halts, the falling edge to STAND never fires. Story 002 fix #2 (a renderer-level return-to-idle when a looping locomotion anim's interpolation completes) fixes this directly for bots.

### Evidence
- **6a full replace / no persistence:** `social_handler.cpp:1710-1711` — clear-then-parse-or-return; there is no merge with the previous roster.
- **6a silent truncation:** `world_packets_world.cpp:408-424` — every field read is guarded by `if (rem() < N) break;` inside the member loop; a break keeps only the members read so far and still `return true` (`:429/447`). Bots, appended after the player, are the first casualties.
- **6a UI renders unconditionally:** `social_panel.cpp:402` `for (const auto& member : partyData.members)` — no `continue`/skip; disappearance is impossible unless the member is absent from the vector.
- **6a no player-GUID assumption (negative result):** no `IS_PLAYER`/high-GUID/GUID-type filter in `handleGroupList`, `GroupListParser::parse`, `GroupMember`, or `handlePartyMemberStats` membership.
- **6a stats vs roster:** `social_handler.cpp:1798-1802` skips unknown GUIDs → blanks stats only, never the row.
- **6b hardcoded RUN:** `entity_spawn_callback_handler.cpp:157-160`:
  ```cpp
  if (!gotState || (curAnimId != rendering::anim::DEATH && curAnimId != rendering::anim::RUN)) {
      cr->playAnimation(instanceId, rendering::anim::RUN, /*loop=*/true);
  }
  entitySpawner_.getCreatureWasMoving()[guid] = true;
  ```
- **6b no renderer self-heal:** `character_renderer.cpp:1833-1842` completes movement (`inst.isMoving=false`) with no `playAnimation(..., STAND)`; contrast the explicit-stop-only reset at `:3194-3202`.
- **6b reset is completion-driven, not stop-packet-driven:** `application.cpp:1912`/`1957-1966` select STAND from `!isMovingNow`, where `entityIsMoving = entity->isActivelyMoving()` (`entity.hpp:229-231`) — this is why a merely-absent `MSG_MOVE_STOP` isn't the cause.
- **6b same path as 002:** `araxia/docs/improvements/002-creature-run-animation-only.md:44-57` describes exactly this teardown gap; 6b is that gap manifesting on bot creatures.

### Suggested approach
- **6b — subsumed by Story 002.** Implement Story 002 fix #2 (renderer-level return-to-idle in `character_renderer.cpp` when a looping locomotion anim's interpolation completes and no new move is queued → STAND/SWIM_IDLE/FLY_IDLE, guarded to looping-locomotion ids and preserving the Death guard) and, ideally, fix #1 (plumb Walkmode) so bots the server walks stroll instead of sprint. No bot-specific animation code is needed. Verification for 6b should be added to Story 002's test matrix (a following bot after the player halts).
- **6a — add client resilience (independent of Story 002):**
  1. **Do not blank the roster on transient/failed parses.** In `handleGroupList`, parse into a local `GroupListData` and only swap into `partyData` on success (don't clear-then-bail); on `parse()==false`, keep the previous roster. (`social_handler.cpp:1710-1711`.)
  2. **Harden `GroupListParser`** so a short/misaligned read logs and preserves already-parsed members rather than silently truncating, and validate `memberCount` vs. members actually read (`world_packets_world.cpp:407-425`). Confirm the NPCBots `Group::SendUpdate` member layout matches the parser (creature-GUID members use the same `name/guid/online/subgroup/flags[/roles]` shape).
  3. **Optional debounce/retention:** if a member present in the last N rosters is missing from a new one while its world entity still exists nearby, retain the row (greyed) for a short grace window to absorb server roster churn.
  4. Keep the packed-GUID stats path correct for creature GUIDs (`social_handler.cpp:1793-1794`) so bot health bars don't blank (secondary reading of the symptom).

### Effort / risk
- **6a:** Medium · Medium. Localized to `handleGroupList` + `GroupListParser` (+ optional retention state). Risk: masking a legitimate leave/disband if retention is too aggressive — gate retention on "entity still present" and a short window.
- **6b:** Low incremental · Low — it is covered by Story 002's existing scope (fix #2 mandatory; fix #1 optional for walk-vs-run). Add one bot-follow regression case to 002's tests. Primary risk is Story 002's own (unverified Walkmode bit; not regressing swim/fly/emote/one-shot).

## Tasks / Subtasks
- [ ] **6b:** Mark as *resolved-by Story 002*; add a bot-follow-stop case to Story 002 fix #2 and its test matrix. Verify no separate follower stop-packet handling is needed (confirmed: reset is completion-driven).
- [ ] **6a:** Change `handleGroupList` to parse into a temporary and swap-on-success; retain previous roster on parse failure (`social_handler.cpp:1705-1740`).
- [ ] **6a:** Harden `GroupListParser::parse` against silent mid-loop truncation; log + preserve parsed members; reconcile `memberCount` with members read (`world_packets_world.cpp:407-425`).
- [ ] **6a:** Confirm NPCBots `SMSG_GROUP_LIST` member layout against the client parser for creature-GUID members (capture a live packet).
- [ ] **6a (optional):** Add short-grace retention for a member missing from one roster while its world entity is still present.
- [ ] **6a:** Regression-guard that no player/high-GUID assumption is introduced into the roster path.
- [ ] Verify packed-GUID stats matching keeps bot health bars populated (`social_handler.cpp:1785-1894`).

## Testing
Against a live AzerothCore 3.3.5a server with the NPCBots module and 1–4 bots grouped:
1. **6a stability:** Stand grouped with bots and idle for several minutes; watch the party frames. With a client-side `SMSG_GROUP_LIST` log, correlate any row blink with an incoming roster packet — confirm the client no longer blanks the roster on a transient/omitting packet or a parse hiccup.
2. **6a truncation:** Add bots up to a full party; confirm all bot rows persist and none are dropped from the tail. Inspect the parser log for early-`break` warnings.
3. **6a out-of-range:** Send a bot far away (or let it fall behind), triggering out-of-range `SMSG_PARTY_MEMBER_STATS`; confirm the row stays (greyed/OOR), does not vanish, and its bars repopulate on return.
4. **6a stats vs row:** Verify bot health bars stay populated (packed-GUID stats decode for creature GUIDs).
5. **6b:** Follow with a bot, run, then stop. Confirm the bot returns to Stand within ~1–2 frames and does not run in place. With an `SMSG_MONSTER_MOVE` log filtered to the bot GUID, note whether splines keep arriving after the player halts (stream → the residual trigger) or stop while the bot still runs (a genuine reset gap) — both are fixed by Story 002 fix #2.
6. **6b regression:** Confirm the return-to-idle fallback does not stomp bot combat/emote/one-shot animations, and Death is never overridden (shared with Story 002).

## Change Log
| Date | Change | Author |
|---|---|---|
| 2026-07-09 | Story created + investigated | agent |

## Dev Agent Record
_(populated during implementation)_
