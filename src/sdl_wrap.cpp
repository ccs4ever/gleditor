#include <gleditor/sdl_wrap.hpp> // IWYU pragma: associated
#include <cstdint>
#include <stdexcept>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#include <SDL3_image/SDL_image.h>

AutoSDL::AutoSDL(const std::uint32_t flags) : flags(flags) {
  if (!SDL_InitSubSystem(flags)) {
    throw std::runtime_error(std::string("SDL init failed: ") + SDL_GetError());
  }
}
AutoSDL::~AutoSDL() {
  if (flags == SDL_WasInit(flags)) {
    SDL_QuitSubSystem(flags);
  }
}


// NOLINTNEXTLINE(readability-identifier-length)
AutoSDLWindow::AutoSDLWindow(const char *title, const int /*x*/, const int /*y*/, const int width,
                             const int height, const std::uint32_t flags,
                             SDL_Surface *icon) {
  // The context version and profile belong to the chosen backend, which sets
  // them through render::configureBackendWindowAttributes() before getting
  // here. Forcing a desktop core profile in this constructor would silently
  // defeat a request for OpenGL ES.
  window = SDL_CreateWindow(title, width, height, flags);

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
  surface = IMG_Load(fileName);
  if (nullptr == surface) {
    throw std::runtime_error(std::string("IMG_load failed: ") + SDL_GetError() +
                             "\n");
  }
}
AutoSDLSurface::~AutoSDLSurface() {
  SDL_DestroySurface(surface);
  surface = nullptr;
}
