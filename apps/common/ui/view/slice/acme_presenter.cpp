#include "common/ui/view/slice/acme_presenter.hpp"

namespace xanadu::ui {

AcmePresenter::AcmePresenter(std::shared_ptr<view::AcmeSliceView> acmeView,
                             std::string fontName)
    : SliceViewPresenter(acmeView, std::move(fontName)),
      acmeView_(std::move(acmeView)) {}

view::AcmeChordResult AcmePresenter::handlePointerChord(
    view::AcmeChord chord, float x, float y, const view::ViewManifold &space,
    zigzag::CommandMailbox *mailbox, zigzag::ArenaManifold *mailboxArena) {
  if (!acmeView_) {
    return view::AcmeChordResult{};
  }

  // 1. Hit test frames to find target pane
  std::optional<zigzag::CellRef> hitPane = std::nullopt;
  for (const auto &frame : sink_.frames()) {
    const float left   = frame.centre.x - (frame.width * 0.5F);
    const float right  = frame.centre.x + (frame.width * 0.5F);
    const float bottom = frame.centre.y - (frame.height * 0.5F);
    const float top    = frame.centre.y + (frame.height * 0.5F);

    if (x >= left && x <= right && y >= bottom && y <= top) {
      hitPane = static_cast<zigzag::CellRef>(frame.id.value);
      break;
    }
  }

  // 2. Hit test items for text / token
  std::string hitText;
  for (const auto &item : sink_.items()) {
    const float left   = item.centre.x - (item.width * 0.5F);
    const float right  = item.centre.x + (item.width * 0.5F);
    const float bottom = item.centre.y - (item.height * 0.5F);
    const float top    = item.centre.y + (item.height * 0.5F);

    if (x >= left && x <= right && y >= bottom && y <= top) {
      hitText   = "cell_" + std::to_string(item.id.value);
      selected_ = item.id;
      ++revision_;
      break;
    }
  }

  if (hitText.empty() && hitPane.has_value()) {
    hitText = "pane_" + std::to_string(*hitPane);
  }

  view::AcmeEvent ev{};
  ev.pane  = hitPane;
  ev.chord = chord;
  ev.text  = hitText;
  ev.q0    = 0;
  ev.q1    = hitText.size();

  return acmeView_->dispatchChord(space, ev, mailbox, mailboxArena);
}

} // namespace xanadu::ui
