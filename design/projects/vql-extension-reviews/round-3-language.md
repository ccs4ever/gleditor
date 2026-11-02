# Round 3: language and ergonomics critique

Read the shared R1/R2 ledger, all four peer R3 reports, current conformance/semantic boundaries and
the full workflow improvement set. **Not quiescent:** the empty-stream repair missed macro
composition. No additional distinct material workflow-contract omission was found this round.

## L6 — P1: a macro resurrects an empty inner stream

Current-binary headless reproduction:

```vql
def f($x) := "unexpected";
##/(/+d.missing/f())
```

Observed: `unexpected`, exit 0. The equivalent ordinary path correctly returns empty after L1.
`MacroDimensionGroup`'s `runOnce` passed an empty intermediate stream into later `evaluateStep`
calls; UDF/lambda bodies and root selectors could consequently revive it. This is a residual L1
execution hole, not a newly proposed macro semantics.

Implemented the authorized narrow fix: stop the macro's inner step loop when its stream becomes
empty. This applies the same composition rule used by ordinary paths and bound functors. Added one
regression covering direct and nested macro groups, UDF and lambda bodies, attempted root
regrounding, and optional/zero-or-more/one-or-more wrappers. It asserts no downstream allocation or
subject link changes, and retains existing optional/zero-repeat identity behavior.

The earlier arity limits remain precise: excess UDF arguments and non-final variadics refuse;
missing/default/type-match policies are unchanged. Bare `f()` and explicit `./f()` retain the same
Context-anchor AST and initial invocation behavior. R1 scope restoration restores lexical state, not
arena edits performed before an error. L5 keeps deliberate missing-variable scalar rendering
separate from unknown-function refusal.

All proposed SQL/REST/shell-history/mail/IRC/fediverse records, immutable observations and durable
anchors, rejection/coverage lineage, async owner boundaries, explicit dispatch/admission,
reconciliation, private publication closure and interface acceptance remain materially covered.
Those contracts are accepted as proposals, not delivered external integrations.

Verification: `make -j$(nproc) xuzz_test vquery` succeeds; all **146 VQL tests from 12 suites** pass
headlessly. Clang-format19 dry-run passes both owned files. Engine and semantics tests are released
for the next shared-state review; no commit was made.
