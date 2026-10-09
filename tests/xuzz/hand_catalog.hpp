/**
 * @file hand_catalog.hpp
 * @brief A PageCatalog written by hand, as design/view-system.md §8.6 says a
 *        test writes one, for the page view tests to share.
 */
#ifndef TESTS_XUZZ_HAND_CATALOG_HPP
#define TESTS_XUZZ_HAND_CATALOG_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/view/page_view.hpp"

namespace xuzz_test {

/// One document as pagination has it so far: each page's facts, and the
/// byte at which each page's text starts.
struct HandDocument {
  xanadu::view::DocumentFacts facts;
  std::vector<xanadu::view::PageFacts> pages;
  std::vector<std::uint32_t> pageStarts;
};

class HandCatalog final : public xanadu::view::PageCatalog {
public:
  explicit HandCatalog(std::vector<HandDocument> documents)
      : documents_(std::move(documents)) {}

  [[nodiscard]] std::uint32_t documents() const noexcept override {
    return static_cast<std::uint32_t>(documents_.size());
  }
  [[nodiscard]] xanadu::view::DocumentFacts
  document(const std::uint32_t index) const noexcept override {
    return documents_.at(index).facts;
  }
  [[nodiscard]] xanadu::view::PageFacts
  page(const xanadu::view::PageRef ref) const noexcept override {
    return documents_.at(ref.document).pages.at(ref.page);
  }
  [[nodiscard]] std::optional<xanadu::view::PageRef>
  pageOf(const xanadu::DocumentSite &site) const noexcept override {
    for (std::uint32_t d = 0; d < documents_.size(); ++d) {
      const auto &doc = documents_[d];
      if (!(doc.facts.id == site.store)) {
        continue;
      }
      const auto after =
          std::ranges::upper_bound(doc.pageStarts, site.range.start);
      if (after == doc.pageStarts.begin()) {
        return std::nullopt;
      }
      const auto page =
          static_cast<std::uint32_t>(after - doc.pageStarts.begin() - 1);
      // Past the last page known so far, while pagination is still running,
      // is not yet on any page.
      if (doc.facts.paginating && after == doc.pageStarts.end()) {
        return std::nullopt;
      }
      return xanadu::view::PageRef{.document = d, .page = page};
    }
    return std::nullopt;
  }

private:
  std::vector<HandDocument> documents_;
};

} // namespace xuzz_test

#endif // TESTS_XUZZ_HAND_CATALOG_HPP
