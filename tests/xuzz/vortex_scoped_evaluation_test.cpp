#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
namespace zigzag::vortex {
namespace {
class ScopedEvaluationTest : public testing::Test {
protected:
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  static constexpr ScopedEvaluationLimits limits{65536, 1048576, 32,
                                                 1024,  65536,   32};
  ScopeCodec codec{core, limits};
  CellRef argX{}, argY{}, resultX{}, frameReg{}, args{}, dest{}, invoke{},
      cursor{};
  CellRef add{}, jump{}, completion{}, halt{};
  CellRef handle(CellRef target) {
    auto h = arena.makeCell();
    EXPECT_TRUE(arena.setValueBits(h, xanadu::ValueKind::OpHandle, target));
    auto marker = arena.makeCell(xanadu::cellRelocationMarker);
    EXPECT_TRUE(
        arena.link(h, arena.ensureDimension(xanadu::cellRelocationDimension),
                   DimVector::POS, marker));
    return h;
  }
  void wing(CellRef op, DimVector side,
            std::initializer_list<CellRef> targets) {
    auto tail  = op;
    bool first = true;
    for (auto target : targets) {
      auto slot = handle(target);
      EXPECT_TRUE(arena.link(tail, first ? core.dims().grab : core.dims().step,
                             first ? side : DimVector::POS, slot));
      tail  = slot;
      first = false;
    }
  }
  CellRef list(const std::vector<CellRef> &items) {
    auto root  = arena.makeCell("[]");
    auto tail  = root;
    bool first = true;
    for (auto item : items) {
      EXPECT_TRUE(arena.link(tail, first ? core.dims().grab : core.dims().step,
                             DimVector::POS, item));
      tail  = item;
      first = false;
    }
    return root;
  }
  CellRef schema(std::initializer_list<std::string_view> names) {
    std::vector<CellRef> fields;
    for (auto name : names)
      fields.push_back(codec.record(
          "ScopeFieldSchema",
          {{"name", arena.makeCell(name)}, {"kind", arena.makeCell("Float")}}));
    return codec.record("ScopeRecordSchema", {{"fields", list(fields)}});
  }
  CellRef binding(CellRef reg, std::string_view target) {
    return codec.record("ScopeBinding", {{"register", handle(reg)},
                                         {"target", arena.makeCell(target)},
                                         {"kind", arena.makeCell("Float")}});
  }
  CellRef descriptor(std::vector<CellRef> graph) {
    std::vector<CellRef> graphHandles;
    for (auto op : graph) graphHandles.push_back(handle(op));
    return codec.record(
        "epistemic-scope-1",
        {{"args_schema", schema({"x", "y"})},
         {"result_schema", schema({"x"})},
         {"effect_graph", list(graphHandles)},
         {"bindings", list({binding(argX, "arg.x"), binding(argY, "arg.y"),
                            binding(resultX, "result.x")})}});
  }
  void setup(OpcodeKind body = OpcodeKind::Add) {
    ASSERT_TRUE(vm.setScopedEvaluationLimits(limits));
    argX     = handle(arena.makeCell("private.x"));
    argY     = handle(arena.makeCell("private.y"));
    resultX  = handle(arena.makeCell("private.result"));
    frameReg = handle(arena.makeCell("private.frame"));
    args     = codec.record("Arguments", {{"x", arena.makeScalarCell(2.0)},
                                          {"y", arena.makeScalarCell(3.0)}});
    core.pin("scope.args", args);
    dest = codec.record("Destination", {{"outcome", arena.makeCell("Empty")}});
    add  = vm.mintOpcode(body);
    jump = vm.mintOpcode(OpcodeKind::Jump);
    completion = vm.mintOpcode(OpcodeKind::Nop);
    halt       = vm.mintOpcode(OpcodeKind::Halt);
    if (body != OpcodeKind::Fail && body != OpcodeKind::Return) {
      wing(add, DimVector::POS, {argX, argY});
      wing(add, DimVector::NEG, {resultX});
      EXPECT_TRUE(arena.link(add, core.dims().spin, DimVector::POS, jump));
    }
    wing(jump, DimVector::POS, {completion});
    auto desc = descriptor({add, jump, completion});
    invoke    = vm.mintOpcode(OpcodeKind::ScopedEvaluate);
    wing(invoke, DimVector::POS, {add, completion, args, desc, frameReg});
    wing(invoke, DimVector::NEG, {dest});
    EXPECT_TRUE(arena.link(invoke, core.dims().spin, DimVector::POS, halt));
    cursor = vm.spawnCursor(invoke);
  }
  void selectEntryAndGraph(CellRef entry, const std::vector<CellRef> &graph) {
    const auto invocation = codec.wing(invoke, DimVector::POS);
    const auto fields     = codec.fields(invocation[3]);
    std::vector<CellRef> handles;
    for (auto op : graph) handles.push_back(handle(op));
    codec.replaceRecord(invocation[3],
                        {{"args_schema", fields.at("args_schema")},
                         {"result_schema", fields.at("result_schema")},
                         {"bindings", fields.at("bindings")},
                         {"effect_graph", list(handles)}});
    const auto first = core.readLink(invoke, core.dims().grab, DimVector::POS);
    EXPECT_TRUE(first);
    EXPECT_TRUE(arena.setValueBits(*first, xanadu::ValueKind::OpHandle, entry));
  }
  void setupChoiceRecovery() {
    setup(OpcodeKind::Fail);
    const auto alternative = vm.mintOpcode(OpcodeKind::Add);
    wing(alternative, DimVector::POS, {argX, argY});
    wing(alternative, DimVector::NEG, {resultX});
    EXPECT_TRUE(
        arena.link(alternative, core.dims().spin, DimVector::POS, jump));
    const auto choose = vm.mintOpcode(OpcodeKind::Choice);
    wing(choose, DimVector::POS, {alternative});
    EXPECT_TRUE(arena.link(choose, core.dims().spin, DimVector::POS, add));
    selectEntryAndGraph(choose, {choose, add, alternative, jump, completion});
  }
  CellRef setupPinned(bool pinSource = true) {
    setup();
    const auto source = arena.makeCell("caller observation");
    if (pinSource) core.pin("observation", source);
    codec.replaceRecord(args, {{"x", handle(source)}});
    auto pinnedSchema = [&] {
      auto field = codec.record("ScopeFieldSchema",
                                {{"name", arena.makeCell("x")},
                                 {"kind", arena.makeCell("PinnedRef")},
                                 {"pinned_root", arena.makeCell("x")}});
      return codec.record("ScopeRecordSchema", {{"fields", list({field})}});
    };
    auto bind = [&](CellRef reg, std::string_view target) {
      return codec.record("ScopeBinding",
                          {{"register", handle(reg)},
                           {"target", arena.makeCell(target)},
                           {"kind", arena.makeCell("PinnedRef")}});
    };
    const auto invocation = codec.wing(invoke, DimVector::POS);
    codec.replaceRecord(
        invocation[3],
        {{"args_schema", pinnedSchema()},
         {"result_schema", pinnedSchema()},
         {"effect_graph", list({handle(jump), handle(completion)})},
         {"bindings", list({bind(argX, "arg.x"), bind(resultX, "result.x")})}});
    const auto entry = core.readLink(invoke, core.dims().grab, DimVector::POS);
    EXPECT_TRUE(entry);
    EXPECT_TRUE(arena.setValueBits(*entry, xanadu::ValueKind::OpHandle, jump));
    return source;
  }

public:
  struct NestedGraph {
    CellRef outer;
    CellRef nested;
    CellRef parentFrame;
    CellRef childFrame;
  };

protected:
  NestedGraph setupNested(bool strings = false, std::size_t copies = 1,
                          bool loop = false, bool childChoice = false,
                          bool childHalts = false) {
    setup();
    const auto childDescriptor = codec.wing(invoke, DimVector::POS)[3];
    auto childEntry            = add;
    const auto childJump       = jump;
    const auto childCompletion = completion;
    const auto childFrame      = frameReg;
    auto field                 = [&](std::string_view name) {
      std::vector<std::pair<std::string, CellRef>> values{
                          {"name", arena.makeCell(name)},
                          {"kind", arena.makeCell(strings ? "Str" : "Float")}};
      if (strings)
        values.emplace_back("max_bytes",
                                            arena.makeScalarCell(std::int64_t{512}));
      return codec.record("ScopeFieldSchema", values);
    };
    auto bind = [&](CellRef reg, std::string_view target,
                    std::string_view kind) {
      return codec.record("ScopeBinding", {{"register", handle(reg)},
                                           {"target", arena.makeCell(target)},
                                           {"kind", arena.makeCell(kind)}});
    };
    const auto kind = strings ? "Str" : "Float";
    if (strings) {
      auto childFields = codec.fields(childDescriptor);
      codec.replaceRecord(
          childDescriptor,
          {{"args_schema",
            codec.record("ScopeRecordSchema",
                         {{"fields", list({field("x"), field("y")})}})},
           {"result_schema", codec.record("ScopeRecordSchema",
                                          {{"fields", list({field("x")})}})},
           {"effect_graph", childFields.at("effect_graph")},
           {"bindings",
            list({bind(argX, "arg.x", kind), bind(argY, "arg.y", kind),
                  bind(resultX, "result.x", kind)})}});
      codec.replaceRecord(args, {{"x", arena.makeCell(std::string(512, 's'))},
                                 {"y", arena.makeCell("")}});
    }
    std::vector<CellRef> childGraph{childEntry};
    if (childChoice) {
      // The child leaves an open choice at completion; release must close it.
      const auto choose = vm.mintOpcode(OpcodeKind::Choice);
      wing(choose, DimVector::POS, {add});
      EXPECT_TRUE(arena.link(choose, core.dims().spin, DimVector::POS, add));
      childGraph.insert(childGraph.begin(), choose);
      childEntry = choose;
    }
    auto childTail = add;
    for (std::size_t i = 1; i < copies; ++i) {
      auto next = vm.mintOpcode(OpcodeKind::Add);
      wing(next, DimVector::POS, {argX, argY});
      wing(next, DimVector::NEG, {resultX});
      EXPECT_TRUE(
          arena.link(childTail, core.dims().spin, DimVector::POS, next));
      childTail = next;
      childGraph.push_back(next);
    }
    if (childHalts) {
      // Halt is never a scoped completion; the verified Jump stays unreachable.
      const auto childHalt = vm.mintOpcode(OpcodeKind::Halt);
      EXPECT_TRUE(
          arena.link(childTail, core.dims().spin, DimVector::POS, childHalt));
      childGraph.push_back(childHalt);
    } else {
      EXPECT_TRUE(
          arena.link(childTail, core.dims().spin, DimVector::POS, childJump));
    }
    childGraph.push_back(childJump);
    childGraph.push_back(childCompletion);
    std::vector<CellRef> childHandles;
    for (auto op : childGraph) childHandles.push_back(handle(op));
    auto childFields = codec.fields(childDescriptor);
    codec.replaceRecord(childDescriptor,
                        {{"args_schema", childFields.at("args_schema")},
                         {"result_schema", childFields.at("result_schema")},
                         {"bindings", childFields.at("bindings")},
                         {"effect_graph", list(childHandles)}});
    auto parentArg             = handle(arena.makeCell("parent.arg"));
    auto parentResult          = handle(arena.makeCell("parent.result"));
    auto parentFrame           = handle(arena.makeCell("parent.frame"));
    const auto outerJump       = vm.mintOpcode(OpcodeKind::Jump);
    const auto outerCompletion = vm.mintOpcode(OpcodeKind::Nop);
    wing(outerJump, DimVector::POS, {outerCompletion});
    const auto nested = vm.mintOpcode(OpcodeKind::ScopedEvaluate);
    const std::vector<CellRef> slots{
        handle(childEntry), handle(childCompletion), parentArg,
        handle(childDescriptor), handle(childFrame)};
    auto tail  = nested;
    bool first = true;
    for (auto slot : slots) {
      EXPECT_TRUE(arena.link(tail, first ? core.dims().grab : core.dims().step,
                             DimVector::POS, slot));
      first = false;
      tail  = slot;
    }
    EXPECT_TRUE(
        arena.link(nested, core.dims().grab, DimVector::NEG, parentResult));
    EXPECT_TRUE(arena.link(nested, core.dims().spin, DimVector::POS,
                           loop ? nested : outerJump));
    auto recordField = [&](std::string_view name, std::vector<CellRef> fields) {
      return codec.record("ScopeFieldSchema",
                          {{"name", arena.makeCell(name)},
                           {"kind", arena.makeCell("Record")},
                           {"fields", list(fields)}});
    };
    auto status =
        codec.record("ScopeFieldSchema",
                     {{"name", arena.makeCell("outcome")},
                      {"kind", arena.makeCell("Str")},
                      {"max_bytes", arena.makeScalarCell(std::int64_t{64})}});
    auto argsSchema = codec.record(
        "ScopeRecordSchema",
        {{"fields", list({recordField("nested", {field("x"), field("y")})})}});
    auto resultSchema = codec.record(
        "ScopeRecordSchema",
        {{"fields",
          list({recordField(
              "query", {status, recordField("payload", {field("x")})})})}});
    std::vector<CellRef> parentHandles{handle(nested)};
    for (auto op : childGraph) parentHandles.push_back(handle(op));
    parentHandles.push_back(handle(outerJump));
    parentHandles.push_back(handle(outerCompletion));
    auto parentDescriptor = codec.record(
        "epistemic-scope-1",
        {{"args_schema", argsSchema},
         {"result_schema", resultSchema},
         {"effect_graph", list(parentHandles)},
         {"bindings", list({bind(parentArg, "arg.nested", "Record"),
                            bind(parentResult, "result.query", "Record"),
                            bind(argX, "temp.childX", kind),
                            bind(argY, "temp.childY", kind),
                            bind(resultX, "temp.childResult", kind)})}});
    auto parentArgs = codec.record("ParentArgs", {{"nested", args}});
    core.pin("parent.args", parentArgs);
    auto outer = vm.mintOpcode(OpcodeKind::ScopedEvaluate);
    wing(outer, DimVector::POS,
         {nested, outerCompletion, parentArgs, parentDescriptor, parentFrame});
    wing(outer, DimVector::NEG, {dest});
    EXPECT_TRUE(arena.link(outer, core.dims().spin, DimVector::POS, halt));
    vm.setCursorOpcode(cursor, outer);
    return {outer, nested, parentFrame, childFrame};
  }
  std::string outcome() {
    return arena.textOf(codec.fields(dest).at("outcome"));
  }
};
TEST_F(ScopedEvaluationTest, SameCursorExportsTypedValueAfterRelease) {
  setup();
  const auto priorFrame = arena.handleTarget(frameReg);
  const auto priorX     = arena.handleTarget(argX);
  const auto marks      = arena.outstandingMarks();
  auto r                = vm.run(cursor);
  ASSERT_TRUE(r.success) << r.errorMessage;
  EXPECT_FALSE(r.scopedRefusal);
  EXPECT_EQ(vm.activeCursors().size(), 1u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(arena.handleTarget(frameReg), priorFrame);
  EXPECT_EQ(arena.handleTarget(argX), priorX);
  EXPECT_EQ(arena.outstandingMarks(), marks);
  EXPECT_EQ(vm.choiceDepth(), 0u);
  ASSERT_EQ(outcome(), "Value");
  auto payload = codec.fields(dest).at("payload");
  EXPECT_EQ(arena.asDouble(codec.fields(payload).at("x")), 5.0);
  EXPECT_FALSE(codec.fields(dest).contains("error"));
}
TEST_F(ScopedEvaluationTest, RootFailIsEmptyAndPreservesCallerChoice) {
  setup(OpcodeKind::Fail);
  vm.pushChoicePoint(cursor, halt);
  const auto marks = arena.outstandingMarks();
  ASSERT_TRUE(vm.run(cursor).success);
  EXPECT_EQ(outcome(), "Empty");
  EXPECT_EQ(vm.choiceDepth(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), marks);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
  EXPECT_FALSE(codec.fields(dest).contains("error"));
  EXPECT_TRUE(vm.backtrack(cursor));
}
TEST_F(ScopedEvaluationTest, RootReturnRefusesAndRestoresContinuation) {
  setup(OpcodeKind::Return);
  auto r = vm.run(cursor);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::InvalidScopedCompletion);
  EXPECT_EQ(outcome(), "Refused");
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_FALSE(core.readLink(cursor, core.dims().stack, DimVector::POS));
}
TEST_F(ScopedEvaluationTest, RefusesUnmarkedReferenceBeforeEntry) {
  setup();
  auto input = core.readLink(invoke, core.dims().grab, DimVector::POS);
  ASSERT_TRUE(input);
  EXPECT_TRUE(arena.setValueBits(*input, xanadu::ValueKind::Int64, add));
  const auto cells = arena.cellCount();
  auto r           = vm.step(cursor);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::InvalidOperands);
  EXPECT_EQ(vm.getCursorOpcode(cursor), invoke);
  EXPECT_EQ(arena.cellCount(), cells);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(outcome(), "Empty");
}
TEST_F(ScopedEvaluationTest, ImpureManifestRefusesBeforeMark) {
  setup(OpcodeKind::Link);
  const auto cells = arena.cellCount();
  auto r           = vm.step(cursor);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::InvalidGraph);
  EXPECT_EQ(arena.cellCount(), cells);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, PublicSchedulerCutStealCannotConsumeScope) {
  setup();
  vm.pushChoicePoint(cursor, halt);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto marks = arena.outstandingMarks();
  vm.cut();
  EXPECT_FALSE(vm.stealNearRootChoicePoint());
  EXPECT_EQ(vm.stepScheduler(), 0u);
  EXPECT_EQ(vm.choiceDepth(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), marks);
  ASSERT_TRUE(vm.run(cursor).success);
  EXPECT_EQ(vm.choiceDepth(), 1u);
  vm.clearChoicePoints();
}
TEST_F(ScopedEvaluationTest, AnotherCursorCannotDriveLiveScope) {
  setup();
  auto other = vm.spawnCursor(halt);
  ASSERT_TRUE(vm.step(cursor).success);
  auto r = vm.step(other);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::CursorOwnership);
  ASSERT_TRUE(vm.run(cursor).success);
  EXPECT_EQ(outcome(), "Value");
}
TEST_F(ScopedEvaluationTest, RunCycleLimitClosesScopeAndPreservesCallerChoice) {
  setup();
  vm.pushChoicePoint(cursor, halt);
  const auto priorFrame = arena.handleTarget(frameReg);
  auto r                = vm.run(cursor, 1);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(arena.handleTarget(frameReg), priorFrame);
  EXPECT_EQ(arena.outstandingMarks(), 1u);
  EXPECT_EQ(vm.choiceDepth(), 1u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(outcome(), "Refused");
  vm.clearChoicePoints();
}
TEST_F(ScopedEvaluationTest, CancelledRunUnwindsLiveContinuation) {
  setup();
  ASSERT_TRUE(vm.step(cursor).success);
  std::stop_source stop;
  stop.request_stop();
  auto r = vm.run(cursor, 100, stop.get_token());
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::Cancelled);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
}
TEST_F(ScopedEvaluationTest, PublicationFailureRestoresWholeDestination) {
  setup();
  const auto before = codec.fields(dest);
  const auto cells  = arena.cellCount();
  arena.setAllocationLimitForTesting(static_cast<std::uint32_t>(cells));
  auto r = vm.run(cursor);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(codec.fields(dest), before);
  EXPECT_EQ(outcome(), "Empty");
  EXPECT_EQ(arena.cellCount(), cells);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, RedirectedWriteHandleCannotTouchCallerState) {
  setup();
  auto callerState = arena.makeScalarCell(99.0);
  ASSERT_TRUE(vm.step(cursor).success);
  ASSERT_TRUE(
      arena.setValueBits(resultX, xanadu::ValueKind::OpHandle, callerState));
  auto r = vm.step(cursor);
  EXPECT_FALSE(r.success);
  EXPECT_EQ(r.scopedRefusal, ScopedRefusal::InvalidGraph);
  EXPECT_EQ(arena.asDouble(callerState), 99.0);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, InvalidHostGrantDoesNotEnableScopes) {
  VortexVM ungranted(core);
  EXPECT_FALSE(ungranted.setScopedEvaluationLimits({}));
  setup();
  auto result = ungranted.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, StablePrimediaSpansRetainTheirIdentity) {
  ScopeSchema str;
  str.root().type  = ScopeType::Str;
  str.root().limit = limits.bytes;
  const xanadu::PrimediaSpan stable{xanadu::localScroll, 17, 4};
  auto source = arena.makeCell();
  ASSERT_TRUE(arena.setContent(source, std::span{&stable, 1}));
  const auto mark = arena.mark();
  ScopeCodec snapshotter(core, limits, mark.scratchSize);
  auto value = snapshotter.snapshot(source, str);
  arena.release(mark);
  auto copy = codec.materialize(value, str);
  ASSERT_EQ(arena.contentOf(copy).size(), 1u);
  EXPECT_EQ(arena.contentOf(copy)[0].scroll, stable.scroll);
  EXPECT_EQ(arena.contentOf(copy)[0].start, stable.start);
  EXPECT_EQ(arena.contentOf(copy)[0].length, stable.length);
}
TEST_F(ScopedEvaluationTest, PreMarkScratchKeepsItsSpanAndInnerTextSurvives) {
  ScopeSchema str;
  str.root().type    = ScopeType::Str;
  str.root().limit   = limits.bytes;
  auto old           = arena.makeCell("caller text");
  const auto oldSpan = arena.contentOf(old)[0];
  const auto mark    = arena.mark();
  auto inner         = arena.makeCell("inner text");
  ScopeCodec snapshotter(core, limits, mark.scratchSize);
  auto oldValue   = snapshotter.snapshot(old, str);
  auto innerValue = snapshotter.snapshot(inner, str);
  arena.release(mark);
  auto oldCopy   = codec.materialize(oldValue, str);
  auto innerCopy = codec.materialize(innerValue, str);
  ASSERT_EQ(arena.contentOf(oldCopy).size(), 1u);
  EXPECT_EQ(arena.contentOf(oldCopy)[0].start, oldSpan.start);
  EXPECT_EQ(arena.contentOf(oldCopy)[0].length, oldSpan.length);
  EXPECT_EQ(arena.textOf(innerCopy), "inner text");
}
TEST_F(ScopedEvaluationTest, DirectCompletionPcCannotSkipRootJump) {
  setup();
  ASSERT_TRUE(vm.step(cursor).success);
  vm.setCursorOpcode(cursor, completion);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidScopedCompletion);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, CrossCursorBacktrackCannotConsumeScopedChoice) {
  setup();
  auto other = vm.spawnCursor(halt);
  ASSERT_TRUE(vm.step(cursor).success);
  vm.pushChoicePoint(cursor, jump);
  auto marks = arena.outstandingMarks();
  auto depth = vm.choiceDepth();
  EXPECT_FALSE(vm.backtrack(other));
  EXPECT_EQ(vm.choiceDepth(), depth);
  EXPECT_EQ(arena.outstandingMarks(), marks);
  ASSERT_TRUE(vm.run(cursor).success);
}
class ScopeTestReader final : public xanadu::SpanReader {
public:
  std::string read(const xanadu::PrimediaSpan &span) const override {
    if (span.start > text.size() || span.length > text.size() - span.start)
      throw std::out_of_range("test source span");
    return text.substr(static_cast<std::size_t>(span.start),
                       static_cast<std::size_t>(span.length));
  }
  std::string text{"source"};
};
TEST_F(ScopedEvaluationTest, ContentNamespaceAccessorCoversEverySourceShape) {
  auto native     = arena.makeCell("native");
  auto space      = arena.attach(Space{.label = "storeless"});
  auto proxy      = arena.proxyFor(space, 42);
  auto occurrence = arena.quoteOccurrence(1, proxy);
  const xanadu::PrimediaSpan stable{xanadu::localScroll, 0, 2};
  auto quote         = arena.makeQuote(space, std::span{&stable, 1});
  const auto scratch = arena.intern("!");
  const std::array mixed{stable, scratch};
  auto mixedQuote = arena.makeQuote(space, mixed);
  EXPECT_FALSE(arena.contentSpaceOf(native));
  EXPECT_FALSE(arena.contentSpaceOf(noCell));
  EXPECT_EQ(arena.contentSpaceOf(proxy), space);
  EXPECT_EQ(arena.contentSpaceOf(occurrence), space);
  EXPECT_EQ(arena.contentSpaceOf(quote), space);
  EXPECT_EQ(arena.contentSpaceOf(mixedQuote), space);
}
TEST_F(ScopedEvaluationTest, StorelessMixedQuotationKeepsReaderAndStableSpan) {
  ScopeTestReader reader;
  auto space = arena.attach(Space{.reader = &reader, .label = "reader"});
  const xanadu::PrimediaSpan stable{xanadu::localScroll, 0, 2};
  const auto mark    = arena.mark();
  const auto scratch = arena.intern("!");
  const std::array spans{stable, scratch};
  auto source = arena.makeQuote(space, spans);
  ScopeSchema str;
  str.root().type  = ScopeType::Str;
  str.root().limit = limits.bytes;
  ScopeCodec snapshotter(core, limits, mark.scratchSize);
  auto value = snapshotter.snapshot(source, str);
  arena.release(mark);
  auto result = codec.materialize(value, str);
  EXPECT_EQ(arena.contentSpaceOf(result), space);
  ASSERT_EQ(arena.contentOf(result).size(), spans.size());
  EXPECT_EQ(arena.contentOf(result)[0].start, stable.start);
  EXPECT_EQ(arena.contentOf(result)[0].length, stable.length);
  EXPECT_EQ(arena.textOf(result), "so!");
}
TEST_F(ScopedEvaluationTest, QuotationWithoutReaderRefusesIdentityLoss) {
  auto space = arena.attach(Space{.label = "unreadable"});
  const xanadu::PrimediaSpan stable{xanadu::localScroll, 0, 2};
  auto source = arena.makeQuote(space, std::span{&stable, 1});
  ScopeSchema str;
  str.root().type  = ScopeType::Str;
  str.root().limit = limits.bytes;
  try {
    (void)codec.snapshot(source, str);
    FAIL() << "Unreadable source namespace accepted";
  } catch (const ScopeError &error) {
    EXPECT_EQ(error.code, ScopedRefusal::TypeMismatch);
  }
}
TEST_F(ScopedEvaluationTest, OrdinaryCallReturnUsesSameCursorAndRestoresStack) {
  setup();
  const auto call = vm.mintOpcode(OpcodeKind::Call);
  const auto ret  = vm.mintOpcode(OpcodeKind::Return);
  wing(call, DimVector::POS, {add});
  ASSERT_TRUE(arena.link(call, core.dims().spin, DimVector::POS, jump));
  ASSERT_TRUE(arena.link(add, core.dims().spin, DimVector::POS, ret));
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  auto graph = list({handle(call), handle(add), handle(ret), handle(jump),
                     handle(completion)});
  codec.replaceRecord(invocation[3],
                      {{"args_schema", descriptorFields.at("args_schema")},
                       {"result_schema", descriptorFields.at("result_schema")},
                       {"bindings", descriptorFields.at("bindings")},
                       {"effect_graph", graph}});
  const auto entrySlot =
      core.readLink(invoke, core.dims().grab, DimVector::POS);
  ASSERT_TRUE(entrySlot);
  ASSERT_TRUE(
      arena.setValueBits(*entrySlot, xanadu::ValueKind::OpHandle, call));
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(outcome(), "Value");
  EXPECT_EQ(vm.activeCursors().size(), 1u);
  EXPECT_FALSE(core.readLink(cursor, core.dims().stack, DimVector::POS));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, LocalChoiceFailureCannotConsumeCallerChoice) {
  setupChoiceRecovery();
  vm.pushChoicePoint(cursor, halt);
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(outcome(), "Value");
  EXPECT_EQ(vm.choiceDepth(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 1u);
  EXPECT_TRUE(vm.backtrack(cursor));
}
TEST_F(ScopedEvaluationTest, HaltIsInvalidCompletionAndNoPcIsRefused) {
  setup(OpcodeKind::Halt);
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidScopedCompletion);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  vm.setCursorOpcode(cursor, invoke);
  ASSERT_TRUE(vm.step(cursor).success);
  vm.setCursorOpcode(cursor, std::nullopt);
  result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidScopedCompletion);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, CancellationCannotBeDrivenByAnotherCursor) {
  setup();
  const auto other = vm.spawnCursor(halt);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto marks = arena.outstandingMarks();
  std::stop_source stop;
  stop.request_stop();
  auto result = vm.run(other, 100, stop.get_token());
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::CursorOwnership);
  EXPECT_EQ(arena.outstandingMarks(), marks);
  ASSERT_TRUE(vm.run(cursor).success);
}
TEST_F(ScopedEvaluationTest, NestedScopeRestoresBindingsAndExportsBothFrames) {
  const auto graph          = setupNested();
  const auto oldParentFrame = arena.handleTarget(graph.parentFrame);
  const auto oldChildFrame  = arena.handleTarget(graph.childFrame);
  const auto oldChildX      = arena.handleTarget(argX);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto outerFrame = arena.handleTarget(graph.parentFrame);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto innerFrame = arena.handleTarget(graph.childFrame);
  EXPECT_NE(innerFrame, outerFrame);
  EXPECT_EQ(arena.outstandingMarks(), 2u);
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(vm.activeCursors().size(), 1u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(arena.handleTarget(graph.parentFrame), oldParentFrame);
  EXPECT_EQ(arena.handleTarget(graph.childFrame), oldChildFrame);
  EXPECT_EQ(arena.handleTarget(argX), oldChildX);
  auto payload = codec.fields(dest).at("payload");
  auto query   = codec.fields(payload).at("query");
  EXPECT_EQ(arena.textOf(codec.fields(query).at("outcome")), "Value");
  auto childPayload = codec.fields(query).at("payload");
  EXPECT_EQ(arena.asDouble(codec.fields(childPayload).at("x")), 5.0);
}
TEST_F(ScopedEvaluationTest, NestedInstructionsConsumeTheParentLimit) {
  const auto graph   = setupNested();
  auto grant         = limits;
  grant.instructions = 5;
  ASSERT_TRUE(vm.setScopedEvaluationLimits(grant));
  const auto before = vm.totalCycles();
  auto result       = vm.run(cursor, 10000);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(vm.totalCycles() - before, 6u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  (void)graph;
}
TEST_F(ScopedEvaluationTest, RepeatedNestedStringsShareOneTrafficByteBudget) {
  constexpr std::size_t copies = 8;
  const auto graph             = setupNested(true, copies, true);
  auto grant                   = limits;
  grant.bytes                  = 81920;
  ASSERT_TRUE(vm.setScopedEvaluationLimits(grant));
  std::size_t nestedEntries = 0;
  ExecutionResult result;
  for (std::size_t i = 0; i < 10000 && result.success; ++i) {
    if (vm.getCursorOpcode(cursor) == graph.nested) ++nestedEntries;
    result = vm.step(cursor);
  }
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_GE(nestedEntries, 2u);
  // Each completed child copies 512 input and 512 output bytes eight times,
  // in addition to all descriptor/copy traffic. New frames cannot reset it.
  EXPECT_LE(nestedEntries, grant.bytes / (copies * 1024));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
}
TEST_F(ScopedEvaluationTest, NestedFramesCannotResetTraversalWork) {
  const auto graph    = setupNested(false, 1, true);
  auto grant          = limits;
  grant.traversalWork = 10000;
  ASSERT_TRUE(vm.setScopedEvaluationLimits(grant));
  const auto before = vm.totalCycles();
  auto result       = vm.run(cursor, 10000);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_GT(vm.totalCycles() - before, 4u);
  EXPECT_LT(vm.totalCycles() - before, 10000u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  (void)graph;
}
TEST_F(ScopedEvaluationTest, ExportedComputedStringSurvivesReleaseAndPromotes) {
  setup();
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  auto textSchema       = [&](std::string_view name) {
    return codec.record(
        "ScopeFieldSchema",
        {{"name", arena.makeCell(name)},
         {"kind", arena.makeCell("Str")},
         {"max_bytes",
          arena.makeScalarCell(static_cast<std::int64_t>(limits.bytes))}});
  };
  auto textBinding = [&](CellRef reg, std::string_view target) {
    return codec.record("ScopeBinding", {{"register", handle(reg)},
                                         {"target", arena.makeCell(target)},
                                         {"kind", arena.makeCell("Str")}});
  };
  auto argSchema =
      codec.record("ScopeRecordSchema",
                   {{"fields", list({textSchema("x"), textSchema("y")})}});
  auto resultSchema =
      codec.record("ScopeRecordSchema", {{"fields", list({textSchema("x")})}});
  codec.replaceRecord(invocation[3],
                      {{"args_schema", argSchema},
                       {"result_schema", resultSchema},
                       {"effect_graph", descriptorFields.at("effect_graph")},
                       {"bindings", list({textBinding(argX, "arg.x"),
                                          textBinding(argY, "arg.y"),
                                          textBinding(resultX, "result.x")})}});
  codec.replaceRecord(args, {{"x", arena.makeCell("Hello")},
                             {"y", arena.makeCell(" scoped world")}});
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(outcome(), "Value");
  auto payload = codec.fields(dest).at("payload");
  auto text    = codec.fields(payload).at("x");
  EXPECT_EQ(arena.textOf(text), "Hello scoped world");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  xanadu::Store store;
  const auto at       = store.sliceGenesis(xanadu::MicroversionId{});
  const auto promoted = zigzag::promote(store, at, arena, text);
  ASSERT_TRUE(promoted);
  const auto manifold   = store.rebuildManifold(promoted->version);
  const auto persistent = promoted->mapping.at(text);
  EXPECT_EQ(manifold.textOf(persistent, store), "Hello scoped world");
  EXPECT_EQ(manifold.refusedOps(), 0u);
  for (const auto &span : manifold.contentOf(persistent))
    EXPECT_NE(span.scroll, xanadu::scratchScroll);
}
TEST_F(ScopedEvaluationTest, NestedDataPipelinesRefuseBeforeScopeMark) {
  setup();
  auto leaf     = codec.fields(args).at("x");
  auto pipeline = vm.mintOpcode(OpcodeKind::Nop);
  ASSERT_TRUE(arena.link(leaf, core.dims().spin, DimVector::POS, pipeline));
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidGraph);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), invoke);
  EXPECT_EQ(outcome(), "Empty");
}
TEST_F(ScopedEvaluationTest, OldDestinationLeafPipelineCannotBeOverwritten) {
  setup();
  auto leaf     = codec.fields(dest).at("outcome");
  auto pipeline = vm.mintOpcode(OpcodeKind::Nop);
  ASSERT_TRUE(arena.link(leaf, core.dims().spin, DimVector::POS, pipeline));
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidGraph);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(arena.textOf(leaf), "Empty");
}
TEST_F(ScopedEvaluationTest, ScratchSpanCrossingFloorRetainsCallerPrefix) {
  ScopeSchema str;
  str.root().type   = ScopeType::Str;
  str.root().limit  = limits.bytes;
  const auto prefix = arena.intern("old");
  const auto mark   = arena.mark();
  (void)arena.intern("new");
  const xanadu::PrimediaSpan crossing{prefix.scroll, prefix.start, 6};
  auto source = arena.makeCell();
  ASSERT_TRUE(arena.setContent(source, std::span{&crossing, 1}));
  ScopeCodec snapshotter(core, limits, mark.scratchSize);
  auto value = snapshotter.snapshot(source, str);
  arena.release(mark);
  auto result = codec.materialize(value, str);
  ASSERT_EQ(arena.contentOf(result).size(), 2u);
  EXPECT_EQ(arena.contentOf(result)[0], prefix);
  EXPECT_EQ(arena.textOf(result), "oldnew");
}
TEST_F(ScopedEvaluationTest,
       TinyActorWorkGrantRefusesBeforeMarkAndDestination) {
  setup();
  auto before        = codec.fields(dest);
  const auto scratch = arena.intern("").start;
  arena.setExecutionLimits(static_cast<std::uint32_t>(arena.cellCount() + 1000),
                           1, static_cast<std::size_t>(scratch) + 1000);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), invoke);
  EXPECT_EQ(arena.textOf(before.at("outcome")), "Empty");
  EXPECT_EQ(arena.remainingExecutionWork(), 0u);
}
TEST_F(ScopedEvaluationTest, TinyActorByteGrantCapsMetadataNativeCopies) {
  setup();
  auto before        = codec.fields(dest);
  const auto scratch = arena.intern("").start;
  arena.setExecutionLimits(static_cast<std::uint32_t>(arena.cellCount() + 1000),
                           limits.traversalWork,
                           static_cast<std::size_t>(scratch) + 1);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(arena.textOf(before.at("outcome")), "Empty");
  EXPECT_EQ(vm.getCursorOpcode(cursor), invoke);
}
TEST_F(ScopedEvaluationTest, InstructionLimitStopsLoopWithoutRunCycleLimit) {
  setup();
  const auto loop = vm.mintOpcode(OpcodeKind::Jump);
  wing(loop, DimVector::POS, {loop});
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  codec.replaceRecord(invocation[3],
                      {{"args_schema", descriptorFields.at("args_schema")},
                       {"result_schema", descriptorFields.at("result_schema")},
                       {"bindings", descriptorFields.at("bindings")},
                       {"effect_graph", list({handle(loop), handle(jump),
                                              handle(completion)})}});
  auto first = core.readLink(invoke, core.dims().grab, DimVector::POS);
  ASSERT_TRUE(first);
  ASSERT_TRUE(arena.setValueBits(*first, xanadu::ValueKind::OpHandle, loop));
  auto small         = limits;
  small.instructions = 2;
  ASSERT_TRUE(vm.setScopedEvaluationLimits(small));
  const auto before = vm.totalCycles();
  auto result       = vm.run(cursor, 10000);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::ResourceLimit);
  // Two admitted callback instructions and one refused attempt.
  EXPECT_EQ(vm.totalCycles() - before, 3u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
}
TEST_F(ScopedEvaluationTest, HugeConfiguredDepthUsesFlatNativeStorage) {
  constexpr std::size_t depth = 10000;
  auto field =
      codec.record("ScopeFieldSchema", {{"name", arena.makeCell("next")},
                                        {"kind", arena.makeCell("Unit")}});
  auto value = arena.makeCell("()");
  // This exceeds ordinary native recursion depth, while the explicit flat
  // node/work grants admit the finite structural tree.
  ScopeCodec builder(
      core, {depth * 100, depth * 256, depth + 2, depth + 2, depth * 100, 1});
  for (std::size_t i = 0; i < depth; ++i) {
    field =
        builder.record("ScopeFieldSchema", {{"name", arena.makeCell("next")},
                                            {"kind", arena.makeCell("Record")},
                                            {"fields", list({field})}});
    value = builder.record("Deep", {{"next", value}});
  }
  auto schemaRoot =
      builder.record("ScopeRecordSchema", {{"fields", list({field})}});
  auto emptySchema =
      builder.record("ScopeRecordSchema", {{"fields", list({})}});
  auto descriptor =
      builder.record("epistemic-scope-1", {{"args_schema", schemaRoot},
                                           {"result_schema", emptySchema},
                                           {"effect_graph", list({})},
                                           {"bindings", list({})}});
  auto argsRoot       = builder.record("Args", {{"next", value}});
  auto grant          = limits;
  grant.depth         = std::numeric_limits<std::size_t>::max();
  grant.fields        = depth + 2;
  grant.traversalWork = depth * 100;
  grant.bytes         = depth * 256;
  ScopeCodec deep(core, grant);
  auto parsed = deep.descriptor(descriptor);
  auto owned  = deep.snapshot(argsRoot, parsed.args);
  EXPECT_EQ(parsed.args.nodes.size(), depth + 2);
  EXPECT_EQ(owned.nodes.size(), depth + 2);
  auto copy = deep.materialize(owned, parsed.args);
  EXPECT_TRUE(arena.contains(copy));
}
TEST_F(ScopedEvaluationTest,
       OracleAndDeweyStateRestoreWithoutCutObserverEffects) {
  setupChoiceRecovery();
  const std::array<std::uint16_t, 1> oracle{1};
  const std::array<std::uint16_t, 2> beforePath{8, 3};
  vm.setOraclePath(oracle)->setCurrentDeweyPath(beforePath);
  std::size_t cutCalls = 0;
  vm.setCutCallback([&](std::string_view) { ++cutCalls; });
  const auto cycles = vm.totalCycles();
  auto result       = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(vm.totalCycles() - cycles, 5u);
  EXPECT_EQ(
      std::vector(vm.currentDeweyPath().begin(), vm.currentDeweyPath().end()),
      std::vector(beforePath.begin(), beforePath.end()));
  EXPECT_EQ(cutCalls, 0u);
  const auto callerChoice = vm.mintOpcode(OpcodeKind::Choice);
  const auto alternative  = vm.mintOpcode(OpcodeKind::Nop);
  const auto ordinary     = vm.mintOpcode(OpcodeKind::Nop);
  core.bindInput(callerChoice,
                 arena.makeScalarCell(static_cast<std::int64_t>(alternative)));
  ASSERT_TRUE(
      arena.link(callerChoice, core.dims().spin, DimVector::POS, ordinary));
  vm.setCursorOpcode(cursor, callerChoice);
  ASSERT_TRUE(vm.step(cursor).success);
  EXPECT_EQ(vm.getCursorOpcode(cursor), alternative);
  EXPECT_EQ(vm.choiceDepth(), 0u);
  EXPECT_EQ(vm.currentDeweyPath().back(), 1u);
  EXPECT_EQ(cutCalls, 0u);
}
TEST_F(ScopedEvaluationTest, CursorlessScopePipelineCannotSucceedAsNoop) {
  auto parameter   = arena.makeScalarCell(2.0);
  auto destination = arena.makeScalarCell(99.0);
  auto body        = vm.mintOpcode(OpcodeKind::Add);
  core.bindInput(body, parameter);
  core.bindOutput(body, destination);
  auto scoped = vm.mintOpcode(OpcodeKind::ScopedEvaluate);
  core.attachPipeline(parameter, scoped);
  auto owner  = vm.spawnCursor(body);
  auto result = vm.step(owner);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidOperands);
  EXPECT_EQ(arena.asDouble(destination), 99.0);
  EXPECT_EQ(vm.getCursorOpcode(owner), body);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, CursorlessScopeContractCannotPassByTruthiness) {
  auto destination = arena.makeScalarCell(99.0);
  auto body        = vm.mintOpcode(OpcodeKind::Add);
  core.bindInput(body, arena.makeScalarCell(2.0));
  core.bindOutput(body, destination);
  auto scoped = vm.mintOpcode(OpcodeKind::ScopedEvaluate);
  core.attachPrecondition(body, scoped);
  auto owner  = vm.spawnCursor(body);
  auto result = vm.step(owner);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidOperands);
  EXPECT_EQ(arena.asDouble(destination), 99.0);
  EXPECT_EQ(vm.getCursorOpcode(owner), body);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, PipelineNumericFaultPropagatesBeforeBodyWrite) {
  auto parameter   = arena.makeScalarCell(-1.0);
  auto destination = arena.makeScalarCell(99.0);
  auto body        = vm.mintOpcode(OpcodeKind::Add);
  core.bindInput(body, parameter);
  core.bindOutput(body, destination);
  auto logarithm = vm.mintOpcode(OpcodeKind::Ln);
  core.attachPipeline(parameter, logarithm);
  auto owner  = vm.spawnCursor(body);
  auto result = vm.step(owner);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.failingClause, logarithm);
  EXPECT_EQ(arena.asDouble(destination), 99.0);
  EXPECT_EQ(vm.getCursorOpcode(owner), body);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, PinnedReferenceDefaultIsNotAnAssignedResult) {
  const auto source = setupPinned();
  auto result       = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_TRUE(arena.contains(source));
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, UnpinnedReferenceRefusesBeforeMark) {
  (void)setupPinned(false);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidOperands);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(outcome(), "Empty");
}
TEST_F(ScopedEvaluationTest, InnerReferenceCannotEscapeAsPinnedResult) {
  (void)setupPinned();
  ASSERT_TRUE(vm.step(cursor).success);
  auto resultCell = arena.handleTarget(resultX);
  ASSERT_TRUE(resultCell);
  auto inner = arena.makeCell("inner observation");
  ASSERT_TRUE(
      arena.setValueBits(*resultCell, xanadu::ValueKind::OpHandle, inner));
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_FALSE(arena.contains(inner));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, DestinationCannotAliasAnExecutableManifestNode) {
  setup();
  vm.setOpcodeKind(dest, OpcodeKind::Nop);
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  codec.replaceRecord(
      invocation[3],
      {{"args_schema", descriptorFields.at("args_schema")},
       {"result_schema", descriptorFields.at("result_schema")},
       {"bindings", descriptorFields.at("bindings")},
       {"effect_graph",
        list({handle(add), handle(jump), handle(completion), handle(dest)})}});
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidGraph);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(outcome(), "Empty");
}
TEST_F(ScopedEvaluationTest, NonfiniteIntermediateCannotBecomeSuccessfulValue) {
  setup();
  codec.replaceRecord(
      args, {{"x", arena.makeScalarCell(std::numeric_limits<double>::max())},
             {"y", arena.makeScalarCell(std::numeric_limits<double>::max())}});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::CallbackFault);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(outcome(), "Refused");
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, ValueCompletionWithoutRequiredWriteRefuses) {
  setup();
  selectEntryAndGraph(jump, {jump, completion});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(outcome(), "Refused");
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, PartialResultCannotPublishUnwrittenZeroField) {
  setup();
  auto invocation = codec.wing(invoke, DimVector::POS);
  auto fields     = codec.fields(invocation[3]);
  codec.replaceRecord(invocation[3],
                      {{"args_schema", fields.at("args_schema")},
                       {"result_schema", schema({"x", "y"})},
                       {"effect_graph", fields.at("effect_graph")},
                       {"bindings", fields.at("bindings")}});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedEvaluationTest, ExplicitlyComputedZeroIsAnAssignedValue) {
  setup();
  codec.replaceRecord(args, {{"x", arena.makeScalarCell(0.0)},
                             {"y", arena.makeScalarCell(0.0)}});
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(outcome(), "Value");
  auto payload = codec.fields(dest).at("payload");
  EXPECT_EQ(arena.asDouble(codec.fields(payload).at("x")), 0.0);
}
TEST_F(ScopedEvaluationTest, UnassignedResultCannotAssignItself) {
  setup();
  ASSERT_TRUE(arena.link(add, core.dims().grab, DimVector::POS, std::nullopt));
  wing(add, DimVector::POS, {resultX});
  ASSERT_TRUE(vm.step(cursor).success);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, FailedChoiceRestoresUnassignedMetadataWithArena) {
  setup();
  auto fail = vm.mintOpcode(OpcodeKind::Fail);
  ASSERT_TRUE(arena.link(add, core.dims().spin, DimVector::POS, fail));
  auto choose = vm.mintOpcode(OpcodeKind::Choice);
  wing(choose, DimVector::POS, {jump});
  ASSERT_TRUE(arena.link(choose, core.dims().spin, DimVector::POS, add));
  selectEntryAndGraph(choose, {choose, add, fail, jump, completion});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, NestedArgumentsCannotReadAnUnassignedResultLeaf) {
  const auto graph    = setupNested();
  auto outer          = codec.wing(graph.outer, DimVector::POS);
  auto outerFields    = codec.fields(outer[3]);
  auto resultFields   = codec.fields(outerFields.at("result_schema"));
  auto existingFields = codec.list(resultFields.at("fields"));
  auto x =
      codec.record("ScopeFieldSchema", {{"name", arena.makeCell("x")},
                                        {"kind", arena.makeCell("Float")}});
  auto pendingField =
      codec.record("ScopeFieldSchema", {{"name", arena.makeCell("arguments")},
                                        {"kind", arena.makeCell("Record")},
                                        {"fields", list({x})}});
  existingFields.push_back(pendingField);
  auto resultSchema =
      codec.record("ScopeRecordSchema", {{"fields", list(existingFields)}});
  auto pendingArgs    = handle(arena.makeCell("private.pending.arguments"));
  auto pendingBinding = codec.record(
      "ScopeBinding", {{"register", handle(pendingArgs)},
                       {"target", arena.makeCell("result.arguments")},
                       {"kind", arena.makeCell("Record")}});
  auto bindings = codec.list(outerFields.at("bindings"));
  bindings.push_back(pendingBinding);
  codec.replaceRecord(outer[3],
                      {{"args_schema", outerFields.at("args_schema")},
                       {"result_schema", resultSchema},
                       {"effect_graph", outerFields.at("effect_graph")},
                       {"bindings", list(bindings)}});
  auto child       = codec.wing(graph.nested, DimVector::POS);
  auto childFields = codec.fields(child[3]);
  codec.replaceRecord(child[3],
                      {{"args_schema", schema({"x"})},
                       {"result_schema", childFields.at("result_schema")},
                       {"effect_graph", childFields.at("effect_graph")},
                       {"bindings", list({binding(argX, "arg.x"),
                                          binding(resultX, "result.x")})}});
  auto nestedSlots =
      core.rawWingSlotsOf(graph.nested, DimVector::POS, limits.fields);
  ASSERT_TRUE(nestedSlots);
  ASSERT_TRUE(arena.link(nestedSlots->at(1), core.dims().step, DimVector::POS,
                         pendingArgs));
  ASSERT_TRUE(arena.link(pendingArgs, core.dims().step, DimVector::POS,
                         nestedSlots->at(3)));
  const auto childSlots =
      core.rawWingSlotsOf(add, DimVector::POS, limits.fields);
  ASSERT_TRUE(childSlots);
  ASSERT_TRUE(arena.link(childSlots->at(0), core.dims().step, DimVector::POS,
                         std::nullopt));
  ASSERT_TRUE(vm.step(cursor).success);
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, NestedPublicationAssignmentAlsoRollsBackOnFail) {
  const auto graph           = setupNested();
  const auto outerInvocation = codec.wing(graph.outer, DimVector::POS);
  const auto outerJump =
      core.readLink(graph.nested, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(outerJump);
  const auto fail   = vm.mintOpcode(OpcodeKind::Fail);
  const auto choose = vm.mintOpcode(OpcodeKind::Choice);
  wing(choose, DimVector::POS, {*outerJump});
  ASSERT_TRUE(
      arena.link(choose, core.dims().spin, DimVector::POS, graph.nested));
  ASSERT_TRUE(arena.link(graph.nested, core.dims().spin, DimVector::POS, fail));
  auto descriptorFields = codec.fields(outerInvocation[3]);
  auto handles          = codec.list(descriptorFields.at("effect_graph"));
  std::vector<CellRef> newHandles;
  for (auto old : handles) newHandles.push_back(handle(codec.reference(old)));
  newHandles.push_back(handle(choose));
  newHandles.push_back(handle(fail));
  codec.replaceRecord(outerInvocation[3],
                      {{"args_schema", descriptorFields.at("args_schema")},
                       {"result_schema", descriptorFields.at("result_schema")},
                       {"bindings", descriptorFields.at("bindings")},
                       {"effect_graph", list(newHandles)}});
  auto first = core.readLink(graph.outer, core.dims().grab, DimVector::POS);
  ASSERT_TRUE(first);
  ASSERT_TRUE(arena.setValueBits(*first, xanadu::ValueKind::OpHandle, choose));
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
}
TEST_F(ScopedEvaluationTest, OnlyExplicitUnitConstantMayCompleteWithoutWrite) {
  setup();
  selectEntryAndGraph(jump, {jump, completion});
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  for (const bool declared : {false, true}) {
    std::vector<std::pair<std::string, CellRef>> fields{
        {"name", arena.makeCell("unit")}, {"kind", arena.makeCell("Unit")}};
    if (declared) fields.emplace_back("constant", arena.makeScalarCell(true));
    auto field = codec.record("ScopeFieldSchema", fields);
    auto resultSchema =
        codec.record("ScopeRecordSchema", {{"fields", list({field})}});
    codec.replaceRecord(invocation[3],
                        {{"args_schema", descriptorFields.at("args_schema")},
                         {"result_schema", resultSchema},
                         {"effect_graph", descriptorFields.at("effect_graph")},
                         {"bindings", list({})}});
    vm.setCursorOpcode(cursor, invoke);
    auto result = vm.run(cursor);
    if (!declared)
      EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
    else {
      ASSERT_TRUE(result.success) << result.errorMessage;
      auto payload = codec.fields(dest).at("payload");
      EXPECT_EQ(arena.textOf(codec.fields(payload).at("unit")), "()");
    }
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
}
TEST_F(ScopedEvaluationTest, EmptyRecordNeedsItsExplicitConstantDeclaration) {
  setup();
  selectEntryAndGraph(jump, {jump, completion});
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  for (const bool declared : {false, true}) {
    std::vector<std::pair<std::string, CellRef>> fields{{"fields", list({})}};
    if (declared) fields.emplace_back("constant", arena.makeScalarCell(true));
    auto resultSchema = codec.record("ScopeRecordSchema", fields);
    codec.replaceRecord(invocation[3],
                        {{"args_schema", descriptorFields.at("args_schema")},
                         {"result_schema", resultSchema},
                         {"effect_graph", descriptorFields.at("effect_graph")},
                         {"bindings", list({})}});
    vm.setCursorOpcode(cursor, invoke);
    auto result = vm.run(cursor);
    if (!declared)
      EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
    else {
      ASSERT_TRUE(result.success) << result.errorMessage;
      EXPECT_TRUE(codec.fields(codec.fields(dest).at("payload")).empty());
    }
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
}
TEST_F(ScopedEvaluationTest, NumericConstantFlagCannotLaunderDefaultZero) {
  setup();
  auto invocation       = codec.wing(invoke, DimVector::POS);
  auto descriptorFields = codec.fields(invocation[3]);
  auto field            = codec.record("ScopeFieldSchema",
                                       {{"name", arena.makeCell("x")},
                                        {"kind", arena.makeCell("Float")},
                                        {"constant", arena.makeScalarCell(true)}});
  auto resultSchema =
      codec.record("ScopeRecordSchema", {{"fields", list({field})}});
  codec.replaceRecord(invocation[3],
                      {{"args_schema", descriptorFields.at("args_schema")},
                       {"result_schema", resultSchema},
                       {"effect_graph", descriptorFields.at("effect_graph")},
                       {"bindings", descriptorFields.at("bindings")}});
  auto result = vm.step(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidSchema);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(outcome(), "Empty");
}
// The caller holds a choice, a Dewey path and an unconsumed oracle directive
// across the scope; its successor is a Choice that the oracle steers to halt.
struct CallerContinuation {
  CellRef choose{};
  CellRef fallthrough{};
  std::vector<std::uint16_t> dewey;
  std::size_t depth{};
  std::size_t marks{};
  std::optional<CellRef> stack;
};
CallerContinuation
armCallerContinuation(ScopedEvaluationTest::NestedGraph graph,
                      ArenaManifold &arena, VortexCore &core, VortexVM &vm,
                      CellRef cursor, CellRef halt) {
  CallerContinuation caller;
  caller.choose      = vm.mintOpcode(OpcodeKind::Choice);
  caller.fallthrough = vm.mintOpcode(OpcodeKind::Halt);
  core.bindInput(caller.choose,
                 arena.makeScalarCell(static_cast<std::int64_t>(halt)));
  EXPECT_TRUE(arena.link(caller.choose, core.dims().spin, DimVector::POS,
                         caller.fallthrough));
  EXPECT_TRUE(
      arena.link(graph.outer, core.dims().spin, DimVector::POS, std::nullopt));
  EXPECT_TRUE(
      arena.link(graph.outer, core.dims().spin, DimVector::POS, caller.choose));
  const std::array<std::uint16_t, 1> oracle{1};
  vm.setOraclePath(oracle);
  vm.setDeweyTracking(true);
  vm.pushChoicePoint(cursor, halt);
  caller.dewey.assign(vm.currentDeweyPath().begin(),
                      vm.currentDeweyPath().end());
  caller.depth = vm.choiceDepth();
  caller.marks = arena.outstandingMarks();
  caller.stack = core.readLink(cursor, core.dims().stack, DimVector::POS);
  return caller;
}
void expectCallerRestored(const CallerContinuation &caller,
                          ArenaManifold &arena, VortexCore &core, VortexVM &vm,
                          CellRef cursor, CellRef halt) {
  EXPECT_EQ(vm.getCursorOpcode(cursor), caller.choose);
  EXPECT_EQ(vm.choiceDepth(), caller.depth);
  EXPECT_EQ(arena.outstandingMarks(), caller.marks);
  EXPECT_EQ(core.readLink(cursor, core.dims().stack, DimVector::POS),
            caller.stack);
  EXPECT_TRUE(std::ranges::equal(vm.currentDeweyPath(), caller.dewey));
  // The restored oracle directive steers the caller's own Choice: no choice
  // point is pushed and the cursor takes the alternative directly.
  ASSERT_TRUE(vm.step(cursor).success);
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(vm.choiceDepth(), caller.depth);
  auto steered = caller.dewey;
  steered.push_back(1);
  EXPECT_TRUE(std::ranges::equal(vm.currentDeweyPath(), steered));
  // The caller's own choice outlived the scope and is still backtrackable.
  EXPECT_TRUE(vm.backtrack(cursor));
  EXPECT_EQ(vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(vm.choiceDepth(), caller.depth - 1);
}
TEST_F(ScopedEvaluationTest, NestedSuccessRestoresCallerChoiceDeweyAndOracle) {
  const auto graph = setupNested(false, 1, false, true);
  const auto caller =
      armCallerContinuation(graph, arena, core, vm, cursor, halt);
  ASSERT_TRUE(vm.step(cursor).success); // enter outer
  ASSERT_TRUE(vm.step(cursor).success); // enter nested
  EXPECT_EQ(arena.outstandingMarks(), caller.marks + 2);
  ASSERT_TRUE(vm.step(cursor).success); // child Choice, oracle hidden
  EXPECT_EQ(vm.choiceDepth(), caller.depth + 1);
  for (std::size_t i = 0; i < 64 && vm.getCursorOpcode(cursor) != caller.choose;
       ++i)
    ASSERT_TRUE(vm.step(cursor).success);
  ASSERT_EQ(outcome(), "Value");
  auto query = codec.fields(codec.fields(dest).at("payload")).at("query");
  EXPECT_EQ(arena.textOf(codec.fields(query).at("outcome")), "Value");
  expectCallerRestored(caller, arena, core, vm, cursor, halt);
}
TEST_F(ScopedEvaluationTest, NestedRefusalRestoresCallerChoiceDeweyAndOracle) {
  const auto graph = setupNested(false, 1, false, true, true);
  const auto caller =
      armCallerContinuation(graph, arena, core, vm, cursor, halt);
  ExecutionResult result;
  for (std::size_t i = 0;
       i < 64 && result.success && vm.getCursorOpcode(cursor) != caller.choose;
       ++i)
    result = vm.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::InvalidScopedCompletion);
  ASSERT_EQ(outcome(), "Refused");
  EXPECT_EQ(arena.textOf(codec.fields(dest).at("error")),
            "InvalidScopedCompletion");
  EXPECT_FALSE(codec.fields(dest).contains("payload"));
  expectCallerRestored(caller, arena, core, vm, cursor, halt);
}
TEST_F(ScopedEvaluationTest, LiveScopeArgumentKeepsForeignQuoteIdentity) {
  setup();
  ScopeTestReader reader;
  const auto space = arena.attach(Space{.reader = &reader, .label = "reader"});
  const xanadu::PrimediaSpan stable{xanadu::localScroll, 0, 2};
  const auto quote = arena.makeQuote(space, std::span{&stable, 1});
  auto invocation  = codec.wing(invoke, DimVector::POS);
  auto fields      = codec.fields(invocation[3]);
  auto text        = [&](std::string_view name) {
    return codec.record(
        "ScopeFieldSchema",
        {{"name", arena.makeCell(name)},
         {"kind", arena.makeCell("Str")},
         {"max_bytes", arena.makeScalarCell(std::int64_t{64})}});
  };
  auto bind = [&](CellRef reg, std::string_view target) {
    return codec.record("ScopeBinding", {{"register", handle(reg)},
                                         {"target", arena.makeCell(target)},
                                         {"kind", arena.makeCell("Str")}});
  };
  codec.replaceRecord(
      invocation[3],
      {{"args_schema",
        codec.record("ScopeRecordSchema",
                     {{"fields", list({text("x"), text("y")})}})},
       {"result_schema",
        codec.record("ScopeRecordSchema", {{"fields", list({text("x")})}})},
       {"effect_graph", fields.at("effect_graph")},
       {"bindings", list({bind(argX, "arg.x"), bind(argY, "arg.y"),
                          bind(resultX, "result.x")})}});
  codec.replaceRecord(args, {{"x", quote}, {"y", arena.makeCell("!")}});
  ASSERT_TRUE(vm.step(cursor).success);
  // Inside the live scope the owned copy is a quotation in the same pinned
  // namespace and names the same stable span, not a native copy of its text.
  const auto owned = arena.handleTarget(argX);
  ASSERT_TRUE(owned);
  EXPECT_NE(*owned, quote);
  EXPECT_EQ(arena.contentSpaceOf(*owned), space);
  ASSERT_EQ(arena.contentOf(*owned).size(), 1u);
  EXPECT_EQ(arena.contentOf(*owned)[0], stable);
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(outcome(), "Value");
  auto exported = codec.fields(codec.fields(dest).at("payload")).at("x");
  EXPECT_EQ(arena.textOf(exported), "so!");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  ASSERT_EQ(arena.contentOf(quote).size(), 1u);
  EXPECT_EQ(arena.contentOf(quote)[0], stable);
  xanadu::Store store;
  const auto at       = store.sliceGenesis(xanadu::MicroversionId{});
  const auto promoted = zigzag::promote(store, at, arena, exported);
  ASSERT_TRUE(promoted);
  const auto manifold = store.rebuildManifold(promoted->version);
  EXPECT_EQ(manifold.textOf(promoted->mapping.at(exported), store), "so!");
}
// I2B-2: typed structural reads. The descriptor is rebuilt per test so each
// names exactly the bindings and kinds it exercises.
class ScopedStructureTest : public ScopedEvaluationTest {
protected:
  CellRef field(std::string_view name, std::string_view kind,
                std::vector<std::pair<std::string, CellRef>> extra = {}) {
    std::vector<std::pair<std::string, CellRef>> values{
        {"name", arena.makeCell(name)}, {"kind", arena.makeCell(kind)}};
    for (auto &entry : extra) values.push_back(std::move(entry));
    return codec.record("ScopeFieldSchema", values);
  }
  CellRef str(std::string_view name) {
    return field(name, "Str",
                 {{"max_bytes", arena.makeScalarCell(std::int64_t{64})}});
  }
  CellRef pinnedField(std::string_view name, std::string_view root) {
    return field(name, "PinnedRef", {{"pinned_root", arena.makeCell(root)}});
  }
  CellRef recordSchema(const std::vector<CellRef> &fields) {
    return codec.record("ScopeRecordSchema", {{"fields", list(fields)}});
  }
  CellRef reg(std::string_view label) { return handle(arena.makeCell(label)); }
  CellRef bind(CellRef r, std::string_view target, std::string_view kind) {
    return codec.record("ScopeBinding", {{"register", handle(r)},
                                         {"target", arena.makeCell(target)},
                                         {"kind", arena.makeCell(kind)}});
  }
  CellRef op(OpcodeKind kind, std::initializer_list<CellRef> ins,
             std::initializer_list<CellRef> outs) {
    const auto cell = vm.mintOpcode(kind);
    if (ins.size() != 0) wing(cell, DimVector::POS, ins);
    if (outs.size() != 0) wing(cell, DimVector::NEG, outs);
    return cell;
  }
  // Chains @p body on d.spin into the verified completion Jump and installs
  // it as the callback with the given schemas, bindings and arguments.
  void install(CellRef argsSchema, CellRef resultSchema,
               const std::vector<CellRef> &bindings,
               const std::vector<CellRef> &body,
               const std::vector<std::pair<std::string, CellRef>> &values,
               bool chain = true) {
    std::vector<CellRef> graph = body;
    for (std::size_t i = 0; chain && i < body.size(); ++i)
      EXPECT_TRUE(arena.link(body[i], core.dims().spin, DimVector::POS,
                             i + 1 < body.size() ? body[i + 1] : jump));
    graph.push_back(jump);
    graph.push_back(completion);
    std::vector<CellRef> handles;
    for (auto cell : graph) handles.push_back(handle(cell));
    const auto invocation = codec.wing(invoke, DimVector::POS);
    codec.replaceRecord(invocation[3], {{"args_schema", argsSchema},
                                        {"result_schema", resultSchema},
                                        {"effect_graph", list(handles)},
                                        {"bindings", list(bindings)}});
    const auto entry = core.readLink(invoke, core.dims().grab, DimVector::POS);
    EXPECT_TRUE(entry);
    EXPECT_TRUE(
        arena.setValueBits(*entry, xanadu::ValueKind::OpHandle, body.front()));
    codec.replaceRecord(args, values);
  }
  // A caller-pinned list of observation records {w: Float}.
  CellRef observations(std::string_view pin, std::initializer_list<double> ws,
                       std::vector<CellRef> *cells = nullptr) {
    std::vector<CellRef> items;
    for (auto w : ws)
      items.push_back(
          codec.record("Observation", {{"w", arena.makeScalarCell(w)}}));
    if (cells) *cells = items;
    const auto root = list(items);
    core.pin(pin, root);
    return root;
  }
  CellRef payloadField(std::string_view name) {
    return codec.fields(codec.fields(dest).at("payload")).at(std::string(name));
  }
};
TEST_F(ScopedStructureTest, SelectsAReachedObservationAndReadsItsField) {
  setup();
  std::vector<CellRef> obs;
  const auto root   = observations("obs", {0.25, 0.75}, &obs);
  const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
             selNext = reg("arg.next"), selW = reg("arg.w"), cur = reg("cur"),
             wcur = reg("wcur"), found = reg("found"), best = reg("best"),
             score = reg("score");
  const std::vector body{op(OpcodeKind::Point, {argObs}, {cur}),
                         op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
                         op(OpcodeKind::Link, {cur, selNext}, {cur, found}),
                         op(OpcodeKind::Link, {cur, selW}, {wcur, found}),
                         op(OpcodeKind::Deref, {wcur}, {score}),
                         op(OpcodeKind::Point, {cur}, {best})};
  install(recordSchema(
              {pinnedField("obs", "obs"), str("first"), str("next"), str("w")}),
          recordSchema({pinnedField("best", "obs"), field("score", "Float")}),
          {bind(argObs, "arg.obs", "PinnedRef"),
           bind(selFirst, "arg.first", "Str"), bind(selNext, "arg.next", "Str"),
           bind(selW, "arg.w", "Str"), bind(cur, "temp.cur", "Cursor"),
           bind(wcur, "temp.wcur", "Cursor"), bind(found, "temp.found", "Bool"),
           bind(best, "result.best", "PinnedRef"),
           bind(score, "result.score", "Float")},
          body,
          {{"obs", handle(root)},
           {"first", arena.makeCell("first")},
           {"next", arena.makeCell("next")},
           {"w", arena.makeCell(".w")}});
  const auto marks = arena.outstandingMarks();
  auto result      = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(outcome(), "Value");
  EXPECT_EQ(arena.outstandingMarks(), marks);
  EXPECT_EQ(arena.asDouble(payloadField("score")), 0.75);
  // The exported reference names the caller's own observation, which
  // outlives the released scope.
  EXPECT_EQ(arena.handleTarget(payloadField("best")), obs[1]);
  EXPECT_EQ(arena.asDouble(codec.fields(obs[1]).at("w")), 0.75);
}
TEST_F(ScopedStructureTest, OwnedArgumentDataCannotBecomeAResultReference) {
  setup();
  const auto root   = observations("obs", {1.0});
  const auto argObs = reg("arg.obs"), argData = reg("arg.data"),
             cur = reg("cur"), best = reg("best");
  install(recordSchema({pinnedField("obs", "obs"),
                        field("data", "Record",
                              {{"fields", list({field("w", "Float")})}})}),
          recordSchema({pinnedField("best", "obs")}),
          {bind(argObs, "arg.obs", "PinnedRef"),
           bind(argData, "arg.data", "Record"), bind(cur, "temp.cur", "Cursor"),
           bind(best, "result.best", "PinnedRef")},
          {op(OpcodeKind::Point, {argData}, {cur}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"obs", handle(root)},
           {"data", codec.record("Data", {{"w", arena.makeScalarCell(1.0)}})}});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(outcome(), "Refused");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedStructureTest, AReferenceCannotBeLaunderedAcrossPinRoots) {
  for (const bool sameRoot : {true, false}) {
    SCOPED_TRACE(sameRoot);
    setup();
    std::vector<CellRef> other;
    const auto mine    = observations("mine", {1.0});
    const auto theirs  = observations("theirs", {2.0}, &other);
    const auto argMine = reg("arg.mine"), argTheirs = reg("arg.theirs"),
               selFirst = reg("arg.first"), cur = reg("cur"),
               found = reg("found"), best = reg("best");
    install(recordSchema({pinnedField("mine", "mine"),
                          pinnedField("theirs", "theirs"), str("first")}),
            recordSchema({pinnedField("best", sameRoot ? "theirs" : "mine")}),
            {bind(argMine, "arg.mine", "PinnedRef"),
             bind(argTheirs, "arg.theirs", "PinnedRef"),
             bind(selFirst, "arg.first", "Str"),
             bind(cur, "temp.cur", "Cursor"), bind(found, "temp.found", "Bool"),
             bind(best, "result.best", "PinnedRef")},
            {op(OpcodeKind::Point, {argTheirs}, {cur}),
             op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
             op(OpcodeKind::Point, {cur}, {best})},
            {{"mine", handle(mine)},
             {"theirs", handle(theirs)},
             {"first", arena.makeCell("first")}});
    auto result = vm.run(cursor);
    if (sameRoot) {
      // The reached cell publishes under the root it was reached from.
      ASSERT_TRUE(result.success) << result.errorMessage;
      EXPECT_EQ(arena.handleTarget(payloadField("best")), other[0]);
    } else {
      EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
      EXPECT_EQ(outcome(), "Refused");
      EXPECT_FALSE(codec.fields(dest).contains("payload"));
    }
  }
}
TEST_F(ScopedStructureTest, MissingFieldReportsNotFoundWithoutMovingTheCursor) {
  setup();
  const auto root   = observations("obs", {1.0});
  const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
             selMissing = reg("arg.missing"), cur = reg("cur"),
             other = reg("other"), found = reg("found"), seen = reg("seen");
  install(
      recordSchema({pinnedField("obs", "obs"), str("first"), str("missing")}),
      recordSchema({field("seen", "Bool"), field("found", "Bool")}),
      {bind(argObs, "arg.obs", "PinnedRef"), bind(selFirst, "arg.first", "Str"),
       bind(selMissing, "arg.missing", "Str"), bind(cur, "temp.cur", "Cursor"),
       bind(other, "temp.other", "Cursor"), bind(seen, "result.seen", "Bool"),
       bind(found, "result.found", "Bool")},
      {op(OpcodeKind::Point, {argObs}, {cur}),
       op(OpcodeKind::Link, {cur, selFirst}, {cur, seen}),
       op(OpcodeKind::Link, {cur, selMissing}, {other, found})},
      {{"obs", handle(root)},
       {"first", arena.makeCell("first")},
       {"missing", arena.makeCell(".absent")}});
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(outcome(), "Value");
  EXPECT_EQ(arena.asBool(payloadField("seen")), true);
  EXPECT_EQ(arena.asBool(payloadField("found")), false);
}
TEST_F(ScopedStructureTest, UnassignedCursorAndWrongScalarKindRefuse) {
  for (const bool readUnassigned : {true, false}) {
    SCOPED_TRACE(readUnassigned);
    setup();
    const auto root   = observations("obs", {1.0});
    const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
               selW = reg("arg.w"), cur = reg("cur"), found = reg("found"),
               n = reg("n");
    std::vector<CellRef> body;
    if (!readUnassigned) {
      body.push_back(op(OpcodeKind::Point, {argObs}, {cur}));
      body.push_back(op(OpcodeKind::Link, {cur, selFirst}, {cur, found}));
      body.push_back(op(OpcodeKind::Link, {cur, selW}, {cur, found}));
    }
    // w holds a Float; reading it as Int is a type error, not a conversion.
    body.push_back(op(OpcodeKind::Deref, {cur}, {n}));
    install(recordSchema({pinnedField("obs", "obs"), str("first"), str("w")}),
            recordSchema({field("n", "Int")}),
            {bind(argObs, "arg.obs", "PinnedRef"),
             bind(selFirst, "arg.first", "Str"), bind(selW, "arg.w", "Str"),
             bind(cur, "temp.cur", "Cursor"), bind(found, "temp.found", "Bool"),
             bind(n, "result.n", "Int")},
            body,
            {{"obs", handle(root)},
             {"first", arena.makeCell("first")},
             {"w", arena.makeCell(".w")}});
    auto result = vm.run(cursor);
    EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
    EXPECT_EQ(outcome(), "Refused");
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
}
TEST_F(ScopedStructureTest,
       BacktrackingUnassignsACursorMovedInTheFailedBranch) {
  // The branch parks the cursor on a real Float. Only the twin without Fail
  // may read it; after Fail the alternative finds the cursor unassigned.
  for (const bool failBranch : {false, true}) {
    SCOPED_TRACE(failBranch);
    setup();
    const auto root   = observations("obs", {1.5});
    const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
               selW = reg("arg.w"), cur = reg("cur"), found = reg("found"),
               n      = reg("n");
    const auto read   = op(OpcodeKind::Deref, {cur}, {n});
    const auto choose = vm.mintOpcode(OpcodeKind::Choice);
    wing(choose, DimVector::POS, {read});
    const std::vector moves{op(OpcodeKind::Point, {argObs}, {cur}),
                            op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
                            op(OpcodeKind::Link, {cur, selW}, {cur, found})};
    const auto fail = vm.mintOpcode(OpcodeKind::Fail);
    ASSERT_TRUE(arena.link(choose, core.dims().spin, DimVector::POS, moves[0]));
    ASSERT_TRUE(
        arena.link(moves[0], core.dims().spin, DimVector::POS, moves[1]));
    ASSERT_TRUE(
        arena.link(moves[1], core.dims().spin, DimVector::POS, moves[2]));
    ASSERT_TRUE(arena.link(moves[2], core.dims().spin, DimVector::POS,
                           failBranch ? fail : read));
    install(recordSchema({pinnedField("obs", "obs"), str("first"), str("w")}),
            recordSchema({field("n", "Float")}),
            {bind(argObs, "arg.obs", "PinnedRef"),
             bind(selFirst, "arg.first", "Str"), bind(selW, "arg.w", "Str"),
             bind(cur, "temp.cur", "Cursor"), bind(found, "temp.found", "Bool"),
             bind(n, "result.n", "Float")},
            {read},
            {{"obs", handle(root)},
             {"first", arena.makeCell("first")},
             {"w", arena.makeCell(".w")}});
    std::vector graph{choose, moves[0], moves[1],  moves[2],
                      read,   jump,     completion};
    if (failBranch) graph.push_back(fail);
    selectEntryAndGraph(choose, graph);
    auto result = vm.run(cursor);
    if (failBranch) {
      EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
      EXPECT_EQ(outcome(), "Refused");
    } else {
      ASSERT_TRUE(result.success) << result.errorMessage;
      EXPECT_EQ(arena.asDouble(payloadField("n")), 1.5);
    }
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
}
TEST_F(ScopedStructureTest, ACellPinnedTwiceIsAttributedToBothRoots) {
  // Order independence: walking from one root may publish under the other.
  setup();
  std::vector<CellRef> obs;
  const auto root = observations("a", {1.0}, &obs);
  core.pin("b", root);
  const auto argA = reg("arg.a"), argB = reg("arg.b"),
             selFirst = reg("arg.first"), cur = reg("cur"),
             found = reg("found"), best = reg("best");
  install(recordSchema(
              {pinnedField("a", "a"), pinnedField("b", "b"), str("first")}),
          recordSchema({pinnedField("best", "b")}),
          {bind(argA, "arg.a", "PinnedRef"), bind(argB, "arg.b", "PinnedRef"),
           bind(selFirst, "arg.first", "Str"), bind(cur, "temp.cur", "Cursor"),
           bind(found, "temp.found", "Bool"),
           bind(best, "result.best", "PinnedRef")},
          {op(OpcodeKind::Point, {argA}, {cur}),
           op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"a", handle(root)},
           {"b", handle(root)},
           {"first", arena.makeCell("first")}});
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(arena.handleTarget(payloadField("best")), obs[0]);
}
TEST_F(ScopedStructureTest, TargetFollowsAnOwnedReferenceOnlyToItsPin) {
  setup();
  std::vector<CellRef> obs;
  const auto root    = observations("obs", {2.5}, &obs);
  const auto argRefs = reg("arg.refs"), argText = reg("arg.text"),
             selFirst = reg("arg.first"), selTarget = reg("arg.target"),
             selW = reg("arg.w"), cur = reg("cur"), w = reg("w"),
             found = reg("found"), plain = reg("plain"), plainFound = reg("pf"),
             best = reg("best"), score = reg("score"), deref = reg("deref");
  install(recordSchema(
              {field("refs", "List",
                     {{"item", pinnedField("item", "obs")},
                      {"max_items", arena.makeScalarCell(std::int64_t{4})}}),
               pinnedField("obs", "obs"), str("text"), str("first"),
               str("target"), str("w")}),
          recordSchema({pinnedField("best", "obs"), field("score", "Float"),
                        field("deref", "Bool"), field("plain", "Bool")}),
          {bind(argRefs, "arg.refs", "List"), bind(argText, "arg.text", "Str"),
           bind(selFirst, "arg.first", "Str"),
           bind(selTarget, "arg.target", "Str"), bind(selW, "arg.w", "Str"),
           bind(cur, "temp.cur", "Cursor"), bind(w, "temp.w", "Cursor"),
           bind(found, "temp.found", "Bool"),
           bind(plain, "temp.plain", "Cursor"),
           bind(plainFound, "result.plain", "Bool"),
           bind(best, "result.best", "PinnedRef"),
           bind(score, "result.score", "Float"),
           bind(deref, "result.deref", "Bool")},
          {op(OpcodeKind::Point, {argRefs}, {cur}),
           op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
           op(OpcodeKind::Link, {cur, selTarget}, {cur, deref}),
           // The observation record is not a reference: target is not found.
           op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
           op(OpcodeKind::Link, {cur, selTarget}, {plain, plainFound}),
           op(OpcodeKind::Link, {cur, selW}, {w, found}),
           op(OpcodeKind::Deref, {w}, {score}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"refs", list({handle(root)})},
           {"obs", handle(root)},
           {"text", arena.makeCell("x")},
           {"first", arena.makeCell("first")},
           {"target", arena.makeCell("target")},
           {"w", arena.makeCell(".w")}});
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(arena.asBool(payloadField("deref")), true);
  EXPECT_EQ(arena.asBool(payloadField("plain")), false);
  EXPECT_EQ(arena.asDouble(payloadField("score")), 2.5);
  EXPECT_EQ(arena.handleTarget(payloadField("best")), obs[0]);
}
TEST_F(ScopedStructureTest, UnknownSelectorWordRefusesInsteadOfMissing) {
  setup();
  const auto root   = observations("obs", {1.0});
  const auto argObs = reg("arg.obs"), sel = reg("arg.sel"), cur = reg("cur"),
             found = reg("found");
  install(recordSchema({pinnedField("obs", "obs"), str("sel")}),
          recordSchema({field("found", "Bool")}),
          {bind(argObs, "arg.obs", "PinnedRef"), bind(sel, "arg.sel", "Str"),
           bind(cur, "temp.cur", "Cursor"),
           bind(found, "result.found", "Bool")},
          {op(OpcodeKind::Point, {argObs}, {cur}),
           op(OpcodeKind::Link, {cur, sel}, {cur, found})},
          {{"obs", handle(root)}, {"sel", arena.makeCell("frist")}});
  EXPECT_EQ(vm.run(cursor).scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(outcome(), "Refused");
}
TEST_F(ScopedStructureTest, NonFiniteFloatAndNumericFaultRefuseUnderAChoice) {
  // Neither a stored infinity nor Ln(0) may backtrack into the open choice's
  // alternative and so turn into a plausible Empty.
  for (const bool numericFault : {false, true}) {
    SCOPED_TRACE(numericFault);
    setup();
    const auto root = observations(
        "obs", {numericFault ? 0.0 : std::numeric_limits<double>::infinity()});
    const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
               selW = reg("arg.w"), cur = reg("cur"), found = reg("found"),
               n = reg("n"), logn = reg("logn");
    const std::vector body{op(OpcodeKind::Point, {argObs}, {cur}),
                           op(OpcodeKind::Link, {cur, selFirst}, {cur, found}),
                           op(OpcodeKind::Link, {cur, selW}, {cur, found}),
                           op(OpcodeKind::Deref, {cur}, {n}),
                           op(OpcodeKind::Ln, {n}, {logn})};
    install(recordSchema({pinnedField("obs", "obs"), str("first"), str("w")}),
            recordSchema({field("logn", "Float")}),
            {bind(argObs, "arg.obs", "PinnedRef"),
             bind(selFirst, "arg.first", "Str"), bind(selW, "arg.w", "Str"),
             bind(cur, "temp.cur", "Cursor"), bind(found, "temp.found", "Bool"),
             bind(n, "temp.n", "Float"), bind(logn, "result.logn", "Float")},
            body,
            {{"obs", handle(root)},
             {"first", arena.makeCell("first")},
             {"w", arena.makeCell(".w")}});
    const auto fail   = vm.mintOpcode(OpcodeKind::Fail);
    const auto choose = vm.mintOpcode(OpcodeKind::Choice);
    wing(choose, DimVector::POS, {fail});
    ASSERT_TRUE(arena.link(choose, core.dims().spin, DimVector::POS, body[0]));
    std::vector graph{choose, fail, jump, completion};
    graph.insert(graph.end(), body.begin(), body.end());
    selectEntryAndGraph(choose, graph);
    auto result = vm.run(cursor);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.scopedRefusal, numericFault ? ScopedRefusal::CallbackFault
                                                 : ScopedRefusal::TypeMismatch);
    EXPECT_EQ(outcome(), "Refused");
    EXPECT_EQ(arena.outstandingMarks(), 0u);
  }
}
TEST_F(ScopedStructureTest, BestOfCandidatesWithProfileLookupAndAccumulator) {
  // The I4 shape: walk candidate observations, look each source's profile up
  // by name, score w * alpha with Int alpha promoted by Add, keep the maximum
  // and accumulate the total weight. Everything is an ordinary scoped graph.
  setup();
  std::vector<CellRef> obsItems;
  for (const auto &[w, src] : std::vector<std::pair<double, std::string>>{
           {0.5, "a"}, {0.4, "b"}, {0.9, "zzz"}})
    obsItems.push_back(
        codec.record("Observation", {{"w", arena.makeScalarCell(w)},
                                     {"src", arena.makeCell(src)}}));
  const auto obsRoot = list(obsItems);
  core.pin("obs", obsRoot);
  std::vector<CellRef> profileItems;
  for (const auto &[name, alpha] :
       std::vector<std::pair<std::string, std::int64_t>>{{"a", 1}, {"b", 3}})
    profileItems.push_back(
        codec.record("Profile", {{"name", arena.makeCell(name)},
                                 {"alpha", arena.makeScalarCell(alpha)}}));
  const auto profileRoot = list(profileItems);
  core.pin("profiles", profileRoot);

  const auto argObs = reg("arg.obs"), argProfiles = reg("arg.profiles"),
             sFirst = reg("s.first"), sNext = reg("s.next"), sW = reg("s.w"),
             sSrc = reg("s.src"), sName = reg("s.name"),
             sAlpha = reg("s.alpha"), zero = reg("zero"), negOne = reg("neg"),
             c = reg("c"), pc = reg("pc"), x = reg("x"), found = reg("found"),
             nf = reg("nf"), w = reg("w"), src = reg("src"), name = reg("name"),
             alphaI = reg("alphaI"), alpha = reg("alpha"), sc = reg("sc"),
             same = reg("same"), better = reg("better"), nb = reg("nb"),
             best = reg("best"), score = reg("score"), total = reg("total");
  const auto done    = op(OpcodeKind::Nop, {}, {});
  const auto nextObs = op(OpcodeKind::Link, {c, sNext}, {c, found});
  const auto loop    = op(OpcodeKind::Not, {found}, {nf});
  const auto ploop   = op(OpcodeKind::Not, {found}, {nf});
  const auto hit     = op(OpcodeKind::Link, {pc, sAlpha}, {x, found});
  const auto branch  = [&](CellRef cond, CellRef target) {
    const auto cell = vm.mintOpcode(OpcodeKind::Branch);
    wing(cell, DimVector::POS, {cond, target});
    return cell;
  };
  const auto jumpTo = [&](CellRef target) {
    const auto cell = vm.mintOpcode(OpcodeKind::Jump);
    wing(cell, DimVector::POS, {target});
    return cell;
  };
  const auto backToPloop = jumpTo(ploop);
  const auto backToLoop  = jumpTo(loop);
  const std::vector<std::vector<CellRef>> runs{
      {op(OpcodeKind::Add, {zero, zero}, {total}),
       op(OpcodeKind::Add, {negOne, zero}, {score}),
       op(OpcodeKind::Point, {argObs}, {c}),
       op(OpcodeKind::Link, {c, sFirst}, {c, found}),
       loop,
       branch(nf, done),
       op(OpcodeKind::Link, {c, sW}, {x, found}),
       op(OpcodeKind::Deref, {x}, {w}),
       op(OpcodeKind::Add, {total, w}, {total}),
       op(OpcodeKind::Link, {c, sSrc}, {x, found}),
       op(OpcodeKind::Deref, {x}, {src}),
       op(OpcodeKind::Point, {argProfiles}, {pc}),
       op(OpcodeKind::Link, {pc, sFirst}, {pc, found}),
       ploop,
       branch(nf, nextObs),
       op(OpcodeKind::Link, {pc, sName}, {x, found}),
       op(OpcodeKind::Deref, {x}, {name}),
       op(OpcodeKind::Eq, {name, src}, {same}),
       branch(same, hit),
       op(OpcodeKind::Link, {pc, sNext}, {pc, found}),
       backToPloop},
      {hit, op(OpcodeKind::Deref, {x}, {alphaI}),
       op(OpcodeKind::Add, {alphaI, zero}, {alpha}),
       op(OpcodeKind::Mul, {w, alpha}, {sc}),
       op(OpcodeKind::Gt, {sc, score}, {better}),
       op(OpcodeKind::Not, {better}, {nb}), branch(nb, nextObs),
       op(OpcodeKind::Add, {sc, zero}, {score}),
       op(OpcodeKind::Point, {c}, {best}), nextObs, backToLoop},
      {done}};
  std::vector<CellRef> graph;
  for (const auto &run : runs) {
    for (std::size_t i = 0; i + 1 < run.size(); ++i)
      ASSERT_TRUE(
          arena.link(run[i], core.dims().spin, DimVector::POS, run[i + 1]));
    graph.insert(graph.end(), run.begin(), run.end());
  }
  ASSERT_TRUE(arena.link(done, core.dims().spin, DimVector::POS, jump));
  graph.push_back(jump);
  graph.push_back(completion);
  const auto text = [&](CellRef r, std::string_view t) {
    return bind(r, t, "Str");
  };
  install(recordSchema(
              {pinnedField("obs", "obs"), pinnedField("profiles", "profiles"),
               str("first"), str("next"), str("w"), str("src"), str("name"),
               str("alpha"), field("zero", "Float"), field("neg", "Float")}),
          recordSchema({pinnedField("best", "obs"), field("score", "Float"),
                        field("total", "Float")}),
          {bind(argObs, "arg.obs", "PinnedRef"),
           bind(argProfiles, "arg.profiles", "PinnedRef"),
           text(sFirst, "arg.first"),
           text(sNext, "arg.next"),
           text(sW, "arg.w"),
           text(sSrc, "arg.src"),
           text(sName, "arg.name"),
           text(sAlpha, "arg.alpha"),
           bind(zero, "arg.zero", "Float"),
           bind(negOne, "arg.neg", "Float"),
           bind(c, "temp.c", "Cursor"),
           bind(pc, "temp.pc", "Cursor"),
           bind(x, "temp.x", "Cursor"),
           bind(found, "temp.found", "Bool"),
           bind(nf, "temp.nf", "Bool"),
           bind(w, "temp.w", "Float"),
           text(src, "temp.src"),
           text(name, "temp.name"),
           bind(alphaI, "temp.alphaI", "Int"),
           bind(alpha, "temp.alpha", "Float"),
           bind(sc, "temp.sc", "Float"),
           bind(same, "temp.same", "Bool"),
           bind(better, "temp.better", "Bool"),
           bind(nb, "temp.nb", "Bool"),
           bind(best, "result.best", "PinnedRef"),
           bind(score, "result.score", "Float"),
           bind(total, "result.total", "Float")},
          {runs[0][0]},
          {{"obs", handle(obsRoot)},
           {"profiles", handle(profileRoot)},
           {"first", arena.makeCell("first")},
           {"next", arena.makeCell("next")},
           {"w", arena.makeCell(".w")},
           {"src", arena.makeCell(".src")},
           {"name", arena.makeCell(".name")},
           {"alpha", arena.makeCell(".alpha")},
           {"zero", arena.makeScalarCell(0.0)},
           {"neg", arena.makeScalarCell(-1.0)}},
          false);
  selectEntryAndGraph(runs[0][0], graph);
  auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(outcome(), "Value");
  EXPECT_EQ(arena.handleTarget(payloadField("best")), obsItems[1]);
  EXPECT_DOUBLE_EQ(*arena.asDouble(payloadField("score")), 1.2);
  EXPECT_DOUBLE_EQ(*arena.asDouble(payloadField("total")), 1.8);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedStructureTest, TheDestinationRecordCannotBePublishedAsAResult) {
  // Publication relinks the destination, so naming it would build a cycle.
  setup();
  core.pin("d", dest);
  const auto argD = reg("arg.d"), cur = reg("cur"), best = reg("best");
  install(recordSchema({pinnedField("d", "d")}),
          recordSchema({pinnedField("best", "d")}),
          {bind(argD, "arg.d", "PinnedRef"), bind(cur, "temp.cur", "Cursor"),
           bind(best, "result.best", "PinnedRef")},
          {op(OpcodeKind::Point, {argD}, {cur}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"d", handle(dest)}});
  auto result = vm.run(cursor);
  EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
  EXPECT_EQ(outcome(), "Refused");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedStructureTest,
       OldDestinationFieldGraphCannotEscapeThroughItsOwnPin) {
  for (const auto part : {"marker", "name", "value"}) {
    SCOPED_TRACE(part);
    setup();
    const auto marker = core.readLink(dest, core.dims().vars, DimVector::POS);
    ASSERT_TRUE(marker);
    const auto name = core.readLink(*marker, core.dims().vars, DimVector::POS);
    ASSERT_TRUE(name);
    const auto value = core.readLink(*name, core.dims().values, DimVector::POS);
    ASSERT_TRUE(value);
    const auto target = std::string_view(part) == "marker" ? *marker
                        : std::string_view(part) == "name" ? *name
                                                           : *value;
    core.pin("saved", target);
    const auto argSaved = reg("arg.saved"), cur = reg("cur"),
               best = reg("best");
    install(recordSchema({pinnedField("saved", "saved")}),
            recordSchema({pinnedField("best", "saved")}),
            {bind(argSaved, "arg.saved", "PinnedRef"),
             bind(cur, "temp.cur", "Cursor"),
             bind(best, "result.best", "PinnedRef")},
            {op(OpcodeKind::Point, {argSaved}, {cur}),
             op(OpcodeKind::Point, {cur}, {best})},
            {{"saved", handle(target)}});
    // Establish that this is a valid pinned argument and readable position;
    // refusal must occur specifically at the attempted reference export.
    ASSERT_TRUE(vm.step(cursor).success);
    ASSERT_TRUE(vm.step(cursor).success);
    const auto result = vm.step(cursor);
    EXPECT_EQ(result.scopedRefusal, ScopedRefusal::TypeMismatch);
    EXPECT_EQ(outcome(), "Refused");
    EXPECT_FALSE(codec.fields(dest).contains("payload"));
    EXPECT_EQ(arena.outstandingMarks(), 0u);
    EXPECT_FALSE(core.readLink(*marker, core.dims().vars, DimVector::NEG));
  }
}
TEST_F(ScopedStructureTest, UnrelatedCallerFieldNameRemainsAnExportablePin) {
  setup();
  const auto callerValue = arena.makeCell("caller value");
  const auto callerRecord =
      codec.record("CallerRecord", {{"outcome", callerValue}});
  const auto marker =
      core.readLink(callerRecord, core.dims().vars, DimVector::POS);
  ASSERT_TRUE(marker);
  const auto name = core.readLink(*marker, core.dims().vars, DimVector::POS);
  ASSERT_TRUE(name);
  core.pin("saved", *name);
  const auto argSaved = reg("arg.saved"), cur = reg("cur"), best = reg("best");
  install(recordSchema({pinnedField("saved", "saved")}),
          recordSchema({pinnedField("best", "saved")}),
          {bind(argSaved, "arg.saved", "PinnedRef"),
           bind(cur, "temp.cur", "Cursor"),
           bind(best, "result.best", "PinnedRef")},
          {op(OpcodeKind::Point, {argSaved}, {cur}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"saved", handle(*name)}});
  ASSERT_TRUE(vm.step(cursor).success);
  ASSERT_TRUE(vm.step(cursor).success);
  ASSERT_TRUE(vm.step(cursor).success);
  const auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(outcome(), "Value");
  EXPECT_EQ(arena.handleTarget(payloadField("best")), *name);
  EXPECT_EQ(arena.textOf(*name), "outcome");
  EXPECT_EQ(core.readLink(callerRecord, core.dims().vars, DimVector::POS),
            marker);
  EXPECT_EQ(core.readLink(*marker, core.dims().vars, DimVector::NEG),
            callerRecord);
  EXPECT_EQ(core.readLink(*name, core.dims().values, DimVector::POS),
            callerValue);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedStructureTest,
       OldDestinationReferenceDoesNotOwnItsCallerReferent) {
  setup();
  const auto observation = arena.makeCell("caller observation");
  core.pin("obs", observation);
  const auto oldPayload =
      codec.record("ScopeRecord", {{"best", handle(observation)}});
  codec.replaceRecord(
      dest, {{"outcome", arena.makeCell("Empty")}, {"payload", oldPayload}});
  const auto argObs = reg("arg.obs"), cur = reg("cur"), best = reg("best");
  install(recordSchema({pinnedField("obs", "obs")}),
          recordSchema({pinnedField("best", "obs")}),
          {bind(argObs, "arg.obs", "PinnedRef"),
           bind(cur, "temp.cur", "Cursor"),
           bind(best, "result.best", "PinnedRef")},
          {op(OpcodeKind::Point, {argObs}, {cur}),
           op(OpcodeKind::Point, {cur}, {best})},
          {{"obs", handle(observation)}});
  const auto result = vm.run(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(outcome(), "Value");
  EXPECT_EQ(arena.handleTarget(payloadField("best")), observation);
  EXPECT_EQ(arena.textOf(observation), "caller observation");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
TEST_F(ScopedStructureTest, VerifierRefusesIllTypedStructuralOperands) {
  struct Case {
    const char *name;
    ScopedRefusal expected;
  };
  for (const auto &c : {Case{"cursor-result", ScopedRefusal::InvalidSchema},
                        Case{"float-found", ScopedRefusal::TypeMismatch},
                        Case{"result-source", ScopedRefusal::TypeMismatch},
                        Case{"cursor-schema", ScopedRefusal::InvalidSchema}}) {
    SCOPED_TRACE(c.name);
    setup();
    const auto root   = observations("obs", {1.0});
    const auto argObs = reg("arg.obs"), selFirst = reg("arg.first"),
               cur = reg("cur"), flag = reg("flag"), best = reg("best");
    const std::string name = c.name;
    std::vector<CellRef> bindings{bind(argObs, "arg.obs", "PinnedRef"),
                                  bind(selFirst, "arg.first", "Str")};
    std::vector<CellRef> body;
    auto resultSchema = recordSchema({pinnedField("best", "obs")});
    if (name == "cursor-result") {
      bindings.push_back(bind(best, "result.best", "Cursor"));
      body.push_back(op(OpcodeKind::Point, {argObs}, {best}));
    } else if (name == "float-found") {
      bindings.push_back(bind(cur, "temp.cur", "Cursor"));
      bindings.push_back(bind(flag, "temp.flag", "Float"));
      body.push_back(op(OpcodeKind::Link, {argObs, selFirst}, {cur, flag}));
    } else if (name == "result-source") {
      bindings.push_back(bind(best, "result.best", "PinnedRef"));
      bindings.push_back(bind(cur, "temp.cur", "Cursor"));
      body.push_back(op(OpcodeKind::Point, {best}, {cur}));
    } else {
      resultSchema = recordSchema({field("best", "Cursor")});
      bindings.push_back(bind(cur, "temp.cur", "Cursor"));
      body.push_back(op(OpcodeKind::Point, {argObs}, {cur}));
    }
    install(recordSchema({pinnedField("obs", "obs"), str("first")}),
            resultSchema, bindings, body,
            {{"obs", handle(root)}, {"first", arena.makeCell("first")}});
    auto result = vm.step(cursor);
    EXPECT_EQ(result.scopedRefusal, c.expected);
    EXPECT_EQ(arena.outstandingMarks(), 0u);
    EXPECT_EQ(outcome(), "Empty");
  }
}
TEST_F(ScopedEvaluationTest, OpcodeNumbersAppendAndDecodeStable) {
  EXPECT_EQ(static_cast<unsigned>(OpcodeKind::ScopedEvaluate),
            static_cast<unsigned>(OpcodeKind::Sqrt) + 1);
  auto op = arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::ScopedEvaluate));
  EXPECT_EQ(vm.getOpcodeKind(op), OpcodeKind::ScopedEvaluate);
  auto unknown = arena.makeScalarCell(
      static_cast<std::int64_t>(OpcodeKind::ScopedEvaluate) + 1);
  EXPECT_FALSE(vm.getOpcodeKind(unknown));
}
} // namespace
} // namespace zigzag::vortex
