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

  /// §5.1's CodeBody parse — a decoded code"…" literal's inner text into
  /// its optional '#' label, optional CodeParams header and body
  /// expression. The Code node stores the literal's text raw and expand
  /// runs this per literal, so the entry is static: the '#' strip must
  /// precede the inner lex, which means running a Parser of its own over
  /// the label-stripped text (consumed characters are blanked, keeping
  /// every inner SourceLocation literal-relative).
  ///
  /// Diagnostics are offset-only — inner coordinates, not yet qualified
  /// with the literal's own loc. The reporter applies the
  /// src:m:l:c+o:c mapping (code_path's innerDiagnostic), which keeps
  /// parser.o free of code_path symbols so VAGENT_SYNTAX_OBJS needs no
  /// edit.
  [[nodiscard]] static std::expected<CodeBody, Diagnostic>
  parseCodeBody(std::string_view text, ParseLimits limits = {});

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
  // When lastSuffix is given it receives the kind of the outermost suffix
  // this postfix chain applied — the 'splice' peeling rule needs to know
  // whether its operand ended in a call's arglist (§5.1's 'splice' Postfix)
  // or produced a call another way (parenthesized), which is a check error.
  ExprPtr postfix(ExprPtr base, TokenKind *lastSuffix = nullptr);
  ExprPtr operand();
  ExprPtr pattern();
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
  // The inner parse behind parseCodeBody — [CodeParams '|'] Expr on this
  // parser's own stream, whose source is already '#'-stripped.
  std::expected<CodeBody, Diagnostic> codeBody();

  std::vector<Token> tokens_;
  std::unordered_map<const Expr *, std::size_t> nodeDepths_;
  std::optional<Diagnostic> lexicalError_;
  ParseLimits limits_;
  std::size_t position_{0};
  std::size_t depth_{0};
  bool significantNewlines_{false};
  bool allowRecord_{true};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_PARSER_HPP
