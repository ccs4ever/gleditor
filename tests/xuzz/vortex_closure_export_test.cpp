#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"

namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::ClosureDescriptorSpec;
using zigzag::vortex::ClosureFrameInput;
using zigzag::vortex::ClosureInputSource;
using zigzag::vortex::FrameDescriptorSpec;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_closures_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }
};

TEST(VortexClosureExportTest,
     ProgramReferencesPreserveClosureCaptureStreamsAcrossReload) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  std::vector<CellRef> originalCaptures;
  std::vector<CellRef> originalApplied;
  std::vector<zigzag::vortex::CellValue> expectedCaptures;
  std::vector<zigzag::vortex::CellValue> expectedApplied;
  {
    Harness h;
    const auto frameCapture = h.arena.makeCell("frame capture stream");
    const auto frameApplied = h.arena.makeCell("frame applied stream");
    const auto ret          = h.op(OpcodeKind::ReturnFrame);
    const auto frame        = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
               .entry         = ret,
               .locals        = {frameCapture, frameApplied},
               .argumentPorts = {{frameCapture, FramePortKind::Stream},
                                 {frameApplied, FramePortKind::Stream}},
               .resultPorts   = {{frameCapture, FramePortKind::Stream},
                                 {frameApplied, FramePortKind::Stream}},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto closureDescriptor =
        h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
            .frameDescriptor    = *frame,
            .captureStreamCount = 1,
            .appliedStreamCount = 1,
            .frameInputs        = {{ClosureInputSource::Capture, 0},
                                   {ClosureInputSource::Applied, 0}},
        });
    ASSERT_TRUE(closureDescriptor);

    const auto repeatedCapture = h.arena.makeCell("capture α");
    const auto typedDouble     = h.arena.makeScalarCell(1.5);
    const auto misleadingText  = h.arena.intern("not a numeric rendering");
    const std::vector<xanadu::PrimediaSpan> doubleContent{misleadingText};
    ASSERT_TRUE(h.arena.setContent(typedDouble, doubleContent));
    originalCaptures = {repeatedCapture,
                        h.arena.makeScalarCell(std::int64_t{41}),
                        repeatedCapture, typedDouble};
    originalApplied  = {h.arena.makeCell("applied β"),
                        h.arena.makeScalarCell(std::int64_t{99}),
                        h.arena.makeCell("applied β")};
    expectedCaptures = {zigzag::vortex::CellValue{std::string{"capture α"}},
                        zigzag::vortex::CellValue{std::int64_t{41}},
                        zigzag::vortex::CellValue{std::string{"capture α"}},
                        zigzag::vortex::CellValue{1.5}};
    expectedApplied  = {zigzag::vortex::CellValue{std::string{"applied β"}},
                        zigzag::vortex::CellValue{std::int64_t{99}},
                        zigzag::vortex::CellValue{std::string{"applied β"}}};
    const auto captureStream  = h.arena.makeCell("capture stream source");
    const auto appliedStream  = h.arena.makeCell("applied stream source");
    const auto appendCaptures = h.op(OpcodeKind::StreamAppend);
    for (const auto cell : originalCaptures)
      h.core.bindInput(appendCaptures, cell);
    h.core.bindOutput(appendCaptures, captureStream);
    const auto appendApplied = h.op(OpcodeKind::StreamAppend);
    for (const auto cell : originalApplied)
      h.core.bindInput(appendApplied, cell);
    h.core.bindOutput(appendApplied, appliedStream);
    const auto closureHandle = h.arena.makeCell("closure result slot");
    const auto create        = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *closureDescriptor);
    h.core.bindInput(create, captureStream);
    h.core.bindOutput(create, closureHandle);
    const auto outputCapture = h.arena.makeCell("capture output");
    const auto outputApplied = h.arena.makeCell("applied output");
    const auto apply         = h.op(OpcodeKind::ApplyClosure);
    h.core.bindInput(apply, closureHandle);
    h.core.bindInput(apply, appliedStream);
    h.core.bindOutput(apply, outputCapture);
    h.core.bindOutput(apply, outputApplied);
    const auto halt = h.op(OpcodeKind::Halt);
    h.next(appendCaptures, appendApplied);
    h.next(appendApplied, create);
    h.next(create, apply);
    h.next(apply, halt);
    h.next(h.core.home(), appendCaptures);

    const std::vector<CellRef> program{appendCaptures, appendApplied, create,
                                       apply,          halt,          ret};
    xanadu::Store store(scroll);
    const auto exported =
        zigzag::vortex::exportProgram(h.core, h.vm, program, store);
    EXPECT_FALSE(exported.isZero());
    store.save((temporary.path / "program").string());
  }

  xanadu::Store loaded(scroll);
  loaded.load((temporary.path / "program").string());
  ASSERT_TRUE(loaded.structureHead());
  const auto manifold = loaded.rebuildManifold(*loaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, loaded);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto first =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(first);
  std::vector<CellRef> graph;
  auto next = first;
  while (next) {
    graph.push_back(*next);
    next = arena.linked(*next, core.dims().spin, DimVector::POS);
  }
  ASSERT_GE(graph.size(), 5u);
  ASSERT_EQ(vm.getOpcodeKind(graph[2]), OpcodeKind::CreateClosure);
  ASSERT_EQ(vm.getOpcodeKind(graph[3]), OpcodeKind::ApplyClosure);

  const auto cursor    = vm.spawnCursor(*first, "reloaded-closure");
  const auto execution = vm.run(cursor, 200);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto outputs = core.outputsOf(graph[3]);
  ASSERT_EQ(outputs.size(), 2u);
  const auto captured = vm.resultCells(cursor, outputs[0]);
  const auto applied  = vm.resultCells(cursor, outputs[1]);
  ASSERT_EQ(captured.size(), originalCaptures.size());
  ASSERT_EQ(applied.size(), originalApplied.size());
  EXPECT_NE(captured.front(), originalCaptures.front());
  EXPECT_NE(applied.front(), originalApplied.front());
  EXPECT_EQ(captured[0], captured[2]);
  for (std::size_t i = 0; i < captured.size(); ++i)
    EXPECT_EQ(core.render(captured[i]), std::optional{expectedCaptures[i]});
  for (std::size_t i = 0; i < applied.size(); ++i)
    EXPECT_EQ(core.render(applied[i]), std::optional{expectedApplied[i]});
  EXPECT_EQ(arena.asInt64(captured[1]), std::optional{std::int64_t{41}});
  EXPECT_EQ(arena.asInt64(applied[1]), std::optional{std::int64_t{99}});
  EXPECT_EQ(arena.asDouble(captured[3]), std::optional{1.5});
  EXPECT_EQ(arena.textOf(captured[3]), "not a numeric rendering");
}

TEST(VortexClosureExportTest,
     ClosedFrameReturnedClosureHandleAppliesInFreshReloadedVM) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const auto innerArgument = h.arena.makeCell("inner capture");
    const auto innerReturn   = h.op(OpcodeKind::ReturnFrame);
    const auto unusedReturn  = h.op(OpcodeKind::ReturnFrame);
    const auto innerBranch   = h.op(OpcodeKind::BranchFalseCell);
    h.core.bindInput(innerBranch, h.arena.makeScalarCell(false));
    h.core.bindInput(innerBranch, innerReturn);
    h.next(innerBranch, unusedReturn);
    const auto innerFrame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = innerBranch,
        .locals        = {innerArgument},
        .argumentPorts = {{innerArgument, FramePortKind::Stream}},
        .resultPorts   = {{innerArgument, FramePortKind::Stream}},
    });
    ASSERT_TRUE(innerFrame);
    h.core.bindInput(innerReturn, *innerFrame);
    h.core.bindInput(unusedReturn, *innerFrame);
    const auto closureDescriptor =
        h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
            .frameDescriptor    = *innerFrame,
            .captureStreamCount = 1,
            .appliedStreamCount = 0,
            .frameInputs        = {{ClosureInputSource::Capture, 0}},
        });
    ASSERT_TRUE(closureDescriptor);

    const auto frameInput   = h.arena.makeCell("value boxed inside frame");
    const auto localData    = h.arena.makeCell("frame local data");
    const auto closureLocal = h.arena.makeCell("frame local closure");
    const auto box          = h.op(OpcodeKind::VqlBoxScalar);
    h.core.bindInput(box, frameInput);
    h.core.bindOutput(box, localData);
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *closureDescriptor);
    h.core.bindInput(create, localData);
    h.core.bindOutput(create, closureLocal);
    const auto outerReturn = h.op(OpcodeKind::ReturnFrame);
    h.next(box, create);
    h.next(create, outerReturn);
    const auto outerFrame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry       = box,
        .locals      = {localData, closureLocal},
        .resultPorts = {closureLocal},
    });
    ASSERT_TRUE(outerFrame);
    h.core.bindInput(outerReturn, *outerFrame);

    const auto invoke = h.op(OpcodeKind::InvokeFrame);
    h.core.bindInput(invoke, *outerFrame);
    const auto returnedHandle = h.arena.makeCell("returned handle slot");
    h.core.bindOutput(invoke, returnedHandle);
    const auto applyOutput = h.arena.makeCell("fresh vm result");
    const auto apply       = h.op(OpcodeKind::ApplyClosure);
    h.core.bindInput(apply, returnedHandle);
    h.core.bindOutput(apply, applyOutput);
    const auto halt = h.op(OpcodeKind::Halt);
    h.next(invoke, apply);
    h.next(apply, halt);
    h.next(h.core.home(), invoke);
    const auto originalCursor =
        h.vm.spawnCursor(invoke, "closure-export-source");
    const auto originalRun = h.vm.run(originalCursor, 100);
    ASSERT_TRUE(originalRun.success) << originalRun.errorMessage;
    const auto originalClosure =
        h.vm.resultCell(originalCursor, returnedHandle);
    ASSERT_TRUE(originalClosure);
    ASSERT_TRUE(h.vm.closureValue(*originalClosure));
    h.vm.setCursorOpcode(originalCursor, std::nullopt);

    xanadu::Store store(scroll);
    const auto before = store.latest();
    const std::vector<CellRef> runtimeBoundProgram{apply};
    EXPECT_THROW((void)zigzag::vortex::exportProgram(
                     h.core, h.vm, runtimeBoundProgram, store),
                 std::runtime_error);
    EXPECT_EQ(store.latest(), before);

    // A materialized value is portable graph data; the VM register referring
    // to it requires a separate runtime-binding initialization protocol.
    const auto portableApply  = h.op(OpcodeKind::ApplyClosure);
    const auto portableOutput = h.arena.makeCell("portable result");
    h.core.bindInput(portableApply, *originalClosure);
    h.core.bindOutput(portableApply, portableOutput);
    const auto portableHalt = h.op(OpcodeKind::Halt);
    h.next(portableApply, portableHalt);
    h.next(h.core.home(), portableApply);
    const std::vector<CellRef> program{portableApply, portableHalt};
    const auto exported =
        zigzag::vortex::exportProgram(h.core, h.vm, program, store);
    EXPECT_FALSE(exported.isZero());
    store.save((temporary.path / "materialized-closure").string());
  }

  xanadu::Store loaded(scroll);
  loaded.load((temporary.path / "materialized-closure").string());
  ASSERT_TRUE(loaded.structureHead());
  const auto manifold = loaded.rebuildManifold(*loaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, loaded);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto apply =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(apply);
  ASSERT_EQ(vm.getOpcodeKind(*apply), OpcodeKind::ApplyClosure);
  const auto cursor    = vm.spawnCursor(*apply, "closure-export-destination");
  const auto execution = vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto outputs = core.outputsOf(*apply);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(cursor, outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(core.render(values.front()),
            std::optional{zigzag::vortex::CellValue{
                std::string{"value boxed inside frame"}}});
}

TEST(VortexClosureExportTest,
     NumericChoiceInDisconnectedFrameRefusesBeforeStoreWrites) {
  Harness h;
  const auto alternative = h.op(OpcodeKind::ReturnFrame);
  const auto choice      = h.op(OpcodeKind::Choice);
  h.core.bindInput(
      choice, h.arena.makeScalarCell(static_cast<std::int64_t>(alternative)));
  h.next(choice, alternative);
  const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry = choice,
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(alternative, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor = *frame,
  });
  ASSERT_TRUE(descriptor);
  const auto value = h.vm.makeClosureValue(*descriptor, {});
  ASSERT_TRUE(value);
  const auto apply = h.op(OpcodeKind::ApplyClosure);
  h.core.bindInput(apply, *value);
  const std::vector<CellRef> program{apply};
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto before = store.latest();
  EXPECT_THROW(
      (void)zigzag::vortex::exportProgram(h.core, h.vm, program, store),
      std::runtime_error);
  EXPECT_EQ(store.latest(), before);
}

TEST(VortexClosureExportTest,
     RuntimeBoundStreamHandleRefusesBeforeStoreWrites) {
  Harness h;
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  const auto frame =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = ret});
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(
      ClosureDescriptorSpec{.frameDescriptor = *frame});
  ASSERT_TRUE(descriptor);
  const auto value = h.vm.makeClosureValue(*descriptor, {});
  ASSERT_TRUE(value);
  const auto handleStream = h.arena.makeCell("runtime handle stream");
  const auto append       = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(append, *value);
  h.core.bindOutput(append, handleStream);
  const auto apply = h.op(OpcodeKind::ApplyClosure);
  h.core.bindInput(apply, handleStream);
  const auto halt = h.op(OpcodeKind::Halt);
  h.next(append, apply);
  h.next(apply, halt);
  const auto cursor    = h.vm.spawnCursor(append, "bound-stream-export");
  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  h.vm.setCursorOpcode(cursor, std::nullopt);
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto before = store.latest();
  const std::vector<CellRef> program{apply, halt};
  EXPECT_THROW(
      (void)zigzag::vortex::exportProgram(h.core, h.vm, program, store),
      std::runtime_error);
  EXPECT_EQ(store.latest(), before);
}

} // namespace
