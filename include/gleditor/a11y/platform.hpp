/**
 * @file platform.hpp
 * @brief Where a described user interface goes.
 *
 * The tree in tree.hpp is this program's own description of what is on screen.
 * This is the one thing that hands it to the operating system -- through
 * AccessKit, which speaks UI Automation on Windows and AT-SPI on X11 and
 * Wayland -- and the one thing that hears back.
 *
 * It is an interface with two implementations, chosen at build time:
 *
 *   src/a11y/platform_accesskit.cpp  against AccessKit's C bindings
 *   src/a11y/platform_none.cpp       doing nothing
 *
 * That is the whole of the conditional compilation. Nothing above this file
 * knows whether AccessKit is in the build: the tree is assembled every frame
 * either way, out of the same state, by the same code, and only its
 * destination differs. A description that is only built in some builds is one
 * that is only correct in some builds.
 */
#ifndef GLEDITOR_A11Y_PLATFORM_H
#define GLEDITOR_A11Y_PLATFORM_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <gleditor/a11y/tree.hpp>

namespace gleditor::a11y {

/// Something an assistive technology has asked for, on its way back in.
struct ActionRequest {
  std::uint64_t node{};
  Action action{Action::Focus};
  /// The new text, for Action::SetValue. Empty otherwise.
  std::string value;
};

/**
 * @brief The platform's end of the conversation.
 *
 * Every method is called from the thread that owns the window, because that is
 * what the Windows adapter requires and what the others are content with. The
 * one exception is that AccessKit itself calls back from its own threads --
 * an implementation deals with that internally and never passes it on; see
 * platform_accesskit.cpp for how, and why the answer is a queue.
 */
class Platform {
public:
  Platform()          = default;
  virtual ~Platform() = default;

  Platform(const Platform &)            = delete;
  Platform &operator=(const Platform &) = delete;
  Platform(Platform &&)                 = delete;
  Platform &operator=(Platform &&)      = delete;

  /// Hand over what is on screen now.
  virtual void update(const Tree &tree) = 0;

  /// The window gained or lost the keyboard.
  virtual void setWindowFocused(bool focused) = 0;

  /**
   * @brief Where the window is on screen.
   *
   * @param outer The whole window including whatever frame the desktop drew
   *        around it, @p inner the part this program draws in. Both in
   *        physical pixels from the top left of the primary display, which is
   *        the space a node's own bounds are offset into.
   */
  virtual void setWindowBounds(const Rect &outer, const Rect &inner) = 0;

  /// The next thing asked for, or nothing. Drained by the event loop.
  virtual std::optional<ActionRequest> nextAction() = 0;
};

/**
 * @brief Open the platform's connection, if there is one to open.
 *
 * @param nativeWindow On Windows the HWND, which the adapter subclasses to
 *        answer WM_GETOBJECT; the window must not have been shown yet. Ignored
 *        elsewhere -- AT-SPI is a bus rather than a property of a window, so
 *        one adapter serves X11 and Wayland alike.
 * @param name What this program is called, as an assistive technology will
 *        announce it.
 * @param toolkit,version What drew the window, reported as the toolkit: a bug
 *        report about a misdescribed widget has to be able to name what
 *        misdescribed it.
 *
 * @return Null in a build without AccessKit, and on a platform it has no
 *         adapter for. Neither is an error and neither changes what the rest
 *         of the program does.
 */
[[nodiscard]] std::unique_ptr<Platform>
openPlatform(void *nativeWindow, const std::string &name,
             const std::string &toolkit, const std::string &version);

/// Whether this build has AccessKit in it. For a program that wants to say so
/// on its start-up banner, and for the tests.
[[nodiscard]] bool platformAvailable();

} // namespace gleditor::a11y

#endif // GLEDITOR_A11Y_PLATFORM_H
// vi: set sw=2 sts=2 ts=2 et:
