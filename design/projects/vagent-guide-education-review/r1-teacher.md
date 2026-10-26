# Programming teacher review — R1

## Decision: NOTCONVERGED

The guide is unusually careful about separating what `check` proves from what a future executor is
predicted to do. The first edit checkpoint is useful, and the rollback and await chapters contain
good stepwise traces. I still see actionable teaching gaps for a programmer who knows ordinary
imperative programming but has no logic, BDI, or actor background.

## Actionable teaching gaps

### 1. Teach “expression succeeds or fails” with a tiny trace before introducing search

**Evidence:** The mental model says expressions can “produce a value, try another possibility, or
fail” (lines 1–5), but the first explicit treatment is “Conditions test success” (lines 292–298).
That chapter introduces the non-Boolean meaning of conditions, matching comparisons, and explicit
`else fail` together. The next chapter adds `&`, `|`, bounded results, and parentheses (lines
336–366). A reader familiar with `if` and `||` has several familiar-looking operators whose meanings
differ, but no small evaluation trace that separates value, success, and failure before the
alternative search example.

**Concrete fix:** Before the `Switch` example, add a three-row worked trace such as: `enabled`
evaluates successfully to `false`; `enabled == true` fails when false and succeeds with a matching
value when true; `if enabled == true then ...` skips its body on failure but itself succeeds with
`()`. Then show how `else fail` changes that last result. Ask the learner to predict the outcomes
for `enabled = false` before revealing the row values. This gives a stable model to use later for
`not`, alternatives, and `await` recovery.

### 2. Add a checkpoint for belief provenance and one for belief-vs-event waits

**Evidence:** The beliefs chapter introduces keyed replacement, multiple sources, queries,
`findall`, snapshots, and closed-world behavior in one stretch (lines 547–612). Its source table is
helpful, but there is no learner prediction or small edit/check task. The later waits chapter
explains the important difference between an already-present belief and a future-only event (lines
798–825), also without asking the learner to predict a concrete pre-arrival/post-arrival case. These
are central concepts likely to be confused with ordinary variables and queued messages.

**Concrete fix:** Add a short “predict, then check the source” exercise after each topic. For
provenance, give two sources asserting one keyed fact, then ask which facts remain after one source
withdraws and after self-forget. For waiting, ask whether `Ready` delivered before `Start` satisfies
each of the two plans in the existing example. Include an answer table immediately after a divider
or fold, identifying that source/arrival trace is a language prediction; `build/vagent check` only
checks the edited syntax. The guide already contains the needed examples and facts, so this can be a
small addition rather than another tutorial.

### 3. Make the workshop require one independent transfer before showing its resolution

**Evidence:** The workshop is the only named synthesis project (lines 1141–1226), and it combines
spawn, a question, fallback, delegation, a source-bearing belief, an await, and asynchronous outcome
ordering. But it presents the complete solution first. Its practice prompt then asks the learner to
trace the two delivery/fact orders, whose key behavior has already been narrated in lines 1206–1209.
There is no unsolved task that asks the learner to combine concepts and diagnose a changed
requirement.

**Concrete fix:** Add a brief independent extension before the workshop’s full trace: for example,
change the insufficient-stock path to report a typed `Shortage{order_id}` event while ensuring no
Fetch is delegated; ask the learner to state what happens on timeout fallback and list the state and
staged output after the continuation. Give acceptance criteria, a hint distinguishing ordinary plan
failure from a successful report path, and a sample answer/trace afterward. Label the trace as
predicted behavior and its `check` result as syntax-only. This tests synthesis without implying that
the unavailable runtime can execute it.

## Exercise attempt and limits

I used the existing `build/vagent` headlessly; no build or runtime command was needed. The
documented sample `tests/samples/vagent/mini_01.vagent` checked successfully. A small
positive-amount handler transfer checked successfully, and replacing `enabled == true` with a bare
Bool condition produced the expected `TypeMismatch` diagnostic. This confirms the guide’s first
syntax/diagnostic practice is verifiable today. It cannot confirm the predicted state changes,
source tracking, wake order, or message ordering because the CLI does not execute agents.

No runtime conclusion is drawn from those checks. The points above concern teachability and
independent practice, not the language contract or runtime correctness.
