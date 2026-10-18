/**
 * @file parser.cpp
 * @brief vicon parser: precedence climbing over Icon's operator table
 *        (spec §8.2; the table is pinned in parser.hpp).
 */
#include "common/xanadu/vicon/parser.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "common/xanadu/vicon/lexer.hpp"

namespace xanadu::vicon {

namespace {

// Precedence levels for the climbing loop; a higher number binds tighter.
// The ordering is the one parser.hpp documents loosest-first: assignment,
// alternation/limitation, `to`/`by`, `?`, `&`, comparisons, `||`, `+`/`-`,
// `*`/`/`/`%`, then `^` above the prefix unary operators. `&` binds tighter
// than `?` so a trailing conjunction stays inside the scanning environment:
// `s ? e1 & e2` is `s ? (e1 & e2)`.
constexpr int kPrecAssign = 1;  // := <-
constexpr int kPrecAltern = 2;  // | and \ limitation
constexpr int kPrecTo     = 3;  // to / by — non-associative
constexpr int kPrecScan   = 4;  // ? string scanning
constexpr int kPrecConj   = 5;  // & conjunction
constexpr int kPrecRel    = 6;  // comparisons (produce right or fail)
constexpr int kPrecConcat = 7;  // ||
constexpr int kPrecAdd    = 8;  // + -
constexpr int kPrecMul    = 9;  // * / %
constexpr int kPrecPow    = 10; // ^ — right-assoc, above prefix unary

struct Infix {
  int prec;
  bool right; ///< right-associative operand climbs at the same level
};

/// Infix level for a token, or nullopt when it cannot continue an
/// expression (the climbing loop's stop condition).
std::optional<Infix> infixOf(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Assign:
  case TokenKind::RevAssign:
    return Infix{kPrecAssign, true};
  case TokenKind::Alternation:
  case TokenKind::Limit:
    return Infix{kPrecAltern, true};
  case TokenKind::To:
    return Infix{kPrecTo, false};
  case TokenKind::Ampersand:
    return Infix{kPrecConj, false};
  case TokenKind::Scan:
    return Infix{kPrecScan, false};
  case TokenKind::Equal:
  case TokenKind::NotEqual:
  case TokenKind::Less:
  case TokenKind::LessEqual:
  case TokenKind::Greater:
  case TokenKind::GreaterEqual:
  case TokenKind::StringEqual:
  case TokenKind::StringNotEq:
  case TokenKind::StringLess:
  case TokenKind::StringLessEq:
  case TokenKind::StringGreater:
  case TokenKind::StringGtrEq:
    return Infix{kPrecRel, false};
  case TokenKind::Concat:
    return Infix{kPrecConcat, false};
  case TokenKind::Plus:
  case TokenKind::Minus:
    return Infix{kPrecAdd, false};
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
    return Infix{kPrecMul, false};
  case TokenKind::Caret:
    return Infix{kPrecPow, true};
  default:
    return std::nullopt;
  }
}

/// The EBinary op for the infix tokens the generic path handles. Alternation,
/// limitation, `to`, `&`, and `?` are non-EBinary nodes handled case-wise in
/// parseExpr().
EBinary::Op binaryOpOf(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Plus:
    return EBinary::Op::Add;
  case TokenKind::Minus:
    return EBinary::Op::Sub;
  case TokenKind::Star:
    return EBinary::Op::Mul;
  case TokenKind::Slash:
    return EBinary::Op::Div;
  case TokenKind::Percent:
    return EBinary::Op::Mod;
  case TokenKind::Caret:
    return EBinary::Op::Pow;
  case TokenKind::Concat:
    return EBinary::Op::Concat;
  case TokenKind::Less:
    return EBinary::Op::Lt;
  case TokenKind::LessEqual:
    return EBinary::Op::Le;
  case TokenKind::Equal:
    return EBinary::Op::Eq;
  case TokenKind::NotEqual:
    return EBinary::Op::Ne;
  case TokenKind::Greater:
    return EBinary::Op::Gt;
  case TokenKind::GreaterEqual:
    return EBinary::Op::Ge;
  case TokenKind::StringLess:
    return EBinary::Op::StrLt;
  case TokenKind::StringLessEq:
    return EBinary::Op::StrLe;
  case TokenKind::StringEqual:
    return EBinary::Op::StrEq;
  case TokenKind::StringNotEq:
    return EBinary::Op::StrNe;
  case TokenKind::StringGreater:
    return EBinary::Op::StrGt;
  case TokenKind::StringGtrEq:
    return EBinary::Op::StrGe;
  case TokenKind::Assign:
    return EBinary::Op::Assign;
  case TokenKind::RevAssign:
    return EBinary::Op::RevAssign;
  case TokenKind::Scan:
    return EBinary::Op::Scan;
  default:
    return EBinary::Op::Add; // unreachable — guarded by infixOf()
  }
}

/// Can a token begin an expression? Guards the optional operands of `break`,
/// `return`, `suspend`, `do` bodies, and the `?` scan. Kinds that only lead
/// to a named refusal inside (unary `+`/`?`/`@`, `[` lists) are included so
/// the refusal surfaces with its own reason.
bool startsExpr(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Identifier:
  case TokenKind::Keyword:
  case TokenKind::IntegerLiteral:
  case TokenKind::RealLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CsetLiteral:
  case TokenKind::OpenParen:
  case TokenKind::OpenBrace:
  case TokenKind::OpenBracket:
  case TokenKind::Bang:
  case TokenKind::Minus:
  case TokenKind::Plus:
  case TokenKind::Not:
  case TokenKind::Scan:
  case TokenKind::At:
  case TokenKind::If:
  case TokenKind::While:
  case TokenKind::Until:
  case TokenKind::Every:
  case TokenKind::Case:
  case TokenKind::Repeat:
  case TokenKind::Break:
  case TokenKind::Next:
  case TokenKind::Return:
  case TokenKind::Fail:
  case TokenKind::Suspend:
    return true;
  default:
    return false;
  }
}

template <typename N> ExprPtr mkExpr(N &&n, SourceLocation loc) {
  return std::make_unique<Expr>(std::forward<N>(n), loc);
}

std::unexpected<ParseFailure> perr(ParseError code, std::string detail,
                                   SourceLocation loc) {
  return std::unexpected(
      ParseFailure{.code = code, .detail = std::move(detail), .loc = loc});
}

/// The lexer's Error tokens carry their message in stringValue; fall back to
/// the raw text if a producer left it empty.
std::string tokenMessage(const Token &t) {
  return t.stringValue.empty() ? std::string(t.text)
                               : std::string(t.stringValue);
}

std::string_view parseErrorName(ParseError e) noexcept {
  switch (e) {
  case ParseError::LexicalError:
    return "LexicalError";
  case ParseError::UnexpectedToken:
    return "UnexpectedToken";
  case ParseError::UnbalancedParen:
    return "UnbalancedParen";
  case ParseError::UnbalancedBrace:
    return "UnbalancedBrace";
  case ParseError::UnbalancedScan:
    return "UnbalancedScan";
  case ParseError::MissingThen:
    return "MissingThen";
  case ParseError::MissingDo:
    return "MissingDo";
  case ParseError::MissingEnd:
    return "MissingEnd";
  case ParseError::ExpectedExpr:
    return "ExpectedExpr";
  case ParseError::Unimplemented:
    return "Unimplemented";
  }
  return "Unknown";
}

} // namespace

Parser::Parser(std::string_view source) : tokens_(Lexer(source).tokenize()) {}

// -- Token plumbing ----------------------------------------------------------

const Token &Parser::peek(std::size_t ahead) const {
  static const Token eof{}; // EndOfFile sentinel for an empty stream
  if (tokens_.empty()) {
    return eof;
  }
  const std::size_t i =
      pos_ + ahead < tokens_.size() ? pos_ + ahead : tokens_.size() - 1;
  return tokens_[i];
}

const Token &Parser::advance() {
  const Token &t = peek();
  if (pos_ + 1 < tokens_.size()) {
    ++pos_;
  }
  return t;
}

bool Parser::at(TokenKind k) const { return peek().is(k); }

bool Parser::accept(TokenKind k) {
  if (!at(k)) {
    return false;
  }
  advance();
  return true;
}

std::expected<Token, ParseFailure> Parser::expect(TokenKind k, ParseError err,
                                                  std::string_view what) {
  if (at(k)) {
    return advance();
  }
  // A lexer's Error surfaces as itself, not as a masked "expected" failure.
  if (at(TokenKind::Error)) {
    return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
  }
  return perr(err,
              std::string("expected ") + std::string(what) + ", got '" +
                  std::string(tokenKindName(peek().kind)) + "'",
              peek().loc);
}

void Parser::skipSeparators() {
  while (accept(TokenKind::Semicolon)) {
  }
}

// -- Program ------------------------------------------------------------------

std::expected<Program, ParseFailure> Parser::parseProgram() {
  Program prog;
  for (;;) {
    skipSeparators();
    switch (peek().kind) {
    case TokenKind::EndOfFile:
      return prog;
    case TokenKind::Error:
      return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
    case TokenKind::End:
      return perr(ParseError::UnexpectedToken, "'end' without 'procedure'",
                  peek().loc);
    case TokenKind::Procedure: {
      auto p = parseProcedure();
      if (!p) return std::unexpected(p.error());
      prog.procedures.push_back(std::move(*p));
      break;
    }
    default: {
      auto e = parseExpr();
      if (!e) return std::unexpected(e.error());
      prog.statements.push_back(std::move(*e));
      break;
    }
    }
    // Statements and declarations are `;`- or newline-separated; the lexer
    // already turned separating newlines into Semicolons, so anything else
    // following here is a syntax error, not juxtaposition.
    if (at(TokenKind::Error)) {
      return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
    }
    if (!at(TokenKind::EndOfFile) && !at(TokenKind::Semicolon) &&
        !at(TokenKind::End)) {
      return perr(ParseError::UnexpectedToken,
                  "expected ';' or newline between statements", peek().loc);
    }
  }
}

std::expected<ExprPtr, ParseFailure> Parser::parseExpression() {
  skipSeparators();
  if (at(TokenKind::EndOfFile)) {
    return perr(ParseError::ExpectedExpr, "empty input", peek().loc);
  }
  auto first = parseExpr();
  if (!first) return first;
  // A REPL line may carry several `;`-separated statements; they behave as an
  // explicit sequence, whose results are the last element's.
  if (at(TokenKind::Semicolon)) {
    ESequence seq;
    seq.exprs.push_back(std::move(*first));
    while (accept(TokenKind::Semicolon)) {
      skipSeparators();
      if (!startsExpr(peek().kind)) break;
      auto e = parseExpr();
      if (!e) return std::unexpected(e.error());
      seq.exprs.push_back(std::move(*e));
    }
    const SourceLocation seqLoc = seq.exprs.front()->loc;
    first                       = mkExpr(std::move(seq), seqLoc);
  }
  skipSeparators();
  if (at(TokenKind::Error)) {
    return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
  }
  if (!at(TokenKind::EndOfFile)) {
    return perr(ParseError::UnexpectedToken, "trailing input after expression",
                peek().loc);
  }
  return first;
}

// -- Expressions
// ---------------------------------------------------------------

std::expected<ExprPtr, ParseFailure> Parser::parseExpr(int minPrec) {
  auto left = parseUnary();
  if (!left) return std::unexpected(left.error());

  bool consumedTo = false; // `to` is non-associative; a second one is refused
  for (;;) {
    const Token &op = peek();
    const auto info = infixOf(op.kind);
    if (!info || info->prec < minPrec) {
      break;
    }
    const SourceLocation opLoc = op.loc;
    advance();
    switch (op.kind) {
    case TokenKind::To: {
      if (consumedTo) {
        return perr(ParseError::UnexpectedToken,
                    "'to' does not chain; add parentheses", opLoc);
      }
      auto hi = parseExpr(info->prec + 1);
      if (!hi) return std::unexpected(hi.error());
      ExprPtr by;
      if (accept(TokenKind::By)) {
        auto step = parseExpr(info->prec + 1);
        if (!step) return std::unexpected(step.error());
        by = std::move(*step);
      }
      left       = mkExpr(ETo{.lo = std::move(*left),
                              .hi = std::move(*hi),
                              .by = std::move(by)},
                          opLoc);
      consumedTo = true;
      break;
    }
    case TokenKind::Alternation: {
      auto rhs = parseExpr(info->prec);
      if (!rhs) return std::unexpected(rhs.error());
      left = mkExpr(
          EAlternation{.first = std::move(*left), .second = std::move(*rhs)},
          opLoc);
      break;
    }
    case TokenKind::Limit: {
      auto rhs = parseExpr(info->prec);
      if (!rhs) return std::unexpected(rhs.error());
      left = mkExpr(ELimit{.expr = std::move(*left), .count = std::move(*rhs)},
                    opLoc);
      break;
    }
    case TokenKind::Ampersand: {
      // `e1 & e2` is conjunction: e1 is bounded and the result is e2's —
      // exactly the semantics ESequence encodes.
      auto rhs = parseExpr(info->prec + 1);
      if (!rhs) return std::unexpected(rhs.error());
      ESequence seq;
      seq.exprs.push_back(std::move(*left));
      seq.exprs.push_back(std::move(*rhs));
      left = mkExpr(std::move(seq), opLoc);
      break;
    }
    case TokenKind::Scan: {
      if (!startsExpr(peek().kind)) {
        return perr(ParseError::UnbalancedScan,
                    "'?' scan needs a following expression", peek().loc);
      }
      auto rhs = parseExpr(info->prec + 1);
      if (!rhs) return std::unexpected(rhs.error());
      left = mkExpr(EBinary{.op    = EBinary::Op::Scan,
                            .left  = std::move(*left),
                            .right = std::move(*rhs)},
                    opLoc);
      break;
    }
    default: {
      const int rhsPrec = info->right ? info->prec : info->prec + 1;
      auto rhs          = parseExpr(rhsPrec);
      if (!rhs) return std::unexpected(rhs.error());
      left = mkExpr(EBinary{.op    = binaryOpOf(op.kind),
                            .left  = std::move(*left),
                            .right = std::move(*rhs)},
                    opLoc);
      break;
    }
    }
  }
  return left;
}

std::expected<ExprPtr, ParseFailure> Parser::parseUnary() {
  const Token &t = peek();
  switch (t.kind) {
  case TokenKind::Bang:
  case TokenKind::Minus:
  case TokenKind::Not: {
    const SourceLocation loc = t.loc;
    advance();
    // The design table places `^` above the prefix operators, so a unary
    // operand climbs at kPrecPow: `-i ^ j` parses as `-(i ^ j)`.
    auto operand = parseExpr(kPrecPow);
    if (!operand) return std::unexpected(operand.error());
    const EUnary::Op op = t.is(TokenKind::Bang)    ? EUnary::Op::Bang
                          : t.is(TokenKind::Minus) ? EUnary::Op::Neg
                                                   : EUnary::Op::Not;
    return mkExpr(EUnary{.op = op, .operand = std::move(*operand)}, loc);
  }
  case TokenKind::Plus:
    return perr(ParseError::Unimplemented,
                "unary '+' is not in the vicon subset", t.loc);
  case TokenKind::Scan:
    return perr(ParseError::Unimplemented,
                "unary '?' (random element) is not in the vicon subset", t.loc);
  case TokenKind::At:
    return perr(ParseError::Unimplemented,
                "co-expression activation '@' is deferred to a later "
                "iteration",
                t.loc);
  default: {
    auto prim = parsePrimary();
    if (!prim) return std::unexpected(prim.error());
    return parsePostfix(std::move(*prim));
  }
  }
}

std::expected<ExprPtr, ParseFailure> Parser::parsePostfix(ExprPtr base) {
  for (;;) {
    if (at(TokenKind::OpenParen)) {
      // ECall carries a bare name, so `expr(args)` is not representable:
      // only `name(args)` parses as a call.
      if (!std::holds_alternative<EIdent>(base->node)) {
        return perr(ParseError::UnexpectedToken,
                    "call target must be a procedure name", peek().loc);
      }
      const SourceLocation loc = base->loc;
      ECall call;
      call.name = std::get<EIdent>(base->node).name;
      advance(); // (
      skipSeparators();
      if (!at(TokenKind::CloseParen)) {
        for (;;) {
          auto arg = parseExpr();
          if (!arg) return std::unexpected(arg.error());
          call.args.push_back(std::move(*arg));
          skipSeparators();
          if (!accept(TokenKind::Comma)) break;
          skipSeparators();
        }
      }
      auto cp = expect(TokenKind::CloseParen, ParseError::UnbalancedParen,
                       "')' after arguments");
      if (!cp) return std::unexpected(cp.error());
      base = mkExpr(std::move(call), loc);
      continue;
    }
    if (at(TokenKind::OpenBracket)) {
      const SourceLocation loc = base->loc;
      advance(); // [
      auto i = parseExpr();
      if (!i) return std::unexpected(i.error());
      if (accept(TokenKind::Colon)) {
        auto j = parseExpr();
        if (!j) return std::unexpected(j.error());
        auto cb = expect(TokenKind::CloseBracket, ParseError::UnexpectedToken,
                         "']' after slice");
        if (!cb) return std::unexpected(cb.error());
        base = mkExpr(ESlice{.base = std::move(base),
                             .from = std::move(*i),
                             .to   = std::move(*j)},
                      loc);
      } else {
        auto cb = expect(TokenKind::CloseBracket, ParseError::UnexpectedToken,
                         "']' after subscript");
        if (!cb) return std::unexpected(cb.error());
        base = mkExpr(
            ESubscript{.base = std::move(base), .index = std::move(*i)}, loc);
      }
      continue;
    }
    break;
  }
  return base;
}

std::expected<ExprPtr, ParseFailure> Parser::parsePrimary() {
  const Token &t           = peek();
  const SourceLocation loc = t.loc;
  switch (t.kind) {
  case TokenKind::Error:
    return perr(ParseError::LexicalError, tokenMessage(t), loc);
  case TokenKind::IntegerLiteral: {
    ELiteral lit;
    lit.kind     = ELiteral::Kind::Int;
    lit.intValue = t.intValue;
    advance();
    return mkExpr(std::move(lit), loc);
  }
  case TokenKind::RealLiteral: {
    ELiteral lit;
    lit.kind      = ELiteral::Kind::Real;
    lit.realValue = t.floatValue;
    advance();
    return mkExpr(std::move(lit), loc);
  }
  case TokenKind::StringLiteral:
  case TokenKind::CsetLiteral: {
    ELiteral lit;
    lit.kind = t.is(TokenKind::StringLiteral) ? ELiteral::Kind::Str
                                              : ELiteral::Kind::Cset;
    lit.text = t.stringValue;
    advance();
    return mkExpr(std::move(lit), loc);
  }
  case TokenKind::Identifier: {
    std::string name(t.text);
    advance();
    return mkExpr(EIdent{.name = std::move(name)}, loc);
  }
  case TokenKind::Keyword: {
    std::string name(t.stringValue);
    if (name.empty() && t.text.starts_with('&')) {
      name = std::string(t.text.substr(1));
    }
    advance();
    return mkExpr(EKeyword{.name = std::move(name)}, loc);
  }
  case TokenKind::OpenParen: {
    advance(); // (
    skipSeparators();
    if (at(TokenKind::CloseParen)) {
      return perr(ParseError::ExpectedExpr, "empty '()' group", peek().loc);
    }
    auto first = parseExpr();
    if (!first) return first;
    if (!at(TokenKind::Semicolon)) {
      auto cp =
          expect(TokenKind::CloseParen, ParseError::UnbalancedParen, "')'");
      if (!cp) return std::unexpected(cp.error());
      return first; // plain grouping
    }
    ESequence seq;
    seq.exprs.push_back(std::move(*first));
    while (accept(TokenKind::Semicolon)) {
      skipSeparators();
      if (at(TokenKind::CloseParen)) break;
      auto e = parseExpr();
      if (!e) return std::unexpected(e.error());
      seq.exprs.push_back(std::move(*e));
    }
    auto cp = expect(TokenKind::CloseParen, ParseError::UnbalancedParen, "')'");
    if (!cp) return std::unexpected(cp.error());
    return mkExpr(std::move(seq), loc);
  }
  case TokenKind::OpenBrace:
    return parseBraceSeq();
  case TokenKind::OpenBracket:
    return perr(ParseError::Unimplemented,
                "list construction '[...]' is not in the vicon subset", loc);
  case TokenKind::If:
    return parseIf();
  case TokenKind::While:
    return parseWhileOrUntil(true);
  case TokenKind::Until:
    return parseWhileOrUntil(false);
  case TokenKind::Every:
    return parseEvery();
  case TokenKind::Case:
    return parseCase();
  case TokenKind::Repeat:
    return parseRepeat();
  case TokenKind::Break: {
    advance();
    ExprPtr result;
    if (startsExpr(peek().kind)) {
      auto e = parseExpr();
      if (!e) return std::unexpected(e.error());
      result = std::move(*e);
    }
    return mkExpr(EBreak{.result = std::move(result)}, loc);
  }
  case TokenKind::Next:
    advance();
    return mkExpr(ENext{}, loc);
  case TokenKind::Return:
  case TokenKind::Suspend: {
    const bool isReturn = t.is(TokenKind::Return);
    advance();
    ExprPtr operand;
    if (startsExpr(peek().kind)) {
      auto e = parseExpr();
      if (!e) return std::unexpected(e.error());
      operand = std::move(*e);
    }
    if (isReturn) {
      return mkExpr(EReturn{.expr = std::move(operand)}, loc);
    }
    return mkExpr(ESuspend{.expr = std::move(operand)}, loc);
  }
  case TokenKind::Fail:
    advance();
    return mkExpr(EFail{}, loc);
  case TokenKind::Procedure:
    return perr(ParseError::UnexpectedToken,
                "'procedure' is only valid at top level", loc);
  case TokenKind::End:
    return perr(ParseError::UnexpectedToken, "'end' without 'procedure'", loc);
  case TokenKind::Local:
  case TokenKind::Global:
  case TokenKind::Static:
    return perr(ParseError::Unimplemented,
                "'" + std::string(t.text) +
                    "' declarations are not in the vicon subset",
                loc);
  default:
    return perr(ParseError::ExpectedExpr,
                std::string("expected an expression, got '") +
                    std::string(tokenKindName(t.kind)) + "'",
                loc);
  }
}

std::expected<ExprPtr, ParseFailure> Parser::parseBraceSeq() {
  const SourceLocation loc = peek().loc;
  advance(); // {
  ESequence seq;
  for (;;) {
    skipSeparators();
    if (accept(TokenKind::CloseBrace)) {
      break;
    }
    if (at(TokenKind::Error)) {
      return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
    }
    if (at(TokenKind::EndOfFile)) {
      return perr(ParseError::UnbalancedBrace, "unterminated '{' block", loc);
    }
    auto e = parseExpr();
    if (!e) return std::unexpected(e.error());
    seq.exprs.push_back(std::move(*e));
    // EOF/Error continue to the loop head, which reports the real failure.
    if (accept(TokenKind::Semicolon) || at(TokenKind::CloseBrace) ||
        at(TokenKind::EndOfFile) || at(TokenKind::Error)) {
      continue;
    }
    return perr(ParseError::UnexpectedToken, "expected ';' or '}' in block",
                peek().loc);
  }
  return mkExpr(std::move(seq), loc);
}

// -- Control structures
// ---------------------------------------------------------

std::expected<ExprPtr, ParseFailure> Parser::parseIf() {
  const SourceLocation loc = peek().loc;
  advance(); // if
  auto cond = parseExpr();
  if (!cond) return std::unexpected(cond.error());
  auto th = expect(TokenKind::Then, ParseError::MissingThen, "'then'");
  if (!th) return std::unexpected(th.error());
  auto thenE = parseExpr();
  if (!thenE) return std::unexpected(thenE.error());
  ExprPtr elseE;
  if (accept(TokenKind::Else)) {
    auto e = parseExpr();
    if (!e) return std::unexpected(e.error());
    elseE = std::move(*e);
  }
  return mkExpr(EIf{.cond       = std::move(*cond),
                    .thenBranch = std::move(*thenE),
                    .elseBranch = std::move(elseE)},
                loc);
}

std::expected<ExprPtr, ParseFailure> Parser::parseWhileOrUntil(bool isWhile) {
  const SourceLocation loc = peek().loc;
  advance(); // while / until
  auto cond = parseExpr();
  if (!cond) return std::unexpected(cond.error());
  ExprPtr body;
  if (accept(TokenKind::Do)) {
    if (!startsExpr(peek().kind)) {
      return perr(ParseError::ExpectedExpr, "'do' needs a following expression",
                  peek().loc);
    }
    auto b = parseExpr();
    if (!b) return std::unexpected(b.error());
    body = std::move(*b);
  } else if (startsExpr(peek().kind)) {
    // `while c stmt` on one line still needs `do`; name the omission rather
    // than guessing where the body begins.
    return perr(ParseError::MissingDo, "expected 'do' before the loop body",
                peek().loc);
  }
  if (isWhile) {
    return mkExpr(EWhile{.cond = std::move(*cond), .body = std::move(body)},
                  loc);
  }
  return mkExpr(EUntil{.cond = std::move(*cond), .body = std::move(body)}, loc);
}

std::expected<ExprPtr, ParseFailure> Parser::parseEvery() {
  const SourceLocation loc = peek().loc;
  advance(); // every
  auto gen = parseExpr();
  if (!gen) return std::unexpected(gen.error());
  ExprPtr body;
  if (accept(TokenKind::Do)) {
    if (!startsExpr(peek().kind)) {
      return perr(ParseError::ExpectedExpr, "'do' needs a following expression",
                  peek().loc);
    }
    auto b = parseExpr();
    if (!b) return std::unexpected(b.error());
    body = std::move(*b);
  } else if (startsExpr(peek().kind)) {
    return perr(ParseError::MissingDo, "expected 'do' before the every body",
                peek().loc);
  }
  return mkExpr(EEvery{.gen = std::move(*gen), .body = std::move(body)}, loc);
}

std::expected<ExprPtr, ParseFailure> Parser::parseCase() {
  const SourceLocation loc = peek().loc;
  advance(); // case
  auto subject = parseExpr();
  if (!subject) return std::unexpected(subject.error());
  auto of = expect(TokenKind::Of, ParseError::UnexpectedToken, "'of'");
  if (!of) return std::unexpected(of.error());
  auto ob = expect(TokenKind::OpenBrace, ParseError::UnexpectedToken,
                   "'{' after 'of'");
  if (!ob) return std::unexpected(ob.error());
  ECase node;
  node.subject    = std::move(*subject);
  bool sawDefault = false;
  for (;;) {
    skipSeparators();
    if (accept(TokenKind::CloseBrace)) {
      break;
    }
    if (at(TokenKind::Error)) {
      return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
    }
    if (at(TokenKind::EndOfFile)) {
      return perr(ParseError::UnbalancedBrace, "unterminated 'case' block",
                  loc);
    }
    auto match = parseExpr(); // stops at ':' — Colon is not an operator
    if (!match) return std::unexpected(match.error());
    auto co = expect(TokenKind::Colon, ParseError::UnexpectedToken,
                     "':' in case arm");
    if (!co) return std::unexpected(co.error());
    auto body = parseExpr();
    if (!body) return std::unexpected(body.error());
    // `default` is spelled as a bare identifier, not a reserved word.
    if (std::holds_alternative<EIdent>((*match)->node) &&
        std::get<EIdent>((*match)->node).name == "default") {
      if (sawDefault) {
        return perr(ParseError::UnexpectedToken,
                    "duplicate 'default' arm in case", (*match)->loc);
      }
      sawDefault     = true;
      node.otherwise = std::move(*body);
    } else {
      node.arms.push_back(
          ECaseArm{.match = std::move(*match), .body = std::move(*body)});
    }
    if (accept(TokenKind::Semicolon) || at(TokenKind::CloseBrace) ||
        at(TokenKind::EndOfFile) || at(TokenKind::Error)) {
      continue;
    }
    return perr(ParseError::UnexpectedToken,
                "expected ';' or '}' between case arms", peek().loc);
  }
  return mkExpr(std::move(node), loc);
}

std::expected<ExprPtr, ParseFailure> Parser::parseRepeat() {
  const SourceLocation loc = peek().loc;
  advance(); // repeat
  if (!startsExpr(peek().kind)) {
    return perr(ParseError::ExpectedExpr, "'repeat' needs an expression",
                peek().loc);
  }
  auto body = parseExpr();
  if (!body) return std::unexpected(body.error());
  return mkExpr(ERepeat{.expr = std::move(*body)}, loc);
}

// -- Declarations
// ----------------------------------------------------------------

std::expected<ProcDecl, ParseFailure> Parser::parseProcedure() {
  const SourceLocation loc = peek().loc;
  advance(); // procedure
  auto name = expect(TokenKind::Identifier, ParseError::UnexpectedToken,
                     "procedure name");
  if (!name) return std::unexpected(name.error());
  ProcDecl pd;
  pd.name = std::string(name->text);
  pd.loc  = loc;
  auto op = expect(TokenKind::OpenParen, ParseError::UnexpectedToken,
                   "'(' after procedure name");
  if (!op) return std::unexpected(op.error());
  if (!at(TokenKind::CloseParen)) {
    for (;;) {
      auto p = expect(TokenKind::Identifier, ParseError::UnexpectedToken,
                      "parameter name");
      if (!p) return std::unexpected(p.error());
      pd.params.emplace_back(p->text);
      if (!accept(TokenKind::Comma)) break;
    }
  }
  auto cp = expect(TokenKind::CloseParen, ParseError::UnbalancedParen,
                   "')' after parameters");
  if (!cp) return std::unexpected(cp.error());
  ESequence body;
  for (;;) {
    skipSeparators();
    if (at(TokenKind::End)) {
      break;
    }
    if (at(TokenKind::Error)) {
      return perr(ParseError::LexicalError, tokenMessage(peek()), peek().loc);
    }
    if (at(TokenKind::EndOfFile)) {
      return perr(ParseError::MissingEnd, "'end' to close procedure", loc);
    }
    if (at(TokenKind::Local) || at(TokenKind::Global) ||
        at(TokenKind::Static)) {
      // ProcDecl carries no declaration list, so these cannot be
      // represented — a named refusal, not a silent drop.
      return perr(ParseError::Unimplemented,
                  "'" + std::string(peek().text) +
                      "' declarations are not in the vicon subset",
                  peek().loc);
    }
    auto e = parseExpr();
    if (!e) return std::unexpected(e.error());
    body.exprs.push_back(std::move(*e));
    // EOF falls through to the loop head, which reports MissingEnd.
    if (at(TokenKind::Semicolon) || at(TokenKind::End) ||
        at(TokenKind::EndOfFile) || at(TokenKind::Error)) {
      continue;
    }
    return perr(ParseError::UnexpectedToken, "expected ';' or 'end'",
                peek().loc);
  }
  advance(); // end
  pd.body = mkExpr(std::move(body), loc);
  return pd;
}

// -- Diagnostics
// ------------------------------------------------------------------

std::string Parser::formatError(const ParseFailure &f) {
  return "vicon: " + std::to_string(f.loc.line) + ":" +
         std::to_string(f.loc.column) + ": " +
         std::string(parseErrorName(f.code)) + ": " + f.detail;
}

} // namespace xanadu::vicon
