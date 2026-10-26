#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/token.hpp"

namespace {

TEST(VagentGuideTest, EveryCompleteSourceExampleParsesAndChecks) {
  std::ifstream input("docs/vagent-user-guide.md");
  ASSERT_TRUE(input.is_open());
  const std::string guide{std::istreambuf_iterator<char>(input), {}};
  const std::string opening = "```vagent\n";
  std::size_t position      = 0;
  std::size_t examples      = 0;
  while ((position = guide.find(opening, position)) != std::string::npos) {
    const auto start = position + opening.size();
    const auto end   = guide.find("\n```", start);
    ASSERT_NE(end, std::string::npos);
    const auto source = guide.substr(start, end - start);
    SCOPED_TRACE("guide example " + std::to_string(++examples));
    xanadu::vagent::Parser parser(source);
    auto program = parser.parseProgram();
    ASSERT_TRUE(program.has_value())
        << xanadu::vagent::formatDiagnostic(program.error());
    auto result = xanadu::vagent::check(*program);
    if (!result) {
      std::string errors;
      for (const auto &diagnostic : result.error()) {
        errors += xanadu::vagent::formatDiagnostic(diagnostic) + '\n';
      }
      ADD_FAILURE() << errors;
    }
    position = end + 4;
  }
  EXPECT_GT(examples, 0U);
}

} // namespace
