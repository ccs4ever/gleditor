/**
 * @file vlace_records_test.cpp
 * @brief Stage 6 record tests: field access, construction, and functional
 *        update in the vlace evaluator and type checker (§5.7).
 *
 * The parser does not yet accept the `{ f = e }` surface syntax, so these
 * tests build the record AST (ERecordUpdate over an ECtor for construction,
 * over an EVar for update; EApp of a bare field name for access) directly and
 * drive the Compiler and TypeChecker over it -- exactly the nodes the parser
 * will emit once its aexpr record form lands.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/types.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

// -- small AST builders ------------------------------------------------------

template <typename N> Expr mkExpr(N n) {
  auto e  = std::make_unique<ExprNode>();
  e->node = std::move(n);
  return e;
}

Type mkTCon(std::string name) {
  auto t  = std::make_unique<TypeNode>();
  t->node = TCon{.name = std::move(name), .args = {}};
  return t;
}

Expr mkInt(std::int64_t i) {
  return mkExpr(
      ELiteral{.lit = Literal{.kind = LiteralKind::Int, .intValue = i}});
}
Expr mkVar(std::string name) { return mkExpr(EVar{.name = std::move(name)}); }
Expr mkCtor(std::string name) { return mkExpr(ECtor{.name = std::move(name)}); }
Expr mkApp(Expr fn, Expr arg) {
  return mkExpr(EApp{.fn = std::move(fn), .arg = std::move(arg)});
}
Expr mkBinOp(std::string op, Expr l, Expr r) {
  return mkExpr(
      EBinOp{.op = std::move(op), .lhs = std::move(l), .rhs = std::move(r)});
}

// A `data Point = Point { x :: Int, y :: Int }` declaration.
DataDecl pointDecl() {
  CtorDef c;
  c.name     = "Point";
  c.isRecord = true;
  c.fields.emplace_back("x", mkTCon("Int"));
  c.fields.emplace_back("y", mkTCon("Int"));
  DataDecl dd;
  dd.name = "Point";
  dd.ctors.push_back(std::move(c));
  return dd;
}

// Build an ERecordUpdate expression: `record { f1 = e1, ... }`.
Expr mkRecord(Expr record, std::vector<std::pair<std::string, Expr>> fields) {
  return mkExpr(
      ERecordUpdate{.record = std::move(record), .fields = std::move(fields)});
}

// Evaluate a program and deeply render its result (or "ERR:<Code>:<detail>").
std::string eval(Program &&prog) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  auto v = compiler.evaluateProgram(prog);
  if (!v) {
    return "ERR:" + std::string(compileErrorName(v.error().code)) + ":" +
           v.error().detail;
  }
  return compiler.render(*v);
}

// Type-check a program and return the rendered type (or "ERR:<Code>:<detail>").
std::string typecheck(Program &&prog) {
  TypeChecker tc;
  auto t = tc.checkProgram(prog);
  if (!t) {
    return "ERR:" + std::string(compileErrorName(t.error().code)) + ":" +
           t.error().detail;
  }
  return *t;
}

// Program with the Point data decl and a trailing result expression.
Program pointProgram(Expr result) {
  Program prog;
  prog.decls.push_back(Decl{pointDecl()});
  prog.result = std::move(result);
  return prog;
}

// -- evaluator ---------------------------------------------------------------

TEST(VlaceRecordsTest, Construction) {
  // Point { x = 1, y = 2 }
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("x", mkInt(1));
  fields.emplace_back("y", mkInt(2));
  auto prog = pointProgram(mkRecord(mkCtor("Point"), std::move(fields)));
  EXPECT_EQ(eval(std::move(prog)), "Point 1 2");
}

TEST(VlaceRecordsTest, ConstructionOutOfOrder) {
  // Point { y = 2, x = 1 } places by name, so x is still arg 0.
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("y", mkInt(2));
  fields.emplace_back("x", mkInt(1));
  auto prog = pointProgram(mkRecord(mkCtor("Point"), std::move(fields)));
  EXPECT_EQ(eval(std::move(prog)), "Point 1 2");
}

TEST(VlaceRecordsTest, FieldAccess) {
  // x (Point { x = 1, y = 2 })  ==>  1
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("x", mkInt(1));
  fields.emplace_back("y", mkInt(2));
  auto rec  = mkRecord(mkCtor("Point"), std::move(fields));
  auto prog = pointProgram(mkApp(mkVar("x"), std::move(rec)));
  EXPECT_EQ(eval(std::move(prog)), "1");
}

TEST(VlaceRecordsTest, FieldAccessSecond) {
  // y (Point { x = 1, y = 2 })  ==>  2
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("x", mkInt(1));
  fields.emplace_back("y", mkInt(2));
  auto rec  = mkRecord(mkCtor("Point"), std::move(fields));
  auto prog = pointProgram(mkApp(mkVar("y"), std::move(rec)));
  EXPECT_EQ(eval(std::move(prog)), "2");
}

TEST(VlaceRecordsTest, FunctionalUpdateLeavesOtherField) {
  // let p = Point { x = 1, y = 2 } in p { x = 5 }  ==>  Point 5 2
  std::vector<std::pair<std::string, Expr>> ctorFields;
  ctorFields.emplace_back("x", mkInt(1));
  ctorFields.emplace_back("y", mkInt(2));
  auto p = mkRecord(mkCtor("Point"), std::move(ctorFields));

  std::vector<std::pair<std::string, Expr>> upd;
  upd.emplace_back("x", mkInt(5));
  auto updated = mkRecord(mkVar("p"), std::move(upd));

  // let p = <ctor> in <updated>
  Binding b;
  b.pattern       = std::make_unique<PatternNode>();
  b.pattern->node = PVar{.name = "p"};
  b.value         = std::move(p);
  std::vector<Binding> binds;
  binds.push_back(std::move(b));
  auto letExpr = mkExpr(ELet{.recursive = false,
                             .bindings  = std::move(binds),
                             .body      = std::move(updated)});

  auto prog = pointProgram(std::move(letExpr));
  EXPECT_EQ(eval(std::move(prog)), "Point 5 2");
}

TEST(VlaceRecordsTest, UpdateExpressionUsesAccessor) {
  // let p = Point { x = 1, y = 2 } in p { x = x p + 10 }  ==>  Point 11 2
  std::vector<std::pair<std::string, Expr>> ctorFields;
  ctorFields.emplace_back("x", mkInt(1));
  ctorFields.emplace_back("y", mkInt(2));
  auto p = mkRecord(mkCtor("Point"), std::move(ctorFields));

  auto newX = mkBinOp("+", mkApp(mkVar("x"), mkVar("p")), mkInt(10));
  std::vector<std::pair<std::string, Expr>> upd;
  upd.emplace_back("x", std::move(newX));
  auto updated = mkRecord(mkVar("p"), std::move(upd));

  Binding b;
  b.pattern       = std::make_unique<PatternNode>();
  b.pattern->node = PVar{.name = "p"};
  b.value         = std::move(p);
  std::vector<Binding> binds;
  binds.push_back(std::move(b));
  auto letExpr = mkExpr(ELet{.recursive = false,
                             .bindings  = std::move(binds),
                             .body      = std::move(updated)});

  auto prog = pointProgram(std::move(letExpr));
  EXPECT_EQ(eval(std::move(prog)), "Point 11 2");
}

TEST(VlaceRecordsTest, UnknownFieldIsRefused) {
  // Point { z = 1 }  ==>  UnboundIdentifier (no field z)
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("z", mkInt(1));
  auto prog = pointProgram(mkRecord(mkCtor("Point"), std::move(fields)));
  EXPECT_EQ(eval(std::move(prog)), "ERR:UnboundIdentifier:record 'Point' "
                                   "has no field 'z'");
}

// -- type checker ------------------------------------------------------------

TEST(VlaceRecordsTest, ConstructionType) {
  // Point { x = 1, y = 2 } : Point
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("x", mkInt(1));
  fields.emplace_back("y", mkInt(2));
  auto prog = pointProgram(mkRecord(mkCtor("Point"), std::move(fields)));
  EXPECT_EQ(typecheck(std::move(prog)), "Point");
}

TEST(VlaceRecordsTest, FieldAccessType) {
  // x (Point { x = 1, y = 2 }) : Int
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back("x", mkInt(1));
  fields.emplace_back("y", mkInt(2));
  auto rec  = mkRecord(mkCtor("Point"), std::move(fields));
  auto prog = pointProgram(mkApp(mkVar("x"), std::move(rec)));
  EXPECT_EQ(typecheck(std::move(prog)), "Int");
}

TEST(VlaceRecordsTest, FunctionalUpdateType) {
  // let p = Point { x = 1, y = 2 } in p { x = 5 } : Point
  std::vector<std::pair<std::string, Expr>> ctorFields;
  ctorFields.emplace_back("x", mkInt(1));
  ctorFields.emplace_back("y", mkInt(2));
  auto p = mkRecord(mkCtor("Point"), std::move(ctorFields));

  std::vector<std::pair<std::string, Expr>> upd;
  upd.emplace_back("x", mkInt(5));
  auto updated = mkRecord(mkVar("p"), std::move(upd));

  Binding b;
  b.pattern       = std::make_unique<PatternNode>();
  b.pattern->node = PVar{.name = "p"};
  b.value         = std::move(p);
  std::vector<Binding> binds;
  binds.push_back(std::move(b));
  auto letExpr = mkExpr(ELet{.recursive = false,
                             .bindings  = std::move(binds),
                             .body      = std::move(updated)});

  auto prog = pointProgram(std::move(letExpr));
  EXPECT_EQ(typecheck(std::move(prog)), "Point");
}

TEST(VlaceRecordsTest, FieldTypeMismatchIsRejected) {
  // Point { x = "nope", y = 2 }  ==>  TypeMismatch (x :: Int)
  std::vector<std::pair<std::string, Expr>> fields;
  fields.emplace_back(
      "x", mkExpr(ELiteral{.lit = Literal{.kind      = LiteralKind::String,
                                          .textValue = "nope"}}));
  fields.emplace_back("y", mkInt(2));
  auto prog = pointProgram(mkRecord(mkCtor("Point"), std::move(fields)));
  const std::string r = typecheck(std::move(prog));
  EXPECT_EQ(r.substr(0, 15), "ERR:TypeMismatc");
}

} // namespace
