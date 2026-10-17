/**
 * @file driver.hpp
 * @brief The vlace driver: parse -> type-check (normative) -> evaluate ->
 *        render. One entry point the REPL, the batch runner, and tests share.
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

  /// Run a whole vlace program source: parse, optionally type-check (rejecting
  /// ill-typed programs), evaluate, and render both the type and the value.
  [[nodiscard]] std::expected<DriverResult, DriverError>
  run(std::string_view source);

  /// Run a vlace program as an effectful entry point (§6.5/§8.2): parse,
  /// optionally type-check, evaluate the program to an `IO a` value, then
  /// perform that action at the boundary (`runIO`), reading console input from
  /// `in` and writing output to `out`. The returned DriverResult carries the
  /// program's static type and the rendered final wrapped value `a`. A program
  /// whose result is not an IO action is reported as a type/eval error. This is
  /// the only Driver entry point that performs effects; `run` never does.
  [[nodiscard]] std::expected<DriverResult, DriverError>
  runIO(std::string_view source, std::istream &in, std::ostream &out);

  /// Render a DriverError as "vlace: line:col: Code: detail" (§8.3).
  [[nodiscard]] static std::string formatError(const DriverError &e);

private:
  bool typeCheck_{true};
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_DRIVER_HPP
