/**
 * @file sdl_compat.hpp
 * @brief The only place in the program that knows which SDL major version it
 *        was built against.
 *
 * SDL3 is preferred and is what the rest of the code is written against: its
 * spelling of every call, constant and event this program uses is the one that
 * appears everywhere else. SDL2 is supported because it is what most
 * distributions still ship, and it is reached through the shims below rather
 * than through conditional compilation scattered across the source.
 *
 * The differences that actually matter here are small and of four kinds:
 *
 *  - Renamed symbols, which a macro settles.
 *  - Functions that returned 0 for success in SDL2 and true in SDL3. These
 *    cannot be macros: getting the sense backwards turns a failure into a
 *    success, so each is a real function whose SDL2 body converts explicitly.
 *  - Signature changes -- window creation, and both Vulkan entry points.
 *  - Window resize, which SDL2 reports as one event type with a sub-type and
 *    SDL3 reports as distinct event types. That is a shape difference rather
 *    than a naming one, so it is exposed as a predicate instead of a constant.
 *
 * Building against SDL2 requires 2.0.22 or newer, for
 * SDL_GetWindowSizeInPixels.
 */
#ifndef GLEDITOR_SDL_COMPAT_H
#define GLEDITOR_SDL_COMPAT_H

#ifndef GLEDITOR_SDL_MAJOR
#error GLEDITOR_SDL_MAJOR must be defined by the build (2 or 3)
#endif

#include <cstdint>
#include <string>
#include <vector>

#if GLEDITOR_SDL_MAJOR == 3

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#elif GLEDITOR_SDL_MAJOR == 2

#include <SDL2/SDL.h>
#include <SDL2/SDL_messagebox.h>

#if !SDL_VERSION_ATLEAST(2, 0, 22)
#error SDL2 2.0.22 or newer is required (SDL_GetWindowSizeInPixels)
#endif

// -- renamed symbols ---------------------------------------------------------
#define SDL_EVENT_QUIT         SDL_QUIT
#define SDL_EVENT_KEY_DOWN     SDL_KEYDOWN
#define SDL_EVENT_MOUSE_MOTION      SDL_MOUSEMOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SDL_MOUSEBUTTONDOWN
#define SDL_EVENT_TEXT_INPUT        SDL_TEXTINPUT
#define SDL_KMOD_SHIFT         KMOD_SHIFT
#define SDL_KMOD_CTRL          KMOD_CTRL
#define SDL_KMOD_ALT           KMOD_ALT
// SDL3 renamed this to say what it does; SDL2's spelling says what you are
// allowed to do.
#define SDL_WINDOW_HIGH_PIXEL_DENSITY SDL_WINDOW_ALLOW_HIGHDPI
#define SDL_GL_DestroyContext         SDL_GL_DeleteContext
#define SDL_DestroySurface            SDL_FreeSurface

#else
#error GLEDITOR_SDL_MAJOR must be 2 or 3
#endif

namespace sdl {

/// Which SDL this binary was built against, for the startup banner.
inline constexpr int majorVersion = GLEDITOR_SDL_MAJOR;

// -- calls whose success convention changed ----------------------------------

/// True on success. SDL2 returns 0 for success, SDL3 returns true.
inline bool initSubSystem(const std::uint32_t flags) {
#if GLEDITOR_SDL_MAJOR == 3
  return SDL_InitSubSystem(flags);
#else
  return 0 == SDL_InitSubSystem(flags);
#endif
}

/// True on success. Same inversion as initSubSystem().
inline bool glMakeCurrent(SDL_Window *window, const SDL_GLContext context) {
#if GLEDITOR_SDL_MAJOR == 3
  return SDL_GL_MakeCurrent(window, context);
#else
  return 0 == SDL_GL_MakeCurrent(window, context);
#endif
}

// -- calls whose signature changed -------------------------------------------

/**
 * @brief Create a window.
 *
 * SDL3 dropped the position arguments: a window is created wherever the window
 * manager puts it and moved afterwards if that is not wanted. SDL2 takes them,
 * and SDL_WINDOWPOS_UNDEFINED asks it for the same behaviour.
 */
inline SDL_Window *createWindow(const char *title, const int width,
                                const int height, const std::uint64_t flags) {
#if GLEDITOR_SDL_MAJOR == 3
  return SDL_CreateWindow(title, width, height, flags);
#else
  return SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED,
                          SDL_WINDOWPOS_UNDEFINED, width, height,
                          static_cast<std::uint32_t>(flags));
#endif
}

/**
 * @brief Report a window's new size in pixels, if this event carries one.
 *
 * SDL2 delivers every window change as SDL_WINDOWEVENT with a sub-type in
 * `window.event`; SDL3 gives each its own event type. The caller wants the
 * question rather than the constants, so this is a predicate.
 */
inline bool windowSizeChanged(const SDL_Event &event, int &width, int &height) {
#if GLEDITOR_SDL_MAJOR == 3
  switch (event.type) {
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
  case SDL_EVENT_WINDOW_MAXIMIZED:
    break;
  default:
    return false;
  }
#else
  if (SDL_WINDOWEVENT != event.type) {
    return false;
  }
  switch (event.window.event) {
  case SDL_WINDOWEVENT_RESIZED:
  case SDL_WINDOWEVENT_SIZE_CHANGED:
  case SDL_WINDOWEVENT_MAXIMIZED:
    break;
  default:
    return false;
  }
#endif
  width  = event.window.data1;
  height = event.window.data2;
  return true;
}

/**
 * @brief Begin or end delivery of composed text events.
 *
 * SDL3 takes the window the text is destined for, since it tracks input focus
 * per window; SDL2 has one global input context and takes nothing.
 */
inline void startTextInput([[maybe_unused]] SDL_Window *window) {
#if GLEDITOR_SDL_MAJOR == 3
  SDL_StartTextInput(window);
#else
  SDL_StartTextInput();
#endif
}

inline void stopTextInput([[maybe_unused]] SDL_Window *window) {
#if GLEDITOR_SDL_MAJOR == 3
  SDL_StopTextInput(window);
#else
  SDL_StopTextInput();
#endif
}

/**
 * @brief Tell the platform where the text being typed will appear.
 *
 * What an input method needs in order to put its candidate window somewhere
 * sensible, and what an on-screen keyboard needs in order to avoid covering
 * the field being typed into. Ignored by platforms with neither, which is why
 * it is cheap to call whenever the focus moves.
 *
 * Coordinates are window pixels from the top left, which is SDL's convention
 * and not the bottom-up one the glyph pipeline draws in.
 */
inline void setTextInputArea([[maybe_unused]] SDL_Window *window,
                             const int posX, const int posY, const int width,
                             const int height) {
  const SDL_Rect area{posX, posY, width, height};
#if GLEDITOR_SDL_MAJOR == 3
  // The third argument is where the caret is within the area, which SDL3 uses
  // to place a candidate window against the insertion point rather than the
  // whole field.
  SDL_SetTextInputArea(window, &area, 0);
#else
  SDL_SetTextInputRect(&area);
#endif
}

/// What a message box is telling somebody, which decides its icon.
enum class MessageKind : std::uint8_t { Info, Warning, Error };

/**
 * @brief Show a native modal message box and wait for an answer.
 *
 * This is the whole of SDL's cross-platform dialog support: a title, a
 * message, and buttons. It has no text fields, no lists and no widgets of any
 * kind -- which is why anything that has to be filled in is drawn in the
 * window instead, and why what goes through here is the things that really are
 * a message and a choice.
 *
 * Blocking, and platform-modal: on Windows and macOS it takes over the parent
 * window until it is answered. Must be called from the thread that created
 * @p parent.
 *
 * @param buttons Labels, left to right. The first is the default that return
 *        presses; the last is what escape presses.
 * @return Which button was pressed, or -1 when the platform could not show it
 *         at all -- a headless run, or a system with no dialog service.
 */
inline int showMessageBox(SDL_Window *parent, const MessageKind kind,
                          const std::string &title, const std::string &message,
                          const std::vector<std::string> &buttons) {
  std::vector<SDL_MessageBoxButtonData> laid;
  laid.reserve(buttons.size());
  for (std::size_t i = 0; i < buttons.size(); i++) {
    std::uint32_t flags = 0;
    if (0 == i) {
      flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    }
    if (i + 1 == buttons.size()) {
      flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    }
    laid.push_back(SDL_MessageBoxButtonData{flags, static_cast<int>(i),
                                            buttons[i].c_str()});
  }

  SDL_MessageBoxData data{};
  data.flags      = MessageKind::Error == kind     ? SDL_MESSAGEBOX_ERROR
                    : MessageKind::Warning == kind ? SDL_MESSAGEBOX_WARNING
                                                   : SDL_MESSAGEBOX_INFORMATION;
  data.window     = parent;
  data.title      = title.c_str();
  data.message    = message.c_str();
  data.numbuttons = static_cast<int>(laid.size());
  data.buttons    = laid.data();

  int pressed = -1;
#if GLEDITOR_SDL_MAJOR == 3
  if (!SDL_ShowMessageBox(&data, &pressed)) {
    return -1;
  }
#else
  if (0 != SDL_ShowMessageBox(&data, &pressed)) {
    return -1;
  }
#endif
  return pressed;
}

/**
 * @brief Directory the executable was loaded from, with a trailing separator,
 *        or nullptr when SDL cannot tell.
 *
 * SDL3 owns the string and hands back a pointer good for the life of the
 * process; SDL2 returns one the caller must SDL_free. Rather than push that
 * difference onto callers, the SDL2 side copies it into a static once and
 * frees the original -- there is exactly one executable path per process, so a
 * cache costs one small allocation and removes the ownership question from
 * every call site.
 *
 * Returns nullptr before SDL_Init, which is not an error: the caller is
 * expected to have another way of finding its files.
 */
inline const char *basePath() {
#if GLEDITOR_SDL_MAJOR == 3
  return SDL_GetBasePath();
#else
  static const std::string cached = [] {
    char *path = SDL_GetBasePath();
    if (nullptr == path) {
      return std::string{};
    }
    std::string owned(path);
    SDL_free(path);
    return owned;
  }();
  return cached.empty() ? nullptr : cached.c_str();
#endif
}

/// Scancode of a key event. SDL3 flattened away the SDL_Keysym wrapper.
inline SDL_Scancode keyScancode(const SDL_Event &event) {
#if GLEDITOR_SDL_MAJOR == 3
  return event.key.scancode;
#else
  return event.key.keysym.scancode;
#endif
}

/// Modifier state of a key event. Same flattening as keyScancode().
inline std::uint16_t keyModifiers(const SDL_Event &event) {
#if GLEDITOR_SDL_MAJOR == 3
  return event.key.mod;
#else
  return event.key.keysym.mod;
#endif
}

} // namespace sdl

#endif // GLEDITOR_SDL_COMPAT_H
// vi: set sw=2 sts=2 ts=2 et:
