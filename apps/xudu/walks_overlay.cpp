#include "xudu/walks_overlay.hpp"

#include <algorithm>
#include <array>
#include <gleditor/logging.hpp>
#include <glm/ext/matrix_clip_space.hpp>

namespace xudu {
namespace {
constexpr std::array<std::string_view, 7> kControls{
    "Previous",      "Next",      "Restore visit", "Reference (R)",
    "Edit note (N)", "Save note", "Close"};
}

void WalksOverlay::open() {
  const std::scoped_lock lock(mutex_);
  const auto visits = context_.savedVisits();
  visits_.assign(visits.begin(), visits.end());
  chosen_  = context_.currentVisit() ? context_.currentVisit()->value - 1 : 0;
  first_   = 0;
  visible_ = true;
  editing_ = false;
  focus_   = 0;
  status_.clear();
  ++revision_;
}

void WalksOverlay::setConfig(const xanadu::LinkPanelConfig &config) {
  const std::scoped_lock lock(mutex_);
  const bool fontChanged = config_.font != config.font;
  config_                = config;
  if (fontChanged && device_) {
    canvas_ = std::make_unique<gleditor::Canvas>(device_, config_.font);
    canvas_->createPipeline(pipeline_, false);
  }
  ++revision_;
}

void WalksOverlay::deviceReady(render::RenderDevice &device,
                               const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(mutex_);
  device_   = &device;
  pipeline_ = pipeline;
  canvas_   = std::make_unique<gleditor::Canvas>(&device, config_.font);
  canvas_->createPipeline(pipeline, false);
}

bool WalksOverlay::grabbing() const {
  const std::scoped_lock lock(mutex_);
  return visible_;
}

void WalksOverlay::move(const int delta) {
  if (editing_) {
    status_ = "Save or cancel the note before changing visits";
    return;
  }
  if (visits_.empty()) return;
  if (delta < 0 && chosen_ > 0) --chosen_;
  if (delta > 0 && chosen_ + 1 < visits_.size()) ++chosen_;
  focus_ = 0;
  status_.clear();
  ++revision_;
}

void WalksOverlay::activate(const std::uint64_t control) {
  renderer_->runWithState([this, control](RenderState &) {
    const std::scoped_lock lock(mutex_);
    if (!visible_) return;
    ++revision_;
    if (control == Close) {
      visible_ = false;
      return;
    }
    if (control == Previous) {
      move(-1);
      return;
    }
    if (control == Next) {
      move(1);
      return;
    }
    if (control >= kRow) {
      const auto row = control - kRow;
      if (!editing_ && row < visits_.size()) {
        chosen_ = row;
        focus_  = 0;
        status_.clear();
      }
      return;
    }
    if (visits_.empty()) return;
    const auto id = visits_[chosen_].id;
    try {
      if (control == Restore) {
        if (editing_) {
          status_ = "Save or cancel the note first";
          return;
        }
        const auto result = context_.execute(xanadu::nav::EnterSavedVisit{id});
        if (result)
          visible_ = false;
        else
          status_ = std::string(xanadu::name(result.error()));
      } else if (control == Reference) {
        context_.referenceVisit(id);
        status_ = "Visit referenced";
      } else if (control == Edit) {
        note_    = context_.visitNote(id);
        editing_ = true;
        focus_   = kNote;
      } else if (control == Save && editing_) {
        context_.annotateVisit(id, note_);
        editing_ = false;
        focus_   = 0;
        status_  = "Note saved";
      }
    } catch (const std::exception &error) {
      status_ = "Activity could not be saved";
      GLEDITOR_LOG_WARN("xudu.activity", "Walks action failed: {}",
                        error.what());
    }
  });
}

bool WalksOverlay::keyPressed(const gleditor::Key key,
                              const gleditor::KeyMods mods) {
  std::uint64_t action{};
  {
    const std::scoped_lock lock(mutex_);
    if (!visible_) return false;
    ++revision_;
    using gleditor::Key;
    if (key == Key::Escape) {
      if (editing_) {
        editing_ = false;
        focus_   = 0;
        status_  = "Note edit cancelled";
      } else
        visible_ = false;
    } else if (key == Key::Up)
      move(-1);
    else if (key == Key::Down)
      move(1);
    else if (key == Key::Tab) {
      if (gleditor::held(mods, gleditor::KeyMods::Shift))
        focus_ = focus_ <= Previous || focus_ > Close ? Close : focus_ - 1;
      else
        focus_ = focus_ >= Close ? Previous : focus_ + 1;
    } else if (key == Key::Return) {
      action = focus_ >= Previous && focus_ <= Close ? focus_
               : editing_                            ? Save
                                                     : Restore;
    } else if (editing_ && key == Key::Backspace && !note_.empty()) {
      auto end = note_.size() - 1;
      while (end && (static_cast<unsigned char>(note_[end]) & 0xC0U) == 0x80U)
        --end;
      note_.erase(end);
    } else if (key == Key::Home && !editing_) {
      chosen_ = 0;
      focus_  = 0;
    } else if (key == Key::End && !editing_ && !visits_.empty()) {
      chosen_ = visits_.size() - 1;
      focus_  = 0;
    } else
      return true;
  }
  if (action) activate(action);
  return true;
}

void WalksOverlay::textTyped(const std::string &text) {
  std::uint64_t action{};
  {
    const std::scoped_lock lock(mutex_);
    if (!visible_) return;
    if (editing_) {
      note_ += text;
      ++revision_;
    } else if (text == "r" || text == "R")
      action = Reference;
    else if (text == "n" || text == "N")
      action = Edit;
  }
  if (action) activate(action);
}

bool WalksOverlay::pointerPressed(const int x, const int y) {
  std::uint64_t action{};
  {
    const std::scoped_lock lock(mutex_);
    if (!visible_) return false;
    for (const auto &[id, area] : areas_) {
      if (x >= area.x && x < area.x + area.width && y >= area.y &&
          y < area.y + area.height) {
        action = id;
        break;
      }
    }
  }
  if (action == kNote) {
    const std::scoped_lock lock(mutex_);
    focus_ = kNote;
    ++revision_;
    return true;
  }
  if (action) activate(action);
  return true;
}

std::optional<gleditor::InputArea> WalksOverlay::textArea() const {
  const std::scoped_lock lock(mutex_);
  const auto found = areas_.find(kNote);
  if (!visible_ || !editing_ || found == areas_.end()) return std::nullopt;
  return found->second;
}

void WalksOverlay::drawFrame(gleditor::FrameContext &frame) {
  const std::scoped_lock lock(mutex_);
  if (!visible_ || !canvas_) return;
  if (built_ != revision_ || contextRevision_ != context_.revision() ||
      width_ != frame.screenWidth || height_ != frame.screenHeight) {
    built_           = revision_;
    contextRevision_ = context_.revision();
    width_           = frame.screenWidth;
    height_          = frame.screenHeight;
    canvas_->clear();
    areas_.clear();
    labels_.clear();
    preview_.clear();
    const auto margin  = config_.marginPx;
    const auto padding = config_.paddingPx;
    const auto rowHeight =
        canvas_->measureText("Walks").height + config_.lineGapPx * 2;
    const auto left = margin, wide = static_cast<float>(width_) - 2 * margin;
    canvas_->addRect(left, margin, wide,
                     static_cast<float>(height_) - 2 * margin,
                     config_.backgroundColour);
    float top       = static_cast<float>(height_) - margin - padding;
    const auto text = [&](const std::string &value) {
      const auto budget = static_cast<std::size_t>(
          std::max(0.0F, wide - 2 * padding) /
          std::max(1.0F, canvas_->measureText("i").width));
      auto end = std::min(value.size(), budget);
      while (end < value.size() && end &&
             (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U)
        --end;
      auto clipped = value.substr(0, end);
      std::ranges::replace(clipped, '\n', ' ');
      while (!clipped.empty() &&
             canvas_->measureText(clipped).width > wide - 2 * padding) {
        auto end = clipped.size() - 1;
        while (end &&
               (static_cast<unsigned char>(clipped[end]) & 0xC0U) == 0x80U)
          --end;
        clipped.erase(end);
      }
      std::ignore =
          canvas_->addText(frame.state, left + padding, top, clipped,
                           config_.textColour, config_.backgroundColour);
      top -= rowHeight;
    };
    text("Walks — preview with arrows; Enter restores; R references; N edits a "
         "note");
    const auto visibleRows = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::max(
               0.0F, (static_cast<float>(height_) - 2 * margin - 2 * padding) /
                             rowHeight -
                         10)));
    if (chosen_ < first_) first_ = chosen_;
    if (chosen_ >= first_ + visibleRows) first_ = chosen_ - visibleRows + 1;
    for (auto i = first_; i < visits_.size() && i < first_ + visibleRows; ++i) {
      const auto &visit = visits_[i];
      auto label =
          "Visit " + std::to_string(visit.id.value) +
          (visit.parent ? " · parent " + std::to_string(visit.parent->value)
                        : " · root");
      if (context_.currentVisit() == visit.id) label += " · current";
      if (context_.visitReferenced(visit.id)) label += " · reference";
      const auto id = kRow + i;
      labels_[id]   = label;
      areas_[id]    = {static_cast<int>(left), static_cast<int>(height_ - top),
                       static_cast<int>(wide), static_cast<int>(rowHeight)};
      if (i == chosen_)
        canvas_->addRect(left, top - rowHeight, wide, rowHeight,
                         config_.buttonColour);
      text((i == chosen_ ? "> " : "  ") + label);
    }
    if (visits_.empty())
      text("No completed visits yet");
    else {
      const auto &visit = visits_[chosen_];
      preview_.push_back("Preview Visit " + std::to_string(visit.id.value) +
                         ": " + context_.describe(visit.target));
      preview_.push_back(std::visit(
          [](const auto &site) {
            return site.store.str() + " @ " + site.version.str();
          },
          visit.target));
      preview_.push_back(
          context_.visitAvailable(visit)
              ? "Target available"
              : "Target unavailable — notes and references remain available");
      if (visit.link)
        preview_.push_back(
            "Link " + std::to_string(visit.link->key.id) + " · " +
            (visit.link->active == xanadu::LinkSide::Left ? "Left" : "Right"));
      else
        preview_.push_back("Arrived without a link");
      for (const auto &line : preview_) text(line);
      if (!editing_) note_ = context_.visitNote(visit.id);
      if (editing_)
        areas_[kNote] = {static_cast<int>(left),
                         static_cast<int>(height_ - top),
                         static_cast<int>(wide), static_cast<int>(rowHeight)};
      text((editing_ ? "Editing note: " : "Note: ") + note_);
    }
    text(status_);
    float x = left + padding;
    for (std::uint64_t id = Previous; id <= Close; ++id) {
      const std::string label(kControls[id - 1]);
      const auto buttonWidth = canvas_->measureText(label).width + 2 * padding;
      if (x + buttonWidth > left + wide - padding) {
        x = left + padding;
        top -= rowHeight;
      }
      areas_[id]  = {static_cast<int>(x), static_cast<int>(height_ - top),
                     static_cast<int>(buttonWidth), static_cast<int>(rowHeight)};
      labels_[id] = label;
      canvas_->addRect(x, top - rowHeight, buttonWidth, rowHeight,
                       focus_ == id ? config_.chosenHighlightColour
                                    : config_.buttonColour);
      std::ignore = canvas_->addText(frame.state, x + padding, top, label,
                                     config_.textColour, config_.buttonColour);
      x += buttonWidth + config_.lineGapPx;
    }
    canvas_->commit();
  }
  const auto ortho = glm::ortho(0.0F, static_cast<float>(width_), 0.0F,
                                static_cast<float>(height_), -1.0F, 1.0F);
  canvas_->draw(frame.state, ortho);
}

void WalksOverlay::describe(gleditor::a11y::Builder &builder) {
  const std::scoped_lock lock(mutex_);
  if (!visible_) return;
  using namespace gleditor::a11y;
  std::vector<std::uint64_t> children;
  for (const auto &[id, label] : labels_) {
    auto &node   = builder.add(id, id >= kRow ? Role::ListItem : Role::Button);
    node.label   = label;
    node.actions = bit(Action::Click) | bit(Action::Focus);
    node.focusable = true;
    if (const auto found = areas_.find(id); found != areas_.end()) {
      const auto &area = found->second;
      node.bounds =
          Rect{static_cast<double>(area.x), static_cast<double>(area.y),
               static_cast<double>(area.x + area.width),
               static_cast<double>(area.y + area.height)};
    }
    if (id == kRow + chosen_) node.value = "previewed";
    children.push_back(builder.id(id));
  }
  for (std::size_t i = 0; i < preview_.size(); ++i) {
    auto &node = builder.add(30 + i, Role::Label);
    node.label = preview_[i];
    children.push_back(builder.id(30 + i));
  }
  auto &note     = builder.add(kNote, editing_ ? Role::TextInput : Role::Label);
  note.label     = editing_ ? "Edit visit note" : "Visit note";
  note.value     = note_;
  note.focusable = editing_;
  if (editing_) note.actions = bit(Action::Focus) | bit(Action::SetValue);
  children.push_back(builder.id(kNote));
  auto &status = builder.add(40, Role::Label);
  status.label = status_;
  children.push_back(builder.id(40));
  auto &dialog    = builder.add(0, Role::Dialog);
  dialog.label    = "Walks";
  dialog.modal    = true;
  dialog.children = std::move(children);
  builder.contribute(builder.id(0));
  builder.takeFocus(builder.id(focus_            ? focus_
                               : editing_        ? kNote
                               : visits_.empty() ? Close
                                                 : kRow + chosen_));
}

std::uint64_t WalksOverlay::accessibilityRevision() const {
  const std::scoped_lock lock(mutex_);
  return revision_ + contextRevision_;
}

bool WalksOverlay::performAction(const std::uint64_t nodeId,
                                 const gleditor::a11y::Action action,
                                 const std::string_view value) {
  const auto id = gleditor::a11y::Ids::localOf(nodeId);
  {
    const std::scoped_lock lock(mutex_);
    if (!visible_) return false;
    if (id == kNote && editing_) {
      if (action == gleditor::a11y::Action::SetValue) {
        note_ = value;
        ++revision_;
        return true;
      }
      if (action == gleditor::a11y::Action::Focus) {
        focus_ = kNote;
        ++revision_;
        return true;
      }
      return false;
    }
    if (!labels_.contains(id)) return false;
    if (action == gleditor::a11y::Action::Focus && id < kRow) {
      focus_ = id;
      ++revision_;
      return true;
    }
    if (action != gleditor::a11y::Action::Click &&
        action != gleditor::a11y::Action::Focus)
      return false;
  }
  activate(id);
  return true;
}
} // namespace xudu
