/**
 * @file parser.cpp
 * @brief Recursive-descent parser implementation for VQL v13.0.
 */
#include "common/xanadu/vql/parser.hpp"

#include <format>
#include <utility>

namespace xanadu::vql {

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
  while (check(TokenKind::KwDef) || check(TokenKind::KwMount)) {
    if (check(TokenKind::KwDef)) {
      defs.push_back(parseDefDeclaration());
      match(TokenKind::Semicolon);
    } else {
      mounts.push_back(parseMountDeclaration());
      match(TokenKind::Semicolon);
    }
  }

  ScriptBody body = parseScriptBody();

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
      .defs   = std::move(defs),
      .mounts = std::move(mounts),
      .body   = std::move(body),
      .expr   = std::move(expr),
  };
}

ScriptBody Parser::parseScriptBody() {
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
  if (check(TokenKind::StringLiteral)) {
    Token sTok = advance();
    name       = sTok.stringValue;
  } else if (check(TokenKind::Identifier)) {
    Token idTok = advance();
    name        = std::string(idTok.text);
  } else if (check(TokenKind::Variable)) {
    Token vTok = advance();
    name       = vTok.stringValue;
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
      .perspectiveName = std::move(name),
      .body            = std::make_shared<ScriptBody>(std::move(body)),
  };
}

StoreAnchor Parser::parseStoreAnchor() {
  StoreAnchor anchor;
  if (match(TokenKind::Home)) {
    anchor.method = "";
  } else if (check(TokenKind::NamedStore)) {
    Token tok     = advance();
    anchor.method = tok.stringValue;
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

DefDeclaration Parser::parseDefDeclaration() {
  consume(TokenKind::KwDef, "Expected 'def'");
  Token nameTok =
      consume(TokenKind::Identifier, "Expected function name after 'def'");
  consume(TokenKind::OpenParen, "Expected '(' after function name");
  std::vector<std::string> params;
  if (!check(TokenKind::CloseParen)) {
    while (true) {
      if (check(TokenKind::Variable)) {
        Token pTok = advance();
        params.push_back(pTok.stringValue);
      } else if (check(TokenKind::Identifier)) {
        Token pTok = advance();
        params.push_back(std::string(pTok.text));
      } else if (match(TokenKind::Dot)) {
        params.push_back(".");
      }
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  }
  consume(TokenKind::CloseParen, "Expected ')' after parameters in 'def'");
  consume(TokenKind::Assign, "Expected ':=' after parameter list in 'def'");
  ValueExpr body = parseValueExpr();
  return DefDeclaration{.name   = std::string(nameTok.text),
                        .params = std::move(params),
                        .body   = std::move(body)};
}

PathExpression Parser::parsePathExpression(bool allowCloneTail) {
  bool implicitContext = false;
  if (check(TokenKind::Identifier) || check(TokenKind::Plus) ||
      check(TokenKind::Minus) || check(TokenKind::OpenParen) ||
      check(TokenKind::Tilde) ||
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

      FunctionInvocation fn{.name = "value", .args = std::move(args)};
      PathStep step{.selector = std::move(fn)};
      steps.push_back(std::move(step));
      continue;
    }

    if (check(TokenKind::Arrow) || check(TokenKind::Transclude)) {
      PathStep step = parsePathStep(false /*requireSlash*/);
      if (std::holds_alternative<PathMorphism>(step.selector) &&
          !steps.empty() &&
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
      continue;
    }

    if (check(TokenKind::Slash)) {
      steps.push_back(parsePathStep(true /*requireSlash*/));
    } else {
      break;
    }
  }

  std::optional<ShapeLiteral> shapeProjection;
  if (check(TokenKind::OpenBrace)) {
    Token nextTok = peekToken();
    if (!nextTok.is(TokenKind::KwWeave) && !nextTok.is(TokenKind::KwFor) &&
        !nextTok.is(TokenKind::KwLet) && !nextTok.is(TokenKind::KwIf) &&
        !nextTok.is(TokenKind::KwReturn) && !nextTok.is(TokenKind::KwWatch)) {
      shapeProjection = parseShapeLiteral();
    }
  }

  std::optional<CloneTail> cloneTail;
  if (allowCloneTail && check(TokenKind::CloneJoin)) {
    cloneTail = parseCloneTail();
  }

  return PathExpression{.anchor           = std::move(anchor),
                        .anchorPredicates = std::move(anchorPredicates),
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
      check(TokenKind::KwFind) || check(TokenKind::OpenBrace)) {
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

  ActionClause action = parseActionClause();

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
    if (check(TokenKind::Slash)) {
      // FieldWeave: sequence of PathSteps rooted at result cell
      std::vector<PathStep> steps;
      while (check(TokenKind::Slash)) {
        steps.push_back(parsePathStep());
      }
      items.push_back(
          ReturnItem{.item = FieldWeave{.steps = std::move(steps)}});
    } else {
      items.push_back(ReturnItem{.item = parsePathExpression()});
    }

    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  return ReturnClause{.items = std::move(items)};
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
    if (peekToken().is(TokenKind::Slash) || peekToken().is(TokenKind::Dot) ||
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
  consume(TokenKind::OpenParen, "Expected '(' after function name");
  std::vector<ValueExpr> args;

  if (!check(TokenKind::CloseParen)) {
    while (true) {
      args.push_back(parseValueExpr());
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  }

  consume(TokenKind::CloseParen, "Expected ')' after function arguments");
  return FunctionInvocation{.name = std::string(nameTok.text),
                            .args = std::move(args)};
}

DestructurePattern Parser::parseDestructurePattern() {
  consume(TokenKind::OpenBrace, "Expected '{' for destructuring pattern");
  std::vector<DestructureField> fields;
  while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Dot)) {
      fields.push_back(DestructureField{.isContext = true});
    } else {
      bool isBidir                = match(TokenKind::Tilde);
      zigzag::DimVector direction = zigzag::DimVector::POS;
      if (match(TokenKind::Plus)) {
        direction = zigzag::DimVector::POS;
      } else if (match(TokenKind::Minus)) {
        direction = zigzag::DimVector::NEG;
      }
      Token dimTok = consume(TokenKind::Identifier,
                             "Expected dimension in destructuring field");
      consume(TokenKind::Colon,
              "Expected ':' after dimension in destructure field");
      Token varTok = consume(TokenKind::Variable,
                             "Expected $variable in destructure field");
      fields.push_back(DestructureField{
          .isContext = false,
          .dimName   = std::string(dimTok.text),
          .direction = direction,
          .isBidir   = isBidir,
          .varName   = varTok.stringValue,
      });
    }
    if (!match(TokenKind::Comma)) {
      break;
    }
  }
  consume(TokenKind::CloseBrace, "Expected '}' after destructuring pattern");
  return DestructurePattern{.fields = std::move(fields)};
}

PathLambda Parser::parsePathLambda() {
  consume(TokenKind::OpenParen, "Expected '(' after '\\'");
  std::vector<std::string> params;
  while (!check(TokenKind::CloseParen) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Dot)) {
      params.push_back(".");
    } else if (check(TokenKind::Variable)) {
      Token vTok = advance();
      params.push_back(vTok.stringValue);
    } else if (check(TokenKind::Identifier)) {
      Token pTok = advance();
      params.push_back(std::string(pTok.text));
    }
    if (!match(TokenKind::Comma)) {
      break;
    }
  }
  consume(TokenKind::CloseParen, "Expected ')' after lambda parameter list");
  PathExpression path = parsePathExpression();
  return PathLambda{.params = std::move(params),
                    .path = std::make_shared<PathExpression>(std::move(path))};
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

  if (match(TokenKind::Plus)) {
    Token offsetTok =
        consume(TokenKind::IntegerLiteral, "Expected integer offset after '+'");
    VersionExpr nextExpr;
    nextExpr.op         = VersionExpr::Op::Next;
    nextExpr.left       = std::make_shared<VersionExpr>(std::move(expr));
    nextExpr.stepOffset = offsetTok.intValue;
    return nextExpr;
  }
  if (match(TokenKind::Minus)) {
    Token offsetTok =
        consume(TokenKind::IntegerLiteral, "Expected integer offset after '-'");
    VersionExpr parentExpr;
    parentExpr.op         = VersionExpr::Op::Parent;
    parentExpr.left       = std::make_shared<VersionExpr>(std::move(expr));
    parentExpr.stepOffset = offsetTok.intValue;
    return parentExpr;
  }
  if (match(TokenKind::Slash)) {
    Token bTok = advance();
    VersionExpr branchExpr;
    branchExpr.op         = VersionExpr::Op::Branch;
    branchExpr.left       = std::make_shared<VersionExpr>(std::move(expr));
    branchExpr.branchName = (bTok.kind == TokenKind::StringLiteral)
                                ? bTok.stringValue
                                : std::string(bTok.text);
    return branchExpr;
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
    versions.push_back(parseVersionExpr());
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

} // namespace xanadu::vql
