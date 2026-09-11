/**
 * @file scalar_test.cpp
 * @brief A scalar cell carries both a real span and canonical bits.
 *
 * Migration step 15 of design/store-slice-convergence.md, and R6's claims made
 * checkable: the rendering round-trips exactly, the bits are canonical for
 * value equality only, a signalling NaN is refused, and -- the thing the whole
 * ruling is for -- two cells holding the same number are still two cells at two
 * addresses, because numeric coincidence is not quotation.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <xudu/core/microversion.hpp>
#include <xudu/core/scalar.hpp>
#include <xudu/core/store.hpp>
#include <zigzag/core/manifold.hpp>

namespace {

using xudu::canonicalQuietNaN;
using xudu::MicroversionId;
using xudu::ScalarValue;
using xudu::Store;
using xudu::ValueKind;
using zigzag::CellRef;

/// Doubles worth asserting about individually, rather than a random sample:
/// each one is a case the encoding could get wrong on its own.
const std::vector<double> &interestingDoubles() {
  static const std::vector<double> values{
      0.0,
      1.0,
      -1.0,
      42.0,
      3.14159265358979,
      0.1,
      1.0 / 3.0,
      1e308,
      1e-308,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::denorm_min(), // 4.9406564584124654e-324
      -std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::epsilon(),
      9007199254740993.0, // 2^53 + 1, not representable; rounds
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
  };
  return values;
}

TEST(ScalarTest, aRenderingRoundTripsToTheSameDouble) {
  for (const auto value : interestingDoubles()) {
    const auto scalar = xudu::scalarValue(value);
    double parsed{};
    ASSERT_TRUE(xudu::parseDouble(scalar.text, parsed))
        << value << " rendered as " << scalar.text;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(parsed),
              std::bit_cast<std::uint64_t>(value))
        << "via " << scalar.text;
  }
}

TEST(ScalarTest, theWorstCaseRenderingIsTwentyFourBytesAndNotWhereR6SaidItWas) {
  // R6 names 4.9406564584124654e-324 as the 24-byte worst case. That is
  // printf's %.17g of denorm_min, not its shortest round-trip rendering --
  // nothing else is near enough to it for more digits to be needed, so
  // to_chars says this instead. Worth a test rather than a correction alone:
  // the example being a formatting choice is exactly what R6 says the
  // rendering is not.
  EXPECT_EQ(xudu::scalarValue(-std::numeric_limits<double>::denorm_min()).text,
            "-5e-324");

  // 24 bytes is still the real bound, reached by any value needing all 17
  // significant digits with a three-digit exponent and a sign.
  EXPECT_EQ(xudu::scalarValue(-1.2345678901234567e-308).text.size(), 24U);
  EXPECT_EQ(xudu::scalarValue(std::numeric_limits<double>::lowest()).text,
            "-1.7976931348623157e+308");
  EXPECT_EQ(xudu::scalarValue(std::numeric_limits<double>::lowest()).text.size(),
            24U);
  // And most values are nowhere near it, which is the other half of R6's cost
  // argument: a tenth is three bytes, a third eighteen.
  EXPECT_EQ(xudu::scalarValue(0.1).text, "0.1");
  EXPECT_EQ(xudu::scalarValue(1.0 / 3.0).text, "0.3333333333333333");

  for (const auto value : interestingDoubles()) {
    EXPECT_LE(xudu::scalarValue(value).text.size(), 24U) << value;
  }
}

TEST(ScalarTest, everyNaNIsOneNaNInTheBitsAndNegativeZeroIsZero) {
  const auto quiet = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(xudu::canonicalDoubleBits(quiet), canonicalQuietNaN);
  // A NaN with a payload, and one with the sign bit set: both collapse.
  EXPECT_EQ(xudu::canonicalDoubleBits(
                std::bit_cast<double>(0x7ff8000000c0ffeeULL)),
            canonicalQuietNaN);
  EXPECT_EQ(xudu::canonicalDoubleBits(
                std::bit_cast<double>(0xfff8000000000001ULL)),
            canonicalQuietNaN);

  EXPECT_EQ(xudu::canonicalDoubleBits(-0.0), 0U);
  EXPECT_EQ(xudu::canonicalDoubleBits(0.0), 0U);
  // Canonicalisation is for value equality and nothing else: it does not touch
  // anything that already compares equal to itself.
  for (const auto value : interestingDoubles()) {
    EXPECT_EQ(xudu::canonicalDoubleBits(value),
              std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value))
        << value;
  }
}

TEST(ScalarTest, aSignallingNaNIsRefusedRatherThanQuieted) {
  const auto signalling = std::bit_cast<double>(0x7ff0000000000001ULL);
  ASSERT_TRUE(std::isnan(signalling));
  ASSERT_TRUE(xudu::isSignallingNaN(signalling));
  EXPECT_FALSE(xudu::isSignallingNaN(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(xudu::isSignallingNaN(std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(xudu::isSignallingNaN(1.0));

  EXPECT_THROW(xudu::scalarValue(signalling), std::invalid_argument);

  Store store;
  const auto at = store.sliceGenesis(MicroversionId{});
  EXPECT_THROW(store.makeScalarCell(at, signalling), std::invalid_argument);
  // And nothing was recorded, so the document is the one it was.
  EXPECT_EQ(store.opCount(), 3U);
}

TEST(ScalarTest, aCellCarriesTheBitsAndTheBytesAtOnce) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  for (const auto value : interestingDoubles()) {
    at              = store.makeScalarCell(at, value);
    const auto cell = store.cellRefOf(at);
    const auto manifold = store.rebuildManifold(at);

    // The property step 15 asks for, both halves of it.
    ASSERT_TRUE(manifold.asDouble(cell).has_value()) << value;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(*manifold.asDouble(cell)),
              xudu::canonicalDoubleBits(value))
        << value;
    double parsed{};
    ASSERT_TRUE(xudu::parseDouble(manifold.textOf(cell, store), parsed))
        << value;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(parsed),
              std::bit_cast<std::uint64_t>(value))
        << value;
    EXPECT_EQ(manifold.valueKindOf(cell), ValueKind::Double);
    EXPECT_FALSE(manifold.asBool(cell).has_value());
    EXPECT_FALSE(manifold.asInt64(cell).has_value());
  }
}

TEST(ScalarTest, aNaNCellRendersAsANaNAndComparesEqualByBitsOnly) {
  Store store;
  auto at         = store.sliceGenesis(MicroversionId{});
  at              = store.makeScalarCell(at, std::numeric_limits<double>::quiet_NaN());
  const auto cell = store.cellRefOf(at);

  const auto manifold = store.rebuildManifold(at);
  ASSERT_TRUE(manifold.asDouble(cell).has_value());
  EXPECT_TRUE(std::isnan(*manifold.asDouble(cell)));
  // Not via the value, which compares unequal to itself. That is the arithmetic
  // R6 cites for why "same value implies same address" was never sound.
  EXPECT_NE(*manifold.asDouble(cell), *manifold.asDouble(cell));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*manifold.asDouble(cell)),
            canonicalQuietNaN);
  EXPECT_EQ(manifold.textOf(cell, store), "nan");
}

TEST(ScalarTest, boolsAndIntegersCarryTheirOwnKind) {
  Store store;
  auto at            = store.sliceGenesis(MicroversionId{});
  at                 = store.makeScalarCell(at, true);
  const auto yes     = store.cellRefOf(at);
  at                 = store.makeScalarCell(at, false);
  const auto no      = store.cellRefOf(at);
  at                 = store.makeScalarCell(at, std::int64_t{-9007199254740993});
  const auto integer = store.cellRefOf(at);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_THAT(manifold.asBool(yes), testing::Optional(true));
  EXPECT_THAT(manifold.asBool(no), testing::Optional(false));
  EXPECT_EQ(manifold.textOf(yes, store), "true");
  EXPECT_EQ(manifold.textOf(no, store), "false");

  // A negative integer survives the trip through the unsigned field, which it
  // would not if the bits were converted rather than reinterpreted.
  EXPECT_THAT(manifold.asInt64(integer),
              testing::Optional(std::int64_t{-9007199254740993}));
  EXPECT_EQ(manifold.textOf(integer, store), "-9007199254740993");
  // And it is not readable as the double that cannot represent it.
  EXPECT_FALSE(manifold.asDouble(integer).has_value());
  EXPECT_FALSE(manifold.asBool(integer).has_value());
}

TEST(ScalarTest, twoCellsHoldingOneNumberAreTwoCellsAtTwoAddresses) {
  Store store;
  auto at           = store.sliceGenesis(MicroversionId{});
  at                = store.makeScalarCell(at, 3.14);
  const auto mine   = store.cellRefOf(at);
  at                = store.makeScalarCell(at, 3.14);
  const auto theirs = store.cellRefOf(at);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_NE(mine, theirs);
  EXPECT_EQ(*manifold.asDouble(mine), *manifold.asDouble(theirs));
  EXPECT_EQ(manifold.textOf(mine, store), manifold.textOf(theirs, store));

  // The ruling this test exists for: equal values at *different* primedia
  // addresses. Sharing one would assert a transclusion that never happened,
  // classify it as DiffKind::Universal and light up Identity Gold for two
  // people who merely both typed 3.14.
  const auto *const first  = manifold.slot(mine);
  const auto *const second = manifold.slot(theirs);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first->span.start, second->span.start);
  EXPECT_EQ(first->span.length, second->span.length);
}

TEST(ScalarTest, restatingAScalarSpoolsTheNewRenderingAndKeepsTheCell) {
  Store store;
  auto at         = store.sliceGenesis(MicroversionId{});
  at              = store.makeScalarCell(at, 1.5);
  const auto cell = store.cellRefOf(at);
  const auto before = store.rebuildManifold(at);
  ASSERT_THAT(before.asDouble(cell), testing::Optional(1.5));

  at = store.setScalar(at, cell, std::int64_t{7});
  const auto after = store.rebuildManifold(at);
  // Same cell -- identity is the birth op and a restatement does not mint one.
  EXPECT_EQ(after.cellCount(), before.cellCount());
  EXPECT_FALSE(after.asDouble(cell).has_value());
  EXPECT_THAT(after.asInt64(cell), testing::Optional(std::int64_t{7}));
  EXPECT_EQ(after.textOf(cell, store), "7");
  // And the old rendering is still in the permascroll at its own address, as
  // everything typed is: a scroll is append-only and a restatement is an edit
  // in hypertime, so scrubbing back to `before` still reads 1.5.
  EXPECT_EQ(before.textOf(cell, store), "1.5");
}

TEST(ScalarTest, aScalarCellIsAnOrdinaryCellInEveryOtherWay) {
  Store store;
  auto at            = store.sliceGenesis(MicroversionId{});
  const auto minted  = store.makeDimension(at, "d.1");
  at                 = minted.version;
  at                 = store.makeScalarCell(at, 2.5);
  const auto number  = store.cellRefOf(at);
  at                 = store.makeCell(at, "a label");
  const auto label   = store.cellRefOf(at);
  at                 = store.setLink(at, label, minted.dim, false, number);

  const auto manifold = store.rebuildManifold(at);
  // Linked, ranked and read like anything else -- which is the entire argument
  // for spooling the rendering rather than inventing a payload type.
  EXPECT_EQ(manifold.linked(label, minted.dim, false), number);
  EXPECT_EQ(manifold.linked(number, minted.dim, true), label);
  EXPECT_EQ(manifold.textOf(number, store), "2.5");
  EXPECT_THAT(manifold.asDouble(number), testing::Optional(2.5));
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(store));
}

} // namespace
