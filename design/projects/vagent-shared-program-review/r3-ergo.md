# Round 3 critique: PL & Ergonomics Expert (shared-program-and-upgrade)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.1 (`dropped_fields: Any` and undefined migration syntax): RESOLVED — The unparsed `Any` type
  has been completely eliminated. `event Upgrade` now contains strictly `from_version: Str`. Historical
  fields are accessed via the typed reflection function `dropped_field(name: Str) -> Str` (§4.3.1,
  §4.3.5, P17.5). Type-incompatible field changes are statically rejected by `vagent check --upgrade`
  without phantom "explicit migration rules" (§4.3.1).
- R2.2 (Non-native skills lack `on upgrade` lifecycle hook): RESOLVED — §4.3.3 and Proposal P17.5 now
  explicitly specify that non-native skills support an `on upgrade(from_version: Str)` lifecycle
  handler, enabling encapsulated migration of private skill fields along `d.vars` before agent-level
  `on Upgrade` executes.
- R2.3 (Aborted delegated intentions omit staging `cancel` to remote peers): RESOLVED — §4.2, §4.3.4,
  and §8.2 now explicitly mandate that whenever a delegated intention is aborted or replanned during
  an upgrade, the reconciler stages a `$cancel` message with the correlation ID to the delegate peer
  via `d.outbox`, preventing leaked mesh computation.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§4.3.5, l.388–390) Inline event Upgrade definition in user-facing example

Full title: User code example in §4.3.5 includes prelude `event Upgrade` schema definition

**Observation:** §4.3.5 includes `event Upgrade { from_version: Str }` directly preceding
`agent Worker`. Because `Upgrade` is part of the standard prelude loaded automatically by the compiler
(`prelude.cpp:40–55`), users do not and should not re-declare this event in their `.vagent` files.

**Recommendation:** A brief code comment (e.g. `// Built-in prelude definition: event Upgrade { ... }`)
or placing the event declaration under a "Prelude Definition" heading would clarify to newcomers that
only the `agent Worker` block is authored.

### 2. (§4.3.4, l.379–380) Active restorer guard for maintain goal re-evaluation

Full title: Deliberation restorer deduplication on maintain goal condition re-evaluation

**Observation:** Line 379 notes: "If `holds` is false, a restorer intention is adopted at once." In
`vagent` runtime deliberation (`vagent-agent-language.md` §6.2), adoption of a restorer intention
should be guarded by checking that an active restorer intention for that specific maintain goal is not
already present on `d.intention`.

**Recommendation:** Clarify in the implementation notes of `AgentExecutor` that standard deliberation
guards against duplicate restorer intentions apply when re-evaluating `holds` conditions post-upgrade.

______________________________________________________________________

VERDICT: CONVERGED
