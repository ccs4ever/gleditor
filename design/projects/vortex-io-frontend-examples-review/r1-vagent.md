# R1 VAgent review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

References: `design/vortex-io-implementation-plan.md` §§9–10; VAgent's effect contract in
`design/vagent-agent-language.md` §§3.7/6.6; current executor, adapter, and
`EffectDone`/`EffectFailed` tests.

## Findings

### MAJOR — “local runner” does not identify which VAgent execution profile owns the effect

The example calls itself a local-runner example, but uses a `capacity` effect method inside a
`plan`. In current VAgent, this is an actor intention effect: the executor stages an outbox entry
and parks the intention; the adapter later delivers a correlated `EffectDone` or `EffectFailed`. The
capacity must also be provided by an installed native skill (`executor.cpp:1279–1289`). The
implementation plan distinguishes `Interactive`, `ActorTurn`, and `DurableEffect`: actor turns are
atomic and cannot directly submit/await, while committed actor intents are journaled and dispatched
as durable effects (§9). It does not define a standalone local VAgent runner that executes this
source form under `Interactive` and later routes owner events.

Clarify the intended path. If this is an actor plan, call it that and show the actor/turn staging
boundary, with `ActorTurn` deliberation followed by `DurableEffect` dispatch. If it is a distinct
local interactive runner, specify how the plan effect maps to that profile and who owns its
continuation and completion delivery. As written, the final statement that an actor turn is refused
is compatible with neither a clearly identified local path nor a clearly identified actor path.

### MAJOR — `Unit` does not expose the full I/O outcome

The signature declares `write(...): Unit`, then the plan binds `receipt` and calls `decide receipt`.
That does not define a source value carrying the managed I/O outcome or demonstrate inspection of
its tag, progress, and uncertainty. The prose says the continuation inspects outcomes, but current
VAgent effect completion binds the returned payload cell as the call result; failure is routed to
the `else` ladder (`executor.cpp:1294–1318`, `adapter.cpp:689–707`; tests
`EffectDoneResumesTheParkedIntentionWithTheResult` and `EffectFailedRunsTheElseLadderWithTheCause`).
`EffectFailed` is therefore not currently an ordinary tagged result value available to `decide`.

The target language also has no union types and currently relies on a narrow lowering subset; the
example correctly labels `Bytes` and syntax as proposed, but does not name a proposed VAgent
representation/access path for `OutcomeCell`. Spell out whether `EffectDone` binds an outcome cell
and how a plan reads its status fields, or state that outcome inspection is host-side only until a
VAgent bridge/accessor is added. Preserve the operation-appropriate write variants from plan §3.2
(`Complete`, `Partial`, `Failed`, `Cancelled`, `TimedOut`, `Conflict`, `Unknown`), with
counts/uncertainty where meaningful; admission refusal remains separately inspectable. Do not imply
a plain `Unit` receipt can express that information.

### MAJOR — the host capability and native capacity binding are left unconnected

The shared fixture says the host supplies an immutable capability cell, while this example says the
host binds `WriterCap` and declares `capacity Writer`. Current VAgent's effect lookup is by the
installed native skill's capacity instance, not by a source-visible capability cell. The sample
omits `uses Writer`, a native skill implementation/installation, and any rule mapping `WriterCap` to
that capacity's I/O authority. Since the capability cell is the authorization boundary in the common
protocol, leaving this link implicit makes it unclear what code can invoke the writer and which
bounded rights are enforced.

Identify the proposed bridge: for example, show the native skill/provider declaration and say how
its host-issued capability constrains the effect method, or explicitly mark the source-to-capability
binding as an M7 host fixture detail not expressible in this sketch. Do not suggest that the source
constructs or widens authority.

### MINOR — durable admission should name the journal flush acknowledgement gate

The example correctly places intent dispatch after commit and says the durable attempt record
precedes IoService admission. Plan §9 is more specific: the dispatcher writes and flushes the
attempt record off-thread, and work may begin only after the owner receives the acknowledgement;
journal exhaustion or unavailability refuses before external work. Say “flush, then wait for owner
acknowledgement, then admit” explicitly. That keeps “durably written” from being read as an
unacknowledged enqueue and makes the refusal edge reviewable.

## Semantics confirmed

- The committed staging claim agrees with VAgent's effect semantics: the effect is an outbox entry,
  not work run inside the deliberation turn. Current executor and adapter code support the
  stage-then-deliver shape.
- The example correctly keeps mutable worker context away from I/O workers, treats cancellation as a
  request, and forbids automatic resend after an unknown external effect. These agree with plan §§1
  and 9.
- The example is candid that `Bytes`, the new syntax, and the runner behavior are proposed rather
  than current compiler features. It should retain that scaffolding label while making the profile,
  provider, and result interfaces explicit.

## Verdict

**Revise before calling the VAgent example complete.** The commit-before-effect ordering and
no-worker-context boundary are sound, but the local-versus-actor profile, capability/provider
binding, and typed outcome path need a concrete contract. The `Unit` result sketch currently cannot
substantiate the prose's claim that VAgent inspects the full outcome. No target or source files
changed.
