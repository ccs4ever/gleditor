/**
 * @file parser.hpp
 * @brief Recursive-descent + precedence-climbing parser for vlace (§5.1).
 *
 * Produces a Program (declarations plus an optional trailing expression) or a
 * named CompileError. Application is juxtaposition and binds tighter than every
 * operator; operators resolve by the built-in fixity table (§5.1), extensible
 * by `infixl`/`infixr`/`infix` declarations.
 */
#ifndef COMMON_XANADU_VLACE_PARSER_HPP
#define COMMON_XANADU_VLACE_PARSER_HPP

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vlace/lexer.hpp"

namespace xanadu::vlace {

/// A parse failure: the error code plus a human detail and the location.
struct ParseError {
  CompileError code{CompileError::UnexpectedToken};
  std::string detail;
  SourceLocation loc{};
};

struct OpInfo {
  int level{9};
  Fixity fixity{Fixity::Left};
};

class Parser {
public:
  explicit Parser(std::string_view source);

  /// Parse a whole program: zero or more `decl ;` then an optional expression.
  [[nodiscard]] std::expected<Program, ParseError> parseProgram();

  /// Parse a single expression (for the REPL).
  [[nodiscard]] std::expected<Expr, ParseError> parseExpressionOnly();

private:
  // Token cursor helpers.
  const Token &peek() const { return cur_; }
  const Token &peek2() const { return next_; }
  Token advance();
  bool check(TokenKind k) const { return cur_.kind == k; }
  bool match(TokenKind k);
  std::expected<Token, ParseError> expect(TokenKind k, const char *what);

  // Grammar productions.
  std::expected<Decl, ParseError> parseDecl();
  std::expected<DataDecl, ParseError> parseDataDecl();
  std::expected<TypeDecl, ParseError> parseTypeDecl();
  std::expected<ImportDecl, ParseError> parseImportDecl();
  std::expected<FixityDecl, ParseError> parseFixityDecl();
  std::expected<ValueDecl, ParseError> parseValueDecl();

  std::expected<Expr, ParseError> parseExpr();
  std::expected<Expr, ParseError> parseOpExpr(int minLevel);
  std::expected<Expr, ParseError> parseApp();
  std::expected<Expr, ParseError> parseAtom();
  std::expected<Expr, ParseError> parseLet(bool recursive);
  std::expected<Expr, ParseError> parseIf();
  std::expected<Expr, ParseError> parseCase();
  std::expected<Expr, ParseError> parseLambda();

  std::expected<Pattern, ParseError> parsePattern();
  std::expected<Pattern, ParseError> parseAppPattern();
  std::expected<Pattern, ParseError> parseAtomPattern();

  std::expected<Type, ParseError> parseType();
  std::expected<Type, ParseError> parseAtomType();

  [[nodiscard]] std::optional<OpInfo> opInfo(std::string_view op) const;
  [[nodiscard]] bool atExprStart() const;

  static Expr desugarList(std::vector<Expr> elems, SourceLocation loc);

  Lexer lexer_;
  Token cur_{};
  Token next_{};
  std::unordered_map<std::string, OpInfo> fixity_;
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_PARSER_HPP
