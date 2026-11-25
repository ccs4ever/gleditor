/**
 * @file parser.hpp
 * @brief Recursive-descent parser for VQL v16.0 adhering strictly to formal
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

  /// Parses an unmount declaration.
  UnmountDeclaration parseUnmountDeclaration();

  /// Parses a weave statement.
  WeaveStatement parseWeaveStatement();

  /// Parses a script I/O action: `effect { ... }` (top-level script only).
  IoEffectStatement parseIoEffectStatement();

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
  ValueExpr parseValueAtom();
  ValueExpr parseValueOrComparisonExpr();
  FunctionInvocation parseFunctionInvocation();
  std::shared_ptr<RuntimeClosureInvocation> parseRuntimeClosureInvocation();
  ValueExpr parseRuntimeClosureValue();

  DefDeclaration parseDefDeclaration();
  ShapeLiteral parseShapeLiteral();
  ShapeRule parseShapeRule();
  VersionPin parseVersionPin();
  VersionExpr parseVersionExpr();
  DestructurePattern parseDestructurePattern();
  DestructureField parseDestructureField();
  TypeExpr parseTypeExpr();
  PrimaryType parsePrimaryType();
  LambdaParam parseLambdaParam();
  std::vector<LambdaParam> parseLambdaParams();
  PathLambda parsePathLambda();
  bool isBlockBody();
  [[nodiscard]] bool atIoEffect();
  [[nodiscard]] bool checkWord(std::string_view word) const;
  void consumeWord(std::string_view word);
  Token consumeVariable(std::string_view what);
  IoOperand parseIoOperand();
  /// Refuse a script I/O name in a row or query expression.
  void refuseIoCall(const Token &name) const;

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
  /// Set by parseQuery for the one script body that may hold `effect`
  /// actions: the top-level body, not a def, watch, perspective or lambda.
  bool nextBodyIsScript_{false};
  bool effectsAllowed_{false};
  bool hasLookahead_{false};
  Token lookahead_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_PARSER_HPP
