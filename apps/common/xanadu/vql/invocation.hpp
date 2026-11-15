#pragma once

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/query_error.hpp"

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

[[noreturn]] inline void
throwUnresolvedFunction(const FunctionInvocation &invocation,
                        const QueryErrorCategory category) {
  throw QueryRefusal(category, "VQL function '" + invocation.name + "' with " +
                                   std::to_string(invocation.args.size()) +
                                   " argument(s) is not compiled to Vortex");
}

} // namespace xanadu::vql
