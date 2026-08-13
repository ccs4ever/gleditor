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

#include <cstdint>
#include <optional>
#include <string>

namespace gleditor {

/**
 * @brief Keys a modal reacts to that are not text.
 *
 * A small list on purpose: this is what a form needs to be usable, not a
 * general keyboard abstraction. Everything else arrives as text or not at all,
 * which is what keeps the platform's key codes out of application code.
 */
enum class Key : std::uint8_t {
  Escape,
  Return,
  Tab,
  Backspace,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
};

/// Modifier keys held with it. Matches gleditor::Mod, and is repeated here so
/// that a modal does not have to include the application header.
enum class KeyMods : std::uint16_t {
  None  = 0,
  Shift = 1U << 0U,
  Ctrl  = 1U << 1U,
  Alt   = 1U << 2U,
};

[[nodiscard]] inline bool held(const KeyMods mods, const KeyMods which) {
  return 0 !=
         (static_cast<std::uint16_t>(mods) & static_cast<std::uint16_t>(which));
}

/// A rectangle in window pixels, top left origin: SDL's convention.
struct InputArea {
  int x{};
  int y{};
  int width{};
  int height{};

  bool operator==(const InputArea &) const = default;
};

/**
 * @brief Something that takes the keyboard while it is up.
 *
 * Consulted on the event thread. An implementation that also draws will be
 * read from the render thread at the same time, and is responsible for its own
 * locking -- see Form, which holds one mutex over the whole of its state.
 */
class ModalInput {
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

  /// A key that is not text. Ignored keys should return false, so that a modal
  /// which does not use a key does not silently swallow it.
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
  [[nodiscard]] virtual std::optional<InputArea> textArea() const {
    return std::nullopt;
  }
};

} // namespace gleditor

#endif // GLEDITOR_MODAL_INPUT_H
// vi: set sw=2 sts=2 ts=2 et:
