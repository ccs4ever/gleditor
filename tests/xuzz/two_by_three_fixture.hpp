/**
 * @file two_by_three_fixture.hpp
 * @brief The 2x3 discontinuous link the Xuzz navigation tests share.
 */
#ifndef TESTS_XUZZ_TWO_BY_THREE_FIXTURE_HPP
#define TESTS_XUZZ_TWO_BY_THREE_FIXTURE_HPP

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <ranges>
#include <string>

#include "common/xanadu/link_views.hpp"
#include "common/xanadu/store.hpp"

namespace xuzz_test {

using xanadu::PrimediaSpan;

// "Alpha beta gamma delta. one two three." -- the offsets below are into it.
inline constexpr std::uint32_t kBaseLength = 38;
inline constexpr std::uint32_t kThreeAt    = 32;
inline constexpr std::uint32_t kWordLength = 5;

/**
 * One 2x3 link over one document and two cells, plus a second link that
 * overlaps its first left member.
 *
 * Left members are "Alpha" and "gamma", disjoint so that a covering extent
 * would swallow " beta ". Right members are "one", "two", "three"; "three" is
 * quoted twice back to back after the original, once whole in a cell and once
 * in part in another, and typed afresh at the end so the same words exist at
 * an address no member names.
 */
struct TwoByThree {
  std::unique_ptr<xanadu::Store> store = std::make_unique<xanadu::Store>();
  xanadu::MicroversionId base;
  xanadu::MicroversionId head;
  zigzag::CellRef wholeCell{zigzag::noCell};
  zigzag::CellRef partCell{zigzag::noCell};
  zigzag::CellRef link{zigzag::noCell};
  zigzag::CellRef overlapping{zigzag::noCell};
  PrimediaSpan three;
};

inline zigzag::CellRef linkOwnedBy(const xanadu::Store &store,
                                   const std::string &owner) {
  const auto found = gleditor::firstOf(
      store.linkView() | std::views::filter(xanadu::links::ownedBy(owner)));
  return found ? found->id : zigzag::noCell;
}

inline TwoByThree makeTwoByThree() {
  TwoByThree f;
  auto &store = *f.store;
  f.base      = store.insert({}, 0, "Alpha beta gamma delta. one two three.");
  const auto base = store.rebuild(f.base);
  const auto word = [&](std::uint32_t at, std::uint32_t length) {
    const auto spans = base.spansFor(at, length);
    EXPECT_EQ(spans.size(), 1U);
    return spans.front();
  };
  f.three = word(kThreeAt, kWordLength);

  auto at =
      store.transclude(f.base, kBaseLength, f.base, kThreeAt, kWordLength);
  at = store.transclude(at, kBaseLength + kWordLength, f.base, kThreeAt,
                        kWordLength);
  at = store.insert(at, kBaseLength + 2 * kWordLength, " three");

  at          = store.sliceGenesis(at);
  at          = store.makeCell(at, f.three);
  f.wholeCell = store.cellRefOf(at);
  at          = store.makeCell(at, PrimediaSpan{.scroll = f.three.scroll,
                                                .start  = f.three.start,
                                                .length = 3});
  f.partCell  = store.cellRefOf(at);

  xanadu::Link link;
  link.type  = xanadu::LinkType::Comment;
  link.owner = "twobythree";
  link.left  = {word(0, 5), word(11, 5)};
  link.right = {word(24, 3), word(28, 3), f.three};
  at         = store.addLink(at, link);

  xanadu::Link other;
  other.type  = xanadu::LinkType::Comment;
  other.owner = "overlap";
  other.left  = {word(0, 10)};
  other.right = {word(28, 3)};
  at          = store.addLink(at, other);

  f.head        = at;
  f.link        = linkOwnedBy(store, "twobythree");
  f.overlapping = linkOwnedBy(store, "overlap");
  return f;
}

} // namespace xuzz_test

#endif // TESTS_XUZZ_TWO_BY_THREE_FIXTURE_HPP
