#include "common/xanadu/vagent/frontend.hpp"

#include <utility>

#include "common/xanadu/vagent/expand.hpp"

namespace xanadu::vagent {

std::expected<CompileArtifacts, std::vector<Diagnostic>>
compileArtifacts(std::string_view source, FrontendLimits limits) {
  // Stage 1 — parse the whole unit.
  Parser parser(source, limits.parse);
  auto program = parser.parseProgram();
  if (!program) return std::unexpected(std::vector{program.error()});
  // Stage 2 — resolve: stamp every evaluated Identifier's binding identity
  // (scope id plus binding ordinal), one scope truth shared with the
  // checker's env walk. `seeds` collects each code"…" literal's env
  // snapshot and mint watermarks for stage 3's per-literal resume.
  LiteralSeeds seeds;
  auto resolved = resolve(*program, &seeds, limits.resolve);
  if (!resolved) return std::unexpected(std::move(resolved.error()));
  // Stage 3 — expand: the phase-1 skeleton parses and narrows each
  // code"…" body into the fragment table and refuses residual
  // Splice/Static nodes with the named DiagnosticCode::Unsupported;
  // phase 2 adds the composed-tree rewriting.
  auto expanded =
      expand(*program, seeds, {limits.parse, limits.resolve, limits.check});
  if (!expanded) return std::unexpected(std::move(expanded.error()));
  // §5.3's env records ride the fragment table into check(): expand's
  // capture set is program-wide (one cap$N alias per captured binding,
  // aliases reused across literals), so each record's env names every
  // capture — an alias only ever appears where the rename placed it.
  for (auto &entry : expanded->fragments)
    for (const auto &[alias, capture] : expanded->captures)
      entry.second.env[alias] = capture.captured;
  // Stage 4 — check the merged tree once, with the fragment table the
  // expand stage produced so a record-less literal is a named refusal.
  // `info` collects the per-fragment result/purity/closed/arity table
  // phase 3's descriptors carry.
  FragmentInfoMap info;
  auto checked = check(*program, expanded->fragments, limits.check, &info);
  if (!checked) return std::unexpected(std::move(checked.error()));
  return CompileArtifacts{std::move(*program), std::move(*expanded),
                          std::move(info)};
}

std::expected<Program, std::vector<Diagnostic>> compile(std::string_view source,
                                                        FrontendLimits limits) {
  auto artifacts = compileArtifacts(source, limits);
  if (!artifacts) return std::unexpected(std::move(artifacts.error()));
  return std::move(artifacts->program);
}

} // namespace xanadu::vagent
