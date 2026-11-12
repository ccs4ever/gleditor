/**
 * @file vortex_hypertime_pow_test.cpp
 * @brief Hypertime (the arena's operation high-water mark) and Pow (numeric
 *        power with explicit domain errors).
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

struct Harness {
  explicit Harness(zigzag::ArenaManifold &target)
      : arena(target), core(target), vm(core) {}

  zigzag::ArenaManifold &arena;
  VortexCore core;
  VortexVM vm;

  struct Outcome {
    ExecutionResult result;
    CellValue value;
  };

  Outcome hypertime() {
    const auto op     = vm.mintOpcode(OpcodeKind::Hypertime);
    const auto output = arena.makeCell("hypertime");
    core.bindOutput(op, output);
    const auto result = vm.step(vm.spawnCursor(op));
    return {result, core.render(output).value_or(CellValue{false})};
  }

  Outcome pow(const CellRef base, const CellRef exponent) {
    const auto op     = vm.mintOpcode(OpcodeKind::Pow);
    const auto output = arena.makeCell("power");
    core.bindInput(op, base);
    core.bindInput(op, exponent);
    core.bindOutput(op, output);
    const auto result = vm.step(vm.spawnCursor(op));
    return {result, core.render(output).value_or(CellValue{false})};
  }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }
  CellRef real(const double value) { return arena.makeScalarCell(value); }
};

struct OwnedArena {
  zigzag::ArenaManifold owned;
};

// The arena is a base, so it is built before the core that points at it.
struct ArenaHarness : OwnedArena, Harness {
  ArenaHarness() : Harness(owned) {}
};

// Hypertime ------------------------------------------------------------

TEST(VortexHypertimeTest, ReadsTheArenaCellCountAndGrowsWithMinting) {
  ArenaHarness h;
  const auto first = h.hypertime();
  ASSERT_TRUE(first.result.success) << first.result.errorMessage;
  const auto *const firstValue = std::get_if<std::int64_t>(&first.value);
  ASSERT_NE(firstValue, nullptr);
  EXPECT_GT(*firstValue, 0);
  EXPECT_LE(static_cast<std::size_t>(*firstValue), h.arena.cellCount());

  for (int i = 0; i < 16; ++i) (void)h.arena.makeCell();
  const auto second = h.hypertime();
  ASSERT_TRUE(second.result.success) << second.result.errorMessage;
  const auto *const secondValue = std::get_if<std::int64_t>(&second.value);
  ASSERT_NE(secondValue, nullptr);
  EXPECT_GE(*secondValue, *firstValue + 16);
}

TEST(VortexHypertimeTest, StoreBackedArenaReportsTheFoldedOperationIndex) {
  xanadu::Store store;
  auto at           = store.sliceGenesis(xanadu::MicroversionId{});
  const auto minted = store.makeDimension(at, "d.step");
  at                = minted.version;
  at                = store.makeCell(at, "first");
  const auto first  = store.cellRefOf(at).value();
  at                = store.makeCell(at, "second");
  const auto second = store.cellRefOf(at).value();
  // SetLink mints no cell, so operations and cells now differ.
  at = store.setLink(at, first, minted.dim, DimVector::POS, second);
  const auto base = store.rebuildManifold(at);
  ASSERT_NE(store.opCount(), base.cellCount());
  ASSERT_EQ(base.foldedThrough(), store.opCount());

  zigzag::ArenaManifold arena(&base, &store);
  Harness h(arena);
  const auto before = h.hypertime();
  ASSERT_TRUE(before.result.success) << before.result.errorMessage;
  EXPECT_EQ(before.value,
            CellValue{static_cast<std::int64_t>(store.opCount())});
  EXPECT_NE(before.value,
            CellValue{static_cast<std::int64_t>(base.cellCount())});
  // Ephemeral cells are not operations: minting them leaves the mark alone.
  for (int i = 0; i < 16; ++i) (void)arena.makeCell();
  const auto after = h.hypertime();
  ASSERT_TRUE(after.result.success) << after.result.errorMessage;
  EXPECT_EQ(after.value, before.value);
}

TEST(VortexHypertimeTest, RefusesAnInput) {
  ArenaHarness h;
  const auto op = h.vm.mintOpcode(OpcodeKind::Hypertime);
  h.core.bindInput(op, h.integer(1));
  h.core.bindOutput(op, h.arena.makeCell("hypertime"));
  const auto result = h.vm.step(h.vm.spawnCursor(op));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("Hypertime"), std::string::npos);
}

// Pow ------------------------------------------------------------------

TEST(VortexPowTest, IntegerBaseAndNonNegativeExponentStaysExact) {
  ArenaHarness h;
  EXPECT_EQ(h.pow(h.integer(2), h.integer(10)).value,
            CellValue{std::int64_t{1024}});
  EXPECT_EQ(h.pow(h.integer(-8), h.integer(3)).value,
            CellValue{std::int64_t{-512}});
  EXPECT_EQ(h.pow(h.integer(0), h.integer(0)).value,
            CellValue{std::int64_t{1}});
  EXPECT_EQ(h.pow(h.integer(0), h.integer(5)).value,
            CellValue{std::int64_t{0}});
  EXPECT_EQ(h.pow(h.integer(3), h.integer(39)).value,
            CellValue{std::int64_t{4052555153018976267}});
  EXPECT_EQ(h.pow(h.integer(2), h.integer(62)).value,
            CellValue{std::int64_t{1} << 62});
  // A huge exponent is cheap when the base is 1 or -1.
  EXPECT_EQ(h.pow(h.integer(1), h.integer(std::int64_t{1} << 40)).value,
            CellValue{std::int64_t{1}});
  EXPECT_EQ(h.pow(h.integer(-1), h.integer((std::int64_t{1} << 40) + 1)).value,
            CellValue{std::int64_t{-1}});
}

TEST(VortexPowTest, NegativeIntegerExponentGivesAReal) {
  ArenaHarness h;
  EXPECT_EQ(h.pow(h.integer(2), h.integer(-1)).value, CellValue{0.5});
  EXPECT_EQ(h.pow(h.integer(-2), h.integer(-2)).value, CellValue{0.25});
}

TEST(VortexPowTest, NonIntegerExponentGivesAReal) {
  ArenaHarness h;
  EXPECT_EQ(h.pow(h.integer(9), h.real(0.5)).value, CellValue{3.0});
  EXPECT_EQ(h.pow(h.real(2.0), h.integer(3)).value, CellValue{8.0});
  const auto negativeBaseIntegralExponent = h.pow(h.real(-2.0), h.real(2.0));
  ASSERT_TRUE(negativeBaseIntegralExponent.result.success);
  EXPECT_EQ(negativeBaseIntegralExponent.value, CellValue{4.0});
}

TEST(VortexPowTest, IntegerOverflowFailsLikeMul) {
  ArenaHarness h;
  const auto tooBig = h.pow(h.integer(2), h.integer(63));
  EXPECT_FALSE(tooBig.result.success);
  EXPECT_NE(tooBig.result.errorMessage.find("overflow"), std::string::npos);
  const auto squaring = h.pow(h.integer(std::int64_t{1} << 40), h.integer(2));
  EXPECT_FALSE(squaring.result.success);
}

TEST(VortexPowTest, ZeroToANegativePowerIsAnExplicitFailure) {
  ArenaHarness h;
  for (const auto exponent : {h.integer(-1), h.real(-0.5)}) {
    const auto outcome = h.pow(h.integer(0), exponent);
    EXPECT_FALSE(outcome.result.success);
    EXPECT_NE(outcome.result.errorMessage.find("zero raised to a negative"),
              std::string::npos);
  }
  EXPECT_FALSE(h.pow(h.real(0.0), h.integer(-3)).result.success);
}

TEST(VortexPowTest, NegativeBaseWithNonIntegerExponentIsAnExplicitFailure) {
  ArenaHarness h;
  const auto outcome = h.pow(h.integer(-8), h.real(1.0 / 3.0));
  EXPECT_FALSE(outcome.result.success);
  EXPECT_NE(outcome.result.errorMessage.find("negative base"),
            std::string::npos);
  EXPECT_FALSE(h.pow(h.real(-4.0), h.real(0.5)).result.success);
}

TEST(VortexPowTest, ANonFiniteResultIsAnExplicitFailure) {
  ArenaHarness h;
  const auto outcome = h.pow(h.real(10.0), h.real(400.0));
  EXPECT_FALSE(outcome.result.success);
  EXPECT_NE(outcome.result.errorMessage.find("not finite"), std::string::npos);
}

TEST(VortexPowTest, RefusesNonNumericAndMissingOperands) {
  ArenaHarness h;
  const auto text = h.pow(h.arena.makeCell("two"), h.integer(2));
  EXPECT_FALSE(text.result.success);
  EXPECT_NE(text.result.errorMessage.find("Pow expects"), std::string::npos);

  const auto op = h.vm.mintOpcode(OpcodeKind::Pow);
  h.core.bindInput(op, h.integer(2));
  h.core.bindOutput(op, h.arena.makeCell("power"));
  const auto missing = h.vm.step(h.vm.spawnCursor(op));
  EXPECT_FALSE(missing.success);
  EXPECT_NE(missing.errorMessage.find("Pow expects"), std::string::npos);
}

TEST(VortexPowTest, NegativeBaseReachesInt64MinExactly) {
  ArenaHarness h;
  const auto outcome = h.pow(h.integer(-2), h.integer(63));
  ASSERT_TRUE(outcome.result.success) << outcome.result.errorMessage;
  EXPECT_EQ(outcome.value, CellValue{std::numeric_limits<std::int64_t>::min()});
  EXPECT_FALSE(h.pow(h.integer(2), h.integer(63)).result.success);
}

TEST(VortexPowTest, ZeroToTheZeroIsOneForIntegersAndReals) {
  ArenaHarness h;
  EXPECT_EQ(h.pow(h.integer(0), h.integer(0)).value,
            CellValue{std::int64_t{1}});
  EXPECT_EQ(h.pow(h.real(0.0), h.real(0.0)).value, CellValue{1.0});
  EXPECT_EQ(h.pow(h.real(0.0), h.integer(0)).value, CellValue{1.0});
}

TEST(VortexPowTest, ANaNOperandFailsExplicitly) {
  ArenaHarness h;
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  for (const auto &[base, exponent] : {std::pair{h.real(nan), h.integer(2)},
                                       std::pair{h.real(nan), h.integer(0)},
                                       std::pair{h.integer(2), h.real(nan)}}) {
    const auto outcome = h.pow(base, exponent);
    EXPECT_FALSE(outcome.result.success);
    EXPECT_NE(outcome.result.errorMessage.find("NaN"), std::string::npos);
  }
}

} // namespace
} // namespace zigzag::vortex
