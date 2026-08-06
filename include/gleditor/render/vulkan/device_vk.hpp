/**
 * @file device_vk.hpp
 * @brief RenderDevice implementation for Vulkan.
 *
 * The frame structure deliberately mirrors the OpenGL backend: glyphs are drawn
 * into an offscreen colour target alongside a picking target and a depth
 * buffer, and the colour target is then blitted to the swapchain image. Keeping
 * the two backends structurally alike is what allows their output to be
 * compared directly.
 *
 * Resource updates are simple by design: vertex and uniform buffers live in
 * host-visible coherent memory and stay mapped, and texture uploads go through
 * a staging buffer. Document loading is a burst of such updates between frames,
 * so the cost that matters is the synchronisation, which is amortised to one
 * wait per burst rather than one per update.
 */
#ifndef GLEDITOR_RENDER_VULKAN_DEVICE_H
#define GLEDITOR_RENDER_VULKAN_DEVICE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <gleditor/render/device.hpp>
#include <gleditor/render/diagnostics.hpp>

namespace render::vulkan {

/**
 * @class DeviceVK
 * @brief Vulkan device.
 */
class DeviceVK final : public RenderDevice {
public:
  DeviceVK();
  ~DeviceVK() override;

  [[nodiscard]] Backend backend() const override { return Backend::Vulkan; }

  void initialize(AutoSDLWindow &window) override;
  void shutdown() override;
  void resize(int width, int height) override;

  BufferHandle createBuffer(BufferKind kind, std::size_t bytes) override;
  void destroyBuffer(BufferHandle buffer) override;
  void updateBuffer(BufferHandle buffer, std::size_t offset,
                    std::span<const std::byte> data) override;
  BufferHandle growBuffer(BufferHandle buffer, std::size_t bytes) override;

  TextureHandle createTextureArray(int size, int layers,
                                   TextureFormat format) override;
  void destroyTexture(TextureHandle texture) override;
  void updateTextureLayer(TextureHandle texture, int layer, int xOffset,
                          int yOffset, int width, int height,
                          std::span<const std::byte> data) override;
  [[nodiscard]] TextureLimits textureLimits() const override { return limits; }

  PipelineHandle createPipeline(const PipelineDesc &desc) override;

  bool beginFrame() override;
  void endFrame() override;
  void bindPipeline(PipelineHandle pipeline) override;
  void setFrameUniforms(const FrameUniforms &uniforms) override;
  void bindGlyphTexture(TextureHandle texture) override;
  void setHighlights(std::span<const HighlightRange> ranges) override;
  void drawGlyphs(const DrawUniforms &uniforms, BufferHandle vertices,
                  std::size_t vertexByteOffset,
                  std::uint32_t instanceCount) override;
  void requestPickingTag(int x, int y) override;
  std::optional<PickingResult> takePickingTag() override;
  FrameImage captureColorTarget() override;
  void waitIdle() override;

private:
  /// Frames recorded ahead of the GPU. Two is enough to overlap CPU and GPU
  /// work without letting latency grow.
  static constexpr std::uint32_t framesInFlight = 2;

  struct BufferRecord {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void *mapped{};
    VkDeviceSize bytes{};
    VkBufferUsageFlags usage{};
  };

  struct TextureRecord {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    int size{};
    int layers{};
  };

  struct PipelineRecord {
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
    /// One descriptor set per frame in flight, so updating the set for a new
    /// frame cannot disturb a frame the GPU is still reading.
    std::array<VkDescriptorSet, framesInFlight> sets{};
  };

  /// Per-frame command recording and synchronisation objects.
  struct FrameContext {
    VkCommandBuffer commands{VK_NULL_HANDLE};
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
    VkSemaphore renderFinished{VK_NULL_HANDLE};
    VkFence inFlight{VK_NULL_HANDLE};
    /// Camera uniforms for this frame; per-frame so a submitted frame keeps the
    /// values it was recorded with.
    BufferHandle cameraBuffer{};
    /// Destination of this frame's picking read, if one was requested. The
    /// frame's own fence already says when the copy has completed, so no extra
    /// synchronisation object is needed.
    BufferHandle pickingBuffer{};
    /// Pixel this frame's picking read was aimed at.
    int pickX{};
    int pickY{};
    /// A picking copy was recorded into this frame and its result has not been
    /// collected yet.
    bool pickPending{};
    /// The frame carrying this pick has been submitted, so waiting on its fence
    /// is enough to read the result.
    bool pickSubmitted{};
  };

  // -- setup steps
  void createInstance();
  void createSurface(AutoSDLWindow &window);
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createSwapchain(int width, int height);
  void destroySwapchain();
  void createRenderTargets();
  void destroyRenderTargets();
  void createRenderPass();
  void createFramebuffers();
  void createCommandResources();
  void createDescriptorPool();

  // -- helpers
  [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits,
                                             VkMemoryPropertyFlags props) const;
  BufferRecord allocateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props) const;
  void destroyBufferRecord(BufferRecord &record) const;
  /// Begin a throwaway command buffer for a transfer, and submit + wait on it.
  [[nodiscard]] VkCommandBuffer beginOneShot() const;
  void endOneShot(VkCommandBuffer commands) const;
  /**
   * @brief Wait for submitted frames before mutating a resource they may read.
   *
   * Only the first mutation after a submitted frame actually waits, so a burst
   * of uploads during document loading costs one synchronisation rather than
   * one per upload.
   */
  void ensureIdleForMutation();
  /// Recreate the swapchain and everything sized to it.
  void recreateSwapchain(int width, int height);
  static std::vector<std::uint32_t> readSpirv(const std::string &path);
  VkShaderModule createShaderModule(const std::vector<std::uint32_t> &code) const;

  AutoSDLWindow *targetWindow{};

  VkInstance instance{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
  VkSurfaceKHR surface{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkDevice device{VK_NULL_HANDLE};
  std::uint32_t graphicsFamily{};
  std::uint32_t presentFamily{};
  VkQueue graphicsQueue{VK_NULL_HANDLE};
  VkQueue presentQueue{VK_NULL_HANDLE};

  VkSwapchainKHR swapchain{VK_NULL_HANDLE};
  VkFormat swapchainFormat{VK_FORMAT_UNDEFINED};
  VkExtent2D swapchainExtent{};
  std::vector<VkImage> swapchainImages;

  /// Offscreen colour target the glyphs are drawn into, mirroring the OpenGL
  /// backend's renderbuffer.
  VkImage colourImage{VK_NULL_HANDLE};
  VkDeviceMemory colourMemory{VK_NULL_HANDLE};
  VkImageView colourView{VK_NULL_HANDLE};
  VkImage tagImage{VK_NULL_HANDLE};
  VkDeviceMemory tagMemory{VK_NULL_HANDLE};
  VkImageView tagView{VK_NULL_HANDLE};
  VkImage depthImage{VK_NULL_HANDLE};
  VkDeviceMemory depthMemory{VK_NULL_HANDLE};
  VkImageView depthView{VK_NULL_HANDLE};
  VkFormat depthFormat{VK_FORMAT_UNDEFINED};

  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  VkCommandPool commandPool{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
  VkSampler glyphSampler{VK_NULL_HANDLE};

  std::array<FrameContext, framesInFlight> frames{};
  std::uint32_t frameIndex{};
  std::uint32_t acquiredImage{};
  bool frameActive{};
  bool framesSubmitted{};
  bool swapchainOutOfDate{};

  BufferHandle highlightBuffer{};
  TextureHandle boundTexture{};
  PipelineHandle boundPipeline{};

  TextureLimits limits{};
  /// Diagnostics the validation layers reported since the last frame boundary.
  /// The messenger may be called from any thread the driver uses, which the
  /// sink accounts for.
  DiagnosticSink diagnostics;
  std::unordered_map<std::uint32_t, BufferRecord> buffers;
  std::unordered_map<std::uint32_t, TextureRecord> textures;
  std::unordered_map<std::uint32_t, PipelineRecord> pipelines;
  std::uint32_t nextHandleId{1};
  bool initialised{};
};

} // namespace render::vulkan

#endif // GLEDITOR_RENDER_VULKAN_DEVICE_H
// vi: set sw=2 sts=2 ts=2 et:
