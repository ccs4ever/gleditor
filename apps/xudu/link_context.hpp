/**
 * @file link_context.hpp
 * @brief The selected-link context of a running xudu or xuzz.
 *
 * The engine's LinkNavigator decides what a gesture means; this carries its
 * answers out against the open session -- resolving a link's occurrences in
 * the open documents and the bridge's cells, and moving the caret or the
 * ZigZag focus when, and only when, the navigator says focus moves. Beam
 * picks, cell activation, accessibility actions and keymap actions all end up
 * in execute(), which is the reason it exists.
 *
 * Why C++ rather than Vortex: this is the render-thread seam between the
 * navigator and the caret, document views and presentation surface, none of
 * which Vortex reaches. The keymap actions that call it are Vortex-bound.
 */
#ifndef XUDU_LINK_CONTEXT_HPP
#define XUDU_LINK_CONTEXT_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/link_panel.hpp"
#include "xudu/session.hpp"

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xudu {

class LinkContext {
public:
  /// Put the caret on @p range of the open view at @p viewIndex.
  using FocusDocument =
      std::function<void(std::size_t viewIndex, xanadu::Extent range)>;
  /// Bring @p cell into focus in the ZigZag presentation.
  using FocusCell =
      std::function<void(zigzag::CellRef cell, xanadu::Extent range)>;
  /// The caret, as an open view index and a byte offset, when it is showing.
  struct CaretPosition {
    std::uint32_t view{};
    std::uint32_t offset{};
    bool operator==(const CaretPosition &) const = default;
  };
  using CaretQuery = std::function<std::optional<CaretPosition>()>;
  /// The ZigZag presentation's focused cell, or noCell.
  using CellFocusQuery = std::function<zigzag::CellRef()>;

  /// What the reading position depends on, as plain values, so a caller can
  /// tell each frame whether it moved without building the position itself.
  struct ReadingStamp {
    std::optional<CaretPosition> caret;
    zigzag::CellRef cell{zigzag::noCell};
    std::optional<xanadu::VisitId> visit;
    bool operator==(const ReadingStamp &) const = default;
  };

  explicit LinkContext(Session &session) : session(session) {}

  void setFocusDocument(FocusDocument handler) {
    focusDocument = std::move(handler);
  }
  void setFocusCell(FocusCell handler) { focusCell = std::move(handler); }
  void setCaretQuery(CaretQuery query) { caretQuery = std::move(query); }
  void setCellFocusQuery(CellFocusQuery query) {
    cellFocusQuery = std::move(query);
  }

  /// The bridge's manifold, a replay of the primary store, or null outside
  /// xuzz. Every cell in it is searched, so that an endpoint outside the
  /// visible neighbourhood can still be entered.
  void setManifold(const zigzag::Manifold *bridge) noexcept {
    manifold = bridge;
  }

  /// The links on screen, in the stable order Next/Previous link walk.
  void setCandidates(const std::vector<zigzag::CellRef> &linkIds);

  /// The key of link @p id in the primary store, which holds every link the
  /// beams draw; federated links need their own authority design.
  [[nodiscard]] xanadu::LinkKey keyOf(zigzag::CellRef id) const;

  /**
   * @brief Carry out @p command. Render thread only: it may move the caret.
   *
   * A refused command is logged and returned, never retried against some
   * other link or occurrence.
   */
  xanadu::NavigationResult execute(const xanadu::NavigationCommand &command);

  [[nodiscard]] gleditor::cpp26::optional<const xanadu::SelectedLink &>
  selection() const noexcept {
    return navigator.selection();
  }
  /// What is being shown without moving there, if anything.
  [[nodiscard]] const std::optional<xanadu::Preview> &preview() const noexcept {
    return previewing;
  }
  [[nodiscard]] ReadingStamp readingStamp() const;

  /**
   * @brief Where the reader is, for the panel.
   *
   * The caret, unless the current visit is a cell, in which case the ZigZag
   * focus: a reader who entered a cell reads on along its rank.
   */
  [[nodiscard]] xanadu::ReadingPosition reading() const;

  /// Where the selected link was selected from, if that was anywhere.
  [[nodiscard]] std::optional<xanadu::OccurrenceSite> originSite() const;

  /// The open view showing @p site's version, if one is.
  [[nodiscard]] std::optional<std::size_t>
  viewIndexOf(const xanadu::DocumentSite &site) const;

  /// @p site in words: "document 2, bytes 10 to 14" or "cell 7, bytes 0 to
  /// 5". Shared by the panel and the accessibility tree so both say the same.
  [[nodiscard]] std::string describe(const xanadu::OccurrenceSite &site) const;

  /// Bumped by every change a reader of selection() might care about.
  [[nodiscard]] std::uint64_t revision() const noexcept { return changes; }

private:
  [[nodiscard]] std::expected<xanadu::LinkOccurrences, xanadu::LinkQueryError>
  resolve(const xanadu::LinkKey &key) const;
  void noteOrigin();
  [[nodiscard]] std::optional<xanadu::OccurrenceSite> caretSite() const;
  [[nodiscard]] std::optional<xanadu::OccurrenceSite>
  cellSite(zigzag::CellRef cell) const;
  void apply(xanadu::NavigationEffect effect);
  void focus(const xanadu::OccurrenceSite &site);

  Session &session;
  const zigzag::Manifold *manifold{};
  xanadu::InMemoryActivityLog activity;
  xanadu::LinkNavigator navigator{activity};
  FocusDocument focusDocument;
  FocusCell focusCell;
  CaretQuery caretQuery;
  CellFocusQuery cellFocusQuery;
  std::optional<xanadu::Preview> previewing;
  std::uint64_t changes{};
};

} // namespace xudu

#endif // XUDU_LINK_CONTEXT_HPP
