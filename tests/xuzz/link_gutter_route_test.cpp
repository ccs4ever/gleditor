#include "common/xanadu/link_gutter_route.hpp"
#include <gtest/gtest.h>

TEST(LinkGutterRouteTest, RemotePagesRetainExactEndsAndLocalTurns) {
  for (const float remote : {-100000.F, 100000.F}) {
    const glm::vec3 from(0, 0, 3), to(16, remote, 3);
    const auto route = xanadu::gutterRoute(from, to, 6, 14);
    EXPECT_EQ(route.front(), from);
    EXPECT_EQ(route.back(), to);
    EXPECT_FLOAT_EQ(route[1].y, from.y);
    EXPECT_FLOAT_EQ(route[5].y, to.y);
    EXPECT_FLOAT_EQ(std::abs(route[2].y - from.y), 14);
    EXPECT_FLOAT_EQ(std::abs(route[4].y - to.y), 14);
    for (int i = 1; i < 6; ++i) EXPECT_FLOAT_EQ(route[i].x, 6);
    EXPECT_GT((route[2].y - route[1].y) * (route[3].y - route[2].y), 0);
    EXPECT_GT((route[4].y - route[3].y) * (route[5].y - route[4].y), 0);
  }
}
TEST(LinkGutterRouteTest, ShortRunsDoNotReverseOrMoveAttachments) {
  const auto route = xanadu::gutterRoute({0, 0, 0}, {16, 4, 0}, 8, 14);
  for (std::size_t i = 1; i < route.size(); ++i)
    EXPECT_GE(route[i].y, route[i - 1].y);
  EXPECT_FLOAT_EQ(route[2].y, 1);
  EXPECT_FLOAT_EQ(route[4].y, 3);
}
