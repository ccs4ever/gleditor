#include "common/xanadu/vagent/parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

#include "common/xanadu/vagent/lexer.hpp"

namespace xanadu::vagent {
namespace {

bool isWord(const Token &token) {
  return token.kind == TokenKind::Identifier ||
         token.kind == TokenKind::Keyword;
}

bool pascalCase(std::string_view name) {
  return !name.empty() && name.front() >= 'A' && name.front() <= 'Z';
}

struct Operator {
  int precedence;
  bool rightAssociative{false};
  bool nonAssociative{false};
};

std::optional<Operator> binaryOperator(const Token &token) {
  switch (token.kind) {
  case TokenKind::Ampersand:
    return Operator{1};
  case TokenKind::Assign:
  case TokenKind::RevAssign:
    return Operator{2, true};
  case TokenKind::Scan:
    return Operator{3, false, true};
  case TokenKind::Alternation:
    return Operator{5};
  case TokenKind::Equal:
  case TokenKind::NotEqual:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
    return Operator{7, false, true};
  case TokenKind::Concat:
    return Operator{8};
  case TokenKind::Plus:
  case TokenKind::Minus:
    return Operator{9};
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
    return Operator{10};
  default:
    break;
  }
  if (token.text == "to") return Operator{4, false, true};
  return std::nullopt;
}

} // namespace

class Parser::Mode {
public:
  Mode(bool &slot, bool value) : slot_(slot), previous_(slot) { slot_ = value; }
  Mode(const Mode &)            = delete;
  Mode &operator=(const Mode &) = delete;
  ~Mode() { slot_ = previous_; }

private:
  bool &slot_;
  bool previous_;
};

class Parser::Depth {
public:
  explicit Depth(Parser &parser) : parser_(parser) {
    if (parser_.depth_ >= parser_.limits_.maxDepth) {
      parser_.fail("syntax nesting exceeds the configured maxDepth");
    }
    ++parser_.depth_;
  }
  Depth(const Depth &)            = delete;
  Depth &operator=(const Depth &) = delete;
  ~Depth() { --parser_.depth_; }

private:
  Parser &parser_;
};

Parser::Parser(std::string_view source, ParseLimits limits) : limits_(limits) {
  auto tokens = Lexer(source).tokenize();
  if (!tokens) {
    lexicalError_ = std::move(tokens.error());
    tokens_.push_back(Token{});
  } else if (tokens->size() > limits_.maxTokens) {
    lexicalError_ = Diagnostic{DiagnosticCode::Syntax,
                               "token count exceeds the configured maxTokens",
                               tokens->at(limits_.maxTokens).loc};
    tokens_.push_back(Token{});
  } else {
    tokens_ = std::move(*tokens);
  }
}

const Token &Parser::peek(std::size_t ahead) const {
  return tokens_[std::min(position_ + ahead, tokens_.size() - 1)];
}

bool Parser::at(TokenKind kind) const { return peek().kind == kind; }
bool Parser::word(std::string_view spelling) const {
  return isWord(peek()) && peek().text == spelling;
}

Token Parser::take() {
  Token token = peek();
  if (position_ + 1 < tokens_.size()) ++position_;
  return token;
}

void Parser::layout() {
  if (!significantNewlines_) {
    while (at(TokenKind::Newline)) take();
  }
}

void Parser::separators() {
  while (at(TokenKind::Newline) || at(TokenKind::Semicolon)) take();
}

bool Parser::accept(TokenKind kind) {
  layout();
  if (!at(kind)) return false;
  take();
  return true;
}

bool Parser::acceptWord(std::string_view spelling) {
  layout();
  if (!word(spelling)) return false;
  take();
  return true;
}

Token Parser::expect(TokenKind kind, std::string_view description) {
  layout();
  if (!at(kind)) fail("expected " + std::string(description));
  return take();
}

void Parser::expectWord(std::string_view spelling) {
  layout();
  if (!acceptWord(spelling)) fail("expected '" + std::string(spelling) + "'");
}

Token Parser::identifier() {
  return expect(TokenKind::Identifier,
                "an identifier (reserved words are not names)");
}

Token Parser::anyWord() {
  layout();
  if (!isWord(peek())) fail("expected a field or module-path word");
  return take();
}

void Parser::fail(std::string detail, DiagnosticCode code) const {
  throw Diagnostic{code, std::move(detail), peek().loc};
}

bool Parser::startsContinuation(std::size_t position) const {
  const auto &token = tokens_[std::min(position, tokens_.size() - 1)];
  switch (token.kind) {
  case TokenKind::Ampersand:
  case TokenKind::Alternation:
  case TokenKind::Concat:
  case TokenKind::Plus:
  case TokenKind::Slash:
  case TokenKind::Percent:
  case TokenKind::Equal:
  case TokenKind::NotEqual:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
  case TokenKind::Dot:
  case TokenKind::Scan:
    return true;
  default:
    break;
  }
  return token.text == "to" || token.text == "by" || token.text == "then" ||
         token.text == "else" || token.text == "do";
}

void Parser::expressionLayout() {
  if (!at(TokenKind::Newline)) return;
  std::size_t after = position_;
  while (tokens_[after].kind == TokenKind::Newline) ++after;
  if (!significantNewlines_ || startsContinuation(after)) position_ = after;
}

bool Parser::expressionEndsHere() const {
  return at(TokenKind::Newline) || at(TokenKind::Semicolon) ||
         at(TokenKind::CloseBrace) || at(TokenKind::CloseParen) ||
         at(TokenKind::CloseBracket) || at(TokenKind::Comma) ||
         at(TokenKind::EndOfFile) || word("else") || word("then") || word("do");
}

ExprPtr Parser::make(ExprKind kind, SourceLocation loc, std::string text) {
  auto result               = std::make_unique<Expr>();
  result->kind              = kind;
  result->text              = std::move(text);
  result->loc               = loc;
  nodeDepths_[result.get()] = 1;
  return result;
}

ExprPtr Parser::finish(ExprPtr node) {
  std::size_t height = 1;
  const auto include = [&](const ExprPtr &child) {
    if (child) height = std::max(height, nodeDepths_.at(child.get()) + 1);
  };
  for (const auto &child : node->children) include(child);
  include(node->scope);
  include(node->recipients);
  include(node->deadline);
  include(node->guard);
  include(node->recovery);
  if (height > limits_.maxDepth) {
    throw Diagnostic{DiagnosticCode::Syntax,
                     "expression tree exceeds the configured maxDepth",
                     node->loc};
  }
  nodeDepths_[node.get()] = height;
  return node;
}

std::expected<Program, Diagnostic> Parser::parseProgram() {
  if (lexicalError_) return std::unexpected(*lexicalError_);
  try {
    Program program;
    layout();
    if (acceptWord("module")) program.module = modulePath();
    layout();
    while (word("import")) {
      Import imported;
      imported.loc  = take().loc;
      imported.path = modulePath();
      if (acceptWord("as")) imported.alias = identifier().text;
      if (accept(TokenKind::OpenParen)) {
        if (!at(TokenKind::CloseParen)) imported.symbols = identifierList();
        expect(TokenKind::CloseParen, "')' after imported symbols");
      }
      program.imports.push_back(std::move(imported));
      layout();
    }
    while (!at(TokenKind::EndOfFile)) {
      program.declarations.push_back(declaration(Context::Top));
      layout();
    }
    return program;
  } catch (const Diagnostic &diagnostic) {
    return std::unexpected(diagnostic);
  }
}

std::expected<ExprPtr, Diagnostic> Parser::parseExpression() {
  if (lexicalError_) return std::unexpected(*lexicalError_);
  try {
    auto result = expression();
    separators();
    if (!at(TokenKind::EndOfFile)) fail("unexpected input after expression");
    return result;
  } catch (const Diagnostic &diagnostic) {
    return std::unexpected(diagnostic);
  }
}

std::string Parser::modulePath() {
  auto result = anyWord().text;
  while (accept(TokenKind::Colon)) result += ':' + anyWord().text;
  return result;
}

std::vector<std::string> Parser::identifierList() {
  std::vector<std::string> result;
  result.push_back(identifier().text);
  while (accept(TokenKind::Comma)) result.push_back(identifier().text);
  return result;
}

Type Parser::type() {
  Depth depth(*this);
  if (accept(TokenKind::OpenBracket)) {
    Type result{"List", {type()}};
    expect(TokenKind::CloseBracket, "']' after a list type");
    return result;
  }
  if (accept(TokenKind::OpenParen)) {
    expect(TokenKind::CloseParen, "')' in the unit type");
    return Type{"()", {}};
  }
  Type result{identifier().text, {}};
  if (accept(TokenKind::Less)) {
    result.arguments.push_back(type());
    while (accept(TokenKind::Comma)) result.arguments.push_back(type());
    expect(TokenKind::Greater, "'>' after type arguments");
  }
  return result;
}

std::vector<Parameter> Parser::parameters(bool fields, bool keyed) {
  Mode newlines(significantNewlines_, false);
  const auto closing = fields ? TokenKind::CloseBrace : TokenKind::CloseParen;
  std::vector<Parameter> result;
  layout();
  if (at(closing)) return result;
  for (;;) {
    Parameter parameter;
    parameter.key   = keyed && acceptWord("key");
    const auto name = fields ? anyWord() : identifier();
    parameter.name  = name.text;
    parameter.loc   = name.loc;
    expect(TokenKind::Colon, "':' after a parameter or field name");
    parameter.type = type();
    result.push_back(std::move(parameter));
    if (!accept(TokenKind::Comma)) break;
    layout();
    if (at(closing)) {
      if (keyed) fail("predicate parameter list cannot end with a comma");
      break;
    }
  }
  return result;
}

ExprPtr Parser::headerExpression() {
  Mode records(allowRecord_, false);
  return expression();
}

Decl Parser::method(bool answer, bool signature) {
  Decl declaration;
  declaration.loc      = peek().loc;
  declaration.kind     = answer ? DeclKind::Answer : DeclKind::Method;
  declaration.effect   = acceptWord("effect");
  declaration.preempts = declaration.effect && acceptWord("preempts");
  expectWord(answer ? "answer" : "def");
  declaration.name = identifier().text;
  expect(TokenKind::OpenParen, "'(' after a method name");
  declaration.parameters = parameters();
  expect(TokenKind::CloseParen, "')' after parameters");
  if (accept(TokenKind::Colon))
    declaration.result = type();
  else if (answer)
    fail("an answer requires an explicit result type");
  if (acceptWord("within")) {
    const auto timeout =
        expect(TokenKind::DurationLiteral, "an effect duration");
    declaration.deadline = make(ExprKind::Duration, timeout.loc, timeout.text);
  }
  while (word("requires") || word("ensures")) {
    const auto keyword = take();
    Contract contract;
    contract.postcondition = keyword.text == "ensures";
    contract.loc           = keyword.loc;
    contract.condition     = headerExpression();
    declaration.contracts.push_back(std::move(contract));
  }
  if (!signature) declaration.body = block();
  return declaration;
}

Decl Parser::handler(Context context) {
  Decl result;
  result.loc = peek().loc;
  expectWord("on");
  if (context == Context::Skill) {
    result.kind = DeclKind::Lifecycle;
    if (!word("install") && !word("uninstall")) {
      fail("a skill lifecycle handler must be 'on install' or 'on uninstall'");
    }
    result.name = take().text;
    result.body = block();
    return result;
  }
  result.kind = DeclKind::Handler;
  if (at(TokenKind::Plus) || at(TokenKind::Minus)) {
    result.name    = take().text;
    result.trigger = term();
  } else {
    const auto name = identifier();
    result.name     = name.text;
    result.trigger  = make(ExprKind::Identifier, name.loc, name.text);
    if (acceptWord("as")) result.binding = identifier().text;
  }
  if (accept(TokenKind::OpenBracket)) {
    Mode newlines(significantNewlines_, false);
    Mode records(allowRecord_, true);
    result.guard = expression();
    expect(TokenKind::CloseBracket, "']' after a handler guard");
  }
  if (acceptWord("priority")) {
    auto priority =
        expect(TokenKind::IntegerLiteral, "an integer handler priority");
    auto spelling = priority.text;
    std::erase(spelling, '_');
    std::string_view digits = spelling;
    int radix               = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
      digits.remove_prefix(2);
      radix = 16;
    }
    std::int64_t number{};
    const auto converted = std::from_chars(
        digits.data(), digits.data() + digits.size(), number, radix);
    if (converted.ec != std::errc{} ||
        converted.ptr != digits.data() + digits.size()) {
      throw Diagnostic{DiagnosticCode::Syntax,
                       "handler priority is outside the Int range",
                       priority.loc};
    }
    result.priority = number;
  }
  result.body = block();
  return result;
}

Decl Parser::goal() {
  Decl result;
  result.kind = DeclKind::Goal;
  result.loc  = take().loc;
  result.name = identifier().text;
  expect(TokenKind::OpenParen, "'(' after a goal name");
  result.parameters = parameters();
  expect(TokenKind::CloseParen, "')' after goal parameters");
  if (acceptWord("achieve"))
    result.goalKind = GoalKind::Achieve;
  else if (acceptWord("perform"))
    result.goalKind = GoalKind::Perform;
  else if (acceptWord("maintain"))
    result.goalKind = GoalKind::Maintain;
  else
    fail("expected goal kind 'achieve', 'perform' or 'maintain'");
  if (accept(TokenKind::OpenBrace)) {
    while (!accept(TokenKind::CloseBrace)) {
      GoalClause clause;
      clause.loc = peek().loc;
      if (acceptWord("succeed")) {
        expectWord("when");
        clause.kind = GoalClause::Kind::Succeed;
      } else if (acceptWord("drop")) {
        expectWord("when");
        clause.kind = GoalClause::Kind::Drop;
      } else if (acceptWord("holds"))
        clause.kind = GoalClause::Kind::Holds;
      else
        fail("expected 'succeed when', 'drop when' or 'holds' in a goal");
      clause.condition = headerExpression();
      result.goalClauses.push_back(std::move(clause));
    }
  }
  return result;
}

Decl Parser::plan() {
  Decl result;
  result.kind = DeclKind::Plan;
  result.loc  = take().loc;
  result.name = identifier().text;
  expectWord("for");
  result.target = identifier().text;
  if (accept(TokenKind::OpenParen)) {
    result.planBindings = std::vector<std::string>{};
    if (!at(TokenKind::CloseParen)) *result.planBindings = identifierList();
    expect(TokenKind::CloseParen, "')' after plan parameter names");
  }
  if (acceptWord("where")) result.guard = headerExpression();
  result.body = block();
  return result;
}

Decl Parser::declaration(Context context) {
  Depth depth(*this);
  layout();
  if (word("on") && context != Context::Top && context != Context::Capacity) {
    return handler(context);
  }
  if (word("def") || word("effect")) {
    if (context == Context::Top)
      fail("methods must be declared in an agent, behavior, skill or capacity");
    return method(false, context == Context::Capacity);
  }
  if (word("answer")) {
    if (context != Context::Agent && context != Context::Behavior) {
      fail("answers must be declared in an agent or behavior");
    }
    return method(true, false);
  }
  if (word("goal") || word("plan")) {
    if (context == Context::Skill || context == Context::Capacity)
      fail("goal/plan declaration is not valid here");
    return word("goal") ? goal() : plan();
  }
  if (context == Context::Capacity)
    fail("a capacity body contains method signatures only");
  Decl result;
  result.loc = peek().loc;
  if (acceptWord("uses")) {
    if (context == Context::Top) fail("'uses' is a member declaration");
    result.kind  = DeclKind::Uses;
    result.names = identifierList();
    return result;
  }
  if (acceptWord("var")) {
    if (context == Context::Top)
      fail("fields must belong to an agent, behavior or skill");
    result.kind = DeclKind::Field;
    result.name = identifier().text;
    expect(TokenKind::Colon, "':' after a field name");
    result.result = type();
    expect(TokenKind::Assign, "':=' and an initial field value");
    result.initializer = expression();
    return result;
  }
  if (word("install") || (word("behavior") && context == Context::Agent)) {
    if (context != Context::Agent)
      fail("install and behavior-use members belong to agents");
    const auto keyword = take();
    result.kind =
        keyword.text == "install" ? DeclKind::Install : DeclKind::BehaviorUse;
    result.initializer = namedCall(false);
    result.name        = result.initializer->children.front()->text;
    return result;
  }
  if (acceptWord("believe")) {
    if (context != Context::Agent) fail("initial beliefs belong to agents");
    result.kind        = DeclKind::Belief;
    result.initializer = term();
    result.name        = result.initializer->text;
    return result;
  }
  if (context != Context::Top) fail("unexpected member declaration");
  if (acceptWord("event") || acceptWord("record")) {
    const bool recordKind = tokens_[position_ - 1].text == "record";
    result.kind           = recordKind ? DeclKind::Record : DeclKind::Event;
    result.name           = identifier().text;
    if (!recordKind && acceptWord("extends"))
      result.names.push_back(identifier().text);
    if (accept(TokenKind::OpenBrace)) {
      result.parameters = parameters(true);
      expect(TokenKind::CloseBrace, "'}' after record/event fields");
    } else if (recordKind)
      fail("a record requires a field block");
    return result;
  }
  if (acceptWord("predicate") || acceptWord("question")) {
    const bool predicate = tokens_[position_ - 1].text == "predicate";
    result.kind          = predicate ? DeclKind::Predicate : DeclKind::Question;
    result.name          = identifier().text;
    expect(TokenKind::OpenParen, "'(' after a predicate/question name");
    result.parameters = parameters(false, predicate);
    expect(TokenKind::CloseParen, "')' after predicate/question parameters");
    if (!predicate) {
      expect(TokenKind::Colon, "':' and a question result type");
      result.result = type();
    }
    return result;
  }
  if (acceptWord("capacity")) {
    result.kind = DeclKind::Capacity;
    result.name = identifier().text;
    if (acceptWord("extends")) result.names = identifierList();
    expect(TokenKind::OpenBrace, "'{' before capacity methods");
    while (!accept(TokenKind::CloseBrace))
      result.members.push_back(declaration(Context::Capacity));
    return result;
  }
  result.native = acceptWord("native");
  if (acceptWord("skill"))
    result.kind = DeclKind::Skill;
  else if (!result.native && acceptWord("behavior"))
    result.kind = DeclKind::Behavior;
  else if (!result.native && acceptWord("agent"))
    result.kind = DeclKind::Agent;
  else
    fail("expected a top-level declaration");
  result.name = identifier().text;
  if (accept(TokenKind::OpenParen)) {
    result.parameters = parameters();
    expect(TokenKind::CloseParen, "')' after declaration parameters");
  }
  if (result.kind == DeclKind::Skill) {
    expectWord("implements");
    result.names = identifierList();
    if (result.native) return result;
  }
  expect(TokenKind::OpenBrace, "'{' before declaration members");
  const auto memberContext = result.kind == DeclKind::Skill ? Context::Skill
                             : result.kind == DeclKind::Agent
                                 ? Context::Agent
                                 : Context::Behavior;
  while (!accept(TokenKind::CloseBrace))
    result.members.push_back(declaration(memberContext));
  return result;
}

std::vector<ExprPtr> Parser::arguments() {
  Mode newlines(significantNewlines_, false);
  Mode records(allowRecord_, true);
  Mode scans(allowScan_, true);
  std::vector<ExprPtr> result;
  layout();
  if (accept(TokenKind::CloseParen)) return result;
  for (;;) {
    result.push_back(expression());
    if (!accept(TokenKind::Comma)) break;
    if (accept(TokenKind::CloseParen)) return result;
  }
  expect(TokenKind::CloseParen, "')' after arguments");
  return result;
}

ExprPtr Parser::namedCall(bool required) {
  const auto name = identifier();
  auto result     = make(ExprKind::Call, name.loc);
  result->children.push_back(make(ExprKind::Identifier, name.loc, name.text));
  if (accept(TokenKind::OpenParen)) {
    auto args = arguments();
    for (auto &arg : args) result->children.push_back(std::move(arg));
  } else if (required)
    fail("expected '(' after callable name");
  return finish(std::move(result));
}

ExprPtr Parser::block() {
  Depth depth(*this);
  layout();
  const auto open = expect(TokenKind::OpenBrace, "'{' before a body");
  Mode newlines(significantNewlines_, true);
  Mode records(allowRecord_, true);
  Mode scans(allowScan_, true);
  auto result = make(ExprKind::Block, open.loc);
  separators();
  while (!accept(TokenKind::CloseBrace)) {
    if (at(TokenKind::EndOfFile)) fail("unterminated block; expected '}'");
    ExprPtr statement;
    if (acceptWord("let")) {
      const auto name = identifier();
      statement       = make(ExprKind::Let, name.loc, name.text);
      if (accept(TokenKind::Colon)) statement->annotation = type();
      expect(TokenKind::Assign, "':=' after a local declaration");
      statement->children.push_back(expression());
      statement = finish(std::move(statement));
    } else {
      statement = expression();
    }
    // A postfix call cannot own else before resolution: doing so would steal
    // the outer branch in `if c then x := read() else x := 0`.
    expressionLayout();
    if (acceptWord("else")) {
      ExprPtr *call = nullptr;
      if (statement->kind == ExprKind::Call) {
        call = &statement;
      } else if (statement->kind == ExprKind::Let &&
                 statement->children.size() == 1 &&
                 statement->children.front()->kind == ExprKind::Call) {
        call = &statement->children.front();
      } else if (statement->kind == ExprKind::Binary &&
                 statement->text == ":=" && statement->children.size() == 2 &&
                 statement->children[1]->kind == ExprKind::Call) {
        call = &statement->children[1];
      }
      if (!call) {
        fail("statement recovery requires a direct call, a ':=' direct-call "
             "RHS, or a let direct-call initializer");
      }
      (*call)->recovery = expression();
      *call             = finish(std::move(*call));
      // The call has already been included in its wrapper's cached height.
      // Recompute it before any outer block can trust that height.
      statement = finish(std::move(statement));
    }
    result->children.push_back(std::move(statement));
    if (!at(TokenKind::CloseBrace) && !at(TokenKind::Newline) &&
        !at(TokenKind::Semicolon)) {
      fail("expected newline or ';' between statements");
    }
    separators();
  }
  return finish(std::move(result));
}

ExprPtr Parser::expression(int minimum) {
  Depth depth(*this);
  while (at(TokenKind::Newline)) take();
  auto left = prefix();
  for (;;) {
    expressionLayout();
    const auto op = binaryOperator(peek());
    if (!op || op->precedence < minimum || (at(TokenKind::Scan) && !allowScan_))
      break;
    const auto token = take();
    auto result = make(token.text == "to" ? ExprKind::Range : ExprKind::Binary,
                       left->loc, token.text);
    result->children.push_back(std::move(left));
    result->children.push_back(
        expression(op->precedence + (op->rightAssociative ? 0 : 1)));
    if (token.text == "to" && acceptWord("by"))
      result->children.push_back(expression(5));
    if (op->nonAssociative) {
      expressionLayout();
      const auto next = binaryOperator(peek());
      if (next && next->precedence == op->precedence)
        fail("operator '" + token.text +
             "' does not chain; use parentheses or '&'");
    }
    left = finish(std::move(result));
  }
  return finish(std::move(left));
}

ExprPtr Parser::prefix() {
  layout();
  const auto token = peek();
  if (word("not") || at(TokenKind::Minus) || at(TokenKind::Bang) ||
      at(TokenKind::Star)) {
    take();
    auto result = make(ExprKind::Unary, token.loc, token.text);
    result->children.push_back(expression(token.text == "not" ? 6 : 11));
    return finish(std::move(result));
  }
  return postfix(primary());
}

ExprPtr Parser::postfix(ExprPtr base) {
  for (;;) {
    expressionLayout();
    if (accept(TokenKind::OpenParen)) {
      auto call = make(ExprKind::Call, base->loc);
      call->children.push_back(std::move(base));
      auto args = arguments();
      for (auto &arg : args) call->children.push_back(std::move(arg));
      base = finish(std::move(call));
    } else if (accept(TokenKind::OpenBracket)) {
      Mode newlines(significantNewlines_, false);
      Mode records(allowRecord_, true);
      Mode scans(allowScan_, true);
      auto indexed = make(ExprKind::Index, base->loc);
      indexed->children.push_back(std::move(base));
      indexed->children.push_back(expression());
      expect(TokenKind::CloseBracket, "']' after index");
      base = finish(std::move(indexed));
    } else if (accept(TokenKind::Dot)) {
      const auto field = anyWord();
      auto accessed    = make(ExprKind::Field, base->loc, field.text);
      accessed->children.push_back(std::move(base));
      base = finish(std::move(accessed));
    } else
      break;
  }
  return finish(std::move(base));
}

ExprPtr Parser::operand() {
  while (at(TokenKind::Newline)) take();
  return postfix(primary());
}

ExprPtr Parser::list() {
  const auto open = take();
  Mode newlines(significantNewlines_, false);
  Mode records(allowRecord_, true);
  Mode scans(allowScan_, true);
  auto result = make(ExprKind::List, open.loc);
  if (!accept(TokenKind::CloseBracket)) {
    for (;;) {
      result->children.push_back(expression());
      if (!accept(TokenKind::Comma)) break;
      if (accept(TokenKind::CloseBracket)) return finish(std::move(result));
    }
    expect(TokenKind::CloseBracket, "']' after a list");
  }
  return finish(std::move(result));
}

ExprPtr Parser::record(Token name) {
  expect(TokenKind::OpenBrace, "'{' after a record type");
  Mode newlines(significantNewlines_, false);
  Mode records(allowRecord_, true);
  Mode scans(allowScan_, true);
  auto result = make(ExprKind::Record, name.loc, name.text);
  if (!accept(TokenKind::CloseBrace)) {
    for (;;) {
      result->names.push_back(anyWord().text);
      expect(TokenKind::Colon, "':' after a record field name");
      result->children.push_back(expression());
      if (!accept(TokenKind::Comma)) break;
      if (accept(TokenKind::CloseBrace)) return finish(std::move(result));
    }
    expect(TokenKind::CloseBrace, "'}' after record fields");
  }
  return finish(std::move(result));
}

ExprPtr Parser::termArgument() {
  if (accept(TokenKind::Scan)) {
    const auto name = identifier();
    return make(ExprKind::LogicVariable, name.loc, name.text);
  }
  Mode scans(allowScan_, false);
  return expression();
}

ExprPtr Parser::term() {
  const auto name = identifier();
  auto result     = make(ExprKind::Term, name.loc, name.text);
  expect(TokenKind::OpenParen, "'(' after a predicate name");
  Mode newlines(significantNewlines_, false);
  Mode records(allowRecord_, true);
  if (!accept(TokenKind::CloseParen)) {
    for (;;) {
      result->children.push_back(termArgument());
      if (!accept(TokenKind::Comma)) break;
    }
    expect(TokenKind::CloseParen, "')' after predicate arguments");
  }
  return finish(std::move(result));
}

ExprPtr Parser::primary() {
  layout();
  const auto token = peek();
  switch (token.kind) {
  case TokenKind::IntegerLiteral:
    take();
    return make(ExprKind::Integer, token.loc, token.text);
  case TokenKind::FloatLiteral:
    take();
    return make(ExprKind::Float, token.loc, token.text);
  case TokenKind::StringLiteral:
    take();
    return make(ExprKind::String, token.loc, token.stringValue);
  case TokenKind::DurationLiteral:
    take();
    return make(ExprKind::Duration, token.loc, token.text);
  case TokenKind::OpenBrace:
    return block();
  case TokenKind::OpenBracket:
    return list();
  case TokenKind::OpenParen: {
    take();
    Mode newlines(significantNewlines_, false);
    Mode records(allowRecord_, true);
    Mode scans(allowScan_, true);
    if (accept(TokenKind::CloseParen)) return make(ExprKind::Unit, token.loc);
    auto result = expression();
    expect(TokenKind::CloseParen, "')' after expression");
    return finish(std::move(result));
  }
  default:
    break;
  }
  if (acceptWord("true") || acceptWord("false"))
    return make(ExprKind::Boolean, token.loc, token.text);
  if (acceptWord("none")) return make(ExprKind::None, token.loc);
  if (acceptWord("if")) {
    auto result = make(ExprKind::If, token.loc);
    result->children.push_back(expression());
    expressionLayout();
    expectWord("then");
    result->children.push_back(expression());
    expressionLayout();
    if (acceptWord("else")) result->children.push_back(expression());
    return finish(std::move(result));
  }
  if (acceptWord("while")) {
    auto result = make(ExprKind::While, token.loc);
    result->children.push_back(expression());
    expressionLayout();
    expectWord("do");
    result->children.push_back(expression());
    return finish(std::move(result));
  }
  if (acceptWord("for")) {
    if (acceptWord("believes")) {
      auto result = make(ExprKind::ForBelieves, token.loc);
      result->children.push_back(term());
      expressionLayout();
      expectWord("do");
      result->children.push_back(expression());
      return finish(std::move(result));
    }
    auto result     = make(ExprKind::For, token.loc);
    result->binding = identifier().text;
    expectWord("in");
    result->children.push_back(expression());
    expressionLayout();
    expectWord("do");
    result->children.push_back(expression());
    return finish(std::move(result));
  }
  if (acceptWord("believes")) {
    auto result = make(ExprKind::Believes, token.loc);
    result->children.push_back(term());
    if (acceptWord("from")) result->scope = operand();
    return finish(std::move(result));
  }
  if (acceptWord("findall")) {
    auto result = make(ExprKind::FindAll, token.loc);
    expect(TokenKind::OpenParen, "'(' after findall");
    Mode newlines(significantNewlines_, false);
    result->children.push_back(termArgument());
    expect(TokenKind::Comma, "',' between findall template and term");
    result->children.push_back(term());
    expect(TokenKind::CloseParen, "')' after findall");
    return finish(std::move(result));
  }
  if (token.kind == TokenKind::Keyword && token.text != "self") {
    return action(take());
  }
  if (token.kind == TokenKind::Identifier || token.text == "self") {
    take();
    layout();
    if (pascalCase(token.text) && allowRecord_ && at(TokenKind::OpenBrace))
      return record(token);
    return make(ExprKind::Identifier, token.loc, token.text);
  }
  fail("expected an expression");
}

ExprPtr Parser::action(Token keyword) {
  const auto &name = keyword.text;
  if (name == "terminate") return make(ExprKind::Terminate, keyword.loc);
  if (name == "yield") return make(ExprKind::Yield, keyword.loc);
  if (name == "consume") return make(ExprKind::Consume, keyword.loc);
  if (name == "fail") return make(ExprKind::Fail, keyword.loc);
  if (name == "return") {
    auto result = make(ExprKind::Return, keyword.loc);
    if (!expressionEndsHere()) result->children.push_back(expression());
    return finish(std::move(result));
  }
  if (name == "emit") {
    auto result = make(ExprKind::Emit, keyword.loc);
    result->children.push_back(operand());
    if (acceptWord("in")) result->scope = operand();
    if (acceptWord("for")) result->recipients = operand();
    return finish(std::move(result));
  }
  if (name == "tell" || name == "untell") {
    auto result =
        make(name == "tell" ? ExprKind::Tell : ExprKind::Untell, keyword.loc);
    result->children.push_back(operand());
    result->children.push_back(term());
    return finish(std::move(result));
  }
  if (name == "believe" || name == "forget" || name == "revise") {
    const auto kind = name == "believe"  ? ExprKind::Believe
                      : name == "forget" ? ExprKind::Forget
                                         : ExprKind::Revise;
    auto result     = make(kind, keyword.loc);
    result->children.push_back(term());
    return finish(std::move(result));
  }
  if (name == "spawn" || name == "adopt" || name == "achieve" ||
      name == "install" || name == "mount") {
    const auto kind = name == "spawn"     ? ExprKind::Spawn
                      : name == "adopt"   ? ExprKind::Adopt
                      : name == "achieve" ? ExprKind::Achieve
                      : name == "install" ? ExprKind::Install
                                          : ExprKind::Mount;
    auto result     = make(kind, keyword.loc);
    result->children.push_back(
        namedCall(name == "spawn" || name == "adopt" || name == "achieve"));
    expressionLayout();
    if (name == "spawn" && acceptWord("in")) result->scope = operand();
    if (name == "achieve") {
      if (acceptWord("by")) {
        result->recipients = operand();
        expectWord("within");
        result->deadline = operand();
      }
      expressionLayout();
      if (acceptWord("else")) result->recovery = expression();
    }
    return finish(std::move(result));
  }
  if (name == "uninstall" || name == "unmount") {
    auto result =
        make(name == "uninstall" ? ExprKind::Uninstall : ExprKind::Unmount,
             keyword.loc);
    const auto target = identifier();
    result->text      = target.text;
    result->children.push_back(
        make(ExprKind::Identifier, target.loc, target.text));
    return finish(std::move(result));
  }
  if (name == "drop" || name == "stop") {
    auto result =
        make(name == "drop" ? ExprKind::Drop : ExprKind::Stop, keyword.loc);
    result->children.push_back(operand());
    return finish(std::move(result));
  }
  if (name == "ask") {
    auto result = make(ExprKind::Ask, keyword.loc);
    result->children.push_back(operand());
    expectWord("within");
    result->deadline = operand();
    expressionLayout();
    if (acceptWord("else")) result->recovery = expression();
    return finish(std::move(result));
  }
  if (name == "await") {
    const auto target = identifier();
    auto result       = make(ExprKind::AwaitTerm, keyword.loc);
    if (word("as")) {
      take();
      result->kind    = ExprKind::AwaitEvent;
      result->text    = target.text;
      result->binding = identifier().text;
    } else {
      --position_;
      result->children.push_back(term());
    }
    if (accept(TokenKind::OpenBracket)) {
      Mode newlines(significantNewlines_, false);
      Mode records(allowRecord_, true);
      result->guard = expression();
      expect(TokenKind::CloseBracket, "']' after await guard");
    }
    expectWord("within");
    result->deadline = operand();
    expressionLayout();
    if (acceptWord("else")) result->recovery = expression();
    return finish(std::move(result));
  }
  fail("unexpected keyword '" + name + "' in an expression");
}

} // namespace xanadu::vagent
