/**
 * @file vlace_driver_test.cpp
 * @brief Stage 6 driver integration: parse -> type-check -> evaluate -> render.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vlace/driver.hpp"

namespace {
using namespace xanadu::vlace;

TEST(VlaceDriverTest, EvaluatesWithType) {
  Driver d;
  auto r = d.run("1 + 2");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, "Int");
  EXPECT_EQ(r->value, "3");
}

TEST(VlaceDriverTest, RejectsIllTypedBeforeEval) {
  Driver d;
  auto r = d.run("1 + \"x\"");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, CompileError::TypeMismatch);
  EXPECT_EQ(r.error().phase, "type");
}

TEST(VlaceDriverTest, TypeCheckingCanBeDisabled) {
  Driver d;
  d.withTypeChecking(false);
  auto r = d.run("map f xs = case xs of [] => [] ; (h : t) => f h : map f t ;"
                 "map (fn x => x * x) [1, 2, 3]");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->value, "[1, 4, 9]");
}

TEST(VlaceDriverTest, FormatsError) {
  Driver d;
  auto r = d.run("y");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(Driver::formatError(r.error()).find("UnboundIdentifier"),
            std::string::npos);
}

} // namespace
