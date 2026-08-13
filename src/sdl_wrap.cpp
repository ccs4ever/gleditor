#include <cstdint>
#include <gleditor/sdl_wrap.hpp> // IWYU pragma: associated
#include <stdexcept>
#include <string>

#include <gleditor/sdl_compat.hpp>

#ifdef GLEDITOR_HAVE_SDL_IMAGE
#if GLEDITOR_SDL_MAJOR == 3
#include <SDL3_image/SDL_image.h>
#else
#include <SDL2/SDL_image.h>
#endif
#endif

#include <iostream>

AutoSDL::AutoSDL(const std::uint32_t flags) : flags(flags) {
  if (!sdl::initSubSystem(flags)) {
    throw std::runtime_error(std::string("SDL init failed: ") + SDL_GetError());
  }
}
AutoSDL::~AutoSDL() {
  if (flags == SDL_WasInit(flags)) {
    SDL_QuitSubSystem(flags);
  }
}

AutoSDLWindow::AutoSDLWindow(const char *title, const int width,
                             const int height, const std::uint64_t flags,
                             SDL_Surface *icon) {
  // The context version and profile belong to the chosen backend, which sets
  // them through render::configureBackendWindowAttributes() before getting
  // here. Forcing a desktop core profile in this constructor would silently
  // defeat a request for OpenGL ES.
  window = sdl::createWindow(title, width, height, flags);

  if (nullptr == window) {
    throw std::runtime_error(std::string("SDL create window failed: ") +
                             SDL_GetError() + "\n");
  }

  if (nullptr != icon) {
    SDL_SetWindowIcon(window, icon);
  }
}
AutoSDLWindow::~AutoSDLWindow() {
  if (nullptr != window) {
    SDL_DestroyWindow(window);
    window = nullptr;
  }
}

AutoSDLGL::AutoSDLGL(SDL_Window *window) {
  // create context for window and make current
  ctx = SDL_GL_CreateContext(window);

  if (nullptr == ctx) {
    throw std::runtime_error(std::string("SDL create GL context failed: ") +
                             SDL_GetError() + "\n");
  }
}
AutoSDLGL::~AutoSDLGL() {
  if (nullptr != ctx) {
    SDL_GL_DestroyContext(ctx);
    ctx = nullptr;
  }
}

AutoSDLSurface::AutoSDLSurface(const char *fileName) {
  // The window icon is decoration. Failing to load it must not stop the editor
  // from starting -- which it used to, both when SDL_image was unavailable and
  // whenever the program was run from a directory that does not hold the file.
#ifdef GLEDITOR_HAVE_SDL_IMAGE
  surface = IMG_Load(fileName);
  if (nullptr == surface) {
    std::cerr << "could not load window icon " << fileName << ": "
              << SDL_GetError() << "\n";
  }
#else
  (void)fileName;
#endif
}
AutoSDLSurface::~AutoSDLSurface() {
  if (nullptr != surface) {
    SDL_DestroySurface(surface);
    surface = nullptr;
  }
}
