# R2 disposition — vql-lens-bridge v2.2/v2.3

Five critics reported (all NOT CONVERGED; zigzag purist pending —
folded when it lands). Consensus on mechanism gaps, all ACCEPTED:

- Zero-arg gestures need () (writer M1, codebase B1, vortex M1, ergo
  B1): bare path/lensmount parses as SignedDimensionStep -> mints a
  d.lensmount dim + silent empty. All zero-arg gestures carry ().
- contextCells-nonempty backstop unsound (vortex M2, architect m2,
  ergo M1): nested evals pass {} context -> evaluation-depth/
  topLevelStatement flag only.
- D7 omits link() step itself (vortex M4, architect M1, ergo M2):
  link(...) writes posward on currentCells; cloneGenerator appends
  d.clone on the target; performCreates checks attachPoint too.
- D7's guard had no register row (architect M2, writer M2, codebase
  M1): Q8 added.
- lensbrowse under-specified (architect M3, vortex m4, ergo m5):
  sessionBrowseAt(session,key,table) with mount-scoped routing;
  browse->expected<void> so the yield needs a SqlLens root accessor;
  session_ops.cpp keeps SqlLens out of the header.
- $lens rebinds wrong session (vortex M3): registry's most recent
  open; # literal anchors reach any cell across lines.
- Registry element type (writer M3, architect m1): vector<shared_ptr>
  (unique_ptr can't hold the non-owning LensRepl join).
- lensmount yields roots().front() only (architect m4): single cell.
- Session-scoped ops dedupe (vortex m2): one commit/browse per
  session, not per cell.
- ?fn() dead arm (vortex m3, architect m3, codebase M2, ergo m1):
  the FunctionInvocation arm is parser dead code; ?fn() produces
  PredicateTest::ValueExpr -> fix is evaluateValueExpr's fn table
  for ALL names + mutating-name refusal.
- cellEq cardinality: N!=1 -> TypeContractError.
- let/flat-for-in single-eval allowed (ergo m4, architect m5):
  bindLet/inPath evaluate exactly once; nested-for in-paths banned.
- value() already reads scalar bits (ergo m2): cellScalar's novelty
  is arg-position kind visibility.
- d.contains/d.seq drops firstborn (vortex R1 M4): /d.seq::rank
  re-seeks the head; every all-children traversal uses it.
- shared_ptr registry (vortex M3); dirty-drops-on-session-death;
  refusal-invisibility vs executeScriptBody; sessionMounts_
  vocabulary; lens* dispatch before silent-home fallback;
  resolveAnchor :846 cite; ref-aliasing order-dependence;
  casing lowercase; ast.hpp:176/shapeProjection/main.cpp cite fixes;
  vql/vlace compiler.cpp disambiguation; Q4-vs-Q5 cross-ref.

## Ergo R3 (7686a502) — folded into v2.6

- let/for self-contradiction (B1): reconciled — single-eval let/for-in
  targets allowed iff they bind CELLS at bind time; `let $f :=`
  functor-bound paths banned; `:=` not `=`; everywhere else refused.
- Wrong dispatch table (B2): comparison operands route through
  cellsOfValueExpr -> evaluateStep, not evaluateValueExpr; the probes
  register in BOTH the step arm and the value arm (contains/count/
  delta precedent) + mutating-name refusal in both.
- same_cell (M1): does NOT exist in this worktree — the critic read
  the main tree; cellEq remains needed.
- M3 wrong-tree read: projection_session.cpp is 556 lines here (not
  114); session_ops.hpp 175 lines exists; main.cpp:526 IS
  setVariable("lens"). Cite corrections from main discarded.
- "at most one mutating step per path; reads may follow" rule.
- # is Root not LiteralCellId (bare IntegerLiteral); let uses :=;
  path/lensmount mints `lensmount` not `d.lensmount`; dedupe arg
  variance first-wins; prose parens drift.
