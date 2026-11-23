# Round 1 disposition

- Reviewed target: `design/vortex-io-redesign.md`, version 0.2.
- Round-one target SHA256: `736312926160e9405763c7c7d355199ed8292a30550b17c6410fe93b4d8012cc`.
- Revised target: version 0.3; SHA256
  `9f74ece850b238ec38016e34143ae4c9c958e1e827aa40bd3829e106a715ecc6` before the final P4 register
  entry; recompute after this revision for the round-two snapshot.
- The six reports total 1 BLOCKING, 11 MAJOR, and 0 MINOR. The sole BLOCKING item is resolved by
  adding a concrete Proposal Register. Round-one reports remain the historical record of version
  0.2.

## Writer

- Missing Proposal Register (reported also by `vortex` and `codebase`): **Accepted.** Added P1–P4
  with initial exact source-file scopes, caller/Vortex justification, costs to measure, and fallback
  if declined. These entries are proposals only and authorize no code edits.
- Missing Open Questions table: **Accepted.** Added U-1–U-5 with interim defaults, prohibited
  assumptions, and named evidence gates. Deferred ABI, lifecycle, and journal choices remain
  genuinely open, with concrete safe behavior while unresolved.

## Vortex & Spool-Mesh Expert

- Pending execution lacks a scoped core proposal: **Accepted.** P1 names `VortexCore` result, VM
  parked continuation, and `VortexHost` owner-pump boundaries, their rationale and costs, and the
  no-async-await fallback. P4 covers the shared language migration sites.

## Codebase Expert

- Existing runtime changes have no Proposal Register: **Accepted.** Resolved by P1–P4; P2 covers
  typed transient payload resolution and arena lifetime integration, and P3 covers
  capture/permascroll/store paths. The design still requires each acceptance gate to confirm and
  narrow exact implementation scope before code changes.
- Dense-slot ABA and absent general lease hooks: **Accepted as an open implementation gate, not as a
  settled mechanism.** U-2 now prohibits `contains(ref)` or arena generation alone as identity
  validation and requires a release/reuse plus exception-path proof. Any required arena change must
  be registered before implementation.
- Capture journal and durable ordering: **Accepted as an open implementation gate.** U-3 requires
  injected failures at each durable step, restart reconciliation, mapping reuse, permanent-span
  validity, and orphan-byte handling. P3 names likely existing storage/promotion files and the
  explicit no-capture fallback.

## System Architect

- Logical cell state versus native service bookkeeping is unclear: **Accepted.** §6.2 now makes
  cells/Vortex authoritative for inspectable identities, statuses, outcomes, progress, and
  continuation identity; opaque OS state is native. Native indexes must be rebuildable or
  incarnation-validated, bounded, and reclaimable.
- Owner-side planning and capture work may be unbounded: **Accepted.** §§8.3 and 9.2 require
  per-turn work budgets for validation/freezing/overlap traversal and a resumable preparation job or
  explicit refusal at exhaustion.
- Resource budgets and configuration delivery are underspecified: **Accepted.** §8.3 adds a
  charge-class inventory, units, owner, budget kind, immutable host/service snapshot, and a
  system-setting gate for user-adjustable classes. Numeric limits remain open to measured
  implementation evidence.

## Programming Language Design & Ergonomics Expert

- Shared effect legality and evaluation multiplicity are missing: **Accepted.** §4 now defines
  submit, await, and render/display across languages; guards, predicates/filters, and read-only
  answers are pure. Each bridge must state static placement and multiplicity and provide diagnostics
  where supported.
- Terminal outcomes lack a program-level rule: **Accepted.** §4 now requires a typed Outcome cell
  for terminal states, treats EOF as a distinct normal terminal observation, preserves partial
  payload/progress with status, and forbids implicit conversion into logical failure. Frontends
  specify control-model recovery.

## Xanadu / ZigZag Purist

- Occurrence ranks lack concrete reciprocal topology: **Accepted.** §6.2 now gives a two-request
  shared endpoint/capability topology using per-request occurrences and posward clone views, and
  makes the transient spike prove reciprocal links and clone placement. Exact dimension names and
  resolver ABI remain U-1.

## Proposal Register and owner decisions

P1–P4 are reviewable proposals, not approved source changes. Their final scope and costs must be
validated against implementation evidence. Per the workflow, present these proposals for owner
approval after convergence; no runtime or on-disk source files were changed in this round.
