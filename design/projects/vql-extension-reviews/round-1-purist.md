# Purist critique, round 1

Current working tree, VQL 16.1; the earlier 732b8280 report is historical. The
subject/field/observation/occurrence model, retention/R8 boundary, redacted-derivative distinction,
and private action histories now incorporate the prior conceptual findings. Three actionable
findings remain. Reproducers below ran successfully against the current `build/vquery`, headlessly
with the six repository isolation variables; neither persists or dispatches external work.

## P1 — Medium: bulk shape assignment silently drops earlier relationships (implemented source bug)

```vql
let $subjects := ##/d.subject%"A"%"B"
let $target := ##/d.target%"shared"
let $edited := apply_shape($subjects, {*, +d.edge: $target})
return count($subjects/+d.edge)
```

Observed exit 0, output `1`: A's new relationship disappears when B claims the target's negward
slot. `applyShape` precomputes assignments (`vql_engine.cpp:2188–2193`) but then calls raw
`arena.link` for each (`:2209`) without checking assignment-to-assignment slot collisions. The
operation returns both subjects as if both edits succeeded. This bypasses existing-target fan-out's
explicit clone policy and undermines the new relationship-occurrence model.

**Decision/fix:** preflight all endpoint slots and reject incompatible assignments before masking;
alternatively require explicit occurrence/facet construction or apply a documented fan-out policy.
Do not silently attach only the last subject. Test common target, negward assignments, and
pre-existing target occupancy. Whether single explicit reassignment intentionally displaces a prior
edge can remain separately documented.

## P2 — High: nested assignment fabricates Boolean content

```vql
let $s := ##/d.subject%"subject"
let $edited := apply_shape($s, {+d.outer: {+d.inner: "expected"}})
return $s/+d.outer
```

Observed exit 0, output `true`; no `d.inner` structure is constructed. `evaluateValueExpr` reduces
every ShapeLiteral to boolean true (`vql_engine.cpp:1996`); `applyShape` invokes it through
`cellsOfValueExpr` when resolving RHS targets. This contradicts §4.8.3's explicit promise that
nested/dynamic unsupported forms refuse. It manufactures plausible success and can sever the
previous neighborhood before installing the wrong target.

**Fix:** recursively validate RHS supported value forms before resolving assignments or editing.
Refuse nested shape values until construction semantics exist. Add a test preserving original
reciprocal links on refusal, including unsupported shape syntax hidden inside a function result if
such values are supported.

## P3 — Medium: proposed cross-store clone model contradicts private authority

VQL §4.9.4 still depicts P linked to both Alice and Bob directly on `d.clone`, claims ordinary
`Manifold::cloneMaster()` resolves these private cross-store cells, and promises `~d.clone`
discovers all commentary. Two direct links exceed rank valence; a reciprocal global rank
additionally requires edits outside each observer's private store. Private commentary cannot be
universally discoverable (§workflow 7).

**Fix/decision:** specify observer-local proxy/occurrence ranks with stable external references to
the pinned/shared subject; identify which authority owns each reciprocal edge. Discover published
accessible references with coverage. Do not present a composite local view as a single globally
mutable clone rank.

## Implemented response and validation

P1 now preflights both directed endpoint slots for every requested assignment before masks or links
change; repeated identical claims remain valid. P2 recursively rejects literal shape values in
function arguments before assignment target evaluation, and shape-as-scalar evaluation now refuses
globally instead of fabricating boolean true (including user functions returning shape literals).
Shape-consuming builtin arguments remain valid. The empty-shape corpus was updated from an unused
fabricated boolean binding to an actual empty snapshot/shape-predicate check.

Added regressions cover shared-target collisions in both directions, source reciprocal-link
preservation on refusal, identical duplicate requests, nested RHS forms before earlier target
allocations, shape-returning function refusal, and valid project-valued assignment.
