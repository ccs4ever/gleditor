/**
 * @file vql_types_functors_test.cpp
 * @brief VQL v16.0: gradual structural types, unified shape contracts and
 * point-free path functors (§4.12).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

/// One arena, one engine, and a rank of items along home's +d.items.
class VQLTypesFunctorsTest : public ::testing::Test {
protected:
  CellRef item(const std::string &text) {
    const CellRef cell = arena.makeCell(text);
    EXPECT_TRUE(arena.link(lastItem, dim("d.items"), DimVector::POS, cell));
    lastItem = cell;
    return cell;
  }

  CellRef attach(CellRef from, const std::string &dimName,
                 const std::string &text) {
    const CellRef cell = arena.makeCell(text);
    EXPECT_TRUE(arena.link(from, dim(dimName), DimVector::POS, cell));
    return cell;
  }

  DimRef dim(const std::string &name) { return engine.resolveDimension(name); }

  std::string text(CellRef cell) {
    const auto value = core.render(cell);
    if (const auto *s = std::get_if<std::string>(&value)) {
      return *s;
    }
    if (const auto *i = std::get_if<std::int64_t>(&value)) {
      return std::to_string(*i);
    }
    return "<non-text>";
  }

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  CellRef lastItem{core.home()};
};

// -- Parsing ------------------------------------------------------------------

TEST_F(VQLTypesFunctorsTest, ParsesTypedSignatureAndReturnType) {
  Parser parser("def pure f($c: Cell, $d: Dim?, $xs: Stream<Int | Float>,"
                " $p: Cell { +d.title: String }): (Int | String)? := 1; f()");
  const QueryExpression query = parser.parseQuery();
  ASSERT_EQ(query.defs.size(), 1u);
  const auto &def = query.defs.front();
  ASSERT_EQ(def.params.size(), 4u);

  ASSERT_NE(def.params[0].type, nullptr);
  const auto &cell = def.params[0].type->unionType.members.at(0);
  EXPECT_EQ(std::get<BasicType>(cell.kind).kind, BasicType::Kind::Cell);
  EXPECT_FALSE(cell.isOptional);

  const auto &optDim = def.params[1].type->unionType.members.at(0);
  EXPECT_EQ(std::get<BasicType>(optDim.kind).kind, BasicType::Kind::Dim);
  EXPECT_TRUE(optDim.isOptional);

  const auto &stream =
      std::get<GenericType>(def.params[2].type->unionType.members.at(0).kind);
  EXPECT_EQ(stream.name, "Stream");
  ASSERT_EQ(stream.args.size(), 1u);
  EXPECT_EQ(stream.args[0]->unionType.members.size(), 2u);

  const auto &refined = def.params[3].type->unionType.members.at(0);
  ASSERT_TRUE(refined.refinement.has_value());
  ASSERT_EQ(refined.refinement->pattern.fields.size(), 1u);
  EXPECT_EQ(refined.refinement->pattern.fields[0].dimName, "d.title");
  EXPECT_TRUE(refined.refinement->pattern.fields[0].varName.empty());

  ASSERT_NE(def.returnType, nullptr);
  const auto &ret = def.returnType->unionType.members.at(0);
  EXPECT_TRUE(ret.isOptional);
  EXPECT_TRUE(std::holds_alternative<std::shared_ptr<TypeExpr>>(ret.kind));
}

TEST_F(VQLTypesFunctorsTest, ParsesAllFourDestructureFieldForms) {
  Parser parser("def pure f({ .: $self, +d.a, +d.b: $b, +d.c: Int,"
                " -d.e: $e: String := \"x\" }) := $b; f()");
  const auto query = parser.parseQuery();
  const auto &field =
      std::get<DestructurePattern>(query.defs.front().params.front().target)
          .fields;
  ASSERT_EQ(field.size(), 5u);
  EXPECT_TRUE(field[0].isContext);
  EXPECT_EQ(field[0].varName, "self");
  EXPECT_EQ(field[1].dimName, "d.a");
  EXPECT_TRUE(field[1].varName.empty());
  EXPECT_EQ(field[1].type, nullptr);
  EXPECT_EQ(field[2].varName, "b");
  EXPECT_EQ(field[3].type->unionType.members.size(), 1u);
  EXPECT_TRUE(field[3].varName.empty());
  EXPECT_EQ(field[4].direction, DimVector::NEG);
  EXPECT_EQ(field[4].varName, "e");
  EXPECT_NE(field[4].type, nullptr);
  EXPECT_NE(field[4].defaultValue, nullptr);
}

TEST_F(VQLTypesFunctorsTest, ParsesLeadingSlashAsPartialPathFunctor) {
  Parser parser("let $up := /+d.parent; $c/$up");
  const auto query = parser.parseQuery();
  ASSERT_EQ(query.body.statements.size(), 2u);
  const auto &block = std::get<ExecutionBlock>(query.body.statements[0]);
  const auto &let   = std::get<LetClause>(block.bindings.at(0));
  const auto &path  = std::get<PathExpression>(let.target);
  EXPECT_TRUE(path.isFunctor);
  ASSERT_EQ(path.steps.size(), 1u);
  EXPECT_FALSE(std::get<PathExpression>(query.body.statements[1]).isFunctor);
}

// -- Typed UDF signatures and return types ---------------------------------

TEST_F(VQLTypesFunctorsTest, TypedCellAndDimParametersGateTheCall) {
  const CellRef book = item("Book");
  const CellRef tag  = attach(book, "d.tag", "SciFi");
  engine.setVariable("dim", dim("d.tag"));
  engine.setVariable("notDim", tag);

  const std::string def = "def pure tagged($c: Cell, $d: Dim) := $c/+d.tag;";
  auto ok               = engine.execute(def + "tagged(##/+d.items, $dim)");
  ASSERT_EQ(ok.size(), 1u);
  EXPECT_EQ(ok[0], tag);

  EXPECT_TRUE(engine.execute(def + "tagged(##/+d.items, $notDim)").empty());
}

TEST_F(VQLTypesFunctorsTest,
       NonStreamTypeRefusesSeveralCellsStreamAcceptsThem) {
  item("A");
  item("B");
  EXPECT_TRUE(
      engine.execute("def pure one($x: Cell) := $x; one(##/+d.items)").empty());
  EXPECT_EQ(
      engine.execute("def pure all($xs: Stream<Cell>) := $xs; all(##/+d.items)")
          .size(),
      2u);
}

TEST_F(VQLTypesFunctorsTest, UnionAndOptionalParameterTypes) {
  const std::string def = "def pure num($x: Int | Bool) := $x;";
  EXPECT_EQ(engine.execute(def + "num(7)").size(), 1u);
  EXPECT_EQ(engine.execute(def + "num(true)").size(), 1u);
  EXPECT_TRUE(engine.execute(def + "num(\"seven\")").empty());

  EXPECT_TRUE(
      engine.execute("def pure need($x: Cell) := 1; need($missing)").empty());
  EXPECT_EQ(
      engine.execute("def pure may($x: Cell?) := 1; may($missing)").size(), 1u);
}

TEST_F(VQLTypesFunctorsTest, ReturnTypeIsAPromiseNotAFilter) {
  auto good = engine.execute("def pure n(): Int := 42; n()");
  ASSERT_EQ(good.size(), 1u);
  EXPECT_EQ(text(good[0]), "42");

  EXPECT_THROW(engine.execute("def pure bad(): Int := \"abc\"; bad()"),
               TypeContractError);
  EXPECT_TRUE(
      engine.execute("def pure none(): Cell? := $missing; none()").empty());
}

TEST_F(VQLTypesFunctorsTest, UnknownTypeNameIsRefusedByName) {
  try {
    engine.execute("def pure f($x: Widget) := $x; f(1)");
    FAIL() << "an unknown type was accepted";
  } catch (const TypeContractError &e) {
    EXPECT_THAT(e.what(), ::testing::HasSubstr("Widget"));
  }
}

// -- Unified shape contracts --------------------------------------------------

TEST_F(VQLTypesFunctorsTest, Mode1PureTypeContractFiltersByShape) {
  const CellRef paper = item("Paper");
  attach(paper, "d.title", "On Hypertext");
  const CellRef score = attach(paper, "d.score", "95");

  const CellRef vague = item("Vague");
  attach(vague, "d.title", "Untitled");
  attach(vague, "d.score", "high");

  const CellRef bare = item("Bare");
  attach(bare, "d.score", "12");

  auto res = engine.execute(R"(
    def pure score_paper($p: Cell { +d.title: String, +d.score: Int }): Int :=
      $p/+d.score;
    ##/+d.items/score_paper()
  )");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], score);
}

TEST_F(VQLTypesFunctorsTest, Mode2DestructuringDefaultsOnlyWhenAbsent) {
  const CellRef tagged = item("Tagged");
  attach(tagged, "d.title", "First");
  const CellRef tag = attach(tagged, "d.tag", "essay");

  const CellRef plain = item("Plain");
  attach(plain, "d.title", "Second");

  auto res = engine.execute(R"(
    def pure tag_of({ ., +d.title: $t, +d.tag: $tag := "note" }): String := $tag;
    ##/+d.items/tag_of()
  )");
  ASSERT_EQ(res.size(), 2u);
  // Present: the linked cell itself, nothing re-minted.
  EXPECT_EQ(res[0], tag);
  EXPECT_EQ(text(res[1]), "note");
}

TEST_F(VQLTypesFunctorsTest, Mode3CombinedBindingAndTypeContract) {
  const CellRef clip = item("Clip");
  attach(clip, "d.codec", "av1");
  const CellRef fps = attach(clip, "d.fps", "30");

  const CellRef still = item("Still");
  attach(still, "d.codec", "png");

  const CellRef broken = item("Broken");
  attach(broken, "d.codec", "h264");
  attach(broken, "d.fps", "fast");

  auto res = engine.execute(R"(
    def pure fps_of({ ., +d.codec: $c: String, +d.fps: $fps: Int := 60 }) := $fps;
    ##/+d.items/fps_of()
  )");
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0], fps);
  EXPECT_EQ(text(res[1]), "60");
}

TEST_F(VQLTypesFunctorsTest, Mode4ParameterAliasingBindsCellAndFields) {
  const CellRef task  = item("Ship it");
  const CellRef prio  = attach(task, "d.priority", "high");
  const CellRef owner = attach(task, "d.owner", "Ted");

  const CellRef orphan = item("Orphan");
  attach(orphan, "d.priority", "low");

  const std::string def = R"(
    def weave owner_of($task: Cell { +d.priority: $p: String,
                                     +d.owner: $o: Cell }): Cell := {
      return $o;
    };
    def pure task_of($task: Cell { +d.priority: $p: String,
                                   +d.owner: $o: Cell }) := $task;
    def pure prio_of($task: Cell { +d.priority: $p: String,
                                   +d.owner: $o: Cell }) := $p;
  )";
  auto owners           = engine.execute(def + "##/+d.items/owner_of()");
  ASSERT_EQ(owners.size(), 1u);
  EXPECT_EQ(owners[0], owner);

  auto tasks = engine.execute(def + "##/+d.items/task_of()");
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0], task);

  auto prios = engine.execute(def + "##/+d.items/prio_of()");
  ASSERT_EQ(prios.size(), 1u);
  EXPECT_EQ(prios[0], prio);
}

TEST_F(VQLTypesFunctorsTest, OptionalFieldTypeAdmitsAbsence) {
  const CellRef withSub = item("A");
  const CellRef sub     = attach(withSub, "d.subtitle", "sub");
  item("B");

  auto res =
      engine.execute("##/+d.items / \\({ ., +d.subtitle: $s: String? }) $s");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], sub);
  // The match itself is not refused for the cell without one.
  EXPECT_EQ(
      engine.execute("##/+d.items / \\({ ., +d.subtitle: String? }) .").size(),
      2u);
}

TEST_F(VQLTypesFunctorsTest, TypedLambdaParameterAndForDestructure) {
  const CellRef a  = item("A");
  const CellRef ta = attach(a, "d.tag", "x");
  const CellRef b  = item("B");
  attach(b, "d.score", "9");
  const CellRef c = item("C");
  attach(c, "d.score", "nine");

  auto lambda =
      engine.execute("##/+d.items / \\($x: Cell { +d.tag: String }) $x/+d.tag");
  ASSERT_EQ(lambda.size(), 1u);
  EXPECT_EQ(lambda[0], ta);

  // A for binding still binds nothing for a missing field, but a written type
  // refuses the cell whose field breaks it.
  auto loop =
      engine.execute("for { ., +d.score: $s: Int? } in ##/+d.items return .");
  EXPECT_THAT(loop, ::testing::ElementsAre(a, b));
}

TEST_F(VQLTypesFunctorsTest, LetOfAVariableBindsTheSameCells) {
  const CellRef a = item("A");
  engine.setVariable("x", a);
  auto res = engine.execute("let $y := $x; $y");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], a);
}

// -- Point-free traversal functors -------------------------------------------

TEST_F(VQLTypesFunctorsTest, LetBindsFunctorAndAppliesItAsAStep) {
  const CellRef child  = arena.makeCell("child");
  const CellRef parent = attach(child, "d.parent", "parent");
  engine.setVariable("c", child);

  auto res = engine.execute("let $up := /+d.parent; $c / $up");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], parent);
}

TEST_F(VQLTypesFunctorsTest, ComposesFunctorsWithVariablesAndLiterals) {
  const CellRef child  = arena.makeCell("child");
  const CellRef parent = attach(child, "d.parent", "parent");
  const CellRef title  = attach(parent, "d.title", "Parent Title");
  const CellRef meta   = attach(parent, "d.meta", "meta");
  const CellRef author = attach(meta, "d.author", "Ted Nelson");
  engine.setVariable("c", child);

  auto titled = engine.execute(R"(
    let $up := /+d.parent;
    let $titled := $up / /+d.title;
    $c / $titled
  )");
  ASSERT_EQ(titled.size(), 1u);
  EXPECT_EQ(titled[0], title);

  auto authored = engine.execute(R"(
    let $to_parent := /+d.parent;
    let $to_author := /+d.meta/+d.author;
    let $inspect := $to_parent / $to_author;
    $c / $inspect
  )");
  ASSERT_EQ(authored.size(), 1u);
  EXPECT_EQ(authored[0], author);
}

TEST_F(VQLTypesFunctorsTest, FunctorMapsOverAnIncomingStream) {
  const CellRef a  = item("A");
  const CellRef pa = attach(a, "d.parent", "pa");
  const CellRef b  = item("B");
  const CellRef pb = attach(b, "d.parent", "pb");

  auto res = engine.execute("let $up := /+d.parent; ##/+d.items / $up");
  EXPECT_THAT(res, ::testing::ElementsAre(pa, pb));
}

TEST_F(VQLTypesFunctorsTest, ComposedFunctorKeepsTheStepsItWasBuiltFrom) {
  const CellRef child  = arena.makeCell("child");
  const CellRef parent = attach(child, "d.parent", "parent");
  const CellRef title  = attach(parent, "d.title", "T");
  attach(child, "d.other", "other");
  engine.setVariable("c", child);

  auto res = engine.execute(R"(
    let $up := /+d.parent;
    let $titled := $up / /+d.title;
    let $up := /+d.other;
    $c / $titled
  )");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], title);
}

TEST_F(VQLTypesFunctorsTest, FunctorPassedToADefIsAppliedThere) {
  const CellRef child  = arena.makeCell("child");
  const CellRef parent = attach(child, "d.parent", "parent");
  engine.setVariable("c", child);

  const std::string def = "def pure via($x, $f) := $x / $f;";
  auto byName = engine.execute(def + "let $up := /+d.parent; via($c, $up)");
  ASSERT_EQ(byName.size(), 1u);
  EXPECT_EQ(byName[0], parent);

  auto literal = engine.execute(def + "via($c, /+d.parent)");
  ASSERT_EQ(literal.size(), 1u);
  EXPECT_EQ(literal[0], parent);
}

TEST_F(VQLTypesFunctorsTest, StepPredicatesApplyToWhatTheFunctorReaches) {
  const CellRef a = item("A");
  attach(a, "d.parent", "keep");
  const CellRef b = item("B");
  attach(b, "d.parent", "drop");

  auto res =
      engine.execute("let $up := /+d.parent; ##/+d.items / $up[. = \"keep\"]");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(text(res[0]), "keep");
}

TEST_F(VQLTypesFunctorsTest, RebindingAFunctorNameToCellsForgetsTheFunctor) {
  const CellRef a = item("A");
  engine.execute("let $up := /+d.parent");
  engine.setVariable("up", a);
  auto res = engine.execute("$up");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], a);
}

TEST_F(VQLTypesFunctorsTest, CompilerRefusesAFunctorBindingByName) {
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(core, vm);
  const auto result = compiler.compile("let $up := /+d.parent return $up");
  EXPECT_FALSE(result.success);
  EXPECT_THAT(result.errorMessage, ::testing::HasSubstr("functor"));
}

} // namespace
