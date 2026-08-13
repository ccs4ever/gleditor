/**
 * @file link_layout.cpp
 * @brief Implementation of the link placement rules.
 */
#include "link_layout.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace xudu {

namespace {

/// The extent covering every occurrence of an endset in one document, or
/// nothing when none of it is there.
std::optional<std::pair<std::uint32_t, std::uint32_t>>
coveringExtent(const Version &pieces, const std::vector<PrimediaSpan> &ends) {
  auto first         = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t last = 0;
  bool any           = false;
  for (const auto &span : ends) {
    for (const auto &extent : pieces.occurrencesOf(span)) {
      any   = true;
      first = std::min(first, extent.start);
      last  = std::max(last, extent.end);
    }
  }
  if (!any) {
    return std::nullopt;
  }
  return std::make_pair(first, last);
}

} // namespace

void placeLinks(const std::map<std::uint64_t, Link> &links,
                const std::vector<const Version *> &views,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving) {
  between.clear();
  leaving.clear();

  for (const auto &[id, link] : links) {
    std::vector<LinkEnd> lefts;
    std::vector<LinkEnd> rights;
    for (std::uint32_t doc = 0; doc < views.size(); doc++) {
      if (nullptr == views[doc]) {
        continue;
      }
      if (const auto extent = coveringExtent(*views[doc], link.left)) {
        lefts.push_back(LinkEnd{doc, extent->first, extent->second});
      }
      if (const auto extent = coveringExtent(*views[doc], link.right)) {
        rights.push_back(LinkEnd{doc, extent->first, extent->second});
      }
    }

    for (const auto &left : lefts) {
      for (const auto &right : rights) {
        if (left.doc == right.doc) {
          continue;
        }
        between.push_back(LinkedPair{id, link.type, left, right});
      }
    }

    // Exactly one side present is a link reaching out of what is on screen.
    // Both sides absent is a link about something else entirely, and both
    // present has already been dealt with above.
    if (lefts.empty() != rights.empty()) {
      leaving.push_back(HalfLink{id,
                                 lefts.empty() ? rights.front() : lefts.front(),
                                 lefts.empty() ? link.left : link.right});
    }
  }
}

std::uint32_t linkColour(const LinkType type) {
  switch (type) {
  case LinkType::Comment:
    return 0x7FB2FFB0U;
  case LinkType::Illustration:
    return 0xFFC46BB0U;
  case LinkType::Disagreement:
    return 0xFF7A6BB0U;
  case LinkType::Authorship:
    return 0xB98CFFB0U;
  case LinkType::Quotation:
    return 0x7FE0A8B0U;
  case LinkType::Other:
    break;
  }
  return 0xCFCFCFB0U;
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
