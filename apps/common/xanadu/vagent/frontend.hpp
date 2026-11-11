#ifndef COMMON_XANADU_VAGENT_FRONTEND_HPP
#define COMMON_XANADU_VAGENT_FRONTEND_HPP

#include <expected>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/expand.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/resolve.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

struct FrontendLimits {
  ParseLimits parse{};
  ResolveLimits resolve{};
  CheckLimits check{};
};

// Everything stage 4 leaves for phase 3's emission half: the merged,
// checked Program; the expand stage's fragment table, per-literal
// metadata, capture records and synthesized bindings; and the
// checker's per-fragment info table (result type, purity, closed,
// arity — the fields the fragment descriptor carries). A
// `FragmentContext` is built from the Expansion/FragmentInfoMap
// members directly.
struct CompileArtifacts {
  Program program;
  Expansion expansion;
  FragmentInfoMap fragmentInfo;
};

// The §5.6 pipeline driver (design/vagent-metaprogramming.md) — parse →
// resolve → expand → check — owning invocation order for one standalone
// source unit. The returned Program is parsed, resolve-stamped (a
// ScopeStamp on every evaluated Expr), expanded and checked; a stage's
// diagnostics end the pipeline, and the parser's single Diagnostic
// arrives as a one-element vector so callers see one error stream.
[[nodiscard]] std::expected<Program, std::vector<Diagnostic>>
compile(std::string_view source, FrontendLimits limits = {});

// Same pipeline, plus the side tables phase-3 emission consumes:
// Expansion::fragments/fragmentMeta/captures/synthesized and the
// per-fragment FragmentInfo check() computes.
[[nodiscard]] std::expected<CompileArtifacts, std::vector<Diagnostic>>
compileArtifacts(std::string_view source, FrontendLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_FRONTEND_HPP
