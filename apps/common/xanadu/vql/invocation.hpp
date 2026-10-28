#pragma once

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

/// User-defined VQL calls in the bounded compiler foundation are positional.
/// Builtins with named parameters validate their own argument contract.
inline void requirePositionalArguments(const FunctionInvocation &invocation) {
  for (const auto &name : invocation.argNames) {
    if (!name.empty()) {
      throw std::invalid_argument(
          "named arguments are not supported for user-defined function '" +
          invocation.name + "'");
    }
  }
}

} // namespace xanadu::vql
