/**
 * @file vql_structural_frames_test.cpp
 * @brief Structural VQL contracts through reusable Vortex frames.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_structural_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CellRef first{core.home()};

  DimRef dim(const std::string &name) {
    return coordinator.resolveDimension(name);
  }

  CellRef item(const std::string &text) {
    const CellRef result = arena.makeCell(text);
    zigzag::expectWritten(
        arena.link(first, dim("d.items"), DimVector::POS, result));
    first = result;
    return result;
  }

  CellRef attach(const CellRef from, const std::string &dimension,
                 const std::string &text) {
    const CellRef result = arena.makeCell(text);
    zigzag::expectWritten(
        arena.link(from, dim(dimension), DimVector::POS, result));
    return result;
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {},
      const std::vector<CellRef> &context = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source, context);
  }

  std::size_t invokeCount(const std::string_view source) {
    VQLCompiler compiler(coordinator, vm);
    const auto result = compiler.compile(source);
    EXPECT_TRUE(result.success) << result.errorMessage;
    std::size_t count = 0;
    for (const CellRef opcode : result.generatedOpcodes)
      count += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    return count;
  }
};

TEST(VQLStructuralFrameTest, AllFourStructuralModesUseFramesAndMatchValues) {
  Harness h;
  const CellRef paper = h.item("paper");
  h.attach(paper, "d.title", "The title");
  const CellRef score = h.attach(paper, "d.score", "95");
  const CellRef tag   = h.attach(paper, "d.tag", "essay");
  const CellRef bare  = h.item("bare");
  h.attach(bare, "d.score", "fast");
  h.item("plain");

  const auto mode1 = h.run(
      "def pure good($p: Cell { +d.title: String, +d.score: Int }): Cell := "
      "$p; good(##/d.items[1])");
  ASSERT_TRUE(mode1) << mode1.error().message;
  EXPECT_EQ(*mode1, (std::vector<CellRef>{paper}));

  const auto mode2 =
      h.run("def pure tag_of({ ., +d.tag: $tag := \"note\" }): Cell := $tag; "
            "tag_of(##/d.items[1])");
  ASSERT_TRUE(mode2) << mode2.error().message;
  EXPECT_EQ(*mode2, (std::vector<CellRef>{tag}));
  const auto mode2Default =
      h.run("def pure tag_of({ ., +d.tag: $tag := \"note\" }): String := $tag; "
            "tag_of(##/d.items[3])");
  ASSERT_TRUE(mode2Default) << mode2Default.error().message;
  ASSERT_EQ(mode2Default->size(), 1u);
  EXPECT_EQ(h.arena.textOf(mode2Default->front()), "note");

  const auto mode3 =
      h.run("def pure score_of({ ., +d.score: $score: Int := 60 }): Int := "
            "$score; score_of(##/d.items[1])");
  ASSERT_TRUE(mode3) << mode3.error().message;
  EXPECT_EQ(*mode3, (std::vector<CellRef>{score}));
  const auto mode3Default =
      h.run("def pure score_of({ ., +d.score: $score: Int := 60 }): Int := "
            "$score; score_of(##/d.items[3])");
  ASSERT_TRUE(mode3Default) << mode3Default.error().message;
  ASSERT_EQ(mode3Default->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(mode3Default->front()), 60);
  const auto mismatch = h.run(
      "def pure required($item: Cell { +d.required: String }): Cell := $item; "
      "required(##/d.items[3])");
  ASSERT_TRUE(mismatch) << mismatch.error().message;
  EXPECT_TRUE(mismatch->empty());
  const CellRef meta        = h.attach(paper, "d.meta", "metadata");
  const CellRef nestedScore = h.attach(meta, "d.score", "95");
  const auto nested =
      h.run("def pure nested($item: Cell { +d.meta: Cell { +d.score: $score: "
            "Int? } }): Cell? := $score; nested(##/d.items[1])");
  ASSERT_TRUE(nested) << nested.error().message;
  EXPECT_EQ(*nested, (std::vector<CellRef>{nestedScore}));

  const auto mode4 =
      h.run("def pure alias($p: Cell { +d.title: $title: String }): Cell := "
            "$title; alias(##/d.items[1])");
  ASSERT_TRUE(mode4) << mode4.error().message;
  EXPECT_EQ(
      *mode4,
      (std::vector<CellRef>{
          h.arena.linked(paper, h.dim("d.title"), DimVector::POS).value()}));
  const auto mode4Parent =
      h.run("def pure identity($p: Cell { +d.title: String }): Cell := $p; "
            "identity(##/d.items[1])");
  ASSERT_TRUE(mode4Parent) << mode4Parent.error().message;
  EXPECT_EQ(*mode4Parent, (std::vector<CellRef>{paper}));

  for (const auto query :
       {"def pure good($p: Cell { +d.title: String }): Cell := $p; "
        "good(##/d.items[1])",
        "def pure bind({ ., +d.tag: $tag }): Cell := $tag; "
        "bind(##/d.items[1])",
        "def pure both({ ., +d.score: $score: Int }): Cell := $score; "
        "both(##/d.items[1])",
        "def pure alias($p: Cell { +d.title: $title: String }): Cell := "
        "$title; alias(##/d.items[1])"})
    EXPECT_GT(h.invokeCount(query), 0u);
}

TEST(VQLStructuralFrameTest, StreamShapePreservesOrderDuplicatesAndScope) {
  Harness h;
  const CellRef a = h.arena.makeCell("A");
  const CellRef b = h.arena.makeCell("B");
  ContractBindings bindings{{"items", {a, b, a}}};
  const auto result = h.run(
      "def pure pass($xs: Stream<Cell { +d.tag: String? }>): Stream<Cell> "
      ":= $xs; pass($items)",
      std::move(bindings));
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{a, b, a}));

  const auto after = h.run("$capture");
  ASSERT_TRUE(after) << after.error().message;
  EXPECT_TRUE(after->empty());
}

TEST(VQLStructuralFrameTest, LaterDefaultsSeeEarlierParameterCaptures) {
  Harness h;
  const CellRef root = h.item("root");
  const CellRef tag  = h.attach(root, "d.tag", "from shape");
  const auto result =
      h.run("def pure choose($item: Cell { +d.tag: $tag: String }, "
            "$fallback: String := $tag): String := $fallback; "
            "choose(##/d.items[1])");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{tag}));
}

TEST(VQLStructuralFrameTest, PatternDefaultCanReadEarlierFormalParameter) {
  Harness h;
  const CellRef root     = h.item("root");
  const CellRef fallback = h.arena.makeCell("fallback");
  const auto result =
      h.run("def pure choose($fallback: Cell, $item: Cell { +d.missing: "
            "$value: Cell := $fallback }): Cell := $value; "
            "choose($source, ##/d.items[1])",
            ContractBindings{{"source", {fallback}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{fallback}));
  EXPECT_NE(root, fallback);
}

TEST(VQLStructuralFrameTest,
     PatternDefaultRetainsCallerAliasWithSameCaptureName) {
  Harness h;
  const CellRef root = h.item("root");
  h.attach(root, "d.present", "local capture");
  const CellRef outside = h.arena.makeCell("caller default");
  ContractBindings bindings{{"outside", {outside}}};

  const auto result =
      h.run("def pure choose($item: Cell { +d.present: $outside: String, "
            "+d.missing: $value: String := $outside }): String := $value; "
            "choose(##/d.items[1])",
            std::move(bindings));
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{outside}));
}

TEST(VQLStructuralFrameTest,
     UnionGenericAndOptionalContractsDoNotLeakCaptures) {
  Harness h;
  const CellRef root = h.item("root");
  h.attach(root, "d.mark", "ok");
  const CellRef caller = h.arena.makeCell("caller value");
  ContractBindings bindings{{"leak", {caller}}};
  const auto unionResult =
      h.run("def pure read($item: Cell { +d.mark: $leak: String, "
            "+d.absent: Cell } | Cell): Cell? := $leak; "
            "read(##/d.items[1])",
            bindings);
  ASSERT_TRUE(unionResult) << unionResult.error().message;
  EXPECT_TRUE(unionResult->empty());

  const auto genericResult =
      h.run("def pure pass($prior: String, "
            "$items: Stream<Cell { +d.mark: $prior: String }>, "
            "$fallback: String := $prior): String := $fallback; "
            "pass(\"caller\", ##/d.items)",
            std::move(bindings));
  ASSERT_TRUE(genericResult) << genericResult.error().message;
  ASSERT_EQ(genericResult->size(), 1u);
  EXPECT_EQ(h.arena.textOf(genericResult->front()), "caller");
  EXPECT_GT(h.invokeCount("def pure pass($prior: String, "
                          "$items: Stream<Cell { +d.mark: $prior: String }>, "
                          "$fallback: String := $prior): String := $fallback; "
                          "pass(\"caller\", ##/d.items)"),
            0u);

  const auto callerOnly =
      h.run("def pure pass($items: Stream<Cell { +d.mark: String }>, "
            "$fallback: String := $leak): String := $fallback; "
            "pass(##/d.items)",
            ContractBindings{{"leak", {caller}}});
  ASSERT_TRUE(callerOnly) << callerOnly.error().message;
  EXPECT_EQ(*callerOnly, (std::vector<CellRef>{caller}));
  EXPECT_EQ(
      h.invokeCount("def pure pass($items: Stream<Cell { +d.mark: String }>, "
                    "$fallback: String := $leak): String := $fallback; "
                    "pass(##/d.items)"),
      0u);

  const auto optional = h.run(
      "def pure maybe($item: Cell { +d.absent: String? }): Cell := $item; "
      "maybe(##/d.items[1])");
  ASSERT_TRUE(optional) << optional.error().message;
  EXPECT_EQ(*optional, (std::vector<CellRef>{root}));

  const auto badReturn =
      h.run("def pure invalid($item: Cell { +d.mark: String }): Int := "
            "$item/+d.mark; invalid(##/d.items[1])");
  ASSERT_FALSE(badReturn);
  EXPECT_EQ(badReturn.error().category,
            QueryExecutionError::Category::TypeContract);
}

TEST(VQLStructuralFrameTest, GenericRefinementFieldTypeCyclesAreHardErrors) {
  QueryExpression query =
      Parser(
          "def pure pass($items: Stream<Cell>): Stream<Cell> := $items; pass()")
          .parseQuery();
  auto &generic = std::get<GenericType>(
      query.defs.front().params.front().type->unionType.members.front().kind);
  auto refined = generic.args.front();
  DestructurePattern pattern;
  pattern.fields.push_back(DestructureField{.dimName = "d.child"});
  pattern.fields.back().type = refined;
  refined->unionType.members.front().refinement =
      ShapeType{.pattern = std::move(pattern)};

  const auto result = analyzeFrameEligibility(query);
  refined->unionType.members.front().refinement.reset();
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("cyclic"), std::string::npos);
}

TEST(VQLStructuralFrameTest,
     GenericPrimaryRefinementIsScannedWithoutPublishing) {
  auto query =
      Parser(
          "def pure pass($items: Stream<Cell> { +d.first: $outside: String, "
          "+d.second: $value: String := $outside }): Stream<Cell> := $items; "
          "pass()")
          .parseQuery();
  const auto open = analyzeFrameEligibility(query);
  ASSERT_TRUE(open) << open.error().message;
  EXPECT_FALSE(open->reusableDefinitions.contains("pass"));
  EXPECT_NE(open->refusalReasons.at("pass").find("$outside"),
            std::string::npos);

  auto root     = query.defs.front().params.front().type;
  auto &primary = root->unionType.members.front();
  primary.refinement->pattern.fields.front().type = root;
  const auto cyclic = analyzeFrameEligibility(query);
  primary.refinement.reset();
  ASSERT_FALSE(cyclic);
  EXPECT_NE(cyclic.error().message.find("cyclic"), std::string::npos);
}

TEST(VQLStructuralFrameTest, SamePatternDefaultCannotCaptureItsOwnMatch) {
  const QueryExpression query =
      Parser("def pure read($item: Cell { +d.first: $first: String, "
             "+d.second: $second: String := $first }): String := $second; "
             "read(##)")
          .parseQuery();
  const auto result = analyzeFrameEligibility(query);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->reusableDefinitions.contains("read"));
  EXPECT_NE(result->refusalReasons.at("read").find("$first"),
            std::string::npos);
}

TEST(VQLStructuralFrameTest, UnionArmCaptureDoesNotScopeAnotherArmsDefault) {
  const QueryExpression query =
      Parser("def pure read($item: Cell { +d.first: $outside: String } | "
             "Cell { +d.second: $value: String := $outside }): "
             "String := $value; read(##)")
          .parseQuery();
  const auto result = analyzeFrameEligibility(query);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->reusableDefinitions.contains("read"));
  EXPECT_NE(result->refusalReasons.at("read").find("$outside"),
            std::string::npos);
}

TEST(VQLStructuralFrameTest,
     ContextCaptureOverridesCallerAndNestedCallRestoresIt) {
  Harness h;
  const CellRef first   = h.item("first");
  const CellRef nested  = h.attach(first, "d.child", "nested");
  const CellRef ambient = h.arena.makeCell("ambient");
  ContractBindings bindings{{"self", {ambient}}};
  const auto innerResult =
      h.run("def pure inner($item: Cell { .: $self }): Cell := $self; "
            "inner(##/d.items[1])",
            bindings);
  ASSERT_TRUE(innerResult) << innerResult.error().message;
  EXPECT_EQ(*innerResult, (std::vector<CellRef>{first}));
  const auto result =
      h.run("def pure inner($item: Cell { .: $self }): Cell := .; "
            "def pure outer($item: Cell { .: $self, +d.child: $child: Cell }): "
            "Cell := { let $found := inner($child); return .; }; "
            "outer(##/d.items[1])",
            std::move(bindings));
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first}));
  EXPECT_NE(nested, first);
}

TEST(VQLStructuralFrameTest, RecursiveStructuralFrameSurvivesFreshStoreReload) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const CellRef second = h.item("second");
    const CellRef third  = h.item("third");
    zigzag::expectWritten(
        h.arena.link(second, h.dim("d.next"), DimVector::POS, third));
    zigzag::expectWritten(
        h.arena.link(h.core.home(), h.dim("d.next"), DimVector::POS, second));
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(
        "def pure last($node: Cell { +d.next: $next: Cell? }): Cell := "
        "{ if $next return last($next) else return $node; }; last(##)");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  Store store(scroll);
  ASSERT_NO_THROW(store.load((temporary.path / "program").string()));
  ASSERT_TRUE(store.structureHead());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "structural-frame-reload");
  const auto execution = vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultSlots = core.inputsOf(*next);
  ASSERT_EQ(resultSlots.size(), 1u);
  const auto results = vm.resultCells(resultSlots.front());
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(arena.textOf(results.front()), "third");
}

TEST(VQLStructuralFrameTest, HostAndMeshVquerySourceRoutesExecuteShapeFrames) {
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(
      "def pure identity($value: Int { .: $self: Int }): Int := $self; "
      "identity(7)");
  ASSERT_TRUE(hostResult) << hostResult.error().message;

  const auto meshResult = zigzag::vortex::mesh::evaluate(
      zigzag::vortex::mesh::Frontend::Vquery,
      "def pure identity($value: Int { .: $self: Int }): Int := $self; "
      "identity(7)",
      {.maxCycles      = 10000,
       .maxCells       = 10000,
       .maxSolutions   = 16,
       .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
}

} // namespace
} // namespace xanadu::vql
