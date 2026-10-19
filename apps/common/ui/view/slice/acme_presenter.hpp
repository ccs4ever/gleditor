/**
 * @file acme_presenter.hpp
 * @brief Hardware-bridging presenter for AcmeSliceView mouse interaction and
 * chording.
 *
 * Maps screen-space pointer clicks to Acme B1/B2/B3 chords, hit-testing
 * spatial panes and tag buffers, and routing events to AcmeSliceView.
 */
#ifndef XANADU_UI_VIEW_SLICE_ACME_PRESENTER_HPP
#define XANADU_UI_VIEW_SLICE_ACME_PRESENTER_HPP

#include <memory>
#include <string>

#include "common/ui/view/slice/slice_view_presenter.hpp"
#include "common/xanadu/view/slice/acme_view.hpp"

namespace xanadu::ui {

/**
 * @class AcmePresenter
 * @brief Acme-specific UI presenter handling mouse chording (B1/B2/B3) and
 * spatial panes.
 */
class AcmePresenter : public SliceViewPresenter {
public:
  explicit AcmePresenter(std::shared_ptr<view::AcmeSliceView> acmeView,
                         std::string fontName = "Sans 12");
  ~AcmePresenter() override = default;

  [[nodiscard]] const view::AcmeSliceView *acmeView() const noexcept {
    return acmeView_.get();
  }

  /**
   * @brief Handles mouse button chords at screen position (x, y).
   *
   * Finds the target pane or cell under (x, y) and forwards to
   * AcmeSliceView::dispatchChord.
   */
  view::AcmeChordResult
  handlePointerChord(view::AcmeChord chord, float x, float y,
                     const view::ViewManifold &space,
                     zigzag::CommandMailbox *mailbox     = nullptr,
                     zigzag::ArenaManifold *mailboxArena = nullptr);

private:
  std::shared_ptr<view::AcmeSliceView> acmeView_;
};

} // namespace xanadu::ui

#endif // XANADU_UI_VIEW_SLICE_ACME_PRESENTER_HPP
