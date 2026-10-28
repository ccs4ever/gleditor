# I2A scoped-evaluation quality gate

Reviewed the applied I2A source at `b51a5574` before the centralized build. This is a read-only
quality review, not an acceptance pass. Build and focused test evidence are still pending. The
source and tests establish the scalar/control VM primitive only; structural lowering and full P19.8
integration remain later work.

## Findings

### M1: nested typed values can carry unexecuted pipelines

`enterScope()` rejects pipelines on its four data roots (`in[2]`, `in[3]`, `in[4]`, and `out[0]`),
but the recursive typed snapshot does not reject a pipeline on each field value. A `Float`, `Int`,
`Bool`, or `Str` leaf under the argument record can therefore carry `+d.spin` and be copied without
the transformation that normal VM input processing would run. The destination's `outcome`, `error`,
and nested `payload` fields are also read or replaced without checking every nested cell.

The narrow checks are in
[vortex_scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:250)
and recursive field reads are in
[scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/scoped_evaluation.cpp:321).
Reject `hasPipeline(value)` at each data node visited by `ScopeCodec::snapshot()` and cover nested
argument and destination leaves. Keep control references exempt: a callback opcode can have a normal
`d.spin` successor.

### M2: the scope lifecycle has no end-to-end string persistence test

The tests cover stable spans, pre-mark scratch spans, and storeless mixed quotations through direct
`ScopeCodec` snapshot/materialize calls. They do not produce a `Str` result inside a live scope,
transfer it through `finishScope()` after mark release, and promote the destination. That path joins
the source-space pin and scratch-floor behavior and is where a correct isolated codec result could
still be attached to the wrong content namespace or fail persistence.

Relevant direct tests are in
[vortex_scoped_evaluation_test.cpp](/data/git/gleditor/tests/xuzz/vortex_scoped_evaluation_test.cpp:252)
and publication occurs in
[vortex_scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:629).
Add one live callback test that returns a stable foreign span and one that returns inner-scope
scratch text, releases to the caller arena, then promotes and verifies the resulting text/source
identity.

### M3: nested scope runtime and state restoration are not covered

The VM supports nested `ScopedEvaluate`, but the current suite has no nested invocation test. It
also does not assert that caller oracle and Dewey paths survive choices/cuts through a completed or
refused scope. The code snapshots/restores these values in
[vortex_scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:260)
and
[vortex_scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:632),
but those paths currently have no regression evidence.

Add tests for nested success and nested refusal, each checking cursor successor, stack, choices,
marks, oracle path, and current Dewey path. Include a parent caller choice and verify it remains
backtrackable after the nested scope closes.

### M4: the VM trusts the caller to cap host limits

`ScopedEvaluationLimits::valid()` only rejects zero-valued members. `setScopedEvaluationLimits()`
accepts every positive `size_t`, so its safe use depends on the actor/host layer constraining the
grant to the actor's byte, cell, depth, and fuel budgets. The comments state this contract, but I2A
does not demonstrate its enforcement. See
[scoped_evaluation.hpp](/data/git/gleditor/apps/common/xanadu/vortex/scoped_evaluation.hpp:12) and
[vortex_vm.hpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp:170).

Before enabling scopes in an actor, test that configuration/grant resolution clamps or refuses
limits above the actor budget. The VM API may remain host-grant based if that boundary is explicit
and verified.

## Confirmed improvements in the applied source

- Callback graph cells are checked against scope data cells before execution in
  [vortex_scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_scoped_evaluation.cpp:109).
- A live scope refuses cross-cursor backtracking in
  [vortex_vm.cpp](/data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp:231), and scope-created
  choices are restricted to the active cursor and verified graph.
- Source namespaces are captured by exact `contentSpaceOf()` ID, and the source pin is checked again
  before reconstruction in
  [scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/scoped_evaluation.cpp:89) and
  [scoped_evaluation.cpp](/data/git/gleditor/apps/common/xanadu/vortex/scoped_evaluation.cpp:145).
- Scoped execution remains default-disabled until a nonzero host limit grant is installed. Artifact
  admission remains closed to the post-`Cut` scoped opcode.

## Evidence status

The new focused scope suite has not yet been run in this review. No pass is claimed until the
centralized build and focused tests complete against a recorded source hash. The full P19.8 gate
also remains open for I2B structural operations, compiler-generated scope descriptors, actor grant
integration, and the remaining approved P19 work.
