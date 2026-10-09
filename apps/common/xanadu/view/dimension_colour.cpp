#include "common/xanadu/view/dimension_colour.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <gleditor/color_space.hpp>
#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"

namespace xanadu::view {
namespace {

constexpr const char *kColourCategory = "view.colour";

/// The rule V1 fixed (view-system-implementation-plan.md §3.1). A reader
/// overrides a dimension's colour, never the rule, so these are not
/// settings: a tier moved by a setting could leave the gamut or come within
/// reach of a reserved colour without anything noticing.
struct BandTone {
  double lightness{};
  double chroma{};
};
/// OKLCH. Each chroma is the largest, to three places, that keeps its
/// lightness in the sRGB gamut at every open hue; the pale tier is held
/// lower than that on purpose, so that it reads as pale.
constexpr std::array<BandTone, kColourBandCount> kBandTones{{
    {.lightness = 0.75, .chroma = 0.127}, // vivid
    {.lightness = 0.60, .chroma = 0.102}, // deep
    {.lightness = 0.80, .chroma = 0.08},  // pale
}};

/// Degrees of hue kept free on either side of a reserved family's outermost
/// members, so that the nearest dimension is still a different hue from the
/// family and not a lighter or darker member of it.
constexpr double kFamilyGuardDegrees = 7.0;

/// CIEDE2000 distance every dimension colour keeps from every reserved
/// colour: about where two colours stop being mistaken for each other at a
/// glance, rather than only told apart side by side.
constexpr double kReservedClearance = 10.0;

/// Sampling step, in degrees, for finding where a tier's colour comes
/// within kReservedClearance of a reserved colour. A quarter degree of hue
/// moves a dimension colour by well under one CIEDE2000 unit.
constexpr double kHueScanStep = 0.25;

/// Successive multiples of the golden ratio's conjugate, taken modulo 1,
/// never fall near an earlier one: each new dimension lands in the largest
/// gap left by those before it.
constexpr double kGoldenConjugate = std::numbers::phi - 1.0;

// clang-format off
/// Every colour with a fixed meaning the interface draws, as drawn. Float
/// sources are given as packRgba() packs them, by truncation.
constexpr std::array kReservedColours{
    ReservedColour{"red",            "disagreement link",                  0xFF7A6BFFU},
    ReservedColour{"gold",           "illustration link",                  0xFFC46BFFU},
    ReservedColour{"gold",           "transclusion beam (Identity Gold)",  0xFFD700FFU},
    ReservedColour{"gold",           "transcopyright-locked beam",         0xF59E0BFFU},
    ReservedColour{"gold",           "slice focus",                        0xF3C542FFU},
    ReservedColour{"gold",           "notes pouch aura",                   0xEAB308FFU},
    ReservedColour{"gold",           "transclusion highlight",             0xFFE9A8FFU},
    ReservedColour{"green",          "quotation link",                     0x7FE0A8FFU},
    ReservedColour{"green",          "scratch pouch aura",                 0x10B981FFU},
    ReservedColour{"green",          "link mark",                          0xB9E8C4FFU},
    ReservedColour{"cyan-blue",      "theme accent",                       0x3FB2F2FFU},
    ReservedColour{"cyan-blue",      "tether, anchor and satelloid",       0x38BDF8FFU},
    ReservedColour{"cyan-blue",      "to-link-left pouch aura",            0x06B6D4FFU},
    ReservedColour{"cyan-blue",      "comment link",                       0x7FB2FFFFU},
    ReservedColour{"cyan-blue",      "quotation cell",                     0x38BCF7FFU},
    ReservedColour{"purple-magenta", "authorship link",                    0xB98CFFFFU},
    ReservedColour{"purple-magenta", "dimension link",                     0xAA00FFFFU},
    ReservedColour{"purple-magenta", "link ribbon, to-link-right pouch",   0xEC4899FFU},
};
// clang-format on

constexpr std::array<std::string_view, kDashStyleCount> kDashNames{
    "solid", "dash", "dot", "dash-dot-dot", "dash-dot", "long-short"};
constexpr std::array<std::string_view, kCueGlyphCount> kGlyphNames{
    "circle", "triangle", "square", "diamond", "plus", "cross", "ring"};
constexpr std::array<std::string_view, kColourBandCount> kBandNames{
    "vivid", "deep", "pale"};

[[nodiscard]] double wrapDegrees(const double degrees) noexcept {
  const double wrapped = std::fmod(degrees, 360.0);
  return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
}

/// The smallest arc covering every hue in @p hues: the circle less the
/// widest gap between neighbouring members.
[[nodiscard]] HueArc coveringArc(std::vector<double> hues) {
  std::ranges::sort(hues);
  std::size_t widestAfter = hues.size() - 1;
  double widest           = hues.front() + 360.0 - hues.back();
  for (std::size_t i = 0; i + 1 < hues.size(); ++i) {
    if (const double gap = hues[i + 1] - hues[i]; gap > widest) {
      widest      = gap;
      widestAfter = i;
    }
  }
  const double from = hues[(widestAfter + 1) % hues.size()];
  const double to   = hues[widestAfter];
  return HueArc{.from = from, .to = to >= from ? to : to + 360.0};
}

[[nodiscard]] bool inArc(const HueArc &arc, const double hue) noexcept {
  return wrapDegrees(hue - arc.from) <= arc.length();
}

[[nodiscard]] std::uint32_t colourAt(const BandTone &tone, const double hue) {
  return gleditor::color::rgbaFromLinear(gleditor::color::linearFromOklch(
      {.l = tone.lightness, .c = tone.chroma, .h = hue}));
}

} // namespace

std::string_view dashStyleName(const DashStyle style) noexcept {
  return kDashNames.at(static_cast<std::size_t>(style));
}

std::optional<DashStyle> dashStyleNamed(const std::string_view name) {
  const auto it = std::ranges::find(kDashNames, name);
  if (it == kDashNames.end()) {
    return std::nullopt;
  }
  return static_cast<DashStyle>(std::distance(kDashNames.begin(), it));
}

std::string_view cueGlyphName(const CueGlyph glyph) noexcept {
  return kGlyphNames.at(static_cast<std::size_t>(glyph));
}

std::string_view colourBandName(const ColourBand band) noexcept {
  return kBandNames.at(static_cast<std::size_t>(band));
}

std::span<const ReservedColour> reservedColours() noexcept {
  return kReservedColours;
}

DimensionPalette::DimensionPalette(
    const std::span<const ReservedColour> reserved) {
  std::map<std::string_view, std::vector<double>, std::less<>> familyHues;
  std::vector<gleditor::color::CieLab> reservedLab;
  reservedLab.reserve(reserved.size());
  for (const auto &colour : reserved) {
    familyHues[colour.family].push_back(
        gleditor::color::oklchFromRgba(colour.rgba).h);
    reservedLab.push_back(gleditor::color::cieLabFromRgba(colour.rgba));
  }
  std::vector<HueArc> guarded;
  for (const auto &hues : familyHues | std::views::values) {
    const auto members = coveringArc(hues);
    guarded.push_back({.from = wrapDegrees(members.from - kFamilyGuardDegrees),
                       .to   = wrapDegrees(members.from - kFamilyGuardDegrees) +
                             members.length() + (2.0 * kFamilyGuardDegrees)});
  }

  const auto samples =
      static_cast<std::size_t>(std::lround(360.0 / kHueScanStep));
  for (std::size_t band = 0; band < kColourBandCount; ++band) {
    const auto &tone = kBandTones.at(band);
    const auto open  = [&](const double hue) {
      if (std::ranges::any_of(
              guarded, [hue](const HueArc &arc) { return inArc(arc, hue); })) {
        return false;
      }
      const auto linear = gleditor::color::linearFromOklch(
          {.l = tone.lightness, .c = tone.chroma, .h = hue});
      if (!gleditor::color::inGamut(linear)) {
        return false;
      }
      const auto lab = gleditor::color::cieLabFromRgba(
          gleditor::color::rgbaFromLinear(linear));
      return std::ranges::all_of(reservedLab, [&lab](const auto &other) {
        return gleditor::color::ciede2000(lab, other) >= kReservedClearance;
      });
    };

    // Runs of open samples, each an arc from its first sample to its last,
    // so that every hue in an arc lies between two samples that passed.
    std::vector<HueArc> arcs;
    std::optional<std::size_t> runStart;
    for (std::size_t k = 0; k <= samples; ++k) {
      const bool isOpen =
          k < samples && open(static_cast<double>(k) * kHueScanStep);
      if (isOpen && !runStart) {
        runStart = k;
      } else if (!isOpen && runStart) {
        arcs.push_back({.from = static_cast<double>(*runStart) * kHueScanStep,
                        .to   = static_cast<double>(k - 1) * kHueScanStep});
        runStart.reset();
      }
    }
    // A run that reaches 360 continues the one that starts at 0.
    if (arcs.size() > 1 && arcs.front().from == 0.0 &&
        arcs.back().to + kHueScanStep >= 360.0) {
      arcs.back().to = 360.0 + arcs.front().to;
      arcs.erase(arcs.begin());
    }
    std::erase_if(arcs, [](const HueArc &arc) { return arc.length() <= 0.0; });
    if (arcs.empty()) {
      throw std::logic_error(
          "dimension palette: the reserved colours leave the " +
          std::string{kBandNames.at(band)} + " tier no hue");
    }
    GLEDITOR_LOG_DEBUG(kColourCategory, "{} tier: {} open arcs",
                       kBandNames.at(band), arcs.size());
    arcs_.at(band) = std::move(arcs);
  }
}

const DimensionPalette &DimensionPalette::standard() {
  static const DimensionPalette palette{kReservedColours};
  return palette;
}

std::span<const HueArc>
DimensionPalette::openArcs(const ColourBand band) const {
  return arcs_.at(static_cast<std::size_t>(band));
}

ColourBand DimensionPalette::bandOf(const std::uint64_t ordinal) noexcept {
  return static_cast<ColourBand>(ordinal % kColourBandCount);
}

DashStyle DimensionPalette::dashOf(const std::uint64_t ordinal) noexcept {
  return static_cast<DashStyle>(ordinal % kDashStyleCount);
}

CueGlyph DimensionPalette::glyphOf(const std::uint64_t ordinal) noexcept {
  return static_cast<CueGlyph>(ordinal % kCueGlyphCount);
}

double DimensionPalette::hueOf(const std::uint64_t ordinal) const {
  const auto arcs = openArcs(bandOf(ordinal));
  const double total =
      std::ranges::fold_left(arcs, 0.0, [](const double sum, const auto &arc) {
        return sum + arc.length();
      });
  double along =
      std::fmod(static_cast<double>(ordinal) * kGoldenConjugate, 1.0) * total;
  for (const auto &arc : arcs) {
    if (along < arc.length()) {
      return wrapDegrees(arc.from + along);
    }
    along -= arc.length();
  }
  return wrapDegrees(arcs.back().to);
}

DimensionCue DimensionPalette::cueOf(const std::uint64_t ordinal) const {
  return DimensionCue{
      .colour =
          colourAt(kBandTones.at(static_cast<std::size_t>(bandOf(ordinal))),
                   hueOf(ordinal)),
      .dash  = dashOf(ordinal),
      .glyph = glyphOf(ordinal),
  };
}

DimensionCues DimensionCues::fromSettings(const SystemStoreModel &ui) {
  DimensionCues cues;
  std::uint64_t ordinal = 0;
  for (const auto &setting : ui.settings()) {
    const auto dimension = settings::dimensionOfColourKey(setting.name);
    if (!dimension) {
      continue;
    }
    auto cue = DimensionPalette::standard().cueOf(ordinal);

    const auto colour = setting.value.asInt64(0, std::int64_t{cue.colour});
    if (colour >= 0 && colour <= std::numeric_limits<std::uint32_t>::max()) {
      cue.colour = static_cast<std::uint32_t>(colour);
    } else {
      GLEDITOR_LOG_WARN(kColourCategory,
                        "{} is not an RGBA colour; the palette's stands",
                        setting.name);
    }

    const auto dashKey = settings::dimensionDashKey(*dimension);
    if (const auto dash = ui.find(dashKey)) {
      const auto name = dash->value.asString(0, dashStyleName(cue.dash));
      if (const auto style = dashStyleNamed(name)) {
        cue.dash = *style;
      } else {
        GLEDITOR_LOG_WARN(kColourCategory,
                          "{} names no dash pattern; the palette's stands",
                          dashKey);
      }
    }
    cues.entries_.push_back(
        {.dimension = std::string{*dimension}, .ordinal = ordinal, .cue = cue});
    ++ordinal;
  }
  return cues;
}

const DimensionCues::Entry *
DimensionCues::find(const std::string_view dimension) const {
  const auto it = std::ranges::find(entries_, dimension, &Entry::dimension);
  return it == entries_.end() ? nullptr : &*it;
}

std::optional<DimensionCue>
DimensionCues::cueOf(const std::string_view dimension) const {
  const auto *entry = find(dimension);
  return entry == nullptr ? std::nullopt : std::optional{entry->cue};
}

std::optional<std::uint64_t>
DimensionCues::ordinalOf(const std::string_view dimension) const {
  const auto *entry = find(dimension);
  return entry == nullptr ? std::nullopt : std::optional{entry->ordinal};
}

MicroversionId assignDimensionCue(Store &ui, const MicroversionId &parent,
                                  const std::string_view dimension) {
  auto cur           = parent.isZero() && !ui.currentVersions().empty()
                           ? ui.currentVersions().front()
                           : parent;
  const auto model   = SystemStoreModel::fromStore(ui, cur);
  std::uint64_t seen = 0;
  std::optional<std::uint64_t> ordinal;
  for (const auto &setting : model.settings()) {
    if (const auto named = settings::dimensionOfColourKey(setting.name)) {
      if (*named == dimension) {
        ordinal = seen;
      }
      ++seen;
    }
  }
  if (ordinal && model.find(settings::dimensionDashKey(dimension))) {
    return cur;
  }
  const auto cue = DimensionPalette::standard().cueOf(ordinal.value_or(seen));
  for (const auto &spec : dimensionCueSettingSpecs(dimension, cue.colour,
                                                   dashStyleName(cue.dash))) {
    cur = ensureSetting(ui, cur, spec);
  }
  GLEDITOR_LOG_DEBUG(kColourCategory, "dimension {} seen as number {}",
                     dimension, ordinal.value_or(seen));
  return cur;
}

} // namespace xanadu::view
