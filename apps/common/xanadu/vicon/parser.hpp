/**
 * @file parser.hpp
 * @brief vicon parser: precedence climbing over Icon's operator table.
 *
 * Refuses by returning std::expected<_, ParseError> — a refusal names its
 * reason, never throws for user-input errors and never produces a partial
 * AST (AGENTS.md "refuse loudly, by number").
 */
#ifndef COMMON_XANADU_VICON_PARSER_HPP
#define COMMON_XANADU_VICON_PARSER_HPP

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vicon/ast.hpp"
#include "common/xanadu/vicon/token.hpp"

namespace xanadu::vicon {

enum class ParseError : std::uint8_t {
  LexicalError,
  UnexpectedToken,
  UnbalancedParen,
  UnbalancedBrace,
  UnbalancedScan,  ///< `?` without a following expression
  MissingThen,
  MissingDo,
  MissingEnd,
  ExpectedExpr,
};

struct ParseFailure {
  ParseError code{ParseError::UnexpectedToken};
  std::string detail;
  SourceLocation loc{};
};

class Parser {
public:
  explicit Parser(std::string_view source);

  /// A whole program: procedure declarations plus top-level statements.
  [[nodiscard]] std::expected<Program, ParseFailure> parseProgram();

  /// One expression (for the REPL's expression lines).
  [[nodiscard]] std::expected<ExprPtr, ParseFailure> parseExpression();

  /// Render a failure as "line:col: Code: detail".
  [[nodiscard]] static std::string formatError(const ParseFailure &f);

private:
  // Precedence-climbing driver. Icon precedences (low binds looser):
  //   |  alternation (right), := <- assignment (right),
  //   to/by sequence (non-assoc), ? scan,
  //   relational (produce-or-fail), || concat, + -, * / %, unary, ^ power
  std::expected<ExprPtr, ParseFailure> parseExpr(int minPrec = 0);
  std::expected<ExprPtr, ParseFailure> parseUnary();
  std::expected<ExprPtr, ParseFailure> parsePrimary();
  std::expected<ExprPtr, ParseFailure> parsePostfix(ExprPtr base);
  std::expected<ExprPtr, ParseFailure> parseBraceSeq();
  std::expected<ExprPtr, ParseFailure> parseIf();
  std::expected<ExprPtr, ParseFailure> parseWhileOrUntil(bool isWhile);
  std::expected<ExprPtr, ParseFailure> parseEvery();
  std::expected<ExprPtr, ParseFailure> parseCase();
  std::expected<ExprPtr, ParseFailure> parseRepeat();
  std::expected<ProcDecl, ParseFailure> parseProcedure();

  [[nodiscard]] const Token &peek(std::size_t ahead = 0) const;
  const Token &advance();
  [[nodiscard]] bool at(TokenKind k) const;
  [[nodiscard]] bool accept(TokenKind k);
  std::expected<const Token &, ParseFailure> expect(TokenKind k,
                                                    ParseError err,
                                                    std::string_view what);
  /// Skip optional Semicolons between statements.
  void skipSeparators();

  std::vector<Token> tokens_;
  std::size_t pos_{0};
};

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_PARSER_HPP
