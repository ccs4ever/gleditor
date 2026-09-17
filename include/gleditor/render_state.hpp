/**
 * @file render_state.hpp
 * @brief State owned exclusively by the render thread.
 *
 * Replaces the old GLState, which carried OpenGL program and uniform location
 * tables. Those are now the device's business; what remains is the glyph
 * cache, the pipeline handle and the open documents.
 */
#ifndef GLEDITOR_RENDER_STATE_H
#define GLEDITOR_RENDER_STATE_H

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/types.hpp>

namespace render {
class RenderDevice;
}

class Doc;
class Caret;

/**
 * @struct RenderState
 * @brief Per-render-thread rendering context.
 */
struct RenderState {
  /**
   * @param aDevice Device every resource in this state belongs to. Not owned;
   *        must outlive the state.
   */
  explicit RenderState(render::RenderDevice *aDevice)
      : device(aDevice), glyphCache(aDevice) {}

  render::RenderDevice *device;           ///< Active graphics device.
  gleditor::GlyphCache glyphCache;        ///< Shared glyph atlas.
  render::PipelineHandle glyphPipeline{}; ///< Pipeline all documents draw with.
  std::vector<std::shared_ptr<Doc>> docs; ///< Open documents.
  /// Stable meanings aligned with docs; raw GPU tags retain only their index.
  std::vector<std::shared_ptr<const render::PickSemanticTarget>> pickTargets;
  render::PickScene overlayPickScene;
  std::uint32_t nextOverlayPickScope{1};

  void beginPickScene() { overlayPickScene.overlays.clear(); }
  [[nodiscard]] std::uint32_t allocateOverlayPickScope() {
    constexpr auto scopeCount = (1U << render::tagDocBits) - 1U;
    const auto scope          = nextOverlayPickScope++;
    return ((scope - 1U) % scopeCount) + 1U;
  }
  void
  bindOverlayPick(const render::PickingTag &tag,
                  std::shared_ptr<const render::PickSemanticTarget> target) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(tag.kind) << 60U) |
        (static_cast<std::uint64_t>(tag.docIndex) << 46U) |
        (static_cast<std::uint64_t>(tag.pageIndex) << 32U) | tag.clusterIndex;
    overlayPickScene.overlays.insert_or_assign(key, std::move(target));
  }
  Caret *caret{nullptr}; ///< Active caret on render thread.
  /**
   * @brief Scratch the frame's page draws are collected into.
   *
   * Handing the device the whole run at once is what lets a backend record it
   * on more than one thread. Kept here rather than built per document so that
   * every page of every document lands in one list -- a per-document list would
   * cap the work available to split at one document's page count -- and reused
   * between frames so that collecting it costs no allocation.
   */
  std::vector<render::GlyphBatch> pageBatches;

  /**
   * @brief When this render loop began.
   *
   * Renderer::renderLoop() sets this at the same point it used to define a
   * local of the same name, right before the first frame -- the reference
   * point its own "[TIMING] First page rendered" / "[TIMING] Complete
   * render settled" lines measure from. Exposed here so a FrameContributor
   * (LinkBeams, reporting how long a beam took to cross the viewport, is
   * the one that exists for) can report elapsed time on the same clock
   * without the renderer having to know what it is timing -- see
   * design/priority-page-building.md's Stage 4. Defaulted to "now" so a
   * RenderState built outside the normal render loop (every test that
   * constructs one directly) still has a real value rather than the epoch.
   */
  std::chrono::steady_clock::time_point loopStart{
      std::chrono::steady_clock::now()};
};

#endif // GLEDITOR_RENDER_STATE_H
