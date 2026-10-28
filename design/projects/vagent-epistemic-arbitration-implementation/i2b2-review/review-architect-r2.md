# I2B-2 architect/governance review, round 2

Inputs: response-r1.md, i2b2-r2.diff, revised "Structural reads" in the ABI doc.

## Owner decision (M1, excluded from verdict)

What the owner should approve or refuse, exactly: that inside `ScopedEvaluate` callbacks only, the
existing `OpcodeKind::Point`, `Link` and `Deref` carry typed read-only meanings (Link as 2-in/2-out
with a runtime `Str` selector, a different arity from the 4-in Link the vagent compiler emits
elsewhere), together with the binding-only `Cursor` kind and the per-frame pin-reach grant. Approve
= this reading of "typed evaluation frame/reference operands" in the P19.8 register. Refuse =
replace with new enum entries (each a core change needing its own approval) or descriptor-declared
views. No other core surface changes; the mesh allowlist (mesh_evaluator.cpp:247) still excludes
ScopedEvaluate.

## Re-review

1. M2 (maintainability of overloading) - RESOLVED in the doc, partially in tests. The doc now has a
   refused- alternatives paragraph (new opcodes, Vortex library over writing `Link`, descriptor
   views that cannot express a runtime-named profile lookup) and a why-C++ justification (live frame
   ownership/assignment/pin provenance). Those arguments are sound; descriptor views are fairly
   refused because source names are runtime data. Residual, MINOR: I asked for a cross-decoder test
   (VQL/VPL name tables, mesh allowlist) and a single shared predicate. The diff has a file-local
   `structural()` and no such audit test. The confinement claim rests on the early dispatch in
   `stepScope` and verification. Acceptable given nothing else executes scope graphs, but record it
   as a follow-up for when a second consumer appears.

1. M3 (I4 coverage) - RESOLVED. `BestOfCandidatesWithProfileLookupAndAccumulator` exercises exactly
   the shape asked for: keyed profile lookup by runtime source name, Int-to-Float promotion, a
   rewritten Float accumulator, max selection and PinnedRef export. The ranked/ties paragraph cites
   spec 6.2 (owner-held generator state, one scope per answer, `truest` fails on a tie), which
   answers the generator-state concern without a result-list append; I accept that. Scratch
   accumulation is demonstrated by rewriting results/temps. One caveat, MINOR: stable observation
   IDs are still read only via Deref of a field, and the test should be confirmed to assert the ID
   round-trip to a selection token (response does not say). Worth one line in the I4 plan.

1. Selector strings (MINOR) - RESOLVED enough. `.name` prefix removes shadowing, unknown words
   refuse TypeMismatch instead of reading as missing, and field comparison is charged by length
   before bytes. Still runtime Str rather than a dimension cell, but it is bounded, verified at step
   time, and the doc argues why a runtime name is required for profile lookup. Not a smell worth
   blocking.

1. Reach budget magic reuse (MINOR) - RESOLVED: reach entries are charged against the shared
   traversal work grant instead of borrowing `limits.fields`. Multimap attribution to every root
   removes the order-dependence and has a test that fails under first-writer-wins.

1. Doc/code mismatches (MINOR) - RESOLVED: the doc now says callers branch on the found flag, states
   the `target` not-found behaviour, non-finite Float refusal, and that numeric faults refuse even
   under a choice (tested). The "arg." property for positions remains a verifier-time check only;
   acceptable.

1. Layering/purity - unchanged and fine. Duplicated verifier/runtime checks remain (arity re-check
   added); defence in depth, no action.

## Tally

BLOCKING 0, MAJOR 0 (excluding owner decision M1), MINOR 2 (shared predicate / cross-decoder audit
deferred; confirm stable-ID round-trip assertion in the best-of-N test).

VERDICT: PASS
