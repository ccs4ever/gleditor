/**
 * @file compiler.cpp
 * @brief Iteration-1 stub: refuses every program as unsupported.
 */
#include "common/xanadu/vicon/compiler.hpp"

#include "common/xanadu/vicon/parser.hpp"

namespace xanadu::vicon {

Compiler::Compiler(zigzag::vortex::VortexCore &core)
    : core_(core), vm_(core), stdlib_(core, vm_) {}

std::expected<CompiledProgram, CompileFailure>
Compiler::compile(const Program & /*program*/) {
  return std::unexpected(
      CompileFailure{.code   = CompileError::UnsupportedConstruct,
                     .detail = "vicon compiler not implemented yet",
                     .loc    = {}});
}

std::expected<CompiledProgram, CompileFailure>
Compiler::compileSource(std::string_view source) {
  Parser parser(source);
  auto program = parser.parseProgram();
  if (!program) {
    return std::unexpected(CompileFailure{.code = CompileError::UnexpectedToken,
                                          .detail = program.error().detail,
                                          .loc    = program.error().loc});
  }
  return compile(*program);
}

std::expected<CompiledProgram, CompileFailure>
Compiler::compileExpression(std::string_view source) {
  Parser parser(source);
  auto expr = parser.parseExpression();
  if (!expr) {
    return std::unexpected(CompileFailure{.code = CompileError::UnexpectedToken,
                                          .detail = expr.error().detail,
                                          .loc    = expr.error().loc});
  }
  return std::unexpected(
      CompileFailure{.code   = CompileError::UnsupportedConstruct,
                     .detail = "vicon compiler not implemented yet",
                     .loc    = {}});
}

} // namespace xanadu::vicon
