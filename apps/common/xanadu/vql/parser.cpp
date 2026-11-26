/**
 * @file parser.cpp
 * @brief Recursive-descent parser implementation for VQL v16.0.
 */
#include "common/xanadu/vql/parser.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "common/xanadu/vortex/frontend_diagnostics.hpp"

namespace xanadu::vql {

namespace {
/// An unknown name is a ContextualTypeIdent, kept for the engine to refuse by
/// name rather than misread here.
BasicType::Kind basicTypeKind(std::string_view name) {
  using K = BasicType::Kind;
  static constexpr std::array<std::pair<std::string_view, K>, 10> named{
      {
          {"Cell", K::Cell},
          {"Dim", K::Dim},
          {"Store", K::Store},
          {"Slice", K::Slice},
          {"Version", K::Version},
          {"Int", K::Int},
          {"Float", K::Float},
          {"String", K::String},
          {"Bool", K::Bool},
          {"Any", K::Any},
      },
  };
  const auto *const it =
      std::ranges::find(named, name, &std::pair<std::string_view, K>::first);
  return it != named.end() ? it->second : K::Contextual;
}
} // namespace

ParseError::ParseError(std::string_view message, SourceLocation loc)
    : std::runtime_error(std::format("Parse error at line {}:{}: {}", loc.line,
                                     loc.column, message)),
      loc_(loc) {}

Parser::Parser(std::string_view source) : lexer_(source) {
  current_ = lexer_.nextToken();
}

Parser::Parser(Lexer lexer) : lexer_(std::move(lexer)) {
  current_ = lexer_.nextToken();
}

Token Parser::currentToken() { return current_; }

Token Parser::peekToken() {
  if (!hasLookahead_) {
    lookahead_    = lexer_.nextToken();
    hasLookahead_ = true;
  }
  return lookahead_;
}

Token Parser::advance() {
  Token prev = current_;
  if (hasLookahead_) {
    current_      = lookahead_;
    hasLookahead_ = false;
  } else {
    current_ = lexer_.nextToken();
  }
  return prev;
}

bool Parser::check(TokenKind kind) const { return current_.kind == kind; }

bool Parser::match(TokenKind kind) {
  if (check(kind)) {
    advance();
    return true;
  }
  return false;
}

Token Parser::consume(TokenKind kind, std::string_view errorMessage) {
  if (check(kind)) {
    return advance();
  }
  throw ParseError(errorMessage, current_.loc);
}

bool Parser::isNextTokenComparisonOperand() {
  Token tok = peekToken();
  return tok.is(TokenKind::IntegerLiteral) || tok.is(TokenKind::FloatLiteral) ||
         tok.is(TokenKind::StringLiteral) || tok.is(TokenKind::KwTrue) ||
         tok.is(TokenKind::KwFalse) || tok.is(TokenKind::Variable) ||
         tok.is(TokenKind::OpenParen) || tok.is(TokenKind::Identifier);
}

QueryExpression Parser::parseQuery() {
  std::vector<DefDeclaration> defs;
  std::vector<MountDeclaration> mounts;
  std::vector<NamespaceDeclaration> namespaceDeclarations;
  while (check(TokenKind::KwDef) || check(TokenKind::KwMount) ||
         check(TokenKind::KwUnmount)) {
    if (check(TokenKind::KwDef)) {
      defs.push_back(parseDefDeclaration());
      match(TokenKind::Semicolon);
    } else {
      if (check(TokenKind::KwMount)) {
        auto mount = parseMountDeclaration();
        mounts.push_back(mount);
        namespaceDeclarations.emplace_back(std::move(mount));
      } else {
        namespaceDeclarations.emplace_back(parseUnmountDeclaration());
      }
      match(TokenKind::Semicolon);
    }
  }

  nextBodyIsScript_ = true;
  ScriptBody body   = parseScriptBody();
  consume(TokenKind::EndOfFile, "Expected end of query");

  std::variant<ExecutionBlock, PathExpression> expr;
  if (!body.statements.empty()) {
    const auto &first = body.statements.front();
    if (std::holds_alternative<ExecutionBlock>(first)) {
      expr = std::get<ExecutionBlock>(first);
    } else if (std::holds_alternative<PathExpression>(first)) {
      expr = std::get<PathExpression>(first);
    } else if (std::holds_alternative<WeaveStatement>(first)) {
      expr =
          ExecutionBlock{.action = ActionClause{
                             .clause = std::get<WeaveStatement>(first).effect}};
    }
  }

  return QueryExpression{
      .defs                  = std::move(defs),
      .mounts                = std::move(mounts),
      .namespaceDeclarations = std::move(namespaceDeclarations),
      .body                  = std::move(body),
      .expr                  = std::move(expr),
  };
}

ScriptBody Parser::parseScriptBody() {
  // Only the body parseQuery marked may hold script I/O actions; a nested
  // body (def, watch, perspective, lambda) restores the outer flag on exit.
  const bool outerAllowed =
      std::exchange(effectsAllowed_, std::exchange(nextBodyIsScript_, false));
  struct Restore {
    bool &flag;
    bool value;
    ~Restore() { flag = value; }
  } const restore{effectsAllowed_, outerAllowed};
  ScriptBody body;
  while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
    while (match(TokenKind::Semicolon)) {
    }
    if (check(TokenKind::CloseBrace) || check(TokenKind::EndOfFile)) {
      break;
    }
    body.statements.push_back(parseStatement());
    match(TokenKind::Semicolon);
  }
  return body;
}

Statement Parser::parseStatement() {
  if (atIoEffect()) {
    if (!effectsAllowed_) {
      throw ParseError(
          std::format("{}: an I/O script action ('effect') is allowed only as "
                      "a top-level script statement, not inside a def, watch, "
                      "perspective or lambda body",
                      vortex::frontend_codes::vqlQueryContext),
          current_.loc);
    }
    auto effect = parseIoEffectStatement();
    noteIoName(effect);
    return effect;
  }
  if (atIoMatch()) {
    if (!effectsAllowed_) {
      throw ParseError(
          std::format("{}: an I/O script 'match' is allowed only as a "
                      "top-level script statement, not inside a def, watch, "
                      "perspective or lambda body",
                      vortex::frontend_codes::vqlQueryContext),
          current_.loc);
    }
    return parseIoMatchStatement();
  }
  if (check(TokenKind::KwWatch)) {
    return parseWatchStatement();
  }
  if (check(TokenKind::KwWith)) {
    return parsePerspectiveBlock();
  }
  if (check(TokenKind::KwWeave)) {
    return parseWeaveStatement();
  }
  if (check(TokenKind::KwFor) || check(TokenKind::KwLet) ||
      check(TokenKind::KwIf) || check(TokenKind::KwReturn)) {
    return parseExecutionBlock();
  }
  if (check(TokenKind::Variable) && peekToken().is(TokenKind::OpenParen)) {
    ValueExpr expression = parseValueOrComparisonExpr();
    ReturnClause returned;
    returned.items.push_back(
        ReturnItem{.item = ReturnValue{.expression = std::move(expression)}});
    return ExecutionBlock{.action =
                              ActionClause{.clause = std::move(returned)}};
  }
  return parsePathExpression();
}

WeaveStatement Parser::parseWeaveStatement() {
  return WeaveStatement{.effect = parseEffectClause()};
}

WatchStatement Parser::parseWatchStatement() {
  consume(TokenKind::KwWatch, "Expected 'watch'");
  PathExpression targetPath = parsePathExpression();

  std::optional<std::string> asVar;
  std::optional<DestructurePattern> asPattern;
  if (match(TokenKind::KwAs)) {
    if (check(TokenKind::OpenBrace)) {
      asPattern = parseDestructurePattern();
    } else if (check(TokenKind::Variable)) {
      Token vTok = advance();
      asVar      = vTok.stringValue;
    } else if (check(TokenKind::Identifier)) {
      Token idTok = advance();
      asVar       = std::string(idTok.text);
    } else {
      throw ParseError("Expected variable or destructure pattern after 'as'",
                       current_.loc);
    }
  }

  std::optional<VersionExpr> sinceVersion;
  if (match(TokenKind::KwSince)) {
    sinceVersion = parseVersionExpr();
  }

  std::optional<BooleanExpr> untilCondition;
  if (match(TokenKind::KwUntil)) {
    untilCondition = parseBooleanExpr();
  }

  consume(TokenKind::OpenBrace, "Expected '{' to start watch body");
  ScriptBody body = parseScriptBody();
  consume(TokenKind::CloseBrace, "Expected '}' to close watch body");

  return WatchStatement{
      .targetPath     = std::move(targetPath),
      .asVar          = std::move(asVar),
      .asPattern      = std::move(asPattern),
      .sinceVersion   = std::move(sinceVersion),
      .untilCondition = std::move(untilCondition),
      .body           = std::make_shared<ScriptBody>(std::move(body)),
  };
}

PerspectiveBlock Parser::parsePerspectiveBlock() {
  consume(TokenKind::KwWith, "Expected 'with'");
  consume(TokenKind::KwPerspective, "Expected 'perspective' after 'with'");
  consume(TokenKind::OpenParen, "Expected '(' after 'perspective'");

  std::string name;
  bool fromVariable = false;
  if (check(TokenKind::StringLiteral)) {
    Token sTok = advance();
    name       = sTok.stringValue;
  } else if (check(TokenKind::Identifier)) {
    Token idTok = advance();
    name        = std::string(idTok.text);
  } else if (check(TokenKind::Variable)) {
    Token vTok   = advance();
    name         = vTok.stringValue;
    fromVariable = true;
  } else {
    throw ParseError("Expected perspective name in perspective block",
                     current_.loc);
  }

  while (match(TokenKind::Comma)) {
    if (check(TokenKind::Identifier) && peekToken().is(TokenKind::Colon)) {
      advance(); // identifier
      advance(); // ':'
      parseValueExpr();
    } else {
      parseValueExpr();
    }
  }

  consume(TokenKind::CloseParen, "Expected ')' after perspective name");
  consume(TokenKind::OpenBrace, "Expected '{' to start perspective body");
  ScriptBody body = parseScriptBody();
  consume(TokenKind::CloseBrace, "Expected '}' to close perspective body");

  return PerspectiveBlock{
      .perspectiveName       = std::move(name),
      .perspectiveIsVariable = fromVariable,
      .body                  = std::make_shared<ScriptBody>(std::move(body)),
  };
}

StoreAnchor Parser::parseStoreAnchor() {
  StoreAnchor anchor;
  if (match(TokenKind::Home)) {
    anchor.method = "";
  } else if (check(TokenKind::KwCurrent) || check(TokenKind::KwLocal) ||
             check(TokenKind::KwTopic) || check(TokenKind::KwBtpk) ||
             check(TokenKind::KwCiting) || check(TokenKind::KwBacklinks) ||
             check(TokenKind::KwSystem) || check(TokenKind::KwBtih) ||
             check(TokenKind::KwLedger) || check(TokenKind::KwSlices) ||
             check(TokenKind::KwDocs) || check(TokenKind::NamedStore)) {
    Token tok     = advance();
    anchor.method = (tok.kind == TokenKind::NamedStore) ? tok.stringValue
                                                        : std::string(tok.text);
    if (match(TokenKind::OpenParen)) {
      if (!check(TokenKind::CloseParen)) {
        while (true) {
          anchor.args.push_back(parseValueExpr());
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      consume(TokenKind::CloseParen,
              "Expected ')' after store anchor arguments");
    }
  } else {
    throw ParseError("Expected store anchor (## or ##method(...))",
                     current_.loc);
  }

  if (match(TokenKind::Root)) {
    Token sTok =
        consume(TokenKind::Identifier, "Expected slice identifier after '#'");
    anchor.sliceName = std::string(sTok.text);
  }

  if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
    advance();
    anchor.derefMaster = true;
  }

  return anchor;
}

MountDeclaration Parser::parseMountDeclaration() {
  consume(TokenKind::KwMount, "Expected 'mount'");
  StoreAnchor storeAnchor = parseStoreAnchor();
  consume(TokenKind::KwAs,
          "Expected 'as' after store anchor in mount declaration");

  zigzag::DimVector direction = zigzag::DimVector::POS;
  if (match(TokenKind::Plus)) {
    direction = zigzag::DimVector::POS;
  } else if (match(TokenKind::Minus)) {
    direction = zigzag::DimVector::NEG;
  }

  Token dimTok        = consume(TokenKind::Identifier,
                                "Expected dimension name in mount declaration");
  std::string dimName = std::string(dimTok.text);
  match(TokenKind::Semicolon);

  return MountDeclaration{
      .storeAnchor = std::move(storeAnchor),
      .dimName     = std::move(dimName),
      .direction   = direction,
  };
}

UnmountDeclaration Parser::parseUnmountDeclaration() {
  consume(TokenKind::KwUnmount, "Expected 'unmount'");
  zigzag::DimVector direction = zigzag::DimVector::POS;
  if (match(TokenKind::Plus)) {
    direction = zigzag::DimVector::POS;
  } else if (match(TokenKind::Minus)) {
    direction = zigzag::DimVector::NEG;
  } else {
    throw ParseError("Expected signed dimension after 'unmount'", current_.loc);
  }
  const Token dim =
      consume(TokenKind::Identifier, "Expected dimension name after 'unmount'");
  return UnmountDeclaration{.dimName   = std::string(dim.text),
                            .direction = direction};
}

DefDeclaration Parser::parseDefDeclaration() {
  consume(TokenKind::KwDef, "Expected 'def'");
  DefDeclaration::Effect effect = DefDeclaration::Effect::Inferred;
  if (match(TokenKind::KwPure)) {
    effect = DefDeclaration::Effect::Pure;
  } else if (match(TokenKind::KwWeave)) {
    effect = DefDeclaration::Effect::Weave;
  }

  Token nameTok =
      consume(TokenKind::Identifier, "Expected function name after 'def'");
  // The three names are reserved, so a definition could never be called;
  // refuse it where it is written, with the code every call gets.
  refuseIoCall(nameTok);
  consume(TokenKind::OpenParen, "Expected '(' after function name");
  std::vector<LambdaParam> params = parseLambdaParams();
  consume(TokenKind::CloseParen, "Expected ')' after parameters in 'def'");
  std::shared_ptr<TypeExpr> returnType;
  if (match(TokenKind::Colon)) {
    returnType = std::make_shared<TypeExpr>(parseTypeExpr());
  }
  consume(TokenKind::Assign, "Expected ':=' after parameter list in 'def'");

  if (check(TokenKind::OpenBrace) && isBlockBody()) {
    consume(TokenKind::OpenBrace, "Expected '{'");
    ScriptBody body = parseScriptBody();
    consume(TokenKind::CloseBrace, "Expected '}' after block body");
    return DefDeclaration{
        .effect     = effect,
        .name       = std::string(nameTok.text),
        .params     = std::move(params),
        .returnType = std::move(returnType),
        .body       = std::make_shared<ScriptBody>(std::move(body)),
    };
  }
  ValueExpr bodyExpr = parseValueOrComparisonExpr();
  return DefDeclaration{
      .effect     = effect,
      .name       = std::string(nameTok.text),
      .params     = std::move(params),
      .returnType = std::move(returnType),
      .body       = std::make_shared<ValueExpr>(std::move(bodyExpr)),
  };
}

PathExpression Parser::parsePathExpression(bool allowCloneTail) {
  // A leading '/' has no anchor to read from: PartialPathFunctor (§4.12.3).
  const bool isFunctor = check(TokenKind::Slash);
  bool implicitContext = false;
  if (check(TokenKind::Identifier) || check(TokenKind::Plus) ||
      check(TokenKind::Minus) || check(TokenKind::OpenParen) ||
      check(TokenKind::Tilde) || check(TokenKind::Backslash) ||
      (check(TokenKind::KwFind) && peekToken().is(TokenKind::OpenParen))) {
    implicitContext = true;
  }

  AnchorNode anchor = parseAnchorNode();
  std::vector<PathStep> steps;
  std::vector<BooleanExpr> anchorPredicates;

  if (implicitContext) {
    steps.push_back(parsePathStep(false /*requireSlash*/));
  } else {
    // Predicates straight after the anchor filter its own cells.
    while (check(TokenKind::OpenBracket)) {
      consume(TokenKind::OpenBracket, "Expected '['");
      anchorPredicates.push_back(parseBooleanExpr());
      consume(TokenKind::CloseBracket, "Expected ']' after predicate");
    }
  }

  std::optional<ShapeLiteral> anchorProjection;
  std::optional<ShapeLiteral> shapeProjection;
  const auto hasShapeProjection = [this]() {
    if (!check(TokenKind::OpenBrace)) return false;
    const Token nextTok = peekToken();
    return !nextTok.is(TokenKind::KwWeave) && !nextTok.is(TokenKind::KwFor) &&
           !nextTok.is(TokenKind::KwLet) && !nextTok.is(TokenKind::KwIf) &&
           !nextTok.is(TokenKind::KwReturn) && !nextTok.is(TokenKind::KwWatch);
  };
  const auto projectionContinuesPath = [this]() {
    return check(TokenKind::Slash) || check(TokenKind::Arrow) ||
           check(TokenKind::Transclude) ||
           (check(TokenKind::Dot) && peekToken().is(TokenKind::OpenBracket));
  };
  const auto recordProjection = [&](ShapeLiteral projection) {
    if (hasShapeProjection())
      throw ParseError("consecutive shape projections are not supported",
                       current_.loc);
    if (!projectionContinuesPath()) {
      shapeProjection = std::move(projection);
    } else if (steps.empty()) {
      anchorProjection = std::move(projection);
    } else {
      steps.back().projectionAfter = std::move(projection);
    }
  };

  if (hasShapeProjection()) recordProjection(parseShapeLiteral());

  while (check(TokenKind::Slash) || check(TokenKind::Dot) ||
         check(TokenKind::Arrow) || check(TokenKind::Transclude)) {
    // Slicing sugar: .[offset, length]
    if (check(TokenKind::Dot) && peekToken().is(TokenKind::OpenBracket)) {
      advance(); // consume '.'
      consume(TokenKind::OpenBracket, "Expected '[' after '.' for slice");
      ValueExpr offset = parseValueExpr();
      std::vector<ValueExpr> args;
      args.push_back(
          ValueExpr{.kind = std::make_shared<PathExpression>(PathExpression{
                        .anchor = AnchorNode{.kind = AnchorKind::Context}})});
      args.push_back(std::move(offset));
      if (match(TokenKind::Comma)) {
        args.push_back(parseValueExpr());
      }
      consume(TokenKind::CloseBracket, "Expected ']' after slice arguments");

      FunctionInvocation fn{
          .name = "value", .args = std::move(args), .isSliceRead = true};
      PathStep step{.selector = std::move(fn)};
      steps.push_back(std::move(step));
      if (hasShapeProjection()) recordProjection(parseShapeLiteral());
      continue;
    }

    if (check(TokenKind::Arrow) || check(TokenKind::Transclude)) {
      PathStep step = parsePathStep(false /*requireSlash*/);
      if (std::holds_alternative<PathMorphism>(step.selector) &&
          !steps.empty() && !steps.back().projectionAfter &&
          std::holds_alternative<SignedDimensionStep>(steps.back().selector)) {
        const auto &sd = std::get<SignedDimensionStep>(steps.back().selector);
        auto &m        = std::get<PathMorphism>(step.selector);
        m.subject =
            ValueExpr{.kind = ScalarLiteral{
                          (sd.direction == zigzag::DimVector::NEG ? "-" : "+") +
                          sd.dimName}};
        steps.pop_back();
      }
      steps.push_back(std::move(step));
      if (hasShapeProjection()) recordProjection(parseShapeLiteral());
      continue;
    }

    if (check(TokenKind::Slash)) {
      steps.push_back(parsePathStep(true /*requireSlash*/));
      if (hasShapeProjection()) recordProjection(parseShapeLiteral());
    } else {
      break;
    }
  }

  std::optional<CloneTail> cloneTail;
  if (allowCloneTail && check(TokenKind::CloneJoin)) {
    cloneTail = parseCloneTail();
  }

  if (shapeProjection &&
      (check(TokenKind::Slash) || check(TokenKind::Dot) ||
       check(TokenKind::Arrow) || check(TokenKind::Transclude)))
    throw ParseError(
        "steps after shape projection require an explicit intermediate binding",
        current_.loc);

  return PathExpression{.anchor           = std::move(anchor),
                        .isFunctor        = isFunctor,
                        .anchorPredicates = std::move(anchorPredicates),
                        .anchorProjection = std::move(anchorProjection),
                        .steps            = std::move(steps),
                        .shapeProjection  = std::move(shapeProjection),
                        .cloneTail        = std::move(cloneTail)};
}

AnchorNode Parser::parseAnchorNode() {
  if (match(TokenKind::Home)) {
    AnchorNode node{.kind = AnchorKind::Home};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::NamedStore)) {
    Token tok = advance();
    if (check(TokenKind::OpenParen)) {
      advance(); // consume '('
      std::vector<ValueExpr> args;
      if (!check(TokenKind::CloseParen)) {
        while (true) {
          args.push_back(parseValueExpr());
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      consume(TokenKind::CloseParen,
              "Expected ')' after store anchor arguments");
      std::string sliceName;
      if (match(TokenKind::Root)) {
        Token sTok = consume(TokenKind::Identifier,
                             "Expected slice identifier after '#'");
        sliceName  = std::string(sTok.text);
      }
      AnchorNode node{.kind        = AnchorKind::StoreAnchor,
                      .storeAnchor = StoreAnchor{
                          .method    = tok.stringValue,
                          .args      = std::move(args),
                          .sliceName = std::move(sliceName),
                      }};
      if (check(TokenKind::At)) {
        node.versionPin = parseVersionPin();
      }
      if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
        advance();
        node.derefMaster = true;
      }
      return node;
    }
    AnchorNode node{.kind = AnchorKind::NamedStore, .name = tok.stringValue};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::KwCurrent) || check(TokenKind::KwLocal) ||
      check(TokenKind::KwTopic) || check(TokenKind::KwBtpk) ||
      check(TokenKind::KwCiting) || check(TokenKind::KwBacklinks) ||
      check(TokenKind::KwSystem) || check(TokenKind::KwBtih) ||
      check(TokenKind::KwLedger) || check(TokenKind::KwSlices) ||
      check(TokenKind::KwDocs)) {
    Token genTok = advance();
    consume(TokenKind::OpenParen, "Expected '(' after generator name");
    std::vector<ValueExpr> args;
    if (!check(TokenKind::CloseParen)) {
      while (true) {
        args.push_back(parseValueExpr());
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
    }
    consume(TokenKind::CloseParen, "Expected ')' after generator arguments");
    AnchorNode node{.kind      = AnchorKind::StoreGenerator,
                    .generator = StoreGeneratorCall{
                        .opName = std::string(genTok.text),
                        .args   = std::move(args),
                    }};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::MicroversionLiteral) ||
      check(TokenKind::TypedVersionLiteral)) {
    Token tok = advance();
    AnchorNode node{.kind           = AnchorKind::Microversion,
                    .microversionId = tok.stringValue};
    return node;
  }

  if (match(TokenKind::Root)) {
    AnchorNode node{.kind = AnchorKind::Root};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Cursor)) {
    AnchorNode node{.kind = AnchorKind::Cursor};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::NamedCursor)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::NamedCursor, .name = tok.stringValue};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::AllCursors)) {
    AnchorNode node{.kind = AnchorKind::AllCursors};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::AllNamedCursors)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::AllNamedCursors,
                    .name = tok.stringValue};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::Variable)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::Variable, .name = tok.stringValue};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::IntegerLiteral)) {
    Token tok = advance();
    if (tok.intValue == 0) {
      throw ParseError("LiteralCellId cannot be 0 (Rule R5: 0 is absence of a "
                       "cell/sentinel)",
                       tok.loc);
    }
    AnchorNode node{.kind   = AnchorKind::LiteralCellId,
                    .cellId = static_cast<std::uint64_t>(tok.intValue)};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Dot)) {
    AnchorNode node{.kind = AnchorKind::Context};
    if (check(TokenKind::At)) {
      node.versionPin = parseVersionPin();
    }
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Percent)) {
    CreateValue cv;
    if (!check(TokenKind::Slash) && !check(TokenKind::OpenBracket) &&
        !check(TokenKind::CloseBracket) && !check(TokenKind::Bang) &&
        !check(TokenKind::Comma) && !check(TokenKind::OpenBrace) &&
        !check(TokenKind::CloseBrace) && !check(TokenKind::CloseParen) &&
        !check(TokenKind::Percent) && !check(TokenKind::CloneJoin) &&
        !check(TokenKind::EndOfFile)) {
      if (check(TokenKind::StringLiteral)) {
        Token sTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = sTok.stringValue;
      } else if (check(TokenKind::IntegerLiteral)) {
        Token iTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = std::to_string(iTok.intValue);
      } else if (check(TokenKind::FloatLiteral)) {
        Token fTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = std::to_string(fTok.floatValue);
      } else if (check(TokenKind::Identifier)) {
        Token idTok = advance();
        cv.kind     = CreateValue::Kind::Literal;
        cv.literal  = std::string(idTok.text);
      }
    }
    AnchorNode node{.kind = AnchorKind::Create, .createValue = std::move(cv)};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  // Inside predicate clauses, for top-level functions, or shape projections,
  // anchor can default to context '.'
  if (check(TokenKind::Slash) || check(TokenKind::Identifier) ||
      check(TokenKind::Plus) || check(TokenKind::Minus) ||
      check(TokenKind::OpenParen) || check(TokenKind::Tilde) ||
      check(TokenKind::KwFind) || check(TokenKind::OpenBrace) ||
      check(TokenKind::Backslash)) {
    return AnchorNode{.kind = AnchorKind::Context};
  }

  throw ParseError("Expected anchor node (##, ##NAME, #, ^, ^NAME, $var, "
                   "generator, literal id, or .)",
                   current_.loc);
}

PathStep Parser::parsePathStep(bool requireSlash) {
  if (requireSlash) {
    consume(TokenKind::Slash, "Expected '/' at start of path step");
  } else {
    match(TokenKind::Slash);
  }
  // `$up / /+d.title`: composing with a functor literal is appending its
  // steps, so the functor's own leading '/' is the step's.
  match(TokenKind::Slash);
  StepSelector selector = parseStepSelector();

  bool derefMaster = false;
  if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
    advance();
    derefMaster = true;
  }

  std::vector<BooleanExpr> predicates;
  std::vector<ShapeLiteral> shapePredicates;
  std::optional<RangeClamp> clamp;

  while (check(TokenKind::OpenBracket)) {
    if (peekToken().is(TokenKind::OpenBrace)) {
      consume(TokenKind::OpenBracket, "Expected '['");
      shapePredicates.push_back(parseShapeLiteral());
      consume(TokenKind::CloseBracket, "Expected ']' after shape predicate");
      continue;
    }

    Token next         = peekToken();
    const bool isClamp = next.is(TokenKind::IntegerLiteral) ||
                         next.is(TokenKind::Minus) || next.is(TokenKind::Plus);

    if (isClamp) {
      clamp = parseRangeClamp();
    } else {
      consume(TokenKind::OpenBracket, "Expected '['");
      predicates.push_back(parseBooleanExpr());
      consume(TokenKind::CloseBracket, "Expected ']' after predicate");
    }
  }

  std::optional<VersionPin> versionPin;
  if (check(TokenKind::At)) {
    versionPin = parseVersionPin();
  }

  if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
    advance();
    derefMaster = true;
  }

  YieldMode yieldMode = parseYield();

  return PathStep{
      .selector        = std::move(selector),
      .derefMaster     = derefMaster,
      .predicates      = std::move(predicates),
      .shapePredicates = std::move(shapePredicates),
      .rangeClamp      = clamp,
      .versionPin      = std::move(versionPin),
      .yieldMode       = yieldMode,
  };
}

StepSelector Parser::parseStepSelector() {
  if (match(TokenKind::Backslash)) {
    return std::make_shared<PathLambda>(parsePathLambda());
  }
  if (match(TokenKind::Home)) {
    return RootSelectorStep{.kind = RootSelectorStep::Kind::Genesis};
  }
  if (match(TokenKind::Root)) {
    return RootSelectorStep{.kind = RootSelectorStep::Kind::RootMetacells};
  }
  if (match(TokenKind::Cursor)) {
    return RootSelectorStep{.kind = RootSelectorStep::Kind::ActiveCursor};
  }
  if (match(TokenKind::AllCursors)) {
    return RootSelectorStep{.kind = RootSelectorStep::Kind::AllCursors};
  }
  if (check(TokenKind::NamedCursor)) {
    Token tok = advance();
    return RootSelectorStep{.kind = RootSelectorStep::Kind::NamedCursor,
                            .name = tok.stringValue};
  }
  if (check(TokenKind::AllNamedCursors)) {
    Token tok = advance();
    return RootSelectorStep{.kind = RootSelectorStep::Kind::AllNamedCursors,
                            .name = tok.stringValue};
  }

  // PerspectiveStep: /@<ident>:<dim_or_step> or /@pers:+d.tag
  if (check(TokenKind::At) && (peekToken().is(TokenKind::Identifier) ||
                               peekToken().is(TokenKind::StringLiteral))) {
    advance(); // consume '@'
    std::string perspName;
    if (check(TokenKind::StringLiteral)) {
      perspName = advance().stringValue;
    } else {
      perspName = std::string(advance().text);
    }
    consume(TokenKind::Colon, "Expected ':' after perspective name in step");
    match(TokenKind::Slash); // optional '/'
    PathStep targetStep = parsePathStep(false /*requireSlash*/);
    return PerspectiveStep{
        .perspective = std::move(perspName),
        .targetStep  = std::make_shared<PathStep>(std::move(targetStep)),
    };
  }

  if (check(TokenKind::KwSlices)) {
    advance();
    consume(TokenKind::OpenParen, "Expected '(' after 'slices'");
    SliceSelectorStep step;
    if (check(TokenKind::StringLiteral)) {
      Token sTok      = advance();
      step.nameFilter = sTok.stringValue;
    } else if (!check(TokenKind::CloseParen)) {
      step.predicateFilter = std::make_shared<BooleanExpr>(parseBooleanExpr());
    }
    consume(TokenKind::CloseParen, "Expected ')' after 'slices' arguments");
    return step;
  }

  if (check(TokenKind::KwDocs)) {
    advance();
    consume(TokenKind::OpenParen, "Expected '(' after 'docs'");
    DocSelectorStep step;
    if (check(TokenKind::StringLiteral)) {
      Token sTok      = advance();
      step.nameFilter = sTok.stringValue;
    } else if (!check(TokenKind::CloseParen)) {
      step.predicateFilter = std::make_shared<BooleanExpr>(parseBooleanExpr());
    }
    consume(TokenKind::CloseParen, "Expected ')' after 'docs' arguments");
    return step;
  }

  if (match(TokenKind::DimPivot)) {
    Token fromTok =
        consume(TokenKind::Identifier, "Expected from-dimension in pivot");
    consume(TokenKind::Comma, "Expected ',' between dimensions in pivot");
    Token toTok =
        consume(TokenKind::Identifier, "Expected to-dimension in pivot");
    return DimensionPivotStep{.fromDim = std::string(fromTok.text),
                              .toDim   = std::string(toTok.text)};
  }

  if (check(TokenKind::Arrow) || check(TokenKind::Transclude)) {
    Token opTok = advance();
    PathMorphism morphism;
    morphism.op = (opTok.kind == TokenKind::Arrow)
                      ? PathMorphism::Op::Arrow
                      : PathMorphism::Op::Transclude;
    if (check(TokenKind::OpenBracket)) {
      advance(); // consume '['
      morphism.isSproutList = true;
      while (!check(TokenKind::CloseBracket) && !check(TokenKind::EndOfFile)) {
        morphism.targets.push_back(parseValueExpr());
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
      consume(TokenKind::CloseBracket, "Expected ']' after sprout list");
    } else {
      morphism.targets.push_back(parseValueExpr());
    }
    return morphism;
  }

  if (match(TokenKind::OpenParen)) {
    std::vector<PathStep> macroSteps;
    while (!check(TokenKind::CloseParen) && !check(TokenKind::EndOfFile)) {
      macroSteps.push_back(parsePathStep());
    }
    consume(TokenKind::CloseParen, "Expected ')' after macro dimension group");

    Repetition rep = Repetition::Once;
    if (match(TokenKind::Star)) {
      rep = Repetition::ZeroOrMore;
    } else if (match(TokenKind::Plus)) {
      rep = Repetition::OneOrMore;
    } else if (match(TokenKind::Question)) {
      rep = Repetition::ZeroOrOne;
    }

    return std::make_shared<MacroDimensionGroup>(
        MacroDimensionGroup{.steps = std::move(macroSteps), .repetition = rep});
  }

  if (check(TokenKind::Variable) && peekToken().is(TokenKind::OpenParen))
    return parseRuntimeClosureInvocation();

  // Function invocation: func(args...) or find(args...)
  if ((check(TokenKind::Identifier) || check(TokenKind::KwFind)) &&
      peekToken().is(TokenKind::OpenParen)) {
    return parseFunctionInvocation();
  }

  // SignedDimension (or bidirectional '~d.dim')
  bool isBidir                = match(TokenKind::Tilde);
  zigzag::DimVector direction = zigzag::DimVector::POS;
  if (match(TokenKind::Plus)) {
    direction = zigzag::DimVector::POS;
  } else if (match(TokenKind::Minus)) {
    direction = zigzag::DimVector::NEG;
  }

  std::string dimName;
  SourceLocation dimLoc = current_.loc;
  if (check(TokenKind::Identifier)) {
    dimName = std::string(advance().text);
  } else if (check(TokenKind::Variable)) {
    dimName = "$" + advance().stringValue;
  } else {
    dimName = std::string(
        consume(TokenKind::Identifier, "Expected dimension identifier").text);
  }

  if (match(TokenKind::DimPivot)) {
    Token toTok =
        consume(TokenKind::Identifier, "Expected target dimension in pivot");
    return DimensionPivotStep{.fromDim = std::move(dimName),
                              .toDim   = std::string(toTok.text)};
  }

  if (match(TokenKind::Assign)) {
    ValueExpr expr = parseValueExpr();
    return VirtualDimensionStep{
        .dimStep =
            SignedDimensionStep{
                .dimName   = std::move(dimName),
                .direction = direction,
                .isBidir   = isBidir,
            },
        .expr = std::move(expr),
    };
  }

  Placement placement = Placement::Default;
  if (match(TokenKind::DoubleColon)) {
    Token pTok = consume(TokenKind::Identifier,
                         "Expected placement (from, rank, head, tail)");
    if (pTok.text == "from") {
      placement = Placement::From;
    } else if (pTok.text == "rank") {
      placement = Placement::Rank;
    } else if (pTok.text == "head") {
      placement = Placement::Head;
    } else if (pTok.text == "tail") {
      placement = Placement::Tail;
    } else {
      throw ParseError("Unknown placement '::" + std::string(pTok.text) + "'",
                       pTok.loc);
    }
  }

  std::vector<CreateValue> creates;
  while (match(TokenKind::Percent)) {
    if (placement == Placement::From || placement == Placement::Rank) {
      throw ParseError("Placement '::from' and '::rank' cannot be used with "
                       "'%' creation sugar (Rule §4.5)",
                       dimLoc);
    }

    CreateValue cv;
    if (!check(TokenKind::Slash) && !check(TokenKind::OpenBracket) &&
        !check(TokenKind::CloseBracket) && !check(TokenKind::Bang) &&
        !check(TokenKind::Comma) && !check(TokenKind::OpenBrace) &&
        !check(TokenKind::CloseBrace) && !check(TokenKind::CloseParen) &&
        !check(TokenKind::Percent) && !check(TokenKind::CloneJoin) &&
        !check(TokenKind::EndOfFile)) {
      if (check(TokenKind::StringLiteral)) {
        Token sTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = sTok.stringValue;
      } else if (check(TokenKind::Variable)) {
        cv.kind = CreateValue::Kind::Expression;
        cv.expr = std::make_shared<ValueExpr>(parseValueExpr());
      } else if (check(TokenKind::Identifier)) {
        Token idTok = advance();
        cv.kind     = CreateValue::Kind::Literal;
        cv.literal  = std::string(idTok.text);
      } else if (check(TokenKind::IntegerLiteral) ||
                 check(TokenKind::FloatLiteral)) {
        Token numTok = advance();
        cv.kind      = CreateValue::Kind::Literal;
        cv.literal   = std::string(numTok.text);
      } else {
        Token bTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = std::string(bTok.text);
      }
    }
    creates.push_back(std::move(cv));
  }

  return SignedDimensionStep{
      .dimName   = std::move(dimName),
      .direction = direction,
      .isBidir   = isBidir,
      .placement = placement,
      .creates   = std::move(creates),
  };
}

RangeClamp Parser::parseRangeClamp() {
  consume(TokenKind::OpenBracket, "Expected '[' for range clamp");

  int sign1 = 1;
  if (match(TokenKind::Minus)) {
    sign1 = -1;
  } else if (match(TokenKind::Plus)) {
    sign1 = 1;
  }
  Token startTok =
      consume(TokenKind::IntegerLiteral, "Expected integer for start clamp");
  std::int64_t start = sign1 * startTok.intValue;

  std::optional<std::int64_t> end;
  if (match(TokenKind::Comma)) {
    int sign2 = 1;
    if (match(TokenKind::Minus)) {
      sign2 = -1;
    } else if (match(TokenKind::Plus)) {
      sign2 = 1;
    }
    Token endTok =
        consume(TokenKind::IntegerLiteral, "Expected integer for end clamp");
    end = sign2 * endTok.intValue;
  } else {
    end = start;
  }

  consume(TokenKind::CloseBracket, "Expected ']' after range clamp");
  return RangeClamp{.start = start, .end = end};
}

YieldMode Parser::parseYield() {
  if (match(TokenKind::Bang)) {
    if (check(TokenKind::Identifier)) {
      Token yTok = currentToken();
      if (yTok.text == "new") {
        advance();
        return YieldMode::New;
      }
      if (yTok.text == "last") {
        advance();
        return YieldMode::Last;
      }
      if (yTok.text == "both") {
        advance();
        return YieldMode::Both;
      }
      if (yTok.text == "keep") {
        advance();
        return YieldMode::Keep;
      }
    }
    return YieldMode::Keep; // Bare '!' is sugar for '!keep'
  }
  return YieldMode::Default;
}

CloneTail Parser::parseCloneTail() {
  std::vector<CloneOperand> operands;
  while (match(TokenKind::CloneJoin)) {
    CloneOperand op;
    if (match(TokenKind::Percent)) {
      CreateValue cv;
      if (check(TokenKind::StringLiteral)) {
        Token sTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = sTok.stringValue;
      } else if (check(TokenKind::Variable)) {
        cv.kind = CreateValue::Kind::Expression;
        cv.expr = std::make_shared<ValueExpr>(parseValueExpr());
      } else if (check(TokenKind::Identifier)) {
        Token idTok = advance();
        cv.kind     = CreateValue::Kind::Literal;
        cv.literal  = std::string(idTok.text);
      } else if (check(TokenKind::IntegerLiteral) ||
                 check(TokenKind::FloatLiteral)) {
        Token numTok = advance();
        cv.kind      = CreateValue::Kind::Literal;
        cv.literal   = std::string(numTok.text);
      } else if (!check(TokenKind::CloneJoin) && !check(TokenKind::Comma) &&
                 !check(TokenKind::CloseBrace) &&
                 !check(TokenKind::CloseParen) &&
                 !check(TokenKind::EndOfFile)) {
        Token bTok = advance();
        cv.kind    = CreateValue::Kind::Literal;
        cv.literal = std::string(bTok.text);
      }
      op.bareCreate = std::move(cv);
    } else {
      op.path = std::make_shared<PathExpression>(
          parsePathExpression(false /*allowCloneTail*/));
    }
    operands.push_back(std::move(op));
  }
  return CloneTail{.operands = std::move(operands)};
}

ExecutionBlock Parser::parseExecutionBlock() {
  std::vector<std::variant<ForClause, LetClause>> bindings;
  std::optional<WhereClause> where;

  while (check(TokenKind::KwFor) || check(TokenKind::KwLet) ||
         check(TokenKind::KwWhere)) {
    if (check(TokenKind::KwFor)) {
      bindings.emplace_back(parseForClause());
    } else if (check(TokenKind::KwLet)) {
      bindings.emplace_back(parseLetClause());
    } else {
      where = parseWhereClause();
    }
  }

  if (bindings.empty() && !check(TokenKind::KwWeave) &&
      !check(TokenKind::KwIf) && !check(TokenKind::KwReturn)) {
    throw ParseError("Execution block must have at least one 'for' or 'let' "
                     "clause, or an action clause",
                     current_.loc);
  }

  ActionClause action{};
  if (check(TokenKind::KwReturn) || check(TokenKind::KwWeave) ||
      check(TokenKind::KwIf)) {
    action = parseActionClause();
  } else if (!bindings.empty() &&
             (check(TokenKind::Semicolon) || check(TokenKind::CloseBrace) ||
              check(TokenKind::EndOfFile))) {
    action = ActionClause{.clause = ReturnClause{}};
  } else {
    action = parseActionClause();
  }

  return ExecutionBlock{
      .bindings = std::move(bindings),
      .where    = std::move(where),
      .action   = std::move(action),
  };
}

ForClause Parser::parseForClause() {
  consume(TokenKind::KwFor, "Expected 'for'");
  std::string varName;
  std::optional<DestructurePattern> destructurePattern;
  if (check(TokenKind::OpenBrace)) {
    destructurePattern = parseDestructurePattern();
  } else {
    Token varTok =
        consume(TokenKind::Variable, "Expected $variable after 'for'");
    varName = varTok.stringValue;
    ioNames_.erase(varName);
  }
  consume(TokenKind::KwIn, "Expected 'in' after variable");
  PathExpression path = parsePathExpression();
  return ForClause{.varName            = std::move(varName),
                   .destructurePattern = std::move(destructurePattern),
                   .inPath             = std::move(path)};
}

LetClause Parser::parseLetClause() {
  consume(TokenKind::KwLet, "Expected 'let'");
  Token varTok = consume(TokenKind::Variable, "Expected $variable after 'let'");
  consume(TokenKind::Assign, "Expected ':=' after variable");
  ioNames_.erase(varTok.stringValue);

  std::variant<PathExpression, ValueExpr> target;
  if (check(TokenKind::Home) || check(TokenKind::NamedStore) ||
      check(TokenKind::Root) || check(TokenKind::Cursor) ||
      check(TokenKind::NamedCursor) || check(TokenKind::Slash) ||
      check(TokenKind::Dot) || check(TokenKind::Percent) ||
      (check(TokenKind::Variable) &&
       (peekToken().is(TokenKind::Slash) || peekToken().is(TokenKind::Dot) ||
        peekToken().is(TokenKind::GreaterThan) ||
        peekToken().is(TokenKind::CloneJoin)))) {
    target = parsePathExpression();
  } else {
    target = parseValueExpr();
  }

  return LetClause{.varName = varTok.stringValue, .target = std::move(target)};
}

WhereClause Parser::parseWhereClause() {
  consume(TokenKind::KwWhere, "Expected 'where'");
  BooleanExpr expr = parseBooleanExpr();
  return WhereClause{.condition = std::move(expr)};
}

ActionClause Parser::parseActionClause() {
  if (atIoEffect()) {
    throw ParseError(
        std::format("{}: an I/O script action ('effect') cannot be the action "
                    "of a for/let/where/if block; it runs once per script, "
                    "never per row",
                    vortex::frontend_codes::vqlQueryContext),
        current_.loc);
  }
  if (check(TokenKind::KwReturn)) {
    return ActionClause{.clause = parseReturnClause()};
  }
  if (check(TokenKind::KwWeave)) {
    return ActionClause{.clause = parseEffectClause()};
  }
  if (check(TokenKind::KwIf)) {
    return ActionClause{.clause = parseConditionalClause()};
  }
  throw ParseError("Expected action clause ('return', 'weave', or 'if')",
                   current_.loc);
}

ReturnClause Parser::parseReturnClause() {
  consume(TokenKind::KwReturn, "Expected 'return'");
  std::vector<ReturnItem> items;

  while (true) {
    items.push_back(parseReturnItem());
    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  return ReturnClause{.items = std::move(items)};
}

ReturnItem Parser::parseReturnItem() {
  if (check(TokenKind::OpenParen) && !peekToken().is(TokenKind::Slash) &&
      !peekToken().is(TokenKind::CloseParen)) {
    consume(TokenKind::OpenParen, "Expected '(' before return value");
    ValueExpr expression = parseValueOrComparisonExpr();
    const Token closeParen =
        consume(TokenKind::CloseParen,
                "Expected ')' after parenthesized return value");
    // Newlines separate statements. On the same line, require an item or
    // clause boundary so a value suffix cannot become a second path query.
    if (current_.loc.line == closeParen.loc.line && !check(TokenKind::Comma) &&
        !check(TokenKind::Semicolon) && !check(TokenKind::CloseBrace) &&
        !check(TokenKind::KwElse) && !check(TokenKind::EndOfFile)) {
      throw ParseError("Continuations after a parenthesized return value "
                       "require a statement separator",
                       current_.loc);
    }
    return ReturnItem{.item = ReturnValue{.expression = std::move(expression)}};
  }
  if (check(TokenKind::Variable) && peekToken().is(TokenKind::OpenParen)) {
    ValueExpr expression = parseValueOrComparisonExpr();
    return ReturnItem{.item = ReturnValue{.expression = std::move(expression)}};
  }
  if (check(TokenKind::Slash)) {
    // FieldWeave: sequence of PathSteps rooted at result cell
    std::vector<PathStep> steps;
    while (check(TokenKind::Slash)) {
      steps.push_back(parsePathStep());
    }
    return ReturnItem{.item = FieldWeave{.steps = std::move(steps)}};
  }
  return ReturnItem{.item = parsePathExpression()};
}

EffectClause Parser::parseEffectClause() {
  consume(TokenKind::KwWeave, "Expected 'weave'");
  std::vector<EffectItem> items;

  auto parseSingleItem = [this]() -> EffectItem {
    std::string effectPersp;
    if (check(TokenKind::At) && (peekToken().is(TokenKind::Identifier) ||
                                 peekToken().is(TokenKind::StringLiteral))) {
      advance(); // consume '@'
      if (check(TokenKind::StringLiteral)) {
        effectPersp = advance().stringValue;
      } else {
        effectPersp = std::string(advance().text);
      }
      consume(TokenKind::Colon, "Expected ':' after perspective name in weave");
    }

    if (check(TokenKind::KwLet)) {
      return EffectItem{.item = parseLetClause()};
    }
    if (check(TokenKind::KwFor)) {
      ForClause forC   = parseForClause();
      EffectClause eff = parseEffectClause();
      return EffectItem{
          .item = std::make_pair(
              std::move(forC), std::make_shared<EffectClause>(std::move(eff)))};
    }
    PathExpression path = parsePathExpression();
    if (!effectPersp.empty()) {
      for (auto &step : path.steps) {
        if (std::holds_alternative<PathMorphism>(step.selector)) {
          auto &m = std::get<PathMorphism>(step.selector);
          if (m.perspective.empty()) {
            m.perspective = effectPersp;
          }
        }
      }
    }
    return EffectItem{.item = std::move(path)};
  };

  if (match(TokenKind::OpenBrace)) {
    while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
      items.push_back(parseSingleItem());
      match(TokenKind::Comma);
    }
    consume(TokenKind::CloseBrace, "Expected '}' after weave block");
  } else {
    items.push_back(parseSingleItem());
  }

  return EffectClause{.items = std::move(items)};
}

ConditionalClause Parser::parseConditionalClause() {
  consume(TokenKind::KwIf, "Expected 'if'");
  BooleanExpr cond = parseBooleanExpr();
  auto thenClause  = std::make_shared<ActionClause>(parseActionClause());
  std::shared_ptr<ActionClause> elseClause = nullptr;

  if (match(TokenKind::KwElse)) {
    elseClause = std::make_shared<ActionClause>(parseActionClause());
  }

  return ConditionalClause{
      .condition  = std::move(cond),
      .thenClause = std::move(thenClause),
      .elseClause = std::move(elseClause),
  };
}

BooleanExpr Parser::parseBooleanExpr() {
  std::vector<BooleanTerm> terms;
  terms.push_back(parseBooleanTerm());

  while (match(TokenKind::KwOr)) {
    terms.push_back(parseBooleanTerm());
  }

  return BooleanExpr{.terms = std::move(terms)};
}

BooleanTerm Parser::parseBooleanTerm() {
  std::vector<BooleanFactor> factors;
  factors.push_back(parseBooleanFactor());

  while (match(TokenKind::KwAnd)) {
    factors.push_back(parseBooleanFactor());
  }

  return BooleanTerm{.factors = std::move(factors)};
}

BooleanFactor Parser::parseBooleanFactor() {
  bool negated = match(TokenKind::KwNot);

  if (check(TokenKind::Question)) {
    return BooleanFactor{.negated = negated, .test = parsePredicateTest()};
  }

  // Look ahead to check if it is a comparison
  ValueExpr left = parseValueExpr();

  CompOp op   = CompOp::Equal;
  bool isComp = false;

  if (match(TokenKind::Equal)) {
    op     = CompOp::Equal;
    isComp = true;
  } else if (match(TokenKind::NotEqual)) {
    op     = CompOp::NotEqual;
    isComp = true;
  } else if (match(TokenKind::LessThan)) {
    op     = CompOp::LessThan;
    isComp = true;
  } else if (match(TokenKind::GreaterThan)) {
    op     = CompOp::GreaterThan;
    isComp = true;
  } else if (match(TokenKind::LessEqual)) {
    op     = CompOp::LessEqual;
    isComp = true;
  } else if (match(TokenKind::GreaterEqual)) {
    op     = CompOp::GreaterEqual;
    isComp = true;
  }

  if (isComp) {
    ValueExpr right = parseValueExpr();
    ComparisonExpr comp{
        .left  = std::make_shared<ValueExpr>(std::move(left)),
        .op    = op,
        .right = std::make_shared<ValueExpr>(std::move(right)),
    };
    return BooleanFactor{.negated = negated, .test = std::move(comp)};
  }

  // Bare predicate test (truthiness of left ValueExpr)
  if (std::holds_alternative<std::shared_ptr<PathExpression>>(left.kind)) {
    return BooleanFactor{
        .negated = negated,
        .test    = PredicateTest{
               .kind = std::get<std::shared_ptr<PathExpression>>(left.kind)}};
  }

  return BooleanFactor{
      .negated = negated,
      .test =
          PredicateTest{.kind = std::make_shared<ValueExpr>(std::move(left))}};
}

PredicateTest Parser::parsePredicateTest() {
  if (check(TokenKind::OpenBrace)) {
    return PredicateTest{
        .kind = std::make_shared<ShapeLiteral>(parseShapeLiteral())};
  }

  if (match(TokenKind::Question)) {
    ValueExpr expr = parseValueExpr();
    return PredicateTest{.kind = std::make_shared<ValueExpr>(std::move(expr))};
  }

  if (match(TokenKind::Dot)) {
    return PredicateTest{.kind = true /*ContextDot*/};
  }

  if ((check(TokenKind::Identifier) || check(TokenKind::KwFind)) &&
      peekToken().is(TokenKind::OpenParen)) {
    return PredicateTest{.kind = std::make_shared<FunctionInvocation>(
                             parseFunctionInvocation())};
  }

  PathExpression path = parsePathExpression();
  return PredicateTest{.kind =
                           std::make_shared<PathExpression>(std::move(path))};
}

ValueExpr Parser::parseValueExpr() {
  ValueExpr expression = parseValueAtom();
  while (match(TokenKind::Plus)) {
    ValueExpr right = parseValueAtom();
    auto addition   = std::make_shared<BinaryValueExpr>();
    addition->left  = std::make_shared<ValueExpr>(std::move(expression));
    addition->right = std::make_shared<ValueExpr>(std::move(right));
    expression      = ValueExpr{.kind = std::move(addition)};
  }
  return expression;
}

ValueExpr Parser::parseValueAtom() {
  if (check(TokenKind::At) && peekToken().is(TokenKind::Identifier) &&
      peekToken().text == "now") {
    advance();
    advance();
    return ValueExpr{.kind = std::make_shared<FunctionInvocation>(
                         FunctionInvocation{.name = "#vql-current-timestamp"})};
  }
  if (check(TokenKind::KwLens) && peekToken().is(TokenKind::OpenParen)) {
    return ValueExpr{.kind = std::make_shared<FunctionInvocation>(
                         parseFunctionInvocation())};
  }
  if (check(TokenKind::OpenBrace)) {
    return ValueExpr{.kind =
                         std::make_shared<ShapeLiteral>(parseShapeLiteral())};
  }
  if (match(TokenKind::Backslash)) {
    return ValueExpr{.kind = std::make_shared<PathLambda>(parsePathLambda())};
  }
  if (check(TokenKind::MicroversionLiteral) ||
      check(TokenKind::TypedVersionLiteral)) {
    return ValueExpr{.kind = std::make_shared<VersionExpr>(parseVersionExpr())};
  }
  if (check(TokenKind::StringLiteral)) {
    Token sTok = advance();
    return ValueExpr{.kind = ScalarLiteral{.value = sTok.stringValue}};
  }
  if (check(TokenKind::IntegerLiteral)) {
    Token iTok = advance();
    return ValueExpr{.kind = ScalarLiteral{.value = iTok.intValue}};
  }
  if (check(TokenKind::FloatLiteral)) {
    Token fTok = advance();
    return ValueExpr{.kind = ScalarLiteral{.value = fTok.floatValue}};
  }
  if (match(TokenKind::KwTrue)) {
    return ValueExpr{.kind = ScalarLiteral{.value = true}};
  }
  if (match(TokenKind::KwFalse)) {
    return ValueExpr{.kind = ScalarLiteral{.value = false}};
  }
  if (check(TokenKind::Variable)) {
    if (peekToken().is(TokenKind::OpenParen)) return parseRuntimeClosureValue();
    if (peekToken().is(TokenKind::Slash) || peekToken().is(TokenKind::Dot) ||
        peekToken().is(TokenKind::OpenBrace) ||
        peekToken().is(TokenKind::GreaterThan) ||
        peekToken().is(TokenKind::CloneJoin)) {
      return ValueExpr{
          .kind = std::make_shared<PathExpression>(parsePathExpression())};
    }
    Token vTok = advance();
    return ValueExpr{.kind = vTok.stringValue};
  }
  if ((check(TokenKind::Identifier) || check(TokenKind::KwFind)) &&
      peekToken().is(TokenKind::OpenParen)) {
    return ValueExpr{.kind = std::make_shared<FunctionInvocation>(
                         parseFunctionInvocation())};
  }

  if (match(TokenKind::OpenParen)) {
    ValueExpr inner = parseValueOrComparisonExpr();
    consume(TokenKind::CloseParen,
            "Expected ')' after parenthesized expression");
    return inner;
  }

  // Fallback: PathExpression
  return ValueExpr{.kind =
                       std::make_shared<PathExpression>(parsePathExpression())};
}

ValueExpr Parser::parseValueOrComparisonExpr() {
  ValueExpr first = parseValueExpr();
  if (check(TokenKind::Equal) || check(TokenKind::NotEqual) ||
      check(TokenKind::LessThan) || check(TokenKind::GreaterThan) ||
      check(TokenKind::LessEqual) || check(TokenKind::GreaterEqual)) {
    Token opTok = advance();
    CompOp op   = CompOp::Equal;
    if (opTok.is(TokenKind::Equal)) {
      op = CompOp::Equal;
    } else if (opTok.is(TokenKind::NotEqual)) {
      op = CompOp::NotEqual;
    } else if (opTok.is(TokenKind::LessThan)) {
      op = CompOp::LessThan;
    } else if (opTok.is(TokenKind::GreaterThan)) {
      op = CompOp::GreaterThan;
    } else if (opTok.is(TokenKind::LessEqual)) {
      op = CompOp::LessEqual;
    } else if (opTok.is(TokenKind::GreaterEqual)) {
      op = CompOp::GreaterEqual;
    }
    ValueExpr right = parseValueExpr();
    ComparisonExpr comp{
        .left  = std::make_shared<ValueExpr>(std::move(first)),
        .op    = op,
        .right = std::make_shared<ValueExpr>(std::move(right)),
    };
    BooleanFactor factor{.negated = false, .test = std::move(comp)};
    BooleanTerm term{.factors = {std::move(factor)}};
    while (match(TokenKind::KwAnd)) {
      term.factors.push_back(parseBooleanFactor());
    }
    BooleanExpr bExpr{.terms = {std::move(term)}};
    while (match(TokenKind::KwOr)) {
      bExpr.terms.push_back(parseBooleanTerm());
    }
    return ValueExpr{.kind = std::make_shared<BooleanExpr>(std::move(bExpr))};
  }
  return first;
}

FunctionInvocation Parser::parseFunctionInvocation() {
  Token nameTok = advance();
  refuseIoCall(nameTok);
  consume(TokenKind::OpenParen, "Expected '(' after function name");
  std::vector<ValueExpr> args;
  std::vector<std::string> names;

  if (!check(TokenKind::CloseParen)) {
    while (true) {
      std::string name;
      if (check(TokenKind::Identifier) && peekToken().is(TokenKind::Colon)) {
        name = std::string(advance().text);
        advance();
      }
      names.push_back(std::move(name));
      args.push_back(parseValueOrComparisonExpr());
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  }

  consume(TokenKind::CloseParen, "Expected ')' after function arguments");
  if (check(TokenKind::Backslash)) {
    advance(); // consume '\'
    args.push_back(
        ValueExpr{.kind = std::make_shared<PathLambda>(parsePathLambda())});
  }
  return FunctionInvocation{.name     = std::string(nameTok.text),
                            .args     = std::move(args),
                            .argNames = std::move(names)};
}

std::shared_ptr<RuntimeClosureInvocation>
Parser::parseRuntimeClosureInvocation() {
  Token calleeToken =
      consume(TokenKind::Variable, "Expected variable closure callee");
  consume(TokenKind::OpenParen, "Expected '(' after closure callee");
  std::vector<ValueExpr> args;
  std::vector<std::string> names;
  if (!check(TokenKind::CloseParen)) {
    while (true) {
      std::string name;
      if (check(TokenKind::Identifier) && peekToken().is(TokenKind::Colon)) {
        name = std::string(advance().text);
        advance();
      }
      names.push_back(std::move(name));
      args.push_back(parseValueOrComparisonExpr());
      if (!match(TokenKind::Comma)) break;
    }
  }
  consume(TokenKind::CloseParen, "Expected ')' after closure arguments");
  return std::make_shared<RuntimeClosureInvocation>(RuntimeClosureInvocation{
      .callee   = ValueExpr{.kind = std::move(calleeToken.stringValue)},
      .args     = std::move(args),
      .argNames = std::move(names),
  });
}

ValueExpr Parser::parseRuntimeClosureValue() {
  auto invocation            = parseRuntimeClosureInvocation();
  const auto pathStepStarter = [](const TokenKind kind) {
    switch (kind) {
    case TokenKind::Home:
    case TokenKind::Root:
    case TokenKind::Cursor:
    case TokenKind::NamedCursor:
    case TokenKind::AllCursors:
    case TokenKind::AllNamedCursors:
    case TokenKind::Variable:
    case TokenKind::Dot:
    case TokenKind::Slash:
    case TokenKind::At:
    case TokenKind::Tilde:
    case TokenKind::Backslash:
    case TokenKind::OpenParen:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::KwSlices:
    case TokenKind::KwDocs:
    case TokenKind::KwCurrent:
    case TokenKind::KwLocal:
    case TokenKind::KwTopic:
    case TokenKind::KwBtpk:
    case TokenKind::KwCiting:
    case TokenKind::KwBacklinks:
    case TokenKind::KwSystem:
    case TokenKind::KwBtih:
    case TokenKind::KwLedger:
    case TokenKind::KwFind:
    case TokenKind::Identifier:
      return true;
    default:
      return false;
    }
  };

  if (check(TokenKind::Slash)) {
    const Token next = peekToken();
    if (!pathStepStarter(next.kind)) {
      if (next.is(TokenKind::IntegerLiteral) ||
          next.is(TokenKind::FloatLiteral))
        throw ParseError(
            "numeric division after a runtime closure call is not supported "
            "by the VQL value grammar",
            current_.loc);
      return ValueExpr{.kind = std::move(invocation)};
    }
  } else if (!check(TokenKind::Arrow) && !check(TokenKind::Transclude)) {
    return ValueExpr{.kind = std::move(invocation)};
  }

  PathExpression path{.anchor = AnchorNode{.kind = AnchorKind::Context}};
  path.steps.push_back(PathStep{.selector = std::move(invocation)});
  while (check(TokenKind::Slash) || check(TokenKind::Arrow) ||
         check(TokenKind::Transclude)) {
    if (check(TokenKind::Slash))
      path.steps.push_back(parsePathStep(true));
    else
      path.steps.push_back(parsePathStep(false));
  }
  return ValueExpr{.kind = std::make_shared<PathExpression>(std::move(path))};
}

DestructurePattern Parser::parseDestructurePattern() {
  consume(TokenKind::OpenBrace, "Expected '{' for destructuring pattern");
  std::vector<DestructureField> fields;
  while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
    fields.push_back(parseDestructureField());
    if (!match(TokenKind::Comma)) {
      break;
    }
  }
  consume(TokenKind::CloseBrace, "Expected '}' after destructuring pattern");
  return DestructurePattern{.fields = std::move(fields)};
}

DestructureField Parser::parseDestructureField() {
  if (match(TokenKind::Dot)) {
    DestructureField field{.isContext = true};
    if (match(TokenKind::Colon)) {
      if (check(TokenKind::Variable)) {
        field.varName = advance().stringValue;
        if (match(TokenKind::Colon)) {
          field.type = std::make_shared<TypeExpr>(parseTypeExpr());
        }
      } else {
        field.type = std::make_shared<TypeExpr>(parseTypeExpr());
      }
    }
    if (match(TokenKind::Assign)) {
      throw ParseError("context destructuring fields cannot have defaults",
                       current_.loc);
    }
    return field;
  }

  DestructureField field;
  field.isBidir = match(TokenKind::Tilde);
  if (match(TokenKind::Plus)) {
    field.direction = zigzag::DimVector::POS;
  } else if (match(TokenKind::Minus)) {
    field.direction = zigzag::DimVector::NEG;
  }
  field.dimName =
      std::string(consume(TokenKind::Identifier,
                          "Expected dimension in destructuring field")
                      .text);

  // `: $var`, `: Type` and `: $var: Type` are told apart by the '$' sigil,
  // which no type name carries.
  if (match(TokenKind::Colon)) {
    if (check(TokenKind::Variable)) {
      field.varName = advance().stringValue;
      if (match(TokenKind::Colon)) {
        field.type = std::make_shared<TypeExpr>(parseTypeExpr());
      }
    } else {
      field.type = std::make_shared<TypeExpr>(parseTypeExpr());
    }
  }
  if (match(TokenKind::Assign)) {
    field.defaultValue = std::make_shared<ValueExpr>(parseValueExpr());
  }
  return field;
}

TypeExpr Parser::parseTypeExpr() {
  UnionType unionType;
  unionType.members.push_back(parsePrimaryType());
  while (match(TokenKind::Pipe)) {
    unionType.members.push_back(parsePrimaryType());
  }
  return TypeExpr{.unionType = std::move(unionType)};
}

PrimaryType Parser::parsePrimaryType() {
  PrimaryType primary;
  if (check(TokenKind::OpenBrace)) {
    primary.kind = ShapeType{.pattern = parseDestructurePattern()};
  } else if (match(TokenKind::OpenParen)) {
    primary.kind = std::make_shared<TypeExpr>(parseTypeExpr());
    consume(TokenKind::CloseParen, "Expected ')' after parenthesised type");
  } else {
    const Token nameTok =
        consume(TokenKind::Identifier, "Expected a type name, '{' or '('");
    const std::string name(nameTok.text);
    if (match(TokenKind::LessThan)) {
      GenericType generic{.name = name};
      generic.args.push_back(std::make_shared<TypeExpr>(parseTypeExpr()));
      while (match(TokenKind::Comma)) {
        generic.args.push_back(std::make_shared<TypeExpr>(parseTypeExpr()));
      }
      consume(TokenKind::GreaterThan, "Expected '>' after type arguments");
      primary.kind = std::move(generic);
    } else {
      primary.kind = BasicType{.kind = basicTypeKind(name), .name = name};
    }
    // `Cell { +d.title: String }`: the named type, refined by a shape.
    if (check(TokenKind::OpenBrace)) {
      primary.refinement = ShapeType{.pattern = parseDestructurePattern()};
    }
  }
  primary.isOptional = match(TokenKind::Question);
  return primary;
}

LambdaParam Parser::parseLambdaParam() {
  bool isVariadic = match(TokenKind::DotDotDot);
  std::variant<std::string, bool, DestructurePattern> target;

  if (check(TokenKind::OpenBrace)) {
    target = parseDestructurePattern();
  } else if (match(TokenKind::Dot)) {
    target = true;
  } else if (check(TokenKind::Variable)) {
    Token vTok = advance();
    target     = vTok.stringValue;
    if (!isVariadic && match(TokenKind::DotDotDot)) {
      isVariadic = true;
    }
  } else if (check(TokenKind::Identifier)) {
    Token idTok = advance();
    target      = std::string(idTok.text);
    if (!isVariadic && match(TokenKind::DotDotDot)) {
      isVariadic = true;
    }
  } else {
    throw ParseError(
        "Expected parameter name ($var, identifier, '.', or '{ ... }')",
        current_.loc);
  }

  std::shared_ptr<TypeExpr> type = nullptr;
  if (!std::holds_alternative<DestructurePattern>(target) &&
      match(TokenKind::Colon)) {
    type = std::make_shared<TypeExpr>(parseTypeExpr());
  }

  std::shared_ptr<ValueExpr> defVal = nullptr;
  if (match(TokenKind::Assign)) {
    defVal = std::make_shared<ValueExpr>(parseValueExpr());
  }
  if (!isVariadic && match(TokenKind::DotDotDot)) {
    isVariadic = true;
  }

  return LambdaParam{
      .target       = std::move(target),
      .type         = std::move(type),
      .defaultValue = std::move(defVal),
      .isVariadic   = isVariadic,
  };
}

std::vector<LambdaParam> Parser::parseLambdaParams() {
  std::vector<LambdaParam> params;
  if (!check(TokenKind::CloseParen)) {
    while (true) {
      params.push_back(parseLambdaParam());
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  }
  return params;
}

bool Parser::isBlockBody() {
  if (!check(TokenKind::OpenBrace)) {
    return false;
  }
  Token next = peekToken();
  if (next.is(TokenKind::KwLet) || next.is(TokenKind::KwFor) ||
      next.is(TokenKind::KwIf) || next.is(TokenKind::KwReturn) ||
      next.is(TokenKind::KwWatch) || next.is(TokenKind::KwWith) ||
      next.is(TokenKind::KwWeave) || next.is(TokenKind::Semicolon) ||
      next.is(TokenKind::KwBreak)) {
    return true;
  }
  if (next.is(TokenKind::Identifier) && next.text == "effect") {
    // `{ effect { ...` can only be a script body holding an I/O action; it is
    // parsed as one so the placement refusal names VQL_IO_QUERY_CONTEXT
    // instead of failing as a shape literal.
    Lexer probe = lexer_;
    if (probe.nextToken().is(TokenKind::OpenBrace)) return true;
  }
  if (next.is(TokenKind::Identifier) && next.text == "match") {
    // `{ match $r {` likewise: a script `match`, refused by placement.
    Lexer probe = lexer_;
    if (probe.nextToken().is(TokenKind::Variable)) return true;
  }
  Lexer tempLexer = lexer_;
  Token cur       = next;
  int braceDepth  = 1;
  while (cur.kind != TokenKind::EndOfFile) {
    if (cur.kind == TokenKind::OpenBrace) {
      braceDepth++;
    } else if (cur.kind == TokenKind::CloseBrace) {
      braceDepth--;
      if (braceDepth == 0) {
        break;
      }
    } else if (braceDepth == 1) {
      if (cur.kind == TokenKind::Semicolon) {
        return true;
      }
      if (cur.kind == TokenKind::Comma) {
        return false;
      }
    }
    cur = tempLexer.nextToken();
  }
  return false;
}

PathLambda Parser::parsePathLambda() {
  consume(TokenKind::OpenParen, "Expected '(' after '\\'");
  std::vector<LambdaParam> params = parseLambdaParams();
  consume(TokenKind::CloseParen, "Expected ')' after lambda parameter list");

  if (check(TokenKind::OpenBrace) && isBlockBody()) {
    consume(TokenKind::OpenBrace, "Expected '{'");
    ScriptBody body = parseScriptBody();
    consume(TokenKind::CloseBrace, "Expected '}' after block body");
    return PathLambda{
        .params = std::move(params),
        .body   = std::make_shared<ScriptBody>(std::move(body)),
    };
  }
  ValueExpr expr = parseValueOrComparisonExpr();
  return PathLambda{
      .params = std::move(params),
      .body   = std::make_shared<ValueExpr>(std::move(expr)),
  };
}

ShapeLiteral Parser::parseShapeLiteral() {
  consume(TokenKind::OpenBrace, "Expected '{' for shape literal");
  std::vector<ShapeRule> rules;
  while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
    rules.push_back(parseShapeRule());
    if (!match(TokenKind::Comma)) {
      break;
    }
  }
  consume(TokenKind::CloseBrace, "Expected '}' after shape literal rules");
  return ShapeLiteral{.rules = std::move(rules)};
}

ShapeRule Parser::parseShapeRule() {
  if (match(TokenKind::Star)) {
    return ShapeRule{.rule = ShapeWildcard::Everything};
  }
  if (check(TokenKind::Bang) && peekToken().is(TokenKind::Star)) {
    advance(); // '!'
    advance(); // '*'
    return ShapeRule{.rule = ShapeWildcard::Nothing};
  }

  if (check(TokenKind::Variable)) {
    Token vTok = advance();
    return ShapeRule{.rule = vTok.stringValue};
  }

  if (check(TokenKind::OpenBrace)) {
    return ShapeRule{.rule =
                         std::make_shared<ShapeLiteral>(parseShapeLiteral())};
  }

  if ((check(TokenKind::Identifier) || check(TokenKind::KwFind)) &&
      peekToken().is(TokenKind::OpenParen)) {
    return ShapeRule{.rule = std::make_shared<FunctionInvocation>(
                         parseFunctionInvocation())};
  }

  if (check(TokenKind::StringLiteral)) {
    Token sTok                             = advance();
    std::string key                        = sTok.stringValue;
    std::shared_ptr<ValueExpr> targetValue = nullptr;
    if (match(TokenKind::Arrow) || match(TokenKind::Colon)) {
      targetValue = std::make_shared<ValueExpr>(parseValueOrComparisonExpr());
    }
    return ShapeRule{.rule = DimensionShapeRule{
                         .dimName     = std::move(key),
                         .targetValue = std::move(targetValue),
                     }};
  }

  bool negated                = match(TokenKind::Bang);
  bool isBidir                = match(TokenKind::Tilde);
  zigzag::DimVector direction = zigzag::DimVector::POS;
  if (match(TokenKind::Plus)) {
    direction = zigzag::DimVector::POS;
  } else if (match(TokenKind::Minus)) {
    direction = zigzag::DimVector::NEG;
  }

  Token dimTok =
      consume(TokenKind::Identifier, "Expected dimension in shape rule");
  std::string dimName = std::string(dimTok.text);

  if (match(TokenKind::DimPivot)) {
    Token toTok =
        consume(TokenKind::Identifier, "Expected target dimension in pivot");
    return ShapeRule{.rule =
                         DimensionPivotRule{.fromDim = std::move(dimName),
                                            .toDim = std::string(toTok.text)}};
  }

  if (match(TokenKind::Assign)) {
    ValueExpr expr = parseValueOrComparisonExpr();
    return ShapeRule{.rule = VirtualDimensionDefRule{
                         .dimName   = std::move(dimName),
                         .direction = direction,
                         .expr = std::make_shared<ValueExpr>(std::move(expr))}};
  }

  std::shared_ptr<ValueExpr> targetValue = nullptr;
  if (match(TokenKind::Colon) || match(TokenKind::Arrow)) {
    targetValue = std::make_shared<ValueExpr>(parseValueOrComparisonExpr());
  }

  return ShapeRule{.rule = DimensionShapeRule{
                       .negated     = negated,
                       .isBidir     = isBidir,
                       .direction   = direction,
                       .dimName     = std::move(dimName),
                       .targetValue = std::move(targetValue),
                   }};
}

VersionExpr Parser::parseVersionExpr() {
  VersionExpr expr;
  if (check(TokenKind::MicroversionLiteral) ||
      check(TokenKind::TypedVersionLiteral)) {
    Token tok       = advance();
    expr.op         = VersionExpr::Op::Literal;
    expr.versionStr = tok.stringValue;
  } else if (check(TokenKind::Variable)) {
    Token tok       = advance();
    expr.op         = VersionExpr::Op::Variable;
    expr.versionStr = tok.stringValue;
  } else if (check(TokenKind::IntegerLiteral)) {
    Token tok       = advance();
    expr.op         = VersionExpr::Op::Literal;
    expr.versionStr = std::to_string(tok.intValue);
  } else if (check(TokenKind::Identifier)) {
    Token tok       = advance();
    expr.op         = VersionExpr::Op::Literal;
    expr.versionStr = std::string(tok.text);
  } else if (match(TokenKind::At)) {
    if (check(TokenKind::IntegerLiteral)) {
      Token tok       = advance();
      expr.op         = VersionExpr::Op::Literal;
      expr.versionStr = std::to_string(tok.intValue);
    } else if (check(TokenKind::Identifier)) {
      Token tok       = advance();
      expr.op         = VersionExpr::Op::Literal;
      expr.versionStr = std::string(tok.text);
    } else {
      throw ParseError("Expected microversion id after '@'", current_.loc);
    }
  } else {
    throw ParseError("Expected version expression", current_.loc);
  }

  while (true) {
    if (match(TokenKind::DotDot)) {
      VersionExpr rangeExpr;
      rangeExpr.op    = VersionExpr::Op::Range;
      rangeExpr.left  = std::make_shared<VersionExpr>(std::move(expr));
      rangeExpr.right = std::make_shared<VersionExpr>(parseVersionExpr());
      return rangeExpr;
    }
    const bool isNext   = match(TokenKind::Plus);
    const bool isParent = !isNext && match(TokenKind::Minus);
    if (isNext || isParent) {
      Token offsetTok = consume(TokenKind::IntegerLiteral,
                                "Expected integer offset after version step");
      VersionExpr stepExpr;
      stepExpr.op   = isNext ? VersionExpr::Op::Next : VersionExpr::Op::Parent;
      stepExpr.left = std::make_shared<VersionExpr>(std::move(expr));
      stepExpr.stepOffset = offsetTok.intValue;
      expr                = std::move(stepExpr);
      continue;
    }
    if (match(TokenKind::Slash)) {
      Token bTok = advance();
      if (bTok.kind != TokenKind::StringLiteral &&
          bTok.kind != TokenKind::Identifier) {
        throw ParseError("Expected branch name after version '/'", bTok.loc);
      }
      VersionExpr branchExpr;
      branchExpr.op         = VersionExpr::Op::Branch;
      branchExpr.left       = std::make_shared<VersionExpr>(std::move(expr));
      branchExpr.branchName = bTok.kind == TokenKind::StringLiteral
                                  ? bTok.stringValue
                                  : std::string(bTok.text);
      expr                  = std::move(branchExpr);
      continue;
    }
    break;
  }

  return expr;
}

VersionPin Parser::parseVersionPin() {
  consume(TokenKind::At, "Expected '@' for version pin");
  if (check(TokenKind::MicroversionLiteral) ||
      check(TokenKind::TypedVersionLiteral)) {
    VersionExpr expr = parseVersionExpr();
    std::vector<VersionExpr> versions;
    versions.push_back(std::move(expr));
    return VersionPin{.versions = std::move(versions), .isRange = false};
  }

  consume(TokenKind::OpenParen, "Expected '(' after '@' for version pin list");
  std::vector<VersionExpr> versions;
  bool isRange = false;

  while (!check(TokenKind::CloseParen) && !check(TokenKind::EndOfFile)) {
    auto expr = parseVersionExpr();
    if (expr.op == VersionExpr::Op::Range && expr.left && expr.right) {
      isRange = true;
      versions.push_back(std::move(*expr.left));
      versions.push_back(std::move(*expr.right));
      break;
    }
    versions.push_back(std::move(expr));
    if (match(TokenKind::DotDot)) {
      isRange = true;
      versions.push_back(parseVersionExpr());
      break;
    }
    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  consume(TokenKind::CloseParen, "Expected ')' after version pin list");
  return VersionPin{.versions = std::move(versions), .isRange = isRange};
}

bool Parser::atIoEffect() {
  return check(TokenKind::Identifier) && current_.text == "effect" &&
         peekToken().is(TokenKind::OpenBrace);
}

bool Parser::checkWord(const std::string_view word) const {
  return check(TokenKind::Identifier) && current_.text == word;
}

void Parser::consumeWord(const std::string_view word) {
  if (!checkWord(word)) {
    throw ParseError(std::format("Expected '{}' in an I/O script action", word),
                     current_.loc);
  }
  advance();
}

Token Parser::consumeVariable(const std::string_view what) {
  if (!check(TokenKind::Variable)) {
    throw ParseError(std::format("Expected {} ($name)", what), current_.loc);
  }
  return advance();
}

IoOperand Parser::parseIoOperand() {
  if (check(TokenKind::IntegerLiteral)) {
    return IoOperand{std::in_place_index<0>, advance().intValue};
  }
  if (check(TokenKind::Variable)) {
    return IoOperand{std::in_place_index<1>, advance().stringValue};
  }
  throw ParseError("An I/O action offset and bound are an integer literal or "
                   "a $variable",
                   current_.loc);
}

void Parser::refuseIoCall(const Token &name) const {
  if (name.text == "submit" || name.text == "await" ||
      name.text == "read_bytes" || name.text == "write_bytes" ||
      name.text == "append_bytes" || name.text == "sync") {
    throw ParseError(
        std::format("{}: '{}' is an I/O script action name and cannot be "
                    "called in a query, row, filter or projection expression",
                    vortex::frontend_codes::vqlQueryContext, name.text),
        name.loc);
  }
}

IoEffectStatement Parser::parseIoEffectStatement() {
  consumeWord("effect");
  consume(TokenKind::OpenBrace, "Expected '{' after 'effect'");
  IoEffectStatement effect;
  const auto simple = [&](const IoEffectStatement::Kind kind,
                          const std::string_view word) {
    effect.kind = kind;
    consumeWord(word);
    consume(TokenKind::OpenParen, "Expected '(' after the action name");
  };
  if (checkWord("await") && peekToken().is(TokenKind::OpenParen)) {
    simple(IoEffectStatement::Kind::Await, "await");
    effect.handleVar = consumeVariable("a request handle").stringValue;
    consume(TokenKind::CloseParen, "Expected ')' after the handle");
  } else if (checkWord("cancel") && peekToken().is(TokenKind::OpenParen)) {
    simple(IoEffectStatement::Kind::Cancel, "cancel");
    effect.handleVar = consumeVariable("a request handle").stringValue;
    consume(TokenKind::CloseParen, "Expected ')' after the handle");
  } else if (checkWord("capture") && peekToken().is(TokenKind::OpenParen)) {
    // `capture` is contextual: it is a call name only inside `effect { ... }`,
    // so an ordinary function of that name still defines and calls.
    simple(IoEffectStatement::Kind::Capture, "capture");
    effect.captureVar = consumeVariable("an outcome variable").stringValue;
    if (match(TokenKind::Comma)) {
      effect.captureRanged = true;
      effect.offset        = parseIoOperand();
      consume(TokenKind::Comma,
              "Expected ',' between the offset and the length");
      effect.maxBytes = parseIoOperand();
    }
    consume(TokenKind::CloseParen, "Expected ')' after the capture arguments");
  } else if (checkWord("open") && peekToken().is(TokenKind::OpenParen)) {
    simple(IoEffectStatement::Kind::Open, "open");
    if (check(TokenKind::StringLiteral)) {
      effect.openPath = {.text = advance().stringValue, .isVariable = false};
    } else if (check(TokenKind::Variable)) {
      effect.openPath = {.text = advance().stringValue, .isVariable = true};
    } else {
      throw ParseError("An open action names its path as a string literal or "
                       "a $variable",
                       current_.loc);
    }
    consume(TokenKind::CloseParen, "Expected ')' after the path");
    effect.openMode = parseIoOpenMode();
  } else {
    parseIoReadAction(effect);
  }
  match(TokenKind::Semicolon);
  consume(TokenKind::CloseBrace, "Expected '}' to close 'effect'");
  if (match(TokenKind::KwAs)) {
    effect.resultVar = consumeVariable("an outcome variable").stringValue;
  }
  return effect;
}

IoOpenMode Parser::parseIoOpenMode() {
  if (!check(TokenKind::KwFor)) return IoOpenMode::Read;
  advance();
  const bool append = checkWord("append");
  if (!append && !checkWord("write")) {
    throw ParseError("An open action's mode is 'for write' or 'for append', "
                     "optionally followed by 'create'",
                     current_.loc);
  }
  advance();
  const bool create = checkWord("create");
  if (create) advance();
  if (append) return create ? IoOpenMode::AppendCreate : IoOpenMode::Append;
  return create ? IoOpenMode::WriteCreate : IoOpenMode::Write;
}

IoByteSource Parser::parseIoByteSource() {
  if (check(TokenKind::StringLiteral)) {
    return {.text = advance().stringValue, .isVariable = false};
  }
  if (check(TokenKind::Variable)) {
    return {.text = advance().stringValue, .isVariable = true};
  }
  throw ParseError("A write's bytes are a string literal or a $variable bound "
                   "to a text cell or an outcome",
                   current_.loc);
}

IoDuration Parser::parseIoDuration() {
  IoDuration duration;
  duration.amount = parseIoOperand();
  if (checkWord("ms")) {
    duration.unitMillis = 1;
  } else if (checkWord("s")) {
    duration.unitMillis = 1000;
  } else {
    throw ParseError("A 'within' bound is a count followed by 'ms' or 's'",
                     current_.loc);
  }
  advance();
  return duration;
}

void Parser::parseIoReadAction(IoEffectStatement &effect) {
  consume(TokenKind::KwLet, "Expected 'let' to bind the submission");
  effect.submissionVar = consumeVariable("a submission variable").stringValue;
  consume(TokenKind::Assign, "Expected ':=' after the submission variable");
  consumeWord("submit");
  consume(TokenKind::OpenParen, "Expected '(' after 'submit'");
  effect.capabilityVar = consumeVariable("a capability variable").stringValue;
  consume(TokenKind::Comma, "Expected ',' after the capability");
  if (checkWord("write_bytes")) {
    advance();
    effect.request = IoRequestKind::WriteBytes;
    consume(TokenKind::OpenParen, "Expected '(' after 'write_bytes'");
    effect.offset = parseIoOperand();
    consume(TokenKind::Comma, "Expected ',' between the offset and the bytes");
    effect.source = parseIoByteSource();
    consume(TokenKind::CloseParen, "Expected ')' after write_bytes arguments");
  } else if (checkWord("append_bytes")) {
    advance();
    effect.request = IoRequestKind::AppendBytes;
    consume(TokenKind::OpenParen, "Expected '(' after 'append_bytes'");
    effect.source = parseIoByteSource();
    consume(TokenKind::CloseParen, "Expected ')' after append_bytes argument");
  } else if (checkWord("sync")) {
    advance();
    effect.request = IoRequestKind::Sync;
    consume(TokenKind::OpenParen, "Expected '(' after 'sync'");
    consume(TokenKind::CloseParen, "sync() takes no arguments");
  } else {
    consumeWord("read_bytes");
    consume(TokenKind::OpenParen, "Expected '(' after 'read_bytes'");
    effect.offset = parseIoOperand();
    consume(TokenKind::Comma, "Expected ',' between the offset and the bound");
    effect.maxBytes = parseIoOperand();
    consume(TokenKind::CloseParen, "Expected ')' after read_bytes arguments");
  }
  consume(TokenKind::CloseParen, "Expected ')' after submit arguments");
  if (checkWord("within")) {
    advance();
    effect.within = parseIoDuration();
  }
  match(TokenKind::Semicolon);

  consumeWord("match");
  const auto matched = consumeVariable("the submission variable");
  if (matched.stringValue != effect.submissionVar) {
    throw ParseError("An I/O action matches the submission it just bound",
                     matched.loc);
  }
  consume(TokenKind::OpenBrace, "Expected '{' to open the match arms");
  bool sawAdmitted = false;
  bool sawRefused  = false;
  while (!check(TokenKind::CloseBrace)) {
    if (checkWord("admitted")) {
      if (sawAdmitted) {
        throw ParseError("Duplicate 'admitted' arm", current_.loc);
      }
      sawAdmitted = true;
      advance();
      consume(TokenKind::OpenParen, "Expected '(' after 'admitted'");
      effect.requestVar = consumeVariable("a request variable").stringValue;
      consume(TokenKind::CloseParen, "Expected ')' after the request variable");
      consume(TokenKind::Arrow, "Expected '=>' after the admitted pattern");
      if (check(TokenKind::Variable)) {
        // `admitted($q) => $q`: the read starts and the handle is the value.
        const auto kept = advance();
        if (kept.stringValue != effect.requestVar) {
          throw ParseError("The 'admitted' arm keeps the request it bound",
                           kept.loc);
        }
        effect.detached = true;
      } else {
        consumeWord("await");
        consume(TokenKind::OpenParen, "Expected '(' after 'await'");
        const auto awaited = consumeVariable("the admitted request");
        if (awaited.stringValue != effect.requestVar) {
          throw ParseError("'await' takes the request the arm bound",
                           awaited.loc);
        }
        consume(TokenKind::CloseParen, "Expected ')' after 'await' argument");
      }
    } else if (checkWord("refused")) {
      if (sawRefused) {
        throw ParseError("Duplicate 'refused' arm", current_.loc);
      }
      sawRefused = true;
      advance();
      consume(TokenKind::OpenParen, "Expected '(' after 'refused'");
      effect.refusalVar = consumeVariable("a refusal variable").stringValue;
      consume(TokenKind::CloseParen, "Expected ')' after the refusal variable");
      consume(TokenKind::Arrow, "Expected '=>' after the refused pattern");
      const auto kept = consumeVariable("the refusal");
      if (kept.stringValue != effect.refusalVar) {
        throw ParseError("The 'refused' arm keeps the refusal it bound",
                         kept.loc);
      }
    } else {
      throw ParseError("Expected an 'admitted' or 'refused' match arm",
                       current_.loc);
    }
    match(TokenKind::Comma);
  }
  if (!sawAdmitted || !sawRefused) {
    throw ParseError("An I/O action matches both 'admitted' and 'refused'; a "
                     "refusal is a value, not a failure",
                     current_.loc);
  }
  consume(TokenKind::CloseBrace, "Expected '}' to close the match");
}

bool Parser::atIoMatch() {
  return checkWord("match") && peekToken().is(TokenKind::Variable);
}

void Parser::noteIoName(const IoEffectStatement &effect) {
  if (!effect.resultVar) return;
  using Kind = IoSubject::Kind;
  const auto operationOf = [](const IoRequestKind request) {
    return request == IoRequestKind::ReadBytes ? IoOperation::Read
                                               : IoOperation::Write;
  };
  IoSubject subject;
  switch (effect.kind) {
  case IoEffectStatement::Kind::Read:
    subject = {.kind      = effect.detached ? Kind::Handle : Kind::Outcome,
               .operation = operationOf(effect.request)};
    break;
  case IoEffectStatement::Kind::Await:
  case IoEffectStatement::Kind::Cancel: {
    // The outcome of a handle is the outcome of the operation that made it.
    const auto held = ioNames_.find(effect.handleVar);
    subject = {.kind      = Kind::Outcome,
               .operation = held != ioNames_.end() ? held->second.operation
                                                   : IoOperation::Any};
    break;
  }
  case IoEffectStatement::Kind::Open:
    subject = {.kind = Kind::Capability};
    break;
  case IoEffectStatement::Kind::Capture:
    subject = {.kind = Kind::Receipt};
    break;
  }
  ioNames_.insert_or_assign(*effect.resultVar, subject);
}

IoMatchStatement Parser::parseIoMatchStatement() {
  const auto refuse = [](const std::string &message, const SourceLocation &at)
      -> ParseError {
    return ParseError(
        std::format("{}: {}", vortex::frontend_codes::vqlMatch, message), at);
  };
  consumeWord("match");
  const auto subjectToken = consumeVariable("the outcome to match");
  IoMatchStatement statement{.subject = subjectToken.stringValue};
  const auto known = ioNames_.find(statement.subject);
  IoOperation operation = IoOperation::Any;
  if (known != ioNames_.end()) {
    using Kind = IoSubject::Kind;
    switch (known->second.kind) {
    case Kind::Handle:
      throw refuse("$" + statement.subject +
                       " is a request handle, not an outcome; await it first "
                       "(effect { await($" +
                       statement.subject + ") } as $r) and match $r",
                   subjectToken.loc);
    case Kind::Capability:
      throw refuse("$" + statement.subject +
                       " names the capability an open minted (or the open's "
                       "outcome when it minted none); read the open's tag "
                       "with io_tag($" +
                       statement.subject + ")",
                   subjectToken.loc);
    case Kind::Receipt:
      throw refuse("$" + statement.subject +
                       " is a capture's receipt (or the outcome that ended "
                       "it); read it with io_tag($" +
                       statement.subject + ")",
                   subjectToken.loc);
    case Kind::Outcome:
      operation = known->second.operation;
      break;
    }
  }
  consume(TokenKind::OpenBrace, "Expected '{' to open the match arms");
  IoTagSet named        = 0;
  bool sawWildcard      = false;
  const IoTagSet possible = ioTagsOf(operation);
  while (!check(TokenKind::CloseBrace)) {
    const auto armLoc = current_.loc;
    if (sawWildcard) {
      throw refuse("an arm after '_' can never be reached", armLoc);
    }
    if (!check(TokenKind::Identifier)) {
      throw refuse("a match arm names an outcome tag or '_'", armLoc);
    }
    IoMatchArm arm;
    const std::string word(current_.text);
    advance();
    if (word == "_") {
      sawWildcard = true;
    } else {
      const auto tag = ioMatchTagOf(word);
      if (!tag) {
        throw refuse("'" + word + "' is not an outcome tag (the tags are " +
                         [] {
                           std::string names;
                           for (std::size_t i = 0; i < ioMatchTagCount; ++i) {
                             if (i != 0) names += ", ";
                             names += ioMatchTagName(
                                 static_cast<IoMatchTag>(i));
                           }
                           return names;
                         }() +
                         ")",
                     armLoc);
      }
      if ((ioTagBit(*tag) & possible) == 0) {
        throw refuse("'" + word + "' is not a tag " +
                         (operation == IoOperation::Read ? "a read"
                                                         : "a write") +
                         " can end with, so no arm for it can ever run",
                     armLoc);
      }
      if ((ioTagBit(*tag) & named) != 0) {
        throw refuse("the tag '" + word + "' is matched twice", armLoc);
      }
      named = static_cast<IoTagSet>(named | ioTagBit(*tag));
      arm.tag = tag;
    }
    if (match(TokenKind::OpenParen)) {
      while (!check(TokenKind::CloseParen)) {
        if (check(TokenKind::Variable)) {
          arm.binders.emplace_back(advance().stringValue);
        } else if (checkWord("_")) {
          advance();
          arm.binders.emplace_back(std::nullopt);
        } else {
          throw refuse("a pattern binds $names or '_'", current_.loc);
        }
        if (!match(TokenKind::Comma)) break;
      }
      consume(TokenKind::CloseParen, "Expected ')' after the pattern");
    }
    const std::size_t arity = arm.tag ? ioMatchArity(*arm.tag) : 0;
    if (arm.binders.size() != arity) {
      throw refuse(
          arm.tag ? "'" + word + "' binds " + std::to_string(arity) +
                        " field(s) and the pattern names " +
                        std::to_string(arm.binders.size())
                  : std::string{"'_' binds nothing"},
          armLoc);
    }
    consume(TokenKind::Arrow, "Expected '=>' after the pattern");
    arm.body = parseReturnItem();
    statement.arms.push_back(std::move(arm));
    match(TokenKind::Comma);
  }
  const auto closeLoc = current_.loc;
  consume(TokenKind::CloseBrace, "Expected '}' to close the match");
  if (!sawWildcard && (possible & ~named) != 0) {
    std::string missing;
    for (std::size_t i = 0; i < ioMatchTagCount; ++i) {
      const auto tag = static_cast<IoMatchTag>(i);
      if ((ioTagBit(tag) & possible & ~named) == 0) continue;
      if (!missing.empty()) missing += ", ";
      missing += ioMatchTagName(tag);
    }
    throw refuse("this match does not name " + missing +
                     "; name each or add a '_' arm",
                 closeLoc);
  }
  return statement;
}

} // namespace xanadu::vql
