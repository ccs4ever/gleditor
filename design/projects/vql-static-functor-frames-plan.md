# Decorated static functors in reusable VQL frames

Iteration 54 is committed as `b12ba35d`. Iteration 55 implements the next combined named-static
functor package. The implementation contract below is followed by the stage validation record.

## Semantics and scope

Static functors preserve three lifetimes. Nested static step bodies freeze when bound; literals
belong to structural specialization; ordinary predicate aliases resolve in application/callee
lexical scope. The threshold characterization (creation 1, caller 9, callee 5 selecting 7 and 11)
remains a regression. Value-lambda creation captures retain their separate semantics.

The package covers graph-supported computed scalar predicates and nested Boolean expressions,
existing literal/nested shape predicates, and nested frozen static step vectors in reusable named
function graphs. Historical pins, ordered projections, computed shape targets and a new ABI for
static functors captured by value lambdas remain outside this stage. Unsupported cases retain
explicit finite-inline or recursive refusal behavior; there is no runtime AST evaluation.

## Compilation and bounds

A bounded normalizer creates a deterministic structural key, a compile-only owned step payload,
symbolic alias uses and scoped named-helper dependencies. Keys encode accepted node tags, string
lengths/bytes, exact scalar type/bits, ordered Boolean terms/factors, direction/modifier/clamp
fields, shape rules and captured body/decorator boundaries. The outer payload owns shared read-only
nested AST leaves during synchronous compilation. Keys never contain resolved alias values or AST
pointer identities. Malformed/cyclic input and exhausted limits are hard errors before payload
copying or frame reservation. Unsupported syntax is distinct from malformed input.

Each named frame variant adds deterministic Stream capture ports for template-only alias
dependencies and transitive helper dependencies. Captures bind before ordinary formals and locals,
which retain their normal lexical precedence at each application. Forwarding sources the current
frame's lexical bindings while reusing the same structural program key. Required field-default
aliases keep pre-match scope and missing-versus-bound-empty checks. Free caller static templates
that cannot be transported safely retain explicit inline boundaries.

The shared scope walker must treat a captured static body as replacing its selector, then inspect
outer decorations. The replaced dollar-selector must not become a false free alias. Borrowed AST
inspection, payload copies, key bytes and repeated variant work need explicit bounds in addition to
existing frame-variant, opcode, descriptor and runtime limits. No total CPU/heap or speedup claim is
implied. Runtime descriptors contain graph references only; ordinary frame export relocates them.

## Ownership and validation

The Luna architecture track owns the normalizer and shared scope analysis. The Luna compiler track
owns variant keys/state, alias ports and graph integration. The root owns regression fixtures and
this record. All source writers freeze before clang-format 19 and every full `make -j$(nproc)`.
Executables run headlessly with all three XDG directories isolated.

Acceptance includes late alias scope and shadowing, distinct structural keys, recursive/mutual
forwarding without variant growth from changing values, nested frozen-body rebinding, ordered
duplicates and decoration/clamp order, exact numeric types, graph-supported shapes, malformed AST
and work/variant refusals with retry, fresh-VM export and CLI/host/mesh SOURCE routes. Independent
architecture, senior and resource gates follow execution validation. Full §4.12 remains open.

## Iteration 55 validation

Full `make -j$(nproc)` passed, all 1,157 affected tests across 104 suites passed, and all 52
expanded focused tests across five suites passed, including the 24 new cases. Export/reload and
host/mesh SOURCE routes execute the recursive computed-predicate graph; the real headless CLI
returns `11`. Independent Luna architecture, senior and resource reviews accepted the bounded stage.
Repository formatting uses clang-format 19.

The shared capture helper now supports borrowed lookups and incrementally charged inspection/copy
work before owned materialization. A separate `FrameEligibilityLimits.maxWork` default of 1,000,000
meters scope metadata without changing the existing node/event or depth counters. Generic program
phases consume the configured functor-template work ceiling; allocator overhead, hashing and total
CPU/heap remain outside that estimate.

Non-closed static formal defaults retain their existing eligibility boundary. Computed shape
targets, ordered projections, historical pins, value-lambda static capture ABI and remaining
completion gates stay open. The broader audits retain seven callable failures and eight route
failures plus one skip. These results are not a full-suite pass or §4.12 completion.
