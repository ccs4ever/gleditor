# R1 disposition — vql-lens-bridge v1

All six critics NOT CONVERGED; strong consensus, all ACCEPTED:

- Pipe syntax `path | gen` doesn't exist (ergo/codebase B1, writer M1,
  architect B2): `|` is union-type-only -> v2 replaced the whole
  mutation model with STEP FUNCTIONS (path/fn(...) — the existing
  FunctionInvocation dispatch; link(...) is the mutating precedent).
  No new grammar; step names stay unreserved Identifier-steps.
- Engine-owned sessions_ dies per-query (all six): D2 -> sessions on
  the coordinator's registry; engine accesses it; LensRepl exposes
  session() so :lens/lens() share recency.
- VQL's own arena writes unguarded (purist B1 — NEW): new D7 —
  performCreates/PathMorphism/cloneTail(d.clone, unmanaged)/shape-
  Projection/executeWatch/perspective clones all link() on session-
  owned cells; sessionFor guard at every link-writing site (Q8).
- Sole-anchor enforcement mechanism (architect M1, purist M1, ergo
  B3): two-layered — parser/type-contract static rule (top-level
  statement path only) + runtime backstop (nested-eval flag/context-
  Cells); lens() classified session-mutating; mutating/exempt sets
  enumerated.
- sessionBrowse doesn't exist (codebase M1, architect M2, writer,
  purist M2): Q6 adds session_ops::sessionBrowseAt (the mountedLens/
  SqlLens/isBrowsed/browse/refresh sequence moved out of vlace's
  runIO inline).
- NULL unrepresentable (writer M3, codebase M3, architect M3):
  CellValue={string,double,int64_t,bool} -> empty-yield model.
- meta: refusal is a choice not a gap (architect M4): engine holds
  coordinator_; borrowed fold must outlive session — restated.
- = on cells compares RENDERS not type error (all): cellEq's real
  motivation.
- evaluateValueExpr renders cells: cell args via cellsOfValueExpr.
- ?fn() PredicateTest::FunctionInvocation dead arm: Q2 dispatch.
- $lens binds roots().front(): vector setVariable overload.
- sessionUnmountAt yields expected<void>: mountBoundary->cellFor
  before unmount.
- lensnext/lensstub scoping: cell->keyFor->mountBoundary->scope;
  nullopt=session-level.
- Multi-cell args: per-cell map semantics.
- lensclose deferred marked; union: semantics; attr-cell no-key;
  register column P->Q; executeScriptBody/script model; literal cell
  refs impossible (step functions take the relation, not an arg);
  KwLens whitelist entry named; cite drift; tests row Q7; --engine
  vortex Q5.
