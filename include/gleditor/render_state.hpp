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

#include <memory>
#include <vector>

#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/types.hpp>

namespace render {
class RenderDevice;
}

class Doc;

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

  render::RenderDevice *device;          ///< Active graphics device.
  GlyphCache glyphCache;                 ///< Shared glyph atlas.
  render::PipelineHandle glyphPipeline{}; ///< Pipeline all documents draw with.
  std::vector<std::shared_ptr<Doc>> docs; ///< Open documents.
};

#endif // GLEDITOR_RENDER_STATE_H
// vi: set sw=2 sts=2 ts=2 et:
