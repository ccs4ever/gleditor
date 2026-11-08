# Runtime binding persistence and VQL sessions

## Deliverable

Iteration 52 groups runtime binding persistence, bound closure export/reload and session
publication. The target is a closure created inside a native activation that remains usable after
its creator returns, a later query starts, and its graph is saved and opened in a fresh VM. Full
build, all 1,084 affected tests across 100 suites and all 26 focused tests across six suites passed.

## Separate cell data and VM bindings

A cell's text, scalar bits and topology are independent of a VM register binding at the same cell
identity. In particular, a stream member can be a cell that also names another register. Register
lookup is one level: if A refers to B and B refers to C, reading A yields B. Streams preserve their
literal member references, order and duplicates. Following B while reconstructing A changes the
program. Binding cycles also preclude reconstruction by a topological sequence of ordinary copies.

A versioned `VORTEX_BINDINGS/1` graph therefore records literal register slots and their values.
Cell bindings include explicit missing values; Stream bindings include explicit empty membership.
Absent slots remain open. Closed Iterator bindings retain unavailable or exhausted type markers.
Live iterator position, activation stacks, choices, guard scopes, mutation groups and resumable
continuations are outside this transport stage and must refuse before Store writes.

`RestoreBindings` initializes the maps through a native VM operation. It parses tagged literal
references without consulting the maps being restored, stages replacement containers, and publishes
them together. Old bindings absent from the snapshot disappear. Successful restore counters remain
cumulative. Descriptor records, slots, references, metadata bytes/relations, inspection work and
cumulative loads have explicit logical limits; these do not claim a total CPU or heap bound. One
local inspection counter covers root/tag text, fragments and bytes, relation scans, records and
final snapshot validation. Metadata roots and records must be local or base-backed; foreign
descriptor metadata explicitly refuses.

The exporter retains exact cell data and relocates declared reference records. VM bindings relocate
through their descriptor instead of overwriting register cells' physical payloads. Parameter wings,
frame/closure descriptor references and cursor PCs retain their explicit relocation roles. Ordinary
integer values never become addresses merely because their bits equal a cell identity. The source
arena remains read-only during export; descriptor records and the initializer are written into the
destination graph after preflight and complete reference mapping.

## Session publication

The compiler reports the final top-level binding layout as compilation metadata. After successful VM
execution, the session executor reads actual cell-reference results and publishes an environment
update before request cleanup. A failed compile or execution publishes nothing. Loop, conditional
and callee locals remain scoped. Rebinding between ordinary values, closure values and static
functors invalidates the previous kind.

Static functor syntax may remain a compilation intermediate for a subsequent query; applying it
still compiles a Vortex graph. Closure values are graph cells and do not require the source AST or a
process-local callback registry. Publication preserves separate Int64 and Double cells, empty
bindings, ordered duplicates and capture membership while sharing the referenced data.

## Acceptance

The combined gate covers literal alias cycles, data/register role overlap, disconnected and missing
bindings, ordered duplicate/empty streams, closed iterator markers, repeated initialization, quota
failure and retry, malformed metadata, and pre-effect refusal of live control state. Portable bound
closure application must run after source VM destruction. Session tests cover later queries with a
fresh VM, shadowing, exact numeric kinds, lexical confinement and execution failure without
publication. Full build, relevant route tests, repository formatting and independent Luna
architecture, senior and resource gates remain required before committing. The real REPL creates a
closure recursively in one query and applies it in the next, returning exact `9007199254740993`. The
portable bound-handle regression starts directly at application after reload, so it cannot recreate
the captured value by rerunning its original factory. A fuel sweep checks every failure threshold
before successful restore and retries under a cumulative load quota.
