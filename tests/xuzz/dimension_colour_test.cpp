#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

#include <gleditor/color_space.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/dimension_colour.hpp"

namespace {

using xanadu::SystemDocKind;
using xanadu::SystemStoreModel;
using xanadu::view::ColourBand;
using xanadu::view::CueGlyph;
using xanadu::view::DashStyle;
using xanadu::view::DimensionCue;
using xanadu::view::DimensionCues;
using xanadu::view::DimensionPalette;

namespace color = gleditor::color;

constexpr std::uint64_t kStrip = 40;
// Far past any slice a reader will hold, to show the rule does not wear out.
constexpr std::uint64_t kMany = 5000;

struct Tone {
  double lightness;
  double chroma;
};
// The tiers V1 fixed (view-system-implementation-plan.md §3.1).
constexpr std::array<Tone, 3> kTones{
    {{0.75, 0.127}, {0.60, 0.102}, {0.80, 0.08}}};

double nearestReserved(const std::uint32_t colour) {
  double nearest = std::numeric_limits<double>::infinity();
  for (const auto &reserved : xanadu::view::reservedColours()) {
    nearest = std::min(nearest, color::ciede2000(colour, reserved.rgba));
  }
  return nearest;
}

std::unique_ptr<xanadu::Store> uiStore() {
  auto store = std::make_unique<xanadu::Store>();
  store->setSystem(true);
  xanadu::initializeSystemStore(*store, SystemDocKind::UI);
  return store;
}

DimensionCues cuesOf(const xanadu::Store &store,
                     const xanadu::MicroversionId &version) {
  return DimensionCues::fromSettings(
      SystemStoreModel::fromStore(store, version));
}

TEST(DimensionColourTest, FortyColoursAreInGamutAtTheirTiersTone) {
  const auto &palette = DimensionPalette::standard();
  for (std::uint64_t n = 0; n < kStrip; ++n) {
    const auto band = DimensionPalette::bandOf(n);
    const auto tone = kTones.at(static_cast<std::size_t>(band));
    const auto hue  = palette.hueOf(n);
    EXPECT_TRUE(color::inGamut(color::linearFromOklch(
        {.l = tone.lightness, .c = tone.chroma, .h = hue})))
        << "dimension " << n;
    // The 8-bit colour keeps the tier's tone: nothing was clipped into it.
    const auto drawn = color::oklchFromRgba(palette.cueOf(n).colour);
    EXPECT_NEAR(drawn.l, tone.lightness, 0.005) << "dimension " << n;
    EXPECT_NEAR(drawn.c, tone.chroma, 0.005) << "dimension " << n;
  }
}

TEST(DimensionColourTest, NeighboursDifferInBandDashAndGlyph) {
  for (std::uint64_t n = 0; n + 1 < kMany; ++n) {
    EXPECT_NE(DimensionPalette::bandOf(n), DimensionPalette::bandOf(n + 1));
    EXPECT_NE(DimensionPalette::dashOf(n), DimensionPalette::dashOf(n + 1));
    EXPECT_NE(DimensionPalette::glyphOf(n), DimensionPalette::glyphOf(n + 1));
  }
}

TEST(DimensionColourTest, CuesCycleInTheOrderV1Fixed) {
  EXPECT_EQ(DimensionPalette::bandOf(0), ColourBand::Vivid);
  EXPECT_EQ(DimensionPalette::bandOf(1), ColourBand::Deep);
  EXPECT_EQ(DimensionPalette::bandOf(2), ColourBand::Pale);
  constexpr std::array dashes{"solid",        "dash",     "dot",
                              "dash-dot-dot", "dash-dot", "long-short"};
  for (std::uint64_t n = 0; n < dashes.size(); ++n) {
    EXPECT_EQ(xanadu::view::dashStyleName(DimensionPalette::dashOf(n)),
              dashes.at(n));
    EXPECT_EQ(xanadu::view::dashStyleNamed(dashes.at(n)),
              DimensionPalette::dashOf(n));
  }
  constexpr std::array glyphs{"circle", "triangle", "square", "diamond",
                              "plus",   "cross",    "ring"};
  for (std::uint64_t n = 0; n < glyphs.size(); ++n) {
    EXPECT_EQ(xanadu::view::cueGlyphName(DimensionPalette::glyphOf(n)),
              glyphs.at(n));
  }
  EXPECT_FALSE(xanadu::view::dashStyleNamed("hexagon"));
}

TEST(DimensionColourTest, NoColourComesWithinTenOfAReservedOne) {
  const auto &palette = DimensionPalette::standard();
  double nearest      = std::numeric_limits<double>::infinity();
  for (std::uint64_t n = 0; n < kMany; ++n) {
    const auto distance = nearestReserved(palette.cueOf(n).colour);
    EXPECT_GE(distance, 10.0) << "dimension " << n;
    nearest = std::min(nearest, distance);
  }
  RecordProperty("nearestReservedDE00", std::to_string(nearest));
}

TEST(DimensionColourTest, HuesLieOnTheirTiersOpenArcs) {
  const auto &palette = DimensionPalette::standard();
  for (std::uint64_t n = 0; n < kMany; ++n) {
    const auto arcs = palette.openArcs(DimensionPalette::bandOf(n));
    const auto hue  = palette.hueOf(n);
    EXPECT_TRUE(std::ranges::any_of(arcs,
                                    [hue](const auto &arc) {
                                      const double from = arc.from;
                                      return (hue >= from && hue <= arc.to) ||
                                             (hue + 360.0 >= from &&
                                              hue + 360.0 <= arc.to);
                                    }))
        << "dimension " << n << " at hue " << hue;
  }
}

TEST(DimensionColourTest, TheRuleIsDeterministic) {
  const DimensionPalette fresh{xanadu::view::reservedColours()};
  const auto &standard = DimensionPalette::standard();
  for (std::uint64_t n = 0; n < kStrip; ++n) {
    EXPECT_EQ(fresh.cueOf(n), standard.cueOf(n));
  }
  for (const auto band :
       {ColourBand::Vivid, ColourBand::Deep, ColourBand::Pale}) {
    EXPECT_TRUE(
        std::ranges::equal(fresh.openArcs(band), standard.openArcs(band)));
  }
}

TEST(DimensionColourTest, StripOfFortyForTheEye) {
  // The strip the frame inspector reads at M2's gate; printed, not judged.
  const auto &palette = DimensionPalette::standard();
  for (const auto band :
       {ColourBand::Vivid, ColourBand::Deep, ColourBand::Pale}) {
    double total = 0.0;
    std::printf("%-5s",
                std::string{xanadu::view::colourBandName(band)}.c_str());
    for (const auto &arc : palette.openArcs(band)) {
      std::printf(" [%.2f, %.2f]", arc.from, arc.to);
      total += arc.length();
    }
    std::printf(" total %.2f degrees\n", total);
  }
  for (std::uint64_t n = 0; n < kStrip; ++n) {
    const auto cue = palette.cueOf(n);
    std::printf(
        "%2llu #%06X %-5s %-12s %-8s hue %6.1f dE00 %5.2f\n",
        static_cast<unsigned long long>(n), cue.colour >> 8U,
        std::string{xanadu::view::colourBandName(DimensionPalette::bandOf(n))}
            .c_str(),
        std::string{xanadu::view::dashStyleName(cue.dash)}.c_str(),
        std::string{xanadu::view::cueGlyphName(cue.glyph)}.c_str(),
        palette.hueOf(n), nearestReserved(cue.colour));
  }
  SUCCEED();
}

TEST(DimensionColourTest, FirstSeenTakesTheNextOrdinalAndKeepsIt) {
  const auto owned  = uiStore();
  auto &store       = *owned;
  auto ver          = store.primaryCurrentVersion();
  ver               = xanadu::view::assignDimensionCue(store, ver, "d.1");
  ver               = xanadu::view::assignDimensionCue(store, ver, "d.clone");
  const auto before = cuesOf(store, ver);
  ASSERT_EQ(before.size(), 2U);
  EXPECT_EQ(before.ordinalOf("d.1"), 0U);
  EXPECT_EQ(before.ordinalOf("d.clone"), 1U);
  EXPECT_EQ(before.cueOf("d.1"), DimensionPalette::standard().cueOf(0));
  EXPECT_EQ(before.cueOf("d.clone"), DimensionPalette::standard().cueOf(1));
  EXPECT_FALSE(before.cueOf("d.2"));

  // Seeing one again changes nothing; adding one moves nothing already seen.
  EXPECT_EQ(xanadu::view::assignDimensionCue(store, ver, "d.1"), ver);
  ver              = xanadu::view::assignDimensionCue(store, ver, "d.2");
  const auto after = cuesOf(store, ver);
  ASSERT_EQ(after.size(), 3U);
  EXPECT_EQ(after.cueOf("d.1"), before.cueOf("d.1"));
  EXPECT_EQ(after.cueOf("d.clone"), before.cueOf("d.clone"));
  EXPECT_EQ(after.ordinalOf("d.2"), 2U);
  EXPECT_EQ(after.cueOf("d.2"), DimensionPalette::standard().cueOf(2));
}

TEST(DimensionColourTest, TheSameDimensionsInTheSameOrderGetTheSameCues) {
  const auto ownedOne = uiStore();
  const auto ownedTwo = uiStore();
  auto &one           = *ownedOne;
  auto &two           = *ownedTwo;
  auto verOne         = one.primaryCurrentVersion();
  auto verTwo         = two.primaryCurrentVersion();
  for (const auto *name : {"d.1", "d.2", "d.cursor"}) {
    verOne = xanadu::view::assignDimensionCue(one, verOne, name);
    verTwo = xanadu::view::assignDimensionCue(two, verTwo, name);
  }
  const auto cuesOne = cuesOf(one, verOne);
  const auto cuesTwo = cuesOf(two, verTwo);
  for (const auto *name : {"d.1", "d.2", "d.cursor"}) {
    EXPECT_EQ(cuesOne.cueOf(name), cuesTwo.cueOf(name)) << name;
  }
}

TEST(DimensionColourTest, TheReadersOverridesWin) {
  const auto owned    = uiStore();
  auto &store         = *owned;
  auto ver            = store.primaryCurrentVersion();
  ver                 = xanadu::view::assignDimensionCue(store, ver, "d.1");
  ver                 = xanadu::view::assignDimensionCue(store, ver, "d.2");
  const auto assigned = cuesOf(store, ver).cueOf("d.2");
  ASSERT_TRUE(assigned);

  constexpr std::uint32_t chosen = 0x123456FFU;
  ver                            = xanadu::setSetting(store, ver,
                                                      xanadu::settings::dimensionColourKey("d.2"),
                                                      std::int64_t{chosen});
  ver =
      xanadu::setSetting(store, ver, xanadu::settings::dimensionDashKey("d.2"),
                         std::string{"long-short"});
  const auto overridden = cuesOf(store, ver);
  const auto cue        = overridden.cueOf("d.2");
  ASSERT_TRUE(cue);
  EXPECT_EQ(cue->colour, chosen);
  EXPECT_EQ(cue->dash, DashStyle::LongShort);
  EXPECT_EQ(cue->glyph, assigned->glyph);
  EXPECT_EQ(overridden.cueOf("d.1"), DimensionPalette::standard().cueOf(0));

  // Resetting gives back the palette's assignment.
  ver = xanadu::resetSettingToDefault(
      store, ver, xanadu::settings::dimensionColourKey("d.2"));
  ver = xanadu::resetSettingToDefault(
      store, ver, xanadu::settings::dimensionDashKey("d.2"));
  EXPECT_EQ(cuesOf(store, ver).cueOf("d.2"), assigned);
}

TEST(DimensionColourTest, ADashNamingNoPatternLeavesThePalettes) {
  const auto owned = uiStore();
  auto &store      = *owned;
  auto ver         = store.primaryCurrentVersion();
  ver              = xanadu::view::assignDimensionCue(store, ver, "d.1");
  ver =
      xanadu::setSetting(store, ver, xanadu::settings::dimensionDashKey("d.1"),
                         std::string{"wavy"});
  EXPECT_EQ(cuesOf(store, ver).cueOf("d.1"),
            DimensionPalette::standard().cueOf(0));
}

} // namespace
