/**
 * @file driver.hpp
 * @brief The vlace driver: parse -> type-check -> VM compilation gate.
 */
#ifndef COMMON_XANADU_VLACE_DRIVER_HPP
#define COMMON_XANADU_VLACE_DRIVER_HPP

#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

#include "common/xanadu/vlace/ast.hpp"

namespace xanadu::vlace {

struct DriverError {
  CompileError code{CompileError::UnexpectedToken};
  std::string detail;
  SourceLocation loc{};
  std::string phase; ///< "parse" | "type" | "eval"
};

struct DriverResult {
  std::string type;  ///< inferred type of the result (empty if no result expr)
  std::string value; ///< rendered value
};

class Driver {
public:
  Driver() = default;

  /// Enable/disable the normative type-check pass. Chainable (AGENTS.md).
  Driver *withTypeChecking(bool on) noexcept {
    typeCheck_ = on;
    return this;
  }

  /// Parse and optionally type-check; execution refuses pending VM lowering.
  [[nodiscard]] std::expected<DriverResult, DriverError>
  run(std::string_view source);

  /// Parse and optionally type-check; IO execution refuses pending VM lowering.
  [[nodiscard]] std::expected<DriverResult, DriverError>
  runIO(std::string_view source, std::istream &in, std::ostream &out);

  /// Render a DriverError as "vlace: line:col: Code: detail" (§8.3).
  [[nodiscard]] static std::string formatError(const DriverError &e);

private:
  bool typeCheck_{true};
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_DRIVER_HPP
