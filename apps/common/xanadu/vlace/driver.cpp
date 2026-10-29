/**
 * @file driver.cpp
 * @brief Implementation of the vlace driver.
 */
#include "common/xanadu/vlace/driver.hpp"

#include <format>
#include <istream>
#include <ostream>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/types.hpp"

namespace xanadu::vlace {

std::string Driver::formatError(const DriverError &e) {
  return std::format("vlace: {}:{}: {}: {}", e.loc.line, e.loc.column,
                     compileErrorName(e.code), e.detail);
}

std::expected<DriverResult, DriverError> Driver::run(std::string_view source) {
  // 1. Parse.
  Parser parser(source);
  auto prog = parser.parseProgram();
  if (!prog) {
    const auto &pe = prog.error();
    return std::unexpected(DriverError{
        .code = pe.code, .detail = pe.detail, .loc = pe.loc, .phase = "parse"});
  }

  DriverResult result;

  // 2. Type-check (normative): reject an ill-typed program before evaluation.
  if (typeCheck_) {
    TypeChecker tc;
    auto t = tc.checkProgram(*prog);
    if (!t) {
      const auto &te = t.error();
      return std::unexpected(DriverError{.code   = te.code,
                                         .detail = te.detail,
                                         .loc    = te.loc,
                                         .phase  = "type"});
    }
    result.type = *t;
  }

  return std::unexpected(
      DriverError{.code   = CompileError::UnexpectedToken,
                  .detail = "VM lowering is not implemented for VLace programs",
                  .phase  = "compile"});
}

std::expected<DriverResult, DriverError>
Driver::runIO(std::string_view source, std::istream &in, std::ostream &out) {
  // 1. Parse.
  Parser parser(source);
  auto prog = parser.parseProgram();
  if (!prog) {
    const auto &pe = prog.error();
    return std::unexpected(DriverError{
        .code = pe.code, .detail = pe.detail, .loc = pe.loc, .phase = "parse"});
  }

  DriverResult result;

  // 2. Type-check (normative).
  if (typeCheck_) {
    TypeChecker tc;
    auto t = tc.checkProgram(*prog);
    if (!t) {
      const auto &te = t.error();
      return std::unexpected(DriverError{.code   = te.code,
                                         .detail = te.detail,
                                         .loc    = te.loc,
                                         .phase  = "type"});
    }
    result.type = *t;
  }

  static_cast<void>(in);
  static_cast<void>(out);
  return std::unexpected(DriverError{
      .code   = CompileError::UnexpectedToken,
      .detail = "VM lowering is not implemented for VLace IO programs",
      .phase  = "compile"});
}

} // namespace xanadu::vlace
