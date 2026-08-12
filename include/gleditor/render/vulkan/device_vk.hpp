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

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <gleditor/render/device.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/worker_pool.hpp>

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

  /**
   * @brief Vulkan can record one frame from several threads.
   *
   * Recording and submission are separate operations here, and a command pool
   * belongs to whichever thread is using it, so each worker builds its own
   * secondary command buffer with no lock between them; one thread then submits
   * the lot. That is what the GL family cannot do, whatever the driver.
   *
   * The thread count is fixed at initialisation because the per-thread command
   * pools are, so this reports what the device will really use rather than what
   * the machine might allow.
   */
  [[nodiscard]] DeviceCapabilities capabilities() const override {
    return DeviceCapabilities{recorders.parallelism() > 1,
                              recorders.parallelism()};
  }

  void initialize(AutoSDLWindow &window) override;
  void shutdown() override;
  void resize(int width, int height) override;

  BufferHandle createBuffer(BufferKind kind, std::size_t bytes) override;
  void destroyBuffer(BufferHandle buffer) override;
  void updateBuffer(BufferHandle buffer, std::size_t offset,
                    std::span<const std::byte> data) override;
  BufferHandle resizeBuffer(BufferHandle buffer, std::size_t bytes) override;

  TextureHandle createTextureArray(int size, int layers, TextureFormat format,
                                   int levels) override;
  void destroyTexture(TextureHandle texture) override;
  void generateMipmaps(TextureHandle texture) override;
  void updateTextureLayer(TextureHandle texture, int layer, int xOffset,
                          int yOffset, int width, int height,
                          std::span<const std::byte> data) override;
  [[nodiscard]] TextureLimits textureLimits() const override { return limits; }

  PipelineHandle createPipeline(const PipelineDesc &desc) override;

  bool beginFrame() override;
  void endFrame() override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindGlyphTexture(TextureHandle texture) override;
  void setHighlights(std::span<const HighlightRange> ranges) override;
  void drawGlyphs(const DrawUniforms &uniforms, BufferHandle vertices,
                  std::size_t vertexByteOffset,
                  std::uint32_t instanceCount) override;
  void drawGlyphBatches(std::span<const GlyphBatch> batches) override;
  void requestPickingTag(int x, int y) override;
  std::optional<PickingResult> takePickingTag() override;
  FrameImage captureColorTarget() override;
  void waitIdle() override;

  std::vector<Diagnostic> takeDiagnostics() override {
    return diagnostics.drain();
  }
  void setStrictDiagnostics(bool strict) override {
    diagnostics.setStrict(strict);
  }
  /**
   * @brief Refused rather than ignored.
   *
   * Every frame acquires a swapchain image and presenting is what hands it
   * back. Skipping the present would work for the first frame or two and then
   * block forever in acquire, which is a worse answer than saying no: the
   * caller wanted a capture and would get a hang.
   */
  void setPresentEnabled(bool enabled) override;

private:
  /// Thread count and whether it was demanded, as one value so that the
  /// environment is consulted once. See the delegating constructor.
  explicit DeviceVK(std::pair<std::uint32_t, bool> recording);

  /// Frames recorded ahead of the GPU. Two is enough to overlap CPU and GPU
  /// work without letting latency grow.
  static constexpr std::uint32_t framesInFlight = 2;

  /// Pipelines the descriptor pool is sized for: the document pipeline and the
  /// overlay one, with room to spare. Vulkan pools are fixed at creation, so
  /// this is a ceiling rather than a hint.
  static constexpr std::uint32_t maxPipelines = 4;

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
    /// Mip levels allocated. One means the chain is never generated.
    int levels{1};
  };

  struct PipelineRecord {
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
    /// One descriptor set per frame in flight, so updating the set for a new
    /// frame cannot disturb a frame the GPU is still reading.
    std::array<VkDescriptorSet, framesInFlight> sets{};
  };

  /**
   * @brief Smallest run of draws that is even considered for a split.
   *
   * A split costs waking the workers and one begin/end pair of a secondary
   * command buffer each, against a few hundred nanoseconds saved per draw.
   * Below this the overhead cannot be repaid however cheap the wake-up is, so
   * the run is recorded in one piece without measuring anything.
   */
  static constexpr std::size_t parallelRecordingThreshold = 128;

  /**
   * @brief Frames spent comparing the two recording strategies, and frames
   *        between one comparison and the next.
   *
   * Whether splitting pays is not a property of the code: it depends on
   * whether this machine has a core free for a worker to wake onto. Measured
   * against a software rasteriser, which keeps every core busy drawing the
   * previous frame, the split loses -- the workers wait for a core longer than
   * recording in one piece would have taken. With a GPU doing the drawing,
   * those cores are idle and the same split wins. Neither answer can be
   * compiled in, so the device times both and keeps the cheaper.
   *
   * Each strategy is measured over a run of consecutive frames, not by
   * alternating between them, and the first few frames of each run are
   * discarded. Both details are load-bearing. What a split costs depends on
   * how long a worker waits for a core, which varies far more frame to frame
   * than recording in one piece does; and alternating quietly penalises the
   * split, because its workers then park between every measured frame instead
   * of being woken by each one as they are in steady state. Measured by
   * alternating, the split read 7% cheaper than recording in one piece where
   * running it for real was 18% cheaper -- little enough to lose to the margin
   * below.
   *
   * This narrows the error rather than removing it. Where the two strategies
   * are close, the split's own run-to-run spread is wider than the difference
   * being measured and the comparison can still go either way; where they are
   * far apart, which is where the choice is worth anything, it has been
   * consistent. GLEDITOR_RECORD_THREADS settles it by hand when that is not
   * good enough.
   */
  static constexpr std::uint32_t recordingProbeWarmup   = 3;
  static constexpr std::uint32_t recordingProbeSamples  = 8;
  static constexpr std::uint32_t recordingProbeBlock =
      recordingProbeWarmup + recordingProbeSamples;
  static constexpr std::uint32_t recordingProbeFrames = 2 * recordingProbeBlock;
  static constexpr std::uint32_t recordingProbeInterval = 600;

  /**
   * @brief How much cheaper the split has to be before it is used.
   *
   * The two strategies can be within the noise of each other, and then the
   * comparison decides by coin toss and changes its mind at the next probe.
   * Requiring a clear margin settles that in favour of recording in one piece,
   * which is the simpler thing to have going on inside a frame.
   */
  static constexpr double parallelRecordingMargin = 0.9;

  /// What one recording strategy cost over the frames it was measured on.
  struct RecordingCost {
    /// Per-draw cost of each measured frame of this strategy's run.
    std::array<double, recordingProbeSamples> samples{};
    std::size_t count{};

    void add(const double sample) {
      if (count < samples.size()) {
        samples[count++] = sample;
      }
    }
    void reset() { count = 0; }
    /// Median cost per draw, or infinity when nothing has been measured, so
    /// that an unmeasured strategy never wins a comparison by default.
    [[nodiscard]] double median() const {
      if (0 == count) {
        return std::numeric_limits<double>::infinity();
      }
      auto sorted = samples;
      const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(count / 2);
      std::nth_element(sorted.begin(), middle,
                       sorted.begin() + static_cast<std::ptrdiff_t>(count));
      return *middle;
    }
  };

  /**
   * @brief One thread's command storage for one frame in flight.
   *
   * A Vulkan command pool may only be used by one thread at a time, so
   * recording on N threads means N pools. They are per frame as well as per
   * thread because a pool cannot be reset while the GPU is still executing
   * buffers allocated from it.
   */
  struct RecordSlot {
    VkCommandPool pool{VK_NULL_HANDLE};
    /// Secondary buffers allocated from this pool, reused frame to frame.
    std::vector<VkCommandBuffer> buffers;
    /// How many of them this frame has handed out.
    std::size_t used{};
  };

  /// Per-frame command recording and synchronisation objects.
  struct FrameContext {
    VkCommandBuffer commands{VK_NULL_HANDLE};
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
    VkSemaphore renderFinished{VK_NULL_HANDLE};
    VkFence inFlight{VK_NULL_HANDLE};
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
    /// Command storage, one entry per thread that may record this frame.
    std::vector<RecordSlot> slots;
    /// Secondary buffers the primary will execute, in the order the caller
    /// issued the draws. Recording order is not this order once threads are
    /// involved, which is exactly why the list is kept separately.
    std::vector<VkCommandBuffer> secondaries;
    /// The buffer draws issued one at a time are currently appending to.
    VkCommandBuffer openSecondary{VK_NULL_HANDLE};
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
  /**
   * @brief Begin a secondary command buffer from @p slot, ready to draw into.
   *
   * A secondary buffer inherits the render pass and framebuffer and nothing
   * else, so the viewport, the pipeline and the descriptor set are recorded
   * into every one of them. That per-buffer cost is what sets the threshold
   * below which splitting a run is not worth it.
   */
  VkCommandBuffer beginSecondary(RecordSlot &slot);
  /// The buffer sequential draws append to, opening one if none is open.
  VkCommandBuffer sequentialSecondary();
  /// End the sequentially recorded buffer, if one is open. Anything that
  /// changes bound state calls this, since a secondary cannot be re-entered.
  void closeSequentialSecondary();
  /// Record one batch into an already-begun secondary buffer.
  void recordBatch(VkCommandBuffer commands, const GlyphBatch &batch) const;
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

  /**
   * @brief Threads a frame's recording is spread across.
   *
   * Sized from the hardware at construction and capped: past a handful of
   * threads the per-buffer setup a split adds grows faster than the recording
   * it saves, and the pools have to be allocated for every one of them whether
   * a frame turns out to need them or not.
   */
  static constexpr std::uint32_t maxRecordingThreads = 4;
  WorkerPool recorders;
  /**
   * @brief GLEDITOR_RECORD_THREADS named a count, so take it as an instruction
   *        rather than a ceiling.
   *
   * With it set the device stops choosing and always splits a run large enough
   * to be worth splitting. That is what makes both paths reachable on demand:
   * left to itself the device picks one and the other is never exercised, so
   * the comparison that proves they draw the same frame could not be run.
   */
  bool recordingThreadsForced{};

  /// What each strategy cost in the current measurement, counted per draw so
  /// that a document changing size mid-comparison does not decide it.
  RecordingCost sequentialCost;
  RecordingCost parallelCost;
  /// Frames since the current comparison began. Below recordingProbeFrames the
  /// device is still measuring; at recordingProbeInterval it starts again.
  std::uint32_t recordingProbeFrame{};
  /// The choice last reported through the diagnostic sink, so that settling on
  /// one strategy says so once rather than every frame.
  std::optional<bool> reportedRecordingChoice;
  /// Record the whole run on one thread, as it would be without a split.
  void recordSequentially(std::span<const GlyphBatch> batches);
  /// Record the run across the worker pool.
  void recordInParallel(std::span<const GlyphBatch> batches,
                        std::size_t chunks);

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
