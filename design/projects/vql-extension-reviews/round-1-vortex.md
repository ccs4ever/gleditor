# Round 1 — Vortex/runtime critique

Read-only source/design inspection of the current working tree; no builds or executions. Findings
below have code-path repro recipes, not claims of experimentally observed failures. Purist ledger
P1/P2 excluded.

**I1 — High: exceptions leak UDF/lambda bindings into the persistent evaluator environment.**
`apps/common/xanadu/vql/vql_engine.cpp`, `callUserDef` and `executeLambda`, save `env_`/`functors_`
and restore them through an explicitly called lambda. They do not install a scope guard. Any
exception from parameter evaluation, a nested body, an unknown type, resource exhaustion or an
unsupported service bypasses restoration. Repro: bind global `$x` to an “original” cell, then
execute `def pure inner(): Int := "bad"; def pure outer($x) := inner(); outer(1)`. Catch the inner
`TypeContractError`; inspect `getVariable("x")`. The inner call restores its caller environment
(which contains outer's argument), then outer unwinds without restoring the global environment. A
subsequent successful query sees `$x=1`. Functor bindings have the same exposure. Fix:
exception-safe RAII restoration from immediately after the snapshots, covering parameter/default
evaluation as well as the body; retain current mismatch-filter behavior. Regression: failed nested
UDF and lambda preserve both ordinary bindings and preexisting functor definitions. This matters
before resumable host effects: a failed request must not change the next task's lexical context.

**I2 — Medium: accepted bare creation is not protected by compiled creation's single-use rule.**
`compiler.cpp::validatePath` accepts `AnchorKind::Create`. `compileAnchor` mints its result at
compile time and optionally emits Value; it emits no New. The added reuse check lives only in
`vortex_vm.cpp`'s append-mode New handler. Repro: compile `%"Alpha"`, run its entry cursor, then
spawn/run another cursor at the same entry. Both reach the same preallocated anchor identity
successfully; the second Value can overwrite edits made to that anchor. Direct evaluation of the
same bare creation mints another cell each time. This also occurs before the reuse failure for
`%"Alpha"/d.child%"Beta"`: rerunning can change the anchor before child New refuses.
`design/vql-conformance.md` describes creation executables as single-use and advises fresh
compilation, but no anchor guard enforces this. Fix now: preflight-refuse bare Create anchors until
a common per-executable consumption/frame rule covers all allocation forms; or implement such a rule
before any executable instruction runs. Add bare-anchor and bare-anchor-plus-child second-run tests,
checking content and topology remain unchanged on refusal. This is separate from the documented
unsupported manually-detached-output case.

**P1 — Medium proposal gap: resource-scoped capabilities need a resolved-target rule.** Workflow
§§3.2/4.1 bind account/resource/operation and canonical arguments, but do not define target changes
introduced by adapter behavior. An authorized REST GET can redirect to another origin; a
database/provider cursor can resolve a resource under a changed connection revision. A capability
check only at logical submission would accept the wrong effective target. Specify immutable
connection/adapter/policy revision in the dispatch manifest; recheck authority immediately before
actual I/O and on redirects/resource expansion, without forwarding credentials to an ungranted
target. Receipts should record permitted resolved-target evidence. Fake-adapter acceptance: allowed
request redirects outside scope and refuses before sending target credentials; revocation between
queueing and dispatch blocks I/O. This does not require a language keyword: C++ enforces
target/credential boundaries; Vortex owns workflow policy.
