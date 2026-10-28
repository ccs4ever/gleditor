# ML gate review: I2B-2 structural reads vs I4 analytic scoring (I2 ABI gate)

Scope: diff /tmp/vagent-p19-i2b2/i2b2.diff, vortex_scoped_evaluation.cpp, scoped-evaluation ABI doc,
spec 3/5/6/8.1. I read the code and the 7 new tests; I did not run them.

## Findings

1. MAJOR - Ranked and tied selection cannot be returned. A Point can only set one PinnedRef or
   Cursor. Nothing in the ABI appends to a result List (the ABI doc: "structured/reference
   assignment ... remain pending"). Result leaves are all-required with no optional field (the
   "unassigned = TypeMismatch" proof). So truest/ranked/consensus with ties (v1.5 order, the tie set
   within score_tolerance) has only three shapes: a fixed-arity Record of PinnedRef plus Bool flags,
   which forces a dummy pin for absent ranks; one scoped call per rank with an exclusion argument;
   or a single winner. I4 would have to pick one of these without the ABI saying which is
   sanctioned. Decide this before I4, as an ABI note plus a test, even if the answer is "one call
   per rank". Defer the append primitive itself. Tests only cover a single selected observation.

1. MAJOR - Per-clause scans are budgeted by the wrong grant. `reached()` refuses with ResourceLimit
   once `f.reach.size() >= f.limits.fields`. `limits.fields` is the schema-node/field bound. Every
   distinct cell touched by `first`/`next`/field links is recorded. A Beta-warrant scan over N
   observations with k fields each adds about N*k entries, so it needs fields >= N*k. A grant sized
   for schema shape will trip on a realistic clause with many supports. The refusal is typed, not
   Empty, but it fails exactly the dense case I4 targets. Recording only cells reached by
   `first`/`next`/`target`, not field-value cells, or giving reach its own validated grant, fixes
   this. Add a test with a scan larger than the schema size. This is a gap I2 must close, because
   the grant semantics are the ABI.

1. MAJOR - The ABI cannot hold the Float constants the scoring graphs need. Only Unit and the empty
   Record may be `constant: true`. There is no literal and no scalar-copy meaning. Accumulator zero,
   1.0, 0.5, epsilon and "2" must arrive as Float arguments or be derived. Add/Sub/Mul with a Float
   argument can produce them, and a temp is writable repeatedly (outputs only need `writable`, and
   assignment flags are re-set). So it is expressible, but nothing proves it. Required before I4:
   one test of a loop with a Float accumulator (Branch/Jump + Add into a temp, trail-restored on
   Fail). State in the ABI that constants come from a validated Float argument record. Without that
   evidence I cannot confirm that sums, inverse-variance fusion and pooling are expressible. This is
   a missing-evidence block, not a design defect.

1. MINOR - Int counts in float math. Deref is exact-kind: `asDouble` is nullopt for Int64 and
   `asInt64` for Double. A TypeMismatch refusal terminates the whole scope; the callback cannot
   catch it. For counts, `executeOpcodeBody` Add/Sub/Mul promote mixed Int/Double through
   `toDouble`, so `Add(IntTemp, FloatZeroArg)` into a Float temp is a clean idiom, and the input
   check accepts it (binding kind matches stored kind). The strictness is right: auto-conversion
   would hide a profile schema that stores alpha as Int. Requirements: (a) document the idiom, (b)
   test mixed Int/Float through a scope, (c) I3 must store alpha/beta/variance/times as Double and
   counts as Int64 consistently. Int overflow becomes CallbackFault, which is safe. Large counts
   lose precision above 2^53, acceptable and worth a note.

1. MINOR - Non-finite stored values. Deref Float refuses a stored NaN/inf as TypeMismatch, with the
   same code as a wrong kind. That is fail-closed, which is right, and it is never Empty and never a
   score. Whether stored non-finite should instead be InvalidEpistemicPolicy depends on the source.
   For policy values (c, rho, epsilon, half-life) the spec requires InvalidEpistemicPolicy before
   arithmetic. The callback cannot emit that from a Deref fault, since the abort pre-empts the
   `error` binding. So I4 must either validate policy at the argument boundary (Float args are
   snapshotted) or read it via Deref only after the outer layer has validated it. For observation
   data (variance, alpha, beta), TypeMismatch mapped to a typed numeric refusal is acceptable. The
   outer mapping TypeMismatch/CallbackFault -> distinguishable epistemic refusal is I4's obligation.
   No test covers non-finite Deref; add one.

1. MINOR - Callback-detectable refusals are available. Missing variance or a missing field: `Link`
   writes `false` without moving the cursor, so the callback can branch to a whitelisted
   `MissingVariance` via the `error` binding. Present-but-zero or negative variance can be tested
   with Lt/Lte before Div. Numeric faults (Ln\<=0, Sqrt\<0, Div by zero, Exp overflow, Int overflow,
   non-finite output) go through `executeOpcodeBody` failure or the isfinite output check straight
   to `finishScope(Refused, CallbackFault)`. They are terminal and do not backtrack into a pending
   Choice, so they cannot turn into Empty or a lower-ranked "plausible" alternative. This is good.
   Guards (Gt before Ln/Sqrt) let the callback emit the specific InvalidEpistemicPolicy code first.
   Add one test of Ln(0) inside a Choice, to prove no backtracking.

1. MINOR - The reach rule is sound for provenance but proves nothing about currency or source. It
   guarantees: the cell predates the mark, is a caller cell, and is reached from the pin named by
   `pinned_root`; `target` can only follow to a pin; and owned copies, scratch and cross-root cells
   refuse (tests 609/588). It cannot make the selection "the current reading of source X". A reached
   cell can be a retired observation, another source's observation, or one with a stale epoch,
   because those all hang off the same clause pin. Staleness and source identity must therefore be
   enforced by Vortex logic reading the retired/source/epoch fields (I3 data) before Point, plus the
   I2 token/epoch invalidation at the outer layer. Mark this explicitly in the I4 design. It is not
   an ABI hole; a stale export is possible only if the graph forgets the filter. Two small notes: a
   cell reachable from two pins is recorded under whichever root reached it first, so a result
   naming the second root refuses (fail-closed, but a false refusal); and `rootOf` is O(pins) and
   charged.

1. MINOR - Test coverage. The 7 tests (select and read, laundering, owned copy, missing field,
   unassigned/wrong kind, verifier arity) do not cover: non-finite Deref, an accumulator loop, mixed
   Int/Float, the reach-size limit, nested scope with parent reach, trail restoration of
   reach/cursor after Fail inside Choice, or a field-selector scan with many fields. Items 2, 3 and
   5 should each gain one.

## I2 gate condition for starting I4 design

Partially met. The result/refusal channel, cleanup and publication safety, the typed refusal
separation from Empty (finding 6), the pin/reach laundering defence, and fail-closed Deref are
sound. Not yet met: findings 1-3 are ABI questions I4 depends on (multi-result shape, the reach
budget, and demonstrated float accumulation) and must be settled in I2, since the doc says I4's
result ABI is frozen after this gate. Deferrable to I4 or later: list-append primitive, Float
literals, Int-to-Float opcode, finer error mapping, reach multi-root.

Tally: BLOCKING 0, MAJOR 3, MINOR 5.

VERDICT: FAIL
