#include "common/xanadu/vagent/frontend.hpp"

#include <utility>

namespace xanadu::vagent {
namespace {

// §5.6 stage 3 — expand, the Program → Program composition stage (parse
// code"…" bodies, cap$N capture aliasing, $-anchor rewriting, splice
// substitution via cloneExpr, Code-splice def expansion, residual-splice
// check, fragment-table emission). Phase 1 moves this seam into
// expand.{hpp,cpp} — keeping the signature and joining
// VAGENT_FRONTEND_SRCS in the same edit — and phase 2 fills the real
// semantics. The identity body is the phase-0 seam, deliberately named so
// the driver is not later read as a stub that forgot a stage; per-literal
// resolve seeding off each literal's ScopeStamp::scope is part of what the
// real stage adds.
std::expected<Program, std::vector<Diagnostic>> expand(Program program) {
  return program;
}

} // namespace

std::expected<Program, std::vector<Diagnostic>> compile(std::string_view source,
                                                        FrontendLimits limits) {
  // Stage 1 — parse the whole unit.
  Parser parser(source, limits.parse);
  auto program = parser.parseProgram();
  if (!program) return std::unexpected(std::vector{program.error()});
  // Stage 2 — resolve: stamp every evaluated Identifier's binding identity
  // (scope id plus binding ordinal), one scope truth shared with the
  // checker's env walk.
  auto resolved = resolve(*program, limits.resolve);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  // Stage 3 — expand: composition over the stamped tree (seam above).
  auto expanded = expand(std::move(*program));
  if (!expanded) return std::unexpected(std::move(expanded.error()));
  // Stage 4 — check the merged tree once.
  auto checked = check(*expanded, limits.check);
  if (!checked) return std::unexpected(std::move(checked.error()));
  return expanded;
}

} // namespace xanadu::vagent
