#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/code_path.hpp"

namespace {

using namespace xanadu::vagent;

TEST(VAgentCodePathTest, ParsesLabelHeaderAndBodyLikeParseCodeBody) {
  const SourceLocation literal{.line = 12, .column = 4, .offset = 200};
  auto parsed = parseCodePath("#inc n: Int, b: Bool | if b then n + 1 else n",
                              literal);
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_TRUE(parsed->label);
  EXPECT_EQ(*parsed->label, "inc");
  ASSERT_EQ(parsed->parameters.size(), 2U);
  EXPECT_EQ(parsed->parameters[0].name, "n");
  EXPECT_EQ(typeName(parsed->parameters[0].type), "Int");
  EXPECT_EQ(parsed->parameters[1].name, "b");
  EXPECT_EQ(parsed->body->kind, ExprKind::If);
}

TEST(VAgentCodePathTest, InnerDiagnosticsCarryTheLiteralQualifiedOffset) {
  const SourceLocation literal{.line = 12, .column = 4, .offset = 200};
  auto parsed = parseCodePath("x + )", literal);
  ASSERT_FALSE(parsed);
  const auto &error = parsed.error();
  EXPECT_EQ(error.code, DiagnosticCode::Syntax);
  EXPECT_EQ(error.loc.line, 12U);
  EXPECT_EQ(error.loc.column, 4U);
  ASSERT_EQ(error.inner.size(), 1U);
  EXPECT_EQ(error.inner[0].line, 1U);
  EXPECT_EQ(error.inner[0].column, 5U);
  EXPECT_EQ(formatDiagnostic(error),
            "12:4+1:5: Syntax: expected an expression");
}

TEST(VAgentCodePathTest, InnerDiagnosticChainsOneArmPerNestingLevel) {
  const SourceLocation literal{.line = 12, .column = 4, .offset = 200};
  const SourceLocation nested{.line = 1, .column = 12, .offset = 11};
  // A diagnostic raised inside a nested literal's body, first mapped to
  // the nested literal then to the outer one — one '+o:c' arm per level.
  auto once = innerDiagnostic(
      Diagnostic{DiagnosticCode::Syntax, "expected an expression",
                 SourceLocation{.line = 1, .column = 9, .offset = 8}},
      nested);
  auto twice = innerDiagnostic(once, literal);
  EXPECT_EQ(twice.loc.line, 12U);
  EXPECT_EQ(twice.loc.column, 4U);
  ASSERT_EQ(twice.inner.size(), 2U);
  EXPECT_EQ(twice.inner[0].line, 1U);
  EXPECT_EQ(twice.inner[0].column, 12U);
  EXPECT_EQ(twice.inner[1].line, 1U);
  EXPECT_EQ(twice.inner[1].column, 9U);
  EXPECT_EQ(formatDiagnostic(twice),
            "12:4+1:12+1:9: Syntax: expected an expression");
}

TEST(VAgentCodePathTest, LabelErrorsMapToTheLiteralToo) {
  const SourceLocation literal{.line = 3, .column = 9, .offset = 40};
  auto parsed = parseCodePath("#9x e", literal);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, DiagnosticCode::Syntax);
  EXPECT_EQ(parsed.error().loc.line, 3U);
  EXPECT_EQ(parsed.error().loc.column, 9U);
  ASSERT_EQ(parsed.error().inner.size(), 1U);
  EXPECT_EQ(parsed.error().inner[0].line, 1U);
  EXPECT_EQ(parsed.error().inner[0].column, 1U);
}

TEST(VAgentCodePathTest, ReturnInsideABodyIsRefused) {
  auto direct = parseCodePath("return x", {});
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().code, DiagnosticCode::InvalidPosition);
  EXPECT_NE(direct.error().detail.find("return"), std::string::npos);
  // The refusal is a narrowing, not a parse failure — it applies however
  // deep the statement sits, including inside a block.
  auto nested = parseCodePath("{ x := 1; return x }", {});
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().code, DiagnosticCode::InvalidPosition);
  EXPECT_EQ(nested.error().inner[0].column, 11U);
}

TEST(VAgentCodePathTest, SuspendingStatementsAreRefused) {
  for (const std::string body :
       {"yield", "await e(x) within 1s", "achieve g(x)",
        "ask p.q(1) within 1s", "if c then yield else 0"}) {
    SCOPED_TRACE(body);
    auto parsed = parseCodePath(body, {});
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, DiagnosticCode::InvalidPosition);
    ASSERT_EQ(parsed.error().inner.size(), 1U);
  }
}

TEST(VAgentCodePathTest, SpliceAndStaticNodesAreNotNarrowed) {
  // The skeleton's named DiagnosticCode refusal for splice/static is the
  // expand stage's answer; narrowing must not pre-empt it.
  for (const std::string body :
       {"splice c", "static (60 * 1000)", "x + splice inc(count)"}) {
    SCOPED_TRACE(body);
    auto parsed = parseCodePath(body, {});
    ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  }
}

TEST(VAgentCodePathTest, ImpureOrNonSuspendingBodiesStayLegal) {
  // 'return' and the suspension forms are the only narrowing — effects and
  // other actions narrow at the checker, not here.
  for (const std::string body :
       {"x := 1", "emit e(x)", "believe p(1)", "{ x := 1; x }"}) {
    SCOPED_TRACE(body);
    auto parsed = parseCodePath(body, {});
    ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  }
}

} // namespace
