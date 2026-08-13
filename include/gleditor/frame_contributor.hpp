/**
 * @file frame_contributor.hpp
 * @brief Drawing something the library has never heard of, inside its frame.
 *
 * The render loop knows how to draw documents, a caret and notifications, and
 * that is the whole list. A program wanting anything else on screen -- a
 * diagram, a map, a panel of its own, connections drawn between two documents
 * -- previously had no way in that did not mean editing the loop.
 *
 * A contributor is that way in. It is called once a frame with the frame's
 * camera and the render state, and draws whatever it likes, usually through a
 * Canvas it owns. What it draws is its own business: the library hands over
 * the transform and the device and has no opinion about what appears.
 */
#ifndef GLEDITOR_FRAME_CONTRIBUTOR_H
#define GLEDITOR_FRAME_CONTRIBUTOR_H

#include <choreograph/Choreograph.h>
#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/render/types.hpp>

struct RenderState;

namespace ch = choreograph;

namespace render {
class RenderDevice;
}

namespace gleditor {

/// What a contributor is given when the frame asks it to draw.
struct FrameContext {
  RenderState &state;
  /// projection * view for this frame. A contributor drawing in the world
  /// multiplies its own model matrix onto this; one drawing in window pixels
  /// ignores it and builds an orthographic projection from the size below.
  const glm::mat4 &viewProjection;
  int screenWidth{};
  int screenHeight{};
  /**
   * @brief The frame's animation timeline.
   *
   * Here because a contributor that only draws is the smaller half of what one
   * is for: a program may want to move the documents themselves -- to bring
   * two that are connected next to each other, say -- and moving them abruptly
   * is not the same thing at all. Stepped once per frame by the render loop,
   * before anything reads a position, so anything started here begins on the
   * next frame rather than partway through this one.
   */
  ch::Timeline &timeline;
};

/**
 * @brief Draws into the frame after the documents and before the
 *        notifications.
 *
 * Called on the render thread, every frame, in the order registered.
 * Contributors are held as bare pointers and are not owned; one must outlive
 * the renderer or remove itself first.
 */
class FrameContributor {
public:
  FrameContributor()          = default;
  virtual ~FrameContributor() = default;

  FrameContributor(const FrameContributor &)            = delete;
  FrameContributor &operator=(const FrameContributor &) = delete;
  FrameContributor(FrameContributor &&)                 = delete;
  FrameContributor &operator=(FrameContributor &&)      = delete;

  /**
   * @brief Called once, before the first frame, when the device exists.
   *
   * A contributor that draws anything needs a pipeline, and a pipeline needs
   * both a device and the description the documents are drawn with -- neither
   * of which exists when a program registers itself, because the device is
   * created on the render thread. This is where a Canvas gets built.
   *
   * Only contributors registered before the render thread starts are called.
   */
  virtual void deviceReady([[maybe_unused]] render::RenderDevice &device,
                           [[maybe_unused]] const render::PipelineDesc
                               &documentPipeline) {}

  virtual void drawFrame(FrameContext &ctx) = 0;

  /**
   * @brief Whether this contributor has work outstanding.
   *
   * The render loop treats a frame as settled only when nothing is pending,
   * and a settled frame is what a screenshot waits for and what a benchmark
   * counts. A contributor still animating, or still loading what it means to
   * draw, says so here; one that is simply drawing the same thing every frame
   * leaves it alone.
   */
  [[nodiscard]] virtual bool busy() const { return false; }
};

} // namespace gleditor

#endif // GLEDITOR_FRAME_CONTRIBUTOR_H
// vi: set sw=2 sts=2 ts=2 et:
