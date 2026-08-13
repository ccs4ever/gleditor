/**
 * @file beams.hpp
 * @brief Ribbons drawn between two places in the world.
 *
 * The glyph pipeline draws quads that are axis-aligned by construction: a
 * centre, a width and a height, with the corners taken from the vertex index.
 * A line from one place to another is not one of those, and a program that
 * wanted to draw a connection between two things on screen had nothing to use.
 *
 * So this is a second pipeline with a record of its own: one instance per
 * beam, holding its two ends. The corners come from the vertex index as
 * before, but from the *ends* rather than from a centre, and the width is
 * taken perpendicular to the run within the plane the pages lie in -- so a
 * beam foreshortens with what it connects rather than facing the camera like
 * a label.
 *
 * Nothing here knows what a beam means. A beam has two ends, a colour and a
 * tag; whether it stands for a link, a reference, a dependency or a wire is
 * the caller's business, and the tag is how the caller recognises its own
 * beam when somebody clicks one.
 */
#ifndef GLEDITOR_BEAMS_H
#define GLEDITOR_BEAMS_H

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace gleditor {

/**
 * @class Beams
 * @brief A rebuildable batch of beams.
 *
 * Built afresh whenever what it shows changes -- clear(), add() the beams,
 * commit() -- which is the same shape as Canvas and for the same reason: the
 * set is small, and rebuilding it is simpler to reason about than editing it.
 *
 * Every method must be called on the render thread.
 */
class Beams {
public:
  /**
   * @brief One beam, as the vertex stage reads it.
   *
   * Field order and offsets must stay in step with layout() and with the
   * attribute locations in assets/shaders/beam.vert.glsl.
   */
  struct Row {
    std::array<float, 3> from{};
    float width{};
    std::array<float, 3> to{};
    std::uint32_t colour{};
    std::uint32_t tag{};
  };

  /// Per-instance layout describing Row to the device.
  [[nodiscard]] static render::VertexLayout layout();

  /**
   * @param aDevice Device the vertex storage lives on. Not owned; must
   *        outlive this.
   * @param initialRows Beams the storage starts out with. It grows on demand.
   */
  explicit Beams(render::RenderDevice *aDevice,
                 std::uint32_t initialRows = 256);
  ~Beams();

  Beams(const Beams &)            = delete;
  Beams &operator=(const Beams &) = delete;
  Beams(Beams &&)                 = delete;
  Beams &operator=(Beams &&)      = delete;

  /**
   * @brief Build the pipeline.
   *
   * @param assetDir Where beam.vert.glsl and beam.frag.glsl are, which is the
   *        same directory the document's shaders came from.
   * @param spirvDir Precompiled SPIR-V for backends that need it.
   * @param depthTest Whether beams are occluded by what they run between.
   *        Usually yes: a beam that passed in front of the page it points at
   *        would be a beam nothing could be behind.
   */
  void createPipeline(const std::string &assetDir, const std::string &spirvDir,
                      bool depthTest = true);

  /// Whether there is a pipeline to draw with. False when the shaders could
  /// not be read, which is not fatal: the rest of the frame still draws.
  [[nodiscard]] bool ready() const { return pipeline.valid(); }

  void clear();

  /**
   * @brief Add a beam.
   * @param width In world units, across the beam.
   * @param colour Packed RGBA8; the alpha is the beam's own, multiplied by the
   *        opacity given to draw().
   * @param tag What a picking query reports for this beam, so the caller can
   *        tell which of its own beams was clicked.
   */
  void add(const glm::vec3 &from, const glm::vec3 &to, float width,
           std::uint32_t colour, std::uint32_t tag);

  /// Hand what has been added to the device. Nothing is drawn until this.
  void commit();

  [[nodiscard]] std::size_t pending() const { return rows.size(); }
  [[nodiscard]] std::uint32_t committed() const { return committedRows; }

  /**
   * @brief Draw the committed beams.
   * @param transform projection * view, with any model transform already on
   *        it: beam ends are given in the space this maps from.
   * @param identity What a picking query reports alongside a beam's tag,
   *        built with render::packTagIdentity and a kind of zero.
   */
  void draw(RenderState &state, const glm::mat4 &transform, float opacity,
            std::uint32_t identity) const;

private:
  render::RenderDevice *device;
  std::unique_ptr<BufferPool> pool;
  render::PipelineHandle pipeline{};
  BufferPool::Allocation backing{};
  std::uint32_t committedRows{};
  std::vector<Row> rows;
};

} // namespace gleditor

#endif // GLEDITOR_BEAMS_H
// vi: set sw=2 sts=2 ts=2 et:
