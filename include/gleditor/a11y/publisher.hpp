/**
 * @file publisher.hpp
 * @brief Collecting what everything on screen has to say, and saying it.
 *
 * The tree in tree.hpp is built by the elements that draw -- documents, the
 * notification overlay, the modal form, whatever a program adds. This is what
 * asks them, on the thread where their state lives, and hands the answer to
 * the platform on the thread that owns the window.
 *
 * The two-thread split is not incidental. The documents and the caret are the
 * render thread's, and reading them from anywhere else is what the whole of
 * this program's threading exists to prevent. AccessKit's Windows adapter, on
 * the other hand, must be talked to from the thread that created the window.
 * So the tree is built on one side, swapped across under a lock, and published
 * from the other -- the same shape as every other hand-off here.
 *
 * Nothing above this file knows whether there is an AccessKit in the build.
 * When there is not, the tree is still assembled and simply goes nowhere. That
 * is on purpose: a description that is only built in some builds is one that is
 * only correct in some builds.
 */
#ifndef GLEDITOR_A11Y_PUBLISHER_H
#define GLEDITOR_A11Y_PUBLISHER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/a11y/platform.hpp>
#include <gleditor/a11y/tree.hpp>

namespace gleditor::a11y {

/**
 * @class Publisher
 * @brief The one thing that talks to AccessKit.
 *
 * Constructed early -- before the window, before the render thread -- so that
 * anything on screen can register itself as it is built. start() is what
 * actually reaches the platform, and everything before it is harmless.
 */
class Publisher {
public:
  /**
   * @param aName What this program is called, as an assistive technology will
   *        announce it.
   * @param aToolkit,aVersion What drew the window. Reported as the toolkit,
   *        because a bug report about a misdescribed widget has to be able to
   *        name what misdescribed it.
   */
  Publisher(std::string aName, std::string aToolkit, std::string aVersion);
  ~Publisher();

  Publisher(const Publisher &)            = delete;
  Publisher &operator=(const Publisher &) = delete;
  Publisher(Publisher &&)                 = delete;
  Publisher &operator=(Publisher &&)      = delete;

  /**
   * @brief Register something that describes itself.
   *
   * Before start(), and in the order the elements should be described: the
   * order is the order an assistive technology moves through them, so it
   * should be the order they are drawn in -- documents first, chrome over
   * them, a modal last.
   *
   * Sources are held as bare pointers and not owned. One must outlive the
   * publisher.
   *
   * @param owner Which of the reserved numbers this is, for the library's own
   *        sources. Left alone by a program, which gets the next free one --
   *        so a program's elements are described in the order it registers
   *        them, after everything the library describes for itself.
   */
  void addSource(Source *source, std::optional<std::uint16_t> owner = {});

  /**
   * @brief Open the platform's connection.
   *
   * @param nativeWindow On Windows the HWND, which the adapter subclasses
   *        before the window is first shown. Null elsewhere: AT-SPI is a bus
   *        rather than a property of a window, so one adapter serves X11 and
   *        Wayland alike.
   * @return Whether there is an adapter. False in a build without AccessKit,
   *         and on a system where nothing accessible is running. Neither is an
   *         error and neither changes what the rest of the program does.
   */
  bool start(void *nativeWindow);

  /// Whether there is a platform to report to. For a program that wants to
  /// say so on its start-up banner.
  [[nodiscard]] bool connected() const { return nullptr != platform; }

  /// What the window is called. Shown as the root node's name.
  void setWindowTitle(std::string title);

  /// What drew the window, and which version of it. Reported as the toolkit,
  /// because a bug report about a misdescribed widget has to be able to name
  /// what misdescribed it. Before start(); afterwards it is what the platform
  /// was already told.
  void setToolkit(std::string aToolkit, std::string aVersion);

  /**
   * @brief Ask every source what it has to say, if any of them has changed.
   *
   * Called once a frame on the render thread. Cheap when nothing moved: the
   * sources' revisions are summed and compared, and nothing else happens.
   *
   * @param width,height The drawing area in pixels, which is the root node's
   *        extent and the space every other node's bounds are in.
   */
  void rebuild(int width, int height);

  /**
   * @brief Send whatever was last built, if it differs from what was last
   *        sent.
   *
   * Called from the event loop. Comparing whole trees rather than trusting the
   * revisions: a revision that moves without the description changing costs a
   * comparison here, where an update that says nothing new costs a screen
   * reader repeating itself.
   */
  void publish();

  /**
   * @brief Carry out whatever the assistive technology has asked for.
   *
   * Called from the event loop. A request names a node; the node's id says
   * which source made it, and that source is asked to do it.
   *
   * @return How many were carried out, which is what a test looks at.
   */
  std::size_t pumpActions();

  /// The window gained or lost the keyboard.
  void setWindowFocused(bool focused);

  /**
   * @brief Where the window is on screen.
   *
   * @param outer The whole window including whatever frame the desktop drew
   *        around it; @p inner the part this program draws in. Both in
   *        physical pixels from the top left of the primary display.
   *
   * Under Wayland a client is not told where it is. Passing zeroes is right
   * there, and means bounds are reported relative to the window -- which is
   * what a Wayland assistive technology expects anyway.
   */
  void setWindowBounds(const Rect &outer, const Rect &inner);

  /// The tree as last built. For tests, and for a program that wants to print
  /// what it would report.
  [[nodiscard]] Tree snapshot() const;

  /// The tree written out as indented text, one node per line. What
  /// `--dump-a11y` prints, and the readable half of the tests.
  [[nodiscard]] static std::string describe(const Tree &tree);

private:
  /// A registered source and the number its node ids are made from.
  struct Registered {
    Source *source{};
    std::uint16_t owner{};
  };

  std::string name;
  std::string toolkit;
  std::string version;

  /// Registered on whichever thread built the thing being described, and read
  /// on both -- the render thread describes, the event loop dispatches what
  /// comes back -- so this is guarded even though registration happens once
  /// and early.
  mutable std::mutex sourcesGuard;
  std::vector<Registered> sources;
  std::uint16_t nextOwner{Ids::firstProgramOwner};

  std::unique_ptr<Platform> platform;

  /// What rebuild() last produced and publish() last sent. Both guarded, since
  /// they are written on one thread and read on the other.
  mutable std::mutex guard;
  Tree built;
  Tree sent;
  std::string windowTitle;
  /// The summed revisions the last rebuild was made from, so a frame where
  /// nothing changed costs one addition per source.
  std::uint64_t builtFrom{};
  bool everBuilt{};
};

} // namespace gleditor::a11y

#endif // GLEDITOR_A11Y_PUBLISHER_H
