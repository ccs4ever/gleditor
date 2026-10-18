/**
 * @file edl_transform_test.cpp
 * @brief Unit tests for the Edit Decision List (EDL) transformation monoid.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/enfilade/edl_transform.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/version.hpp"

namespace {

using xanadu::breakMarkerScroll;
using xanadu::localScroll;
using xanadu::OpKind;
using xanadu::PrimediaSpan;
using xanadu::Version;
using xanadu::enfilade::EdlSlice;
using xanadu::enfilade::EdlTransform;
using xanadu::enfilade::SliceKind;

// Simple span reader for materialize tests
class MockSpanReader : public xanadu::SpanReader {
public:
  void setScrollData(const std::string &data) { data_ = data; }

  [[nodiscard]] std::string read(const PrimediaSpan &span) const override {
    if (span.scroll == breakMarkerScroll || span.empty()) {
      return {};
    }
    if (span.start >= data_.size()) {
      return {};
    }
    const auto len = std::min(
        span.length, static_cast<std::uint64_t>(data_.size() - span.start));
    return data_.substr(span.start, len);
  }

private:
  std::string data_;
};

TEST(EdlTransformTest, IdentityTransform) {
  const auto t0 = EdlTransform::identity(0);
  EXPECT_EQ(t0.inputLength(), 0U);
  EXPECT_EQ(t0.outputLength(), 0U);
  EXPECT_TRUE(t0.empty());

  const auto t10 = EdlTransform::identity(10);
  EXPECT_EQ(t10.inputLength(), 10U);
  EXPECT_EQ(t10.outputLength(), 10U);
  ASSERT_EQ(t10.slices().size(), 1U);
  EXPECT_EQ(t10.slices()[0].kind, SliceKind::Source);
  EXPECT_EQ(t10.slices()[0].sourceOffset, 0U);
  EXPECT_EQ(t10.slices()[0].length, 10U);
}

TEST(EdlTransformTest, InsertAndCoalesce) {
  auto t = EdlTransform::identity(0);
  t.insert(0, PrimediaSpan{localScroll, 0, 5}); // "hello"
  t.insert(5, PrimediaSpan{localScroll, 5, 6}); // " world"
  EXPECT_EQ(t.outputLength(), 11U);
  // Contiguous spans in same scroll should coalesce into 1 slice!
  ASSERT_EQ(t.slices().size(), 1U);
  EXPECT_EQ(t.slices()[0].kind, SliceKind::Primedia);
  EXPECT_EQ(t.slices()[0].span.start, 0U);
  EXPECT_EQ(t.slices()[0].span.length, 11U);

  const auto v = t.materializeState0();
  EXPECT_EQ(v.length(), 11U);
}

TEST(EdlTransformTest, RemoveAndRearrange) {
  auto t = EdlTransform::identity(0);
  t.insert(0, PrimediaSpan{localScroll, 0, 10}); // 0..10
  EXPECT_EQ(t.outputLength(), 10U);

  // Remove middle [3, 7) (length 4)
  const auto removed = t.remove(3, 4);
  EXPECT_EQ(t.outputLength(), 6U);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed[0].span.start, 3U);
  EXPECT_EQ(removed[0].span.length, 4U);

  // Rearrange: move [1, 3) to 4
  t.rearrange(1, 2, 4);
  EXPECT_EQ(t.outputLength(), 6U);
}

TEST(EdlTransformTest, CompositionAssociativity) {
  // Test that (T3 ∘ T2) ∘ T1 == T3 ∘ (T2 ∘ T1)
  const auto t1 = [] {
    auto t = EdlTransform::identity(0);
    t.insert(0, PrimediaSpan{localScroll, 0, 10}); // "0123456789"
    return t;
  }();

  const auto t2 = [] {
    auto t = EdlTransform::identity(10);
    t.insert(
        5, PrimediaSpan{localScroll, 100, 5}); // insert 5 chars at 5 -> len 15
    t.remove(0, 2);                            // delete [0, 2) -> len 13
    return t;
  }();

  const auto t3 = [] {
    auto t = EdlTransform::identity(13);
    t.rearrange(3, 4, 8); // rearrange inside 13
    return t;
  }();

  const auto leftAssoc =
      EdlTransform::compose(EdlTransform::compose(t1, t2), t3);
  const auto rightAssoc =
      EdlTransform::compose(t1, EdlTransform::compose(t2, t3));

  EXPECT_EQ(leftAssoc.inputLength(), rightAssoc.inputLength());
  EXPECT_EQ(leftAssoc.outputLength(), rightAssoc.outputLength());
  ASSERT_EQ(leftAssoc.slices().size(), rightAssoc.slices().size());
  for (std::size_t i = 0; i < leftAssoc.slices().size(); ++i) {
    EXPECT_EQ(leftAssoc.slices()[i], rightAssoc.slices()[i]);
  }
}

TEST(EdlTransformTest, CompositionMatchesSequentialReplay) {
  MockSpanReader reader;
  reader.setScrollData("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

  // Step 1: insert "ABCDE"
  Version v;
  v.insert(0, PrimediaSpan{localScroll, 0, 5});
  auto t1 = EdlTransform::identity(0);
  t1.insert(0, PrimediaSpan{localScroll, 0, 5});

  // Step 2: insert "12345" at 2 -> "AB12345CDE"
  v.insert(2, PrimediaSpan{localScroll, 26, 5});
  auto t2 = EdlTransform::identity(5);
  t2.insert(2, PrimediaSpan{localScroll, 26, 5});

  // Step 3: remove 3 chars at 4 ("345") -> "AB12CDE"
  v.remove(4, 3);
  auto t3 = EdlTransform::identity(10);
  t3.remove(4, 3);

  // Compose all transforms
  const auto comp12    = EdlTransform::compose(t1, t2);
  const auto compTotal = EdlTransform::compose(comp12, t3);

  const auto materialized = compTotal.materializeState0();
  EXPECT_EQ(materialized.length(), v.length());
  EXPECT_EQ(materialized.materialize(reader), v.materialize(reader));
  EXPECT_EQ(materialized.materialize(reader), "AB01CDE");
}

TEST(EdlTransformTest, PageBreakHandling) {
  auto t = EdlTransform::identity(0);
  t.insert(0, PrimediaSpan{localScroll, 0, 10});
  t.insertBreak(5);

  const auto v = t.materializeState0();
  EXPECT_EQ(v.length(), 10U);
  const auto breaks = v.forcedBreaks();
  ASSERT_EQ(breaks.size(), 1U);
  EXPECT_EQ(breaks[0], 5U);
}

} // namespace
