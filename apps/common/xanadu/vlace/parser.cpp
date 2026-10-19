/**
 * @file parser.cpp
 * @brief Implementation of the vlace parser (§5.1).
 */
#include "common/xanadu/vlace/parser.hpp"

#include <memory>
#include <utility>

namespace xanadu::vlace {

namespace {

Literal literalFromToken(const Token &t) {
  Literal lit;
  switch (t.kind) {
  case TokenKind::IntegerLiteral:
    lit.kind     = LiteralKind::Int;
    lit.intValue = t.intValue;
    break;
  case TokenKind::FloatLiteral:
    lit.kind       = LiteralKind::Float;
    lit.floatValue = t.floatValue;
    break;
  case TokenKind::StringLiteral:
    lit.kind      = LiteralKind::String;
    lit.textValue = t.stringValue;
    break;
  case TokenKind::CharLiteral:
    lit.kind      = LiteralKind::Char;
    lit.textValue = t.stringValue;
    lit.intValue  = t.intValue;
    break;
  default:
    break;
  }
  return lit;
}

template <class N> Expr makeExpr(N &&n, SourceLocation loc) {
  auto e  = std::make_unique<ExprNode>();
  e->node = std::forward<N>(n);
  e->loc  = loc;
  return e;
}

template <class N> Pattern makePat(N &&n, SourceLocation loc) {
  auto p  = std::make_unique<PatternNode>();
  p->node = std::forward<N>(n);
  p->loc  = loc;
  return p;
}

template <class N> Type makeType(N &&n, SourceLocation loc) {
  auto t  = std::make_unique<TypeNode>();
  t->node = std::forward<N>(n);
  t->loc  = loc;
  return t;
}

std::unexpected<ParseError> err(CompileError code, std::string detail,
                                SourceLocation loc) {
  return std::unexpected(
      ParseError{.code = code, .detail = std::move(detail), .loc = loc});
}

} // namespace

std::string_view compileErrorName(CompileError e) noexcept {
  switch (e) {
  case CompileError::LexicalError:
    return "LexicalError";
  case CompileError::UnexpectedToken:
    return "UnexpectedToken";
  case CompileError::UnboundIdentifier:
    return "UnboundIdentifier";
  case CompileError::TypeMismatch:
    return "TypeMismatch";
  case CompileError::OccursCheckFailed:
    return "OccursCheckFailed";
  case CompileError::NonLinearPattern:
    return "NonLinearPattern";
  case CompileError::NonExhaustiveMatch:
    return "NonExhaustiveMatch";
  case CompileError::UnknownModule:
    return "UnknownModule";
  }
  return "Unknown";
}

Parser::Parser(std::string_view source) : lexer_(source) {
  // Built-in fixity table (§5.1). Levels and associativities as specified.
  fixity_ = {
      {".", {9, Fixity::Right}},  {"*", {7, Fixity::Left}},
      {"/", {7, Fixity::Left}},   {"%", {7, Fixity::Left}},
      {"+", {6, Fixity::Left}},   {"-", {6, Fixity::Left}},
      {":", {5, Fixity::Right}},  {"++", {5, Fixity::Right}},
      {"==", {4, Fixity::None}},  {"/=", {4, Fixity::None}},
      {"<", {4, Fixity::None}},   {"<=", {4, Fixity::None}},
      {">", {4, Fixity::None}},   {">=", {4, Fixity::None}},
      {"&&", {3, Fixity::Right}}, {"||", {2, Fixity::Right}},
      {">>=", {1, Fixity::Left}}, {">>", {1, Fixity::Left}},
      {"$", {0, Fixity::Right}},
  };
  cur_  = lexer_.nextToken();
  next_ = lexer_.nextToken();
}

Token Parser::advance() {
  Token prev = cur_;
  cur_       = next_;
  next_      = lexer_.nextToken();
  return prev;
}

bool Parser::match(TokenKind k) {
  if (cur_.kind == k) {
    advance();
    return true;
  }
  return false;
}

std::expected<Token, ParseError> Parser::expect(TokenKind k, const char *what) {
  if (cur_.kind == k) {
    return advance();
  }
  return err(CompileError::UnexpectedToken,
             std::string("expected ") + what + ", got '" +
                 std::string(tokenKindName(cur_.kind)) + "'",
             cur_.loc);
}

std::optional<OpInfo> Parser::opInfo(std::string_view op) const {
  const auto it = fixity_.find(std::string(op));
  if (it == fixity_.end()) {
    return std::nullopt;
  }
  return it->second;
}

namespace {
// Does the token stream from `offset` begin a top-level value declaration --
// i.e. a bare `=` (Equals) at bracket depth 0 before a `;` or EOF? Used to
// tell `f x = ...` (a decl) from `f x` (a trailing application expression).
bool valueDeclAhead(std::string_view source, std::size_t offset) {
  Lexer scan(source.substr(offset));
  int depth = 0;
  for (;;) {
    Token t = scan.nextToken();
    switch (t.kind) {
    case TokenKind::EndOfFile:
    case TokenKind::Semicolon:
      return false;
    case TokenKind::OpenParen:
    case TokenKind::OpenBracket:
    case TokenKind::OpenBrace:
      ++depth;
      break;
    case TokenKind::CloseParen:
    case TokenKind::CloseBracket:
    case TokenKind::CloseBrace:
      --depth;
      break;
    case TokenKind::Equals:
      if (depth == 0) return true;
      break;
    // A fat arrow / case / let introduces a nested scope that can carry its
    // own `=`; a top-level decl head never contains these before its `=`.
    case TokenKind::FatArrow:
    case TokenKind::KwLet:
    case TokenKind::KwLetrec:
    case TokenKind::KwCase:
    case TokenKind::KwFn:
      return false;
    default:
      break;
    }
  }
}

// After a ';' inside a case, does another alternative follow -- i.e. a
// `pattern [if guard] =>` whose `=>` appears at bracket depth 0 before the
// next ';'? If not, the ';' separated the case from a following decl/expr and
// the alt list is finished.
bool altAhead(std::string_view source, std::size_t offset) {
  Lexer scan(source.substr(offset));
  int depth = 0;
  for (;;) {
    Token t = scan.nextToken();
    switch (t.kind) {
    case TokenKind::EndOfFile:
    case TokenKind::Semicolon:
      return false;
    case TokenKind::OpenParen:
    case TokenKind::OpenBracket:
    case TokenKind::OpenBrace:
      ++depth;
      break;
    case TokenKind::CloseParen:
    case TokenKind::CloseBracket:
    case TokenKind::CloseBrace:
      if (depth == 0) return false; // closing bracket ends an enclosing form
      --depth;
      break;
    case TokenKind::FatArrow:
      if (depth == 0) return true;
      break;
    // A nested let/case/fn before any top-level => means this is not a bare alt
    // head; bail so we do not misread it.
    case TokenKind::KwLet:
    case TokenKind::KwLetrec:
    case TokenKind::KwCase:
    case TokenKind::KwFn:
    case TokenKind::KwIn:
      return false;
    default:
      break;
    }
  }
}
} // namespace

// -- Program and declarations ------------------------------------------------

bool Parser::atExprStart() const {
  switch (cur_.kind) {
  case TokenKind::Identifier:
  case TokenKind::Constructor:
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CharLiteral:
  case TokenKind::OpenParen:
  case TokenKind::OpenBracket:
  case TokenKind::KwLet:
  case TokenKind::KwLetrec:
  case TokenKind::KwIf:
  case TokenKind::KwCase:
  case TokenKind::KwFn:
    return true;
  default:
    return false;
  }
}

std::expected<Program, ParseError> Parser::parseProgram() {
  Program prog;
  // decl ";" sequences, then an optional trailing expression. A decl begins
  // with data/type/import/infix*/ or a lowercase identifier followed by
  // patterns/`=` (a value_decl). Anything else is the trailing expression.
  for (;;) {
    if (cur_.kind == TokenKind::EndOfFile) {
      break;
    }
    const bool declKeyword =
        cur_.kind == TokenKind::KwData || cur_.kind == TokenKind::KwType ||
        cur_.kind == TokenKind::KwImport || cur_.kind == TokenKind::KwInfixl ||
        cur_.kind == TokenKind::KwInfixr || cur_.kind == TokenKind::KwInfix;

    // A value declaration is `ident ... =` at the top level. Detect it by an
    // identifier whose line eventually reaches `=` before `;` -- cheaply
    // approximated: identifier followed (after optional patterns) by `=`.
    // We detect `ident` then scan for a top-level `=`.
    if (declKeyword) {
      auto d = parseDecl();
      if (!d) return std::unexpected(d.error());
      prog.decls.push_back(std::move(*d));
      if (!match(TokenKind::Semicolon)) {
        // decls are ;-separated; the last one may omit it before EOF.
        if (cur_.kind != TokenKind::EndOfFile) {
          // continue: maybe a trailing expr follows without a ';'
        }
      }
      continue;
    }

    // Lowercase identifier: could be a value_decl or the trailing expr. Look
    // ahead for a `=` that is not part of an operator by scanning the simple
    // case `ident (pattern)* =`.
    if (cur_.kind == TokenKind::Identifier) {
      // Save nothing fancy: a value decl is ident then zero+ atom-patterns then
      // '='. Since our two-token lookahead is limited, we commit to a value
      // decl only when `ident =` or `ident <ident/_/(/[/lit> ... =`. We detect
      // by trying parseValueDecl on a cloned cursor is not possible with this
      // streaming lexer, so we use the heuristic: ident followed by '=' or by a
      // pattern-starting token is a value decl; ident followed by an operator
      // or application argument that is clearly an expression is the result.
      // Practical rule matching the spec's examples: if peek2 is '=' it's a
      // decl; if peek2 is a pattern-start AND we are at top level we treat it
      // as a decl (f x = ...). Otherwise it's the trailing expression.
      const bool looksDecl = next_.kind == TokenKind::Equals ||
                             next_.kind == TokenKind::Identifier ||
                             next_.kind == TokenKind::Underscore ||
                             next_.kind == TokenKind::Constructor ||
                             next_.kind == TokenKind::OpenParen ||
                             next_.kind == TokenKind::OpenBracket ||
                             next_.kind == TokenKind::IntegerLiteral;
      // Only treat as decl if there is in fact a '=' terminating the head.
      // We cannot look arbitrarily far with two-token lookahead, so we parse a
      // value decl and, if it does not find '=', it reports UnexpectedToken.
      // To avoid misparsing the trailing expression, require the simplest
      // signal: peek2 == '='. (f x = ...) with params is still supported via
      // parseValueDecl when peek2 is a pattern-start, but we guard it so a
      // bare application expression `f x` at the end is not stolen: we only
      // enter decl mode when a later top-level '=' exists on this logical line.
      if (looksDecl && valueDeclAhead(lexer_.source(), cur_.loc.offset)) {
        auto d = parseValueDecl();
        if (!d) return std::unexpected(d.error());
        prog.decls.push_back(Decl{std::move(*d)});
        match(TokenKind::Semicolon);
        continue;
      }
    }

    // Otherwise: the trailing expression ends the program.
    auto e = parseExpr();
    if (!e) return std::unexpected(e.error());
    prog.result = std::move(*e);
    match(TokenKind::Semicolon);
    break;
  }
  return prog;
}

std::expected<Expr, ParseError> Parser::parseExpressionOnly() {
  auto e = parseExpr();
  if (!e) return std::unexpected(e.error());
  return e;
}

std::expected<Decl, ParseError> Parser::parseDecl() {
  switch (cur_.kind) {
  case TokenKind::KwData: {
    auto d = parseDataDecl();
    if (!d) return std::unexpected(d.error());
    return Decl{std::move(*d)};
  }
  case TokenKind::KwType: {
    auto d = parseTypeDecl();
    if (!d) return std::unexpected(d.error());
    return Decl{std::move(*d)};
  }
  case TokenKind::KwImport: {
    auto d = parseImportDecl();
    if (!d) return std::unexpected(d.error());
    return Decl{std::move(*d)};
  }
  case TokenKind::KwInfixl:
  case TokenKind::KwInfixr:
  case TokenKind::KwInfix: {
    auto d = parseFixityDecl();
    if (!d) return std::unexpected(d.error());
    return Decl{std::move(*d)};
  }
  default: {
    auto d = parseValueDecl();
    if (!d) return std::unexpected(d.error());
    return Decl{std::move(*d)};
  }
  }
}

std::expected<FixityDecl, ParseError> Parser::parseFixityDecl() {
  FixityDecl fd;
  fd.fixity = cur_.kind == TokenKind::KwInfixl   ? Fixity::Left
              : cur_.kind == TokenKind::KwInfixr ? Fixity::Right
                                                 : Fixity::None;
  advance();
  auto lvl = expect(TokenKind::IntegerLiteral, "fixity level");
  if (!lvl) return std::unexpected(lvl.error());
  fd.level = static_cast<int>(lvl->intValue);
  if (cur_.kind != TokenKind::Operator && cur_.kind != TokenKind::Dot) {
    return err(CompileError::UnexpectedToken, "expected operator in fixity",
               cur_.loc);
  }
  fd.op = std::string(cur_.text);
  advance();
  // Register so subsequent uses parse at the declared precedence.
  fixity_[fd.op] = OpInfo{.level = fd.level, .fixity = fd.fixity};
  return fd;
}

std::expected<ImportDecl, ParseError> Parser::parseImportDecl() {
  advance(); // import
  auto path = expect(TokenKind::StringLiteral, "module path string");
  if (!path) return std::unexpected(path.error());
  ImportDecl imp;
  imp.modulePath = path->stringValue;
  if (cur_.kind == TokenKind::KwAs) {
    advance();
    auto a = expect(TokenKind::Constructor, "alias (uppercase)");
    if (!a) {
      // allow lowercase alias too
      if (cur_.kind == TokenKind::Identifier) {
        imp.alias = std::string(cur_.text);
        advance();
      } else {
        return std::unexpected(a.error());
      }
    } else {
      imp.alias = std::string(a->text);
    }
  }
  if (match(TokenKind::OpenParen)) {
    while (cur_.kind != TokenKind::CloseParen &&
           cur_.kind != TokenKind::EndOfFile) {
      if (cur_.kind == TokenKind::Identifier ||
          cur_.kind == TokenKind::Constructor) {
        imp.names.emplace_back(cur_.text);
        advance();
      } else if (match(TokenKind::OpenParen)) {
        // (op) form
        if (cur_.kind == TokenKind::Operator || cur_.kind == TokenKind::Dot) {
          imp.names.emplace_back(cur_.text);
          advance();
        }
        if (!match(TokenKind::CloseParen)) {
          return err(CompileError::UnexpectedToken, "expected ) after operator",
                     cur_.loc);
        }
      }
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    auto cp = expect(TokenKind::CloseParen, ") to close import list");
    if (!cp) return std::unexpected(cp.error());
  }
  return imp;
}

std::expected<DataDecl, ParseError> Parser::parseDataDecl() {
  advance(); // data
  auto name = expect(TokenKind::Constructor, "type constructor name");
  if (!name) return std::unexpected(name.error());
  DataDecl dd;
  dd.name = std::string(name->text);
  while (cur_.kind == TokenKind::Identifier) {
    dd.params.emplace_back(cur_.text);
    advance();
  }
  auto eq = expect(TokenKind::Equals, "= in data declaration");
  if (!eq) return std::unexpected(eq.error());
  for (;;) {
    auto ctorName = expect(TokenKind::Constructor, "constructor name");
    if (!ctorName) return std::unexpected(ctorName.error());
    CtorDef cd;
    cd.name = std::string(ctorName->text);
    if (match(TokenKind::OpenBrace)) {
      cd.isRecord = true;
      while (cur_.kind != TokenKind::CloseBrace &&
             cur_.kind != TokenKind::EndOfFile) {
        auto fname = expect(TokenKind::Identifier, "record field name");
        if (!fname) return std::unexpected(fname.error());
        auto dc = expect(TokenKind::DoubleColon, ":: in record field");
        if (!dc) return std::unexpected(dc.error());
        auto ft = parseType();
        if (!ft) return std::unexpected(ft.error());
        cd.fields.emplace_back(std::string(fname->text), std::move(*ft));
        if (!match(TokenKind::Comma)) break;
      }
      auto cb = expect(TokenKind::CloseBrace, "} to close record");
      if (!cb) return std::unexpected(cb.error());
    } else {
      // positional constructor args: zero or more atomic types
      while (cur_.kind == TokenKind::Identifier ||
             cur_.kind == TokenKind::Constructor ||
             cur_.kind == TokenKind::OpenParen ||
             cur_.kind == TokenKind::OpenBracket) {
        auto at = parseAtomType();
        if (!at) return std::unexpected(at.error());
        cd.argTypes.push_back(std::move(*at));
      }
    }
    dd.ctors.push_back(std::move(cd));
    if (!match(TokenKind::Bar)) break;
  }
  return dd;
}

std::expected<TypeDecl, ParseError> Parser::parseTypeDecl() {
  advance(); // type
  auto name = expect(TokenKind::Constructor, "type alias name");
  if (!name) return std::unexpected(name.error());
  TypeDecl td;
  td.name = std::string(name->text);
  while (cur_.kind == TokenKind::Identifier) {
    td.params.emplace_back(cur_.text);
    advance();
  }
  auto eq = expect(TokenKind::Equals, "= in type alias");
  if (!eq) return std::unexpected(eq.error());
  auto t = parseType();
  if (!t) return std::unexpected(t.error());
  td.aliased = std::move(*t);
  return td;
}

std::expected<ValueDecl, ParseError> Parser::parseValueDecl() {
  // ident pattern* = expr    (also "(op) pattern* = expr", not yet needed)
  auto name = expect(TokenKind::Identifier, "value name");
  if (!name) return std::unexpected(name.error());
  ValueDecl vd;
  vd.name = std::string(name->text);
  while (cur_.kind != TokenKind::Equals && cur_.kind != TokenKind::EndOfFile) {
    auto p = parseAtomPattern();
    if (!p) return std::unexpected(p.error());
    vd.params.push_back(std::move(*p));
  }
  auto eq = expect(TokenKind::Equals, "= in value declaration");
  if (!eq) return std::unexpected(eq.error());
  auto body = parseExpr();
  if (!body) return std::unexpected(body.error());
  vd.body = std::move(*body);
  return vd;
}

// -- Types -------------------------------------------------------------------

std::expected<Type, ParseError> Parser::parseType() {
  // atype ("->" type)?   (right-assoc function arrow)
  auto left = parseAtomType();
  if (!left) return std::unexpected(left.error());
  // a type constructor applied to atom types: "List a", "Map k v"
  if (std::holds_alternative<TCon>((*left)->node)) {
    auto &con = std::get<TCon>((*left)->node);
    while (cur_.kind == TokenKind::Identifier ||
           cur_.kind == TokenKind::Constructor ||
           cur_.kind == TokenKind::OpenParen ||
           cur_.kind == TokenKind::OpenBracket) {
      auto arg = parseAtomType();
      if (!arg) return std::unexpected(arg.error());
      con.args.push_back(std::move(*arg));
    }
  }
  if (match(TokenKind::ThinArrow)) {
    auto right = parseType();
    if (!right) return std::unexpected(right.error());
    SourceLocation loc = (*left)->loc;
    return makeType(TFun{.from = std::move(*left), .to = std::move(*right)},
                    loc);
  }
  return left;
}

std::expected<Type, ParseError> Parser::parseAtomType() {
  const SourceLocation loc = cur_.loc;
  if (cur_.kind == TokenKind::Identifier) {
    std::string n(cur_.text);
    advance();
    return makeType(TVar{.name = std::move(n)}, loc);
  }
  if (cur_.kind == TokenKind::Constructor) {
    std::string n(cur_.text);
    advance();
    return makeType(TCon{.name = std::move(n), .args = {}}, loc);
  }
  if (match(TokenKind::OpenParen)) {
    auto t = parseType();
    if (!t) return std::unexpected(t.error());
    auto cp = expect(TokenKind::CloseParen, ") to close type");
    if (!cp) return std::unexpected(cp.error());
    return t;
  }
  if (match(TokenKind::OpenBracket)) {
    // [t] = List t
    auto t = parseType();
    if (!t) return std::unexpected(t.error());
    auto cb = expect(TokenKind::CloseBracket, "] to close list type");
    if (!cb) return std::unexpected(cb.error());
    std::vector<Type> args;
    args.push_back(std::move(*t));
    return makeType(TCon{.name = "List", .args = std::move(args)}, loc);
  }
  return err(CompileError::UnexpectedToken, "expected a type", loc);
}

// -- Expressions -------------------------------------------------------------

std::expected<Expr, ParseError> Parser::parseExpr() {
  switch (cur_.kind) {
  case TokenKind::KwLet:
    return parseLet(false);
  case TokenKind::KwLetrec:
    return parseLet(true);
  case TokenKind::KwIf:
    return parseIf();
  case TokenKind::KwCase:
    return parseCase();
  case TokenKind::KwFn:
    return parseLambda();
  default:
    return parseOpExpr(0);
  }
}

// Precedence climbing over the fixity table (§5.1). Application binds tighter
// than every operator, so the operand of an operator is a full application.
std::expected<Expr, ParseError> Parser::parseOpExpr(int minLevel) {
  auto left = parseApp();
  if (!left) return std::unexpected(left.error());

  for (;;) {
    if (cur_.kind != TokenKind::Operator && cur_.kind != TokenKind::Dot) {
      break;
    }
    const std::string op(cur_.text);
    const auto info = opInfo(op);
    if (!info || info->level < minLevel) {
      break;
    }
    const SourceLocation opLoc = cur_.loc;
    advance();
    const int nextMin =
        info->fixity == Fixity::Left ? info->level + 1 : info->level;
    auto right = parseOpExpr(nextMin);
    if (!right) return std::unexpected(right.error());
    left = makeExpr(
        EBinOp{.op = op, .lhs = std::move(*left), .rhs = std::move(*right)},
        opLoc);
    if (info->fixity == Fixity::None) {
      // Non-associative: do not chain at the same level.
      break;
    }
  }
  return left;
}

// Application is juxtaposition: atom atom atom ...  (left-assoc).
std::expected<Expr, ParseError> Parser::parseApp() {
  auto fn = parseAtom();
  if (!fn) return std::unexpected(fn.error());
  for (;;) {
    // An argument is another atom. Stop at anything that is not an atom start.
    switch (cur_.kind) {
    case TokenKind::Identifier:
    case TokenKind::Constructor:
    case TokenKind::IntegerLiteral:
    case TokenKind::FloatLiteral:
    case TokenKind::StringLiteral:
    case TokenKind::CharLiteral:
    case TokenKind::OpenParen:
    case TokenKind::OpenBracket:
    case TokenKind::Underscore:
      break;
    default:
      return fn;
    }
    const SourceLocation loc = (*fn)->loc;
    auto arg                 = parseAtom();
    if (!arg) return std::unexpected(arg.error());
    fn = makeExpr(EApp{.fn = std::move(*fn), .arg = std::move(*arg)}, loc);
  }
}

Expr Parser::desugarList(std::vector<Expr> elems, SourceLocation loc) {
  // [a, b, c] => a : b : c : []  (Cons chain ending in Nil).
  Expr acc = makeExpr(ECtor{.name = "Nil"}, loc);
  for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
    acc = makeExpr(
        EBinOp{.op = ":", .lhs = std::move(*it), .rhs = std::move(acc)}, loc);
  }
  return acc;
}

std::expected<Expr, ParseError> Parser::parseAtom() {
  const SourceLocation loc = cur_.loc;
  switch (cur_.kind) {
  case TokenKind::Identifier: {
    std::string n(cur_.text);
    advance();
    // Qualified name M.name: Dot immediately followed by an identifier.
    // (Module aliases are uppercase, handled under Constructor below.)
    return makeExpr(EVar{.name = std::move(n)}, loc);
  }
  case TokenKind::Constructor: {
    std::string n(cur_.text);
    advance();
    // Qualified name "Module.name".
    if (cur_.kind == TokenKind::Dot && next_.kind == TokenKind::Identifier) {
      advance(); // .
      std::string member(cur_.text);
      advance();
      return makeExpr(EVar{.name = n + "." + member}, loc);
    }
    return makeExpr(ECtor{.name = std::move(n)}, loc);
  }
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CharLiteral: {
    Literal lit = literalFromToken(cur_);
    advance();
    return makeExpr(ELiteral{.lit = std::move(lit)}, loc);
  }
  case TokenKind::Underscore:
    // A bare underscore is only valid in patterns; in an expression it is an
    // error, but we accept it as a hole-named variable to keep parsing robust.
    advance();
    return err(CompileError::UnexpectedToken,
               "'_' is a pattern wildcard, not an expression", loc);
  case TokenKind::OpenParen: {
    advance();
    // "(op)" operator section as a function value.
    if ((cur_.kind == TokenKind::Operator || cur_.kind == TokenKind::Dot) &&
        next_.kind == TokenKind::CloseParen) {
      std::string op(cur_.text);
      advance();
      advance(); // )
      return makeExpr(EVar{.name = op}, loc);
    }
    auto first = parseExpr();
    if (!first) return std::unexpected(first.error());
    if (match(TokenKind::CloseParen)) {
      return first; // grouping
    }
    // Tuple: (a, b, ...) -- needs >= 1 comma.
    if (cur_.kind == TokenKind::Comma) {
      std::vector<Expr> elems;
      elems.push_back(std::move(*first));
      while (match(TokenKind::Comma)) {
        auto e = parseExpr();
        if (!e) return std::unexpected(e.error());
        elems.push_back(std::move(*e));
      }
      auto cp = expect(TokenKind::CloseParen, ") to close tuple");
      if (!cp) return std::unexpected(cp.error());
      return makeExpr(ETuple{.elems = std::move(elems)}, loc);
    }
    return err(CompileError::UnexpectedToken, "expected ) or , in parentheses",
               cur_.loc);
  }
  case TokenKind::OpenBracket: {
    advance();
    std::vector<Expr> elems;
    if (cur_.kind != TokenKind::CloseBracket) {
      for (;;) {
        auto e = parseExpr();
        if (!e) return std::unexpected(e.error());
        elems.push_back(std::move(*e));
        if (!match(TokenKind::Comma)) break;
      }
    }
    auto cb = expect(TokenKind::CloseBracket, "] to close list");
    if (!cb) return std::unexpected(cb.error());
    return desugarList(std::move(elems), loc);
  }
  default:
    return err(CompileError::UnexpectedToken,
               std::string("unexpected '") +
                   std::string(tokenKindName(cur_.kind)) +
                   "' at start of expression",
               loc);
  }
}

std::expected<Expr, ParseError> Parser::parseLet(bool recursive) {
  const SourceLocation loc = cur_.loc;
  advance(); // let / letrec
  ELet node;
  node.recursive = recursive;
  for (;;) {
    auto pat = parsePattern();
    if (!pat) return std::unexpected(pat.error());
    auto eq = expect(TokenKind::Equals, "= in binding");
    if (!eq) return std::unexpected(eq.error());
    auto val = parseExpr();
    if (!val) return std::unexpected(val.error());
    node.bindings.push_back(
        Binding{.pattern = std::move(*pat), .value = std::move(*val)});
    if (!match(TokenKind::Semicolon)) break;
    if (cur_.kind == TokenKind::KwIn) break;
  }
  auto in = expect(TokenKind::KwIn, "in after bindings");
  if (!in) return std::unexpected(in.error());
  auto body = parseExpr();
  if (!body) return std::unexpected(body.error());
  node.body = std::move(*body);
  return makeExpr(std::move(node), loc);
}

std::expected<Expr, ParseError> Parser::parseIf() {
  const SourceLocation loc = cur_.loc;
  advance(); // if
  auto cond = parseExpr();
  if (!cond) return std::unexpected(cond.error());
  auto th = expect(TokenKind::KwThen, "then");
  if (!th) return std::unexpected(th.error());
  auto thenE = parseExpr();
  if (!thenE) return std::unexpected(thenE.error());
  auto el = expect(TokenKind::KwElse, "else");
  if (!el) return std::unexpected(el.error());
  auto elseE = parseExpr();
  if (!elseE) return std::unexpected(elseE.error());
  return makeExpr(EIf{.cond = std::move(*cond),
                      .then = std::move(*thenE),
                      .elze = std::move(*elseE)},
                  loc);
}

std::expected<Expr, ParseError> Parser::parseCase() {
  const SourceLocation loc = cur_.loc;
  advance(); // case
  auto scrut = parseExpr();
  if (!scrut) return std::unexpected(scrut.error());
  auto of = expect(TokenKind::KwOf, "of");
  if (!of) return std::unexpected(of.error());
  ECase node;
  node.scrutinee = std::move(*scrut);
  for (;;) {
    auto pat = parsePattern();
    if (!pat) return std::unexpected(pat.error());
    Alt alt;
    alt.pattern = std::move(*pat);
    if (cur_.kind == TokenKind::KwIf) {
      advance();
      auto g = parseExpr();
      if (!g) return std::unexpected(g.error());
      alt.guard = std::move(*g);
    }
    auto arrow = expect(TokenKind::FatArrow, "=> in case alternative");
    if (!arrow) return std::unexpected(arrow.error());
    auto body = parseExpr();
    if (!body) return std::unexpected(body.error());
    alt.body = std::move(*body);
    node.alts.push_back(std::move(alt));
    if (cur_.kind != TokenKind::Semicolon) break;
    // Peek past the ';': continue only if another `pattern => ...` alt follows.
    // Otherwise the ';' separates the case from a following decl/expression.
    if (!altAhead(lexer_.source(), next_.loc.offset)) {
      break;
    }
    advance(); // consume the ';' now that we know an alt follows
  }
  return makeExpr(std::move(node), loc);
}

std::expected<Expr, ParseError> Parser::parseLambda() {
  const SourceLocation loc = cur_.loc;
  advance(); // fn
  ELambda node;
  while (cur_.kind != TokenKind::FatArrow &&
         cur_.kind != TokenKind::EndOfFile) {
    auto p = parseAtomPattern();
    if (!p) return std::unexpected(p.error());
    node.params.push_back(std::move(*p));
  }
  auto arrow = expect(TokenKind::FatArrow, "=> in lambda");
  if (!arrow) return std::unexpected(arrow.error());
  auto body = parseExpr();
  if (!body) return std::unexpected(body.error());
  node.body = std::move(*body);
  if (node.params.empty()) {
    return err(CompileError::UnexpectedToken, "fn needs at least one parameter",
               loc);
  }
  return makeExpr(std::move(node), loc);
}

// -- Patterns ----------------------------------------------------------------

std::expected<Pattern, ParseError> Parser::parsePattern() {
  // cons pattern: appPattern (":" pattern)?  (right-assoc)
  auto left = parseAppPattern();
  if (!left) return std::unexpected(left.error());
  if (cur_.kind == TokenKind::Operator && cur_.text == ":") {
    const SourceLocation loc = cur_.loc;
    advance();
    auto right = parsePattern();
    if (!right) return std::unexpected(right.error());
    std::vector<Pattern> args;
    args.push_back(std::move(*left));
    args.push_back(std::move(*right));
    return makePat(PConstructor{.ctor = "Cons", .args = std::move(args)}, loc);
  }
  return left;
}

std::expected<Pattern, ParseError> Parser::parseAppPattern() {
  // Constructor applied to atom patterns: C p1 p2 ...
  if (cur_.kind == TokenKind::Constructor) {
    const SourceLocation loc = cur_.loc;
    std::string ctor(cur_.text);
    advance();
    // record pattern C { f = p, ... }
    if (match(TokenKind::OpenBrace)) {
      PRecord rec;
      rec.ctor = std::move(ctor);
      while (cur_.kind != TokenKind::CloseBrace &&
             cur_.kind != TokenKind::EndOfFile) {
        auto fname = expect(TokenKind::Identifier, "record field name");
        if (!fname) return std::unexpected(fname.error());
        auto eq = expect(TokenKind::Equals, "= in record pattern");
        if (!eq) return std::unexpected(eq.error());
        auto p = parsePattern();
        if (!p) return std::unexpected(p.error());
        rec.fields.emplace_back(std::string(fname->text), std::move(*p));
        if (!match(TokenKind::Comma)) break;
      }
      auto cb = expect(TokenKind::CloseBrace, "} to close record pattern");
      if (!cb) return std::unexpected(cb.error());
      return makePat(std::move(rec), loc);
    }
    PConstructor pc;
    pc.ctor = std::move(ctor);
    while (cur_.kind == TokenKind::Identifier ||
           cur_.kind == TokenKind::Constructor ||
           cur_.kind == TokenKind::Underscore ||
           cur_.kind == TokenKind::OpenParen ||
           cur_.kind == TokenKind::OpenBracket ||
           cur_.kind == TokenKind::IntegerLiteral ||
           cur_.kind == TokenKind::StringLiteral ||
           cur_.kind == TokenKind::CharLiteral) {
      // Only consume bare constructors as nullary atoms here.
      auto p = parseAtomPattern();
      if (!p) return std::unexpected(p.error());
      pc.args.push_back(std::move(*p));
    }
    return makePat(std::move(pc), loc);
  }
  return parseAtomPattern();
}

std::expected<Pattern, ParseError> Parser::parseAtomPattern() {
  const SourceLocation loc = cur_.loc;
  switch (cur_.kind) {
  case TokenKind::Underscore:
    advance();
    return makePat(PWildcard{}, loc);
  case TokenKind::Identifier: {
    std::string n(cur_.text);
    advance();
    return makePat(PVar{.name = std::move(n)}, loc);
  }
  case TokenKind::Constructor: {
    std::string n(cur_.text);
    advance();
    return makePat(PConstructor{.ctor = std::move(n), .args = {}}, loc);
  }
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CharLiteral: {
    Literal lit = literalFromToken(cur_);
    advance();
    return makePat(PLiteral{.lit = std::move(lit)}, loc);
  }
  case TokenKind::OpenParen: {
    advance();
    auto first = parsePattern();
    if (!first) return std::unexpected(first.error());
    if (match(TokenKind::CloseParen)) {
      return first;
    }
    if (cur_.kind == TokenKind::Comma) {
      PTuple tup;
      tup.elems.push_back(std::move(*first));
      while (match(TokenKind::Comma)) {
        auto p = parsePattern();
        if (!p) return std::unexpected(p.error());
        tup.elems.push_back(std::move(*p));
      }
      auto cp = expect(TokenKind::CloseParen, ") to close tuple pattern");
      if (!cp) return std::unexpected(cp.error());
      return makePat(std::move(tup), loc);
    }
    return err(CompileError::UnexpectedToken, "expected ) or , in pattern",
               cur_.loc);
  }
  case TokenKind::OpenBracket: {
    advance();
    // [a, b] => Cons a (Cons b Nil)
    std::vector<Pattern> elems;
    if (cur_.kind != TokenKind::CloseBracket) {
      for (;;) {
        auto p = parsePattern();
        if (!p) return std::unexpected(p.error());
        elems.push_back(std::move(*p));
        if (!match(TokenKind::Comma)) break;
      }
    }
    auto cb = expect(TokenKind::CloseBracket, "] to close list pattern");
    if (!cb) return std::unexpected(cb.error());
    Pattern acc = makePat(PConstructor{.ctor = "Nil", .args = {}}, loc);
    for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
      std::vector<Pattern> args;
      args.push_back(std::move(*it));
      args.push_back(std::move(acc));
      acc = makePat(PConstructor{.ctor = "Cons", .args = std::move(args)}, loc);
    }
    return acc;
  }
  default:
    return err(CompileError::UnexpectedToken, "expected a pattern", loc);
  }
}

} // namespace xanadu::vlace
