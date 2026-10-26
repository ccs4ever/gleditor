# R3 final learner review

**CONVERGED** for learning and practicing the currently available frontend workflow.

Reviewed only `docs/vagent-user-guide.md`. Its SHA256 is
`6adf9a4e099fc1890f78b6d9080de7c7604b8eac53e257a2f1092c94b2eb8667`, matching the frozen R3
candidate.

## Learning and check workflow

The reading path now moves cleanly from the mental model to a first source unit, a one-handler
checkpoint, search and rollback, then beliefs, goals, waits, delegation, and the workshop. Every
source fence is captioned as checkable with a named file; its runtime story is labeled a prediction.
The guide gives the baseline expected CLI output and a clear stopping point: checks and intentional
rejections are observable today; there is no event-delivery or agent-stepping command to run. The
implemented-checker status row and known checking gaps also set expectations without requiring the
learner to consult another document.

I reused the R2 checker observations for the counter checkpoint, Bool rejection, nested rollback,
one-wait deadline edit, delegation, and workshop because those source units and exercises are
unchanged. The final guide hash confirms they are against this frozen snapshot. The exact
intentional Bool rejection was:

```text
/tmp/r2-switch-bad.vagent:7:16: TypeMismatch: a Bool condition must compare with '== true' or '== false'
```

The restored Switch source and all the accepted scratch units returned `checked` with exit 0. As
before, those results verify only frontend acceptance. I also reran the unchanged 3s wait edit and
workshop source under the guide's headless/XDG exports; both returned `checked` with exit 0.

## Last synthesis exercise

I traced both message orders in the workshop as requested. With fixed stock 40 and order count 3,
Handle asks the warehouse, delegates Fetch, then awaits the order-keyed delivered belief.

- If the delegated outcome arrives first, Handle resumes and suspends at
  `await delivered(order_id)`; receipt of the fact then lets it continue, increment `completed`, and
  stage Completed.
- If the fact arrives first, it is already present when the delegated outcome resumes Handle; the
  belief await matches it immediately, and the same completion steps follow.

Both traces predict `completed == 1` after the continuation. With units 2 and count 3,
`available < count` matches and `then fail` fails Handle before delegation. A timed-out question
with fallback 0 takes the same branch. The guide makes explicit that these schedules are paper
traces, not CLI observations.

The former workshop gap is resolved: Warehouse is explicitly described as a fixed test double that
ignores `item`, always returns its constructor's `units`, and neither tracks per-item balances nor
decrements stock. The question now has a clear teaching purpose without implying inventory behavior.
The guide also says the scanner still needs lowering/runtime helpers, without relying on unexplained
vicon terminology.

I found no remaining actionable learning gap in the guide's intended frontend-only scope. It does
not need an implemented executor to teach the specified examples because it consistently marks their
traces as predictions and provides useful source-checking exercises today.
