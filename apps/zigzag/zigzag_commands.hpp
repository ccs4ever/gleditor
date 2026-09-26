/**
 * @file zigzag_commands.hpp
 * @brief ZigZag's keyboard commands, for every program that shows a slice.
 */
#ifndef ZIGZAG_ZIGZAG_COMMANDS_HPP
#define ZIGZAG_ZIGZAG_COMMANDS_HPP

#include <functional>
#include <memory>

namespace gleditor {
class CommandTable;
}

namespace zigzag {

class ZigzagVisualizer;

/// What a host changes about ZigZag's commands.
struct ZigzagCommandHooks {
  /// What Return does when neither the omnibar nor the palette is open:
  /// nothing on its own, following the focused cell's content in xuzz.
  std::function<void()> activateFocus;
  /// What Escape does when nothing is open to dismiss: in xuzz, hand the
  /// keyboard back to the document.
  std::function<void()> leave;
  /// Called after any command that moves the focus, so a host can bring the
  /// new focus into view.
  std::function<void()> focusMoved;
};

/**
 * @brief Register ZigZag's navigation, editing and view commands on
 *        @p table, each under its bare-key name and its Alt-prefixed
 *        zigzag_* twin where the keymap has one.
 *
 * Shared so zigzag and xuzz register one set rather than xuzz keeping a
 * partial copy that drifted: the UX audit found xuzz with the palette and
 * bundles and no way to move the focus. Which of the two names reaches the
 * keyboard where is the keymap's business (xanadu::keymapScope()); saving and
 * exporting a slice stay with the program that owns the store.
 */
void registerZigzagCommands(gleditor::CommandTable &table,
                            const std::shared_ptr<ZigzagVisualizer> &viz,
                            ZigzagCommandHooks hooks = {});

} // namespace zigzag

#endif // ZIGZAG_ZIGZAG_COMMANDS_HPP
