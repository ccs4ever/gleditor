/**
 * @file projection_syntax.hpp
 * @brief Pure syntactic checks for the bounded native shape-projection form.
 */
#ifndef COMMON_XANADU_VQL_PROJECTION_SYNTAX_HPP
#define COMMON_XANADU_VQL_PROJECTION_SYNTAX_HPP

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

/// Whether a path uses only the read-only selectors supported by a projection
/// target. Binding and arena checks belong to the compiler.
[[nodiscard]] bool isSimpleProjectionPath(const PathExpression &path) noexcept;

/// Checks only the top-level rule syntax. Nested AST validation and bounded
/// traversal remain the caller's responsibility.
[[nodiscard]] bool
isStaticShapeProjectionSyntax(const ShapeLiteral &shape) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_PROJECTION_SYNTAX_HPP
