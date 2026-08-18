/**
 * @file version_model.cpp
 * @brief A version checked against a plain string doing the same edits.
 *
 * Version stores a document as a list of pointers and never as its text, which
 * is what lets it answer questions about shared content. The risk in that is
 * that the list and the text it stands for drift apart, and the tests that
 * pin particular pieces cannot see it: they assert the representation, which is
 * exactly the thing allowed to change.
 *
 * So this asserts the other side. The same edits are applied to a std::string,
 * and after every one the version must materialise to it. The model is
 * obviously correct, which is the whole reason to have it.
 *
 * It exists because pieces are now joined when they name consecutive addresses
 * -- typing a hundred characters leaves one piece, not a hundred -- and that is
 * a change to the representation that must not be a change to the document.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <xudu/core/spool.hpp>
#include <xudu/core/version.hpp>

namespace {

using xudu::PrimediaSpan;
using xudu::Version;

/// A spool that hands back the bytes at an address, so a version can be
/// materialised without a store.
class Bytes : public xudu::SpanReader {
public:
  /// Append, as the primedia spool does, and say where it landed.
  PrimediaSpan put(const std::string &text) {
    const PrimediaSpan span{xudu::localScroll, contents.size(), text.size()};
    contents += text;
    return span;
  }

  [[nodiscard]] std::string read(const PrimediaSpan &span) const override {
    if (span.start >= contents.size()) {
      return {};
    }
    return contents.substr(static_cast<std::size_t>(span.start),
                           static_cast<std::size_t>(span.length));
  }

private:
  std::string contents;
};

TEST(VersionModelTest, typingSequentiallyLeavesOnePiece) {
  // The case that matters for cost: a person typing. Every insert continues
  // the last, so the version is one piece however long they go on.
  Bytes spool;
  Version version;
  std::string model;

  for (int i = 0; i < 200; i++) {
    const auto text = "word" + std::to_string(i) + " ";
    version.insert(version.length(), spool.put(text));
    model += text;
  }

  EXPECT_EQ(version.materialize(spool), model);
  EXPECT_EQ(version.length(), model.size());
  EXPECT_EQ(version.pieces().size(), 1U)
      << "typing left one piece per keystroke";
}

TEST(VersionModelTest, aQuotationOfTheWholeIsOnePiece) {
  Bytes spool;
  Version version;
  for (int i = 0; i < 50; i++) {
    version.insert(version.length(), spool.put("sentence "));
  }

  Version quoting;
  quoting.insertSpans(0, version.spansFor(0, version.length()));
  EXPECT_EQ(quoting.materialize(spool), version.materialize(spool));
  EXPECT_EQ(quoting.pieces().size(), 1U);
}

TEST(VersionModelTest, joiningDoesNotJoinUnrelatedContent) {
  // Only consecutive addresses in one scroll may be joined. Content that
  // merely sits next to something else in the document must stay its own
  // piece, or a quotation of it would be reported as covering its neighbour.
  Bytes spool;
  Version version;
  const auto first  = spool.put("alpha ");
  const auto middle = spool.put("beta ");
  const auto last   = spool.put("gamma");
  static_cast<void>(middle); // written, never pointed at

  // alpha then gamma, with beta left out. They sit next to each other in the
  // document and are nowhere near each other in the spool, so joining them
  // would claim a quotation of one covered the other.
  version.insert(0, last);
  version.insert(0, first);
  EXPECT_EQ(version.materialize(spool), "alpha gamma");
  EXPECT_EQ(version.pieces().size(), 2U);

  // Where they really are consecutive, one piece is right: the document reads
  // the same and the addresses run on.
  Version joined;
  joined.insert(0, first);
  joined.insert(joined.length(), middle);
  EXPECT_EQ(joined.materialize(spool), "alpha beta ");
  EXPECT_EQ(joined.pieces().size(), 1U);

  // Nor across scrolls, however the numbers line up.
  Version across;
  across.insert(0, PrimediaSpan{xudu::localScroll, 0, 10});
  across.insert(10, PrimediaSpan{7, 10, 10});
  EXPECT_EQ(across.pieces().size(), 2U);
}

/// Every edit applied to both, and compared after each one.
TEST(VersionModelTest, randomEditsAgreeWithAString) {
  Bytes spool;
  Version version;
  std::string model;
  std::mt19937 random(20260812);

  for (int step = 0; step < 1500; step++) {
    const auto length = static_cast<std::uint32_t>(model.size());
    switch (random() % 4) {
    case 0:
    case 1: { // insert, sometimes at the end and sometimes within
      const auto text = "t" + std::to_string(step) + " ";
      const auto at = 0 == length
                          ? 0U
                          : static_cast<std::uint32_t>(random() % (length + 1));
      version.insert(at, spool.put(text));
      model.insert(at, text);
      break;
    }
    case 2: { // remove
      if (0 == length) {
        break;
      }
      const auto at    = static_cast<std::uint32_t>(random() % length);
      const auto count = static_cast<std::uint32_t>(
          1 + (random() % std::min<std::uint32_t>(20, length - at)));
      version.remove(at, count);
      model.erase(at, count);
      break;
    }
    default: { // rearrange
      if (length < 4) {
        break;
      }
      const auto at    = static_cast<std::uint32_t>(random() % (length - 2));
      const auto count = static_cast<std::uint32_t>(
          1 + (random() % std::min<std::uint32_t>(15, length - at - 1)));
      auto to = static_cast<std::uint32_t>(random() % length);
      if (to > at && to < at + count) {
        break; // moving a range into itself does nothing; the model agrees
      }
      const auto moved = model.substr(at, count);
      version.rearrange(at, count, to);
      model.erase(at, count);
      model.insert(to > at ? to - count : to, moved);
      break;
    }
    }

    ASSERT_EQ(version.length(), model.size()) << "at step " << step;
    ASSERT_EQ(version.materialize(spool), model) << "at step " << step;
    // The invariant the joining relies on: no piece is ever empty, and no two
    // consecutive pieces could have been one.
    for (std::size_t i = 0; i < version.pieces().size(); i++) {
      const auto &piece = version.pieces()[i];
      ASSERT_FALSE(piece.empty())
          << "empty piece at " << i << ", step " << step;
      if (i + 1 < version.pieces().size()) {
        const auto &next = version.pieces()[i + 1];
        ASSERT_FALSE(piece.scroll == next.scroll && piece.end() == next.start)
            << "pieces " << i << " and " << i + 1 << " should have been joined"
            << ", step " << step;
      }
    }
  }
}

TEST(VersionModelTest, joiningDoesNotChangeWhatIsFoundSharedWithAnother) {
  // The question the whole representation exists to answer. Whether a passage
  // is stored as one piece or twenty must not change which bytes of a document
  // are reported as shared with another.
  Bytes spool;
  Version whole;
  std::vector<PrimediaSpan> written;
  for (int i = 0; i < 30; i++) {
    const auto span = spool.put("chunk" + std::to_string(i) + " ");
    written.push_back(span);
    whole.insert(whole.length(), span);
  }
  ASSERT_EQ(whole.pieces().size(), 1U);

  // A second document quoting the middle third.
  const auto from = whole.length() / 3;
  Version quoting;
  quoting.insertSpans(0, whole.spansFor(from, whole.length() / 3));

  std::vector<xudu::Extent> found;
  for (const auto &piece : quoting.pieces()) {
    for (const auto &extent : whole.occurrencesOf(piece)) {
      found.push_back(extent);
    }
  }
  ASSERT_EQ(found.size(), 1U) << "the shared passage came back in parts";
  EXPECT_EQ(found.front().start, from);
  EXPECT_EQ(found.front().end, from + (whole.length() / 3));
}

} // namespace
