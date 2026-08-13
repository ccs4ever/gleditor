/**
 * @file app.hpp
 * @brief The parts of being a program that are not specific to any one
 *        program.
 *
 * Starting SDL, choosing a backend, opening a window with the attributes that
 * backend needs, running a render thread, pumping events, turning a mouse
 * button into a picking request and a keystroke into an action, and shutting
 * all of it down in the right order: none of that is about editing text, and
 * all of it was in the editor's main().
 *
 * A second program on this library would have had to copy it, and a copy of an
 * event loop is the kind of thing that stays almost identical for a while and
 * then quietly stops being identical in one place. So it lives here, and what
 * a program supplies is the part that differs -- which options it takes, which
 * keys do what, and what it puts on screen.
 */
#ifndef GLEDITOR_APP_H
#define GLEDITOR_APP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/render/types.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>

// Only ever taken by reference, so the parser's own header -- which is a
// vendored dependency and has no business in an installed one -- is not needed
// to declare this.
namespace argparse {
class ArgumentParser;
}

namespace gleditor {

/**
 * @brief Modifier keys as a mask, in the library's own spelling.
 *
 * SDL's constants differ between its major versions and a binding written
 * against one of them would not compile against the other. These do not.
 */
enum class Mod : std::uint16_t {
  None  = 0,
  Shift = 1U << 0U,
  Ctrl  = 1U << 1U,
  Alt   = 1U << 2U,
};

inline Mod operator|(const Mod lhs, const Mod rhs) {
  return static_cast<Mod>(static_cast<std::uint16_t>(lhs) |
                          static_cast<std::uint16_t>(rhs));
}

/**
 * @brief A key, its modifiers, and what pressing it does.
 *
 * The key is named by its physical position -- an SDL scancode -- rather than
 * by the character it produces, so a binding lands on the same key whatever
 * layout is in use.
 */
struct Command {
  int scancode{};
  Mod mods{Mod::None};
  /// Short name, for a help listing.
  std::string name;
  /// One line saying what it does.
  std::string help;
  std::function<void()> run;
};

/**
 * @class CommandTable
 * @brief What each key does, and what to say when asked.
 *
 * A binding runs on the thread pumping events, which is not the render thread.
 * Anything touching the render state belongs inside AbstractRenderer::run().
 */
class CommandTable {
public:
  void bind(int scancode, Mod mods, std::string name, std::string help,
            std::function<void()> run);
  /// Convenience for a binding with no modifiers.
  void bind(int scancode, std::string name, std::string help,
            std::function<void()> run) {
    bind(scancode, Mod::None, std::move(name), std::move(help),
         std::move(run));
  }

  /**
   * @brief Run the command bound to @p scancode with @p mods.
   *
   * Bindings are tried in the order they were added, and the first whose key
   * and modifiers both match wins. Modifiers must match exactly, so a binding
   * on a bare key does not fire when it is pressed with control held -- which
   * is what lets the two be bound to different things.
   *
   * @return Whether anything was bound.
   */
  bool dispatch(int scancode, Mod mods) const;

  [[nodiscard]] const std::vector<Command> &all() const { return bindings; }
  /// The bindings as lines of text, for a program that wants to print them.
  [[nodiscard]] std::string helpText() const;

private:
  std::vector<Command> bindings;
};

/**
 * @brief Register the options every program built on this library accepts.
 *
 * The backend, the font, the field of view, and the whole set for driving a
 * run without a person at the keyboard -- screenshots, picking queries, timing.
 * A program adds its own on top and is not obliged to explain these.
 *
 * @param detailed Describe them at length and list the automation ones, rather
 *        than hiding those behind a shorter listing. See what the editor does
 *        with `--help` and `--help-all`.
 */
void addCommonArguments(argparse::ArgumentParser &parser, bool detailed);

/**
 * @brief Copy the parsed common options into @p state.
 * @return The backend that was asked for.
 * @throws std::runtime_error if the backend named is not in this build, or an
 *         option's value cannot be parsed.
 */
/**
 * @brief The automation options, in the order they were written.
 *
 * Exposed rather than kept private because the order is the whole of what it
 * produces, and reading a command line by hand -- both "--click 3,4" and
 * "--click=3,4", stopping at the ones that are not automation -- has enough
 * edges to be worth testing directly.
 *
 * @param argc,argv A command line the parser has already accepted.
 */
std::vector<AppState::AutomationStep>
readAutomationScript(int argc, const char *const *argv);

/**
 * @brief Apply the parsed common options to @p state.
 *
 * @param argc,argv The command line as it was given. Needed for the
 *        automation options alone, and for one thing about them the parser
 *        does not keep: the order they were written in, which is the order
 *        they are carried out in.
 */
render::Backend applyCommonArguments(argparse::ArgumentParser &parser,
                                     const AppStateRef &state, int argc,
                                     const char *const *argv);

/**
 * @class Application
 * @brief Window, render thread and event loop.
 *
 * Construct it, bind whatever keys the program wants, then call run(). It
 * returns once the user has asked to quit or the render thread has stopped,
 * and its value is what main() should return.
 */
class Application {
public:
  Application(AppStateRef aState, RendererRef aRenderer, render::Backend aBackend,
              std::string aTitle);
  ~Application();

  Application(const Application &)            = delete;
  Application &operator=(const Application &) = delete;
  Application(Application &&)                 = delete;
  Application &operator=(Application &&)      = delete;

  [[nodiscard]] CommandTable &commands() { return commandTable; }

  /**
   * @brief Whether typed characters are collected for the render thread to
   *        insert at the caret.
   *
   * On by default. A program that wants keystrokes to mean commands rather
   * than text turns it off, and then a bare letter reaches the command table
   * instead of being typed into a document.
   */
  void setTextInputEnabled(bool enabled) { textInput = enabled; }

  /**
   * @brief Set up SDL, open the window, start rendering and pump events until
   *        asked to stop.
   * @return 0, or 1 if the render thread failed.
   */
  int run();

  /// Bind the camera controls the library's own view model implies -- moving,
  /// zooming and resetting the view. A program is free not to.
  void bindDefaultViewCommands();

private:
  AppStateRef state;
  RendererRef renderer;
  render::Backend backend;
  std::string title;
  CommandTable commandTable;
  bool textInput{true};
};

/**
 * @brief Set the process locale from the environment, falling back to the
 *        classic one.
 *
 * Called by Application::run(), and separately available because a program
 * that parses its command line before constructing one wants it done first.
 * MinGW's standard library has no named locales and throws whenever the
 * environment names one, which used to kill the program before it had parsed
 * an argument, with a message that named neither the program nor the cause.
 */
void initLocale();

} // namespace gleditor

#endif // GLEDITOR_APP_H
// vi: set sw=2 sts=2 ts=2 et:
