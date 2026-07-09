# Improvement stories — araxiaonline/WoWee

[BMAD](https://github.com/bmad-code-org/BMAD-METHOD)-style stories for bugs and features in our
WoWee fork. Each was **root-caused in the codebase** by an investigation agent with `file:line`
evidence and a suggested fix; status flows Draft → Investigated → Ready → In Progress → Review → Done.

## Strategic decision (read first)

**[SPIKE-001](SPIKE-001-engine-swap-feasibility.md) — verdict: polish the bespoke renderer in place; do NOT swap to a full engine.**
The backend (protocol + asset parsing + game logic, ~110k LOC) is cleanly separable, but the
renderer complaints below are *bounded, understood bugs* — not architectural failures. A full
engine swap would discard the cheap-to-fix renderer, then still force re-authoring every
WoW-specific rendering behavior + rebuilding the UI + fighting PBR lighting, and a proprietary
engine sabotages the preservation goal. Recommended now: **Phase 0 "seam hardening"** (small, low
risk) to firm up the backend↔renderer boundary — valuable regardless.

## Backlog

| Story | Title | Root cause (one line) | Conf | Effort |
|---|---|---|---|---|
| [001](001-resolution-settings-noop.md) | Resolution setting no-op | `applyResolution()` early-returns in fullscreen + macOS HiDPI clamp + never persisted | med | med |
| [002](002-creature-run-animation-only.md) | Creatures always run | spline move-callback hardcodes `anim::RUN` for every move; no return-to-idle | **high** | med |
| [003](003-animation-not-fluid-interpolation.md) | Choppy animation | distance-LOD throttles bone-matrix recompute (every 2/4/8th frame) while anim time advances every frame → pose snapping | med | **small** |
| [004](004-light-sources-flat-square-no-emission.md) | Lights flat, no emission | no light-source system — WMO(MOLT)/M2 lights never fed to shaders | med | **large** |
| [005](005-camera-collision-clipping.md) | Camera clips walls/ground | auto-zoom raycast tests only WMO walls (not terrain/M2); ground-clamp broken | **high** | med |
| [006](006-npcbot-party-members.md) | NPCBots vanish from UI / run when stopped | 6a: no roster persistence + parser silently truncates (bots at tail dropped first); 6b: **same as 002** | med / high | med / — |

## Suggested tackling order

1. **[003] Choppy animation — do first.** Small effort, huge visible payoff: every character and
   creature on screen gets smoother in one change.
2. **[002] Hardcoded run animation.** High-confidence, and it **also fixes 006-6b** (bots running in
   place) for free — one fix, two stories.
3. **[005] Camera collision.** High-confidence; the raycast infra (WMO/terrain you already parse)
   is mostly there — extend it to terrain/M2 and fix the ground clamp.
4. **[001] Resolution setting.** Self-contained; fixes a visibly-broken settings control + adds
   persistence.
5. **[006-6a] NPCBot roster resilience.** Client-side robustness (parse-into-temp, harden parser);
   independent of the animation fix.
6. **[004] Lighting system — biggest, do last / plan separately.** Real feature work (build the
   light-collection + shader path), not a bug fix.

**Phase 0 (optional, anytime):** the SPIKE-001 "seam hardening" — tidy the ~35 game→renderer call
sites behind the two injected pointers into a clean data-only boundary. Low risk, makes every
renderer fix easier, and keeps a future engine option open.

## Cross-cutting notes

- **002 + 006-6b are one fix** — the creature animation-state bug manifests on bots too.
- **003 + 002** both touch creature/character animation; do 003 first (rendering-side smoothing),
  then 002 (state selection) — they don't conflict.
- **005** reuses the same WMO/terrain collision data the player already collides against; check
  whether the *camera* uses any of it (investigation says it only tests WMO walls).
