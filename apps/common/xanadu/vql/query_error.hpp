// Typed query failures shared by the compiler, the executor and VQLEngine.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace xanadu::vql {

/// Why a query did not produce cells. This is the one category enum for the
/// compiler (CompilationResult), the executor and VQLEngine::tryExecute.
enum class QueryErrorCategory : std::uint8_t {
  Parse,        ///< source text is not VQL
  Malformed,    ///< well-formed syntax that the caller got wrong (arguments)
  Unsupported,  ///< a feature the Vortex lowering does not cover yet
  Unknown,      ///< a name (function, generator) that is not defined
  TypeContract, ///< a type or contract violation
  Runtime,      ///< the VM or a store failed while running the program
  Budget,       ///< a work, cycle or depth limit was exhausted
  /// Pre-typed spelling of "the compiler refused this query": every
  /// compile-time refusal is Unsupported unless a throw site says otherwise.
  Compile = Unsupported,
};

struct QueryError {
  using Category = QueryErrorCategory;

  Category category{Category::Runtime};
  std::string message;
};

/// A refusal that is not a malformed request: Unsupported, Unknown, Runtime
/// or Budget. Derives from runtime_error so callers can tell it apart from
/// std::invalid_argument, which is reserved for Malformed source.
class QueryRefusal : public std::runtime_error {
public:
  QueryRefusal(QueryErrorCategory category, const std::string &message)
      : std::runtime_error(message), category_(category) {}

  [[nodiscard]] QueryErrorCategory category() const noexcept {
    return category_;
  }

private:
  QueryErrorCategory category_;
};

} // namespace xanadu::vql
