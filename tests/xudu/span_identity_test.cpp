/**
 * @file span_identity_test.cpp
 * @brief Primedia coordinates mean quotation, so identical text does not get
 *        identical coordinates by accident.
 *
 * Was span_deduplication_test.cpp, which asserted the opposite: that typing a
 * passage somebody had already typed would reuse their span. That saved bytes
 * and cost the data model, because in this system two documents at the same
 * primedia address *are* transcluded -- it is what Version::occurrencesOf
 * reports, what the gold beams draw, and what transcopyright would settle
 * royalties against. Textual coincidence is not quotation.
 *
 * Store::insertSpan survives, because reusing a span deliberately is a real
 * operation. What is gone is doing it automatically on a text match.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "xudu/core/store.hpp"
#include "xudu/core/uncommitted_op_log.hpp"
#include "xudu/core/user_permascroll.hpp"

namespace fs = std::filesystem;

namespace xudu {
namespace {

// The regression guard. If automatic deduplication is ever reintroduced, this
// is the test that fails: the second document gets its own coordinates, and
// so is not reported as quoting the first.
TEST(SpanIdentityTest, IdenticalTextTypedTwiceGetsDistinctCoordinates) {
  auto shared = std::make_shared<UserPermascroll>();
  Store alice(shared);
  Store bob(shared);

  const std::string boilerplate =
      "Copyright 2026 Example Corporation. All rights reserved.";
  ASSERT_GT(boilerplate.size(), 24U) << "long enough that dedup would fire";

  const auto aliceVersion = alice.insert(MicroversionId{}, 0, boilerplate);
  const auto bobVersion   = bob.insert(MicroversionId{}, 0, boilerplate);

  EXPECT_EQ(alice.textOf(aliceVersion), boilerplate);
  EXPECT_EQ(bob.textOf(bobVersion), boilerplate);

  const auto *aliceOp = alice.getCompactOp(aliceVersion);
  const auto *bobOp   = bob.getCompactOp(bobVersion);
  ASSERT_NE(aliceOp, nullptr);
  ASSERT_NE(bobOp, nullptr);

  EXPECT_NE(aliceOp->spanStart, bobOp->spanStart)
      << "two authors typing the same line were placed at one address, which "
         "reads as one quoting the other";

  // Both runs are really in the scroll: nothing was elided.
  EXPECT_EQ(shared->size(), boilerplate.size() * 2);
}

// Typing character by character and flushing produces one appended span, not
// a reference to anything already present.
TEST(SpanIdentityTest, TypingOverExistingTextStillAppends) {
  Store store;
  const std::string quote =
      "Project Xanadu is a computer network with universal transclusion.";
  static_cast<void>(store.insert(MicroversionId{}, 0, quote));
  const auto sizeAfterFirst = store.primedia().size();

  UncommittedOpLog log;
  for (std::size_t i = 0; i < quote.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&quote[i], 1));
  }
  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].text, quote);

  const auto second =
      store.insert(MicroversionId{}, compacted[0].at, compacted[0].text);

  EXPECT_EQ(store.textOf(second), quote);
  EXPECT_EQ(store.primedia().size(), sizeAfterFirst + quote.size())
      << "the second passage was folded into the first";
}

// insertSpan is kept: pointing at an existing span on purpose is how an
// author quotes, and is the primitive an explicit transclusion command needs.
TEST(SpanIdentityTest, InsertSpanQuotesAnExistingSpanOnPurpose) {
  Store store;
  const auto span = store.primedia().append("Original master text passage.");
  const auto v1   = store.insertSpan(MicroversionId{}, 0, span);

  EXPECT_EQ(v1.str(), "1");
  EXPECT_EQ(store.textOf(v1), "Original master text passage.");

  const auto sizeAfterAppend = store.primedia().size();

  // Quoting it again adds no primedia, because it really is the same bytes.
  const auto v2 = store.insertSpan(v1, 0, span);
  EXPECT_EQ(store.textOf(v2),
            "Original master text passage.Original master text passage.");
  EXPECT_EQ(store.primedia().size(), sizeAfterAppend);
}

} // namespace
} // namespace xudu
