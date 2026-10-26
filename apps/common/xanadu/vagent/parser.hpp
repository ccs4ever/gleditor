#ifndef COMMON_XANADU_VAGENT_PARSER_HPP
#define COMMON_XANADU_VAGENT_PARSER_HPP

#include <expected>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"

namespace xanadu::vagent {

struct ParseLimits {
  std::size_t maxTokens{1'000'000};
  std::size_t maxDepth{256};
};

class Parser {
public:
  explicit Parser(std::string_view source, ParseLimits limits = {});

  /// Consumes the whole source. Use a fresh parser for an independent parse.
  [[nodiscard]] std::expected<Program, Diagnostic> parseProgram();
  [[nodiscard]] std::expected<ExprPtr, Diagnostic> parseExpression();

private:
  enum class Context : std::uint8_t { Top, Agent, Behavior, Skill, Capacity };
  class Depth;
  class Mode;

  [[nodiscard]] const Token &peek(std::size_t ahead = 0) const;
  [[nodiscard]] bool at(TokenKind kind) const;
  [[nodiscard]] bool word(std::string_view spelling) const;
  Token take();
  bool accept(TokenKind kind);
  bool acceptWord(std::string_view spelling);
  Token expect(TokenKind kind, std::string_view description);
  void expectWord(std::string_view spelling);
  Token identifier();
  Token anyWord();
  [[noreturn]] void fail(std::string detail,
                         DiagnosticCode code = DiagnosticCode::Syntax) const;
  void layout();
  void separators();
  void expressionLayout();
  [[nodiscard]] bool startsContinuation(std::size_t position) const;
  [[nodiscard]] bool expressionEndsHere() const;

  std::string modulePath();
  std::vector<std::string> identifierList();
  Type type();
  std::vector<Parameter> parameters(bool fields = false, bool keyed = false);
  Decl declaration(Context context);
  Decl method(bool answer, bool signature);
  Decl handler(Context context);
  Decl goal();
  Decl plan();
  ExprPtr headerExpression();

  ExprPtr expression(int minimum = 0);
  ExprPtr prefix();
  ExprPtr primary();
  ExprPtr postfix(ExprPtr base);
  ExprPtr operand();
  ExprPtr block();
  ExprPtr list();
  ExprPtr record(Token name);
  ExprPtr action(Token keyword);
  ExprPtr term();
  ExprPtr termArgument();
  ExprPtr namedCall(bool required);
  std::vector<ExprPtr> arguments();
  ExprPtr make(ExprKind kind, SourceLocation loc, std::string text = {});
  ExprPtr finish(ExprPtr expression);

  std::vector<Token> tokens_;
  std::unordered_map<const Expr *, std::size_t> nodeDepths_;
  std::optional<Diagnostic> lexicalError_;
  ParseLimits limits_;
  std::size_t position_{0};
  std::size_t depth_{0};
  bool significantNewlines_{false};
  bool allowRecord_{true};
  bool allowScan_{true};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_PARSER_HPP
