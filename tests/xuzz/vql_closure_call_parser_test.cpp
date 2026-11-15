/**
 * @file vql_closure_call_parser_test.cpp
 * @brief Syntax tests for first-class runtime closure invocations.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace {

using namespace xanadu::vql;

const RuntimeClosureInvocation &
returnedClosureCall(const QueryExpression &query) {
  const auto &block    = std::get<ExecutionBlock>(query.expr);
  const auto &returned = std::get<ReturnClause>(block.action.clause);
  const auto &value    = std::get<ReturnValue>(returned.items.front().item);
  const auto &call     = std::get<std::shared_ptr<RuntimeClosureInvocation>>(
      value.expression.kind);
  return *call;
}

TEST(VQLClosureCallParserTest, BareCallPreservesOrderedActualExpressions) {
  const auto query = Parser("$f(1, $x)").parseQuery();
  const auto &call = returnedClosureCall(query);

  ASSERT_EQ(std::get<std::string>(call.callee.kind), "f");
  ASSERT_EQ(call.args.size(), 2U);
  ASSERT_EQ(call.argNames.size(), 2U);
  EXPECT_TRUE(call.argNames[0].empty());
  EXPECT_TRUE(call.argNames[1].empty());
  EXPECT_EQ(
      std::get<std::int64_t>(std::get<ScalarLiteral>(call.args[0].kind).value),
      1);
  EXPECT_EQ(std::get<std::string>(call.args[1].kind), "x");
}

TEST(VQLClosureCallParserTest, LetAndReturnUseValueInvocationNode) {
  const auto query =
      Parser("let $answer := $f(1, 2); return $answer").parseQuery();
  ASSERT_EQ(query.body.statements.size(), 2U);
  const auto &block = std::get<ExecutionBlock>(query.body.statements.front());
  ASSERT_EQ(block.bindings.size(), 1U);
  const auto &let   = std::get<LetClause>(block.bindings.front());
  const auto &value = std::get<ValueExpr>(let.target);
  EXPECT_TRUE(std::holds_alternative<std::shared_ptr<RuntimeClosureInvocation>>(
      value.kind));
}

TEST(VQLClosureCallParserTest, BareCallResultCanContinueAsPath) {
  const auto query     = Parser("$f(1)/+d.x").parseQuery();
  const auto &block    = std::get<ExecutionBlock>(query.expr);
  const auto &returned = std::get<ReturnClause>(block.action.clause);
  const auto &value    = std::get<ReturnValue>(returned.items.front().item);
  const auto &path =
      std::get<std::shared_ptr<PathExpression>>(value.expression.kind);
  ASSERT_TRUE(path);
  ASSERT_EQ(path->steps.size(), 2U);
  const auto &call = std::get<std::shared_ptr<RuntimeClosureInvocation>>(
      path->steps.front().selector);
  ASSERT_TRUE(call);
  EXPECT_EQ(std::get<std::string>(call->callee.kind), "f");
  ASSERT_EQ(call->args.size(), 1U);
  const auto &dimension =
      std::get<SignedDimensionStep>(path->steps.back().selector);
  EXPECT_EQ(dimension.dimName, "d.x");
}

TEST(VQLClosureCallParserTest, LetAndReturnCallsCanContinueAsPath) {
  const auto letQuery = Parser("let $r := $f(1)/+d.x; return $r").parseQuery();
  const auto &block =
      std::get<ExecutionBlock>(letQuery.body.statements.front());
  const auto &let   = std::get<LetClause>(block.bindings.front());
  const auto &value = std::get<ValueExpr>(let.target);
  const auto &path  = std::get<std::shared_ptr<PathExpression>>(value.kind);
  ASSERT_TRUE(path);
  ASSERT_EQ(path->steps.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<std::shared_ptr<RuntimeClosureInvocation>>(
      path->steps.front().selector));

  const auto returnQuery = Parser("return $f(1)/+d.x").parseQuery();
  const auto &returnBlock =
      std::get<ExecutionBlock>(returnQuery.body.statements.front());
  const auto &returned    = std::get<ReturnClause>(returnBlock.action.clause);
  const auto &returnValue = std::get<ReturnValue>(returned.items.front().item);
  EXPECT_TRUE(std::holds_alternative<std::shared_ptr<PathExpression>>(
      returnValue.expression.kind));
}

TEST(VQLClosureCallParserTest, ScalarAdditionIsNotPathContinuation) {
  const auto query     = Parser("$f(1) + 2").parseQuery();
  const auto &block    = std::get<ExecutionBlock>(query.expr);
  const auto &returned = std::get<ReturnClause>(block.action.clause);
  const auto &value    = std::get<ReturnValue>(returned.items.front().item);
  EXPECT_TRUE(std::holds_alternative<std::shared_ptr<BinaryValueExpr>>(
      value.expression.kind));
}

TEST(VQLClosureCallParserTest, NumericSlashAfterCallIsExplicitlyRefused) {
  try {
    (void)Parser("$f(4)/2").parseQuery();
    FAIL() << "numeric division is outside the current VQL value grammar";
  } catch (const ParseError &error) {
    EXPECT_NE(std::string{error.what()}.find("numeric division"),
              std::string::npos);
  }
}

TEST(VQLClosureCallParserTest, PathStepCallIsDistinctFromContextualAlias) {
  const auto explicitQuery = Parser("##/d.items/$f(1, 2)").parseQuery();
  const auto &explicitPath = std::get<PathExpression>(explicitQuery.expr);
  ASSERT_EQ(explicitPath.steps.size(), 2U);
  const auto &selector = std::get<std::shared_ptr<RuntimeClosureInvocation>>(
      explicitPath.steps.back().selector);
  ASSERT_TRUE(selector);
  EXPECT_EQ(std::get<std::string>(selector->callee.kind), "f");
  ASSERT_EQ(selector->args.size(), 2U);

  const auto contextualQuery = Parser("##/d.items/$f").parseQuery();
  const auto &contextualPath = std::get<PathExpression>(contextualQuery.expr);
  ASSERT_EQ(contextualPath.steps.size(), 2U);
  const auto &dimension =
      std::get<SignedDimensionStep>(contextualPath.steps.back().selector);
  EXPECT_EQ(dimension.dimName, "$f");
}

TEST(VQLClosureCallParserTest, NamedActualLabelsRemainExplicitInAst) {
  const auto query =
      Parser("let $answer := $f(first: 1); return $answer").parseQuery();
  const auto &block = std::get<ExecutionBlock>(query.body.statements.front());
  const auto &let   = std::get<LetClause>(block.bindings.front());
  const auto &value = std::get<ValueExpr>(let.target);
  const auto &call =
      std::get<std::shared_ptr<RuntimeClosureInvocation>>(value.kind);
  ASSERT_TRUE(call);
  ASSERT_EQ(call->argNames.size(), 1U);
  EXPECT_EQ(call->argNames.front(), "first");
}

TEST(VQLClosureCallParserTest, IncompleteCallIsRejected) {
  EXPECT_THROW((void)Parser("$f(1,").parseQuery(), ParseError);
}

} // namespace
