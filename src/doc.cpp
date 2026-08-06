#include <algorithm>                   // for min, max
#include <cmath>                       // for ceil
#include <cstddef>                     // for byte
#include <format>                      // for format
#include <gleditor/doc.hpp>            // IWYU pragma: associated
#include <gleditor/render/device.hpp>  // for RenderDevice
#include <gleditor/render_state.hpp>   // for RenderState
#include <gleditor/renderer.hpp>       // for Renderer, RendererRef
#include <glm/detail/qualifier.hpp>    // for qualifier
#include <glm/ext/matrix_float4x4.hpp> // for mat4
#include <glm/ext/vector_float3.hpp>   // for vec3
#include <glm/gtc/type_ptr.hpp>
#include <iostream>               // for basic_ostream, operator<<
#include <memory>                 // for __shared_ptr_access, shared...
#include <pangomm/cairofontmap.h> // for CairoFontMap
#include <span>                   // for span
#include <stdexcept>              // for logic_error
#include <string>                 // for char_traits, basic_string
#include <string_view>            // for string_view
#include <sys/types.h>            // for uint
#include <utility>                // for move
#include <vector>                 // for vector

#include "glibmm/convert.h"              // for get_charset
#include "glibmm/fileutils.h"            // for file_get_contents
#include "glibmm/refptr.h"               // for RefPtr
#include "glibmm/ustring.h"              // for ustring, operator==, UStrin...
#include "pango/pango-layout.h"          // for pango_layout_set_text
#include "pango/pango-types.h"           // for PANGO_SCALE
#include "pangomm/attributes.h"          // for AttrFontDesc, Attribute
#include "pangomm/attrlist.h"            // for AttrList
#include "pangomm/fontdescription.h"     // for FontDescription
#include "pangomm/layout.h"              // for Layout, EllipsizeMode
#include "pangomm/layoutiter.h"          // for LayoutIter
#include "pangomm/layoutline.h"          // for LayoutLine
#include "pangomm/rectangle.h"           // for Rectangle
#include <gleditor/drawable.hpp>         // for Drawable
#include <gleditor/glyphcache/cache.hpp> // for GlyphCache
#include <gleditor/glyphcache/types.hpp> // for TextureCoords, PointF, Rect
#include <glm/gtx/string_cast.hpp>

namespace {

/// Rows the document's vertex buffer starts out with. It grows on demand, so
/// this only needs to cover a first page or two without a reallocation.
constexpr std::uint32_t initialPoolRows = 1U << 16U;

/// Unpack the layer/width/height triple written by
/// Doc::VBORow::layerWidthHeight. The field widths must stay in step with that
/// function and with unpackLayerWH() in assets/shaders/glyph.vert.glsl.
[[maybe_unused]] glm::vec3 lwh(const uint packed3DDims) {
  return {packed3DDims >> static_cast<uint>(28),
          packed3DDims >> static_cast<uint>(14) & static_cast<uint>(16383),
          packed3DDims & static_cast<uint>(16383)};
}

/// Margin in layout pixels between the page edge and its text.
constexpr float pageMargin = 24.0F;

/// Convert Pango units to pixels.
double toPixels(const int pangoUnits) {
  return static_cast<double>(pangoUnits) / PANGO_SCALE;
}

/// Number of characters in a UTF-8 range, counting lead bytes.
std::size_t utf8Length(const std::string_view text) {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](const char chr) {
    return 0x80 != (static_cast<unsigned char>(chr) & 0xC0);
  }));
}

/// Byte at @p pos widened without sign extension, for comparing against byte
/// order marks.
unsigned int byteAt(const std::string &str, const std::size_t pos) {
  return static_cast<unsigned char>(str[pos]);
}

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

/// View a row vector as the raw bytes the buffer pool wants.
std::span<const std::byte> asBytes(const std::vector<Doc::VBORow> &rows) {
  return {reinterpret_cast<const std::byte *>(rows.data()),
          rows.size() * sizeof(Doc::VBORow)};
}

/// Copy a matrix into the flat array the device uniform structs carry.
std::array<float, 16> toArray(const glm::mat4 &mat) {
  std::array<float, 16> out{};
  const auto *src = glm::value_ptr(mat);
  std::copy_n(src, out.size(), out.begin());
  return out;
}

} // namespace

render::VertexLayout Doc::vertexLayout() {
  using render::AttributeType;
  static_assert(sizeof(Doc::VBORow) == 48);

  render::VertexLayout layout;
  layout.stride = sizeof(VBORow);
  layout.attributes = {
      {"position", 0, AttributeType::Float, 3, offsetof(VBORow, pos)},
      {"fgcolor", 1, AttributeType::UnsignedInt, 1, offsetof(VBORow, fg)},
      {"bgcolor", 2, AttributeType::UnsignedInt, 1, offsetof(VBORow, bg)},
      {"texcoord", 3, AttributeType::Float, 2, offsetof(VBORow, texcoord)},
      {"texBox", 4, AttributeType::Float, 2, offsetof(VBORow, texBox)},
      {"layerWH", 5, AttributeType::UnsignedInt, 1, offsetof(VBORow, layer)},
      {"tag", 6, AttributeType::UnsignedInt, 2, offsetof(VBORow, tag)},
  };
  return layout;
}

// The constructor parameters are named with a leading `a` so that the body can
// refer to the members without ambiguity: the members are move-constructed from
// the parameters, which leaves the parameters empty.
Page::Page(std::shared_ptr<Doc> aDoc, RenderState &state, glm::mat4 &model,
           Glib::RefPtr<Pango::Layout> aLayout, const std::uint32_t aTextOffset,
           const std::uint32_t aPageIndex)
    : Drawable(model), doc(std::move(aDoc)), layout(std::move(aLayout)),
      textOffset(aTextOffset), pageIndex(aPageIndex) {
  const auto &layout = this->layout;

  const auto color   = Doc::VBORow::color;
  const auto layerWH = Doc::VBORow::layerWidthHeight;

  // Everything below is in layout pixels, the unit Pango reports positions and
  // sizes in. The page's model matrix scales them to world units, so a glyph's
  // quad and the advance that places it shrink together -- they did not before,
  // and the mismatch drew every glyph on top of its neighbours.
  int textWidthPx  = 0;
  int textHeightPx = 0;
  layout->get_pixel_size(textWidthPx, textHeightPx);
  const auto pageWidth  = static_cast<float>(textWidthPx) + (2 * pageMargin);
  const auto pageHeight = static_cast<float>(textHeightPx) + (2 * pageMargin);

  // The page is centred on its own origin, so that the model matrix placing it
  // in the scene positions its middle rather than its top left corner.
  const auto originX = -pageWidth / 2.0F;
  const auto originY = pageHeight / 2.0F;

  // Row 0 is the page background; the glyphs follow it. Every row is one
  // instance of the glyph quad.
  std::vector<Doc::VBORow> vertexData;
  vertexData.push_back(Doc::VBORow{
      {0.0F, 0.0F, 0.0F},
      color(255),
      color(255),
      {0, 0},
      {0, 0},
      layerWH(0, std::min(16383U, static_cast<unsigned int>(pageWidth)),
              std::min(16383U, static_cast<unsigned int>(pageHeight))),
      {render::packTagIdentity(2, this->doc->documentIndex(), aPageIndex), 0}});

  const auto text = layout->get_text().raw();
  const auto font =
      layout->get_context()->load_font(layout->get_font_description());

  // Walk the clusters. A cluster is the smallest run Pango will not break
  // apart, so it is what one quad can represent: an "ffi" ligature or a letter
  // with its combining marks is one cluster covering several characters.
  auto iter = layout->get_iter();
  while (true) {
    Pango::Rectangle clusterInk;
    Pango::Rectangle clusterLogical;
    iter.get_cluster_extents(clusterInk, clusterLogical);

    const auto start = static_cast<std::size_t>(std::max(0, iter.get_index()));
    const bool more  = iter.next_cluster();
    const auto end =
        more ? static_cast<std::size_t>(std::max(0, iter.get_index()))
             : text.size();

    if (end <= start || start >= text.size()) {
      if (!more) {
        break;
      }
      continue;
    }

    // A trailing newline is part of the cluster's byte range but has nothing
    // to draw; it still has to be counted so offsets stay in step with the
    // text.
    std::size_t drawEnd = end;
    while (drawEnd > start && ('\n' == text[drawEnd - 1] ||
                               '\r' == text[drawEnd - 1])) {
      drawEnd--;
    }

    clusters.push_back(
        ClusterBox{static_cast<std::uint32_t>(start),
                   static_cast<std::uint32_t>(end - start),
                   static_cast<std::uint32_t>(utf8Length(
                       std::string_view(text).substr(start, end - start)))});

    if (drawEnd == start) {
      if (!more) {
        break;
      }
      continue;
    }

    // The whole cluster is rasterised, not just its first codepoint. Taking
    // the leading sequence dropped the rest of every ligature and every
    // combining mark from the page.
    const std::string_view chr(text.data() + start, drawEnd - start);
    const auto glyph    = state.glyphCache.put(chr, font);
    const auto &coords  = glyph.texCoords;
    const auto &extents = glyph.dims;

    const auto glyphWidth  = static_cast<float>(static_cast<int>(extents.width));
    const auto glyphHeight = static_cast<float>(static_cast<int>(extents.height));

    if (0.0F < glyphWidth && 0.0F < glyphHeight) {
      // Pango measures from the top left of the text block downwards; the page
      // runs upwards from its own origin, hence the negated Y.
      const auto left =
          pageMargin + static_cast<float>(toPixels(clusterLogical.get_x()));
      const auto top =
          pageMargin + static_cast<float>(toPixels(clusterLogical.get_y()));

      vertexData.push_back(Doc::VBORow{
          {originX + left + (glyphWidth / 2.0F),
           originY - (top + (glyphHeight / 2.0F)), 0.1F},
          color(0),
          color(255),
          {coords.topLeft.x, coords.topLeft.y},
          {coords.box.width, coords.box.height},
          layerWH(static_cast<unsigned char>(glyph.layer),
                  static_cast<unsigned int>(glyphWidth),
                  static_cast<unsigned int>(glyphHeight)),
          // The cluster index into this page's cluster table, which is what
          // turns a picked fragment back into a text position. The identity
          // word says which document and page that table belongs to.
          {render::packTagIdentity(3, this->doc->documentIndex(), aPageIndex),
           static_cast<unsigned int>(clusters.size() - 1)}});
    }

    if (!more) {
      break;
    }
  }

  instanceCount = static_cast<std::uint32_t>(vertexData.size());
  pageBacking   = this->doc->pool->reserve(instanceCount);
  this->doc->pool->write(pageBacking, 0, asBytes(vertexData));
}

// Always called from the render thread
void Page::draw(RenderState &state, const glm::mat4 &docTransform) const {
  if (0 == instanceCount) {
    return;
  }
  const render::DrawUniforms uniforms{toArray(docTransform * model)};
  state.device->drawGlyphs(uniforms, doc->pool->buffer(),
                           doc->pool->byteOffset(pageBacking), instanceCount);
}

// Always called from the render thread
void Doc::draw(RenderState &state, const glm::mat4 &viewProjection) const {
  const auto docTransform = viewProjection * model;
  for (const auto &page : pages) {
    page.draw(state, docTransform);
  }
}

Doc::Doc(const RendererRef &renderer, render::RenderDevice *device,
         const glm::mat4 &model, [[maybe_unused]] const Private _priv)
    : Drawable(model), renderer(renderer),
      pool(std::make_unique<BufferPool>(device, sizeof(VBORow),
                                        initialPoolRows)) {}

Doc::Doc(const RendererRef &renderer, render::RenderDevice *device,
         const glm::mat4 &model, const std::string &fileName,
         [[maybe_unused]] const Private _priv)
    : Doc(renderer, device, model, _priv) {
  docFile = fileName;
  std::cout << "NEW DOC: " << this << " " << fileName << " "
            << glm::to_string(model) << "\n";
  std::string tmpText = Glib::file_get_contents(docFile);
  // Read no further than the file actually goes: a file shorter than three
  // bytes has no third byte to inspect.
  for (std::size_t i = 0; i < std::min<std::size_t>(3, tmpText.size()); i++) {
    std::cout << std::format("bom[{}]: {:02x}\n", i,
                             static_cast<unsigned char>(tmpText[i]));
  }
  if (tmpText.size() >= 3 && static_cast<unsigned char>(tmpText[0]) == 0xEF &&
      static_cast<unsigned char>(tmpText[1]) == 0xBB &&
      static_cast<unsigned char>(tmpText[2]) == 0xBF) {
    std::cout << "found utf8 bom: " << tmpText.size() << " "
              << tmpText.capacity() << "\n";
    text = Glib::ustring(tmpText.data() + 3, tmpText.data() + tmpText.size());
    // `char` is signed on most targets, so the BOM bytes have to be widened
    // through `unsigned char`; comparing the raw char against 0xEF/0xFF is
    // never true and silently disables the detection below.
  } else if (tmpText.size() >= 4 &&
             ((/*utf32BE*/ byteAt(tmpText, 0) == 0x00 &&
               byteAt(tmpText, 1) == 0x00 && byteAt(tmpText, 2) == 0xFE &&
               byteAt(tmpText, 3) == 0xFF) ||
              (/*utf32LE*/ byteAt(tmpText, 0) == 0xFF &&
               byteAt(tmpText, 1) == 0xFE && byteAt(tmpText, 2) == 0x00 &&
               byteAt(tmpText, 3) == 0x00))) {
    throw std::logic_error("utf32 not supported yet");
  } else if (tmpText.size() >= 2 &&
             ((/*utf16BE*/ byteAt(tmpText, 0) == 0xFE &&
               byteAt(tmpText, 1) == 0xFF) ||
              (/*utf16LE*/ byteAt(tmpText, 0) == 0xFF &&
               byteAt(tmpText, 1) == 0xFE))) {
    throw std::logic_error("utf16 not supported yet");
  } else {
    text = tmpText;
  }
  auto iter        = text.begin();
  const bool valid = text.validate(iter);
  std::cout << "validate: " << valid << "\n";
  if (!valid) {
    // Only a failed validate() leaves `iter` on a real character; on success it
    // is the end iterator and must not be dereferenced.
    std::cout << "first bad offset: " << std::distance(text.begin(), iter)
              << "\n";
    text = text.make_valid();
  }
}

void Doc::makePages(RenderState &state) {
  std::cout << "MAKING PAGES: " << this << " " << glm::to_string(model) << "\n";
  const auto fontDesc =
      Pango::FontDescription(renderer->defaultFontName().data());
  const auto fonts = Pango::CairoFontMap::get_default();
  const auto ctx   = fonts->create_context();
  ctx->set_font_description(fontDesc);
  Pango::AttrList attrs;
  auto fontAttr = Pango::Attribute::create_attr_font_desc(fontDesc);
  attrs.change(fontAttr);
  std::string loc;
  Glib::get_charset(loc);
  auto tSize      = 0UL;
  const char *txt = text.raw().c_str();
  while (tSize < text.bytes()) {
    auto lay = Pango::Layout::create(ctx);
    lay->set_font_description(fontDesc);
    lay->set_single_paragraph_mode(false);
    lay->set_height(std::ceil(139.70 * 11 * PANGO_SCALE));
    lay->set_width(std::ceil(139.70 * 8.5 * PANGO_SCALE));
    lay->set_ellipsize(Pango::EllipsizeMode::END);
    pango_layout_set_text(lay->gobj(), txt + tSize,
                          static_cast<int>(text.bytes() - tSize));
    const auto &line = lay->get_const_line(lay->get_line_count() - 1);
    newPage(state, lay, static_cast<std::uint32_t>(tSize));
    const int len = line->get_length();
    tSize += line->get_start_index() + (0 == len ? 1 : len);
  }
}

void Doc::newPage(RenderState &state, Glib::RefPtr<Pango::Layout> &layout,
                  const std::uint32_t textOffset) {
  renderer->run([this, &state, layout, textOffset] {
    const auto numPages = this->pages.size();
    // Pages are laid out in pixels and scaled here, so the stacking distance is
    // in world units while everything inside the page is not.
    glm::mat4 trans = glm::translate(
        glm::mat4(1.0),
        glm::vec3(0.0F, -100 * static_cast<float>(numPages), 0.0F));
    trans = glm::scale(trans, glm::vec3(pixelsToWorld, pixelsToWorld, 1.0F));
    pages.emplace_back(this->getPtr(), state, trans, layout, textOffset,
                       static_cast<std::uint32_t>(numPages));
  });
}
// vi: set sw=2 sts=2 ts=2 et:
