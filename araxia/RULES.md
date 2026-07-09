# Working rules — araxiaonline/WoWee

The agreed rules James and Claude work under on this fork. These govern *how* we work;
code conventions themselves come from upstream `CONTRIBUTING.md` (see rule 8).

1. **Always work in a branch.** Never commit directly to `master`.
2. **Prove the bug first.** When we reasonably can, write a unit test that **fails**
   against the current code *before* changing it — the failing test is the proof the
   bug is real and that our fix addresses it.
3. **TDD on new code.** Write the test, then the code, for new behavior.
4. **Green before push.** All tests must pass before a branch is pushed.
5. **Document in code, not in docs.** Explain *how things work* in comments next to the
   code, as close to the code as possible — prefer that over separate documentation files.
6. **Real tests only.** A unit test must exercise something real and important. If we
   catch ourselves writing a test just to have a test, we skip it — and we decide that
   together (see rule 9).
7. **One story per branch/PR.** Keep changes focused on a single story as best we can.
8. **Upstream rules win.** If our rules conflict with the upstream project's rules
   (`CONTRIBUTING.md`, `.clang-tidy`, etc.), follow theirs.
9. **Rules break only deliberately, together.** Any rule may be broken *when it makes
   sense*, but Claude and James decide that jointly — never break a rule in a vacuum.
10. **Keep stories current.** Update the relevant story in `araxia/docs/improvements/`
    as we learn things (corrected root causes, new evidence) and record progress — so the
    story always reflects reality, not just the first guess.

---

**Rule 32 — Enjoy the little things.** _(Columbus's rules, [Zombieland](https://en.wikipedia.org/wiki/Zombieland), 2009.)_
Out of order, unbreakable. A native WoW client walking around your own server counts.

---

_Add new rules here as we agree them. Reference rules by number in commits/PRs/story notes._
