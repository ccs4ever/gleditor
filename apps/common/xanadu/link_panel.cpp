/**
 * @file link_panel.cpp
 * @brief The selected-link panel's text.
 */
#include "common/xanadu/link_panel.hpp"

#include <format>

namespace xanadu {

namespace {

// An unset cursor is shown as a dash rather than left blank or defaulted, so
// a reader can tell "nothing chosen" from "the first one".
constexpr std::string_view kUnset = "—";
constexpr std::string_view kSep   = " · ";

PanelLine sideLine(const SelectedLink &selected,
                   const LinkOccurrences &resolved, const LinkSide side,
                   SiteNamer name) {
  const auto &members = resolved.members(side);
  const auto &cursor  = selected.cursor(side);
  const bool active   = side == selected.active;

  std::string text = std::format(
      "{} {}/{}", LinkSide::Left == side ? "Left" : "Right",
      cursor.member ? std::to_string(*cursor.member + 1) : std::string{kUnset},
      members.size());
  if (!cursor.member) {
    return {.text   = std::move(text),
            .tone   = PanelLine::Tone::Muted,
            .active = active};
  }

  const auto &member = members[*cursor.member];
  if (!member.inView()) {
    text += std::format("{}not in view", kSep);
    return {.text   = std::move(text),
            .tone   = PanelLine::Tone::Muted,
            .active = active};
  }
  text += std::format("{}occurrence {}/{}", kSep,
                      cursor.occurrence ? std::to_string(*cursor.occurrence + 1)
                                        : std::string{kUnset},
                      member.occurrences.size());
  if (cursor.occurrence) {
    const auto &occurrence = member.occurrences[*cursor.occurrence];
    text += std::format("{}{}", kSep, name(occurrence.site));
    if (Coverage::Partial == occurrence.coverage) {
      text += " (part)";
    }
  }
  return {.text   = std::move(text),
          .tone   = active ? PanelLine::Tone::Active : PanelLine::Tone::Normal,
          .active = active};
}

} // namespace

std::vector<PanelLine>
linkPanelLines(const SelectedLink &selected,
               const std::optional<OccurrenceSite> &origin, SiteNamer name) {
  std::vector<PanelLine> lines;
  if (!selected.occurrences) {
    lines.push_back(
        {.text = std::format("link {}{}resolving", selected.key.id, kSep),
         .tone = PanelLine::Tone::Muted});
    return lines;
  }

  const auto &link = selected.occurrences->link;
  auto heading     = std::format("{}{}link {}", linkTypeName(link.type), kSep,
                                 selected.key.id);
  if (!link.owner.empty()) {
    heading += std::format("{}{}", kSep, link.owner);
  }
  lines.push_back({.text = std::move(heading)});
  lines.push_back(
      sideLine(selected, *selected.occurrences, LinkSide::Left, name));
  lines.push_back(
      sideLine(selected, *selected.occurrences, LinkSide::Right, name));
  lines.push_back(
      {.text = std::format("origin: {}", origin ? name(*origin) : "none"),
       .tone = PanelLine::Tone::Muted});
  return lines;
}

} // namespace xanadu
