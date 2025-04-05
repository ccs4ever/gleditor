#include <gleditor/sdl_wrap.hpp>

#include <string>
#include <cstdint>
#include <stdexcept>
#include <sys/wait.h>

#include "SDL.h"
#include "SDL_image.h"
#include "SDL_surface.h"
#include "SDL_video.h"

AutoSDL::AutoSDL(std::uint32_t flags) : flags(flags) {
  if (0 != SDL_Init(flags)) {
    throw std::runtime_error(std::string("SDL init failed: ") + SDL_GetError());
  }
}
AutoSDL::~AutoSDL() {
  if (flags == SDL_WasInit(flags)) {
    SDL_Quit();
  }
}

AutoSDLImg::AutoSDLImg(int flags) {
  if ((flags & IMG_Init(flags)) != flags) {
    throw std::runtime_error(std::string("Failed to load SDL image: ") +
                             IMG_GetError() + "\n");
  }
}
AutoSDLImg::~AutoSDLImg() { IMG_Quit(); }

// NOLINTNEXTLINE(readability-identifier-length)
AutoSDLWindow::AutoSDLWindow(const char *title, int x, int y, int width,
                             int height, std::uint32_t flags,
                             SDL_Surface *icon) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                      SDL_GL_CONTEXT_DEBUG_FLAG |
                          SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

  window = SDL_CreateWindow(title, x, y, width, height, flags);

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
    SDL_GL_DeleteContext(ctx);
    ctx = nullptr;
  }
}

AutoSDLSurface::AutoSDLSurface(const char *fileName) {
  surface = IMG_Load(fileName);
  if (nullptr == surface) {
    throw std::runtime_error(std::string("IMG_load failed: ") + IMG_GetError() +
                             "\n");
  }
}
AutoSDLSurface::~AutoSDLSurface() {
  SDL_FreeSurface(surface);
  surface = nullptr;
}
