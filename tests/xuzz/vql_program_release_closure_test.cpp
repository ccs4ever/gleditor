/**
 * @file vql_program_release_closure_test.cpp
 * @brief A query that makes closures is released like any other read-only
 * program unless a result or binding holds a closure into it, which
 * VortexVM::closureReferences() answers without charging the run any work.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_copy_out.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;

struct Session {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  std::optional<std::string> retained;

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view query) {
    zigzag::vortex::VortexVM vm{core};
    QueryExecutor executor(engine, vm);
    auto result = executor.execute(query);
    retained    = executor.programRetainedBecause();
    return result;
  }

  std::optional<std::int64_t> intOf(const CellRef cell) const {
    return arena.asInt64(cell);
  }
};

TEST(VqlProgramReleaseClosureTest, TwentyClosureRunsLeaveTheArenaFlat) {
  Session s;
  std::vector<CellRef> held;
  std::size_t heldCells = 0;
  const auto before     = s.arena.cellCount();
  for (int run = 0; run < 20; ++run) {
    const auto result = s.run(
        R"(def run($v: Int) := { let $f := \($x) $x; return $f($v); }; return run(5))");
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(s.intOf(result->front()), std::optional<std::int64_t>{5});
    EXPECT_FALSE(s.retained) << s.retained.value_or("");
    held.insert(held.end(), result->begin(), result->end());
    heldCells += result->size();
  }
  EXPECT_EQ(s.arena.cellCount() - before, heldCells);
}

TEST(VqlProgramReleaseClosureTest, AResultThatIsAClosureRetainsByName) {
  Session s;
  const auto made = s.run(R"(let $f := \($x) $x; return $f)");
  ASSERT_TRUE(made) << made.error().message;
  ASSERT_TRUE(s.retained);
  EXPECT_NE(s.retained->find("a result holds a closure into the program"),
            std::string::npos)
      << s.retained.value_or("");
  const auto applied = s.run("return $f(4)");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(s.intOf(applied->front()), std::optional<std::int64_t>{4});
}

TEST(VqlProgramReleaseClosureTest, ASessionClosureSurvivesLaterReleases) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) $x; return $f)"));
  const auto before = s.arena.cellCount();
  std::size_t held  = 0;
  std::vector<std::vector<CellRef>> keep;
  for (int run = 0; run < 5; ++run) {
    const auto again = s.run("return $f(3)");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(s.intOf(again->front()), std::optional<std::int64_t>{3});
    EXPECT_FALSE(s.retained) << s.retained.value_or("");
    held += again->size();
    keep.push_back(*again);
  }
  EXPECT_EQ(s.arena.cellCount() - before, held);
  const auto last = s.run("return $f(9)");
  ASSERT_TRUE(last) << last.error().message;
  EXPECT_EQ(s.intOf(last->front()), std::optional<std::int64_t>{9});
}

TEST(VqlProgramReleaseClosureTest, ABoundClosureRetainsByNameAndStaysCallable) {
  Session s;
  const auto made = s.run(R"(let $g := \($x) $x; return 5)");
  ASSERT_TRUE(made) << made.error().message;
  ASSERT_TRUE(s.retained);
  EXPECT_NE(s.retained->find("a session binding holds a closure"),
            std::string::npos)
      << *s.retained;
  const auto applied = s.run("return $g(2)");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(s.intOf(applied->front()), std::optional<std::int64_t>{2});
  EXPECT_FALSE(s.retained);
}

TEST(VqlProgramReleaseClosureTest, AnOlderClosureReturnedAgainStaysCallable) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) $x; return $f)"));
  const auto again = s.run("return $f");
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_FALSE(s.retained) << s.retained.value_or("");
  s.engine.setVariable("g", *again);
  const auto applied = s.run("return $g(6)");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(s.intOf(applied->front()), std::optional<std::int64_t>{6});
}

TEST(VqlProgramReleaseClosureTest, ANewClosureCapturingAnOlderOneIsRetained) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) $x; return $f)"));
  const auto outer = s.run(R"(let $h := \($y) $f($y); return $h)");
  ASSERT_TRUE(outer) << outer.error().message;
  ASSERT_TRUE(s.retained);
  const auto applied = s.run("return $h(12)");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(s.intOf(applied->front()), std::optional<std::int64_t>{12});
}

TEST(VqlProgramReleaseClosureTest,
     ANestedClosureIsReleasedWhenOnlyAnswerLeaves) {
  Session s;
  const auto before = s.arena.cellCount();
  const auto result = s.run(
      R"(def run($n: Int) := { let $add := \($a) \($b) $a + $b; let $inc := $add(1); return $inc($n); }; return run(41))");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(s.intOf(result->front()), std::optional<std::int64_t>{42});
  EXPECT_FALSE(s.retained) << s.retained.value_or("");
  EXPECT_EQ(s.arena.cellCount() - before, result->size());
}

TEST(VqlProgramReleaseClosureTest, AFailureInsideAClosureCallLeaksNothing) {
  Session s;
  const auto before = s.arena.cellCount();
  const auto failed = s.run(R"(let $f := \($x) one($x); return $f(##/d.none))");
  ASSERT_FALSE(failed);
  EXPECT_EQ(s.arena.outstandingMarks(), 0U);
  EXPECT_EQ(s.arena.cellCount(), before);
}

struct VmHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};

  /// A descriptor with one capture stream, which needs a frame port to land in.
  CellRef descriptor() {
    using namespace zigzag::vortex;
    const auto captured = arena.makeCell("captured stream");
    const auto ret      = vm.mintOpcode(OpcodeKind::ReturnFrame);
    const auto frame    = vm.makeFrameDescriptor(FrameDescriptorSpec{
           .entry         = ret,
           .locals        = {captured},
           .argumentPorts = {{captured, FramePortKind::Stream}},
           .resultPorts   = {{captured, FramePortKind::Stream}},
    });
    EXPECT_TRUE(frame);
    core.bindInput(ret, *frame);
    const auto made = vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor    = *frame,
        .captureStreamCount = 1,
        .maxFormalCount     = 0,
        .frameInputs        = {{ClosureInputSource::Capture, 0}},
    });
    EXPECT_TRUE(made);
    return made.value_or(0);
  }
};

TEST(VortexClosureReferencesTest, ListsTheDescriptorAndEveryCapturedCell) {
  VmHarness h;
  const auto descriptor = h.descriptor();
  const auto one        = h.arena.makeScalarCell(std::int64_t{1});
  const auto two        = h.arena.makeScalarCell(std::int64_t{2});
  const std::vector<std::vector<CellRef>> captures{{one, two}};
  const auto closure = h.vm.makeClosureValue(descriptor, captures);
  ASSERT_TRUE(closure);
  const auto refs = h.vm.closureReferences(*closure);
  ASSERT_TRUE(refs) << refs.error().message;
  EXPECT_EQ(refs->front(), descriptor);
  for (const auto cell : {one, two})
    EXPECT_NE(std::ranges::find(*refs, cell), refs->end());
  const auto parsed = h.vm.closureDescriptor(descriptor);
  ASSERT_TRUE(parsed);
  EXPECT_NE(std::ranges::find(*refs, parsed->presenceTrue), refs->end());
  EXPECT_NE(std::ranges::find(*refs, parsed->presenceFalse), refs->end());
  EXPECT_EQ(refs->size(), 6U); // plus the frame descriptor
}

TEST(VortexClosureReferencesTest,
     AClosureWithNoCapturesNamesOnlyItsDescriptor) {
  VmHarness h;
  const auto made = h.vm.makeClosureDescriptor(
      zigzag::vortex::ClosureDescriptorSpec{.maxFormalCount = 0});
  ASSERT_TRUE(made);
  const auto closure =
      h.vm.makeClosureValue(*made, std::vector<std::vector<CellRef>>{});
  ASSERT_TRUE(closure);
  const auto refs = h.vm.closureReferences(*closure);
  ASSERT_TRUE(refs) << refs.error().message;
  EXPECT_EQ(refs->size(), 3U);
}

TEST(VortexClosureReferencesTest, RefusesACellThatIsNotAClosureValueByName) {
  VmHarness h;
  const auto plain = h.arena.makeScalarCell(std::int64_t{7});
  const auto text  = h.arena.makeCell("closure");
  const auto refs  = h.vm.closureReferences(plain);
  ASSERT_FALSE(refs);
  EXPECT_NE(refs.error().message.find("not a Vortex closure value"),
            std::string::npos);
  EXPECT_FALSE(h.vm.closureReferences(text));
  // A descriptor is a runtime object but not a closure value.
  EXPECT_FALSE(h.vm.closureReferences(h.descriptor()));
  EXPECT_FALSE(h.vm.closureReferences(CellRef{999999}));
}

TEST(VortexClosureReferencesTest, ChargesNoWorkAndChangesNothing) {
  VmHarness h;
  const auto descriptor = h.descriptor();
  const auto one        = h.arena.makeScalarCell(std::int64_t{1});
  const auto closure    = h.vm.makeClosureValue(
      descriptor, std::vector<std::vector<CellRef>>{{one}});
  ASSERT_TRUE(closure);
  const auto cells = h.arena.cellCount();
  // One unit of fuel: any charged read would exhaust it on the first record,
  // so a thousand calls succeeding shows the accessor draws none.
  constexpr std::size_t fuel  = 1;
  constexpr int calls         = 1000;
  constexpr std::size_t bytes = 1U << 20;
  h.arena.setExecutionLimits(static_cast<std::uint32_t>(cells + 16), fuel,
                             bytes);
  for (int call = 0; call < calls; ++call)
    ASSERT_TRUE(h.vm.closureReferences(*closure)) << "call " << call;
  EXPECT_EQ(h.arena.cellCount(), cells);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  // The fuel is still whole: the first charged read spends it, the second
  // throws.
  EXPECT_NO_THROW(h.arena.consumeWork());
  EXPECT_THROW(h.arena.consumeWork(), std::runtime_error);
}

TEST(VortexClosureReferencesTest, CopyOutSharesOnlyAnOlderClosureTheVmClears) {
  VmHarness h;
  const auto older = h.vm.makeClosureValue(
      h.descriptor(), std::vector<std::vector<CellRef>>{{}});
  ASSERT_TRUE(older);
  const auto floor = h.arena.mark();
  const auto claim = h.core.dims().runtimeObjectClaim;
  // An integer naming an older closure survives the release as it is.
  const auto names = h.arena.makeScalarCell(static_cast<std::int64_t>(*older));
  const std::vector<CellRef> cells{names};
  EXPECT_TRUE(zigzag::CopyOut::capture(h.arena, floor, cells, claim,
                                       [](CellRef) { return true; }));
  const auto refused = zigzag::CopyOut::capture(h.arena, floor, cells, claim,
                                                [](CellRef) { return false; });
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason, zigzag::CopyOutRefusal::RuntimeObject);
  // A closure the program minted is never shared, whatever the predicate says,
  // and neither is one a result links to.
  const auto fresh = h.vm.makeClosureValue(
      h.descriptor(), std::vector<std::vector<CellRef>>{{}});
  ASSERT_TRUE(fresh);
  const auto holder = h.arena.makeCell("holder");
  ASSERT_TRUE(
      h.arena.link(holder, h.core.dims().step, zigzag::DimVector::POS, *fresh));
  for (const CellRef start : {*fresh, holder}) {
    const std::vector<CellRef> minted{start};
    const auto direct = zigzag::CopyOut::capture(h.arena, floor, minted, claim,
                                                 [](CellRef) { return true; });
    ASSERT_FALSE(direct);
    EXPECT_EQ(direct.error().reason, zigzag::CopyOutRefusal::RuntimeObject);
  }
}

} // namespace
} // namespace xanadu::vql
