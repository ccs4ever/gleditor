// Phase-3 fragment emission tests (design/vagent-metaprogramming-
// implementation.md:224-321 — the emission half). The pipeline side is
// driven through frontend's compileArtifacts (parse → resolve → expand
// → check) so the FragmentContext sees the real §5.6 tables; the arena
// side asserts the §6.1 object schema meta.cpp reads back — head,
// descriptor fields, ordered parameter cells, +d.refs name/ref
// records, +d.recipe member taxonomy and the cap$N clone-rank slots.
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/fragment.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

// One arena/core/vm/compiler plus the phase-3 frontend driver.
// `pipeline` runs the full frontend and returns the artifact tables;
// `contextOf` binds them into the FragmentContext emission reads.
struct FragmentHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};

  std::expected<CompileArtifacts, std::vector<Diagnostic>>
  pipeline(std::string_view source) {
    return compileArtifacts(source);
  }

  // The first member decl carrying a body — the test programs keep a
  // single `def` per agent, so its body is the compiled statement.
  static const Expr *firstMethodBody(const Program &program) {
    for (const auto &decl : program.declarations)
      for (const auto &member : decl.members)
        if (member.body) return member.body.get();
    return nullptr;
  }
  static const Expr *findKind(const Expr &root, ExprKind kind) {
    if (root.kind == kind) return &root;
    for (const auto &child : root.children)
      if (child)
        if (const auto *found = findKind(*child, kind)) return found;
    return nullptr;
  }

  FragmentContext contextOf(const CompileArtifacts &artifacts,
                            const CompilerBindings &bindings) {
    return {.fragments = &artifacts.expansion.fragments,
            .meta      = &artifacts.expansion.fragmentMeta,
            .captures  = &artifacts.expansion.captures,
            .info      = &artifacts.fragmentInfo,
            .bindings  = &bindings,
            .module    = artifacts.program.module,
            .emitted   = {}};
  }

  CellRef run(const CompiledStatement &statement) {
    const auto cursor = vm.spawnCursor(statement.entry);
    vm.setCursorOpcode(cursor, statement.entry);
    const auto result = vm.run(cursor);
    EXPECT_TRUE(result.success) << result.errorMessage;
    return cursor;
  }

  bool produced(const CompiledStatement &statement) {
    return arena.asBool(statement.produced).value_or(false);
  }

  // A descriptor's +d.vars record whose content is `key` verbatim —
  // the same walk the sibling's descriptorField performs.
  std::optional<CellRef> fieldRecord(CellRef descriptor, std::string_view key) {
    for (const auto record :
         zigzag::rankAfter(arena, descriptor, core.dims().vars))
      if (arena.textOf(record) == key) return record;
    return std::nullopt;
  }
  std::optional<CellRef> fieldValue(CellRef descriptor, std::string_view key) {
    const auto record = fieldRecord(descriptor, key);
    return record ? arena.linked(*record, core.dims().values, Posward)
                  : std::nullopt;
  }

  // `host +firstDim → c1 +d.step → c2 …` — the chain shape +d.refs,
  // +d.recipe and the parameter rank share.
  std::vector<CellRef> chain(CellRef host, DimRef firstDim) {
    std::vector<CellRef> out;
    for (auto cell = arena.linked(host, firstDim, Posward);
         cell && arena.contains(*cell);
         cell = arena.linked(*cell, core.dims().step, Posward))
      out.push_back(*cell);
    return out;
  }

  // The capture record naming `captured`, or nullptr — the cap$N alias
  // mint order is expand's, so tests look records up by captured name.
  const CaptureRecord *capture(const CompileArtifacts &artifacts,
                               std::string_view captured) {
    for (const auto &[alias, record] : artifacts.expansion.captures)
      if (record.captured == captured) return &record;
    return nullptr;
  }
};

std::string describe(const std::vector<Diagnostic> &diagnostics) {
  std::string out;
  for (const auto &diagnostic : diagnostics)
    out += formatDiagnostic(diagnostic) + '\n';
  return out;
}

// ---- `run` on the emitted call convention -------------------------------

TEST(VagentFragmentTest, RunNullaryExecutesOnTheEmissionConvention) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
module demo
import std:meta
agent MetaUser {
  def probe(): Int { run(code"1 + 2") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  EXPECT_EQ(statement->resultType, (Type{"Int", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{3}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  // One fragment object — memoized through context.emitted.
  EXPECT_EQ(context.emitted.size(), 1U);
}

TEST(VagentFragmentTest, RunBindsPositionalArgsByParameterName) {
  FragmentHarness h;
  // Subtraction pins the binding order: name-keyed on the cursor's
  // +d.vars rank, args bound in declaration order.
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"w: Int, v: Int | w - v", [10, 3]) }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{7}));
}

TEST(VagentFragmentTest, DynamicRunOfKnownCodeTypeYieldsItsResult) {
  FragmentHarness h;
  // `let c := code"…"` binds Cell in the scalar local but Code<Int>
  // on the richer channel — `run(c)` reports Int, not Bottom.
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int { let c := code"1 + 2"; run(c) }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  EXPECT_EQ(statement->resultType, (Type{"Int", {}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{3}));
}

TEST(VagentFragmentTest, RunOfACodeResultYieldsTheInnerHeadRef) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Code<Int> { run(code"code\"1\"") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  EXPECT_EQ(statement->resultType, (Type{"Code", {Type{"Int", {}}}}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  // The produced value is a ref-box whose int64 names a live head.
  const auto referent = h.arena.asInt64(statement->result);
  ASSERT_TRUE(referent);
  const auto fdims   = mintFragmentDims(h.core);
  const CellRef head = static_cast<CellRef>(*referent);
  ASSERT_TRUE(h.arena.contains(head));
  EXPECT_TRUE(h.arena.linked(head, fdims.entry, Posward).has_value());
}

// ---- descriptor / head schema -------------------------------------------

TEST(VagentFragmentTest, HeadAndDescriptorCarryTheSiblingContract) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
module demo
import std:meta
agent MetaUser {
  def probe(): Code<Int> { code"#inc w: Int | w + 1" }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));

  const auto referent = h.arena.asInt64(statement->result);
  ASSERT_TRUE(referent);
  const CellRef head = static_cast<CellRef>(*referent);
  ASSERT_TRUE(h.arena.contains(head));

  const auto fdims = mintFragmentDims(h.core);
  const auto &dims = h.core.dims();
  const auto entry = h.arena.linked(head, fdims.entry, Posward);
  ASSERT_TRUE(entry);
  EXPECT_TRUE(h.vm.getOpcodeKind(*entry).has_value());
  const auto label = h.arena.linked(head, dims.name, Posward);
  ASSERT_TRUE(label);
  EXPECT_EQ(h.arena.textOf(*label), "inc"); // declared #label wins

  const auto descriptor = h.arena.linked(head, dims.values, Posward);
  ASSERT_TRUE(descriptor);
  // result record + its +d.name result-type channel.
  const auto resultRecord = h.fieldRecord(*descriptor, "result");
  ASSERT_TRUE(resultRecord);
  EXPECT_TRUE(h.arena.linked(*resultRecord, dims.values, Posward).has_value());
  const auto resultType = h.arena.linked(*resultRecord, dims.name, Posward);
  ASSERT_TRUE(resultType);
  EXPECT_EQ(h.arena.textOf(*resultType), "Int");
  EXPECT_TRUE(h.fieldValue(*descriptor, "produced").has_value());
  const auto pure = h.fieldValue(*descriptor, "pure");
  ASSERT_TRUE(pure);
  EXPECT_EQ(h.arena.asBool(*pure).value_or(false), true);
  const auto closed = h.fieldValue(*descriptor, "closed");
  ASSERT_TRUE(closed);
  EXPECT_EQ(h.arena.asBool(*closed).value_or(false), true);
  const auto arity = h.fieldValue(*descriptor, "arity");
  ASSERT_TRUE(arity);
  EXPECT_EQ(h.arena.asInt64(*arity).value_or(-1), 1);
  // The per-parameter +d.vars field: name → type name.
  const auto paramField = h.fieldValue(*descriptor, "w");
  ASSERT_TRUE(paramField);
  EXPECT_EQ(h.arena.textOf(*paramField), "Int");

  // Ordered parameter cells: descriptor +d.grab, +d.step chained, each
  // +d.name-labelled and +d.values-typed (spec :914-917).
  const auto param = h.arena.linked(*descriptor, dims.grab, Posward);
  ASSERT_TRUE(param);
  const auto paramName = h.arena.linked(*param, dims.name, Posward);
  ASSERT_TRUE(paramName);
  EXPECT_EQ(h.arena.textOf(*paramName), "w");
  const auto paramType = h.arena.linked(*param, dims.values, Posward);
  ASSERT_TRUE(paramType);
  EXPECT_EQ(h.arena.textOf(*paramType), "Int");
  EXPECT_FALSE(h.arena.linked(*param, dims.step, Posward).has_value());
}

TEST(VagentFragmentTest, UnlabelledHeadCarriesTheSrcPositionLabel) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
module demo
import std:meta
agent MetaUser {
  def probe(): Code<Int> { code"1" }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  const auto referent = h.arena.asInt64(statement->result);
  ASSERT_TRUE(referent);
  const CellRef head = static_cast<CellRef>(*referent);
  const auto label   = h.arena.linked(head, h.core.dims().name, Posward);
  ASSERT_TRUE(label);
  // §6.7's default: src:<module>:<line>:<col>.
  EXPECT_TRUE(h.arena.textOf(*label).starts_with("src:demo:"));
}

// ---- refs / recipe record schema ----------------------------------------

TEST(VagentFragmentTest, CallsiteNameRecordsAndRecipeTaxonomy) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"VA(
module demo
import std:meta
agent MetaUser {
  def probe(): Int { run(code"run(code\"#inner 1 + 2\")") }
}
)VA");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{3}));

  // Two fragment objects: the outer literal and the nested one.
  ASSERT_EQ(context.emitted.size(), 2U);
  const auto fdims = mintFragmentDims(h.core);
  const auto &dims = h.core.dims();

  const Expr *outerLit = FragmentHarness::findKind(*body, ExprKind::Code);
  ASSERT_NE(outerLit, nullptr);
  const FragmentKey outerKey = fragmentKeyOf(*outerLit);
  const auto outer           = context.emitted.find(outerKey);
  ASSERT_NE(outer, context.emitted.end());
  const CellRef outerHead = outer->second.head;
  const auto &outerRecord = artifacts->expansion.fragments.at(outerKey);
  const Expr *innerLit =
      FragmentHarness::findKind(*outerRecord.body.body, ExprKind::Code);
  ASSERT_NE(innerLit, nullptr);
  const auto inner = context.emitted.find(fragmentKeyOf(*innerLit));
  ASSERT_NE(inner, context.emitted.end());
  const auto innerLabel =
      h.arena.linked(inner->second.head, dims.name, Posward);
  ASSERT_TRUE(innerLabel);
  EXPECT_EQ(h.arena.textOf(*innerLabel), "inner");

  // +d.refs: the outer body's run callsite carries the §6.2
  // name-record pair — "call" (entry-op operand slot) and "run"
  // (head-valued convention cell), both +d.name'd on the callee label.
  std::set<std::pair<std::string, std::string>> nameRecords;
  std::size_t refRecords = 0;
  for (const auto record : h.chain(outerHead, fdims.refs)) {
    const auto marked = h.arena.linked(record, fdims.ref, Posward);
    EXPECT_TRUE(marked.has_value());
    const auto name = h.arena.linked(record, dims.name, Posward);
    if (name) {
      std::string role;
      if (const auto roleCell = h.arena.linked(record, dims.grab, Posward))
        role = h.arena.textOf(*roleCell);
      nameRecords.emplace(h.arena.textOf(*name), role);
    } else {
      ++refRecords;
      // A REF-record's marked cell carries the relocation marker.
      const auto marker =
          h.arena.linked(*marked, fdims.meshRelocation, Posward);
      ASSERT_TRUE(marker);
      EXPECT_EQ(h.arena.textOf(*marker), "cell");
    }
  }
  EXPECT_TRUE(nameRecords.contains({"inner", "call"}));
  EXPECT_TRUE(nameRecords.contains({"inner", "run"}));
  EXPECT_GT(refRecords, 0U);

  // +d.recipe: literal members, name-keyed dim anchors and
  // +d.refFor ref-capture members — the taxonomy reify classifies on.
  bool dimAnchor = false, refCapture = false, data = false;
  for (const auto member : h.chain(outerHead, fdims.recipe)) {
    if (h.arena.linked(member, fdims.refFor, Posward)) {
      refCapture = true;
      continue;
    }
    if (h.arena.linked(member, dims.name, Posward)) {
      dimAnchor = true;
      continue;
    }
    EXPECT_TRUE(h.arena.asInt64(member).has_value());
    data = true;
  }
  EXPECT_TRUE(data);
  EXPECT_TRUE(dimAnchor);
  EXPECT_TRUE(refCapture);
}

// ---- captures ------------------------------------------------------------

TEST(VagentFragmentTest, CaptureSlotJoinsTheBindingCloneRank) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  var count: Int := 5
  def probe(): Int { run(code"count + 1") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const auto *capture = h.capture(*artifacts, "count");
  ASSERT_NE(capture, nullptr);
  EXPECT_TRUE(capture->stable);

  const CellRef countCell = h.arena.makeScalarCell(std::int64_t{5});
  const CompilerBindings bindings{
      {capture->alias, {countCell, Type{"Int", {}}, /*stateSide=*/true}}};
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{6}));

  // §5.3's wing shape: the operand wraps the binding's own cell as an
  // int64 slot appended to the binding's +d.clone rank tail.
  bool cloneSlot = false;
  for (const auto slot :
       zigzag::rankAfter(h.arena, countCell, h.core.dims().clone)) {
    if (h.arena.asInt64(slot) == static_cast<std::int64_t>(countCell))
      cloneSlot = true;
  }
  EXPECT_TRUE(cloneSlot);

  // …and the recipe's ref-capture member pairs the slot with the
  // binding through +d.values — reify's cap$N re-append arm.
  ASSERT_EQ(context.emitted.size(), 1U);
  const auto fdims   = mintFragmentDims(h.core);
  const auto &dims   = h.core.dims();
  bool captureMember = false;
  for (const auto member :
       h.chain(context.emitted.begin()->second.head, fdims.recipe)) {
    const auto slot = h.arena.linked(member, fdims.refFor, Posward);
    if (!slot) continue;
    const auto binding = h.arena.linked(member, dims.values, Posward);
    if (binding && *binding == countCell) captureMember = true;
  }
  EXPECT_TRUE(captureMember);
}

// ---- refusals --------------------------------------------------------------

TEST(VagentFragmentTest, SpliceOnlyFragmentsRefuseEmission) {
  FragmentHarness h;
  // A Code-parameterized literal is legal *as a splice operand* — the
  // pipeline succeeds — but it emits no evaluated fragment object.
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int {
    splice code"p: Code<Int> | splice p"(code"1")
  }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const FragmentKey *spliceOnlyKey = nullptr;
  for (const auto &[key, meta] : artifacts->expansion.fragmentMeta)
    if (meta.spliceOnly) spliceOnlyKey = &key;
  ASSERT_NE(spliceOnlyKey, nullptr);

  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  // The literal's own Expr left the merged tree with the splice
  // substitution — reconstruct the key's identity on a Code node so
  // compileFragment resolves the same record.
  Expr literal;
  literal.kind      = ExprKind::Code;
  literal.loc       = spliceOnlyKey->loc;
  literal.stamp     = ScopeStamp{0, spliceOnlyKey->ordinal};
  const auto object = compileFragment(literal, context, h.compiler);
  ASSERT_FALSE(object);
  EXPECT_EQ(object.error().code, DiagnosticCode::InvalidDeclaration);
  EXPECT_TRUE(object.error().detail.find("splice-only") != std::string::npos)
      << object.error().detail;
}

TEST(VagentFragmentTest, StaticParamRefusesAtEmission) {
  FragmentHarness h;
  // `static` inside a code"…" body is refused by expand before check —
  // the pipeline arm of the same refusal.
  const auto refused = h.pipeline(R"VA(
agent MetaUser {
  def probe(): Int { let c := code"w: Int | static (w)"; 0 }
}
)VA");
  ASSERT_FALSE(refused.has_value());

  // The emission-half arm: a body that reaches compileFragmentBody
  // without the pipeline (graft's constructed bodies) still refuses —
  // parameters are unvalued at the run/graft emission.
  auto operand = Parser("static (w)").parseExpression();
  ASSERT_TRUE(operand);
  const FragmentBodyRequest request{
      .body       = operand->get(),
      .parameters = {Parameter{"w", Type{"Int", {}}, false, {}}},
      .bindings   = nullptr,
      .fragments  = nullptr,
      .use        = ResultUse::Value};
  const auto body = h.compiler.compileFragmentBody(request);
  ASSERT_FALSE(body);
  EXPECT_EQ(body.error().code, DiagnosticCode::InvalidPosition);
}

TEST(VagentFragmentTest, RunWithoutTablesStaysANamedRefusal) {
  FragmentHarness h;
  // A code"…" literal outside the expansion/check tables — the
  // phase-2 refusal wording stays the refusal.
  auto expression = Parser("code\"1 + 2\"").parseExpression();
  ASSERT_TRUE(expression);
  const auto statement = h.compiler.compileStatement(**expression);
  ASSERT_FALSE(statement);
  EXPECT_EQ(statement.error().code, DiagnosticCode::InvalidDeclaration);
}

// ---- program image ---------------------------------------------------------

TEST(VagentFragmentTest, ProgramImageRecordsFloorAndOpEnumeration) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"1 + 2") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  EXPECT_TRUE(h.compiler.programOps().empty());
  // No emission yet — the image record is absent, not an empty rank.
  EXPECT_FALSE(finalizeProgramImage(h.compiler).has_value());
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  ASSERT_FALSE(h.compiler.programOps().empty());

  const auto record = finalizeProgramImage(h.compiler);
  ASSERT_TRUE(record.has_value());
  const auto floor = h.fieldValue(*record, "floor");
  ASSERT_TRUE(floor);
  EXPECT_EQ(h.arena.asInt64(*floor).value_or(-1),
            static_cast<std::int64_t>(h.compiler.imageFloor()));
  const auto opsHead = h.fieldValue(*record, "ops");
  ASSERT_TRUE(opsHead);
  std::vector<CellRef> positions;
  for (const auto cell :
       zigzag::rankAfter(h.arena, *opsHead, h.core.dims().step)) {
    const auto position = h.arena.asInt64(cell);
    ASSERT_TRUE(position);
    positions.push_back(static_cast<CellRef>(*position));
  }
  EXPECT_EQ(positions, h.compiler.programOps());
  for (const auto op : positions)
    EXPECT_TRUE(h.vm.getOpcodeKind(op).has_value());
}

// ---- the sibling's readers against the emitted object ----------------------

TEST(VagentFragmentTest, MetaRuntimeRunsNullaryAndReadsDescriptor) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"1 + 2") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  ASSERT_EQ(context.emitted.size(), 1U);
  const CellRef head = context.emitted.begin()->second.head;

  const auto binds = mintMetaBindSet(
      h.arena, h.core, h.arena.makeCell("program"), h.arena.makeCell("agent"));
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime runtime{h.core, h.vm, *binds};

  // The sibling's reader: descriptor fields, parameter cells and the
  // head-vs-op discriminator all read the emitted object.
  const auto outcome = runtime.runFragment(head, {});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None) << outcome.detail;
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(*outcome.result, CellValue(std::int64_t{3}));

  // Headless-run discrimination: the bare entry op is not a head.
  const auto entryOp = context.emitted.begin()->second.entry;
  EXPECT_EQ(runtime.runFragment(entryOp, {}).refusal,
            MetaRunOutcome::Refusal::NotAHead);
}

TEST(VagentFragmentTest, RunFragmentBindsPositionalArgsOnTheCursorVars) {
  FragmentHarness h;
  // The engine-level call convention: #RESOLVE walks the cursor's
  // +d.vars rank, so positional args must land there — not on the
  // pushed frame cell — in #BIND's prepend order. 10-3=7 pins it.
  auto artifacts = h.pipeline(R"(
import std:meta
agent MetaUser {
  def probe(): Int { run(code"w: Int, v: Int | w - v", [10, 3]) }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  ASSERT_EQ(context.emitted.size(), 1U);
  const CellRef head = context.emitted.begin()->second.head;

  const auto binds = mintMetaBindSet(
      h.arena, h.core, h.arena.makeCell("program"), h.arena.makeCell("agent"));
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime runtime{h.core, h.vm, *binds};

  const auto outcome =
      runtime.runFragment(head, {CellValue(std::int64_t{10}),
                                 CellValue(std::int64_t{3})});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None) << outcome.detail;
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(*outcome.result, CellValue(std::int64_t{7}));
}

TEST(VagentFragmentTest, ReifyReplaysTheEmittedRecipe) {
  FragmentHarness h;
  auto artifacts = h.pipeline(R"(
module demo
import std:meta
agent MetaUser {
  def probe(): Int { run(code"#inc 1 + 2") }
}
)");
  ASSERT_TRUE(artifacts) << describe(artifacts.error());
  const auto *body = FragmentHarness::firstMethodBody(artifacts->program);
  ASSERT_NE(body, nullptr);
  const CompilerBindings bindings;
  auto context = h.contextOf(*artifacts, bindings);
  auto statement =
      h.compiler.compileStatement(*body, bindings, ResultUse::Value, &context);
  ASSERT_TRUE(statement) << formatDiagnostic(statement.error());
  ASSERT_EQ(context.emitted.size(), 1U);
  const CellRef head = context.emitted.begin()->second.head;

  const auto binds = mintMetaBindSet(
      h.arena, h.core, h.arena.makeCell("program"), h.arena.makeCell("agent"));
  ASSERT_TRUE(binds) << binds.error();
  MetaRuntime runtime{h.core, h.vm, *binds};

  const auto copy = runtime.reify(head);
  ASSERT_TRUE(copy) << copy.error();
  const auto label = runtime.labelOf(*copy);
  ASSERT_TRUE(label);
  EXPECT_TRUE(label->starts_with("reify:")) << *label;
  EXPECT_NE(label->find("inc"), std::string::npos) << *label;
  // The copy is a self-describing head: own +d.entry reach, its own
  // regenerated +d.refs and +d.recipe chains.
  const auto fdims = mintFragmentDims(h.core);
  EXPECT_TRUE(h.arena.linked(*copy, fdims.entry, Posward).has_value());
  EXPECT_TRUE(h.arena.linked(*copy, fdims.refs, Posward).has_value());
  EXPECT_TRUE(h.arena.linked(*copy, fdims.recipe, Posward).has_value());

  // …and it runs: the copied body graph is a runnable fragment, not a
  // bare wrapper.
  const auto outcome = runtime.runFragment(*copy, {});
  EXPECT_EQ(outcome.refusal, MetaRunOutcome::Refusal::None) << outcome.detail;
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(*outcome.result, CellValue(std::int64_t{3}));
}

} // namespace
