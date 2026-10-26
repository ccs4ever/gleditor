#ifndef COMMON_XANADU_VAGENT_CHECKER_HPP
#define COMMON_XANADU_VAGENT_CHECKER_HPP

#include <cstddef>
#include <expected>
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
};

[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
check(const Program &program, CheckLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_CHECKER_HPP
