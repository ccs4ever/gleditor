/**
 * @file sdl_vulkan_compat.hpp
 * @brief SDL's two Vulkan entry points, whose signatures differ between SDL2
 *        and SDL3.
 *
 * Kept apart from sdl_compat.hpp so that a build without the Vulkan backend
 * never includes SDL_vulkan.h or the Vulkan headers behind it. See
 * sdl_compat.hpp for the reasoning behind the shims generally.
 */
#ifndef GLEDITOR_RENDER_VULKAN_SDL_COMPAT_H
#define GLEDITOR_RENDER_VULKAN_SDL_COMPAT_H

#include <gleditor/sdl_compat.hpp>

// See device_vk.hpp for why this is defined before the Vulkan header.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#if GLEDITOR_SDL_MAJOR == 3
#include <SDL3/SDL_vulkan.h>
#else
#include <SDL2/SDL_vulkan.h>
#endif

namespace sdl {

/**
 * @brief Instance extensions the windowing system needs.
 *
 * SDL3 returns a pointer to a list it owns. SDL2 fills a caller-supplied array
 * and has to be asked for the count first, so the copy the caller ends up with
 * is made here either way.
 *
 * @param window Only used by SDL2, which resolves the extensions per window.
 * @return The extension names, or an empty vector on failure.
 */
inline std::vector<std::string> vulkanInstanceExtensions(SDL_Window *window) {
#if GLEDITOR_SDL_MAJOR == 3
  (void)window;
  Uint32 count             = 0;
  const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
  if (nullptr == names) {
    return {};
  }
  return {names, names + count};
#else
  unsigned int count = 0;
  if (SDL_TRUE != SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) {
    return {};
  }
  std::vector<const char *> names(count);
  if (SDL_TRUE !=
      SDL_Vulkan_GetInstanceExtensions(window, &count, names.data())) {
    return {};
  }
  return {names.begin(), names.begin() + count};
#endif
}

/// True on success. SDL3 gained an allocator argument and returns the surface
/// through an out parameter; SDL2 returns SDL_bool.
inline bool vulkanCreateSurface(SDL_Window *window, const VkInstance instance,
                                VkSurfaceKHR &surface) {
#if GLEDITOR_SDL_MAJOR == 3
  return SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface);
#else
  return SDL_TRUE == SDL_Vulkan_CreateSurface(window, instance, &surface);
#endif
}

} // namespace sdl

#endif // GLEDITOR_RENDER_VULKAN_SDL_COMPAT_H
// vi: set sw=2 sts=2 ts=2 et:
