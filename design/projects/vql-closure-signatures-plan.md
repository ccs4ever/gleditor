# Runtime closure signatures and grouped variadic arguments

The baseline is `5e8cbbdd`. This combines remaining dynamic closure named-argument and variadic
signature work into one bounded implementation package. Implementation and execution gates are
complete; §4.12 remains incomplete. The initial research and independent Luna architecture, senior
and resource reviews identified the lifecycle/type/accounting requirements below. These are
mandatory implementation constraints, not optional performance improvements.

## Language and mapping rules

Runtime value closures accept named actuals and a final variadic formal. Keep the parser's strict
`RuntimeClosureInvocation.argNames.size() == args.size()` invariant; visit every child under
existing cycle/depth/work limits before rejecting malformed or unsupported syntax. Ordinary
`FunctionInvocation` has a different positional AST convention and is not tightened by this change.

Actuals evaluate once in source order, each normalized immediately. Parameters match and defaults
run in declaration order after all actual preparation. Named arguments address string-named formals
only; dot/context and destructure-only targets are not name-addressable. Unknown names, duplicate
labels, fixed positional/named collisions and positional arguments after a named argument refuse
before any actual effects. A named rest actual is allowed and contributes exactly one original group
at its source position; only one occurrence of that label is allowed. Earlier positional rest groups
remain distinct and ordered. Passing `$rest` as one argument forwards one group; no spread syntax or
recovery of an earlier call's groups is implied.

Presence follows binder assignment rather than stream length. Omitted fixed formals retain existing
empty/filter/default behavior; VQL emits minimum arity zero. One supplied empty rest group
suppresses its default. An omitted rest default runs once and is checked as one group. Each original
typed or destructured group matches before concatenation, using shared graph matching with captures
disabled, exactly as existing named rest does. Aggregate named/dot rest binds only after every group
passes. Per-group destructuring/type captures do not leak. Preserve pre-match lexical defaults.
Named reusable calls retain deferred required-alias checks according to actual group/default
selection. Closure construction validates and snapshots required, conditional-rest-type and
omitted-rest-default lexical aliases at creation, because later calls may choose any arity; a future
unused default does not defer lexical capture. Bound-empty is valid and distinct from a missing
required binding. Ordinary free variable reads keep their existing empty-stream semantics; this rule
concerns aliases identified as required by contract/default compilation, not every free name.

Explicit calls never inject context. Contextual `/$f` still permits zero raw explicit actuals. If
any fixed formal exists, context supplies formal zero and leaves the tail omitted; for a
variadic-only signature it supplies one present rest group, including an explicitly empty context
stream. Hidden context remains separate. All context rebinding/default ordering and caller
restoration rules remain.

## Descriptor and operand protocol

Use `VORTEX_CLOSURE_DESCRIPTOR/3`, `VORTEX_FRAME/2`, and unchanged `VORTEX_CLOSURE_VALUE/1` shape.
Remove old descriptor/frame readers. Keep existing Capture/Applied/Context/AppliedPresence ordinals;
append AppliedRest and AppliedRestGroups. FramePortKind appends Groups after Cell and Stream.

Keep `maxFormalCount` as declared formal-slot count, not the raw variadic actual ceiling. Existing
optional `minFormalCount` remains minimum supplied raw count, defaulting to declared count for
generic exact-arity callers; VQL sets zero. Add optional final `variadicFormalIndex` and optional
exact formal-name metadata. A populated name table has one `optional<string>` per formal; nullopt
means non-addressable. Empty names are invalid in that API. An absent table is unnamed/positional,
as generic VLace remains. Names are unique, byte-bounded text associated with formal indices, not
cell references requiring relocation. Rest sources map exactly once to the final formal: AppliedRest
to Stream, AppliedRestGroups to Groups. Fixed Applied may not target the variadic index. Presence
ports remain typed Cell Bool references owned by the descriptor.

CheckClosureCall inputs are `[callee, mode, rawCount, name0, ..., nameN-1]`, exactly `3+N` inputs.
ApplyClosure inputs are
`[checkedHandle, mode, context, rawCount, name0, stream0, ..., nameN-1, streamN-1]`, exactly `4+2N`
inputs. Empty name text denotes a positional actual on the wire. Count, shape, limits and mode
validate before name reads or allocations; Contextual has rawCount zero. The compiler emits separate
internal literal name cells and wires the same cells to Check and Apply. Generic VLace emits empty
names, explicit mode and exact count.

Program graph metadata is mutable. Check validates current names/signature and freezes only callee
identity, as it does today. Apply reparses current names, operands and descriptor and recomputes
mapping before entry. Invalid edits refuse before body execution without undoing already executed
actuals. Compatible deliberate code edits use modified semantics. No immutable-constant promise,
frozen mapping token, registry or runtime AST is added. Arbitrary operand-edge rewiring is arbitrary
program editing; Apply validates its current instruction rather than claiming to prove unchanged
Check wiring. Ordinary compiler output guarantees the shared operand identities.

The root coordinator approves only two new append-only opcodes, GroupBegin and GroupNext after
CheckClosureCall. Update every switch, mnemonic and opcode ceiling deliberately. Native source mesh
is distinct from sealed Vortex artifact admission; do not claim sealed frame support in this
package.

## Private Groups frame value

Typed/destructured dynamic tails use one Groups argument port and one constant-size matching loop,
not per-runtime-arity frame variants. Untyped tails may flatten into a normal Stream port. Groups is
private VM execution state containing ordered member-reference groups; it contains no AST,
evaluator, callbacks or process-local callable registry. Frame descriptors and instruction graphs
remain portable and reconstruct the state during Apply in a fresh VM.

Use a typed prepared-argument representation for internal frame entry. The existing ordinary
Cell/Stream path may delegate through a checked adapter; no borrowed span may survive entry. Groups
is forbidden for frame results, closure captures, guards and direct InvokeFrame inputs. Only the
explicit AppliedRestGroups entry path may initialize it. Generic VLace remains unnamed, nonvariadic
and exact positional arity.

An immutable payload belongs to its ActivationFrame and is indexed by its private backing argument
slot. A weak slot-to-owner index is runtime type information, not another payload owner. Ordinary
reads, aliases, copies, clearing and writes must reject Groups ports before effects, including
resolved reference/clone paths. Preflight all generic outputs before mutation. Cover resultCell(s),
inputValue, forEachResultCell, CopyCellRefs and common output paths; a blank arena backing cell must
never silently become an ordinary scalar or empty stream. Include the weak index in
entry/return/choice rollback and cursor cleanup. No compiler-only secrecy assumption is acceptable.

GroupBegin reads exactly one Groups port of the current top activation and produces a group
iterator. The iterator holds weak owner/payload references plus cursor and backing identity.
GroupNext requires that exact owner to be the current top activation of that cursor. An outer
iterator refuses during a nested activation and becomes usable again after return. External
CopyCellRefs cannot keep its data alive or make it usable after return; choice restoration can
restore the genuine owner and iterator. Regular IteratorNext rejects group iterators and GroupNext
rejects ordinary iterators.

GroupNext produces an ordinary Stream and Bool presence. An empty group gives empty stream/true;
exhaustion clears the stream and gives false. Prepare/check/charge the selected group's output
before advancing iterator state or publishing output. Initially copying member refs is acceptable;
sharing requires a demonstrated COW invariant. Pending contract guards must freeze each group's
subject set before the shared output register advances. Unsupported pipeline/condition/memo
combinations refuse before effects.

## Resource and ownership accounting

Keep per-call `maxAppliedStreams`, `maxInvocationRefs`, metadata byte/record and frame/local limits.
Bound input wings before materializing them: at most 67 operands for Check and 132 for Apply under
the fixed 64-actual capacity. Then validate count arithmetic, mode and exact `3+N` / `4+2N` shape
before reading names or walking argument streams. Output wings remain bounded by the configured
frame descriptor-record ceiling. Count groups including empty headers, all member refs,
context/captures and mapping fanout before reserves/copies. Charge inspection and copy work before
allocation/publication. A failed call leaves outputs, cursor, frames, choices and backing accounting
unchanged; fuel is not refunded.

Add a distinct configurable retained-Groups reference budget to FrameLimits, initially 1,048,576
units. A unit is one group header or retained member reference, including duplicates. Reserve a
lease before payload allocation/copy and release only when the last frame or restorable choice
snapshot owner drops that unique payload. A payload shared across snapshots is counted once. Use
shared accounting state with RAII, not a raw-VM callback/deleter; accounting must remain correct
through failure, return, choice discard/restore, close and VM destruction. Lowering the configured
ceiling must not reset live usage. This is separate from cumulative arena backing cells, which keep
their existing no-refund success policy. No total CPU/heap or speedup claim follows from these unit
limits.

## Gating evidence

Require native instruction/descriptor proof alongside behavior. Cover named reorder, source-order
actual effects and formal-order defaults, omission versus explicit empty,
duplicate/unknown/collision preflight, positional-after-named refusal and named-rest one-group
behavior. Test final named/dot/ destructured rest, per-group filtering, capture isolation, lazy
default as one group, conditional required default aliases, exact numeric identity,
duplicates/order, return failures and mutation guards. Different callsites with different tail
counts must invoke the same closure body graph and descriptor; no per-arity body variants are
generated.

Test private-port scalar/read/write/copy/alias refusals, ordinary/group iterator cross-use, copied
external iterator after return, nested activation, cross-cursor use, backtracking restore, close,
retained-payload budget/release, frozen guards, output failure atomicity and same-opcode retry.
Export and reopen real graphs after destroying source AST/compiler/VM; require CLI, host and Vquery
SOURCE mesh. Preserve generic VLace and all existing frame/closure tests across format migration.

All source writers freeze before full `make -j$(nproc)`; use exact clang-format 19. Every executable
runs headlessly with all three XDG directories isolated. Commit only after combined tests and
independent architecture, senior and profiler/resource implementation gates. Current baseline
failures remain classified; passing this package alone does not close §4.12 or the overall
specification.

## Design gate disposition

Independent Luna architecture and resource reviews accepted this revised design after the explicit
private-port type boundary and retained-payload lease resolved their blockers. The senior review
accepted the creation-time conditional-capture policy above while retaining deferred named-frame
checks. These design gates authorize the bounded implementation, not completion or a performance
claim. Implementation gates and all execution validation remain required.

## Implementation and execution evidence

The full `make -j$(nproc)` passes. The focused matrix passes all 82 tests, including 28 added cases
and two updated former-refusal cases. Tests prove one closure descriptor and group loop serve
callsites with different arities; shared matching filters whole invocations on any failed group. The
real headless CLI returns `9007199254740993` three times exactly. Fresh-VM Store reload after
AST/compiler/source-VM destruction and host execution retain those three identical references;
Vquery SOURCE mesh executes named/rest calls with distinct exact integer outputs.

The complete engine run has 4,455 active tests: 4,394 passed, 33 failed and 28 skipped, with 12
disabled probes. Of the 33 failures, 32 are the unchanged committed baseline names. The additional
headless UI selection-pixel test passed in an isolated retry; no source change was made for it. The
affected VQL/VM/VLace subset of that run has 1,641 active tests: 1,619 passed, 21 baseline failures
and one opt-in skip, plus eight disabled probes. All added cases passed. This is not a full-suite
pass.

Independent Luna architecture, senior and resource/lifecycle reviews accepted the bounded
implementation. Follow-up review verified creation-time required rest aliases, bound-empty capture,
per-group guard subject snapshots, snapshot restoration and retained-payload release. Tests make no
speedup or total-heap claim. Native descriptor/frame version changes have no old-format reader.

The route test exposed an unchanged C8 packaging defect: `ResultBuilder` connects multiple result
records to one repeated value through a reciprocal `d.mesh.value` rank, so later occurrences evict
earlier value links. Native/host duplicate assertions remain enabled; the bounded mesh signature
case uses distinct values. This defect remains open and is not counted as successful duplicate mesh
transport. The next route package must represent occurrences separately from their canonical value
and validate publish/import/reload identity, order, duplicates and exact scalar bits.

Logs: `/tmp/vql-signatures-build.log`, `/tmp/vql-signatures-focus.log`,
`/tmp/vql-signatures-full.log`, `/tmp/vql-signatures-ui-retry.log`, `/tmp/vql-signatures-cli.log`,
`/tmp/vql-signatures-format.log`, `/tmp/vql-signatures-lint.log`, `/tmp/vql-signatures-lib.log`,
`/tmp/vql-signatures-zigzag.log`.

Repository format-check with exact clang-format 19 passes. Library tests retain their one baseline
failure (841 passes); slice tests retain their two baseline failures (266 passes). Lint retains only
the two unrelated UI typography violations at `acme_presenter.hpp:28` and
`slice_view_presenter.hpp:46`; none of those files changed.
