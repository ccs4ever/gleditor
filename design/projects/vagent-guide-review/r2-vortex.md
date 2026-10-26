# Vortex/runtime review — R2

Verdict: CONVERGED — no actionable runtime-contract or teaching errors found.

Verified the guide matches the frozen R2 input (SHA-256
`b2fa52e2874a657d1ce33c3e9031e05ce59e9991da0fc4ba6b84c6f1f638ed6a`) and reviewed it end to end
against the vagent v3.9 contract and Vortex/vicon grounding notes. The added examples resolve the
prior teaching questions without introducing runtime inaccuracies:

- The nested rollback trace follows statement marks: the speculative `1` is undone, the successful
  alternative leaves `3`, and failure of the enclosing block restores the preceding value `6`. The
  guide correctly distinguishes this from handler rollback and hard turn rollback.
- `OneWait` distinguishes Start handling, selecting the intention, suspension, matching a later
  Done, and resuming. The deadline variant is a predicted deadline wake; the text explicitly avoids
  promising wall-clock scheduling.
- The standalone delegation trace separates the delegate outcome from application-level belief
  delivery and correctly places the caller's continuation after a successful delegated outcome.
- The A/B provenance table correctly tracks source assertions, including fact survival after one
  source withdraws and removal after the final source forgets.
- The workshop's availability condition is an ordinary failing `if` body: sufficient stock skips it;
  insufficient stock or a zero timeout fallback fails Handle before delegation. Its fact/outcome
  order explanation correctly relies on the belief await's immediate existing-fact check and copied
  match state.

The updated captions and implementation-status text consistently distinguish source checking from
runtime execution. I found no concrete correction to recommend within this guide review.
