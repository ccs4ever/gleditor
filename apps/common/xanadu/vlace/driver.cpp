/**
 * @file driver.cpp
 * @brief Implementation of the vlace driver.
 */
#include "common/xanadu/vlace/driver.hpp"

#include <format>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/types.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

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

  // 3. Evaluate over a fresh arena and render.
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  auto v = compiler.evaluateProgram(*prog);
  if (!v) {
    const auto &ve = v.error();
    return std::unexpected(DriverError{
        .code = ve.code, .detail = ve.detail, .loc = ve.loc, .phase = "eval"});
  }
  result.value = compiler.render(*v);
  return result;
}

} // namespace xanadu::vlace
