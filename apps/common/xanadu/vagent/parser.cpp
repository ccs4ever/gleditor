/**
 * @file parser.cpp
 * @brief Implementation of the vagent parser (design §8.4, §8.9).
 */
#include "common/xanadu/vagent/parser.hpp"

#include <utility>

namespace xanadu::vagent {

namespace {

/// Deeper than any program written by hand; bounds the native stack on
/// hostile input such as ten thousand `(`.
constexpr std::size_t maxNesting = 200;

template <typename T> ExprPtr mk(T &&node, SourceLocation loc) {
  return std::make_unique<Expr>(std::forward<T>(node), loc);
}

ExprPtr bin(EBinary::Op op, ExprPtr l, ExprPtr r, SourceLocation loc) {
  return mk(EBinary{.op = op, .left = std::move(l), .right = std::move(r)},
            loc);
}

bool isPascalCase(std::string_view s) {
  return !s.empty() && s.front() >= 'A' && s.front() <= 'Z';
}

std::optional<EBinary::Op> comparisonOp(TokenKind k) {
  switch (k) {
  case TokenKind::Less:
    return EBinary::Op::Lt;
  case TokenKind::LessEqual:
    return EBinary::Op::Le;
  case TokenKind::Greater:
    return EBinary::Op::Gt;
  case TokenKind::GreaterEqual:
    return EBinary::Op::Ge;
  case TokenKind::EqualEqual:
    return EBinary::Op::Eq;
  case TokenKind::NotEqual:
    return EBinary::Op::Ne;
  default:
    return std::nullopt;
  }
}

ELiteral literal(ELiteral::Kind k) {
  ELiteral l;
  l.kind = k;
  return l;
}

std::string describe(const Token &t) {
  if (t.kind == TokenKind::EndOfFile) {
    return "end of input";
  }
  return "'" + std::string(t.text) + "'";
}

} // namespace

std::string_view parseErrorName(ParseErrorCode code) noexcept {
  switch (code) {
  case ParseErrorCode::LexicalError:
    return "LexicalError";
  case ParseErrorCode::UnexpectedToken:
    return "UnexpectedToken";
  case ParseErrorCode::ExpectedExpression:
    return "ExpectedExpression";
  case ParseErrorCode::ExpectedDeclaration:
    return "ExpectedDeclaration";
  case ParseErrorCode::ExpectedType:
    return "ExpectedType";
  case ParseErrorCode::ExpectedName:
    return "ExpectedName";
  case ParseErrorCode::UnbalancedBrace:
    return "UnbalancedBrace";
  case ParseErrorCode::UnbalancedParen:
    return "UnbalancedParen";
  case ParseErrorCode::UnbalancedBracket:
    return "UnbalancedBracket";
  case ParseErrorCode::NonAssociativeComparison:
    return "NonAssociativeComparison";
  case ParseErrorCode::MissingWithin:
    return "MissingWithin";
  case ParseErrorCode::MissingSeparator:
    return "MissingSeparator";
  case ParseErrorCode::MissingBlock:
    return "MissingBlock";
  case ParseErrorCode::InvalidTerm:
    return "InvalidTerm";
  case ParseErrorCode::NotAllowedHere:
    return "NotAllowedHere";
  case ParseErrorCode::NestingTooDeep:
    return "NestingTooDeep";
  }
  return "Unknown";
}

std::string formatParseError(const ParseError &e) {
  return std::to_string(e.loc.line) + ":" + std::to_string(e.loc.column) +
         ": " + std::string(parseErrorName(e.code)) + ": " + e.message;
}

// -- RAII helpers -------------------------------------------------------------

/// Enters a bracket: sets the two context flags, restores them on exit.
class Parser::Scope {
public:
  Scope(Parser &p, bool newlineSignificant, bool noRecord)
      : p_(p), nl_(p.newlineSignificant_), rec_(p.noRecord_) {
    p.newlineSignificant_ = newlineSignificant;
    p.noRecord_           = noRecord;
  }
  Scope(const Scope &)            = delete;
  Scope &operator=(const Scope &) = delete;
  ~Scope() {
    p_.newlineSignificant_ = nl_;
    p_.noRecord_           = rec_;
  }

private:
  Parser &p_;
  bool nl_;
  bool rec_;
};

class Parser::Depth {
public:
  explicit Depth(Parser &p) : p_(p) {
    if (++p.depth_ > maxNesting) {
      p.failHere(ParseErrorCode::NestingTooDeep,
                 "nesting is deeper than " + std::to_string(maxNesting));
    }
  }
  Depth(const Depth &)            = delete;
  Depth &operator=(const Depth &) = delete;
  ~Depth() { --p_.depth_; }

private:
  Parser &p_;
};

// -- cursor -------------------------------------------------------------------

Parser::Parser(std::string_view source) {
  auto lexed = Lexer(source).tokenize();
  if (lexed) {
    tokens_ = std::move(*lexed);
  } else {
    lexError_ = std::move(lexed.error());
    tokens_.push_back(Token{});
  }
}

const Token &Parser::peek(std::size_t ahead) const {
  return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
}

const Token &Parser::advance() {
  const Token &t = tokens_[pos_];
  if (pos_ + 1 < tokens_.size()) {
    ++pos_;
  }
  return t;
}

bool Parser::atWord(std::string_view text) const {
  return cur().kind == TokenKind::Identifier && cur().text == text;
}

bool Parser::accept(TokenKind k) {
  if (!at(k)) {
    return false;
  }
  advance();
  return true;
}

void Parser::fail(ParseErrorCode code, std::string message,
                  SourceLocation loc) const {
  throw Bail{
      ParseError{.code = code, .message = std::move(message), .loc = loc}};
}

void Parser::failHere(ParseErrorCode code, std::string message) const {
  fail(code, std::move(message), cur().loc);
}

const Token &Parser::expect(TokenKind k, std::string_view what) {
  if (!at(k)) {
    failHere(ParseErrorCode::UnexpectedToken,
             "expected " + std::string(what) + ", found " + describe(cur()));
  }
  return advance();
}

void Parser::expectClose(TokenKind closer) {
  if (at(closer)) {
    advance();
    return;
  }
  ParseErrorCode code = ParseErrorCode::UnbalancedBrace;
  if (closer == TokenKind::CloseParen) {
    code = ParseErrorCode::UnbalancedParen;
  } else if (closer == TokenKind::CloseBracket) {
    code = ParseErrorCode::UnbalancedBracket;
  }
  failHere(code, "expected '" + std::string(tokenKindName(closer)) +
                     "', found " + describe(cur()));
}

std::string Parser::expectName(std::string_view what) {
  if (!at(TokenKind::Identifier)) {
    failHere(ParseErrorCode::ExpectedName,
             "expected " + std::string(what) + ", found " + describe(cur()) +
                 (isKeyword(cur().kind) ? " (a reserved word)" : ""));
  }
  return std::string(advance().text);
}

std::string Parser::expectWord(std::string_view what) {
  if (!isWord(cur().kind)) {
    failHere(ParseErrorCode::ExpectedName,
             "expected " + std::string(what) + ", found " + describe(cur()));
  }
  return std::string(advance().text);
}

void Parser::expectContextual(std::string_view text) {
  if (!atWord(text)) {
    failHere(ParseErrorCode::UnexpectedToken,
             "expected '" + std::string(text) + "', found " + describe(cur()));
  }
  advance();
}

bool Parser::breaksLineAt(std::size_t index) const {
  if (!newlineSignificant_ || index == 0 || index >= tokens_.size()) {
    return false;
  }
  const Token &t = tokens_[index];
  return t.newlineBefore && newlineSeparates(tokens_[index - 1].kind, t.kind);
}

bool Parser::startsExpression(TokenKind k) const {
  switch (k) {
  case TokenKind::Identifier:
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::DurationLiteral:
  case TokenKind::True:
  case TokenKind::False:
  case TokenKind::None:
  case TokenKind::Self:
  case TokenKind::OpenParen:
  case TokenKind::OpenBrace:
  case TokenKind::OpenBracket:
  case TokenKind::Minus:
  case TokenKind::Bang:
  case TokenKind::Star:
  case TokenKind::Not:
  case TokenKind::If:
  case TokenKind::While:
  case TokenKind::For:
  case TokenKind::Believes:
  case TokenKind::Findall:
  case TokenKind::Emit:
  case TokenKind::Tell:
  case TokenKind::Untell:
  case TokenKind::Believe:
  case TokenKind::Forget:
  case TokenKind::Revise:
  case TokenKind::Spawn:
  case TokenKind::Adopt:
  case TokenKind::Achieve:
  case TokenKind::Ask:
  case TokenKind::Await:
  case TokenKind::Drop:
  case TokenKind::Stop:
  case TokenKind::Terminate:
  case TokenKind::Install:
  case TokenKind::Uninstall:
  case TokenKind::Mount:
  case TokenKind::Unmount:
  case TokenKind::Return:
  case TokenKind::Yield:
  case TokenKind::Consume:
  case TokenKind::Fail:
    return true;
  default:
    return false;
  }
}

// -- entry points -------------------------------------------------------------

namespace {

ParseError fromLex(const LexFailure &f) {
  ParseErrorCode code = ParseErrorCode::LexicalError;
  std::string message = f.detail;
  if (f.code == LexErrorCode::UnclosedBracket ||
      f.code == LexErrorCode::MismatchedBracket) {
    switch (f.bracket) {
    case '(':
    case ')':
      code = ParseErrorCode::UnbalancedParen;
      break;
    case '[':
    case ']':
      code = ParseErrorCode::UnbalancedBracket;
      break;
    default:
      code = ParseErrorCode::UnbalancedBrace;
      break;
    }
  } else {
    message = std::string(lexErrorName(f.code)) + ": " + message;
  }
  return ParseError{.code = code, .message = std::move(message), .loc = f.loc};
}

} // namespace

std::expected<Unit, ParseError> Parser::parseUnit() {
  if (lexError_) {
    return std::unexpected(fromLex(*lexError_));
  }
  try {
    Unit unit;
    if (atWord("module") && isWord(peek(1).kind)) {
      ModuleDecl m;
      m.loc       = advance().loc;
      m.path      = parseModulePath();
      unit.module = std::move(m);
    }
    while (at(TokenKind::Import)) {
      unit.imports.push_back(parseImport());
    }
    while (!at(TokenKind::EndOfFile)) {
      unit.decls.push_back(parseTopDecl());
    }
    return unit;
  } catch (Bail &b) {
    return std::unexpected(std::move(b.error));
  }
}

std::expected<ExprPtr, ParseError> Parser::parseExpression() {
  if (lexError_) {
    return std::unexpected(fromLex(*lexError_));
  }
  try {
    ExprPtr e = parseExpr();
    if (!at(TokenKind::EndOfFile)) {
      failHere(ParseErrorCode::UnexpectedToken,
               "unexpected " + describe(cur()) + " after the expression");
    }
    return e;
  } catch (Bail &b) {
    return std::unexpected(std::move(b.error));
  }
}

std::expected<Unit, ParseError> parseUnit(std::string_view source) {
  return Parser(source).parseUnit();
}

std::expected<ExprPtr, ParseError> parseExpression(std::string_view source) {
  return Parser(source).parseExpression();
}

// -- module, imports, declarations
// ---------------------------------------------

std::vector<std::string> Parser::parseModulePath() {
  std::vector<std::string> path;
  path.push_back(expectWord("a module name"));
  while (accept(TokenKind::Colon)) {
    path.push_back(expectWord("a module path segment"));
  }
  return path;
}

ImportDecl Parser::parseImport() {
  ImportDecl d;
  d.loc  = expect(TokenKind::Import, "'import'").loc;
  d.path = parseModulePath();
  if (atWord("as")) {
    advance();
    d.alias = expectName("an alias");
  }
  if (accept(TokenKind::OpenParen)) {
    d.hasNames = true;
    if (!at(TokenKind::CloseParen)) {
      d.names = parseIdentList();
    }
    expectClose(TokenKind::CloseParen);
  }
  return d;
}

std::vector<std::string> Parser::parseIdentList() {
  std::vector<std::string> out;
  out.push_back(expectName("a name"));
  while (accept(TokenKind::Comma)) {
    // A trailing comma is allowed before a closer.
    if (at(TokenKind::CloseParen)) {
      break;
    }
    out.push_back(expectName("a name"));
  }
  return out;
}

TopDecl Parser::parseTopDecl() {
  switch (cur().kind) {
  case TokenKind::Event:
    return parseEvent();
  case TokenKind::Record:
    return parseRecord();
  case TokenKind::Predicate:
    return parsePredicate();
  case TokenKind::Question:
    return parseQuestion();
  case TokenKind::Capacity:
    return parseCapacity();
  case TokenKind::Native:
  case TokenKind::Skill:
    return parseSkill();
  case TokenKind::Behavior:
    return parseBehavior();
  case TokenKind::Agent:
    return parseAgent();
  case TokenKind::Goal:
    return parseGoal();
  case TokenKind::Plan:
    return parsePlan();
  default:
    failHere(ParseErrorCode::ExpectedDeclaration,
             "expected a declaration (event, record, predicate, question, "
             "capacity, skill, behavior, agent, goal or plan), found " +
                 describe(cur()));
  }
}

TypeExpr Parser::parseType() {
  const Depth guard(*this);
  TypeExpr t;
  t.loc = cur().loc;
  if (accept(TokenKind::OpenBracket)) {
    t.kind = TypeExpr::Kind::List;
    t.args.push_back(parseType());
    expectClose(TokenKind::CloseBracket);
    return t;
  }
  if (accept(TokenKind::OpenParen)) {
    expectClose(TokenKind::CloseParen);
    t.kind = TypeExpr::Kind::Unit;
    return t;
  }
  if (!at(TokenKind::Identifier)) {
    failHere(ParseErrorCode::ExpectedType,
             "expected a type, found " + describe(cur()));
  }
  t.name = std::string(advance().text);
  if (accept(TokenKind::Less)) {
    t.args.push_back(parseType());
    while (accept(TokenKind::Comma)) {
      t.args.push_back(parseType());
    }
    expect(TokenKind::Greater, "'>' to close the type arguments");
  }
  return t;
}

std::vector<Field> Parser::parseFieldList() {
  std::vector<Field> fields;
  while (!at(TokenKind::CloseBrace)) {
    Field f;
    f.loc  = cur().loc;
    f.name = expectWord("a field name");
    expect(TokenKind::Colon, "':' after the field name");
    f.type = parseType();
    fields.push_back(std::move(f));
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  return fields;
}

std::vector<Param> Parser::parseParamList() {
  std::vector<Param> params;
  while (!at(TokenKind::CloseParen)) {
    Param p;
    p.loc  = cur().loc;
    p.name = expectName("a parameter name");
    expect(TokenKind::Colon, "':' after the parameter name");
    p.type = parseType();
    params.push_back(std::move(p));
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  return params;
}

EventDecl Parser::parseEvent() {
  EventDecl d;
  d.loc  = expect(TokenKind::Event, "'event'").loc;
  d.name = expectName("an event name");
  if (accept(TokenKind::Extends)) {
    d.extends = expectName("the parent event");
  }
  if (accept(TokenKind::OpenBrace)) {
    d.hasBody = true;
    d.fields  = parseFieldList();
    expectClose(TokenKind::CloseBrace);
  }
  return d;
}

RecordDecl Parser::parseRecord() {
  RecordDecl d;
  d.loc  = expect(TokenKind::Record, "'record'").loc;
  d.name = expectName("a record name");
  expect(TokenKind::OpenBrace, "'{' to begin the record's fields");
  d.fields = parseFieldList();
  expectClose(TokenKind::CloseBrace);
  return d;
}

PredicateDecl Parser::parsePredicate() {
  PredicateDecl d;
  d.loc  = expect(TokenKind::Predicate, "'predicate'").loc;
  d.name = expectName("a predicate name");
  expect(TokenKind::OpenParen, "'(' after the predicate name");
  while (!at(TokenKind::CloseParen)) {
    PredParam p;
    p.key  = accept(TokenKind::Key);
    p.name = expectName("a parameter name");
    expect(TokenKind::Colon, "':' after the parameter name");
    p.type = parseType();
    d.params.push_back(std::move(p));
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  expectClose(TokenKind::CloseParen);
  return d;
}

QuestionDecl Parser::parseQuestion() {
  QuestionDecl d;
  d.loc  = expect(TokenKind::Question, "'question'").loc;
  d.name = expectName("a question name");
  expect(TokenKind::OpenParen, "'(' after the question name");
  d.params = parseParamList();
  expectClose(TokenKind::CloseParen);
  expect(TokenKind::Colon, "':' and the answer type");
  d.result = parseType();
  return d;
}

ExprPtr Parser::parseCondition() {
  const Scope scope(*this, newlineSignificant_, true);
  return parseExpr();
}

MethodSig Parser::parseMethodSig() {
  MethodSig sig;
  sig.loc = cur().loc;
  if (accept(TokenKind::Effect)) {
    sig.isEffect = true;
    if (atWord("preempts")) {
      advance();
      sig.preempts = true;
    }
  }
  expect(TokenKind::Def, "'def'");
  sig.name = expectName("a method name");
  expect(TokenKind::OpenParen, "'(' after the method name");
  sig.params = parseParamList();
  expectClose(TokenKind::CloseParen);
  if (accept(TokenKind::Colon)) {
    sig.result = parseType();
  }
  if (atCont(TokenKind::Within)) {
    advance();
    if (!at(TokenKind::DurationLiteral)) {
      failHere(ParseErrorCode::ExpectedExpression,
               "'within' on a method takes a duration literal such as 30s");
    }
    sig.withinNs = advance().durationNs;
  }
  while (atWord("requires") || atWord("ensures")) {
    Contract c;
    c.kind =
        atWord("requires") ? Contract::Kind::Requires : Contract::Kind::Ensures;
    advance();
    c.condition = parseCondition();
    sig.contracts.push_back(std::move(c));
  }
  return sig;
}

CapacityDecl Parser::parseCapacity() {
  CapacityDecl d;
  d.loc  = expect(TokenKind::Capacity, "'capacity'").loc;
  d.name = expectName("a capacity name");
  if (accept(TokenKind::Extends)) {
    d.extends = parseIdentList();
  }
  expect(TokenKind::OpenBrace, "'{' to begin the capacity");
  while (!at(TokenKind::CloseBrace)) {
    if (!at(TokenKind::Effect) && !at(TokenKind::Def)) {
      failHere(ParseErrorCode::UnexpectedToken,
               "a capacity holds method signatures ('def' or 'effect def'), "
               "found " +
                   describe(cur()));
    }
    d.methods.push_back(parseMethodSig());
  }
  expectClose(TokenKind::CloseBrace);
  return d;
}

SkillDecl Parser::parseSkill() {
  SkillDecl d;
  d.native = accept(TokenKind::Native);
  d.loc    = expect(TokenKind::Skill, "'skill'").loc;
  d.name   = expectName("a skill name");
  if (accept(TokenKind::OpenParen)) {
    d.hasParams = true;
    d.params    = parseParamList();
    expectClose(TokenKind::CloseParen);
  }
  expect(TokenKind::Implements, "'implements' and the capacities it provides");
  d.implements = parseIdentList();
  if (!d.native) {
    expect(TokenKind::OpenBrace, "'{' to begin the skill's body");
    d.members = parseMembers(Container::Skill);
  }
  return d;
}

BehaviorDecl Parser::parseBehavior() {
  BehaviorDecl d;
  d.loc  = expect(TokenKind::Behavior, "'behavior'").loc;
  d.name = expectName("a behavior name");
  if (accept(TokenKind::OpenParen)) {
    d.hasParams = true;
    d.params    = parseParamList();
    expectClose(TokenKind::CloseParen);
  }
  expect(TokenKind::OpenBrace, "'{' to begin the behavior's body");
  d.members = parseMembers(Container::Behavior);
  return d;
}

AgentDecl Parser::parseAgent() {
  AgentDecl d;
  d.loc  = expect(TokenKind::Agent, "'agent'").loc;
  d.name = expectName("an agent name");
  if (accept(TokenKind::OpenParen)) {
    d.hasParams = true;
    d.params    = parseParamList();
    expectClose(TokenKind::CloseParen);
  }
  expect(TokenKind::OpenBrace, "'{' to begin the agent's body");
  d.members = parseMembers(Container::Agent);
  return d;
}

std::vector<Member> Parser::parseMembers(Container c) {
  // Called just after '{'; declaration bodies need no separators, each member
  // ending where the next begins.
  const Scope scope(*this, false, false);
  std::vector<Member> members;
  while (!at(TokenKind::CloseBrace)) {
    if (at(TokenKind::EndOfFile)) {
      failHere(ParseErrorCode::UnbalancedBrace, "the body is never closed");
    }
    members.push_back(parseMember(c));
  }
  expectClose(TokenKind::CloseBrace);
  return members;
}

std::vector<ExprPtr> Parser::parseCallArgs() {
  expect(TokenKind::OpenParen, "'('");
  const Scope scope(*this, false, false);
  std::vector<ExprPtr> args;
  while (!at(TokenKind::CloseParen)) {
    args.push_back(parseExpr());
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  expectClose(TokenKind::CloseParen);
  return args;
}

Member Parser::parseMember(Container c) {
  const auto notHere = [&](std::string_view what) {
    const char *where = c == Container::Skill      ? "a skill"
                        : c == Container::Behavior ? "a behavior"
                                                   : "an agent";
    fail(ParseErrorCode::NotAllowedHere,
         std::string(what) + " is not allowed in " + where, cur().loc);
  };
  const SourceLocation loc = cur().loc;
  switch (cur().kind) {
  case TokenKind::Uses: {
    advance();
    return UsesDecl{.capacities = parseIdentList(), .loc = loc};
  }
  case TokenKind::Var: {
    advance();
    VarDecl v;
    v.loc  = loc;
    v.name = expectName("a field name");
    expect(TokenKind::Colon, "':' and the field's type");
    v.type = parseType();
    expect(TokenKind::Assign,
           "':=' and an initial value (a field requires one)");
    v.init = parseExpr();
    return v;
  }
  case TokenKind::On:
    if (c == Container::Skill && (peek(1).kind == TokenKind::Install ||
                                  peek(1).kind == TokenKind::Uninstall)) {
      advance();
      LifecycleDef l;
      l.loc       = loc;
      l.uninstall = advance().kind == TokenKind::Uninstall;
      l.body      = parseBlockBody();
      return l;
    }
    if (c == Container::Skill) {
      notHere("an event handler");
    }
    return parseHandler(c);
  case TokenKind::Def:
  case TokenKind::Effect: {
    MethodDef m;
    m.sig  = parseMethodSig();
    m.body = parseBlockBody();
    return m;
  }
  case TokenKind::Answer: {
    if (c == Container::Skill) {
      notHere("an answer");
    }
    advance();
    AnswerDef a;
    a.loc  = loc;
    a.name = expectName("a question name");
    expect(TokenKind::OpenParen, "'(' after the answer name");
    a.params = parseParamList();
    expectClose(TokenKind::CloseParen);
    expect(TokenKind::Colon, "':' and the answer type");
    a.result = parseType();
    a.body   = parseBlockBody();
    return a;
  }
  case TokenKind::Goal:
    if (c == Container::Skill) {
      notHere("a goal");
    }
    return parseGoal();
  case TokenKind::Plan:
    if (c == Container::Skill) {
      notHere("a plan");
    }
    return parsePlan();
  case TokenKind::Install: {
    if (c != Container::Agent) {
      notHere("'install'");
    }
    advance();
    InstallDecl d;
    d.loc  = loc;
    d.name = expectName("a skill name");
    if (at(TokenKind::OpenParen)) {
      d.hasParens = true;
      d.args      = parseCallArgs();
    }
    return d;
  }
  case TokenKind::Behavior: {
    if (c != Container::Agent) {
      notHere("'behavior'");
    }
    advance();
    BehaviorUse b;
    b.loc  = loc;
    b.name = expectName("a behavior name");
    if (at(TokenKind::OpenParen)) {
      b.hasParens = true;
      b.args      = parseCallArgs();
    }
    return b;
  }
  case TokenKind::Believe: {
    if (c != Container::Agent) {
      notHere("'believe'");
    }
    advance();
    return BeliefDecl{.fact = parseTerm(), .loc = loc};
  }
  default:
    fail(ParseErrorCode::UnexpectedToken,
         "expected a member (uses, var, on, def, answer, goal, plan, install, "
         "behavior or believe), found " +
             describe(cur()),
         cur().loc);
  }
}

HandlerDef Parser::parseHandler(Container /*c*/) {
  HandlerDef h;
  h.loc = expect(TokenKind::On, "'on'").loc;
  if (at(TokenKind::Plus) || at(TokenKind::Minus)) {
    BeliefTrigger t;
    t.added   = advance().kind == TokenKind::Plus;
    t.fact    = parseTerm();
    h.trigger = std::move(t);
  } else {
    EventTrigger t;
    t.eventType = expectName("an event name");
    if (atWord("as")) {
      advance();
      t.binder = expectName("a name for the event");
    }
    h.trigger = std::move(t);
  }
  if (at(TokenKind::OpenBracket)) {
    advance();
    {
      const Scope scope(*this, false, false);
      h.guard = parseExpr();
    }
    expectClose(TokenKind::CloseBracket);
  }
  if (atWord("priority")) {
    advance();
    if (!at(TokenKind::IntegerLiteral)) {
      failHere(ParseErrorCode::UnexpectedToken,
               "'priority' takes an integer literal, found " + describe(cur()));
    }
    h.priority = advance().intValue;
  }
  h.body = parseBlockBody();
  return h;
}

GoalDecl Parser::parseGoal() {
  GoalDecl d;
  d.loc  = expect(TokenKind::Goal, "'goal'").loc;
  d.name = expectName("a goal name");
  expect(TokenKind::OpenParen, "'(' after the goal name");
  d.params = parseParamList();
  expectClose(TokenKind::CloseParen);
  if (accept(TokenKind::Achieve)) {
    d.kind = GoalDecl::Kind::Achieve;
  } else if (atWord("perform")) {
    advance();
    d.kind = GoalDecl::Kind::Perform;
  } else if (atWord("maintain")) {
    advance();
    d.kind = GoalDecl::Kind::Maintain;
  } else {
    failHere(ParseErrorCode::UnexpectedToken,
             "expected the goal's kind (achieve, perform or maintain), found " +
                 describe(cur()));
  }
  if (accept(TokenKind::OpenBrace)) {
    d.hasBody = true;
    const Scope scope(*this, false, false);
    while (!at(TokenKind::CloseBrace)) {
      GoalClause g;
      if (atWord("succeed")) {
        advance();
        expectContextual("when");
        g.kind = GoalClause::Kind::SucceedWhen;
      } else if (at(TokenKind::Drop)) {
        advance();
        expectContextual("when");
        g.kind = GoalClause::Kind::DropWhen;
      } else if (atWord("holds")) {
        advance();
        g.kind = GoalClause::Kind::Holds;
      } else {
        failHere(ParseErrorCode::UnexpectedToken,
                 "expected a goal clause (succeed when, drop when or holds), "
                 "found " +
                     describe(cur()));
      }
      g.condition = parseCondition();
      d.clauses.push_back(std::move(g));
    }
    expectClose(TokenKind::CloseBrace);
  }
  return d;
}

PlanDecl Parser::parsePlan() {
  PlanDecl d;
  d.loc  = expect(TokenKind::Plan, "'plan'").loc;
  d.name = expectName("a plan name");
  expect(TokenKind::For, "'for' and the goal the plan serves");
  d.goal = expectName("a goal name");
  if (accept(TokenKind::OpenParen)) {
    d.hasParams = true;
    if (!at(TokenKind::CloseParen)) {
      d.params = parseIdentList();
    }
    expectClose(TokenKind::CloseParen);
  }
  if (accept(TokenKind::Where)) {
    d.where = parseCondition();
  }
  d.body = parseBlockBody();
  return d;
}

ExprPtr Parser::parseBlockBody() {
  if (!at(TokenKind::OpenBrace)) {
    failHere(ParseErrorCode::MissingBlock,
             "expected '{' to begin a block, found " + describe(cur()));
  }
  return parseBlock();
}

// -- statements and blocks
// ------------------------------------------------------

ExprPtr Parser::parseBlock() {
  const Depth guard(*this);
  const SourceLocation loc = expect(TokenKind::OpenBrace, "'{'").loc;
  const Scope scope(*this, true, false);
  EBlock block;
  for (;;) {
    while (accept(TokenKind::Semicolon)) {
    }
    if (at(TokenKind::CloseBrace)) {
      break;
    }
    if (at(TokenKind::EndOfFile)) {
      failHere(ParseErrorCode::UnbalancedBrace, "the block is never closed");
    }
    block.stmts.push_back(parseStmt());
    // A statement ends at ';', at a newline the rule of §8.1 honours, or at
    // the closing brace; anything else is two statements run together.
    const bool separated = at(TokenKind::Semicolon) || breaksLine();
    if (!separated && !at(TokenKind::CloseBrace)) {
      failHere(ParseErrorCode::MissingSeparator,
               "expected a newline, ';' or '}' after the statement, found " +
                   describe(cur()));
    }
  }
  expectClose(TokenKind::CloseBrace);
  return mk(std::move(block), loc);
}

Stmt Parser::parseStmt() {
  Stmt s;
  s.loc = cur().loc;
  if (accept(TokenKind::Let)) {
    s.isLet = true;
    s.name  = expectName("a name for the local");
    if (accept(TokenKind::Colon)) {
      s.type = parseType();
    }
    expect(TokenKind::Assign, "':=' in a let");
  }
  s.value = parseExpr();
  return s;
}

// -- expressions
// ----------------------------------------------------------------

ExprPtr Parser::parseExpr() {
  const Depth guard(*this);
  ExprPtr lhs = parseAssign();
  while (atCont(TokenKind::Ampersand)) {
    const SourceLocation loc = advance().loc;
    lhs = bin(EBinary::Op::And, std::move(lhs), parseAssign(), loc);
  }
  return lhs;
}

ExprPtr Parser::parseAssign() {
  const Depth guard(*this);
  ExprPtr lhs = parseScan();
  if (atCont(TokenKind::Assign) || atCont(TokenKind::RevAssign)) {
    const Token &op = advance();
    const auto kind = op.kind == TokenKind::Assign ? EBinary::Op::Assign
                                                   : EBinary::Op::RevAssign;
    return bin(kind, std::move(lhs), parseAssign(), op.loc);
  }
  return lhs;
}

ExprPtr Parser::parseScan() {
  ExprPtr lhs = parseTo();
  if (atCont(TokenKind::QuestionMark)) {
    const SourceLocation loc = advance().loc;
    return bin(EBinary::Op::Scan, std::move(lhs), parseTo(), loc);
  }
  return lhs;
}

ExprPtr Parser::parseTo() {
  ExprPtr lo = parseAlt();
  if (atCont(TokenKind::To)) {
    const SourceLocation loc = advance().loc;
    ERange r;
    r.lo = std::move(lo);
    r.hi = parseAlt();
    if (atCont(TokenKind::By)) {
      advance();
      r.by = parseAlt();
    }
    return mk(std::move(r), loc);
  }
  return lo;
}

ExprPtr Parser::parseAlt() {
  ExprPtr lhs = parseNot();
  while (atCont(TokenKind::Bar)) {
    const SourceLocation loc = advance().loc;
    lhs = bin(EBinary::Op::Alt, std::move(lhs), parseNot(), loc);
  }
  return lhs;
}

ExprPtr Parser::parseNot() {
  if (at(TokenKind::Not)) {
    const Depth guard(*this);
    const SourceLocation loc = advance().loc;
    return mk(EUnary{.op = EUnary::Op::Not, .operand = parseNot()}, loc);
  }
  return parseCmp();
}

ExprPtr Parser::parseCmp() {
  ExprPtr lhs = parseConcat();
  if (const auto op = comparisonOp(cur().kind); op && !breaksLine()) {
    const SourceLocation loc = advance().loc;
    ExprPtr rhs              = parseConcat();
    if (comparisonOp(cur().kind) && !breaksLine()) {
      failHere(ParseErrorCode::NonAssociativeComparison,
               "comparison does not chain; write 'a < b & b < c' instead of "
               "'a < b < c'");
    }
    return bin(*op, std::move(lhs), std::move(rhs), loc);
  }
  return lhs;
}

ExprPtr Parser::parseConcat() {
  ExprPtr lhs = parseAdd();
  while (atCont(TokenKind::Concat)) {
    const SourceLocation loc = advance().loc;
    lhs = bin(EBinary::Op::Concat, std::move(lhs), parseAdd(), loc);
  }
  return lhs;
}

ExprPtr Parser::parseAdd() {
  ExprPtr lhs = parseMul();
  while (atCont(TokenKind::Plus) || atCont(TokenKind::Minus)) {
    const Token &op = advance();
    lhs = bin(op.kind == TokenKind::Plus ? EBinary::Op::Add : EBinary::Op::Sub,
              std::move(lhs), parseMul(), op.loc);
  }
  return lhs;
}

ExprPtr Parser::parseMul() {
  ExprPtr lhs = parseUnary();
  while (atCont(TokenKind::Star) || atCont(TokenKind::Slash) ||
         atCont(TokenKind::Percent)) {
    const Token &op = advance();
    const auto kind = op.kind == TokenKind::Star    ? EBinary::Op::Mul
                      : op.kind == TokenKind::Slash ? EBinary::Op::Div
                                                    : EBinary::Op::Mod;
    lhs             = bin(kind, std::move(lhs), parseUnary(), op.loc);
  }
  return lhs;
}

ExprPtr Parser::parseUnary() {
  if (at(TokenKind::Minus) || at(TokenKind::Bang) || at(TokenKind::Star)) {
    const Depth guard(*this);
    const Token &op = advance();
    const auto kind = op.kind == TokenKind::Minus  ? EUnary::Op::Neg
                      : op.kind == TokenKind::Bang ? EUnary::Op::Generate
                                                   : EUnary::Op::Size;
    return mk(EUnary{.op = kind, .operand = parseUnary()}, op.loc);
  }
  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  ExprPtr base = parsePrimary();
  for (;;) {
    const SourceLocation loc = cur().loc;
    if (at(TokenKind::OpenParen) && !breaksLine()) {
      ECall c;
      c.callee = std::move(base);
      c.args   = parseCallArgs();
      base     = mk(std::move(c), loc);
    } else if (at(TokenKind::OpenBracket) && !breaksLine()) {
      advance();
      ExprPtr index;
      {
        const Scope scope(*this, false, false);
        index = parseExpr();
      }
      expectClose(TokenKind::CloseBracket);
      base =
          mk(EIndex{.base = std::move(base), .index = std::move(index)}, loc);
    } else if (atCont(TokenKind::Dot)) {
      advance();
      base = mk(EField{.base = std::move(base),
                       .name = expectWord("a field or method name after '.'")},
                loc);
    } else {
      return base;
    }
  }
}

ExprPtr Parser::parseRecordLiteral(std::string type, SourceLocation loc) {
  expect(TokenKind::OpenBrace, "'{'");
  const Scope scope(*this, false, false);
  ERecord r;
  r.type = std::move(type);
  while (!at(TokenKind::CloseBrace)) {
    RecordField f;
    f.name = expectWord("a field name");
    expect(TokenKind::Colon, "':' after the field name");
    f.value = parseExpr();
    r.fields.push_back(std::move(f));
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  expectClose(TokenKind::CloseBrace);
  return mk(std::move(r), loc);
}

ExprPtr Parser::parsePrimary() {
  const Token &t           = cur();
  const SourceLocation loc = t.loc;
  switch (t.kind) {
  case TokenKind::IntegerLiteral: {
    ELiteral l = literal(ELiteral::Kind::Int);
    l.intValue = t.intValue;
    advance();
    return mk(std::move(l), loc);
  }
  case TokenKind::FloatLiteral: {
    ELiteral l   = literal(ELiteral::Kind::Float);
    l.floatValue = t.floatValue;
    advance();
    return mk(std::move(l), loc);
  }
  case TokenKind::StringLiteral: {
    ELiteral l = literal(ELiteral::Kind::Str);
    l.text     = t.stringValue;
    advance();
    return mk(std::move(l), loc);
  }
  case TokenKind::DurationLiteral: {
    ELiteral l   = literal(ELiteral::Kind::Duration);
    l.durationNs = t.durationNs;
    advance();
    return mk(std::move(l), loc);
  }
  case TokenKind::True:
  case TokenKind::False: {
    ELiteral l  = literal(ELiteral::Kind::Bool);
    l.boolValue = t.kind == TokenKind::True;
    advance();
    return mk(std::move(l), loc);
  }
  case TokenKind::None:
    advance();
    return mk(literal(ELiteral::Kind::None), loc);
  case TokenKind::Self:
    advance();
    return mk(ESpecial{ESpecial::Kind::Self}, loc);
  case TokenKind::Identifier: {
    const std::string name(t.text);
    advance();
    if (name == "parent") {
      return mk(ESpecial{ESpecial::Kind::Parent}, loc);
    }
    if (name == "inner") {
      return mk(ESpecial{ESpecial::Kind::Inner}, loc);
    }
    if (name == "outer") {
      return mk(ESpecial{ESpecial::Kind::Outer}, loc);
    }
    if (name == "occurrence") {
      return mk(ESpecial{ESpecial::Kind::Occurrence}, loc);
    }
    if (isPascalCase(name) && at(TokenKind::OpenBrace) && !noRecord_ &&
        !breaksLine()) {
      return parseRecordLiteral(name, loc);
    }
    return mk(EName{name}, loc);
  }
  case TokenKind::OpenParen: {
    advance();
    const Scope scope(*this, false, false);
    if (accept(TokenKind::CloseParen)) {
      return mk(literal(ELiteral::Kind::Unit), loc);
    }
    ExprPtr e = parseExpr();
    expectClose(TokenKind::CloseParen);
    return e;
  }
  case TokenKind::OpenBrace:
    return parseBlock();
  case TokenKind::OpenBracket: {
    advance();
    const Scope scope(*this, false, false);
    EList list;
    while (!at(TokenKind::CloseBracket)) {
      list.elems.push_back(parseExpr());
      if (!accept(TokenKind::Comma)) {
        break;
      }
    }
    expectClose(TokenKind::CloseBracket);
    return mk(std::move(list), loc);
  }
  case TokenKind::If:
    return parseIf();
  case TokenKind::While:
    return parseWhile();
  case TokenKind::For:
    return parseFor();
  case TokenKind::Believes: {
    advance();
    EBelieves b;
    b.term = parseTerm();
    if (atWord("from") && !breaksLine()) {
      advance();
      b.from = parsePostfix();
    }
    return mk(std::move(b), loc);
  }
  case TokenKind::Findall: {
    advance();
    expect(TokenKind::OpenParen, "'(' after findall");
    const Scope scope(*this, false, false);
    EFindall f;
    f.templ = parseTermArg();
    expect(TokenKind::Comma, "',' between the template and the goal");
    f.goal = parseTerm();
    expectClose(TokenKind::CloseParen);
    return mk(std::move(f), loc);
  }
  default:
    return parseAction();
  }
}

ExprPtr Parser::parseIf() {
  const SourceLocation loc = expect(TokenKind::If, "'if'").loc;
  EIf e;
  e.cond = parseExpr();
  expect(TokenKind::Then, "'then' after the condition");
  e.thenBranch = parseExpr();
  // `else` binds to the nearest unmatched if/achieve/ask/await: whichever
  // parse is innermost and still looking for one takes it first.
  if (atCont(TokenKind::Else)) {
    advance();
    e.elseBranch = parseExpr();
  }
  return mk(std::move(e), loc);
}

ExprPtr Parser::parseWhile() {
  const SourceLocation loc = expect(TokenKind::While, "'while'").loc;
  EWhile w;
  w.cond = parseExpr();
  expect(TokenKind::Do, "'do' after the condition");
  w.body = parseExpr();
  return mk(std::move(w), loc);
}

ExprPtr Parser::parseFor() {
  const SourceLocation loc = expect(TokenKind::For, "'for'").loc;
  EFor f;
  if (accept(TokenKind::Believes)) {
    f.believesTerm = parseTerm();
  } else {
    f.binder = expectName("a loop variable");
    expect(TokenKind::In, "'in' after the loop variable");
    f.iterable = parseExpr();
  }
  expect(TokenKind::Do, "'do' before the loop body");
  f.body = parseExpr();
  return mk(std::move(f), loc);
}

// -- terms
// ----------------------------------------------------------------------

TermArg Parser::parseTermArg() {
  TermArg a;
  if (accept(TokenKind::QuestionMark)) {
    a.logicVar = expectName("a name after '?'");
    return a;
  }
  a.expr = parseExpr();
  return a;
}

Term Parser::parseTerm() {
  Term t;
  t.loc = cur().loc;
  if (!at(TokenKind::Identifier) || peek(1).kind != TokenKind::OpenParen) {
    failHere(ParseErrorCode::InvalidTerm,
             "expected a term such as name(args), found " + describe(cur()));
  }
  t.name = std::string(advance().text);
  advance(); // '('
  const Scope scope(*this, false, false);
  while (!at(TokenKind::CloseParen)) {
    t.args.push_back(parseTermArg());
    if (!accept(TokenKind::Comma)) {
      break;
    }
  }
  expectClose(TokenKind::CloseParen);
  return t;
}

GoalCall Parser::parseGoalCall() {
  GoalCall g;
  g.loc  = cur().loc;
  g.name = expectName("a goal name");
  g.args = parseCallArgs();
  return g;
}

// -- actions (§8.6)
// -------------------------------------------------------------

ExprPtr Parser::parseOptionalElse() {
  if (atCont(TokenKind::Else)) {
    advance();
    return parseExpr();
  }
  return nullptr;
}

ExprPtr Parser::parseWithin(std::string_view who) {
  if (!atCont(TokenKind::Within)) {
    failHere(ParseErrorCode::MissingWithin,
             std::string(who) + " needs 'within <duration>', found " +
                 describe(cur()));
  }
  advance();
  return parsePostfix();
}

ExprPtr Parser::parseAchieve() {
  const SourceLocation loc = expect(TokenKind::Achieve, "'achieve'").loc;
  EAchieve a;
  a.goal = parseGoalCall();
  if (atCont(TokenKind::By)) {
    advance();
    a.by     = parsePostfix();
    a.within = parseWithin("'achieve ... by'");
  }
  a.orElse = parseOptionalElse();
  return mk(std::move(a), loc);
}

ExprPtr Parser::parseAsk() {
  const SourceLocation loc = expect(TokenKind::Ask, "'ask'").loc;
  EAsk a;
  a.question = parsePostfix();
  a.within   = parseWithin("'ask'");
  a.orElse   = parseOptionalElse();
  return mk(std::move(a), loc);
}

ExprPtr Parser::parseAwait() {
  const SourceLocation loc = expect(TokenKind::Await, "'await'").loc;
  EAwait a;
  if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Identifier &&
      peek(1).text == "as") {
    a.eventType = std::string(advance().text);
    advance(); // as
    a.binder = expectName("a name for the event");
  } else {
    a.term = parseTerm();
  }
  if (atCont(TokenKind::OpenBracket)) {
    advance();
    {
      const Scope scope(*this, false, false);
      a.guard = parseExpr();
    }
    expectClose(TokenKind::CloseBracket);
  }
  a.within = parseWithin("'await'");
  a.orElse = parseOptionalElse();
  return mk(std::move(a), loc);
}

ExprPtr Parser::parseInstallLike() {
  const Token &kw          = advance();
  const SourceLocation loc = kw.loc;
  EInstall i;
  switch (kw.kind) {
  case TokenKind::Install:
    i.kind = EInstall::Kind::Install;
    break;
  case TokenKind::Uninstall:
    i.kind = EInstall::Kind::Uninstall;
    break;
  case TokenKind::Mount:
    i.kind = EInstall::Kind::Mount;
    break;
  default:
    i.kind = EInstall::Kind::Unmount;
    break;
  }
  i.name = expectName("a name");
  const bool takesArgs =
      i.kind == EInstall::Kind::Install || i.kind == EInstall::Kind::Mount;
  if (takesArgs && at(TokenKind::OpenParen) && !breaksLine()) {
    i.hasParens = true;
    i.args      = parseCallArgs();
  }
  return mk(std::move(i), loc);
}

ExprPtr Parser::parseAction() {
  const SourceLocation loc = cur().loc;
  switch (cur().kind) {
  case TokenKind::Emit: {
    advance();
    EEmit e;
    e.event = parsePostfix();
    if (atCont(TokenKind::In)) {
      advance();
      e.space = parsePostfix();
    }
    if (atCont(TokenKind::For)) {
      advance();
      e.recipients = parsePostfix();
    }
    return mk(std::move(e), loc);
  }
  case TokenKind::Tell:
  case TokenKind::Untell: {
    ETell t;
    t.untell = advance().kind == TokenKind::Untell;
    t.to     = parsePostfix();
    t.fact   = parseTerm();
    return mk(std::move(t), loc);
  }
  case TokenKind::Believe:
  case TokenKind::Forget:
  case TokenKind::Revise: {
    EBeliefChange b;
    const TokenKind k = advance().kind;
    b.kind            = k == TokenKind::Believe  ? EBeliefChange::Kind::Believe
                        : k == TokenKind::Forget ? EBeliefChange::Kind::Forget
                                                 : EBeliefChange::Kind::Revise;
    b.fact            = parseTerm();
    return mk(std::move(b), loc);
  }
  case TokenKind::Spawn: {
    advance();
    ESpawn s;
    s.agent = expectName("an agent name");
    s.args  = parseCallArgs();
    if (atCont(TokenKind::In)) {
      advance();
      s.space = parsePostfix();
    }
    return mk(std::move(s), loc);
  }
  case TokenKind::Adopt:
    advance();
    return mk(EAdopt{.goal = parseGoalCall()}, loc);
  case TokenKind::Achieve:
    return parseAchieve();
  case TokenKind::Ask:
    return parseAsk();
  case TokenKind::Await:
    return parseAwait();
  case TokenKind::Drop:
    advance();
    return mk(EDrop{.target = parsePostfix()}, loc);
  case TokenKind::Stop:
    advance();
    return mk(EStop{.target = parsePostfix()}, loc);
  case TokenKind::Terminate:
    advance();
    return mk(ETerminate{}, loc);
  case TokenKind::Install:
  case TokenKind::Uninstall:
  case TokenKind::Mount:
  case TokenKind::Unmount:
    return parseInstallLike();
  case TokenKind::Return: {
    advance();
    EReturn r;
    if (startsExpression(cur().kind) && !breaksLine()) {
      r.value = parseExpr();
    }
    return mk(std::move(r), loc);
  }
  case TokenKind::Yield:
    advance();
    return mk(EYield{}, loc);
  case TokenKind::Consume:
    advance();
    return mk(EConsume{}, loc);
  case TokenKind::Fail:
    advance();
    return mk(EFail{}, loc);
  default:
    failHere(ParseErrorCode::ExpectedExpression,
             "expected an expression, found " + describe(cur()));
  }
}

} // namespace xanadu::vagent
