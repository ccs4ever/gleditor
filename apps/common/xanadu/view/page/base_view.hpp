/**
 * @file base_view.hpp
 * @brief page.base: what xuzz does with pages today, as a view.
 *
 * design/view-system.md §10.3. At rest, foreground documents stand in a row
 * in list order and each one's pages flow down its column, which is the
 * library's own arrangement (Doc's flow matrices, render::kDefaultDocumentGap)
 * produced by a view. When a link is active its pages come together: the
 * anchor page stays, every other page holding an end is brought beside it by
 * a CoalesceStrategy with the passages level, lifted in front of the row, and
 * leaves a ghost and a tether at home; the rest of the row dims. What does not
 * fit at a readable size is shown as windows round the passages.
 *
 * Two sub-views (V-R49) say what moves:
 *
 * - "pages", the default, as §10.3.2 says: only the pages that hold ends fly;
 *   a document every page of which takes part, or with one page, moves whole.
 * - "documents": every document of the row is a body and the anchor's is
 *   pinned, which is how LinkBeams loads its engine. With physics off (the
 *   default, cli.hpp) this is the row users see today: documents keep their
 *   slots and those holding ends move only up or down. It shows no windows.
 *
 * The view is a pure function of its input (V-R2): it reads no clock, keeps
 * no cursor, and two layouts of one input are identical. It does keep
 * scratch storage between layouts, overwritten before it is read, so a layout
 * that has reached its size reuses it; one instance serves one placement on
 * its owner thread (§8.1, §8.9).
 */
#ifndef COMMON_XANADU_VIEW_PAGE_BASE_VIEW_HPP
#define COMMON_XANADU_VIEW_PAGE_BASE_VIEW_HPP

#include <cstdint>
#include <memory>
#include <string_view>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/tension_layout.hpp"
#include "common/xanadu/view/page_view.hpp"
#include "common/xanadu/view/view.hpp"

namespace xanadu::view {

/// Everything the base view reads from system://layout.
struct BaseViewSettings {
  PageViewsConfig pages{};
  PageBaseConfig base{};
  /// In the engine's own units; base.physicsUnitPx converts.
  TensionParams physics{};

  [[nodiscard]] static BaseViewSettings
  fromLayout(const LayoutConfig &layout) noexcept {
    return {.pages   = layout.pages,
            .base    = layout.pageBase,
            .physics = layout.physics.toTensionParams()};
  }
};

/// What moves when a link is active: the base view's sub-views, as
/// PageLayoutInput::subview carries them, in the order the reader cycles.
enum class BaseUnit : std::uint32_t { Pages = 0, Documents = 1 };

/// The slots of a ghost's marker id, so a page's ghost and a document's are
/// different subjects even where their numbers agree.
inline constexpr std::uint32_t kPageGhostSlot     = 0;
inline constexpr std::uint32_t kDocumentGhostSlot = 1;

class BaseView final : public PageView {
public:
  static constexpr std::string_view kKind = "page.base";

  explicit BaseView(BaseViewSettings settings);
  ~BaseView() override;
  BaseView(const BaseView &)            = delete;
  BaseView &operator=(const BaseView &) = delete;
  BaseView(BaseView &&)                 = delete;
  BaseView &operator=(BaseView &&)      = delete;

  /// New settings are a changed input: the placement is laid out again.
  BaseView *configure(const BaseViewSettings &settings) noexcept;
  [[nodiscard]] const BaseViewSettings &settings() const noexcept {
    return settings_;
  }

  [[nodiscard]] std::string_view kind() const noexcept override {
    return kKind;
  }

  /// Every document's frame, then its pages in order, then a ghost and a
  /// tether for every page that flew. A subview the view does not have is
  /// laid out as the default.
  void layout(const PageLayoutInput &in,
              LayoutSink &out) const noexcept override;

  /// The page brought to the reader is the subject of the move and starts
  /// at once, taking page.base.subjectMs; everything else that moves makes
  /// room, waiting rowDelayMs and taking rowMs (§10.3.3). Subjects that
  /// appear or vanish get no hint: the animation layer fades them.
  void transition(const PageLayoutInput &from, const PageLayoutInput &to,
                  LayoutSink &out) const noexcept override;

private:
  struct Work;

  BaseViewSettings settings_;
  // Scratch reused by every layout. Through a pointer, so layout() can stay
  // const: what it holds never decides what a layout produces.
  std::unique_ptr<Work> work_;
};

/// The descriptor a host registers. The view's settings live in
/// system://layout beside the rest of page.base.*, read through
/// LayoutConfig, so the descriptor seeds none of its own.
[[nodiscard]] ViewDescriptor baseViewDescriptor(BaseViewSettings settings);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_PAGE_BASE_VIEW_HPP
