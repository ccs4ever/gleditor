# I2B-2 architect/governance review

Scope: uncommitted diff (scoped_evaluation.\*, vortex_scoped_evaluation.cpp, vortex_vm.hpp, ABI doc,
tests).

## Findings

1. MAJOR - Context-dependent opcode meanings are a core-VM semantic change outside the literal P19.8
   register. Evidence: spec section 8 row P19.8 approves "One ScopedEvaluate enum entry ... typed
   evaluation frame/reference operands and bounded argument/result copy". Section 6.1 describes the
   callback as a "compiler-verified pure callback" using "compiler-generated handles, as ordinary
   framed routines do" - i.e. existing graph ops over frame handles, and the only new engine surface
   named is the primitive, the operand reader and the budget. This diff adds three new read-only
   instruction semantics (Point/Link/Deref), a new `ScopeType::Cursor` kind, a new per-frame `reach`
   grant that widens what a scope may pin, and a PinnedRef acceptance rule in
   `ScopeCodec::snapshot`. "No new enum entry" keeps the letter of the register, but the register's
   stated price (one enum entry) was the owner's approval basis; the project's own memory rule says
   each core change beyond it needs separate approval, and the register does record "typed ...
   operands" only for the frame. Evidence it is not a mere detail: the same opcodes already have
   other shapes in the tree - vagent compiler.cpp:750 `Link` takes 4 inputs / 1 output, :757 `rd`
   pipelines, :592/:721 `Deref` and :652 `Point` with different operand conventions - while the
   scope version is 2-in/2-out for Link. One opcode now has three arities depending on context.
   Recommendation: before commit, record an explicit owner decision (approval-p19.md addendum) that
   "structural reads inside scopes" is within or an extension of P19.8, with the alternatives priced
   below. If the owner declines, the fallback is the alternatives in finding 2.

1. MAJOR - Overloading is a maintainability hazard; the lower-risk alternative was not priced in the
   ABI doc. Evidence: `pure()` in vortex_scoped_evaluation.cpp now whitelists Point/Link/Deref,
   which every other consumer treats as manifold-writing or arena-binding
   (mesh_evaluator.cpp:238-240 returns false for Point as "binds a handle to a local arena cell";
   Link is the VM's relinking op at vortex_vm.cpp:665). The safety of this rests on the single early
   dispatch `if (structural(*kind)) return stepScopedStructure(...)` in stepScope plus static
   verification in verifyScope. Any future path that executes a scope graph outside stepScope
   (artifact serialisation, mesh evaluator, VQL/VPL decoders whose switch tables at
   vql/compiler.cpp:63,115,125 and vpl/compiler.cpp:269,321,331 name these ops with the classical
   meaning, the tracer, a disassembler) will silently misread a scope graph, and the reverse: a
   graph compiled with a classical 4-in Link inside a scope is refused only at verify time with a
   generic "operand arity" message. The mesh artifact allowlist is currently safe only because
   ScopedEvaluate itself is excluded (mesh_evaluator.cpp:247); the inner Point/Link/Deref are not
   individually tagged as scope-only. The alternatives, for the record: (a) new opcodes `ScopeRead*`
   \- clean, but each is a core enum entry needing approval and touching every decoder table (the
   audit P19.8 already budgets for ScopedEvaluate only); (b) Vortex-level library graphs over pins -
   cannot work, because reads of caller cells are exactly what the inner mark/arena discipline
   forbids a plain graph from proving; (c) descriptor-declared views - best fit for "zigzag cells
   and dimensions rather than crammed semantics": the descriptor's bindings would declare
   `view: first/next/field` so the opcode set stays untouched and the verifier sees structure, not
   string selectors. Choosing the overload is defensible for cost, but the doc must say why (a)-(c)
   were refused and add a single shared predicate (e.g. `scopeStructural(OpcodeKind)` in one header)
   plus a test that every decoder/name table rejects or labels these ops when found inside a scope
   descriptor. Recommendation: add a "refused alternatives" paragraph and a cross-decoder audit
   test.

1. MAJOR - Design does not yet cover I4 needs; the primitive set is not closed over the spec's
   queries. Evidence (spec 6.1 plus README I4 row: truest/ranked/consensus, per-source profiles,
   selection tokens from stable observation IDs, scalar fusion exports):

   - No iteration over per-source profiles: `Link first/next` walks `+d.grab`/`+d.step`, and field
     walk takes `d.vars`; a profile table keyed by source needs a keyed lookup or a list walk that
     is not a list-of-record walk. Nothing says profiles live on those dimensions.
   - No scratch accumulators: Cursor and Deref are read-only, and the diff provides no writable
     scalar temp beyond bound temps, no structural scratch alloc. The README itself lists "guarded
     structural scratch operations" as remaining I2B work; ranked/consensus need running sums,
     argmax and top-k state.
   - No ranked/generator state: backtracking over candidates with a running best requires a value
     that survives Fail (non-trailed accumulator or findall/aggregation); `reach` is deliberately
     non-trailed but is just a grant set. Without it truest cannot be a pure Vortex graph without an
     unbounded Cut/Fail trick.
   - Stable observation IDs: the only publishable reference is a PinnedRef to a pin/reached cell;
     the ID the spec wants (stable across release) must be read via Deref Int/Str from the cell.
     This works if observations carry an ID cell, but the diff does not require or test a stable-ID
     field.
   - Scalar fusion export: Deref + Float temps + existing arithmetic work, which is the strongest
     part. Recommendation: state in the ABI doc and I2B plan which of these ship in I2B-3 (scratch
     ops) and that I4 queries are specified as walks over d.grab/d.step observation lists with an ID
     cell; add one end-to-end test shaped like "best observation over N candidates with a
     source-profile lookup".

1. MINOR - Selector strings as runtime `Str` data ("first","next","target", field names). AGENTS
   "ZigZag cell representation" says meaning belongs in dimensions, not in parsed text. Here the
   selector is a Str bound to a register, compared with `==` on each step (O(len) plus charge),
   invisible to the static verifier; a typo is a runtime TypeMismatch rather than a compile-time
   refusal, and field selection is a linear scan of `d.vars`. It is acceptable only because it is
   bounded (`boundedText`, `limits.fields`) and the vagent compiler can emit constants. A cleaner
   shape is a selector cell whose identity is a dimension cell (first/next = d.grab/d.step as
   dimension cells; field = the d.vars name cell), resolved once at verify time. Recommendation:
   either record this as a deliberate I2B-2 shortcut with a follow-up, or switch the selector to a
   dimension-cell reference.

1. MINOR - Magic limits: `reached()` bounds growth with `f.limits.fields`, reusing a schema-field
   budget as the reach budget, and the visited set also uses it. Name a dedicated `reach` limit or
   document why one budget is intended; AGENTS forbids unnamed behavioural numbers (this is a reuse,
   not a literal, so MINOR).

1. MINOR - Doc/code mismatches.

   - ABI doc says "`first` and `next` follow +d.grab and +d.step"; code uses
     `core_.dims().grab/step` with `DimVector::POS` - consistent. But the table says Point outputs
     `Cursor`, `PinnedRef` while the verifier (`to == Cursor || to == PinnedRef`) is only for
     out[0]; fine.
   - Doc: "Result references are never positions, so an unassigned result cannot be walked." Code:
     the verifier requires `from` to be Cursor or an *argument* positional kind; a Result-target
     Record/List is refused only by the `fromArg` conjunction, but the `positional()` set includes
     Record and List that are also valid result kinds - the doc should state "argument" explicitly
     for Record/List (it does) and note the verifier relies on the target prefix `arg.`, which is
     not re-checked dynamically in `position()` (it checks f.readable only).
   - Doc says a missing neighbour "leaves the cursor as it was"; code does exactly that, but then a
     subsequent Deref reads a stale cursor with no found-flag dependence - document that found must
     be branched on, or add a test (partially covered by tests).
   - Doc omits that `target` silently yields not-found (found=false) for a non-pin handle rather
     than a refusal; that is a security-relevant silent narrowing.

1. MINOR - Engine purity and layering are intact. Changes live in `apps/common/xanadu/vortex/`; no
   ui/ or graphics includes; no library dependency added (`<unordered_map>` only). Optionals are
   used, no sentinels, setters chain (`setPinnedReach` returns `this`). C++ justification for
   not-Vortex is implicit (it is VM-internal arena ownership) and should be stated once in the ABI
   doc.

1. MINOR - Verifier/runtime double-check duplication. `kindOf` re-derives kinds from bindings but
   `stepScopedStructure` repeats `bindingTypes` lookups and `bound()` redirection checks; fine for
   defence in depth but the two should share one helper to avoid drift.

## Tally

BLOCKING 0, MAJOR 3 (findings 1, 2, 3), MINOR 5 (4, 5, 6, 7, 8).

The implementation is careful (verify-time kind checks, trailed cursor writes, pin-rooted reach with
publication gating) and I found no soundness hole in the pin/reach model. The MAJOR items are
governance and design-completeness: an unrecorded owner decision on overloading, no
refused-alternatives record or cross-decoder guard, and no demonstrated coverage of the I4 query
shapes.

VERDICT: FAIL
