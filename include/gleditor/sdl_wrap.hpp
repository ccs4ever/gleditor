#ifndef GLEDITOR_SDL_WRAP_H
#define GLEDITOR_SDL_WRAP_H

#include <SDL3/SDL_video.h>
#include <cstdint>

struct SDL_Window;
struct SDL_Surface;

/// Each of these owns a single SDL resource and releases it in its destructor,
/// so copying one would destroy that resource twice. They are only ever used as
/// scoped locals; copies and moves are deleted to keep it that way.
#define GLEDITOR_NON_COPYABLE(Type)                                            \
  Type(const Type &)            = delete;                                      \
  Type &operator=(const Type &) = delete;                                      \
  Type(Type &&)                 = delete;                                      \
  Type &operator=(Type &&)      = delete

struct AutoSDL {
  std::uint32_t flags;
  explicit AutoSDL(std::uint32_t flags);
  ~AutoSDL();
  GLEDITOR_NON_COPYABLE(AutoSDL);
};

struct AutoSDLImg {
  explicit AutoSDLImg(int flags);
  ~AutoSDLImg();
  GLEDITOR_NON_COPYABLE(AutoSDLImg);
};

struct AutoSDLWindow {
  SDL_Window *window;
  AutoSDLWindow(const char *title, int x, int y, int width, int height,
                std::uint32_t flags, SDL_Surface *icon = nullptr);
  ~AutoSDLWindow();
  GLEDITOR_NON_COPYABLE(AutoSDLWindow);
};

struct AutoSDLGL {
  SDL_GLContext ctx;
  explicit AutoSDLGL(SDL_Window *window);
  ~AutoSDLGL();
  GLEDITOR_NON_COPYABLE(AutoSDLGL);
};

struct AutoSDLSurface {
  SDL_Surface *surface;
  explicit AutoSDLSurface(const char *fileName);
  ~AutoSDLSurface();
  GLEDITOR_NON_COPYABLE(AutoSDLSurface);
};

#undef GLEDITOR_NON_COPYABLE

#endif // GLEDITOR_SDL_WRAP_H
