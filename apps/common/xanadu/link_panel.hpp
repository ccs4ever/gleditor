/**
 * @file link_panel.hpp
 * @brief What the selected-link panel says, as lines of text.
 *
 * Kept apart from the drawing so that what a reader is told -- which link,
 * which side is active, which member and occurrence each side has chosen, and
 * which are unset -- is decided once and can be checked without a window. The
 * overlay only places these lines; the accessibility tree says the same
 * things through its own nodes.
 */
#ifndef COMMON_XANADU_LINK_PANEL_HPP
#define COMMON_XANADU_LINK_PANEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/link_navigation.hpp"

namespace xanadu {

struct PanelLine {
  enum class Tone : std::uint8_t {
    /// Ordinary text.
    Normal,
    /// The active side.
    Active,
    /// Nothing chosen, nothing in view, or context rather than content.
    Muted,
  };

  std::string text;
  Tone tone{Tone::Normal};
  /// The active side's line says so in words as well as by tone, since tone
  /// is a colour and not everybody can see one. Drawn in a gutter of its own
  /// so the side names line up whichever is active.
  bool active{};

  bool operator==(const PanelLine &) const = default;
};

/// Names a site for a reader: "document 2, bytes 10 to 14".
using SiteNamer =
    gleditor::cpp26::function_ref<std::string(const OccurrenceSite &)>;

/**
 * @brief The panel for @p selected: a heading, one line per side, and where
 *        the selection began.
 *
 * @param origin The origin visit's site, when there is one.
 */
[[nodiscard]] std::vector<PanelLine>
linkPanelLines(const SelectedLink &selected,
               const std::optional<OccurrenceSite> &origin, SiteNamer name);

} // namespace xanadu

#endif // COMMON_XANADU_LINK_PANEL_HPP
