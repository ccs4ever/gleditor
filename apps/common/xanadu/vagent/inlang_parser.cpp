#include "common/xanadu/vagent/inlang_parser.hpp"

#include <format>
#include <string>

#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"

namespace xanadu::vagent {

std::expected<zigzag::CellRef, std::string>
parseSourceToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                std::string_view source) {
  auto expr = Parser(source).parseExpression();
  if (!expr) {
    return std::unexpected(
        std::format("parse error: {}", formatDiagnostic(expr.error())));
  }
  return marshalAstToR1(arena, core, **expr);
}

std::expected<zigzag::CellRef, std::string>
parseProgramToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 std::string_view source) {
  auto prog = Parser(source).parseProgram();
  if (!prog) {
    return std::unexpected(
        std::format("parse error: {}", formatDiagnostic(prog.error())));
  }
  return marshalProgramToR1(arena, core, *prog);
}

} // namespace xanadu::vagent
