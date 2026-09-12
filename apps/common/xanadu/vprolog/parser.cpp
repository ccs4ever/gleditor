/**
 * @file parser.cpp
 * @brief Implementation of the Prolog Pratt parser.
 */
#include "common/xanadu/vprolog/parser.hpp"

#include <sstream>

namespace xanadu::vprolog {

ParseError::ParseError(std::string_view message, SourceLocation loc)
    : std::runtime_error(std::string(message) + " at line " +
                         std::to_string(loc.line) + ", col " +
                         std::to_string(loc.column)),
      loc_(loc) {}

namespace {

enum class Assoc {
  XFX, // non-associative infix
  XFY, // right-associative infix
  YFX, // left-associative infix
  FX,  // non-associative prefix
  FY,  // associative prefix
};

struct InfixOp {
  int prec;
  Assoc assoc;
  std::string_view name;
};

struct PrefixOp {
  int prec;
  Assoc assoc;
  std::string_view name;
};

std::optional<InfixOp> getInfixOp(TokenKind kind) {
  switch (kind) {
  case TokenKind::Neck:
    return InfixOp{1200, Assoc::XFX, ":-"};
  case TokenKind::Semicolon:
    return InfixOp{1100, Assoc::XFY, ";"};
  case TokenKind::Arrow:
    return InfixOp{1050, Assoc::XFY, "->"};
  case TokenKind::Comma:
    return InfixOp{1000, Assoc::XFY, ","};
  case TokenKind::Equal:
    return InfixOp{700, Assoc::XFX, "="};
  case TokenKind::NotEqual:
    return InfixOp{700, Assoc::XFX, "\\="};
  case TokenKind::StrictEqual:
    return InfixOp{700, Assoc::XFX, "=="};
  case TokenKind::StrictNotEqual:
    return InfixOp{700, Assoc::XFX, "\\=="};
  case TokenKind::Univ:
    return InfixOp{700, Assoc::XFX, "=.."};
  case TokenKind::Is:
    return InfixOp{700, Assoc::XFX, "is"};
  case TokenKind::EqualArith:
    return InfixOp{700, Assoc::XFX, "=:="};
  case TokenKind::NotEqualArith:
    return InfixOp{700, Assoc::XFX, "=\\="};
  case TokenKind::Less:
    return InfixOp{700, Assoc::XFX, "<"};
  case TokenKind::Greater:
    return InfixOp{700, Assoc::XFX, ">"};
  case TokenKind::LessEqual:
    return InfixOp{700, Assoc::XFX, "=<"};
  case TokenKind::GreaterEqual:
    return InfixOp{700, Assoc::XFX, ">="};
  case TokenKind::Plus:
    return InfixOp{500, Assoc::YFX, "+"};
  case TokenKind::Minus:
    return InfixOp{500, Assoc::YFX, "-"};
  case TokenKind::BitAnd:
    return InfixOp{500, Assoc::YFX, "/\\"};
  case TokenKind::BitOr:
    return InfixOp{500, Assoc::YFX, "\\/"};
  case TokenKind::Star:
    return InfixOp{400, Assoc::YFX, "*"};
  case TokenKind::Slash:
    return InfixOp{400, Assoc::YFX, "/"};
  case TokenKind::IntDiv:
    return InfixOp{400, Assoc::YFX, "//"};
  case TokenKind::Mod:
    return InfixOp{400, Assoc::YFX, "mod"};
  case TokenKind::Rem:
    return InfixOp{400, Assoc::YFX, "rem"};
  case TokenKind::BitShiftLeft:
    return InfixOp{400, Assoc::YFX, "<<"};
  case TokenKind::BitShiftRight:
    return InfixOp{400, Assoc::YFX, ">>"};
  case TokenKind::Power:
    return InfixOp{200, Assoc::XFX, "**"};
  default:
    return std::nullopt;
  }
}

std::optional<PrefixOp> getPrefixOp(TokenKind kind) {
  switch (kind) {
  case TokenKind::Neck:
    return PrefixOp{1200, Assoc::FX, ":-"};
  case TokenKind::Query:
    return PrefixOp{1200, Assoc::FX, "?-"};
  case TokenKind::Not:
    return PrefixOp{900, Assoc::FY, "\\+"};
  case TokenKind::Minus:
    return PrefixOp{200, Assoc::FY, "-"};
  case TokenKind::Plus:
    return PrefixOp{200, Assoc::FY, "+"};
  default:
    return std::nullopt;
  }
}

void extractConjunction(const Term &t, std::vector<Term> &out) {
  if (const auto *comp = t.asCompound()) {
    if (comp->functor == "," && comp->args.size() == 2) {
      extractConjunction(comp->args[0], out);
      extractConjunction(comp->args[1], out);
      return;
    }
  }
  out.push_back(t);
}

} // namespace

Parser::Parser(std::string_view source) : lexer_(source) {}
Parser::Parser(Lexer lexer) : lexer_(std::move(lexer)) {}

Token Parser::advance() { return lexer_.nextToken(); }
Token Parser::peek() { return lexer_.peekToken(); }

bool Parser::isAtEnd() { return peek().is(TokenKind::EndOfFile); }

Token Parser::expect(TokenKind kind, std::string_view errMsg) {
  Token tok = advance();
  if (tok.kind != kind) {
    throw ParseError(errMsg, tok.loc);
  }
  return tok;
}

bool Parser::match(TokenKind kind) {
  if (peek().kind == kind) {
    advance();
    return true;
  }
  return false;
}

Program Parser::parseProgram() {
  Program prog;
  while (!isAtEnd()) {
    prog.clauses.push_back(parseClause());
  }
  return prog;
}

Clause Parser::parseClause() {
  SourceLocation startLoc = peek().loc;

  if (match(TokenKind::Query)) {
    Term bodyTerm = parseTerm(1200);
    expect(TokenKind::Dot, "Expected '.' after query");
    std::vector<Term> body;
    extractConjunction(bodyTerm, body);
    return Clause{.head        = Term(),
                  .body        = std::move(body),
                  .isQuery     = true,
                  .isDirective = false,
                  .loc         = startLoc};
  }

  if (peek().is(TokenKind::Neck)) {
    advance();
    Term bodyTerm = parseTerm(1200);
    expect(TokenKind::Dot, "Expected '.' after directive");
    std::vector<Term> body;
    extractConjunction(bodyTerm, body);
    return Clause{.head        = Term(),
                  .body        = std::move(body),
                  .isQuery     = false,
                  .isDirective = true,
                  .loc         = startLoc};
  }

  Term expr = parseTerm(1200);
  expect(TokenKind::Dot, "Expected '.' at end of clause");

  if (const auto *comp = expr.asCompound()) {
    if (comp->functor == ":-" && comp->args.size() == 2) {
      std::vector<Term> body;
      extractConjunction(comp->args[1], body);
      return Clause{.head        = comp->args[0],
                    .body        = std::move(body),
                    .isQuery     = false,
                    .isDirective = false,
                    .loc         = startLoc};
    }
  }

  // Fact
  return Clause{.head        = std::move(expr),
                .body        = {},
                .isQuery     = false,
                .isDirective = false,
                .loc         = startLoc};
}

Term Parser::parseTerm(int maxPrecedence) {
  Token tok = peek();

  // Prefix operator check
  auto prefOp = getPrefixOp(tok.kind);
  Term left;
  if (prefOp && prefOp->prec <= maxPrecedence) {
    advance();
    int rightPrec =
        (prefOp->assoc == Assoc::FY) ? prefOp->prec : prefOp->prec - 1;
    Term operand = parseTerm(rightPrec);
    left         = Term(Compound{std::string(prefOp->name), {operand}});
  } else {
    left = parsePrimary();
  }

  // Infix operator loop
  while (true) {
    Token nextTok = peek();
    auto infOp    = getInfixOp(nextTok.kind);
    if (!infOp || infOp->prec > maxPrecedence) {
      break;
    }

    advance(); // Consume operator

    int rightPrec;
    if (infOp->assoc == Assoc::XFY) {
      rightPrec = infOp->prec;
    } else {
      rightPrec = infOp->prec - 1;
    }

    Term right = parseTerm(rightPrec);
    left       = Term(Compound{std::string(infOp->name), {left, right}});
  }

  return left;
}

Term Parser::parsePrimary() {
  Token tok = advance();

  switch (tok.kind) {
  case TokenKind::Variable:
    return Term(Var{.name        = std::string(tok.stringValue),
                    .isAnonymous = (tok.stringValue == "_")});
  case TokenKind::Atom: {
    // Check if directly followed by '(' without whitespace
    if (peek().is(TokenKind::OpenParen) &&
        (tok.loc.offset + tok.text.size() == peek().loc.offset)) {
      advance(); // Consume '('
      std::vector<Term> args = parseArgumentList();
      expect(TokenKind::CloseParen, "Expected ')' after arguments");
      return Term(Compound{std::string(tok.stringValue), std::move(args)});
    }
    return Term(Atom{std::string(tok.stringValue)});
  }
  case TokenKind::IntegerLiteral:
    return Term(Number{tok.intValue});
  case TokenKind::FloatLiteral:
    return Term(Number{tok.floatValue});
  case TokenKind::StringLiteral:
    return Term(String{std::string(tok.stringValue)});
  case TokenKind::Cut:
    return Term(Atom{"!"});
  case TokenKind::OpenBracket:
    return parseList();
  case TokenKind::OpenParen:
    return parseParenthesized();
  case TokenKind::OpenBrace: {
    Term inner = parseTerm(1200);
    expect(TokenKind::CloseBrace, "Expected '}' after block term");
    return Term(Compound{"{}", {inner}});
  }
  default:
    throw ParseError(std::string("Unexpected token '") + std::string(tok.text) +
                         "'",
                     tok.loc);
  }
}

std::vector<Term> Parser::parseArgumentList() {
  std::vector<Term> args;
  if (peek().is(TokenKind::CloseParen)) {
    return args;
  }
  while (true) {
    args.push_back(parseTerm(999));
    if (match(TokenKind::Comma)) {
      continue;
    }
    break;
  }
  return args;
}

Term Parser::parseList() {
  // Opening '[' was already consumed
  if (match(TokenKind::CloseBracket)) {
    return Term(Atom{"[]"});
  }

  std::vector<Term> elements;
  std::shared_ptr<Term> tail = nullptr;

  while (true) {
    elements.push_back(parseTerm(999));
    if (match(TokenKind::Comma)) {
      continue;
    }
    if (match(TokenKind::Pipe)) {
      tail = std::make_shared<Term>(parseTerm(999));
      expect(TokenKind::CloseBracket, "Expected ']' after list tail");
      return Term(List{std::move(elements), tail});
    }
    break;
  }

  expect(TokenKind::CloseBracket, "Expected ']' at end of list");
  return Term(List{std::move(elements), nullptr});
}

Term Parser::parseParenthesized() {
  // Opening '(' was already consumed
  Term inner = parseTerm(1200);
  expect(TokenKind::CloseParen, "Expected ')' matching '('");
  return inner;
}

std::string formatTerm(const Term &term) {
  std::ostringstream oss;
  if (const auto *var = term.asVar()) {
    oss << var->name;
  } else if (const auto *atom = term.asAtom()) {
    oss << atom->name;
  } else if (const auto *num = term.asNumber()) {
    if (num->isFloat()) {
      oss << num->asFloat();
    } else {
      oss << num->asInt();
    }
  } else if (const auto *str = term.asString()) {
    oss << "\"" << str->value << "\"";
  } else if (const auto *list = term.asList()) {
    oss << "[";
    for (std::size_t i = 0; i < list->elements.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << formatTerm(list->elements[i]);
    }
    if (list->tail) {
      oss << " | " << formatTerm(*list->tail);
    }
    oss << "]";
  } else if (const auto *comp = term.asCompound()) {
    if (comp->functor == "." && comp->args.size() == 2) {
      oss << formatTerm(term.canonicalizeList());
    } else if (comp->args.size() == 2 &&
               (comp->functor == "=" || comp->functor == "is" ||
                comp->functor == "+" || comp->functor == "-" ||
                comp->functor == "*" || comp->functor == "/" ||
                comp->functor == "<" || comp->functor == ">" ||
                comp->functor == "=<" || comp->functor == ">=" ||
                comp->functor == ":-")) {
      oss << formatTerm(comp->args[0]) << " " << comp->functor << " "
          << formatTerm(comp->args[1]);
    } else {
      oss << comp->functor << "(";
      for (std::size_t i = 0; i < comp->args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << formatTerm(comp->args[i]);
      }
      oss << ")";
    }
  }
  return oss.str();
}

std::string formatClause(const Clause &clause) {
  std::ostringstream oss;
  if (clause.isQuery) {
    oss << "?- ";
    for (std::size_t i = 0; i < clause.body.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << formatTerm(clause.body[i]);
    }
    oss << ".";
    return oss.str();
  }
  if (clause.isDirective) {
    oss << ":- ";
    for (std::size_t i = 0; i < clause.body.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << formatTerm(clause.body[i]);
    }
    oss << ".";
    return oss.str();
  }
  oss << formatTerm(clause.head);
  if (!clause.body.empty()) {
    oss << " :- ";
    for (std::size_t i = 0; i < clause.body.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << formatTerm(clause.body[i]);
    }
  }
  oss << ".";
  return oss.str();
}

} // namespace xanadu::vprolog
