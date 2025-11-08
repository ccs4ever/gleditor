#include <gleditor/vulkan/renderer_vk.hpp> // IWYU pragma: associated

#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <format>
#include <stdexcept>

#include <gleditor/sdl_wrap.hpp>

// We only include SDL and Vulkan headers when Vulkan support is enabled.
#ifdef GLEDITOR_ENABLE_VULKAN
  #include "SDL.h"
  #include "SDL_vulkan.h"
  #include <vulkan/vulkan.h>
#endif

#ifdef GLEDITOR_ENABLE_VULKAN
struct RendererVK::VulkanHandles {
  VkInstance instance{VK_NULL_HANDLE};
  VkSurfaceKHR surface{VK_NULL_HANDLE};
};
#endif

RendererVK::~RendererVK() { shutdownVulkan(); }

void RendererVK::initVulkan([[maybe_unused]] SDL_Window *window) {
#ifdef GLEDITOR_ENABLE_VULKAN
  if (vk_) {
    return; // already initialized
  }
  vk_ = std::make_unique<VulkanHandles>();

  // Query required instance extensions from SDL for surface creation
  unsigned int extCount = 0;
  if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr)) {
    throw std::runtime_error(std::format("SDL_Vulkan_GetInstanceExtensions count failed: {}", SDL_GetError()));
  }
  std::vector<const char *> extensions(extCount);
  if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, extensions.data())) {
    throw std::runtime_error(std::format("SDL_Vulkan_GetInstanceExtensions names failed: {}", SDL_GetError()));
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "gleditor";
  appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
  appInfo.pEngineName = "gleditor";
  appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();
  createInfo.enabledLayerCount = 0;

  VkResult res = vkCreateInstance(&createInfo, nullptr, &vk_->instance);
  if (res != VK_SUCCESS || vk_->instance == VK_NULL_HANDLE) {
    throw std::runtime_error("Vulkan: failed to create instance");
  }

  // Create the surface via SDL
  if (!SDL_Vulkan_CreateSurface(window, vk_->instance, &vk_->surface)) {
    vkDestroyInstance(vk_->instance, nullptr);
    vk_->instance = VK_NULL_HANDLE;
    throw std::runtime_error(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
  }

  std::cout << "Vulkan initialized (instance + surface)." << std::endl;
#else
  (void)window; // unused
#endif
}

void RendererVK::shutdownVulkan() {
#ifdef GLEDITOR_ENABLE_VULKAN
  if (!vk_) return;
  if (vk_->surface != VK_NULL_HANDLE && vk_->instance != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(vk_->instance, vk_->surface, nullptr);
    vk_->surface = VK_NULL_HANDLE;
  }
  if (vk_->instance != VK_NULL_HANDLE) {
    vkDestroyInstance(vk_->instance, nullptr);
    vk_->instance = VK_NULL_HANDLE;
  }
  vk_.reset();
#endif
}

bool RendererVK::tick() {
  // Placeholder for frame timing and any background work. For now, just sleep
  // a little to avoid a hot loop when there is nothing to do.
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(1ms);
  return state->alive;
}

void RendererVK::operator()(AutoSDLWindow &window) {
  // Record render thread id so AbstractRenderer::run() can dispatch correctly
  this->renderThreadId = std::this_thread::get_id();

#ifdef GLEDITOR_ENABLE_VULKAN
  // SDL requires the window to be created with SDL_WINDOW_VULKAN flag by the
  // caller; we assume the application does that when using RendererVK.
  initVulkan(window.window);
#else
  std::cout << "RendererVK built without Vulkan support. Running no-op loop." << std::endl;
#endif

  // Basic event/render loop: process queued commands and keep the app alive.
  while (state->alive) {
    // Process at least one tick even if the queue is empty
    if (!tick()) {
      break;
    }

    while (auto item = renderQueue.pop()) {
      switch (item->type) {
      case RenderItem::Type::NewDoc: {
        // In Vulkan mode we don't yet have a document pipeline; accept the
        // command to keep semantics but no-op for now.
        std::cout << "RendererVK: NewDoc requested (no-op stub)\n";
        break;
      }
      case RenderItem::Type::Resize: {
        // Nothing to do yet until swapchain is implemented. No-op.
        break;
      }
      case RenderItem::Type::OpenDoc: {
        // Not implemented in the Vulkan path yet. No-op.
        break;
      }
      case RenderItem::Type::Run: {
        auto *runItem = dynamic_cast<RenderItemRun *>(item.get());
        if (runItem) {
          (*runItem)();
        }
        break;
      }
      default:
        break;
      }

      if (!tick()) {
        break;
      }
    }
  }

  shutdownVulkan();
}
