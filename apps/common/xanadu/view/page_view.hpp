/**
 * @file page_view.hpp
 * @brief What a page view is given, and the interface it implements.
 *
 * design/view-system.md §8.6 and §10.1. A page view arranges pages; it does
 * not make them. Text, pagination and the caret stay with the library's
 * documents and links with the engine, so a page view sees only a
 * PageCatalog: how many documents and pages there are, how big each page is,
 * which pages are marked, and where the ends of the active link fall. That
 * is how a page view stays a pure function of its input (V-R37), and why a
 * test can drive one with a catalog written by hand.
 *
 * A page view has no view space: pages are not cells and it mints nothing.
 */
#ifndef COMMON_XANADU_VIEW_PAGE_VIEW_HPP
#define COMMON_XANADU_VIEW_PAGE_VIEW_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <span>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/link_occurrences.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace xanadu::view {

/// A page of one of the documents a placement shows.
struct PageRef {
  std::uint32_t document{}; // position in the placement's document list
  std::uint32_t page{};
  auto operator<=>(const PageRef &) const = default;
};

/// The identity a page's records carry, so a page keeps its id however a
/// view moves it and the animation layer can tween it (§8.9).
[[nodiscard]] constexpr SubjectId
subjectOf(const PageRef ref, const std::uint32_t slot = 0) noexcept {
  return SubjectId::page(ref.document, ref.page, slot);
}

struct DocumentFacts {
  // DocumentId's own default draws a random name: a catalog states the id.
  DocumentId id;
  std::uint32_t pages{}; // known so far
  bool paginating{};     // more are coming
  bool background{};     // shown for context, behind the row
  bool rightToLeft{};    // base direction of the text
};

struct PageFacts {
  float width{}, height{}, lineHeight{}; // local units
  bool link{};         // holds an end of a non-formatting link
  bool transclusion{}; // holds content also present elsewhere

  /// Marked pages are the ones a view keeps legible (§10.1).
  [[nodiscard]] constexpr bool marked() const noexcept {
    return link || transclusion;
  }
};

/// What pagination and the link index currently know. The presenter
/// implements it over the library's documents; a test implements it by hand.
/// Every answer is about what is known now: a document may gain pages while
/// it is shown, and the view is laid out again when they arrive.
class PageCatalog {
public:
  virtual ~PageCatalog()                                         = default;
  [[nodiscard]] virtual std::uint32_t documents() const noexcept = 0;
  [[nodiscard]] virtual DocumentFacts
  document(std::uint32_t index) const noexcept                      = 0;
  [[nodiscard]] virtual PageFacts page(PageRef page) const noexcept = 0;
  /// The page that holds @p site, if pagination has reached it.
  [[nodiscard]] virtual std::optional<PageRef>
  pageOf(const DocumentSite &site) const noexcept = 0;

protected:
  // Copying through the interface would slice; a catalog written by hand may
  // still be copied as itself.
  PageCatalog()                               = default;
  PageCatalog(const PageCatalog &)            = default;
  PageCatalog &operator=(const PageCatalog &) = default;
  PageCatalog(PageCatalog &&)                 = default;
  PageCatalog &operator=(PageCatalog &&)      = default;
};

/// One end of a link, or of a transclusion, on a page.
struct LinkEnd {
  PageRef page;
  float top{}, bottom{}; // of the passage, from the page's top, local units
  LinkSide side{LinkSide::Left};
  std::uint32_t member{};
};

/// The link or transclusion the reader is following. Its ends are every
/// member of both endsets, so a many-to-many link brings all of its pages.
struct ActiveLink {
  LinkKey key;
  std::span<const LinkEnd> ends;
  std::uint32_t anchor{}; // index into ends: the end the reader is at
};

struct PageCursor {
  PageRef page; // the page that holds the caret
  bool operator==(const PageCursor &) const = default;
};

/// Everything one layout of a page view may read. The catalog and the ends
/// are borrowed for the call.
struct PageLayoutInput {
  const PageCatalog &catalog;
  PageCursor cursor;
  std::optional<ActiveLink> active;
  PaneFrame frame;
  std::uint32_t subview{};
  std::optional<SubjectId> hover;
};

class PageView : public View {
public:
  /// Fill caches (the stagger search of §10.4). Mints nothing: a page view
  /// has no view space.
  virtual PageView *prepare(const PageLayoutInput & /*in*/) { return this; }
  virtual void layout(const PageLayoutInput &in,
                      LayoutSink &out) const noexcept = 0;
  /// How to travel from one cursor to another: MotionHints into @p out. A
  /// view with nothing to say leaves the default ease to every subject.
  virtual void transition(const PageLayoutInput & /*from*/,
                          const PageLayoutInput & /*to*/,
                          LayoutSink & /*out*/) const noexcept {}
};

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_PAGE_VIEW_HPP
