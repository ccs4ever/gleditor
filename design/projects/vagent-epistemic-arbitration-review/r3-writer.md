# Round 3 critique: Technical Writer (epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to the repository root (`/data/git/gleditor`). The v1.2 file is dirty in
the supplied worktree and was reviewed in place; no repository files were edited. Tally: 1 BLOCKING,
1 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2 writer produced no report due quota, per `r2-disposition.md`; there is no R2 writer verdict.
- R1.1 (worked plan declarations do not parse): OPEN. All four v1.2 examples still use typed goal
  arguments and a `body` label.
- R1.2 (`#Exp` absent): RESOLVED by adding P19.7.
- R1.3 (missing query/directive grammar): RESOLVED in §4.1 and §4.3.
- R1.4 (weights serialization vs scalar cells): RESOLVED in §2.1–2.3.
- R1.5–R1.8 (composite weight, context, calibration topology, double feedback): substantially
  addressed in v1.2; R1.9's native model interface gap remains open (below).
- R1.11 (no exact source citations): PARTLY. Some `file:line` citations were added, but several
  assertions still cite files absent from the current checkout, and the P17/P18 companion links do
  not resolve in this tree.

## BLOCKING

### 1. Worked plan declarations still violate the grammar

**Full title and location:** 1. (§7.1–§7.4, lines 936–938, 970–972, 998–1000, 1025–1027) Worked plan
declarations still violate the grammar

**Problem:** Each worked plan declares typed arguments in the goal call (for example
`reach_waypoint(dest_x: Float, dest_y: Float)`) and inserts `body {` after its guard. The canonical
production allows only an identifier list for plan bindings and then a block; it has no typed
argument annotations or `body` keyword. The examples therefore fail to parse before exercising any
proposed feature. Example 2 also writes boolean `and`, while the grammar uses `&`.

**Evidence:** The four examples are at §7.1 lines 936–938, §7.2 lines 970–972, §7.3 lines 998–1000,
and §7.4 lines 1025–1027. Current parser `Parser::plan()` reads a goal name, then `identifierList()`
(not typed parameters), optional `where`, and a block (`xanadu/vagent/parser.cpp:470–484`). The
companion grammar is
`PlanDecl ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block`
(`design/vagent-agent-language.md:1312`). Conjunction is `&`
(`design/vagent-agent-language.md:1322`); Example 2 uses `and` at line 965.

**Fix:** Rewrite each example as `plan Name for goal_name(arg, ...) where condition { ... }`, with
argument names untyped and the block immediately following the guard. Change boolean `and` to `&`.
Keep this grammar distinct from the new `believes truest` modifier grammar.

## MAJOR

### 1. Native-model example never produces the queried belief

**Full title and location:** 1. (§7.4, lines 1008–1029; §8.6, lines 1117–1126) Native-model example
never produces the queried belief

**Problem:** `evaluate_defect(...)` returns a `Float`, but the percept handler discards the result.
Nothing asserts or updates `defect_detected`, while the later plan queries that predicate. The
example declares a custom `SceneVerifier` capacity, whereas P19.6 promises an `EpistemicModel`
capacity and defines neither interface nor the handoff into the on-manifold embeddings. Readers
cannot infer a working data path from inference result to arbitration query.

**Evidence:** §7.4 lines 1009–1029 declares and calls `SceneVerifier::evaluate_defect`, then queries
`defect_detected`; P19.6 only says “Define `EpistemicModel` capacity” and populate embeddings (§8.6
lines 1122–1124).

**Fix:** Define the P19.6 capacity signature and result contract, then make the example connect
output to the query: e.g. post a typed percept carrying the embedding/result, persist it through the
stated ingestion path, and show how `defect_detected` is derived or asserted. Alternatively rewrite
the plan to query the actual result.

## MINOR

### 1. (§1.1, line 39) MD013 overflow remains

**Problem:** The sentence describing continuous-domain unification exceeds the configured 100-column
Markdown limit. `mdl -i` reports `MD013` at line 39.

**Fix:** Reflow the sentence at a word boundary. The requested `mdformat --check` could not run in
this environment because the required `gfm`, `dollarmath`, and `frontmatter` extensions are not
installed; this is an environment limitation, not an additional document finding.

### 2. Absolute local-file links and absent companion targets are not portable

**Full title and location:** 2. (Header and §1, lines 5, 8, 17) Absolute local-file links and absent
companion targets are not portable

**Problem:** The document uses `file:///data/git/gleditor/...` links, which only resolve on a
machine with that exact checkout path. The P17/P18 companion Markdown targets are absent from the
current tree, although they exist in repository history (`b1311f56` and `f033eda8` respectively).
This makes the stated prerequisites hard to follow in a fresh checkout.

**Fix:** Use relative links for files present in the tree. For historical-only prerequisites, label
them as unbuilt/historical and link to a stable commit permalink or record the relevant contract
inline; do not imply the documents are current source files.

VERDICT: NOT CONVERGED
