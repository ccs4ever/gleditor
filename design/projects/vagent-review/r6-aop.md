# Round 6 critique: agent-oriented programming / BDI semantics (role: aop)

Document: `design/vagent-agent-language.md` v3.3 (2060 lines). Read: §4.2, §5.3, §5.6, §6.1-6.7,
§8.6, §10, §11.2, §12.3, §12.4, §14, §15. Focused round: BLOCKING and MAJOR only.

## Status of round-5 BLOCKING/MAJOR findings

| R5  | Topic                                                                       | Status                                                                                             |
| --- | --------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| 1   | emergency-stop handler: `halt()` vs the in-flight drive; restorer in flight | PARTLY (restorer RESOLVED via `restorers(i)` and the back-off; preemption is only asserted, see 1) |
| 2   | back-off cleared by the percepts of its own predicate                       | RESOLVED (§6.3 "only time clears it"; §12.3 step 7 matches)                                        |
| 3   | §12.3 depends on P1, undocumented                                           | RESOLVED (P1 "Used by" lists every example; step 10 of §12.3 names it)                             |

## Findings

### 1. MAJOR - 5.3, 4.2 step 9, 12.3: the emergency stop's two mechanisms exist only in …

Full title: 1. MAJOR - 5.3, 4.2 step 9, 12.3: the emergency stop's two mechanisms exist only in
prose and a comment

Problem. §12.3 step 8 relies on two rules: "`halt` is declared by `Chassis` to preempt any Mobility
effect in flight" and "the drain performs cancels before new effects". Neither is specified where an
implementer looks.

- The host contract in §5.3 still says only: starts return at once, `cancel` is best effort and
  idempotent, and a shared device "is serialised or refused (`EffectFailed{cause: "busy"}`) by its
  binding". Nothing says a method may preempt. A conforming binding that serialises `halt()` behind
  `follow` (10 min) passes every rule in the document and the example's safety claim is false.
- `effect def halt() within 5s` carries no marker; the only statement is the comment
  `// halt preempts any Mobility effect in flight` in `mars:hardware`. `Chassis` is a native skill
  with no body (§5.3), so "declared by Chassis" has no place to be declared. The grammar (§8.5/8.9)
  has no `preempts` form, so the checker cannot know which methods a binding must treat specially,
  and a second binding author cannot tell.
- §4.2 step 9 orders the drain as "local spawns, then messages, then effects, then timer changes".
  It never says cancels precede new effects, and does not say where `cancel` staging (§6.4) sits in
  that list. Under the written order, `halt` (staged first, in the handler body) and the drops'
  `cancel`s are both "effects" and keep emit order, so `halt` reaches the device before
  `cancel(follow)`.

Evidence. §5.3 last paragraph; §4.2 step 9 text; §12.3 `capacity Mobility` block and step 8; §6.4
last paragraph ("stages the host binding's `cancel`").

Fix. (a) In §5.3 add: "A capacity method may be declared `effect def halt() within 5s preempts`; the
checker allows `preempts` only on effect methods. A binding receives a preempting call ahead of, and
cancelling, every call in flight on the same device, so a preempting call is never refused as busy."
Add `preempts` to the §8.9 grammar and drop the comment in favour of the keyword in §12.3. (b) In
§4.2 step 9 write the drain order as "local spawns, messages, effect cancels, effects, timer
changes". (c) Reword step 8's "declared by `Chassis`" to "declared `preempts` on `Mobility.halt`".

## MINOR (not blocking)

- §12.3 step 8: if `KeepCharged` has neither a restorer nor a back-off when the hazard arrives and
  its `holds` fails in that same turn, the settle loop adopts a restorer that stages `dock()`
  straight after `halt()`. One sentence ("a handler that wants no restorer to start drops the
  maintain goal, or the hazard turn's settle loop adopts one") or have the handler set the back-off.
- §6.2 / §12.3: `KeepCharged.holds` and `travel`'s `where` read `battery` of any source; a peer's
  `tell battery(100)` satisfies them. `from self` (§6.1) is the fix; carried from round 5.
- §15: say that the constant 30 s back-off retries a permanently broken dock forever.

Assessment. The agent model (goal kinds, single-minded commitment, plan failure with forward staged
effects, wait states, maintain goals with restorers and time-only back-off, keyed beliefs with
sources) is sound and internally consistent. The one remaining gap is that the document's safety
example claims a device-contract behaviour that no contract states. It is a local fix.

VERDICT: NOT CONVERGED
