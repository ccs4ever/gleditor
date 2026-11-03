#ifndef COMMON_XANADU_VAGENT_EXPAND_HPP
#define COMMON_XANADU_VAGENT_EXPAND_HPP

#include <expected>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/resolve.hpp"

namespace xanadu::vagent {

struct ExpandLimits {
  ParseLimits parse{};
  ResolveLimits resolve{};
  // The §5.6 per-compile counters (maxExpansionDepth/maxSplices) live in
  // CheckLimits so the stage and its consumer share one budget type.
  CheckLimits check{};
};

// What the phase-1 skeleton leaves for check(): the §5.6 fragment table —
// one record per code"…" literal, keyed by the literal's FragmentKey.
// Phase 2 adds the composed-tree rewriting; the Program itself is
// transformed in place.
struct Expansion {
  FragmentTable fragments;
};

// §5.6 stage 3 — the phase-1 skeleton (r1-disposition I3): for every
// code"…" literal, parseCodePath parses and narrows its body, the seed
// resolve() snapshot under `seeds` resumes binding-identity resolution
// inside it via resolveFragment, and a record lands in the fragment table
// under the literal's FragmentKey — nested literals through the same
// recursion, seeded by the fresh stamps their enclosing body's seeded
// walk gave them. Every Splice/Static node, program-wide and
// body-recursive, is a named DiagnosticCode::Unsupported refusal so
// check() never meets either kind; diagnostics inside a body surface in
// the src:m:l:c+o:c convention of §5.1.
[[nodiscard]] std::expected<Expansion, std::vector<Diagnostic>>
expand(Program &program, LiteralSeeds &seeds, ExpandLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_EXPAND_HPP
