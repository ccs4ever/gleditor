#include "link_layout.hpp"

#include <algorithm>
#include <cmath>
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
    // A format link's right end names an attribute, not a passage anywhere a
    // document is open -- see format.hpp -- so it would never find a right
    // side and would misreport as a HalfLink reaching off-screen for every
    // document showing the formatted text. Not a beam-placement concern at
    // all.
    if (LinkType::Format == link.type) {
      continue;
    }
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
        between.push_back(LinkedPair{id, link.type, link.tier, left, right});
      }
    }

    // Exactly one side present is a link reaching out of what is on screen.
    // Both sides absent is a link about something else entirely, and both
    // present has already been dealt with above.
    if (lefts.empty() != rights.empty()) {
      leaving.push_back(HalfLink{id, link.type, link.tier,
                                 lefts.empty() ? rights.front() : lefts.front(),
                                 lefts.empty() ? link.left : link.right});
    }
  }
}

std::uint32_t linkColour(const LinkType type, const ProminenceTier tier) {
  std::uint32_t rgb = 0xCFCFCF00U;
  switch (type) {
  case LinkType::Comment:
    rgb = 0x7FB2FF00U;
    break;
  case LinkType::Illustration:
    rgb = 0xFFC46B00U;
    break;
  case LinkType::Disagreement:
    rgb = 0xFF7A6B00U;
    break;
  case LinkType::Authorship:
    rgb = 0xB98CFF00U;
    break;
  case LinkType::Quotation:
    rgb = 0x7FE0A800U;
    break;
  case LinkType::Other:
    rgb = 0xCFCFCF00U;
    break;
  case LinkType::Format:
    // Not drawn as a highlighted passage at all in the end -- a format link
    // changes the glyphs themselves, which is a shaping concern rather than
    // the background-colour one this function answers -- but a case is kept
    // here so adding it did not leave this switch silently wrong about a
    // link type it does not know how to colour.
    rgb = 0xCFCFCF00U;
    break;
  }

  std::uint32_t alpha = 0xE0U;
  switch (tier) {
  case ProminenceTier::Author:
    alpha = 0xE0U;
    break;
  case ProminenceTier::Curated:
    alpha = 0xB0U;
    break;
  case ProminenceTier::Public:
    alpha = 0x60U;
    break;
  }
  return rgb | alpha;
}

std::uint32_t linkColourWithInstanceShift(const std::uint64_t linkId,
                                          const LinkType type,
                                          const ProminenceTier tier) {
  const auto base = linkColour(type, tier);
  if (0 == linkId) {
    return base;
  }
  const float r = static_cast<float>((base >> 24) & 0xFFU) / 255.0F;
  const float g = static_cast<float>((base >> 16) & 0xFFU) / 255.0F;
  const float b = static_cast<float>((base >> 8) & 0xFFU) / 255.0F;
  const auto a  = base & 0xFFU;

  const float maxVal = std::max({r, g, b});
  const float minVal = std::min({r, g, b});
  const float delta  = maxVal - minVal;

  float h = 0.0F;
  if (delta > 0.0001F) {
    if (maxVal == r) {
      h = std::fmod((g - b) / delta, 6.0F);
    } else if (maxVal == g) {
      h = ((b - r) / delta) + 2.0F;
    } else {
      h = ((r - g) / delta) + 4.0F;
    }
    h /= 6.0F;
    if (h < 0.0F) {
      h += 1.0F;
    }
  }
  const float s = maxVal > 0.0001F ? delta / maxVal : 0.0F;
  const float v = maxVal;

  // Deterministic micro-hue offset of +/- 7% based on golden ratio hash of
  // linkId
  const float hashFrac =
      static_cast<float>((linkId * 0x9E3779B97F4A7C15ULL) % 10000ULL) /
      10000.0F;
  const float hueShift = (hashFrac - 0.5F) * 0.14F;
  float newH           = std::fmod(h + hueShift + 1.0F, 1.0F);

  // Convert back to RGB
  const float c = v * s;
  const float x = c * (1.0F - std::abs(std::fmod(newH * 6.0F, 2.0F) - 1.0F));
  const float m = v - c;

  float newR = 0.0F, newG = 0.0F, newB = 0.0F;
  const int hSector = static_cast<int>(newH * 6.0F) % 6;
  switch (hSector) {
  case 0:
    newR = c;
    newG = x;
    newB = 0.0F;
    break;
  case 1:
    newR = x;
    newG = c;
    newB = 0.0F;
    break;
  case 2:
    newR = 0.0F;
    newG = c;
    newB = x;
    break;
  case 3:
    newR = 0.0F;
    newG = x;
    newB = c;
    break;
  case 4:
    newR = x;
    newG = 0.0F;
    newB = c;
    break;
  case 5:
  default:
    newR = c;
    newG = 0.0F;
    newB = x;
    break;
  }

  const auto outR =
      static_cast<std::uint32_t>(std::clamp((newR + m) * 255.0F, 0.0F, 255.0F));
  const auto outG =
      static_cast<std::uint32_t>(std::clamp((newG + m) * 255.0F, 0.0F, 255.0F));
  const auto outB =
      static_cast<std::uint32_t>(std::clamp((newB + m) * 255.0F, 0.0F, 255.0F));

  return (outR << 24) | (outG << 16) | (outB << 8) | a;
}

float linkPhaseOffset(const std::uint64_t linkId) {
  return static_cast<float>((linkId * 0x517CC1B727220A95ULL) % 10000ULL) /
         10000.0F;
}

} // namespace xudu
