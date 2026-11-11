# Runtime timestamps and computed VQL projection targets

Iteration 55 is committed as `588277f2`. Iteration 56 combines the remaining fourth §4.12 example
with graph-native timestamps and pure computed projection targets. Implementation and validation
are in progress; this document does not establish completion.

## Owner decisions and example audit

The owner explicitly defined `@now` as the current UTC timestamp, rather than a logical
microversion. It must sample during VM execution, never during compilation, and create a fresh
Timestamp cell with signed epoch-nanosecond bits and canonical UTC ISO8601 text. Existing
MicroversionId literals and historical pin semantics keep their separate meaning. Ordinary `now()`
is an additional callable form; the reserved literal cannot be hijacked by a user function.

The committed CLI executes the first three §4.12 example definitions verbatim: `score_paper` returns
`95`, `format_title` returns `[note] Example`, and `process_video` returns `av1 @ 60fps`. The fourth
example fails to parse `@now`. A second issue is independent of the clock: the example's exclusive
projection drops the priority/owner fields required by its own parameter contract. Replacing only
the clock with a literal demonstrates the correct named contract refusal; adding `*` preserves the
fields and archives successfully. Preserve the explicit §4.8.3 masking rules. Retain the original
body as an atomic-refusal regression and correct the example with `*` before its success check.

## Graph implementation

Append `CurrentTimestamp` to the VM opcode enum and use the existing scalar-output transaction
path. Return an actual CellRef through the register map; timestamp bits are ordinary data. Preserve
output maps, cursor state and arena contents on malformed input, quota or publication failure.
Fresh runtime calls return distinct cells; UTC wall-clock samples are not required to be monotonic.

Parse the reserved timestamp literal into a compiler intrinsic and lower it and the callable form to
the native opcode. Literal identity must remain separate from user-defined function lookup. No new
language evaluator, runtime AST callback or compile-time clock constant is permitted. Timestamp
query-scalar reads retain the existing canonical text fallback.

Projection targets may use supported scalar/alias/read-path/arithmetic expressions, clock/string
builtins and verified read-only named helper graphs. Reuse the bounded scope walker for a separate
read-only definition summary and reverse dependency propagation; preserve ordinary frame
eligibility. A `pure` annotation alone is insufficient. Reject writes, creation, unknown effects,
indirect callables and attached pipelines, including hidden helper captures, before effects. Pure
recursive helper SCCs can survive when all reachable operations meet the same rules.

Prepare targets once per owner inside the existing mutation group and before masking. Require
exactly one result, keep the original owner context and attach existing values through fresh clones.
Failure restores links, proxies, temporary timestamps, slots and contracts without refunding fuel.
Keep unknown/unsupported syntax explicit.

## Validation and workflow

VM and frontend Luna tracks own disjoint files; the root owns the documented examples and route
regressions. All writers freeze before clang-format 19 and the full `make -j$(nproc)`. Every
executable is headless with all three XDG directories isolated.

Acceptance includes all four corrected example bodies, original exclusive-projection refusal,
parameter filtering and named promises, real timestamp bits/text, runtime allocation/fresh identity,
function shadowing, owner context, helper recursion/defaults, ordered duplicates, zero/multi-result
refusals before masks, effect/pipeline rejection, rollback/retry, source Store integrity, AST-free
export/reload and CLI/host/mesh SOURCE routes. Sealed volatile-opcode support remains an explicit
separate boundary. Independent architecture, senior and resource reviews follow actual validation.
Full §4.12 remains open.
