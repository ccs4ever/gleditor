#ifndef GLEDITOR_SDL_WRAP_H
#define GLEDITOR_SDL_WRAP_H

#include "cstdint"

struct SDL_Window;
struct SDL_Surface;

struct AutoSDL {
  std::uint32_t flags;
  AutoSDL(std::uint32_t flags);
  ~AutoSDL();
};

struct AutoSDLImg {
  AutoSDLImg(int flags);
  ~AutoSDLImg();
};

struct AutoSDLWindow {
  SDL_Window *window;
  AutoSDLWindow(const char *title, int x, int y, int width, int height,
                std::uint32_t flags, SDL_Surface* icon = nullptr);
  ~AutoSDLWindow();
};

struct AutoSDLGL {
  void *ctx;
  AutoSDLGL(SDL_Window *window);
  ~AutoSDLGL();
};

struct AutoSDLSurface {
  SDL_Surface *surface;
  AutoSDLSurface(const char *fileName);
  ~AutoSDLSurface();
};

#endif // GLEDITOR_SDL_WRAP_H
