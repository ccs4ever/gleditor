/**
 * @file dimension_colour.hpp
 * @brief A dimension's colour, dash and glyph: the rule that generates them,
 *        the colours the rule keeps clear of, and the reader's overrides in
 *        system://ui.
 *
 * design/view-system-implementation-plan.md §5.2, with the rule fixed by
 * spike V1 (§3.1). A dimension has one colour everywhere it is drawn — a
 * strand, a compass arm, a wheel's edge (view-system.md V31) — and colour is
 * never the only cue: each colour comes with a dash pattern and a glyph that
 * cycle independently of it.
 *
 * The n-th dimension seen takes:
 *
 * - band n mod 3: vivid, deep, pale, three OKLCH lightness and chroma pairs
 *   that are in the sRGB gamut at every open hue;
 * - the hue at frac(n times the golden ratio's conjugate), laid along the
 *   band's open arcs only, so adding a dimension never moves an existing one
 *   and consecutive dimensions land far apart;
 * - dash n mod 6 and glyph n mod 7. Six and seven are coprime, so a dash and
 *   glyph pair repeats only every 42 dimensions, and neighbours differ in
 *   band, dash and glyph all three.
 *
 * The open arcs are what the colours with a fixed meaning (link types,
 * transclusion, focus, accent, tether, the pouch auras) leave: each family of
 * them reserves the hues between its outermost members plus a guard, and
 * within what is left a band's colour must also stand at least a CIEDE2000
 * distance of 10 from every one of them, so no dimension reads as a link
 * type or a transclusion. Decorative palettes (per-author, lineage, badges,
 * HUD tints) mean nothing a dimension could be mistaken for and are not
 * reserved; reserving them too leaves too little hue to use (V1).
 *
 * "Seen" is recorded, not recomputed: the first time a dimension is seen its
 * colour and dash are minted as ui.dimension.<name>.colour and .dash in
 * system://ui, defaulting to what the rule gives it, and its ordinal is its
 * place among those settings. The reader changes either setting to override
 * the rule; the glyph follows the ordinal.
 *
 * Device-free, so the engine's layouts and every front end agree on a
 * dimension's cue; the colour maths is the library's header-only
 * <gleditor/color_space.hpp>.
 */
#ifndef COMMON_XANADU_VIEW_DIMENSION_COLOUR_HPP
#define COMMON_XANADU_VIEW_DIMENSION_COLOUR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::view {

/// Lightness and chroma tiers, in the order the rule cycles them.
enum class ColourBand : std::uint8_t { Vivid, Deep, Pale };
inline constexpr std::size_t kColourBandCount = 3;

/// Stroke patterns, in the order the rule cycles them. Dimensions 21 apart
/// share band and glyph and land close in hue (21 times the golden ratio's
/// conjugate is nearly whole), so the dash alone tells them apart; this order
/// pairs those as solid and dash-dot-dot, dash and dash-dot, dot and
/// long-short, where V1 found dash against long-short too alike.
enum class DashStyle : std::uint8_t {
  Solid,
  Dash,
  Dot,
  DashDotDot,
  DashDot,
  LongShort,
};
inline constexpr std::size_t kDashStyleCount = 6;

/// Marks, in the order the rule cycles them. A cross (a diagonal X) stands
/// where a hexagon was: at glyph size a hexagon reads as the circle.
enum class CueGlyph : std::uint8_t {
  Circle,
  Triangle,
  Square,
  Diamond,
  Plus,
  Cross,
  Ring,
};
inline constexpr std::size_t kCueGlyphCount = 7;

[[nodiscard]] std::string_view dashStyleName(DashStyle style) noexcept;
[[nodiscard]] std::optional<DashStyle> dashStyleNamed(std::string_view name);
[[nodiscard]] std::string_view cueGlyphName(CueGlyph glyph) noexcept;
[[nodiscard]] std::string_view colourBandName(ColourBand band) noexcept;

/// What a dimension is drawn with.
struct DimensionCue {
  std::uint32_t colour{}; ///< 0xRRGGBBAA
  DashStyle dash{};
  CueGlyph glyph{};

  bool operator==(const DimensionCue &) const = default;
};

/// A colour with a fixed meaning, which no dimension may resemble. @p family
/// groups the colours whose hues reserve one band of the hue circle together.
struct ReservedColour {
  std::string_view family;
  std::string_view meaning;
  std::uint32_t rgba{}; ///< as drawn, 0xRRGGBBAA
};

/// The colours with a fixed meaning, one entry per distinct colour, each as
/// its source draws it. The UI test reads every source and fails when one
/// differs from its entry here.
[[nodiscard]] std::span<const ReservedColour> reservedColours() noexcept;

/// An arc of OKLCH hue in degrees, from @p from up to @p to; @p from is in
/// [0, 360) and @p to may pass 360 when the arc wraps.
struct HueArc {
  double from{};
  double to{};

  [[nodiscard]] double length() const noexcept { return to - from; }
  bool operator==(const HueArc &) const = default;
};

/// The rule, over one set of reserved colours. Immutable once built.
class DimensionPalette {
public:
  explicit DimensionPalette(std::span<const ReservedColour> reserved);

  /// The palette over reservedColours(), built once.
  [[nodiscard]] static const DimensionPalette &standard();

  /// The hues left to @p band: outside every guarded family, and where the
  /// band's colour stands clear of every reserved colour. In ascending hue.
  [[nodiscard]] std::span<const HueArc> openArcs(ColourBand band) const;

  [[nodiscard]] static ColourBand bandOf(std::uint64_t ordinal) noexcept;
  [[nodiscard]] static DashStyle dashOf(std::uint64_t ordinal) noexcept;
  [[nodiscard]] static CueGlyph glyphOf(std::uint64_t ordinal) noexcept;
  /// OKLCH hue in degrees of the @p ordinal-th dimension's colour.
  [[nodiscard]] double hueOf(std::uint64_t ordinal) const;
  /// The rule's cue for the @p ordinal-th dimension seen, from 0.
  [[nodiscard]] DimensionCue cueOf(std::uint64_t ordinal) const;

private:
  std::array<std::vector<HueArc>, kColourBandCount> arcs_;
};

/// Every dimension's cue as system://ui records it: the reader's overrides
/// where set, the rule's assignment otherwise.
class DimensionCues {
public:
  [[nodiscard]] static DimensionCues fromSettings(const SystemStoreModel &ui);

  [[nodiscard]] std::optional<DimensionCue>
  cueOf(std::string_view dimension) const;
  /// Place in the order the dimensions were first seen, from 0.
  [[nodiscard]] std::optional<std::uint64_t>
  ordinalOf(std::string_view dimension) const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  struct Entry {
    std::string dimension;
    std::uint64_t ordinal{};
    DimensionCue cue;
  };
  [[nodiscard]] const Entry *find(std::string_view dimension) const;
  std::vector<Entry> entries_;
};

/// Record @p dimension as seen in the system://ui store @p ui, minting its
/// colour and dash settings with the rule's cue for the next ordinal. A
/// dimension already seen keeps what it has; the version is then @p parent's.
MicroversionId assignDimensionCue(Store &ui, const MicroversionId &parent,
                                  std::string_view dimension);

} // namespace xanadu::view

#endif
