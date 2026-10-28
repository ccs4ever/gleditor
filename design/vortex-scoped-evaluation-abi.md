# Scoped evaluation graph ABI

`epistemic-scope-1` is the live graph ABI for the scoped result-transfer primitive in
[vagent design section 6.1][p198]. The current I2A implementation provides scalar/control execution,
typed transfer, and nested continuations. Guarded structural operations, structured/reference
assignment, and query compiler lowering remain I2B; this foundation does not complete P19.8.

## Operands and descriptor

`ScopedEvaluate` has exactly five input slots and one output slot. Every slot is a relocation-marked
`OpHandle`; integer cell addresses are refused. The inputs name callback entry, dedicated completion
`Nop`, caller-owned typed arguments, descriptor, and private frame-handle register, in that order.
The output names a caller-owned destination record. Completion requires an explicit callback-root
Jump.

The descriptor content is `epistemic-scope-1`, with exact named fields `args_schema`,
`result_schema`, `effect_graph`, and `bindings`. Records use `d.vars` and `d.values`; lists have
content `[]`, start on `+d.grab`, and continue on `+d.step`. A record marker is the first empty
field name, with no value. Unknown or duplicate fields refuse validation.

A record schema has content `ScopeRecordSchema` and a `fields` list. Each field schema has content
`ScopeFieldSchema`, `name`, and `kind`. Supported kinds are `Float`, `Int`, `Bool`, `Str`, `Unit`,
`Record`, `List`, and `PinnedRef`; `Cursor` is a binding kind only and refuses as a schema field.
Records add `fields`; lists add `item` and positive `max_items`; strings add positive `max_bytes`;
pinned references add `pinned_root`, naming a caller-pinned argument. Bounds must fit the admitted
host and actor grants.

A binding has content `ScopeBinding`, with `register: OpHandle`, `target: Str`, and `kind: Str`.
Targets are `arg.<field>`, `result.<field>`, `temp.<name>`, `outcome`, or `error`. The last two
require `Str`. Argument bindings are read-only. Registers and original targets must be private and
disjoint from protected schema, argument, destination, and instruction cells.

## Explicit assignment and unique constants

Schema validity does not prove assignment. Fresh result storage may contain internal placeholder
bits, but those bits are inaccessible as initialized data and cannot produce `Value`. Every required
result leaf must be assigned by a successful typed VM write or by successful validated nested result
publication. Reading an unassigned result or temporary cell refuses before rendering or executing an
opcode. Nested argument snapshots also refuse any unassigned source cell from an active parent.

The assignment proof is bounded VM execution metadata. It is trailed with choices: a failed branch
restores both the arena and assignment flags. A nested publication updates the parent's assignment
proof atomically with publication; failure rolls back both. A completion with any unwritten required
leaf is `Refused(TypeMismatch)`, including an unwritten numeric zero, false boolean, empty string,
empty list, or caller reference.

Only `Unit` and an empty `Record` may explicitly declare the optional schema field `constant: true`.
This declares their unique value initialized; omission or `false` supplies no assignment proof. The
flag must be a Bool and is refused on all other kinds and on nonempty records. Thus a schema cannot
turn numeric zero or a default reference into an initialized result. I2A does not provide a new
structured-copy meaning for scalar arithmetic; ordinary structured/reference writes remain pending.

## Structural reads

Inside a scope, and only there, three existing opcodes have typed read-only meanings; their
manifold-writing bodies never run. No opcode is added. Operands are bindings, checked statically by
kind and dynamically for assignment and redirection.

| Opcode  | Inputs                   | Outputs               | Meaning                                  |
| ------- | ------------------------ | --------------------- | ---------------------------------------- |
| `Point` | position                 | `Cursor`, `PinnedRef` | set a cursor, or publish a pinned result |
| `Link`  | position, selector `Str` | `Cursor`, `Bool`      | move by a selector; report whether found |
| `Deref` | position                 | scalar                | read the declared scalar at the position |

A position is a `Cursor` temp, an argument `PinnedRef` (its target), or an owned argument `Record`
or `List` (itself). Result references are never positions, so an unassigned result cannot be walked.
Selectors are `first` (`+d.grab`), `next` (`+d.step`), `target` (an `OpHandle`, only when it names a
pin or reached cell), and `.name` for a record field (`d.vars`, then `d.values`). The prefix keeps a
field from shadowing a navigation word; any other word refuses `TypeMismatch`, so a typo is not a
missing field. Field names are compared by charged span length before their bytes are read and
debited. A missing neighbour writes `false` and leaves the cursor as it was; callers branch on the
flag before reading. `Deref` refuses `TypeMismatch` unless the position already holds the declared
kind; a stored non-finite Float refuses the same way, so it can never become a score.

Positions reached by walking from a pin are recorded with every root that reached them. The scope
cannot link caller cells, so a reached cell predates the inner mark and stays reachable; the record
only grows and needs no trail, and each entry is charged as traversal work. A `PinnedRef` result may
name a pin or a cell reached from the root its schema names in `pinned_root`. Owned argument copies,
scratch and cells reached only from another root refuse. Publication also excludes the destination's
old protected record graph: its root, marker/name cells, field values, and recursively typed
payload/error/outcome cells. Even an independent caller pin cannot export these cells, since
publication replaces their destination ownership relation. The exclusion applies to every active
scope, so a nested scope cannot launder a parent's old destination cell. It is checked both when
`Point` assigns a `PinnedRef` result and when the typed result is captured before release.

This is an ownership closure, not unrestricted reference reachability. The shared relocation
dimension and the external caller referents of old `PinnedRef` values are outside it; an unrelated
caller record's field cells remain exportable. Cursor moves are ordinary trailed writes, so a failed
branch restores an unassigned cursor. A nested scope may take a parent's reached cells as pinned
arguments.

Numeric idioms: counts stay `Int`; `Mul` or `Add` with a Float promotes them, so `Add(count, zero)`
converts explicitly. Float constants arrive as arguments. Results and temps may be rewritten, which
is how accumulators work. A domain or overflow fault refuses the scope even under an open choice; it
never backtracks into `Empty`.

Ties and ranked results use one scope per answer, as P19 section 6.2 requires: a ranked generator
keeps its candidate IDs, scores and position on owner cells outside the per-evaluation mark, and
each resume runs its own scope that publishes one binding. `truest` fails on a tie. No result list
append is therefore needed for the initial queries.

Why C++ and why these opcodes: the reads must honour scope ownership, assignment and pin provenance
that only the VM's live frame knows, so they cannot be a Vortex library over the ordinary `Link`,
which takes raw dimension refs and writes. Three refused alternatives: new opcodes, which add enum
entries beyond the approved P19.8 register; a Vortex library, which would have to run the
manifold-writing `Link` inside a scope; and descriptor-declared views, which fix the walk shape per
query and cannot express a profile lookup by a runtime source name. The overloading is confined to
`stepScopedStructure`, reached only for verified scope graphs. `ScopedEvaluate` stays outside the
mesh artifact allowlist, so no persisted pure job carries these meanings.

## Outcome and lifetime

The frame has owned `args`, `result`, and `outcome`. Its initial outcome is `Value`, but a valid
payload still requires the assignment proof above. `Empty` has no payload; `Refused` has a typed
whitelisted symbolic error, supplied through an explicitly assigned `error` binding. The published
destination has `outcome`, optional `payload`, and optional `error`; absence is structural, without
an integer or empty-string sentinel.

The callback runs on the existing cursor under an inner mark. Child choices cannot consume the
caller's choice floor. Result capture precedes inner release; reconstruction runs under a separate
publication mark, so failure restores the complete old destination. New inner strings become normal
outer scratch and can subsequently be promoted. Stable primedia spans, caller scratch prefixes, and
source namespace pins retain their identities; only scratch suffixes invalidated by release are
copied into native ownership.

## Resource admission

Execution requires an explicit validated `ScopedEvaluationLimits` grant. Admission clamps it to
remaining actor work, scratch bytes, and cell allowance before descriptor parsing and native result
carriers. Nested scopes share monotonic work and cumulative byte/traffic budgets, and each callback
step consumes every active ancestor's instruction allowance. Metadata reads are conservatively
charged again. Flat schema/value node arenas keep all walks and teardown independent of native stack
depth. Budget consumption survives arena rollback.

[p198]: vagent-dynamic-source-weighting-and-epistemic-arbitration.md#61-scoped-result-transfer-p198
