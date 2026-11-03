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
// ScopeStamp on every evaluated Expr) and checked; a stage's diagnostics end
// the pipeline, and the parser's single Diagnostic arrives as a one-element
// vector so callers see one error stream.
//
// Phase 0 ships parse → resolve → check: the expand stage is sequenced
// under its final name in frontend.cpp — expand.cpp lands the skeleton in
// phase 1 and the real Program → Program composition stage in phase 2, and
// per-literal resolve seeding for code"…" bodies lands with it.
[[nodiscard]] std::expected<Program, std::vector<Diagnostic>>
compile(std::string_view source, FrontendLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_FRONTEND_HPP
