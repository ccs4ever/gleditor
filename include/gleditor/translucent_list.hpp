/**
 * @file translucent_list.hpp
 * @brief Everything translucent in a frame, drawn back to front as one list.
 *
 * Blending is order dependent: a faded thing is mixed with whatever is already
 * in the target, so what is behind it has to be there first. Each kind of
 * translucent draw used to sort only among its own kind -- documents by their
 * world z, beams not at all -- and draw with depth write on. Where a faded
 * page, a faded sheet and a beam overlap, that put a beam on top of a page it
 * passes behind, and a page drawn first cut a hole in a sheet behind it.
 *
 * So translucent draws of every kind go into one list, sorted by their depth
 * from the camera, and are drawn after everything opaque, depth tested and not
 * written. A beam is a long thing that can be in front of a sheet at one end
 * and behind it at the other, which no single place in the order is right
 * for, so the list cuts it where it crosses a sheet and sorts the pieces.
 *
 * Generic: a sheet is any flat glyph batch, a beam any Beams batch.
 */
#ifndef GLEDITOR_TRANSLUCENT_LIST_H
#define GLEDITOR_TRANSLUCENT_LIST_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace gleditor {

/**
 * @class TranslucentList
 * @brief One frame's translucent draws, sorted by camera depth and drawn.
 *
 * Filled afresh every frame -- clear(), add, draw() -- and drawn after the
 * opaque draws, whose depth it tests against.
 *
 * Depth is the clip-space w of a draw's reference point, which under a
 * perspective camera is its distance along the view direction; under an
 * orthographic camera, where every w is one, clip z decides. A sheet's
 * reference point is the origin of its own space, which is its centre for a
 * page; a beam piece's is its midpoint. Sorting by one point per draw is the
 * painter's approximation: two sheets that interpenetrate, or a sheet tilted
 * steeply across another, can still be ordered wrongly. Cutting beams is what
 * keeps the commonest overlap -- a beam passing through the sheets it joins --
 * right.
 *
 * Every method must be called on the render thread.
 */
class TranslucentList {
public:
  /// What the last draw() did, for tests and the frame's statistics.
  struct Stats {
    std::size_t sheets{};
    /// Beam rows as given, before cutting.
    std::size_t beams{};
    /// Pieces drawn after cutting; equal to @ref beams when nothing was cut.
    std::size_t beamPieces{};
    /// Device draw calls issued.
    std::size_t draws{};
  };

  /**
   * @param aDevice Device the cut beams are uploaded to. Not owned; must
   *        outlive this.
   * @param initialRows Beam pieces the storage starts out with. It grows on
   *        demand.
   */
  explicit TranslucentList(render::RenderDevice *aDevice,
                           std::uint32_t initialRows = 256);
  ~TranslucentList();

  TranslucentList(const TranslucentList &)            = delete;
  TranslucentList &operator=(const TranslucentList &) = delete;
  TranslucentList(TranslucentList &&)                 = delete;
  TranslucentList &operator=(TranslucentList &&)      = delete;

  /// Forget everything added. Keeps its storage, so a frame costs no
  /// allocation once the list has seen its largest frame.
  TranslucentList *clear();

  /**
   * @brief Add a flat glyph batch.
   *
   * The sheet is the plane z = 0 of the space @p batch's matrix maps from,
   * which for a page is its paper. Beams are cut where they cross that plane,
   * inside the sheet or beyond its edge: a cut where none was needed costs a
   * draw and changes no pixel, and the batch does not say where its edges are.
   *
   * @param pipeline Bound to draw it; normally one with depth write off.
   */
  TranslucentList *addSheet(render::PipelineHandle pipeline,
                            const render::GlyphBatch &batch);

  /**
   * @brief Add the committed beams of @p beams.
   *
   * Drawn with the batch's own pipeline, which should test depth and not write
   * it; see Beams::createPipeline. @p beams must stay alive, and must not be
   * committed again, until draw() returns: the pieces are cut from what it
   * holds then.
   *
   * @param transform projection * view * model for the beams, as
   *        Beams::draw() takes it.
   */
  TranslucentList *addBeams(const Beams &beams, const glm::mat4 &transform,
                            float opacity, std::uint32_t identity);

  [[nodiscard]] bool empty() const {
    return sheets.empty() && beamSources.empty();
  }

  /**
   * @brief Sort, cut and draw everything added.
   *
   * Binds pipelines as it goes; the caller rebinds whatever it draws next.
   * The list is left as it was, so drawing twice draws the same thing.
   */
  void draw(RenderState &state);

  [[nodiscard]] const Stats &lastDraw() const { return stats; }

private:
  struct Sheet {
    render::PipelineHandle pipeline;
    render::GlyphBatch batch;
  };

  struct BeamSource {
    const Beams *beams{};
    glm::mat4 transform{1.0F};
    float opacity{1.0F};
    std::uint32_t identity{};
  };

  /// One entry of the sorted order: a sheet, or a run of beam pieces.
  struct Item {
    double w{};
    double z{};
    /// Index into sheets, or into pieces when @ref beam is set.
    std::uint32_t index{};
    bool beam{};
  };

  /// A beam piece waiting to be uploaded, and whose it is.
  struct Piece {
    Beams::Row row;
    std::uint32_t source{};
  };

  /// Cut every beam where it crosses a sheet's plane and append the pieces.
  void cutBeams();
  /// Upload the pieces in the order @ref order puts them.
  void uploadPieces();

  render::RenderDevice *device;
  std::unique_ptr<BufferPool> pool;
  BufferPool::Allocation backing{};

  std::vector<Sheet> sheets;
  std::vector<BeamSource> beamSources;

  // Per-draw scratch, kept between frames for its capacity.
  std::vector<glm::dmat4> sheetToLocal;
  std::vector<Piece> pieces;
  std::vector<Item> order;
  std::vector<Beams::Row> upload;
  /// Where each piece landed in @ref upload, by piece index.
  std::vector<std::uint32_t> uploadedAt;
  std::vector<double> cuts;
  std::vector<render::GlyphBatch> runBatches;

  Stats stats;
};

} // namespace gleditor

#endif // GLEDITOR_TRANSLUCENT_LIST_H
