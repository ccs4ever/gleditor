/**
 * @file vprolog_parser_test.cpp
 * @brief Unit tests for Prolog Stage 0 Core Parser & Tokenizer.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vprolog/lexer.hpp"
#include "common/xanadu/vprolog/parser.hpp"

namespace {

using namespace xanadu::vprolog;

TEST(VPrologParserTest, LexerAtomsAndVariables) {
  Lexer lexer("cat 'hello world' X _ _Var apple_123");
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 6U);
  EXPECT_EQ(tokens[0].kind, TokenKind::Atom);
  EXPECT_EQ(tokens[0].stringValue, "cat");

  EXPECT_EQ(tokens[1].kind, TokenKind::Atom);
  EXPECT_EQ(tokens[1].stringValue, "hello world");

  EXPECT_EQ(tokens[2].kind, TokenKind::Variable);
  EXPECT_EQ(tokens[2].stringValue, "X");

  EXPECT_EQ(tokens[3].kind, TokenKind::Variable);
  EXPECT_EQ(tokens[3].stringValue, "_");

  EXPECT_EQ(tokens[4].kind, TokenKind::Variable);
  EXPECT_EQ(tokens[4].stringValue, "_Var");

  EXPECT_EQ(tokens[5].kind, TokenKind::Atom);
  EXPECT_EQ(tokens[5].stringValue, "apple_123");
}

TEST(VPrologParserTest, LexerNumbersAndStrings) {
  Lexer lexer("42 -100 3.14159 \"quoted string\"");
  auto tokens = lexer.tokenizeAll();

  ASSERT_GE(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(tokens[0].intValue, 42);

  EXPECT_EQ(tokens[1].kind, TokenKind::Minus);
  EXPECT_EQ(tokens[2].kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(tokens[2].intValue, 100);

  EXPECT_EQ(tokens[3].kind, TokenKind::FloatLiteral);
  EXPECT_NEAR(tokens[3].floatValue, 3.14159, 1e-5);

  EXPECT_EQ(tokens[4].kind, TokenKind::StringLiteral);
  EXPECT_EQ(tokens[4].stringValue, "quoted string");
}

TEST(VPrologParserTest, LexerComments) {
  std::string src = R"(
    % This is a line comment
    foo(bar). /* This is a
                 multiline comment */
    baz.
  )";
  Lexer lexer(src);
  auto tokens = lexer.tokenizeAll();

  std::vector<TokenKind> kinds;
  for (const auto &t : tokens) {
    if (t.kind != TokenKind::EndOfFile) {
      kinds.push_back(t.kind);
    }
  }

  // foo ( bar ) . baz .
  std::vector<TokenKind> expected = {TokenKind::Atom, TokenKind::OpenParen,
                                     TokenKind::Atom, TokenKind::CloseParen,
                                     TokenKind::Dot,  TokenKind::Atom,
                                     TokenKind::Dot};
  EXPECT_EQ(kinds, expected);
}

TEST(VPrologParserTest, ParseFactsAndSimpleRules) {
  std::string src = R"(
    parent(bob, alice).
    parent(alice, charlie).
    grandparent(X, Z) :- parent(X, Y), parent(Y, Z).
  )";
  Parser parser(src);
  Program prog = parser.parseProgram();

  ASSERT_EQ(prog.clauses.size(), 3U);

  // Fact 1: parent(bob, alice).
  EXPECT_TRUE(prog.clauses[0].isFact());
  const auto *h0 = prog.clauses[0].head.asCompound();
  ASSERT_NE(h0, nullptr);
  EXPECT_EQ(h0->functor, "parent");
  ASSERT_EQ(h0->args.size(), 2U);
  EXPECT_EQ(h0->args[0].asAtom()->name, "bob");
  EXPECT_EQ(h0->args[1].asAtom()->name, "alice");

  // Rule: grandparent(X, Z) :- parent(X, Y), parent(Y, Z).
  EXPECT_TRUE(prog.clauses[2].isRule());
  const auto *h2 = prog.clauses[2].head.asCompound();
  ASSERT_NE(h2, nullptr);
  EXPECT_EQ(h2->functor, "grandparent");
  ASSERT_EQ(prog.clauses[2].body.size(), 2U);

  const auto *b0 = prog.clauses[2].body[0].asCompound();
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->functor, "parent");
  EXPECT_EQ(b0->args[0].asVar()->name, "X");
  EXPECT_EQ(b0->args[1].asVar()->name, "Y");

  const auto *b1 = prog.clauses[2].body[1].asCompound();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->functor, "parent");
  EXPECT_EQ(b1->args[0].asVar()->name, "Y");
  EXPECT_EQ(b1->args[1].asVar()->name, "Z");
}

TEST(VPrologParserTest, ParseListsAndAppendRules) {
  std::string src = R"(
    append([], L, L).
    append([H|T], L, [H|R]) :- append(T, L, R).
  )";
  Parser parser(src);
  Program prog = parser.parseProgram();

  ASSERT_EQ(prog.clauses.size(), 2U);

  // Fact: append([], L, L).
  const auto *h0 = prog.clauses[0].head.asCompound();
  ASSERT_NE(h0, nullptr);
  EXPECT_EQ(h0->functor, "append");
  ASSERT_EQ(h0->args.size(), 3U);
  EXPECT_TRUE(h0->args[0].isAtom());
  EXPECT_EQ(h0->args[0].asAtom()->name, "[]");
  EXPECT_EQ(h0->args[1].asVar()->name, "L");
  EXPECT_EQ(h0->args[2].asVar()->name, "L");

  // Rule: append([H|T], L, [H|R]) :- append(T, L, R).
  const auto *h1 = prog.clauses[1].head.asCompound();
  ASSERT_NE(h1, nullptr);
  EXPECT_EQ(h1->functor, "append");
  ASSERT_EQ(h1->args.size(), 3U);

  const auto *list1 = h1->args[0].asList();
  ASSERT_NE(list1, nullptr);
  ASSERT_EQ(list1->elements.size(), 1U);
  EXPECT_EQ(list1->elements[0].asVar()->name, "H");
  ASSERT_NE(list1->tail, nullptr);
  EXPECT_EQ(list1->tail->asVar()->name, "T");

  const auto *list3 = h1->args[2].asList();
  ASSERT_NE(list3, nullptr);
  ASSERT_EQ(list3->elements.size(), 1U);
  EXPECT_EQ(list3->elements[0].asVar()->name, "H");
  ASSERT_NE(list3->tail, nullptr);
  EXPECT_EQ(list3->tail->asVar()->name, "R");

  ASSERT_EQ(prog.clauses[1].body.size(), 1U);
  const auto *b0 = prog.clauses[1].body[0].asCompound();
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->functor, "append");
  EXPECT_EQ(b0->args[0].asVar()->name, "T");
  EXPECT_EQ(b0->args[1].asVar()->name, "L");
  EXPECT_EQ(b0->args[2].asVar()->name, "R");
}

TEST(VPrologParserTest, ParseQueriesAndDirectives) {
  std::string src = R"(
    ?- append(X, [c], [a, b, c]).
    :- dynamic(foo/2).
  )";
  Parser parser(src);

  Clause q = parser.parseClause();
  EXPECT_TRUE(q.isQuery);
  ASSERT_EQ(q.body.size(), 1U);
  const auto *qGoal = q.body[0].asCompound();
  ASSERT_NE(qGoal, nullptr);
  EXPECT_EQ(qGoal->functor, "append");

  Clause d = parser.parseClause();
  EXPECT_TRUE(d.isDirective);
  ASSERT_EQ(d.body.size(), 1U);
  const auto *dGoal = d.body[0].asCompound();
  ASSERT_NE(dGoal, nullptr);
  EXPECT_EQ(dGoal->functor, "dynamic");
  ASSERT_EQ(dGoal->args.size(), 1U);
  const auto *slashTerm = dGoal->args[0].asCompound();
  ASSERT_NE(slashTerm, nullptr);
  EXPECT_EQ(slashTerm->functor, "/");
}

TEST(VPrologParserTest, OperatorPrecedenceAndArithmetic) {
  std::string src = "X is 1 + 2 * 3.";
  Parser parser(src);
  Clause c = parser.parseClause();

  const auto *h = c.head.asCompound();
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->functor, "is");
  EXPECT_EQ(h->args[0].asVar()->name, "X");

  // Expect '+' ( 1, '*' ( 2, 3 ) ) because '*' has tighter precedence than '+'
  const auto *arith = h->args[1].asCompound();
  ASSERT_NE(arith, nullptr);
  EXPECT_EQ(arith->functor, "+");
  EXPECT_EQ(arith->args[0].asNumber()->asInt(), 1);

  const auto *mul = arith->args[1].asCompound();
  ASSERT_NE(mul, nullptr);
  EXPECT_EQ(mul->functor, "*");
  EXPECT_EQ(mul->args[0].asNumber()->asInt(), 2);
  EXPECT_EQ(mul->args[1].asNumber()->asInt(), 3);
}

TEST(VPrologParserTest, CutAndFormattingRoundtrip) {
  std::string src = "max(X, Y, X) :- X >= Y, !.";
  Parser parser(src);
  Clause c = parser.parseClause();

  ASSERT_EQ(c.body.size(), 2U);
  EXPECT_EQ(c.body[1].asAtom()->name, "!");

  std::string formatted = formatClause(c);
  EXPECT_EQ(formatted, "max(X, Y, X) :- X >= Y, !.");
}

} // namespace
