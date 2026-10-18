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

/// Names a site for a reader without raw byte dumps, emitting 4-tier semantic
/// passage anchors.
using SiteNamer =
    gleditor::cpp26::function_ref<std::string(const OccurrenceSite &)>;

/**
 * @brief Returns a compact valence badge for a link type.
 *
 * In Nelsonian hypertext, links carry valence/polarity representing the
 * relationship between endsets (comment, quotation, disagreement, etc.).
 */
[[nodiscard]] std::string_view valenceBadge(LinkType type) noexcept;

/**
 * @brief A 4-tier semantic passage anchor replacing raw byte dump coordinates.
 *
 * Tiers:
 * 1. Valence badge (e.g. "[~]", "[-]", "[+]")
 * 2. Document title / container name (e.g. "twobythree", "cell 42")
 * 3. Microversion tag (e.g. "v2a1", "v1")
 * 4. Passage excerpt with 12px right alpha-fade mask
 */
struct SemanticPassageAnchor {
  std::string valenceBadge;
  std::string documentTitle;
  std::string microversionTag;
  std::string excerpt;
  bool alphaFadeMask{true};
  float alphaFadeWidthPx{12.0F};

  bool operator==(const SemanticPassageAnchor &) const = default;
};

/**
 * @brief Construct a 4-tier semantic passage anchor for an occurrence site.
 */
[[nodiscard]] SemanticPassageAnchor makeSemanticPassageAnchor(
    const OccurrenceSite &site, LinkType type = LinkType::Comment,
    std::string_view documentTitle = {}, std::string_view excerpt = {});

/**
 * @brief Format a 4-tier semantic passage anchor into a human-readable string.
 *
 * Guarantees zero raw byte dumps (no "document %u, bytes %u").
 */
[[nodiscard]] std::string
formatSemanticPassageAnchor(const SemanticPassageAnchor &anchor);

/// Where the reader is, for the panel's "reading" line.
struct ReadingPosition {
  /// The caret, or the focused cell's content; nothing when neither is known.
  std::optional<OccurrenceSite> here;
  /// Whether the reader arrived at the current visit by entering this link,
  /// which is when moving off its members is worth saying.
  bool entered{};
};

/**
 * @brief The panel for @p selected: a heading, one line per side, where the
 *        reader is relative to the link, and where the selection began.
 *
 * The reading line names every member the reader is on. Off all of them it
 * says "outside the linked range" only after an entry -- a reader who selected
 * the link from elsewhere is outside it by definition -- and is left out
 * otherwise.
 *
 * @param origin The origin visit's site, when there is one.
 */
[[nodiscard]] std::vector<PanelLine>
linkPanelLines(const SelectedLink &selected,
               const std::optional<OccurrenceSite> &origin,
               const ReadingPosition &reading, SiteNamer name);

/// One of the panel's pointer controls: what it says and what it does.
struct PanelButton {
  std::string label;
  NavigationCommand command;
  /// False when the navigator would refuse the command in this state; drawn
  /// muted, and still sent if pressed, so the refusal is the navigator's.
  bool enabled{true};

  bool operator==(const PanelButton &) const = default;
};

/**
 * @brief The panel's pointer controls, each the same command a keymap action
 *        or accessibility action sends.
 *
 * @param hasOrigin Whether Return to origin has somewhere to go.
 */
[[nodiscard]] std::vector<PanelButton>
linkPanelButtons(const SelectedLink &selected, bool hasOrigin);

} // namespace xanadu

#endif // COMMON_XANADU_LINK_PANEL_HPP
