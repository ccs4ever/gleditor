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

Parser::Parser(Lexer lexer) : lexer_(lexer) { current_ = lexer_.nextToken(); }

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

bool Parser::check(TokenKind kind) { return current_.kind == kind; }

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
  if (check(TokenKind::KwFor) || check(TokenKind::KwLet) ||
      check(TokenKind::KwWeave) || check(TokenKind::KwIf) ||
      check(TokenKind::KwReturn)) {
    return QueryExpression{.expr = parseExecutionBlock()};
  }
  return QueryExpression{.expr = parsePathExpression()};
}

PathExpression Parser::parsePathExpression(bool allowCloneTail) {
  bool implicitContext = false;
  if (check(TokenKind::Identifier) || check(TokenKind::Plus) ||
      check(TokenKind::Minus) || check(TokenKind::OpenParen)) {
    implicitContext = true;
  }

  AnchorNode anchor = parseAnchorNode();
  std::vector<PathStep> steps;

  if (implicitContext) {
    steps.push_back(parsePathStep(false /*requireSlash*/));
  }

  while (check(TokenKind::Slash) || check(TokenKind::Dot)) {
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

    if (check(TokenKind::Slash)) {
      steps.push_back(parsePathStep(true /*requireSlash*/));
    } else {
      break;
    }
  }

  std::optional<CloneTail> cloneTail;
  if (allowCloneTail && check(TokenKind::CloneJoin)) {
    cloneTail = parseCloneTail();
  }

  return PathExpression{.anchor    = std::move(anchor),
                        .steps     = std::move(steps),
                        .cloneTail = std::move(cloneTail)};
}

AnchorNode Parser::parseAnchorNode() {
  if (match(TokenKind::Home)) {
    AnchorNode node{.kind = AnchorKind::Home};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::NamedStore)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::NamedStore, .name = tok.stringValue};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Root)) {
    AnchorNode node{.kind = AnchorKind::Root};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Cursor)) {
    AnchorNode node{.kind = AnchorKind::Cursor};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::NamedCursor)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::NamedCursor, .name = tok.stringValue};
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (check(TokenKind::Variable)) {
    Token tok = advance();
    AnchorNode node{.kind = AnchorKind::Variable, .name = tok.stringValue};
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
    if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
      advance();
      node.derefMaster = true;
    }
    return node;
  }

  if (match(TokenKind::Dot)) {
    AnchorNode node{.kind = AnchorKind::Context};
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

  // Inside predicate clauses, anchor can default to context '.'
  if (check(TokenKind::Slash) || check(TokenKind::Identifier) ||
      check(TokenKind::Plus) || check(TokenKind::Minus) ||
      check(TokenKind::OpenParen)) {
    return AnchorNode{.kind = AnchorKind::Context};
  }

  throw ParseError(
      "Expected anchor node (##, ##NAME, #, ^, ^NAME, $var, literal id, or .)",
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
  std::optional<RangeClamp> clamp;

  while (check(TokenKind::OpenBracket)) {
    Token next   = peekToken();
    bool isClamp = false;
    if (next.is(TokenKind::IntegerLiteral)) {
      isClamp = true;
    } else if ((next.is(TokenKind::Minus) || next.is(TokenKind::Plus))) {
      isClamp = true;
    }

    if (isClamp) {
      clamp = parseRangeClamp();
    } else {
      consume(TokenKind::OpenBracket, "Expected '['");
      predicates.push_back(parseBooleanExpr());
      consume(TokenKind::CloseBracket, "Expected ']' after predicate");
    }
  }

  if (check(TokenKind::GreaterThan) && !isNextTokenComparisonOperand()) {
    advance();
    derefMaster = true;
  }

  YieldMode yieldMode = parseYield();

  return PathStep{
      .selector    = std::move(selector),
      .derefMaster = derefMaster,
      .predicates  = std::move(predicates),
      .rangeClamp  = clamp,
      .yieldMode   = yieldMode,
  };
}

StepSelector Parser::parseStepSelector() {
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

  // Function invocation: func(args...)
  if (check(TokenKind::Identifier) && peekToken().is(TokenKind::OpenParen)) {
    return parseFunctionInvocation();
  }

  // SignedDimension
  int direction = +1;
  if (match(TokenKind::Plus)) {
    direction = +1;
  } else if (match(TokenKind::Minus)) {
    direction = -1;
  }

  Token dimTok =
      consume(TokenKind::Identifier, "Expected dimension identifier");
  std::string dimName = std::string(dimTok.text);

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
                       dimTok.loc);
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

  while (check(TokenKind::KwFor) || check(TokenKind::KwLet)) {
    if (check(TokenKind::KwFor)) {
      bindings.push_back(parseForClause());
    } else {
      bindings.push_back(parseLetClause());
    }
  }

  if (bindings.empty() && !check(TokenKind::KwWeave) &&
      !check(TokenKind::KwIf) && !check(TokenKind::KwReturn)) {
    throw ParseError("Execution block must have at least one 'for' or 'let' "
                     "clause, or an action clause",
                     current_.loc);
  }

  std::optional<WhereClause> where;
  if (check(TokenKind::KwWhere)) {
    where = parseWhereClause();
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
  Token varTok = consume(TokenKind::Variable, "Expected $variable after 'for'");
  consume(TokenKind::KwIn, "Expected 'in' after variable");
  PathExpression path = parsePathExpression();
  return ForClause{.varName = varTok.stringValue, .inPath = std::move(path)};
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

  if (match(TokenKind::OpenBrace)) {
    while (!check(TokenKind::CloseBrace) && !check(TokenKind::EndOfFile)) {
      if (check(TokenKind::KwLet)) {
        items.push_back(EffectItem{.item = parseLetClause()});
      } else if (check(TokenKind::KwFor)) {
        ForClause forC   = parseForClause();
        EffectClause eff = parseEffectClause();
        items.push_back(
            EffectItem{.item = std::make_pair(
                           std::move(forC),
                           std::make_shared<EffectClause>(std::move(eff)))});
      } else {
        items.push_back(EffectItem{.item = parsePathExpression()});
      }

      match(TokenKind::Comma);
    }
    consume(TokenKind::CloseBrace, "Expected '}' after weave block");
  } else {
    if (check(TokenKind::KwLet)) {
      items.push_back(EffectItem{.item = parseLetClause()});
    } else if (check(TokenKind::KwFor)) {
      ForClause forC   = parseForClause();
      EffectClause eff = parseEffectClause();
      items.push_back(EffectItem{
          .item =
              std::make_pair(std::move(forC),
                             std::make_shared<EffectClause>(std::move(eff)))});
    } else {
      items.push_back(EffectItem{.item = parsePathExpression()});
    }
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
  if (match(TokenKind::Question)) {
    ValueExpr expr = parseValueExpr();
    return PredicateTest{.kind = std::make_shared<ValueExpr>(std::move(expr))};
  }

  if (match(TokenKind::Dot)) {
    return PredicateTest{.kind = true /*ContextDot*/};
  }

  if (check(TokenKind::Identifier) && peekToken().is(TokenKind::OpenParen)) {
    return PredicateTest{.kind = std::make_shared<FunctionInvocation>(
                             parseFunctionInvocation())};
  }

  PathExpression path = parsePathExpression();
  return PredicateTest{.kind =
                           std::make_shared<PathExpression>(std::move(path))};
}

ValueExpr Parser::parseValueExpr() {
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
  if (check(TokenKind::Identifier) && peekToken().is(TokenKind::OpenParen)) {
    return ValueExpr{.kind = std::make_shared<FunctionInvocation>(
                         parseFunctionInvocation())};
  }

  // Fallback: PathExpression
  return ValueExpr{.kind =
                       std::make_shared<PathExpression>(parsePathExpression())};
}

FunctionInvocation Parser::parseFunctionInvocation() {
  Token nameTok = consume(TokenKind::Identifier, "Expected function name");
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

} // namespace xanadu::vql
