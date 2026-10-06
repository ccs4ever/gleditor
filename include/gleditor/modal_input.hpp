/**
 * @file modal_input.hpp
 * @brief Taking the keyboard away from the document for a moment.
 *
 * Every keystroke in this editor is text: that is the whole premise, and it is
 * why the commands are all on control. It leaves nowhere for a program to ask
 * a question -- anything it drew to ask with would be a picture, and the answer
 * the person typed would land in the document behind it.
 *
 * A modal is the exception, stated rather than assumed. While one is up it is
 * offered every key and every piece of composed text before either reaches the
 * document or the command table, and mouse clicks are held: the thing on screen
 * is a question, and until it is answered the document is not what is being
 * edited.
 *
 * Nothing here draws. What a modal looks like is a FrameContributor's business
 * -- see Form for one that draws itself as a panel of labelled fields.
 */
#ifndef GLEDITOR_MODAL_INPUT_H
#define GLEDITOR_MODAL_INPUT_H

#include <gleditor/render/types.hpp>
#include <gleditor/ui/focus_manager.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

struct RenderState;

namespace gleditor {

/**
 * @brief Something that takes the keyboard while it is up.
 *
 * Consulted on the event thread. An implementation that also draws will be
 * read from the render thread at the same time, and is responsible for its own
 * locking -- see Form, which holds one mutex over the whole of its state.
 */
class ModalInput : public ui::FocusScope {
public:
  ModalInput()          = default;
  virtual ~ModalInput() = default;

  ModalInput(const ModalInput &)            = delete;
  ModalInput &operator=(const ModalInput &) = delete;
  ModalInput(ModalInput &&)                 = delete;
  ModalInput &operator=(ModalInput &&)      = delete;

  /**
   * @brief Whether this is up and taking input.
   *
   * Asked before every key, so it must be cheap and must be safe to ask from
   * the event thread at any time.
   */
  [[nodiscard]] virtual bool grabbing() const = 0;

  [[nodiscard]] bool active() const override { return grabbing(); }
  [[nodiscard]] bool acceptsCommand(std::string_view command) const override {
    return command == "quit" || command == "std:xudu/quit";
  }
  bool keyPressed(const ui::KeyEvent &event) override {
    return keyPressed(event.key, event.mods);
  }
  void textTyped(std::string_view utf8) override {
    textTyped(std::string(utf8));
  }
  void cancel() override { keyPressed(Key::Escape, KeyMods::None); }
  virtual bool pointerPick(const render::PickingResult &, RenderState &) {
    return false;
  }
  void releaseFocus() {
    ui::FocusManager::ScopeHandle old;
    {
      const std::scoped_lock lock(registrationGuard_);
      old                = std::move(registration_);
      registeredManager_ = nullptr;
      ++registrationGeneration_;
    }
    old.reset();
  }
  virtual void syncFocus(ui::FocusManager &manager) {
    ui::FocusManager::ScopeHandle old;
    std::uint64_t generation;
    {
      const std::scoped_lock lock(registrationGuard_);
      if (registeredManager_ == &manager) return;
      old                = std::move(registration_);
      registeredManager_ = &manager;
      generation         = ++registrationGeneration_;
    }
    // Registration may notify scopes and re-enter this adapter.
    old.reset();
    auto next = manager.registerScope(*this);
    {
      const std::scoped_lock lock(registrationGuard_);
      if (registrationGeneration_ == generation) {
        registration_ = std::move(next);
      }
    }
  }

  /// A key that is not text. False means this scope has no action for the key;
  /// a modal still blocks delivery to the document behind it.
  virtual bool keyPressed(Key key, KeyMods mods) = 0;

  /// Composed text -- what an input method produced, not a scancode.
  virtual void textTyped(const std::string &utf8) = 0;

  /**
   * @brief Where on screen the text being typed will appear.
   *
   * Handed to SDL, which hands it to the platform: it is what an input method
   * needs in order to put its candidate window somewhere sensible, and what an
   * on-screen keyboard needs in order not to cover the field being typed into.
   * Window pixels from the top left, which is SDL's convention rather than the
   * bottom-up one the glyph pipeline draws in.
   *
   * Nothing when the focus is not on anything that takes text -- a button, a
   * closed list -- so that a keyboard is not raised for a field nobody can
   * type into.
   */
  [[nodiscard]] std::optional<InputArea> textArea() const override {
    return std::nullopt;
  }

private:
  std::mutex registrationGuard_;
  std::uint64_t registrationGeneration_{};
  ui::FocusManager *registeredManager_{};
  ui::FocusManager::ScopeHandle registration_;
};

/**
 * @brief Composite modal input dispatcher that checks registered modals in
 * activation order.
 *
 * Allows multiple distinct modals (e.g. publication form and presentation
 * overlay) to coexist within the application state without conflict. The
 * most recently opened modal that returns grabbing() == true receives keyboard
 * and text events.
 */
class CompositeModalInput : public ModalInput {
public:
  CompositeModalInput() = default;
  explicit CompositeModalInput(std::vector<ModalInput *> modals)
      : modals_(std::move(modals)) {}

  void add(ModalInput *modal) {
    if (modal != nullptr) {
      modals_.push_back(modal);
    }
  }

  void remove(ModalInput *modal) {
    if (modal) modal->releaseFocus();
    std::erase(modals_, modal);
  }

  void syncFocus(ui::FocusManager &manager) override {
    for (auto *modal : modals_) {
      if (modal) modal->syncFocus(manager);
    }
  }
  [[nodiscard]] bool grabbing() const override { return top() != nullptr; }
  bool keyPressed(Key key, KeyMods mods) override {
    auto *modal = top();
    return modal && modal->keyPressed(key, mods);
  }
  void textTyped(const std::string &utf8) override {
    if (auto *modal = top()) modal->textTyped(utf8);
  }
  [[nodiscard]] std::optional<InputArea> textArea() const override {
    auto *modal = top();
    return modal ? modal->textArea() : std::nullopt;
  }

private:
  [[nodiscard]] ModalInput *top() const {
    ModalInput *result = nullptr;
    for (auto *modal : modals_) {
      if (modal && modal->grabbing() &&
          (!result || modal->openedSequence() >= result->openedSequence()))
        result = modal;
    }
    return result;
  }
  std::vector<ModalInput *> modals_;
};

} // namespace gleditor

#endif // GLEDITOR_MODAL_INPUT_H
