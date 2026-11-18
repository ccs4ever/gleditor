/**
 * @file vlace_xanadu_names_test.cpp
 * @brief Dimension names at the std:xanadu edge: an empty name is a named
 * error, and a missing link is None, never a cell.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vlace::CompileError;
using xanadu::vlace::Driver;

TEST(VlaceXanaduNamesTest, AnEmptyDimensionNameIsANamedError) {
  for (const char *src : {"makeDim \"\"", "dimensionNamed \"\""}) {
    Driver d;
    const auto r = d.run(src);
    ASSERT_FALSE(r.has_value()) << src;
    EXPECT_EQ(r.error().code, CompileError::InvalidArgument) << src;
    EXPECT_NE(r.error().detail.find("dimension name must not be empty"),
              std::string::npos)
        << r.error().detail;
    // Not the VM's own message leaking through.
    EXPECT_EQ(r.error().detail.find("DimByName"), std::string::npos) << src;
    EXPECT_NE(Driver::formatError(r.error()).find("InvalidArgument"),
              std::string::npos);
  }
}

TEST(VlaceXanaduNamesTest, ANonEmptyNameStillWorks) {
  Driver d;
  const auto made = d.run("makeDim \"d.doc\"");
  ASSERT_TRUE(made.has_value());
  EXPECT_EQ(made->type, "Dim");
  const auto absent = d.run("dimensionNamed \"d.never.made\"");
  ASSERT_TRUE(absent.has_value());
  EXPECT_EQ(absent->value, "None");
}

// `linked` and `dimensionNamed` tell "absent" from "present" by comparing the
// VM's cell result with 0. That is exact only if 0 is never a cell: op index 0
// is state zero, definitionally not a cell (store-slice-convergence), and an
// arena's refs carry ephemeralBit. Both halves are pinned here.
TEST(VlaceXanaduNamesTest, ZeroIsNeverACellSoAbsenceCannotBeConfusedWithOne) {
  zigzag::ArenaManifold arena;
  EXPECT_FALSE(arena.contains(0));
  EXPECT_NE(arena.makeCell(), 0U);
  Driver d;
  const auto none = d.run("let a = makeText \"x\" in let dd = makeDim "
                          "\"d.t\" in linked a dd");
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->value, "None");
}

} // namespace
