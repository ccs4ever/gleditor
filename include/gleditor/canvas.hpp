/**
 * @file canvas.hpp
 * @brief Rectangles and text, drawn through the glyph pipeline.
 *
 * A document is not the only thing worth drawing. A program built on this
 * library may want a panel, a label, a graph of its own -- things that are not
 * paginated text and have no business being a Doc, but which need exactly what
 * a Doc's draw path already does: pack glyphs into the shared atlas, build one
 * instance per quad and hand the run to the device.
 *
 * That capability existed here before this file did, spelled out inside the
 * notification overlay, which is where it was discovered that a solid
 * rectangle is just a quad whose foreground and background are the same colour
 * -- the fragment stage blends between them, so whatever the atlas holds stops
 * mattering. No second pipeline, no blend state, no reserved blank texel. This
 * is that, made available on its own.
 *
 * A canvas is geometry and nothing else. It does not know where it is: the
 * transform is given at draw time, so the same canvas can be an overlay in
 * window pixels or an object standing in the world among the documents.
 */
#ifndef GLEDITOR_CANVAS_H
#define GLEDITOR_CANVAS_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace render {
class RenderDevice;
}

namespace gleditor {

struct ImageResource;

/// Size of a laid-out piece of text, in the pixel space a canvas draws in.
struct TextMetrics {
  float width{};
  float height{};
};

/**
 * @class Canvas
 * @brief A rebuildable batch of coloured rectangles and text.
 *
 * Coordinates are pixels with Y running up, which is what the vertex stage's
 * corner offsets and the bottom-up glyph atlas both expect. What one pixel
 * turns out to be on screen is decided by the transform passed to draw().
 *
 * Every method must be called on the render thread: laying text out
 * rasterises glyphs into the shared cache, and committing writes device
 * memory.
 */
class Canvas {
public:
  /**
   * @param aDevice   Device the vertex storage lives on. Not owned; must
   *        outlive the canvas.
   * @param aFontName Pango font description text is laid out with.
   * @param initialRows Quads the storage starts out with. It grows on demand.
   */
  Canvas(render::RenderDevice *aDevice, std::string aFontName,
         std::uint32_t initialRows = 1U << 12U);
  ~Canvas();

  Canvas(const Canvas &)            = delete;
  Canvas &operator=(const Canvas &) = delete;
  Canvas(Canvas &&)                 = delete;
  Canvas &operator=(Canvas &&)      = delete;

  /**
   * @brief Build the pipeline this canvas draws with, from the document one.
   *
   * @param depthTest Off by default, which is what an overlay wants: it is on
   *        top by virtue of being drawn last, and a Z value could not achieve
   *        the same on both the OpenGL and Vulkan depth conventions. A canvas
   *        standing among the documents in the world wants it on.
   */
  void createPipeline(const render::PipelineDesc &documentDesc,
                      bool depthTest = false);

  /// Throw away the geometry built so far, keeping the storage. The start of
  /// rebuilding a canvas whose contents changed.
  void clear();

  /**
   * @brief Identity written into every primitive added from here on.
   *
   * This is what a picking query reports for the pixels these primitives
   * cover, so it is how a program finds out that one of its own drawings was
   * clicked. @p kind is one of render::tagKindOverlay and friends;
   * render::tagKindOverlay is what anything that is not a document page or a
   * glyph should use. @p index lands in the cluster field, so a caller can
   * tell one primitive from another.
   *
   * Kind zero is not usable: the picking attachment reports zero for pixels
   * nothing was drawn at, so a primitive of kind zero would come back as empty
   * space. Primitives added before any call to this are overlays.
   *
   * The document and page a canvas belongs to are not settable per primitive.
   * They are the same for everything one draw covers, so they are said once,
   * in @ref setIdentity, and cost the instances nothing.
   */
  void setTag(std::uint32_t kind, std::uint32_t index = 0);

  /**
   * @brief The document and page every primitive of this canvas belongs to.
   *
   * Both default to zero, which is what a canvas drawing something that is not
   * part of a document wants. Passed to the draw, not written into the
   * primitives.
   */
  void setIdentity(std::uint32_t docIndex, std::uint32_t pageIndex);

  /// A solid rectangle, given its bottom left corner and its size.
  void addRect(float left, float bottom, float width, float height,
               std::uint32_t colour);

  /**
   * @brief A line segment, drawn as a rotated rectangle.
   *
   * Present because a graph of anything needs edges, and a quad is the only
   * primitive the glyph pipeline draws. The quad is axis-aligned, so this is
   * exact for horizontal and vertical segments and an approximation --
   * a rectangle covering the segment's bounding box's diagonal -- otherwise.
   */
  void addLine(float fromX, float fromY, float toX, float toY, float thickness,
               std::uint32_t colour);

  /**
   * @brief An image quad from an ImageResource.
   */
  void addImage(float left, float bottom, float width, float height,
                const ImageResource &image, std::uint32_t tint = 0xFFFFFFFFU);

  /**
   * @brief An image quad specifying explicit atlas texel coordinates and layer.
   */
  void addImage(float left, float bottom, float width, float height, int layer,
                float texX, float texY, float texW, float texH,
                std::uint32_t tint = 0xFFFFFFFFU);

  /**
   * @brief Text, with the top left of the block at (@p left, @p top).
   *
   * @param background Colour behind the glyphs. Should match whatever the text
   *        is drawn over: the fragment stage blends the two by atlas coverage,
   *        so a mismatched background shows as a halo around every glyph.
   * @return The size of the block, so a caller can lay the next thing out
   *         against it.
   */
  TextMetrics addText(RenderState &state, float left, float top,
                      std::string_view utf8, std::uint32_t colour,
                      std::uint32_t background);

  /// Size @p utf8 would take, without drawing it or touching the glyph cache.
  [[nodiscard]] TextMetrics measureText(std::string_view utf8) const;

  /// Longest a line of text may get before it is ellipsised. Zero, the
  /// default, does not wrap or ellipsise at all.
  void setTextWidthLimit(int pixels) { textWidthLimit = pixels; }

  /// Upload the geometry built since the last clear(). Nothing is drawn until
  /// this has been called.
  void commit();

  [[nodiscard]] bool empty() const { return 0 == committedInstances; }

  /**
   * @brief Draw the committed geometry.
   * @param transform Whatever maps this canvas's pixels to clip space:
   *        an orthographic projection for an overlay, projection * view *
   *        model for something standing in the world.
   */
  void draw(RenderState &state, const glm::mat4 &transform,
            float opacity = 1.0F) const;

private:
  /// A row of the instance buffer. Declared as the document's, because the
  /// pipeline the canvas borrows is described by Doc::vertexLayout().
  void pushQuad(float centreX, float centreY, float width, float height,
                std::uint32_t foreground, std::uint32_t background,
                std::uint32_t layer, float texX, float texY, bool solid);

  render::RenderDevice *device;
  std::string fontName;
  std::unique_ptr<BufferPool> pool;
  render::PipelineHandle pipeline{};
  BufferPool::Allocation backing{};
  std::uint32_t committedInstances{};
  int textWidthLimit{};
  std::uint32_t tagKind;
  std::uint32_t tagIndex{};
  /// Document and page, with no kind: the base every primitive's identity is
  /// built on, handed to the draw rather than to each instance.
  std::uint32_t identity{};
  /// Geometry built since clear(), as raw bytes: the row type belongs to the
  /// document model and is not worth dragging into this header.
  std::vector<std::byte> rows;
  std::uint32_t pendingInstances{};
};

} // namespace gleditor

#endif // GLEDITOR_CANVAS_H
