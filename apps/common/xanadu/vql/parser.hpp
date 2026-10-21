/**
 * @file parser.hpp
 * @brief Recursive-descent parser for VQL v13.0 adhering strictly to formal
 * EBNF.
 */
#ifndef COMMON_XANADU_VQL_PARSER_HPP
#define COMMON_XANADU_VQL_PARSER_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/lexer.hpp"

namespace xanadu::vql {

class ParseError : public std::runtime_error {
public:
  ParseError(std::string_view message, SourceLocation loc);

  [[nodiscard]] const SourceLocation &location() const noexcept { return loc_; }

private:
  SourceLocation loc_;
};

class Parser {
public:
  explicit Parser(std::string_view source);
  explicit Parser(Lexer lexer);

  /// Parses a complete VQL query expression.
  QueryExpression parseQuery();

  /// Parses a script body (sequence of statements).
  ScriptBody parseScriptBody();

  /// Parses a single statement.
  Statement parseStatement();

  /// Parses a watch statement.
  WatchStatement parseWatchStatement();

  /// Parses a perspective block.
  PerspectiveBlock parsePerspectiveBlock();

  /// Parses a mount declaration.
  MountDeclaration parseMountDeclaration();

  /// Parses a weave statement.
  WeaveStatement parseWeaveStatement();

  /// Parses a store anchor.
  StoreAnchor parseStoreAnchor();

  /// Parses a single path expression.
  PathExpression parsePathExpression(bool allowCloneTail = true);

  /// Parses an execution block (FLWOR).
  ExecutionBlock parseExecutionBlock();

  /// Parses a boolean expression.
  BooleanExpr parseBooleanExpr();

private:
  // Grammar parsing helpers
  AnchorNode parseAnchorNode();
  PathStep parsePathStep(bool requireSlash = true);
  StepSelector parseStepSelector();
  RangeClamp parseRangeClamp();
  CloneTail parseCloneTail();
  YieldMode parseYield();

  ForClause parseForClause();
  LetClause parseLetClause();
  WhereClause parseWhereClause();
  ActionClause parseActionClause();
  ReturnClause parseReturnClause();
  EffectClause parseEffectClause();
  ConditionalClause parseConditionalClause();

  BooleanTerm parseBooleanTerm();
  BooleanFactor parseBooleanFactor();
  ComparisonExpr parseComparisonExpr();
  PredicateTest parsePredicateTest();

  ValueExpr parseValueExpr();
  ValueExpr parseValueOrComparisonExpr();
  FunctionInvocation parseFunctionInvocation();

  DefDeclaration parseDefDeclaration();
  ShapeLiteral parseShapeLiteral();
  ShapeRule parseShapeRule();
  VersionPin parseVersionPin();
  VersionExpr parseVersionExpr();
  DestructurePattern parseDestructurePattern();
  PathLambda parsePathLambda();

  // Token stream navigation
  Token currentToken();
  Token peekToken();
  Token advance();
  [[nodiscard]] bool check(TokenKind kind) const;
  bool match(TokenKind kind);
  Token consume(TokenKind kind, std::string_view errorMessage);
  bool isNextTokenComparisonOperand();

  Lexer lexer_;
  Token current_;
  bool hasLookahead_{false};
  Token lookahead_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_PARSER_HPP
