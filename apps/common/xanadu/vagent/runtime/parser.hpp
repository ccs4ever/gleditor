/**
 * @file parser.hpp
 * @brief vagent parser: design §8.9 by recursive descent, with the twelve
 * precedence levels of §8.4 as one function per level.
 *
 * Refuses by returning std::expected<_, ParseError> carrying a code, a
 * message and a source location; user input never throws out of this API and
 * never yields a partial AST. Internally a refusal unwinds with a private
 * exception so the grammar functions stay free of error plumbing.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_PARSER_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_PARSER_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/ast.hpp"
#include "common/xanadu/vagent/runtime/lexer.hpp"
#include "common/xanadu/vagent/runtime/token.hpp"

namespace xanadu::vagent::runtime {

enum class ParseErrorCode : std::uint8_t {
  LexicalError,
  UnexpectedToken,
  ExpectedExpression,
  ExpectedDeclaration,
  ExpectedType,
  ExpectedName,
  UnbalancedBrace,
  UnbalancedParen,
  UnbalancedBracket,
  NonAssociativeComparison, ///< `a < b < c`
  MissingWithin,            ///< `ask`, `await` and `achieve ... by` need it
  MissingSeparator,         ///< two statements on one line
  MissingBlock,
  InvalidTerm,
  NotAllowedHere, ///< a member the enclosing declaration does not admit
  NestingTooDeep,
};

[[nodiscard]] std::string_view parseErrorName(ParseErrorCode code) noexcept;

struct ParseError {
  ParseErrorCode code{ParseErrorCode::UnexpectedToken};
  std::string message;
  SourceLocation loc{};
};

/// "line:col: Code: message"
[[nodiscard]] std::string formatParseError(const ParseError &error);

class Parser {
public:
  explicit Parser(std::string_view source);

  /// A whole file: `[module] {import} {declaration}`.
  [[nodiscard]] std::expected<Unit, ParseError> parseUnit();

  /// One expression and nothing after it, outside any block (newlines are
  /// whitespace). For tests and for a REPL line.
  [[nodiscard]] std::expected<ExprPtr, ParseError> parseExpression();

private:
  struct Bail {
    ParseError error;
  };
  class Scope;
  class Depth;
  enum class Container : std::uint8_t { Skill, Behavior, Agent };

  // -- token cursor ---------------------------------------------------------
  [[nodiscard]] const Token &cur() const { return tokens_[pos_]; }
  [[nodiscard]] const Token &peek(std::size_t ahead) const;
  const Token &advance();
  [[nodiscard]] bool at(TokenKind k) const { return cur().kind == k; }
  [[nodiscard]] bool atWord(std::string_view text) const;
  bool accept(TokenKind k);
  const Token &expect(TokenKind k, std::string_view what);
  void expectClose(TokenKind closer);
  [[nodiscard]] std::string expectName(std::string_view what);
  [[nodiscard]] std::string expectWord(std::string_view what);
  void expectContextual(std::string_view text);
  /// Optional trailing `keeping children` of `stop` and `terminate`.
  [[nodiscard]] bool parseKeepingChildren();
  [[noreturn]] void fail(ParseErrorCode code, std::string message,
                         SourceLocation loc) const;
  [[noreturn]] void failHere(ParseErrorCode code, std::string message) const;

  /// The newline rule of §8.1 for the token at `index`: inside a block, a
  /// newline before it ends the statement unless it continues it.
  [[nodiscard]] bool breaksLineAt(std::size_t index) const;
  [[nodiscard]] bool breaksLine() const { return breaksLineAt(pos_); }
  /// at(k) and the token continues the current statement.
  [[nodiscard]] bool atCont(TokenKind k) const {
    return at(k) && !breaksLine();
  }

  // -- declarations ---------------------------------------------------------
  std::vector<std::string> parseModulePath();
  ImportDecl parseImport();
  TopDecl parseTopDecl();
  EventDecl parseEvent();
  RecordDecl parseRecord();
  PredicateDecl parsePredicate();
  QuestionDecl parseQuestion();
  CapacityDecl parseCapacity();
  SkillDecl parseSkill();
  BehaviorDecl parseBehavior();
  AgentDecl parseAgent();
  GoalDecl parseGoal();
  PlanDecl parsePlan();
  MethodSig parseMethodSig();
  Member parseMember(Container c);
  HandlerDef parseHandler(Container c);
  std::vector<Member> parseMembers(Container c);
  std::vector<Field> parseFieldList();
  std::vector<Param> parseParamList();
  std::vector<std::string> parseIdentList();
  TypeExpr parseType();
  [[nodiscard]] ExprPtr parseCondition(); ///< a record literal needs parens
  ExprPtr parseBlockBody();               ///< requires '{'
  std::vector<ExprPtr> parseCallArgs();   ///< '(' [ArgList] ')'

  // -- expressions, loosest to tightest (§8.4) ------------------------------
  ExprPtr parseExpr();
  ExprPtr parseAssign();
  ExprPtr parseScan();
  ExprPtr parseTo();
  ExprPtr parseAlt();
  ExprPtr parseNot();
  ExprPtr parseCmp();
  ExprPtr parseConcat();
  ExprPtr parseAdd();
  ExprPtr parseMul();
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  ExprPtr parseBlock();
  ExprPtr parseIf();
  ExprPtr parseWhile();
  ExprPtr parseFor();
  ExprPtr parseAction();
  ExprPtr parseAchieve();
  ExprPtr parseAsk();
  ExprPtr parseAwait();
  ExprPtr parseInstallLike();
  ExprPtr parseRecordLiteral(std::string type, SourceLocation loc);
  Stmt parseStmt();
  Term parseTerm();
  TermArg parseTermArg();
  GoalCall parseGoalCall();
  ExprPtr parseOptionalElse();
  ExprPtr parseWithin(std::string_view who);

  [[nodiscard]] bool startsExpression(TokenKind k) const;

  std::optional<LexFailure> lexError_;
  std::vector<Token> tokens_;
  std::size_t pos_{0};
  bool newlineSignificant_{false}; ///< inside a block, outside its brackets
  bool noRecord_{
      false}; ///< `Name {` is not a record literal (where, contracts)
  std::size_t depth_{0};
};

/// Convenience wrappers over Parser.
[[nodiscard]] std::expected<Unit, ParseError>
parseUnit(std::string_view source);
[[nodiscard]] std::expected<ExprPtr, ParseError>
parseExpression(std::string_view source);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_PARSER_HPP
