/**
 * @file canvas.cpp
 * @brief Implementation of the rectangle-and-text drawing surface.
 */
#include <gleditor/canvas.hpp> // IWYU pragma: associated

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include <pango/pango-types.h>
#include <pangomm/cairofontmap.h>
#include <pangomm/fontdescription.h>
#include <pangomm/layout.h>
#include <pangomm/layoutiter.h>
#include <pangomm/rectangle.h>

#include <gleditor/doc.hpp>
#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render_state.hpp>

namespace {

/// Length in bytes of the UTF-8 sequence introduced by @p lead. Continuation
/// and invalid bytes report 1 so that callers always make forward progress.
int utf8SequenceLength(const char lead) {
  const auto byte = static_cast<unsigned char>(lead);
  if (byte < 0x80U) {
    return 1;
  }
  if ((byte & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((byte & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((byte & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

double toPixels(const int pangoUnits) {
  return static_cast<double>(pangoUnits) / PANGO_SCALE;
}

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

namespace gleditor {

namespace {

/// Build the layout a canvas measures and draws text with, so that measuring
/// and drawing cannot disagree about wrapping, ellipsis or font.
Glib::RefPtr<Pango::Layout> makeLayout(const std::string &fontName,
                                       const std::string_view utf8,
                                       const int widthLimit,
                                       Glib::RefPtr<Pango::Context> &ctxOut) {
  const auto fontDesc = Pango::FontDescription(fontName);
  ctxOut              = Pango::CairoFontMap::get_default()->create_context();
  ctxOut->set_font_description(fontDesc);

  auto layout = Pango::Layout::create(ctxOut);
  layout->set_font_description(fontDesc);
  layout->set_single_paragraph_mode(true);
  if (widthLimit > 0) {
    layout->set_width(widthLimit * PANGO_SCALE);
    layout->set_ellipsize(Pango::EllipsizeMode::END);
  }
  layout->set_text(std::string{utf8});
  return layout;
}

} // namespace

Canvas::Canvas(render::RenderDevice *const aDevice, std::string aFontName,
               const std::uint32_t initialRows)
    : device(aDevice), fontName(std::move(aFontName)),
      pool(std::make_unique<BufferPool>(aDevice, sizeof(Doc::VBORow),
                                        initialRows)),
      tagKind(render::tagKindOverlay) {}

Canvas::~Canvas() = default;

void Canvas::createPipeline(const render::PipelineDesc &documentDesc,
                            const bool depthTest) {
  render::PipelineDesc desc = documentDesc;
  desc.name                 = "canvas";
  desc.depthTest            = depthTest;
  pipeline                  = device->createPipeline(desc);
}

void Canvas::clear() {
  rows.clear();
  pendingInstances = 0;
}

void Canvas::setTag(const std::uint32_t kind, const std::uint32_t index) {
  tagKind  = kind;
  tagIndex = index;
}

void Canvas::setIdentity(const std::uint32_t docIndex,
                         const std::uint32_t pageIndex) {
  identity = render::packTagIdentity(0, docIndex, pageIndex);
}

void Canvas::pushQuad(const float centreX, const float centreY,
                      const float width, const float height,
                      const std::uint32_t foreground,
                      const std::uint32_t background,
                      const std::uint32_t layer, const float texX,
                      const float texY, const bool solid) {
  // The width and height fields the vertex stage unpacks are 12 bits each, so
  // a quad larger than this cannot be described. Clamping rather than asserting
  // because the sizes come from whatever a caller is drawing, and a panel too
  // big is a visual mistake where a failed assertion is a crash.
  const auto clamp = [](const float value) {
    return static_cast<unsigned int>(
        std::clamp(value, 0.0F, static_cast<float>(Doc::VBORow::maxQuadExtent)));
  };

  const Doc::VBORow row{
      {centreX, centreY},
      Doc::VBORow::ink(foreground, Doc::VBORow::onPaper, solid),
      Doc::VBORow::atlasAt(static_cast<unsigned int>(texX),
                           static_cast<unsigned int>(texY)),
      Doc::VBORow::box(static_cast<unsigned char>(layer), clamp(width),
                       clamp(height), tagKind),
      Doc::VBORow::paperAt(background, tagIndex)};

  const auto *const bytes = reinterpret_cast<const std::byte *>(&row);
  rows.insert(rows.end(), bytes, bytes + sizeof(row));
  pendingInstances++;
}

void Canvas::addRect(const float left, const float bottom, const float width,
                     const float height, const std::uint32_t colour) {
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  // Solid, so the fragment stage fills it with this colour and never samples
  // the atlas.
  pushQuad(left + (width / 2.0F), bottom + (height / 2.0F), width, height,
           colour, colour, 0, 0.0F, 0.0F, true);
}

void Canvas::addLine(const float fromX, const float fromY, const float toX,
                     const float toY, const float thickness,
                     const std::uint32_t colour) {
  const auto minX = std::min(fromX, toX);
  const auto minY = std::min(fromY, toY);
  const auto spanX = std::abs(toX - fromX);
  const auto spanY = std::abs(toY - fromY);
  // A segment with no extent in one axis is exactly a thin rectangle; one with
  // extent in both is covered by its bounding box, which is the closest an
  // axis-aligned quad gets.
  addRect(minX - (thickness / 2.0F), minY - (thickness / 2.0F),
          std::max(spanX, thickness), std::max(spanY, thickness), colour);
}

TextMetrics Canvas::measureText(const std::string_view utf8) const {
  Glib::RefPtr<Pango::Context> ctx;
  const auto layout = makeLayout(fontName, utf8, textWidthLimit, ctx);
  int width  = 0;
  int height = 0;
  layout->get_pixel_size(width, height);
  return {static_cast<float>(width), static_cast<float>(height)};
}

TextMetrics Canvas::addText(RenderState &state, const float left,
                            const float top, const std::string_view utf8,
                            const std::uint32_t colour,
                            const std::uint32_t background) {
  Glib::RefPtr<Pango::Context> ctx;
  const auto layout = makeLayout(fontName, utf8, textWidthLimit, ctx);

  const auto font = ctx->load_font(Pango::FontDescription(fontName));
  if (!font) {
    // No usable font means no glyphs. Callers have already been told whatever
    // the text was going to say by other means; failing to draw it is not
    // worth an exception out of a frame.
    return {};
  }

  int textWidth  = 0;
  int textHeight = 0;
  layout->get_pixel_size(textWidth, textHeight);
  if (0 >= textWidth || 0 >= textHeight) {
    return {};
  }

  const auto raw = layout->get_text().raw();

  // Cluster boundaries first: positioning a cluster needs the extents of the
  // one the iterator is on and the byte index of the next, which is one step
  // ahead of it.
  struct Cluster {
    int start;
    Pango::Rectangle logical;
  };
  std::vector<Cluster> clusters;
  {
    auto iter = layout->get_iter();
    while (true) {
      Pango::Rectangle ink;
      Pango::Rectangle logical;
      iter.get_cluster_extents(ink, logical);
      clusters.push_back(Cluster{iter.get_index(), logical});
      if (!iter.next_cluster()) {
        break;
      }
    }
  }

  for (std::size_t i = 0; i < clusters.size(); i++) {
    const auto &cluster = clusters[i];
    const int end       = i + 1 < clusters.size()
                              ? clusters[i + 1].start
                              : static_cast<int>(raw.size());
    if (end <= cluster.start || cluster.start >= static_cast<int>(raw.size())) {
      continue;
    }
    // The leading codepoint of the cluster, cut on a sequence boundary so
    // Pango is never handed half of a multi-byte character.
    const int length =
        std::min(end - cluster.start, utf8SequenceLength(raw[cluster.start]));
    const std::string_view chr(raw.data() + cluster.start,
                               static_cast<std::size_t>(length));

    const auto glyph  = state.glyphCache.put(chr, font);
    const auto width  = static_cast<float>(static_cast<int>(glyph.dims.width));
    const auto height = static_cast<float>(static_cast<int>(glyph.dims.height));
    if (0.0F == width || 0.0F == height) {
      continue;
    }

    // Pango measures down from the top of the block; this space runs up, hence
    // the subtraction.
    const auto glyphLeft =
        left + static_cast<float>(toPixels(cluster.logical.get_x()));
    const auto glyphTop =
        top - static_cast<float>(toPixels(cluster.logical.get_y()));

    pushQuad(glyphLeft + (width / 2.0F), glyphTop - (height / 2.0F), width,
             height, colour, background, static_cast<std::uint32_t>(glyph.layer),
             glyph.texCoords.topLeft.x, glyph.texCoords.topLeft.y, false);
  }

  return {static_cast<float>(textWidth), static_cast<float>(textHeight)};
}

void Canvas::commit() {
  if (!backing.empty()) {
    pool->release(backing);
    backing = {};
  }
  committedInstances = pendingInstances;
  if (0 == committedInstances) {
    return;
  }
  backing = pool->reserve(committedInstances);
  pool->write(backing, 0, std::span<const std::byte>(rows));
}

void Canvas::draw(RenderState &state, const glm::mat4 &transform,
                  const float opacity) const {
  if (0 == committedInstances || !pipeline.valid()) {
    return;
  }
  state.device->bindPipeline(pipeline);
  state.device->bindGlyphTexture(state.glyphCache.textureHandle());
  const render::DrawUniforms uniforms{toArray(transform), opacity, identity};
  state.device->drawGlyphs(uniforms, pool->buffer(), pool->byteOffset(backing),
                           committedInstances);
}

} // namespace gleditor

// vi: set sw=2 sts=2 ts=2 et:
