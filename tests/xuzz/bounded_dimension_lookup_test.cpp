/**
 * @file bounded_dimension_lookup_test.cpp
 * @brief Refusal and read-only guarantees for bounded dimension lookup.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::SpanReader;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::DimensionLookupBudget;
using zigzag::DimensionLookupLimits;
using zigzag::DimensionLookupRefusal;
using zigzag::DimensionRegistry;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::Manifold;

class CountingReader final : public SpanReader {
public:
  explicit CountingReader(const Store &store) : store_{store} {}

  [[nodiscard]] std::string
  read(const xanadu::PrimediaSpan &span) const override {
    ++reads;
    auto text = store_.read(span);
    bytesRead += text.size();
    return text;
  }

  mutable std::size_t reads{};
  mutable std::size_t bytesRead{};

private:
  const Store &store_;
};

class ExpandedSpanReader final : public SpanReader {
public:
  ExpandedSpanReader(const Store &store, const PrimediaSpan target,
                     std::string expandedText)
      : store_{store}, target_{target}, expandedText_{std::move(expandedText)} {
  }

  [[nodiscard]] std::string read(const PrimediaSpan &span) const override {
    ++reads;
    if (span == target_) {
      return expandedText_;
    }
    return store_.read(span);
  }

  mutable std::size_t reads{};

private:
  const Store &store_;
  PrimediaSpan target_;
  std::string expandedText_;
};

struct DimensionFixture {
  Store store;
  MicroversionId head{store.sliceGenesis(MicroversionId{})};
  std::vector<DimRef> dimensions;

  DimRef add(const std::string_view name) {
    const auto created = store.makeDimension(head, name);
    head               = created.version;
    dimensions.push_back(created.dim);
    return created.dim;
  }

  Manifold fold() { return store.rebuildManifold(head); }
};

DimensionLookupLimits generousLimits() {
  return DimensionLookupLimits{.directoryCells = 4096,
                               .contentSpans   = 4096,
                               .bytes          = 1'048'576,
                               .work           = 16'384};
}

TEST(BoundedDimensionLookupTest, MissingNameIsNotARefusal) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};

  const auto result =
      manifold.dimensionNamedBounded("d.absent", reader, generousLimits());

  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST(BoundedDimensionLookupTest, HomeCellIsNotANameInTheDimensionDirectory) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  ASSERT_TRUE(manifold.home());
  const auto homeText = manifold.textOf(*manifold.home(), fixture.store);
  for (const auto dimension : manifold.dimensions()) {
    EXPECT_NE(manifold.textOf(dimension, fixture.store), homeText);
  }
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  CountingReader reader{fixture.store};

  const auto bounded =
      manifold.dimensionNamedBounded(homeText, reader, generousLimits());

  ASSERT_TRUE(bounded);
  EXPECT_FALSE(*bounded);
  EXPECT_EQ(manifold.dimensionNamed(homeText, reader), std::nullopt);
}

TEST(BoundedDimensionLookupTest, ExactTextAndOneHopAliasMatch) {
  DimensionFixture fixture;
  const auto canonical  = fixture.add("d.canonical");
  const auto aliasAxis  = fixture.add("d.alias");
  fixture.head          = fixture.store.makeCell(fixture.head, "d.nickname");
  const auto nickname   = fixture.store.cellRefOf(fixture.head).value();
  fixture.head          = fixture.store.makeCell(fixture.head, "d.transitive");
  const auto transitive = fixture.store.cellRefOf(fixture.head).value();
  fixture.head  = fixture.store.setLink(fixture.head, canonical, aliasAxis,
                                        DimVector::POS, nickname);
  fixture.head  = fixture.store.setLink(fixture.head, nickname, aliasAxis,
                                        DimVector::POS, transitive);
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};

  const auto exact =
      manifold.dimensionNamedBounded("d.canonical", reader, generousLimits());
  const auto alias =
      manifold.dimensionNamedBounded("d.nickname", reader, generousLimits());
  const auto transitiveResult =
      manifold.dimensionNamedBounded("d.transitive", reader, generousLimits());

  ASSERT_TRUE(exact);
  ASSERT_TRUE(alias);
  ASSERT_TRUE(transitiveResult);
  EXPECT_EQ(*exact, canonical);
  EXPECT_EQ(*alias, canonical);
  EXPECT_FALSE(*transitiveResult);
}

TEST(BoundedDimensionLookupTest, DuplicateNamesKeepFirstDirectoryWinner) {
  DimensionFixture fixture;
  const auto first  = fixture.add("d.duplicate");
  const auto second = fixture.add("d.duplicate");
  auto manifold     = fixture.fold();
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  CountingReader reader{fixture.store};

  const auto result =
      manifold.dimensionNamedBounded("d.duplicate", reader, generousLimits());

  ASSERT_TRUE(result);
  EXPECT_EQ(*result, first);
  EXPECT_NE(*result, second);
}

TEST(BoundedDimensionLookupTest,
     LegacyLookupAcceptsExpandedReaderTextButBoundedLookupRefusesIt) {
  DimensionFixture fixture;
  const auto dimension = fixture.add("d.x");
  auto manifold        = fixture.fold();
  const auto spans     = manifold.contentOf(dimension);
  ASSERT_EQ(spans.size(), 1U);
  const std::string expandedText{"d.expanded"};
  ExpandedSpanReader reader{fixture.store, spans.front(), expandedText};
  DimensionRegistry::instance().unregisterStore(&fixture.store);

  EXPECT_EQ(manifold.dimensionNamed(expandedText, reader), dimension);
  DimensionRegistry::instance().unregisterStore(&fixture.store);

  reader.reads  = 0;
  auto limited  = generousLimits();
  limited.bytes = expandedText.size();
  const auto preflight =
      manifold.dimensionNamedBounded(expandedText, reader, limited);
  ASSERT_FALSE(preflight);
  EXPECT_EQ(preflight.error(), DimensionLookupRefusal::ByteLimitExceeded);
  EXPECT_EQ(reader.reads, 0U);

  const auto bounded =
      manifold.dimensionNamedBounded(expandedText, reader, generousLimits());
  ASSERT_FALSE(bounded);
  EXPECT_EQ(bounded.error(), DimensionLookupRefusal::ByteLimitExceeded);

  ArenaManifold arena;
  const auto space          = arena.attach(zigzag::Space{.manifold = &manifold,
                                                         .store    = &fixture.store,
                                                         .reader   = &reader,
                                                         .label    = "expanded reader"});
  const auto arenaDimension = arena.ensureDimension(expandedText);
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  EXPECT_NO_THROW(EXPECT_EQ(arena.dimIn(space, arenaDimension), dimension));
}

TEST(BoundedDimensionLookupTest, EachResourceLimitHasATypedRefusal) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};

  auto limits           = generousLimits();
  limits.directoryCells = 0;
  const auto directoryRefusal =
      manifold.dimensionNamedBounded("d.missing", reader, limits);
  ASSERT_FALSE(directoryRefusal);
  EXPECT_EQ(directoryRefusal.error(),
            DimensionLookupRefusal::DirectoryLimitExceeded);

  limits              = generousLimits();
  limits.contentSpans = 0;
  reader.reads        = 0;
  const auto spanRefusal =
      manifold.dimensionNamedBounded("d.missing", reader, limits);
  ASSERT_FALSE(spanRefusal);
  EXPECT_EQ(spanRefusal.error(),
            DimensionLookupRefusal::ContentSpanLimitExceeded);
  EXPECT_EQ(reader.reads, 0U);

  limits       = generousLimits();
  limits.bytes = std::string_view{"d.missing"}.size() - 1;
  reader.reads = 0;
  const auto byteRefusal =
      manifold.dimensionNamedBounded("d.missing", reader, limits);
  ASSERT_FALSE(byteRefusal);
  EXPECT_EQ(byteRefusal.error(), DimensionLookupRefusal::ByteLimitExceeded);
  EXPECT_EQ(reader.reads, 0U);

  limits      = generousLimits();
  limits.work = 0;
  const auto workRefusal =
      manifold.dimensionNamedBounded("d.missing", reader, limits);
  ASSERT_FALSE(workRefusal);
  EXPECT_EQ(workRefusal.error(), DimensionLookupRefusal::WorkLimitExceeded);
}

TEST(BoundedDimensionLookupTest, CachedHitStillChargesRequestBytesAndWork) {
  DimensionFixture fixture;
  const auto dimension = fixture.add("d.cached");
  auto manifold        = fixture.fold();
  DimensionRegistry::instance().registerDim(fixture.store, "d.cached",
                                            dimension);
  CountingReader reader{fixture.store};

  auto limits           = generousLimits();
  limits.directoryCells = 0;
  limits.contentSpans   = 0;
  limits.bytes          = std::string_view{"d.cached"}.size();
  limits.work           = std::string_view{"d.cached"}.size();
  const auto result =
      manifold.dimensionNamedBounded("d.cached", reader, limits);

  ASSERT_TRUE(result);
  EXPECT_EQ(*result, dimension);
  EXPECT_EQ(reader.reads, 0U);
  EXPECT_EQ(reader.bytesRead, 0U);

  limits.bytes--;
  const auto byteRefusal =
      manifold.dimensionNamedBounded("d.cached", reader, limits);
  ASSERT_FALSE(byteRefusal);
  EXPECT_EQ(byteRefusal.error(), DimensionLookupRefusal::ByteLimitExceeded);

  limits.bytes = std::string_view{"d.cached"}.size();
  limits.work  = std::string_view{"d.cached"}.size() - 1;
  const auto workRefusal =
      manifold.dimensionNamedBounded("d.cached", reader, limits);
  ASSERT_FALSE(workRefusal);
  EXPECT_EQ(workRefusal.error(), DimensionLookupRefusal::WorkLimitExceeded);
}

TEST(BoundedDimensionLookupTest, Utf8RequestNameUsesItsEncodedByteLength) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  const std::string_view utf8Name{"d.é"};
  auto limits  = generousLimits();
  limits.bytes = utf8Name.size() - 1;

  const auto refusal = manifold.dimensionNamedBounded(utf8Name, reader, limits);

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::ByteLimitExceeded);
  EXPECT_EQ(reader.reads, 0U);
}

TEST(BoundedDimensionLookupTest, FuelCallbackRefusalKeepsEarlierWorkConsumed) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  const auto limits = generousLimits();
  DimensionLookupBudget budget{limits};
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  std::size_t calls = 0;
  const auto charge = [&](const std::size_t) {
    ++calls;
    return calls == 1;
  };
  const gleditor::cpp26::function_ref<bool(std::size_t)> chargeRef{charge};

  const auto refusal = manifold.dimensionNamedBounded(
      "d.present", reader, budget, std::optional{chargeRef});

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::FuelLimitExceeded);
  EXPECT_EQ(calls, 2U);
  EXPECT_LT(budget.workRemaining, limits.work);
  EXPECT_EQ(reader.reads, 0U);
}

TEST(BoundedDimensionLookupTest, ThrowingFuelCallbackBecomesTypedRefusal) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  const auto limits = generousLimits();
  DimensionLookupBudget budget{limits};
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  const auto charge = [](const std::size_t) -> bool {
    throw std::runtime_error("test fuel callback");
  };
  const gleditor::cpp26::function_ref<bool(std::size_t)> chargeRef{charge};

  const auto refusal = manifold.dimensionNamedBounded(
      "d.present", reader, budget, std::optional{chargeRef});

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::FuelLimitExceeded);
  EXPECT_EQ(budget.workRemaining, limits.work);
  EXPECT_EQ(reader.reads, 0U);
}

TEST(BoundedDimensionLookupTest, OversizedRequestNameRefusesBeforeReading) {
  DimensionFixture fixture;
  fixture.add("d.present");
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  const std::string oversizedName(1024, 'x');
  auto limits  = generousLimits();
  limits.bytes = 128;

  const auto refusal =
      manifold.dimensionNamedBounded(oversizedName, reader, limits);

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::ByteLimitExceeded);
  EXPECT_EQ(reader.reads, 0U);
}

TEST(BoundedDimensionLookupTest, FragmentedNameIsRefusedBeforeReadingSpans) {
  DimensionFixture fixture;
  const auto fragmented = fixture.add("d.fragmented");
  fixture.head  = fixture.store.spliceCell(fixture.head, fragmented, 2, 1, "X");
  auto manifold = fixture.fold();
  ASSERT_GE(manifold.contentOf(fragmented).size(), 2U);
  CountingReader reader{fixture.store};
  auto limits = generousLimits();
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  const auto dimsDimension = manifold.dimensionNamed("d.dims", fixture.store);
  ASSERT_TRUE(dimsDimension);
  const auto bootstrapSpanCount = manifold.contentOf(*dimsDimension).size();
  limits.contentSpans =
      bootstrapSpanCount + manifold.contentOf(fragmented).size() - 1;

  const auto refusal =
      manifold.dimensionNamedBounded("d.Xragmented", reader, limits);

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::ContentSpanLimitExceeded);
  EXPECT_EQ(reader.reads, bootstrapSpanCount);
}

TEST(BoundedDimensionLookupTest, SharedBudgetDoesNotResetBetweenNames) {
  DimensionFixture fixture;
  const auto first  = fixture.add("d.first");
  const auto second = fixture.add("d.second");
  auto manifold     = fixture.fold();
  DimensionRegistry::instance().registerDim(fixture.store, "d.first", first);
  DimensionRegistry::instance().registerDim(fixture.store, "d.second", second);
  CountingReader reader{fixture.store};
  const std::string_view firstName{"d.first"};
  const std::string_view secondName{"d.second"};
  DimensionLookupLimits limits = generousLimits();
  limits.directoryCells        = 0;
  limits.contentSpans          = 0;
  limits.bytes                 = firstName.size() + secondName.size() - 1;
  DimensionLookupBudget budget{limits};

  const auto firstResult =
      manifold.dimensionNamedBounded(firstName, reader, budget);
  const auto secondResult =
      manifold.dimensionNamedBounded(secondName, reader, budget);

  ASSERT_TRUE(firstResult);
  EXPECT_EQ(*firstResult, first);
  ASSERT_FALSE(secondResult);
  EXPECT_EQ(secondResult.error(), DimensionLookupRefusal::ByteLimitExceeded);
  EXPECT_EQ(budget.bytesRemaining, secondName.size() - 1);
  EXPECT_EQ(reader.reads, 0U);
}

TEST(BoundedDimensionLookupTest, BoundedLookupDoesNotRegisterOrMutateStore) {
  DimensionFixture fixture;
  const auto canonical = fixture.add("d.canonical");
  const auto aliasAxis = fixture.add("d.alias");
  fixture.head         = fixture.store.makeCell(fixture.head, "d.nickname");
  const auto nickname  = fixture.store.cellRefOf(fixture.head).value();
  fixture.head  = fixture.store.setLink(fixture.head, canonical, aliasAxis,
                                        DimVector::POS, nickname);
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  auto &registry = DimensionRegistry::instance();
  registry.unregisterStore(&fixture.store);
  const auto versionBefore = fixture.store.primaryCurrentVersion();
  const auto countBefore   = manifold.cellCount();

  const auto result =
      manifold.dimensionNamedBounded("d.nickname", reader, generousLimits());

  ASSERT_TRUE(result);
  EXPECT_EQ(*result, canonical);
  EXPECT_FALSE(registry.get(fixture.store, "d.nickname"));
  EXPECT_EQ(fixture.store.primaryCurrentVersion(), versionBefore);
  EXPECT_EQ(manifold.cellCount(), countBefore);
}

TEST(BoundedDimensionLookupTest, FullThirtyTwoDimensionRankFitsDefaultLimits) {
  DimensionFixture fixture;
  for (std::size_t index = 0; index < 32; ++index) {
    fixture.add("d." + std::to_string(index));
  }
  auto manifold = fixture.fold();
  DimensionRegistry::instance().unregisterStore(&fixture.store);
  CountingReader reader{fixture.store};

  const auto result =
      manifold.dimensionNamedBounded("d.31", reader, DimensionLookupLimits{});

  ASSERT_TRUE(result);
  EXPECT_EQ(*result, fixture.dimensions.back());
}

TEST(BoundedDimensionLookupTest, DISABLED_LargeDirectoryLookupProfile) {
  DimensionFixture fixture;
  for (std::size_t index = 0; index < 2048; ++index) {
    fixture.add("d.profile." + std::to_string(index));
  }
  auto manifold = fixture.fold();
  CountingReader reader{fixture.store};
  auto limits           = generousLimits();
  limits.directoryCells = 8192;
  limits.contentSpans   = 16'384;
  limits.bytes          = 1'048'576;
  limits.work           = 200'000;

  DimensionLookupBudget missBudget{limits};
  const auto missStart = std::chrono::steady_clock::now();
  const auto miss =
      manifold.dimensionNamedBounded("d.not-present", reader, missBudget);
  const auto missUs = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - missStart)
                          .count();

  ASSERT_TRUE(miss);
  EXPECT_FALSE(*miss);
  testing::Test::RecordProperty("large_directory_status", "miss");
  testing::Test::RecordProperty(
      "directory_cells_visited",
      std::to_string(limits.directoryCells -
                     missBudget.directoryCellsRemaining));
  testing::Test::RecordProperty(
      "content_spans_visited",
      std::to_string(limits.contentSpans - missBudget.contentSpansRemaining));
  testing::Test::RecordProperty(
      "lookup_bytes_budget_charged",
      std::to_string(limits.bytes - missBudget.bytesRemaining));
  testing::Test::RecordProperty("reader_bytes_read",
                                std::to_string(reader.bytesRead));
  testing::Test::RecordProperty("reader_calls", std::to_string(reader.reads));
  testing::Test::RecordProperty(
      "work_charged", std::to_string(limits.work - missBudget.workRemaining));
  testing::Test::RecordProperty("raw_miss_us", std::to_string(missUs));

  limits.directoryCells = 256;
  DimensionLookupBudget cappedBudget{limits};
  reader.reads     = 0;
  reader.bytesRead = 0;

  const auto cappedStart = std::chrono::steady_clock::now();
  const auto refusal =
      manifold.dimensionNamedBounded("d.not-present", reader, cappedBudget);
  const auto cappedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - cappedStart)
                            .count();

  ASSERT_FALSE(refusal);
  EXPECT_EQ(refusal.error(), DimensionLookupRefusal::DirectoryLimitExceeded);
  testing::Test::RecordProperty("capped_status", "refusal");
  testing::Test::RecordProperty("refusal_directory_cap",
                                std::to_string(limits.directoryCells));
  testing::Test::RecordProperty(
      "capped_directory_cells_visited",
      std::to_string(limits.directoryCells -
                     cappedBudget.directoryCellsRemaining));
  testing::Test::RecordProperty(
      "capped_content_spans_visited",
      std::to_string(limits.contentSpans - cappedBudget.contentSpansRemaining));
  testing::Test::RecordProperty(
      "capped_lookup_bytes_budget_charged",
      std::to_string(limits.bytes - cappedBudget.bytesRemaining));
  testing::Test::RecordProperty("capped_reader_bytes_read",
                                std::to_string(reader.bytesRead));
  testing::Test::RecordProperty("capped_reader_calls",
                                std::to_string(reader.reads));
  testing::Test::RecordProperty(
      "capped_work_charged",
      std::to_string(limits.work - cappedBudget.workRemaining));
  testing::Test::RecordProperty("raw_capped_us", std::to_string(cappedUs));
}

} // namespace
