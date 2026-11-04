#ifndef COMMON_XANADU_VAGENT_CHECKER_HPP
#define COMMON_XANADU_VAGENT_CHECKER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"

namespace xanadu::vagent {

struct CheckLimits {
  // Flat inheritance is independent of parser nesting. Count graph visits
  // and copied interface entries so a valid-shaped DAG cannot exhaust work.
  std::size_t maxInheritanceWork{1'000'000};
  // Purity propagation may revisit an otherwise small AST across passes.
  std::size_t maxPurityWork{1'000'000};
  // Symbol lookups, uncached scans and copied owner-scope entries.
  std::size_t maxResolutionWork{1'000'000};
  // Public AST callers do not necessarily pass through the parser's limits.
  std::size_t maxExpressionDepth{256};
  std::size_t maxExpressionNodes{1'000'000};
  std::size_t maxDeclarationDepth{256};
  std::size_t maxTypeDepth{256};
  // Includes declarations, expressions and type nodes in the preflight.
  std::size_t maxAstNodes{1'000'000};
  // §5.6 per-compile expansion counters (design/vagent-metaprogramming.md):
  // each nested code"…" body spawns a fresh Parser whose own limits would
  // not accumulate, so literal nesting and substitution totals live here
  // and the expand stage threads them through its per-literal work.
  std::size_t maxExpansionDepth{64};
  std::size_t maxSplices{10'000};
};

// One fragment-table entry: expand's per-literal parseCodeBody output
// (the optional '#' label, the optional CodeParams '|' header, and the
// body's parsed AST, all locs literal-relative) plus the outside-reference
// classification expand computes per §5.6 — `closed` is set when every
// outside reference the body makes is state-side (cap$N captures, ref-box
// slots) or name-resolvable in the program; a program-side ref carrying
// neither is the refused class that leaves it clear (spec :158).
struct FragmentRecord {
  CodeBody body;
  bool closed{false};
  // §5.3's env records at the literal's position: each cap$N alias minted
  // in the body maps to the name it captures. The checker seeds each
  // alias into the body env as a read-only, unvalued binding carrying the
  // captured binding's own type — the env record is the materialized
  // content of the stamp the renamed Identifier carries — and renders
  // the alias in diagnostics back as the captured name.
  std::map<std::string, std::string, std::less<>> env;
};

// The §5.6 fragment table: one FragmentRecord per code"…" literal, keyed
// by the literal's FragmentKey so cloneExpr'd copies share the record.
// check() computes the checked result type T, the body's purity, and the
// declared arity from a record — the per-literal data §7.1's fragment
// tables and, later, the phase-3 fragment descriptor read. A null body
// means the literal's body failed to parse; expand already reported it,
// so check() skips it quietly.
using FragmentTable = std::map<FragmentKey, FragmentRecord>;

// The phase-1 signature (r1-disposition I3): the fragment table produced by
// expand lands with its consumer — a Code node whose key has no record is a
// named refusal, because a table-less check() can only mean the pipeline
// skipped expand.
[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
check(const Program &program, const FragmentTable &fragments,
      CheckLimits limits = {});

// The un-stamped/records-absent entry point. check() is a public API whose
// existing callers parse but never resolve or expand, so the env walk
// covers absent resolve stamps (the F2 env-walk fallback); a Code node with
// no matching record is still a named refusal.
[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
check(const Program &program, CheckLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_CHECKER_HPP
