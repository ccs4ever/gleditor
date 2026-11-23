# VPL stakeholder needs (pre-review)

Target: `design/vortex-io-redesign.md` v0.9. This is a source-grounded frontend needs brief, not a
formal R1 critique. Proposed syntax below is illustrative only; it is not implemented or frozen.
There are no production users or production ABI to preserve. The redesign may freely change
in-memory APIs and interfaces; existing on-disk/wire versioning and `CompactOpNode` size/alignment
invariants remain as stated in §1.

## VPL's evaluation model and design boundary

VPL is an applicative array language: expressions are verbs over `VplView`s, not function calls or a
backtracking query language. The existing lens bridge records that calls such as `f(x)` parse as
strand syntax and `evaluateNode` runs each AST node once (`design/vpl-lens-bridge.md:10-15`, citing
`parser.cpp:223-234` and `vpl_engine.cpp:206-245`). Array traversal is therefore not itself an
effect retry loop.

A `VplView` currently represents either a scalar or one origin plus ordered directed axes; there is
no explicit cell-list payload (`apps/common/xanadu/vpl/view.hpp:40-52`). Its `collectCells()` walks
cells in standard order (`view.hpp:332-340` onward), while `shape()` can detect ragged extents
(`view.hpp:274-303`). The redesign's VPL summary correctly calls for N-cell submission order,
multiplicity, bounded concurrency, and asynchronous array results
(`design/vortex-io-redesign.md:209-210`). It should say how the output preserves input shape,
including empty and ragged views, without materializing a whole large range in memory.

VPL syntax, effect placement, multiplicity, and frontend-local continuation state belong in the VPL
compiler/adapter. The shared layer should provide language-neutral capability, request, payload,
pending, outcome, and cancellation cells; it should not acquire APL glyphs, array-shape policy, or a
VPL-specific opcode. This follows the redesign's P4 boundary (`design/vortex-io-redesign.md:880`)
and keeps VPL's language model separate from the generic protocol.

## Two concrete proposed examples

These sketches clarify desired semantics, not a proposed frozen grammar. They build on the
redesign's existing illustrative VPL line `R ← ⊃ submit¨ Caps ReadSpecs` and `await¨ R`
(`design/vortex-io-redesign.md:502`).

### 1. Vector submission with shape and deterministic correlation

```apl
Caps  ← 2 ⍴ d.capability       ⍝ rank-1 array of two capability cells
Specs ← ReadSpec¨ Offsets       ⍝ same input shape; each spec is typed and range-bounded
Reqs  ← submit¨ Caps Specs      ⍝ one fresh request per corresponding pair, in VPL walk order
Out   ← await¨ Reqs             ⍝ result array retains request positions, even if completion reorders
```

Required rule: `submit¨` over a finite VPL view creates one fresh request for each logical input
position, in deterministic VPL walk order. It returns immediately with a same-shape array of request
cells (or a same-shape typed admission outcome if some positions are refused); it does not wait for
earlier requests to complete before admitting later ones, subject to explicit
bounded-concurrency/backpressure policy. Results correlate to each input position, not to completion
order. `await¨` may yield a pending result per position and resume by observing the same request
cell. An explicit reevaluation of `submit¨` is new work; re-awaiting or rendering `Reqs` submits
nothing. The syntax for zipping two arrays must define shape mismatch and scalar extension; until
then this example assumes equal shapes.

### 2. Bounded range reads and typed payload access

```apl
Cap  ← ⊃ caps
Req  ← submit Cap (ReadRange sourceGeneration byteOffset maxBytes)
Step ← await Req
Bytes ← payload Step.outcome
Part ← slice Bytes start length
```

`ReadRange`, `payload`, and `slice` are proposed names, not actual VPL verbs. They express that
binary data stays typed and range-addressable through cells, rather than becoming text or requiring
an entire file/media object to be copied into one VPL value. A large stream is consumed in bounded
chunks by issuing a later range/read request after observing the prior outcome; every chunk has its
own request and lease, and the program can retain/release the payload cell according to its lifetime
contract. It must not treat a mutable path or a reused VPL slot as a stable source generation.

## Generic request/outcome trace as VPL should see it

1. A host supplies a capability cell and VPL constructs typed request-spec cells. Construction,
   array mapping, shape inspection, arithmetic, query/lens traversal, and display are pure.
1. The explicit `submit` verb is evaluated once per selected array position in VPL walk order. Each
   admitted occurrence mints a distinct request cell; admission refusal is represented at that
   position as a typed outcome. Backpressure is visible, not a hidden wait or dropped item.
1. The host/service runs work off the evaluation/UI thread. It may complete requests in any order,
   but correlation remains attached to the original request cells and the VPL result array preserves
   input positions/shape. Concurrency and retained pending/result bytes obey service budgets.
1. `await` observes each named request. A not-ready request yields `Pending`; resumption observes
   the same request and cannot re-evaluate submit. A terminal outcome cell distinguishes success,
   EOF, rejection, error, cancellation, overflow/gap, and unknown/partial completion as applicable.
   Partial payload/progress remains inspectable with the terminal state.
1. Payload is a typed cell with bounded range/slice reads and an explicit lifetime. VPL may map,
   index, or reduce over returned values, but these observational array operations neither submit
   nor silently capture bytes into permanent store content. Explicit capture is a separate effect.
1. Cancelling an await/wait is distinct from cancelling the underlying request. An explicit
   cancellation request reports actual progress and uncertainty; it does not imply sent bytes or
   backend work were undone.

## Questions for convergence

- Does the generic protocol carry only request identity and individual outcomes, leaving batch
  correlation and shape to VPL, or does it need an optional group/batch identity for diagnostics and
  partial admission? Whatever choice is made, VPL still needs deterministic position mapping.
- Are effects prohibited inside rank operators/adverbs such as `¨`, `/`, and `\\` except at an
  explicit `submit` site? Existing VPL is applicative and no-backtracking, but map/reduce can
  multiply or collapse operations. The language bridge should define map multiplicity and refuse
  ambiguous effectful reductions/folds unless their submission semantics are explicit.
- How are shape mismatch, scalar extension, ragged views, empty arrays, partial admission, and
  per-position errors represented without turning a whole batch into one opaque error?
- How do VQL query results and lens-derived VPL views carry typed request/outcome cells while
  keeping query enumeration/display pure? Traversing or rendering a query/lens result must not
  auto-submit.
- Which language-level operation retains or drops a pending request/payload lease, and what happens
  when a mapped result outlives the array/view or an evaluation frame?

## Source anchors

- `design/vpl-lens-bridge.md:10-15` — applicative evaluation and one evaluation per AST node.
- `apps/common/xanadu/vpl/view.hpp:40-52` — scalar/origin+axes model, with no cell-list form.
- `apps/common/xanadu/vpl/view.hpp:274-303` — shape walks and raggedness detection.
- `apps/common/xanadu/vpl/view.hpp:332-340` onward — standard-order cell collection.
- `design/vortex-io-redesign.md:209-210, 225-226` — VPL bridge requirements and deterministic
  submission order.
- `design/vortex-io-redesign.md:480-507` — illustrative language traces including VPL and direct
  generic Vortex request/await.
- `design/vortex-io-redesign.md:880` — P4 assigns syntax, types, purity/effect placement,
  multiplicity, lowering, and frontend-local continuation to adapters/compilers.

No source or runtime changes are requested by this brief.
