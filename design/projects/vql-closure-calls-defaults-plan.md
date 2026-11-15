# Explicit closure calls and lazy fixed-formal defaults

This is the next C2 implementation package after `979e5b77`. Independent Luna architecture, senior
and resource reviews accepted the design below. Implementation and independent final reviews
accepted this bounded package. Full-suite validation also exposed and repaired shared fuel-error
classification: exhausted global execution fuel remains a budget error, distinct from a local
runtime-object probe limit. §4.12 remains incomplete.

## Language contract

Support positional `$f(a, b)` calls to runtime value closures, multiple fixed lambda formals and
lazy whole-formal defaults. Each actual remains a distinct stream. Explicit calls inherit caller
context independently of positional argument zero. Existing `/$f` application remains contextual:
the candidate is hidden context and, if the closure has a formal, actual zero. A zero-formal closure
receives context only. Omitted suffix formals bind empty and run existing filters/defaults. Missing
required VQL formals do not introduce a new arity error.

Presence is independent of stream length: supplied empty is present and suppresses a default;
omitted is absent. Defaults execute in the callee after earlier completed parameter matches and
before binding the current formal. Captured outer aliases snapshot membership at closure creation,
including duplicates and bound-empty values. Earlier defaults cannot see later formals; defaults
inside a pattern retain the original pre-match lexical scope. Explicit-dot rebinding follows its
existing separate port and ordering rules.

Named closure arguments and variadics remain explicit unsupported cases in this package. Predicate
lambda restrictions remain separate. No execution path evaluates ASTs or falls back to a residual
interpreter.

The shared AST type is `RuntimeClosureInvocation`, referenced by `shared_ptr` from both ValueExpr
and the path selector variant where needed for ordinary bare-call syntax. It contains a callee
ValueExpr, `args` and `argNames`; the parser's initial callee is the sigiled variable. Preserve
ordinary bare calls as well as let/return/scalar-expression uses. Do not encode a runtime callee in
`FunctionInvocation.name`.

## Runtime protocol

Append `CheckClosureCall` to the opcode enum. The root coordinator approves this native operation to
validate a dynamic callee and excess arity before actual-argument effects:

- Inputs: callee stream/reference, `ClosureCallMode` Int64, raw explicit argument count Int64.
- Output: one ordinary reference register bound to the exactly-one validated closure handle.
- Modes: `Explicit` and `Contextual`. Contextual mode permits zero raw explicit arguments and
  derives zero or one effective actual from the runtime formal count. Explicit mode injects none.
- No token allocation, registry, AST, callback or additional per-cursor state. The reference map
  freezes callee identity against later source-stream rebinding and uses existing choice/cleanup
  behavior. It does not promise immutable descriptor contents against deliberate graph edits.

`ApplyClosure` receives `[checkedHandle, mode, hiddenContextStream, rawExplicitActualStreams...]`.
It reparses and revalidates the descriptor, mode, arity and frame-port mapping before entering the
existing transactional native frame. Compiler ordering supplies the source-language preflight
guarantee; arbitrary Vortex graph authors can invoke Apply directly, which still validates safety.

Use `VORTEX_CLOSURE_DESCRIPTOR/2` and remove the old descriptor reader. Keep `VORTEX_FRAME/1` and
the closure value version if their record shapes remain unchanged. The descriptor records minimum
and maximum formal counts. The C++ builder uses an optional minimum, defaulting to maximum for
generic exact-arity callers; VQL emits minimum zero to preserve parameter filtering.

The agreed builder fields are `captureStreamCount`, optional `minFormalCount`, `maxFormalCount` and
`frameInputs`, in addition to the existing frame descriptor. Parsed metadata has effective
minimum/maximum values and the two presence-cell references. The builder creates the canonical
presence cells; callers do not allocate them per application. Keep formal bounds distinct from the
raw supplied count. Update all emitter and generic VM test uses of the former applied-stream count
and Apply operand convention deliberately.

Preserve existing Capture/Applied enum ordinals and append Context/AppliedPresence sources. Context
and Applied map to Stream ports; AppliedPresence maps to a Cell port and indexes a formal. Any
supported generic Cell capture mapping retains its zero-or-one reference check. The descriptor owns
two distinct canonical Bool cells, true and false, used as shared presence references. Check typed
bits rather than display text. Include both tagged reference records in export relocation and
metadata budgets; allocate no presence cells per invocation.

Validate mode/count/index/port kinds, min/max, record bytes, source usage, fanout and reference
totals before reserves, copying or frame allocation. Keep configured closure/frame/work limits and
full-width references. Rejection preserves outputs and cursor/frame/choice state. Successful backing
allocations retain existing cumulative accounting.

## Compiler and acceptance

Add a distinct runtime invocation AST node with callee, ordered arguments and argument names. Walk
all children under existing query-wide cycle/depth/work limits. Explicit unsupported syntax must
still expose malformed children to bounded validation. Update parser, scope/capture analysis, syntax
inspectors and every relevant visitor. Dynamic calls stay excluded from read-only target proofs.
Reuse native frame compilation and the shared matcher for defaults, types and destructuring.

Verify zero/one/multiple fixed formals, both call modes, same callsite with different runtime
closure arities, earlier-formal/default capture scope, explicit dot, lazy default effects, supplied
empty, typed/destructured filtering, nested creation/recursion and return failures in called
helpers. Excess arity, invalid callee and invalid descriptor must refuse before actual effects;
provided arguments of valid calls still prepare before parameter matching. Test frozen callee
selection and descriptor revalidation after argument effects, limits/failure/retry and cleanup.

Require fresh-VM export/reload after AST/source-VM destruction, host, real CLI and
`mesh::Frontend::Vquery` source execution. Sealed Vortex artifact admission remains a separate
boundary. Freeze every source writer before `make -j$(nproc)`, use clang-format 19, run every
program headlessly with all three XDG directories isolated, and obtain independent final gates.

## Implemented boundaries

The parser and bounded AST visitors retain runtime calls as distinct syntax. Each actual is
evaluated once and normalized before the next actual. A candidate-predicate scope guard retains real
caller context through nested scalar calls; the implicit entry control unit permits a top-level call
followed by traversal without fabricating a semantic context. Explicit dot defaults execute before
dot is rebound, while later defaults see the new context.

The descriptor owns its presence Bool cells and exports their references. Revalidation after
argument effects rejects deliberately corrupted metadata before entering the frame. Called-helper
return failures clean frame/guard/arena marks and permit a subsequent query. The runtime-object
scalar probe now distinguishes its own finite inspection limit from global work-fuel exhaustion;
closure parsing, scalar reads and export propagate the latter as a budget error.

The generic VLace emitters use Explicit Apply mode and an empty hidden context, retaining exact
arity. Opcode classifiers, disassembly, export and frontend ceilings include the appended Check
operation. No performance improvement is claimed: preflight adds descriptor validation work and
snapshots existing reference/cursor state. Counts are checked before relevant copying/allocation;
presence cells add no per-invocation allocation. The portability test uses a functional route budget
of 100,000 and separately proves that one unit of fuel refuses.

## Validation

Full `make -j$(nproc)` and the repository clang-format-19 formatting gate passed. All 143 expanded
focused tests passed, including closure syntax/runtime/export, mesh and VLace scalar budget routes.
The final engine run executed 4,408 active tests across 437 suites: 4,342 passed, 38 failed and 28
skipped, with 12 disabled profiling/benchmark probes. Every failing name matches the committed
`979e5b77` baseline; there are no new failures. This is not a full-suite pass.

The library and slice runs retain their baseline failures: library 841/842 passed; slice 266/268
passed. Repository lint still reports the two unrelated UI typography violations at
`apps/common/ui/view/slice/acme_presenter.hpp:28` and
`apps/common/ui/view/slice/slice_view_presenter.hpp:46`. Changed Markdown passed mdformat and mdl.
Independent Luna architecture, senior/runtime and resource gates accepted the bounded
implementation; a further independent review accepted the fuel-classification correction.

Reproduction logs are `/tmp/vql-closure-calls-build.log`,
`/tmp/vql-closure-calls-focused.{log,xml}`, `/tmp/vql-closure-calls-full-final.{log,xml}`,
`/tmp/vql-closure-calls-{gleditor,zigzag}.{log,xml}`, `/tmp/vql-closure-calls-format.log`,
`/tmp/vql-closure-calls-lint.log`, `/tmp/vql-closure-calls-cli.log` and
`/tmp/vql-closure-calls-repl.log`. Every executable used headless drivers and isolated data, config
and cache directories. No build overlapped a source writer.
