#ifndef GLEDITOR_VULKAN_RENDERER_VK_HPP
#define GLEDITOR_VULKAN_RENDERER_VK_HPP

#include <memory>
#include <vector>
#include <string>

#include <gleditor/renderer.hpp>
#include <gleditor/log.hpp>

// Forward declare SDL types
struct SDL_Window;
struct AutoSDLWindow;

// Define a feature toggle macro that downstream build systems can enable
// to turn on real Vulkan initialization. By default, it is disabled to
// avoid adding a hard dependency on Vulkan headers and libraries.
// When enabled (e.g., by adding -DGLEDITOR_ENABLE_VULKAN and linking
// against Vulkan), the RendererVK will perform minimal Vulkan instance
// and surface setup via SDL.

class RendererVK : public AbstractRenderer, public Loggable {
public:
  using Ptr = std::shared_ptr<RendererVK>;

  static Ptr create(const AppStateRef &appState) {
    return std::make_shared<RendererVK>(appState, Private());
  }

  RendererVK(const AppStateRef &state, [[maybe_unused]] Private _priv)
      : AbstractRenderer(state, _priv) {}
  ~RendererVK() override;

  // Entry point for the render thread function.
  void operator()(AutoSDLWindow &window) override;

private:
#ifdef GLEDITOR_ENABLE_VULKAN
  // Opaque Vulkan handles (only present when Vulkan support is enabled)
  struct VulkanHandles;
  std::unique_ptr<VulkanHandles> vk_;
#endif

  // Helpers that are no-ops unless Vulkan is enabled at build time
  void initVulkan(SDL_Window *window);
  void shutdownVulkan();

  // Minimal update loop that mirrors Renderer::operator(): it processes the
  // render queue items and keeps the application running.
  bool tick();
};

#endif // GLEDITOR_VULKAN_RENDERER_VK_HPP
