# R2 VAgent review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

References: `design/vortex-io-implementation-plan.md` §§3.2, 9–10; current VAgent effect handling in
`executor.cpp` and `adapter.cpp`; prior report `r1-vagent.md` against the preceding target hash.

## Findings

### MAJOR — the source sample still binds an outcome-shaped `receipt` from a `Unit` effect

The prose now gives a useful proposed event mapping: `EffectDone` carries the complete terminal
`OutcomeCell`, and `EffectFailed` carries a full rejected outcome. It also says the event field
mapping is companion-local scaffolding. But the source still declares `write(...): Unit`, binds the
effect expression to `receipt`, and calls `decide receipt`. A `Unit` result cannot demonstrate
inspection or preservation of that `OutcomeCell`, and the sketch never says whether `receipt` means
the effect's declared result, the later event's outcome, or a continuation-local binding produced by
a new event bridge.

Choose one source contract: make the effect result/outcome type express the managed cell and show
how the parked plan receives it, or keep `Unit` and show the later event/result handler as the only
outcome consumer, without implying that `receipt` is the I/O receipt. Until the field accessors
exist, explicitly mark tag inspection as host-side (or identify the proposed VAgent accessor and
pattern form). The end note acknowledges that outcome accessors are unresolved, but the example's
main text still claims that “a source or host result handler inspects the tag.”

### MAJOR — `EffectFailed` is not yet a lossless value in the existing VAgent continuation path

The proposed distinction between an admitted operation's `EffectDone{outcome}` and pre-admission
`EffectFailed{refusal}` is sensible and agrees with the common protocol's tagged outcomes. However,
current VAgent routes `EffectFailed` to the suspended statement's `else` ladder, while the adapter
currently represents its failure as a cause string. The executor stores the `EffectDone` event as
the awaited event and binds its payload cell on success (`executor.cpp:1294–1318`); the adapter
constructs a separate failure event with textual `cause` (`adapter.cpp:689–707`).

Therefore this sample specifies a real change to the existing continuation contract: it must
preserve a refused `OutcomeCell` as inspectable event data without turning it into the ordinary
failure/else path, and must expose terminal status plus progress and any permitted payload without
flattening it. The example says this is proposed adapter glue, which is honest scaffolding; keep
that caveat adjacent to the event contract and make clear that current VAgent cannot yet satisfy it.
In particular, do not describe the new event shape as something the existing
`EffectDone`/`EffectFailed` implementation already carries.

## Semantics confirmed

- **Profile separation is resolved.** The example now identifies the actor profile, distinguishes
  `ActorTurn` deliberation from post-commit `DurableEffect` dispatch, and correctly describes staged
  actor intents as the exception to direct submission/await restrictions.
- **Capability/provider completeness is materially improved.** It says the fixture installs a native
  `Writer` skill and binds it to a host-issued bounded `WriterCap`, consistent with current effect
  lookup requiring an installed skill for the capacity (`executor.cpp:1279–1289`). This is explicit
  proposed host wiring, not a claim that VAgent source constructs authority.
- **Committed staging and durable admission agree with the plan.** Failed or rewound turns stage no
  work; the attempt record is flushed and owner acknowledgement precedes service submission. Refusal
  on journal exhaustion or missing acknowledgement is before external work. These match plan §9.
- **The terminal write outcomes are complete for plan §3.2.** The listed admitted write outcomes
  include Complete, Partial, Failed, Cancelled, TimedOut, Conflict, and Unknown; the separate
  `Rejected` value covers pre-admission refusal. Full fields, including progress, optional payload,
  and uncertainty where applicable, are retained in the proposed cell.
- Unknown is not retried, cancellation is not proof of stopping, and close is gated on successful
  flush. These lifecycle claims agree with the shared protocol.

## Verdict

**Revise the source/result binding before calling the VAgent example complete.** R1's profile,
provider, and durable-acknowledgement concerns are addressed. The event mapping now explicitly
preserves full outcomes as proposed glue, but the `Unit`/`receipt` source still does not identify
how that event value reaches VAgent code, and current `EffectFailed` handling collapses into an else
cause. No target or source files changed.
