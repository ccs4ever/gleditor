# Round 7 disposition (orchestrator) — document is now version 3.5

ZigZag, AOP and ergonomics critics returned CONVERGED on v3.4. P1–P11 approved; P12–P14 pending.

- Writer MAJOR: §11.2 lead-in now says the design depends on P1–P11 only; P12–P14 pending, design
  carries their "Without it" cost. §11 intro wording fixed.
- Vortex MAJOR: a hard failure in a turn that runs genesis or Initialize is fatal: adapter stages
  ChildStopping{cause:"init"} and returns an executor error (actor stopped), no retry. $stats
  written after the undo.
- Minors: stale "Dewey path is unused" sentence fixed; dangling else binds to nearest unmatched;
  `for` after an action vs `for` loop stated.
