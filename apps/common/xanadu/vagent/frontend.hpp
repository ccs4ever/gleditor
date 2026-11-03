#ifndef COMMON_XANADU_VAGENT_FRONTEND_HPP
#define COMMON_XANADU_VAGENT_FRONTEND_HPP

#include <expected>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/resolve.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

struct FrontendLimits {
  ParseLimits parse{};
  ResolveLimits resolve{};
  CheckLimits check{};
};

// The §5.6 pipeline driver (design/vagent-metaprogramming.md) — parse →
// resolve → expand → check — owning invocation order for one standalone
// source unit. The returned Program is parsed, resolve-stamped (a
// ScopeStamp on every evaluated Expr), expanded and checked; a stage's
// diagnostics end the pipeline, and the parser's single Diagnostic
// arrives as a one-element vector so callers see one error stream.
//
// Phase 1 runs the expand skeleton: literal bodies are parsed and
// seed-resolved, splice/static are uniformly refused, and the fragment
// table expand emits flows into check(program, table). Phase 2 fills the
// real Program → Program composition stage.
[[nodiscard]] std::expected<Program, std::vector<Diagnostic>>
compile(std::string_view source, FrontendLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_FRONTEND_HPP
