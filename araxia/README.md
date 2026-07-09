# araxia/ — Araxia's WoWee fork workspace

This folder holds **our** project management and working notes for the
`araxiaonline/WoWee` fork, kept separate from upstream code so it's easy to
rebase/merge without conflicts.

## Layout

- **`docs/improvements/`** — [BMAD](https://github.com/bmad-code-org/BMAD-METHOD)-style
  stories for bugs and features. One story per issue, root-caused in the codebase,
  tracked from Draft → Investigated → Ready → In Progress → Review → Done.
  - `_TEMPLATE.md` — the story template.
  - `README.md` — the story index + suggested order.
  - `NNN-slug.md` — individual stories.

## Working conventions

- We follow upstream's `CONTRIBUTING.md` (C++20, `wowee::*` namespaces, conventional
  commits `feat:`/`fix:`/`refactor:`/`perf:`, `[[nodiscard]]`, one logical change per
  commit, compile clean, manual-test vs a 3.3.5a AzerothCore/ChromieCraft server).
- Plus our own rule: **break up god-object files** rather than adding to them.
- Stories are investigated first (grounded in real `path:line` evidence), then
  implemented as focused branches/PRs referencing the story ID.
