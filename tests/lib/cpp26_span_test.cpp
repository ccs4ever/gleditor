#include <gleditor/cpp26_span.hpp>
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <stdexcept>

namespace gleditor {
namespace {

TEST(Cpp26SpanTest, ChecksBoundsAndPreservesElementReferences) {
  std::array<int, 3> values{2, 4, 6};
  std::span<int, 3> fixed{values};
  cpp26::span_at(fixed, 1) = 5;
  EXPECT_EQ(values[1], 5);
  EXPECT_EQ(&cpp26::span_at(fixed, 1), &values[1]);

  const std::span<const int> dynamic{values};
  EXPECT_EQ(cpp26::span_at(dynamic, 2), 6);
  EXPECT_THROW(static_cast<void>(cpp26::span_at(dynamic, dynamic.size())),
               std::out_of_range);
  EXPECT_THROW(static_cast<void>(cpp26::span_at(std::span<const int>{}, 0)),
               std::out_of_range);
}

} // namespace
} // namespace gleditor
