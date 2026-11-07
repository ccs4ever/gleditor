#ifndef COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

/// Whether a path step is one literal, read-only dimension traversal.
[[nodiscard]] bool isPlainClosedFunctorStep(const PathStep &step) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
