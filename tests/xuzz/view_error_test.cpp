#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string_view>
#include <utility>

#include "common/xanadu/view/view_error.hpp"

namespace {

using xanadu::view::kViewErrors;
using xanadu::view::kViewMessages;
using xanadu::view::messageId;
using xanadu::view::messageKey;
using xanadu::view::messageText;
using xanadu::view::ViewError;
using xanadu::view::ViewMessage;

// The lists are what the tests below visit, so they must hold every
// enumerator: dense from zero, in declaration order, ending at the last one.
template <typename List> constexpr bool denseFromZero(const List &list) {
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (std::to_underlying(list[i]) != i) {
      return false;
    }
  }
  return true;
}
static_assert(denseFromZero(kViewErrors));
static_assert(denseFromZero(kViewMessages));
static_assert(kViewErrors.back() == ViewError::ArenaRefused);
static_assert(kViewMessages.back() == ViewMessage::ViewSpaceRefused);

} // namespace

TEST(ViewErrorTest, EveryErrorHasAMessageWithWords) {
  for (const auto error : kViewErrors) {
    const auto message = messageKey(error);
    EXPECT_FALSE(messageId(message).empty())
        << "error " << std::to_underlying(error);
    EXPECT_FALSE(messageText(message).empty())
        << "error " << std::to_underlying(error);
  }
}

TEST(ViewErrorTest, NoTwoErrorsShareAMessage) {
  std::set<ViewMessage> seen;
  for (const auto error : kViewErrors) {
    EXPECT_TRUE(seen.insert(messageKey(error)).second)
        << messageId(messageKey(error)) << " reports two errors";
  }
}

TEST(ViewErrorTest, EveryMessageHasADistinctIdAndWords) {
  std::set<std::string_view> ids;
  for (const auto message : kViewMessages) {
    const auto id = messageId(message);
    EXPECT_TRUE(id.starts_with("view.")) << id;
    EXPECT_TRUE(ids.insert(id).second) << id << " is used twice";
    EXPECT_FALSE(messageText(message).empty()) << id;
  }
}

TEST(ViewErrorTest, FieldsAreClosedBraces) {
  for (const auto message : kViewMessages) {
    const auto text = messageText(message);
    EXPECT_EQ(std::ranges::count(text, '{'), std::ranges::count(text, '}'))
        << messageId(message);
  }
}

TEST(ViewErrorTest, RefusalsTheReaderCausesUseTheWordsOfSection12) {
  EXPECT_EQ(messageText(messageKey(ViewError::EmptyGroupBind)),
            "Group '{name}' has no dimensions yet.");
  EXPECT_EQ(messageText(messageKey(ViewError::GroupCycle)),
            "'{inner}' already contains '{outer}'.");
  EXPECT_EQ(messageText(messageKey(ViewError::PromotionRefused)),
            "This pack is too large to keep ({count} cells).");
}
