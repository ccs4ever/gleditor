#include <gleditor/ui/focus_manager.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace gleditor::ui {
namespace {
std::atomic<std::uint64_t> openingSequence{0};
std::uint64_t nextSequence() { return ++openingSequence; }
} // namespace
void FocusScope::activate() {
  opened_.store(nextSequence());
  active_.store(true);
}
void FocusScope::deactivate() { active_.store(false); }
bool FocusScope::active() const { return active_.load(); }
std::uint64_t FocusScope::openedSequence() const { return opened_.load(); }

struct FocusManager::State {
  struct Entry {
    std::uint64_t id{}, sequence{}, previous{};
    FocusScope *scope{};
    ScopePolicy policy;
    bool forced{}, active{};
  };
  mutable std::mutex mutex;
  std::vector<Entry> entries;
  std::uint64_t nextId{}, focused{};
  std::unordered_map<std::uint32_t, std::uint64_t> captures;
  std::vector<std::string> globalCommands{"quit"};
  KeyMods mods{KeyMods::None};

  Entry find(std::uint64_t id) const {
    const auto it = std::ranges::find(entries, id, &Entry::id);
    return it == entries.end() ? Entry{} : *it;
  }
  Entry refresh(FocusScope *priorFocus = nullptr) {
    std::vector<Entry> snapshot;
    {
      const std::lock_guard lock(mutex);
      snapshot = entries;
    }
    for (auto &entry : snapshot) {
      entry.active = entry.forced || entry.scope->active();
      if (!entry.forced) {
        entry.sequence = entry.scope->openedSequence();
      }
    }
    FocusScope *oldScope{}, *newScope{};
    Entry selected;
    std::vector<std::pair<FocusScope *, std::uint32_t>> cancelled;
    {
      const std::lock_guard lock(mutex);
      oldScope = priorFocus ? priorFocus : find(focused).scope;
      for (const auto &update : snapshot) {
        auto it = std::ranges::find(entries, update.id, &Entry::id);
        if (it == entries.end()) {
          continue;
        }
        if (update.active && !it->active) {
          it->previous = focused;
          it->sequence =
              update.sequence != 0 ? update.sequence : nextSequence();
        } else if (update.active && update.sequence != 0) {
          it->sequence = update.sequence;
        }
        it->active = update.active;
      }
      for (const auto &entry : entries) {
        if (entry.active && entry.policy.modal &&
            (selected.id == 0 || entry.sequence > selected.sequence)) {
          selected = entry;
        }
      }
      if (selected.id == 0) {
        selected = find(focused);
        if (!selected.active) {
          auto previous = selected.previous;
          selected      = {};
          for (std::size_t count = 0; previous != 0 && count < entries.size();
               ++count) {
            const auto candidate = find(previous);
            if (candidate.active) {
              selected = candidate;
              break;
            }
            previous = candidate.previous;
          }
        }
        if (selected.id == 0) {
          for (const auto &entry : entries) {
            if (entry.active && !entry.policy.modal &&
                (selected.id == 0 || entry.sequence < selected.sequence)) {
              selected = entry;
            }
          }
        }
      }
      focused  = selected.id;
      newScope = selected.scope;
      std::erase_if(captures, [&](const auto &capture) {
        const auto captured = find(capture.second);
        const bool lost =
            !captured.active || (selected.policy.modal && selected.id != 0 &&
                                 captured.id != selected.id);
        if (lost && captured.scope) {
          cancelled.emplace_back(captured.scope, capture.first);
        }
        return lost;
      });
    }
    for (const auto &[scope, pointer] : cancelled) {
      scope->pointerEvent(
          PointerEvent{.phase = PointerPhase::Cancel, .pointerId = pointer});
    }
    if (oldScope != newScope) {
      if (oldScope) {
        oldScope->focusChanged(false);
      }
      if (newScope) {
        newScope->focusChanged(true);
      }
    }
    if (oldScope != newScope || !cancelled.empty()) {
      return refresh();
    }
    return selected;
  }
  void erase(std::uint64_t id) {
    FocusScope *removed{};
    {
      const std::lock_guard lock(mutex);
      const auto entry = find(id);
      if (focused == id) {
        removed = entry.scope;
        focused = entry.previous;
      }
      for (auto &other : entries) {
        if (other.previous == id) {
          other.previous = entry.previous;
        }
      }
      std::erase_if(entries,
                    [id](const auto &entry) { return entry.id == id; });
      std::erase_if(captures,
                    [id](const auto &capture) { return capture.second == id; });
    }
    refresh(removed);
  }
};
FocusManager::ScopeHandle::ScopeHandle(std::weak_ptr<State> state,
                                       std::uint64_t id)
    : state_(std::move(state)), id_(id) {}
FocusManager::ScopeHandle::~ScopeHandle() { reset(); }
FocusManager::ScopeHandle::ScopeHandle(ScopeHandle &&other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}
FocusManager::ScopeHandle &
FocusManager::ScopeHandle::operator=(ScopeHandle &&other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    id_    = std::exchange(other.id_, 0);
  }
  return *this;
}
void FocusManager::ScopeHandle::reset() {
  if (auto state = state_.lock(); state && id_ != 0) {
    state->erase(std::exchange(id_, 0));
  }
}
FocusManager::FocusManager() : state_(std::make_shared<State>()) {}
FocusManager::~FocusManager() = default;
FocusManager::ScopeHandle
FocusManager::insert(FocusScope &scope, ScopePolicy policy, bool forced) {
  state_->refresh();
  std::uint64_t id;
  {
    const std::lock_guard lock(state_->mutex);
    id = ++state_->nextId;
    state_->entries.push_back({id, forced ? nextSequence() : 0, state_->focused,
                               &scope, std::move(policy), forced, false});
  }
  state_->refresh();
  return {state_, id};
}
FocusManager::ScopeHandle FocusManager::registerScope(FocusScope &scope,
                                                      ScopePolicy policy) {
  return insert(scope, std::move(policy), false);
}
FocusManager::ScopeHandle FocusManager::push(FocusScope &scope,
                                             ScopePolicy policy) {
  return insert(scope, std::move(policy), true);
}
FocusManager::ScopeHandle FocusManager::addPane(FocusScope &scope) {
  ScopePolicy policy;
  policy.modal = false;
  return insert(scope, std::move(policy), true);
}
FocusScope *FocusManager::focusedScope() { return state_->refresh().scope; }
bool FocusManager::modalActive() {
  const auto entry = state_->refresh();
  return entry.id != 0 && entry.policy.modal;
}
std::optional<InputArea> FocusManager::textArea() {
  const auto scope = focusedScope();
  return scope ? scope->textArea() : std::nullopt;
}
bool FocusManager::dispatchKey(const KeyEvent &event) {
  const auto entry = state_->refresh();
  {
    const std::lock_guard lock(state_->mutex);
    state_->mods = event.mods;
  }
  if ((!entry.scope || !entry.policy.modal) && event.key == Key::F6) {
    return cyclePane(held(event.mods, KeyMods::Shift));
  }
  if (!entry.scope) {
    return false;
  }
  const bool handled = entry.scope->keyPressed(event);
  if (!handled && event.key == Key::Escape && entry.policy.modal) {
    entry.scope->cancel();
    {
      const std::lock_guard lock(state_->mutex);
      const auto it =
          std::ranges::find(state_->entries, entry.id, &State::Entry::id);
      if (it != state_->entries.end()) {
        it->forced = false;
      }
    }
    state_->refresh();
  }
  return handled || entry.policy.modal;
}
bool FocusManager::dispatchText(std::string_view text) {
  const auto entry = state_->refresh();
  if (!entry.scope) {
    return false;
  }
  entry.scope->textTyped(text);
  return true;
}
bool FocusManager::dispatchPointer(const PointerEvent &event) {
  auto entry = state_->refresh();
  {
    const std::lock_guard lock(state_->mutex);
    if (const auto capture = state_->captures.find(event.pointerId);
        capture != state_->captures.end()) {
      entry = state_->find(capture->second);
    }
  }
  if (!entry.scope) {
    return false;
  }
  if (event.phase == PointerPhase::Press) {
    const auto area    = entry.scope->pointerArea();
    const bool outside = area && (event.x < area->x || event.y < area->y ||
                                  event.x >= area->x + area->width ||
                                  event.y >= area->y + area->height);
    if (outside && entry.policy.modal) {
      if (entry.policy.outside == OutsidePointer::Block) {
        return true;
      }
      entry.scope->cancel();
      {
        const std::lock_guard lock(state_->mutex);
        const auto it =
            std::ranges::find(state_->entries, entry.id, &State::Entry::id);
        if (it != state_->entries.end()) {
          it->forced = false;
        }
      }
      if (entry.policy.outside == OutsidePointer::Dismiss) {
        return true;
      }
      const auto next = state_->refresh();
      if (next.id == entry.id) {
        return true;
      }
      return dispatchPointer(event);
    }
  }
  const bool handled = entry.scope->pointerEvent(event);
  {
    const std::lock_guard lock(state_->mutex);
    if (event.phase == PointerPhase::Press && handled &&
        state_->find(entry.id).id != 0) {
      state_->captures[event.pointerId] = entry.id;
    } else if (event.phase == PointerPhase::Release ||
               event.phase == PointerPhase::Cancel) {
      state_->captures.erase(event.pointerId);
    }
  }
  return handled || entry.policy.modal;
}
bool FocusManager::dispatch(const InputEvent &event) {
  return std::visit(
      [this](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, KeyEvent>) {
          return dispatchKey(value);
        } else if constexpr (std::is_same_v<T, PointerEvent>) {
          return dispatchPointer(value);
        } else if constexpr (std::is_same_v<T, TextEvent>) {
          return dispatchText(value.text);
        } else {
          focusLost();
          return true;
        }
      },
      event);
}
bool FocusManager::permitsCommand(std::string_view command) {
  const auto entry = state_->refresh();
  if (!entry.scope || !entry.policy.modal) {
    return true;
  }
  bool global;
  {
    const std::lock_guard lock(state_->mutex);
    global = std::ranges::find(state_->globalCommands, command) !=
             state_->globalCommands.end();
  }
  return global && (std::ranges::find(entry.policy.allowedCommands, command) !=
                        entry.policy.allowedCommands.end() ||
                    entry.scope->acceptsCommand(command));
}
void FocusManager::setGlobalCommandAllowList(
    std::vector<std::string> commands) {
  const std::lock_guard lock(state_->mutex);
  state_->globalCommands = std::move(commands);
}
void FocusManager::focusLost() {
  std::vector<std::pair<FocusScope *, std::uint32_t>> scopes;
  {
    const std::lock_guard lock(state_->mutex);
    for (const auto &[pointer, id] : state_->captures) {
      const auto scope = state_->find(id).scope;
      if (scope) {
        scopes.emplace_back(scope, pointer);
      }
    }
    state_->captures.clear();
    state_->mods = KeyMods::None;
  }
  for (const auto &[scope, pointer] : scopes) {
    scope->pointerEvent(
        PointerEvent{.phase = PointerPhase::Cancel, .pointerId = pointer});
  }
}
bool FocusManager::cyclePane(bool reverse) {
  const auto current = state_->refresh();
  if (current.id != 0 && current.policy.modal) {
    return false;
  }
  FocusScope *next{};
  {
    const std::lock_guard lock(state_->mutex);
    std::vector<State::Entry> panes;
    for (const auto &entry : state_->entries) {
      if (entry.active && !entry.policy.modal) {
        panes.push_back(entry);
      }
    }
    if (panes.empty()) {
      return false;
    }
    auto it = std::ranges::find(panes, current.id, &State::Entry::id);
    const auto index =
        it == panes.end()
            ? 0U
            : static_cast<std::size_t>(std::distance(panes.begin(), it));
    const auto target = reverse ? (index + panes.size() - 1) % panes.size()
                                : (index + 1) % panes.size();
    state_->focused   = panes[target].id;
    next              = panes[target].scope;
  }
  if (current.scope != next) {
    if (current.scope) {
      current.scope->focusChanged(false);
    }
    next->focusChanged(true);
  }
  return true;
}
KeyMods FocusManager::modifiers() const {
  const std::lock_guard lock(state_->mutex);
  return state_->mods;
}
} // namespace gleditor::ui
