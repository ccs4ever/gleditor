#ifndef COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

/// Whether a path step is a literal, read-only dimension traversal, with an
/// optional static range clamp.
[[nodiscard]] bool isClosedTraversalFunctorStep(const PathStep &step) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
