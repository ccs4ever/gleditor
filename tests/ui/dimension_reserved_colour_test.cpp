// Dimension colours against the colours with a fixed meaning, each read from
// where the interface defines it (view-system-implementation-plan.md §5.2).
// Here rather than beside the rule because the sources span the engine, the
// xanadoc UI, the slice visualizer and the library's theme.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <gleditor/color.hpp>
#include <gleditor/color_space.hpp>
#include <gleditor/ui/theme.hpp>

#include "common/ui/slice/zigzag_visualizer.hpp"
#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/pouch_zone.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/dimension_colour.hpp"

namespace {

namespace color = gleditor::color;
using xanadu::view::DimensionPalette;

// Far past any slice a reader will hold.
constexpr std::uint64_t kMany = 5000;

constexpr std::uint32_t opaque(const std::uint32_t rgba) {
  return rgba | 0xFFU;
}

struct Source {
  std::string where;
  std::uint32_t rgba;
};

std::uint32_t packed(const glm::vec3 &colour) {
  return color::packRgba(colour.r, colour.g, colour.b);
}

/// Every colour with a fixed meaning, read from its definition.
std::vector<Source> definedColours() {
  std::vector<Source> out;
  for (const auto type :
       {xanadu::LinkType::Comment, xanadu::LinkType::Illustration,
        xanadu::LinkType::Disagreement, xanadu::LinkType::Authorship,
        xanadu::LinkType::Quotation, xanadu::LinkType::Other,
        xanadu::LinkType::Format, xanadu::LinkType::Dimension}) {
    out.push_back({"linkColour(" + std::to_string(static_cast<int>(type)) + ")",
                   opaque(xanadu::linkColour(type))});
  }
  out.push_back(
      {"BeamColours::transclusion", opaque(xanadu::BeamColours::transclusion)});
  out.push_back({"BeamColours::transcopyrightLocked",
                 opaque(xanadu::BeamColours::transcopyrightLocked)});
  out.push_back(
      {"BeamColours::withheld", opaque(xanadu::BeamColours::withheld)});
  out.push_back({"BeamColours::cellEnd", opaque(xanadu::BeamColours::cellEnd)});
  out.push_back(
      {"BeamColours::flyingTether", opaque(xanadu::BeamColours::flyingTether)});
  out.push_back(
      {"SceneVisual::focus_color", packed(zigzag::SceneVisual{}.focus_color)});
  out.push_back(
      {"SceneVisual::quote_color", packed(zigzag::SceneVisual{}.quote_color)});
  out.push_back(
      {"ThemeColours::accent",
       opaque(gleditor::ui::rgba(gleditor::ui::ThemeColours{}.accent))});
  out.push_back({"Session::transclusionColour",
                 opaque(xanadu::Session::transclusionColour)});
  out.push_back({"Session::linkColour", opaque(xanadu::Session::linkColour)});

  // The pouch auras, as system://pouches defaults them and as a pouch
  // manager with no store falls back to them.
  for (const auto &spec :
       xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Pouches)) {
    if (!spec.name.starts_with("zone.") || spec.schemas.empty() ||
        spec.schemas.front().defaultValues.size() < 2) {
      continue;
    }
    const auto &aura = spec.schemas.front().defaultValues.at(1);
    if (const auto *value = std::get_if<std::int64_t>(&aura)) {
      out.push_back({spec.name, opaque(static_cast<std::uint32_t>(*value))});
    }
  }
  const xanadu::PouchManager pouches;
  for (const auto &zone : pouches.zones()) {
    out.push_back(
        {"PouchManager " + std::string{zone->id()}, opaque(zone->auraColor())});
  }
  return out;
}

/// A colour too grey to mean a hue (a neutral link, redaction's near-black)
/// reserves no hue band; it is still kept clear of below.
bool chromatic(const std::uint32_t rgba) {
  constexpr double kNeutralChroma = 0.04;
  return color::oklchFromRgba(rgba).c >= kNeutralChroma;
}

TEST(DimensionReservedColourTest, EveryDefinedColourIsOneThePaletteReserves) {
  const auto reserved = xanadu::view::reservedColours();
  const auto defined  = definedColours();
  ASSERT_GE(defined.size(), 19U);
  for (const auto &source : defined) {
    if (!chromatic(source.rgba)) {
      continue;
    }
    EXPECT_TRUE(std::ranges::any_of(
        reserved,
        [&](const auto &colour) { return opaque(colour.rgba) == source.rgba; }))
        << source.where << " draws #" << std::hex << (source.rgba >> 8U)
        << ", which the dimension palette does not keep clear of";
  }
}

TEST(DimensionReservedColourTest, NoDimensionColourComesWithinTenOfOne) {
  const auto defined  = definedColours();
  const auto &palette = DimensionPalette::standard();
  double nearest      = std::numeric_limits<double>::infinity();
  for (std::uint64_t n = 0; n < kMany; ++n) {
    const auto colour = palette.cueOf(n).colour;
    for (const auto &source : defined) {
      const double distance = color::ciede2000(colour, source.rgba);
      EXPECT_GE(distance, 10.0)
          << "dimension " << n << " #" << std::hex << (colour >> 8U)
          << " against " << source.where;
      nearest = std::min(nearest, distance);
    }
  }
  RecordProperty("nearestDefinedDE00", std::to_string(nearest));
  std::printf("nearest CIEDE2000 from %llu dimension colours to a defined "
              "reserved colour: %.2f\n",
              static_cast<unsigned long long>(kMany), nearest);
}

} // namespace
