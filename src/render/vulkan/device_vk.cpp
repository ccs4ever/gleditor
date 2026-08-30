/**
 * @file device_vk.cpp
 * @brief Vulkan device implementation: setup and resources.
 */
#include <gleditor/render/vulkan/device_vk.hpp> // IWYU pragma: associated

#include <gleditor/render/vulkan/sdl_vulkan_compat.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gleditor/sdl_wrap.hpp>

namespace render::vulkan {

namespace {

/// Throw with the failing call named, so a VkResult never escapes silently.
void check(const VkResult result, const char *what) {
  if (VK_SUCCESS != result) {
    throw std::runtime_error(std::format("Vulkan: {} failed with VkResult {}",
                                         what, static_cast<int>(result)));
  }
}

/// Format of the picking target. Two 32-bit unsigned channels, matching the
/// uvec4 the fragment stage writes and the RGBA32UI renderbuffer the OpenGL
/// backend uses.
constexpr VkFormat tagFormat = VK_FORMAT_R32G32B32A32_UINT;
/// Format of the offscreen colour target. Fixed rather than taken from the
/// swapchain so that a captured frame has the same channel order everywhere.
constexpr VkFormat colourFormat = VK_FORMAT_R8G8B8A8_UNORM;

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
              const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  auto *sink = static_cast<DiagnosticSink *>(user);
  if (nullptr == sink || nullptr == data || nullptr == data->pMessage) {
    return VK_FALSE;
  }

  // A validation error means this program used the API wrongly. Recording it
  // rather than only printing is what lets the frame boundary fail on it: a
  // printed error that nothing acts on is a message the build stays green
  // through.
  if (0 != (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
    sink->record(DiagnosticSeverity::Error, data->pMessage);
  } else if (0 !=
             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
    sink->record(DiagnosticSeverity::Warning, data->pMessage);
  }
  // VK_FALSE means "do not abort the call that produced this", which is the
  // only value an application is permitted to return here.
  return VK_FALSE;
}

/// True when the validation layer is present. It is enabled when available
/// because the cost is a debug-build concern only and the diagnostics are the
/// difference between a clear message and a blank window.
bool validationLayerAvailable() {
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::ranges::any_of(layers, [](const VkLayerProperties &layer) {
    return 0 == std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation");
  });
}

/// True when the named instance extension is present. Used for
/// VK_KHR_portability_enumeration, which only a loader carrying a
/// portability driver -- MoltenVK on macOS -- advertises; every other
/// platform's loader simply does not have it, and this is what lets the same
/// call sequence run on both without an `#ifdef` on the platform.
bool instanceExtensionAvailable(const char *name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
  return std::ranges::any_of(
      extensions, [name](const VkExtensionProperties &extension) {
        return 0 == std::strcmp(extension.extensionName, name);
      });
}

/// True when the named device extension is present on `physicalDevice`. Used
/// for VK_KHR_portability_subset, which the Vulkan spec requires be enabled
/// whenever a device reports it -- MoltenVK always does, no conformant driver
/// ever does -- so this is what decides per device rather than per platform.
bool deviceExtensionAvailable(const VkPhysicalDevice physicalDevice,
                              const char *name) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count,
                                       nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count,
                                       extensions.data());
  return std::ranges::any_of(
      extensions, [name](const VkExtensionProperties &extension) {
        return 0 == std::strcmp(extension.extensionName, name);
      });
}

} // namespace

namespace {

/**
 * @brief How many threads to record with, and whether that was asked for.
 *
 * GLEDITOR_RECORD_THREADS overrides the default; 1 turns splitting off
 * entirely, and anything more forces it on. Recording threads compete for
 * cores with whatever else is running -- including, on a software rasteriser,
 * the driver drawing the previous frame -- so whether a split pays depends on
 * the machine and not only on the number of draws. Left to itself the device
 * measures and picks; the override is what lets the same binary be run both
 * ways, which is how the two paths get compared at all.
 */
std::pair<std::uint32_t, bool> recordingThreadCount(const std::uint32_t limit) {
  // hardware_concurrency reports 0 when it cannot tell, which would otherwise
  // ask for a pool of zero; treat that as "no idea, do not split".
  const auto automatic =
      std::min(limit, std::max(1U, std::thread::hardware_concurrency()));
  const auto *requested = std::getenv("GLEDITOR_RECORD_THREADS");
  if (nullptr == requested) {
    return {automatic, false};
  }
  try {
    return {std::max(1U, static_cast<std::uint32_t>(
                             std::stoul(std::string(requested)))),
            true};
  } catch (const std::exception &) {
    std::cerr << "GLEDITOR_RECORD_THREADS is not a number, ignoring: "
              << requested << "\n";
  }
  return {automatic, false};
}

} // namespace

// Delegated so that the environment is read once: reading it in each member's
// initialiser would warn twice about the same bad value.
DeviceVK::DeviceVK() : DeviceVK(recordingThreadCount(maxRecordingThreads)) {}

DeviceVK::DeviceVK(const std::pair<std::uint32_t, bool> recording)
    : recorders(recording.first), recordingThreadsForced(recording.second) {}

DeviceVK::~DeviceVK() { DeviceVK::shutdown(); }

// -- setup --------------------------------------------------------------------

void DeviceVK::createInstance() {
  // Owned copies: SDL2 fills a caller-supplied array, so the names have to
  // outlive the call that produced them either way.
  const auto sdlExtNames = sdl::vulkanInstanceExtensions(targetWindow->window);
  if (sdlExtNames.empty()) {
    throw std::runtime_error(std::format(
        "SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError()));
  }
  std::vector<const char *> extensions;
  extensions.reserve(sdlExtNames.size());
  for (const auto &name : sdlExtNames) {
    extensions.push_back(name.c_str());
  }

  const bool wantValidation = validationLayerAvailable();
  if (wantValidation) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  // Since loader version 1.3.216, a loader that carries a portability driver
  // -- MoltenVK, wrapping Metal, is the one this project cares about --
  // excludes that driver from vkEnumeratePhysicalDevices unless the instance
  // both enables this extension and sets the matching create flag below. No
  // other loader advertises the extension, so this is a no-op everywhere
  // except macOS rather than a platform check.
  const bool wantPortability =
      instanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  if (wantPortability) {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  }

  VkApplicationInfo appInfo{};
  appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName   = "gleditor";
  appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
  appInfo.pEngineName        = "gleditor";
  appInfo.apiVersion         = VK_API_VERSION_1_0;

  static constexpr std::array validationLayers = {
      "VK_LAYER_KHRONOS_validation"};

  VkInstanceCreateInfo createInfo{};
  createInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount =
      static_cast<std::uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();
  if (wantPortability) {
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  if (wantValidation) {
    createInfo.enabledLayerCount   = validationLayers.size();
    createInfo.ppEnabledLayerNames = validationLayers.data();
  }

  check(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");

  if (wantValidation) {
    // The debug messenger entry points are extension functions and have to be
    // looked up rather than linked.
    auto *create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (nullptr != create) {
      VkDebugUtilsMessengerCreateInfoEXT info{};
      info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      info.pfnUserCallback = debugCallback;
      info.pUserData       = &diagnostics;
      create(instance, &info, nullptr, &debugMessenger);
    }
  }
}

void DeviceVK::createSurface(AutoSDLWindow &window) {
  if (!sdl::vulkanCreateSurface(window.window, instance, surface)) {
    throw std::runtime_error(
        std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
  }
}

void DeviceVK::pickPhysicalDevice() {
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (0 == count) {
    throw std::runtime_error("Vulkan: no physical devices");
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  for (const auto candidate : devices) {
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                             families.data());

    bool haveGraphics      = false;
    bool havePresent       = false;
    std::uint32_t graphics = 0;
    std::uint32_t present  = 0;
    for (std::uint32_t i = 0; i < familyCount; i++) {
      if (!haveGraphics &&
          0 != (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        graphics     = i;
        haveGraphics = true;
      }
      VkBool32 supported = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
      if (!havePresent && VK_TRUE == supported) {
        present     = i;
        havePresent = true;
      }
    }
    if (!haveGraphics || !havePresent) {
      continue;
    }

    physicalDevice = candidate;
    graphicsFamily = graphics;
    presentFamily  = present;
    break;
  }

  if (VK_NULL_HANDLE == physicalDevice) {
    throw std::runtime_error(
        "Vulkan: no device with both graphics and present queues");
  }

  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physicalDevice, &props);
  std::cout << std::format("render: vulkan device, {} (API {}.{}.{})\n",
                           props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                           VK_VERSION_MINOR(props.apiVersion),
                           VK_VERSION_PATCH(props.apiVersion));

  limits = TextureLimits{static_cast<int>(props.limits.maxImageDimension2D),
                         static_cast<int>(props.limits.maxImageArrayLayers)};

  // Pick the first depth format the device can use as a depth attachment.
  for (const auto candidate :
       std::array{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                  VK_FORMAT_D24_UNORM_S8_UINT}) {
    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate,
                                        &formatProps);
    if (0 != (formatProps.optimalTilingFeatures &
              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      depthFormat = candidate;
      break;
    }
  }
  if (VK_FORMAT_UNDEFINED == depthFormat) {
    throw std::runtime_error("Vulkan: no usable depth format");
  }
}

void DeviceVK::createLogicalDevice() {
  const std::set<std::uint32_t> uniqueFamilies{graphicsFamily, presentFamily};
  std::vector<VkDeviceQueueCreateInfo> queueInfos;
  const float priority = 1.0F;
  for (const auto family : uniqueFamilies) {
    VkDeviceQueueCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.queueFamilyIndex = family;
    info.queueCount       = 1;
    info.pQueuePriorities = &priority;
    queueInfos.push_back(info);
  }

  std::vector<const char *> deviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  // The spec requires this be enabled whenever the device reports it; every
  // MoltenVK device does and no conformant driver ever does, so this is a
  // per-device check rather than a platform one, the same as the instance
  // extension above.
  if (deviceExtensionAvailable(physicalDevice,
                               VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
    deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
  }

  // Without independentBlend every colour attachment of a pipeline must carry
  // identical blend state. This pipeline cannot: the colour target blends on
  // alpha so a draw can fade, and the picking target is an integer format,
  // for which the spec requires blending be off. Asking for the feature is
  // what makes those two states legal in one pipeline.
  VkPhysicalDeviceFeatures available{};
  vkGetPhysicalDeviceFeatures(physicalDevice, &available);
  if (VK_FALSE == available.independentBlend) {
    throw std::runtime_error(
        "vulkan: the device does not support independentBlend, which the "
        "glyph pipeline needs in order to blend colour without blending the "
        "picking attachment");
  }
  VkPhysicalDeviceFeatures features{};
  features.independentBlend = VK_TRUE;

  VkDeviceCreateInfo info{};
  info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.queueCreateInfoCount    = static_cast<std::uint32_t>(queueInfos.size());
  info.pQueueCreateInfos       = queueInfos.data();
  info.pEnabledFeatures        = &features;
  info.enabledExtensionCount   = deviceExtensions.size();
  info.ppEnabledExtensionNames = deviceExtensions.data();

  check(vkCreateDevice(physicalDevice, &info, nullptr, &device),
        "vkCreateDevice");

  vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
  vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
}

void DeviceVK::createSwapchain(const int width, const int height) {
  VkSurfaceCapabilitiesKHR caps{};
  check(
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps),
      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

  std::uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                       nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                       formats.data());
  if (formats.empty()) {
    throw std::runtime_error("Vulkan: surface reports no formats");
  }

  auto chosen = formats.front();
  for (const auto &candidate : formats) {
    if (VK_FORMAT_B8G8R8A8_UNORM == candidate.format ||
        VK_FORMAT_R8G8B8A8_UNORM == candidate.format) {
      chosen = candidate;
      break;
    }
  }
  swapchainFormat = chosen.format;

  // A current extent of 0xFFFFFFFF means the surface defers to the swapchain,
  // so the window size is used and clamped to what the surface permits.
  if (0xFFFFFFFFU == caps.currentExtent.width) {
    swapchainExtent.width =
        std::clamp(static_cast<std::uint32_t>(width), caps.minImageExtent.width,
                   caps.maxImageExtent.width);
    swapchainExtent.height =
        std::clamp(static_cast<std::uint32_t>(height),
                   caps.minImageExtent.height, caps.maxImageExtent.height);
  } else {
    swapchainExtent = caps.currentExtent;
  }

  std::uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR info{};
  info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface          = surface;
  info.minImageCount    = imageCount;
  info.imageFormat      = swapchainFormat;
  info.imageColorSpace  = chosen.colorSpace;
  info.imageExtent      = swapchainExtent;
  info.imageArrayLayers = 1;
  // The swapchain is only ever a blit destination; the glyphs are drawn into
  // the offscreen colour target instead.
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  const std::array families = {graphicsFamily, presentFamily};
  if (graphicsFamily != presentFamily) {
    info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = families.size();
    info.pQueueFamilyIndices   = families.data();
  } else {
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  info.preTransform   = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  // FIFO is the only mode every implementation must support.
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.clipped     = VK_TRUE;

  check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain),
        "vkCreateSwapchainKHR");

  std::uint32_t actual = 0;
  vkGetSwapchainImagesKHR(device, swapchain, &actual, nullptr);
  swapchainImages.resize(actual);
  vkGetSwapchainImagesKHR(device, swapchain, &actual, swapchainImages.data());
}

void DeviceVK::destroySwapchain() {
  if (VK_NULL_HANDLE != swapchain) {
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
  swapchainImages.clear();
}

namespace {

/// Create an image plus its backing memory and a view over it.
void createImage(const VkDevice device,
                 const VkPhysicalDeviceMemoryProperties &memProps,
                 const VkExtent2D extent, const VkFormat format,
                 const VkImageUsageFlags usage, const VkImageAspectFlags aspect,
                 VkImage &image, VkDeviceMemory &memory, VkImageView &view) {
  VkImageCreateInfo info{};
  info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType     = VK_IMAGE_TYPE_2D;
  info.format        = format;
  info.extent        = {extent.width, extent.height, 1};
  info.mipLevels     = 1;
  info.arrayLayers   = 1;
  info.samples       = VK_SAMPLE_COUNT_1_BIT;
  info.tiling        = VK_IMAGE_TILING_OPTIMAL;
  info.usage         = usage;
  info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  check(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage");

  VkMemoryRequirements reqs{};
  vkGetImageMemoryRequirements(device, image, &reqs);

  std::uint32_t typeIndex = 0;
  bool found              = false;
  for (std::uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    if (0 != (reqs.memoryTypeBits & (1U << i)) &&
        (memProps.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
      typeIndex = i;
      found     = true;
      break;
    }
  }
  if (!found) {
    throw std::runtime_error("Vulkan: no device-local memory type for image");
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize  = reqs.size;
  alloc.memoryTypeIndex = typeIndex;
  check(vkAllocateMemory(device, &alloc, nullptr, &memory),
        "vkAllocateMemory (image)");
  check(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory");

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image            = image;
  viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format           = format;
  viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
  check(vkCreateImageView(device, &viewInfo, nullptr, &view),
        "vkCreateImageView");
}

} // namespace

void DeviceVK::createRenderTargets() {
  destroyRenderTargets();

  createImage(device, memoryProperties, swapchainExtent, colourFormat,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, colourImage, colourMemory, colourView);
  createImage(device, memoryProperties, swapchainExtent, tagFormat,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT, tagImage, tagMemory, tagView);
  createImage(device, memoryProperties, swapchainExtent, depthFormat,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
              VK_IMAGE_ASPECT_DEPTH_BIT, depthImage, depthMemory, depthView);
}

void DeviceVK::destroyRenderTargets() {
  /// One offscreen attachment: its view, image and backing memory.
  struct Target {
    VkImageView *view;
    VkImage *image;
    VkDeviceMemory *memory;
  };
  const std::array<Target, 3> targets = {
      Target{&colourView, &colourImage, &colourMemory},
      Target{&tagView, &tagImage, &tagMemory},
      Target{&depthView, &depthImage, &depthMemory}};
  for (const auto &[view, image, memory] : targets) {
    if (VK_NULL_HANDLE != *view) {
      vkDestroyImageView(device, *view, nullptr);
      *view = VK_NULL_HANDLE;
    }
    if (VK_NULL_HANDLE != *image) {
      vkDestroyImage(device, *image, nullptr);
      *image = VK_NULL_HANDLE;
    }
    if (VK_NULL_HANDLE != *memory) {
      vkFreeMemory(device, *memory, nullptr);
      *memory = VK_NULL_HANDLE;
    }
  }
}

void DeviceVK::createRenderPass() {
  std::array<VkAttachmentDescription, 3> attachments{};
  // colour
  attachments[0].format         = colourFormat;
  attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[0].finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  // picking tag
  attachments[1]             = attachments[0];
  attachments[1].format      = tagFormat;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  // depth
  attachments[2].format         = depthFormat;
  attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
  attachments[2].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[2].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[2].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  const std::array<VkAttachmentReference, 2> colourRefs = {
      VkAttachmentReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      VkAttachmentReference{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
  const VkAttachmentReference depthRef{
      2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount    = colourRefs.size();
  subpass.pColorAttachments       = colourRefs.data();
  subpass.pDepthStencilAttachment = &depthRef;

  // The blit that follows the pass reads the colour attachment, so the write
  // has to be ordered before it.
  VkSubpassDependency dependency{};
  dependency.srcSubpass    = 0;
  dependency.dstSubpass    = VK_SUBPASS_EXTERNAL;
  dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependency.dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  VkRenderPassCreateInfo info{};
  info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = attachments.size();
  info.pAttachments    = attachments.data();
  info.subpassCount    = 1;
  info.pSubpasses      = &subpass;
  info.dependencyCount = 1;
  info.pDependencies   = &dependency;

  check(vkCreateRenderPass(device, &info, nullptr, &renderPass),
        "vkCreateRenderPass");
}

void DeviceVK::createFramebuffers() {
  if (VK_NULL_HANDLE != framebuffer) {
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffer = VK_NULL_HANDLE;
  }
  const std::array views = {colourView, tagView, depthView};

  VkFramebufferCreateInfo info{};
  info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass      = renderPass;
  info.attachmentCount = views.size();
  info.pAttachments    = views.data();
  info.width           = swapchainExtent.width;
  info.height          = swapchainExtent.height;
  info.layers          = 1;
  check(vkCreateFramebuffer(device, &info, nullptr, &framebuffer),
        "vkCreateFramebuffer");
}

void DeviceVK::createCommandResources() {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsFamily;
  check(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool),
        "vkCreateCommandPool");

  std::array<VkCommandBuffer, framesInFlight> commandBuffers{};
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool        = commandPool;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = framesInFlight;
  check(vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()),
        "vkAllocateCommandBuffers");

  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  // Signalled to begin with, so the first frame does not wait on a fence that
  // nothing will ever signal.
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (std::uint32_t i = 0; i < framesInFlight; i++) {
    frames[i].commands = commandBuffers[i];
    check(
        vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].imageAvailable),
        "vkCreateSemaphore");
    check(
        vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].renderFinished),
        "vkCreateSemaphore");
    check(vkCreateFence(device, &fenceInfo, nullptr, &frames[i].inFlight),
          "vkCreateFence");
    // Destination for this slot's picking read: four unsigned integers, the
    // uvec4 the fragment stage writes to the picking attachment.
    frames[i].pickingBuffer =
        createBuffer(BufferKind::Readback, 4 * sizeof(std::uint32_t));

    // One command pool per recording thread per frame. A pool may only be
    // touched by one thread at a time, so sharing one between the workers
    // would need a lock and give back exactly what the split was for.
    // RESET_COMMAND_BUFFER_BIT is deliberately absent: the whole pool is reset
    // at the start of the frame, which is cheaper than resetting each buffer
    // and is all the reuse pattern here needs.
    frames[i].slots.resize(recorders.parallelism());
    VkCommandPoolCreateInfo slotPool{};
    slotPool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    slotPool.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    slotPool.queueFamilyIndex = graphicsFamily;
    for (auto &slot : frames[i].slots) {
      check(vkCreateCommandPool(device, &slotPool, nullptr, &slot.pool),
            "vkCreateCommandPool (recording slot)");
    }
  }
}

void DeviceVK::createDescriptorPool() {
  // One uniform buffer and one sampled image per set, and one set per frame in
  // flight for each pipeline: a set updated for the frame being recorded must
  // not disturb the frame the GPU is still reading, nor the other pipeline's.
  constexpr std::uint32_t sets = maxPipelines * framesInFlight;
  const std::array<VkDescriptorPoolSize, 2> sizes = {
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets}};

  VkDescriptorPoolCreateInfo info{};
  info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.maxSets       = sets;
  info.poolSizeCount = sizes.size();
  info.pPoolSizes    = sizes.data();
  check(vkCreateDescriptorPool(device, &info, nullptr, &descriptorPool),
        "vkCreateDescriptorPool");

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  // Blend between mip levels rather than snapping to one, and let the sampler
  // reach every level the atlas might have. A sampler left at the default
  // maxLod of zero would sample level zero however small the glyph became,
  // which is the aliasing this is here to remove.
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.minLod     = 0.0F;
  samplerInfo.maxLod     = VK_LOD_CLAMP_NONE;
  check(vkCreateSampler(device, &samplerInfo, nullptr, &glyphSampler),
        "vkCreateSampler");
}

void DeviceVK::initialize(AutoSDLWindow &window) {
  targetWindow = &window;

  createInstance();
  createSurface(window);
  pickPhysicalDevice();
  createLogicalDevice();

  int width  = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window.window, &width, &height);
  createSwapchain(width > 0 ? width : 1, height > 0 ? height : 1);
  createRenderTargets();
  createRenderPass();
  createFramebuffers();
  createCommandResources();
  createDescriptorPool();
  stagingStream = std::make_unique<StreamBufferVK>(
      device, memoryProperties, 16 * 1024 * 1024,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  for (auto &hb : highlightBuffers) {
    hb = createBuffer(BufferKind::Uniform,
                      sizeof(HighlightRange) * maxHighlightRanges);
  }

  initialised = true;
}

void DeviceVK::shutdown() {
  if (!initialised) {
    return;
  }
  initialised = false;

  vkDeviceWaitIdle(device);

  stagingStream.reset();

  for (auto &[id, record] : pipelines) {
    vkDestroyPipeline(device, record.pipeline, nullptr);
    vkDestroyPipelineLayout(device, record.layout, nullptr);
    vkDestroyDescriptorSetLayout(device, record.setLayout, nullptr);
  }
  pipelines.clear();

  for (auto &[id, record] : textures) {
    vkDestroyImageView(device, record.view, nullptr);
    vkDestroyImage(device, record.image, nullptr);
    vkFreeMemory(device, record.memory, nullptr);
  }
  textures.clear();

  for (auto &[id, record] : buffers) {
    destroyBufferRecord(record);
  }
  buffers.clear();

  if (VK_NULL_HANDLE != glyphSampler) {
    vkDestroySampler(device, glyphSampler, nullptr);
    glyphSampler = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != descriptorPool) {
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    descriptorPool = VK_NULL_HANDLE;
  }
  for (auto &frame : frames) {
    if (VK_NULL_HANDLE != frame.imageAvailable) {
      vkDestroySemaphore(device, frame.imageAvailable, nullptr);
    }
    if (VK_NULL_HANDLE != frame.renderFinished) {
      vkDestroySemaphore(device, frame.renderFinished, nullptr);
    }
    if (VK_NULL_HANDLE != frame.inFlight) {
      vkDestroyFence(device, frame.inFlight, nullptr);
    }
    // Destroying a pool frees every buffer allocated from it, so the recorded
    // secondaries need no separate release.
    for (auto &slot : frame.slots) {
      if (VK_NULL_HANDLE != slot.pool) {
        vkDestroyCommandPool(device, slot.pool, nullptr);
      }
    }
    frame = FrameContext{};
  }
  if (VK_NULL_HANDLE != commandPool) {
    vkDestroyCommandPool(device, commandPool, nullptr);
    commandPool = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != framebuffer) {
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffer = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != renderPass) {
    vkDestroyRenderPass(device, renderPass, nullptr);
    renderPass = VK_NULL_HANDLE;
  }
  destroyRenderTargets();
  destroySwapchain();

  if (VK_NULL_HANDLE != device) {
    vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != surface) {
    vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != debugMessenger) {
    auto *destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (nullptr != destroy) {
      destroy(instance, debugMessenger, nullptr);
    }
    debugMessenger = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != instance) {
    vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
  }
}

void DeviceVK::recreateSwapchain(const int width, const int height) {
  vkDeviceWaitIdle(device);
  destroySwapchain();
  createSwapchain(width, height);
  createRenderTargets();
  createFramebuffers();
  swapchainOutOfDate = false;
}

void DeviceVK::resize(const int width, const int height) {
  if (!initialised || width <= 0 || height <= 0) {
    return;
  }
  recreateSwapchain(width, height);
}

void DeviceVK::waitIdle() {
  if (VK_NULL_HANDLE != device) {
    vkDeviceWaitIdle(device);
    framesSubmitted = false;
  }
}

void DeviceVK::ensureIdleForMutation() {
  if (!framesSubmitted) {
    return;
  }
  vkDeviceWaitIdle(device);
  framesSubmitted = false;
}

} // namespace render::vulkan
