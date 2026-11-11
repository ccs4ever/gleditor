#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

std::string readFileContents(const std::string &path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(VAgentCognitiveTest, ParseMindStdlibModuleToR1Ast) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);

  const std::string mindPath = "apps/common/xanadu/vagent/stdlib/mind.vagent";
  ASSERT_TRUE(std::filesystem::exists(mindPath)) << "mind.vagent must exist";

  const auto source = readFileContents(mindPath);
  ASSERT_FALSE(source.empty());

  auto r1Prog = parseProgramToR1(arena, core, source);
  ASSERT_TRUE(r1Prog.has_value()) << r1Prog.error();
  EXPECT_NE(*r1Prog, noCell);

  const auto &dims = core.dims();
  EXPECT_EQ(arena.textOf(*r1Prog), "program");

  const auto firstChild = arena.linked(*r1Prog, dims.grab, Posward);
  ASSERT_TRUE(firstChild.has_value());
  EXPECT_TRUE(arena.textOf(*firstChild) == "import" ||
              arena.textOf(*firstChild) == "decl.agent");
}

TEST(VAgentCognitiveTest, ParseArbitrationStdlibModuleToR1Ast) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);

  const std::string arbPath =
      "apps/common/xanadu/vagent/stdlib/arbitration.vagent";
  ASSERT_TRUE(std::filesystem::exists(arbPath))
      << "arbitration.vagent must exist";

  const auto source = readFileContents(arbPath);
  ASSERT_FALSE(source.empty());

  auto r1Prog = parseProgramToR1(arena, core, source);
  ASSERT_TRUE(r1Prog.has_value()) << r1Prog.error();
  EXPECT_NE(*r1Prog, noCell);

  const auto &dims = core.dims();
  EXPECT_EQ(arena.textOf(*r1Prog), "program");

  const auto firstChild = arena.linked(*r1Prog, dims.grab, Posward);
  ASSERT_TRUE(firstChild.has_value());
}

TEST(VAgentCognitiveTest, ParseAllStdlibModulesToR1Ast) {
  const std::vector<std::string> stdlibFiles = {
      "apps/common/xanadu/vagent/stdlib/ast.vagent",
      "apps/common/xanadu/vagent/stdlib/lexer.vagent",
      "apps/common/xanadu/vagent/stdlib/parser.vagent",
      "apps/common/xanadu/vagent/stdlib/expand.vagent",
      "apps/common/xanadu/vagent/stdlib/resolve.vagent",
      "apps/common/xanadu/vagent/stdlib/checker.vagent",
      "apps/common/xanadu/vagent/stdlib/emitter.vagent",
      "apps/common/xanadu/vagent/stdlib/fragment.vagent",
      "apps/common/xanadu/vagent/stdlib/decls.vagent",
      "apps/common/xanadu/vagent/stdlib/mind.vagent",
      "apps/common/xanadu/vagent/stdlib/arbitration.vagent",
  };

  for (const auto &path : stdlibFiles) {
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);

    ASSERT_TRUE(std::filesystem::exists(path)) << "Missing file: " << path;
    const auto source = readFileContents(path);
    ASSERT_FALSE(source.empty()) << "Empty file: " << path;

    auto r1Prog = parseProgramToR1(arena, core, source);
    ASSERT_TRUE(r1Prog.has_value())
        << "Failed to parse " << path << " to R1: " << r1Prog.error();
    EXPECT_NE(*r1Prog, noCell);
    EXPECT_EQ(arena.textOf(*r1Prog), "program");
  }
}

} // namespace
