# VAgent stakeholder needs for Vortex I/O (pre-review)

Target: `design/vortex-io-redesign.md` v0.9. This is a source-grounded frontend-needs note for the
draft editor, not a verdict on the frozen design. All syntax/API sketches below are proposed
pseudocode, not implemented or grammar commitments. VAgent owns effect placement, actor-turn
staging, intent multiplicity/identity, and actor-local continuation state. Shared Vortex owns only
generic cell-based requests, admission, suspension, outcomes, and payload leases. There are no
production users or production ABI constraints; prefer the clean contract over compatibility shims.

## Current VAgent contract and implementation seam

- The language has two relevant models: a plan effect call stages an outbox entry and suspends its
  intention, while an effect in a handler stages without waiting and later failure is delivered as
  `EffectFailed` (`design/vagent-agent-language.md:187-199,860-885`). The executor’s `effectcall`
  builds an outbox entry, saves the correlation/deadline, and parks the intention
  (`apps/common/xanadu/vagent/executor.cpp:1264-1328`). Handler effects are allowed in bounded
  positions; effects are excluded from conditions, methods, and answers
  (`design/vagent-agent-language.md:1123-1135`).
- Outbox entries live in the actor arena under the active rollback mark. Backtracking removes them;
  a failed turn performs no staged action (`design/vagent-agent-language.md:187-190`). Their IDs
  currently derive from sender, turn, and outbox position
  (`apps/common/xanadu/vagent/executor.cpp:339-395`). This identity is an intent/correlation
  identity, not evidence of exactly-once external completion.
- The adapter currently drains after the executor returns and invokes bound native effects inline
  with the mutable `WorkerContext`, then delivers `EffectDone`/`EffectFailed`
  (`apps/common/xanadu/vagent/adapter.cpp:406-408,662-710`). That is the seam to replace: workers
  cannot retain the arena/context or a `CellRef`; owner code must snapshot arguments and materialize
  typed result cells. The current adapter’s `dispatched` bit is not a durable proof that the
  external operation completed (`adapter.cpp:662-678`).
- `vagent run` compiles and seeds a local lineage, then starts a single-node `MeshActorRuntime` and
  pumps the VAgent adapter (`apps/vagent/main.cpp:109-160`); it is not presently a separate direct
  synchronous interpreter. The redesign should name host profiles explicitly: a local-runner profile
  may pump and resume a parked VAgent plan continuation; a mesh-actor callback profile remains one
  atomic turn and must reject direct suspending `Submit`/`Await`. Existing staged actor effects
  remain legal in the latter.
- VAgent already treats guards as a constrained pure condition context in checker entry points
  (`apps/common/xanadu/vagent/checker.cpp:2709-2731`); answers are read-only by language contract.
  I/O cannot be smuggled into guard/answer evaluation, handler selection, or rendering. A runtime
  profile guard is still needed where unchecked/dynamic code reaches a forbidden path.

## Requirements for the shared design and VAgent adapter

1. **Keep VAgent policy out of the generic VM.** VAgent syntax, checker rules, effects’
   bounded/suspension positions, and multiplicity belong in its frontend/compiler/adapter. The
   generic protocol can expose cell-valued `Submit`, `Await`, `Pending`, and outcomes; it must not
   know the `effect` keyword or inspect actor ASTs. This matches the draft boundary
   (`design/vortex-io-redesign.md:329-342`).
1. **Keep stage, submit, and await distinct.** In an actor turn, `stage` creates a reversible
   language intent. Only a successful commit makes it eligible for durable post-commit admission.
   The adapter/service then freezes/owns the exact output, records durable admission, and starts
   external work. A re-drain or checkpoint replay with the same stable intent identity
   observes/reconciles that attempt; it does not submit a second operation. A fresh new intent at
   another committed turn is new work. In a local runner that explicitly permits suspending plans,
   `submit` creates a fresh request at its declared evaluation point and `await` observes only that
   request; resume must not revisit the submit site.
1. **Define frozen bytes and buffering boundaries.** VAgent should expose `unbuffered`, `line`,
   `fixed`, and `tokenized` as library/adapter policies over bounded cell payloads, not shared core
   grammars. For staged actor output, the committed intent must name immutable content semantics:
   later mutation of a source cell cannot change the bytes dispatched. It may carry an immutable
   arena/cell snapshot or a bounded owner-created frozen lease; the native worker never reads a
   mutable cell. If freezing is deferred until post-commit dispatch, the contract must prove the
   committed payload is immutable and retained until snapshot completion. Charge staged bytes,
   frozen bytes, and leases against grants/budgets.
1. **Specify actor Flush/Close order and barriers.** Flush and Close must be explicit staged
   intents, ordered with writes on the same endpoint. A flush barrier identifies which preceding
   writes it covers; Close follows that barrier (or an explicit abort policy) and does not imply
   that already-sent bytes were undone. Retries, multiple outbox entries, and replay must preserve
   endpoint sequence and barrier identity. The actor turn itself never blocks waiting for
   flush/close; completion arrives as a later correlated event/turn.
1. **Make cancellation and close preserve partial state.** Cancellation, deadline, close, turn
   rollback, and crash must distinguish bytes still buffered and unsent from bytes admitted or
   possibly transferred. Outcomes report bytes/progress and uncertainty. Locally buffered-unsent
   bytes may be retained in actor state, explicitly discarded, or explicitly retried; admitted bytes
   cannot be represented as rolled back. Close failure leaves a typed outcome and enough state to
   reconcile. Crash after durable admission but before a conclusive receipt is `Unknown`, not
   automatic redispatch.
1. **Keep tokenized ingress incremental and nonblocking.** Token boundaries and grammar belong to
   VAgent library/stdlib or a dedicated frontend adapter. It must consume bounded byte/payload
   cells, carry parser/decoder state across chunks, preserve read-ahead and source-generation
   identity, and yield on bounded work/size limits rather than blocking an actor turn. Incomplete
   token, malformed input, overflow/gap, EOF, and partial bytes are typed data/outcomes, not
   implicit goal failure.
1. **Use stable intent and request identities without conflation.** Actor intent identity should
   include actor generation, committed turn and outbox ordinal as the draft proposes
   (`design/vortex-io-redesign.md:262-280`). It deterministically maps to one durable
   attempt/request identity; completion carries actor generation plus request/correlation and typed
   outcome, never a stale `CellRef` (`:282-287`). Local explicit requests get fresh identity per
   submit evaluation, while Await/re-entry over a parked continuation keeps its existing one.
1. **Retain grants and no-silent-retry semantics.** Admission checks exact authority/capacity and
   byte/request budgets before service work. Bounded journal/queue exhaustion refuses admission.
   Restore/replay checks durable proof before invoking actor code; absent/unreconciled proof refuses
   restore. Surface `Rejected`, backend `Failed`, `Partial`, `Cancelled` with uncertainty,
   `Overflow/Gap`, `Eof`, and `Unknown` as typed cell data. Do not convert them all into
   `EffectFailed` text or VAgent logical failure; let a handler/plan select its own policy.

## Proposed VAgent exposure sketches

### 1. Actor turn: stage write, flush barrier, then close

```text
# Proposed pseudocode; not VAgent grammar or an implemented API.
on FlushRequested{writer: w, bytes: b} {
  stage io.write(w, frozen(b), mode: line)
  stage io.flush(w, after: last_write(w))
}
on StopRequested{writer: w} {
  stage io.close(w, after: last_flush(w))
}
on IoCompleted{request: r, outcome: o} {
  match o {
    Complete{transferred: n} -> believe write_completed(r, n)
    Partial{transferred: n, uncertain: u} -> believe write_partial(r, n, u)
    Failed{kind: k, transferred: n} -> believe write_failed(r, k, n)
    Cancelled{transferred: n, uncertain: u} -> believe write_cancelled(r, n, u)
    Unknown -> believe reconcile_required(r)
    _ -> believe io_observed(r, o)
  }
}
```

The write/flush/close cells are committed-turn intents. Their frozen payload and per-endpoint
sequence survive later state changes. Flush is a barrier over prior writes, and Close either follows
a successful/known barrier or reports the configured abort/uncertain policy. A failed or rewound
turn stages nothing. Actor code never waits in the turn; a completion is a future event. The actual
VAgent handler syntax currently stages effects but does not wait
(`design/vagent-agent-language.md:192-199`).

### 2. Local runner: plan effect may suspend and recover from a typed result

```text
# Proposed pseudocode; local profile only; names/outcome records are illustrative.
plan save_chunk(writer: Cell, chunk: Cell) {
  let result := io.write(writer, frozen(chunk), mode: fixed(4096)) within 30s
    else WriteOutcome{status: Failed, cause: cause}
  match result {
    Complete{transferred: n} -> believe saved(n)
    Partial{transferred: n, uncertain: u} -> believe needs_reconcile(n, u)
    Cancelled{transferred: n, uncertain: u} -> believe cancelled(n, u)
    Unknown -> believe needs_reconcile(0, true)
    _ -> believe write_failed(result)
  }
}
```

The proposed local-runner profile may park the VAgent plan at its language suspension point, pump
the service without blocking the UI/evaluation owner, and later resume the same
intention/correlation. It must not serialize VM choicepoints/CellRefs into a worker or rerun
`io.write` on resume. The actor profile instead rejects direct suspending calls before admission and
accepts the corresponding post-commit staged handler effect only.

## Generic stage → submit → await → outcome trace

1. **Construct / stage:** VAgent builds only cell values on the owner. In an actor turn, `stage`
   appends a reversible outbox intent under the turn mark; no service request or external work
   exists yet. Guard/answer/render paths are pure.
1. **Commit / freeze:** If the turn succeeds, the adapter resolves the committed actor intent,
   validates grants/generation/order, freezes immutable byte ownership and charges bounded
   staged/frozen budgets. Failed turn or backtracked intent disappears with no admission. A
   local-runner submit freezes its evaluated bytes at the explicit submit site.
1. **Durable admission / submit:** Actor profile records a durable attempt before service work;
   durable intent identity maps to exactly one request. If the bounded admission record cannot be
   made, return refusal before work. Local profile returns an admitted request cell or typed
   refusal. Neither owner waits synchronously for backend I/O.
1. **Execute:** Service owns native endpoint handles and frozen payload leases off-thread, under
   request and byte grants. It publishes bounded progress. No worker receives `WorkerContext`,
   VAgent AST/thunk, arena pointer, or `CellRef`.
1. **Await / deliver:** A local VAgent continuation may suspend at an allowed plan statement.
   `Await` returns `Pending` or the retained outcome for the same request; resumption observes that
   request and retains the VAgent-local plan frame/correlation, not a serialized VM continuation.
   Actor profile receives completion as a later message/turn; it never parks a callback. Re-draining
   an intent observes/reconciles its durable attempt, never resubmits it.
1. **Outcome / recovery:** The owner materializes a typed Outcome cell with payload/progress/status;
   normal VAgent handlers or plan recovery choose what to believe, retry, cancel, or reconcile.
   `Eof` is not empty success; partial transfer and uncertainty survive. Cancellation is best effort
   and cannot roll back sent bytes. Buffer leftovers stay explicitly buffered/retained or are
   explicitly discarded.

## Dialogue with VLace stakeholder

VLace asks whether a durable post-commit admission record may own a buffered write before
`IoService::Submit`, or whether the actor intent must describe frozen bytes only at committed-turn
dispatch (`r0-needs-vlace.md`, “Additional VLace needs”). My answer: these are two boundaries and
the design should name both. The actor intent must freeze *which bytes/ordering* it means before
commit can expose it, so subsequent actor mutation cannot change its semantics. At post-commit,
adapter admission takes ownership of a bounded immutable snapshot/lease and durably records attempt
metadata before service work; that admission is the shared request boundary. The durable ledger need
not copy transient bytes, consistent with v0.9: it records metadata, while the payload lease remains
service-owned and a crash may produce `Unknown` (`design/vortex-io-redesign.md:265-280`). VAgent’s
reversible intent and VLace’s explicit `perform` remain different admission policies over the same
service. This exchange sharpened my requirement from “freeze at staging or dispatch” to “freeze
semantic bytes/order at staging; transfer lease and durable admission after commit.”

Reciprocal question for VLace: how should a pure immutable buffer value distinguish `flush`
(external transfer, new effect identity) from `close` (ordered endpoint lifecycle action) when a
lazy bind suffix is abandoned while the flush request has already been admitted? I suggest the
buffer remain inspectable with unsent bytes, while the request keeps its partial/unknown transferred
count; closing/aborting must be an explicit subsequent operation or typed close outcome.

No source/runtime changes are requested by this note.

## Wildcard edge case folded into the needs

The wildcard reviewer notes that VAgent's staged post-commit write and an explicit stream `flush`
must show distinct submission points, and that a request group cannot merge per-operation receipts
or suggest rollback. I agree; this adds a needed distinction to §4 above: a language `flush` may
mean “submit this frozen buffer”, whereas backend drain, filesystem durability (`fsync`-like), or
remote acknowledgement are separate guarantees, each named and capability-gated. An aggregate intent
may group sequencing but must retain each request identity/outcome. Prompt-visible output must be
specified relative to the service admission point, and a completion from an old actor generation
only reconciles/releases its original request, never updates the new actor. This does not change the
staged-versus-explicit-submit distinction; it makes the claimed flush guarantee precise.
