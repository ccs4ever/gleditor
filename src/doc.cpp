#include "doc.hpp"
#include "GLState.hpp"
#include "cairomm/surface.h"
#include "glibmm/convert.h"
#include "pango/pango-types.h"
#include "pango/pangocairo.h"
#include "pangomm/attributes.h"
#include "pangomm/fontdescription.h"
#include "pangomm/layout.h"
#include "vao_supports.hpp"
#include <GL/glew.h>
#include <cstring>
#include <format>
#include <glm/common.hpp>
#include <glm/ext/scalar_common.hpp>
#include <locale>
#include <pangomm/glyphstring.h>
#include <pangomm/item.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <glm/ext.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/trigonometric.hpp>
#include <iostream>
#include <memory>
#include <pangomm.h>
#include <pangomm/cairofontmap.h>
#include <string>

glm::vec3 lwh(uint i) {
  return {i >> uint(24), (i >> uint(12)) & uint(4095), i & uint(4095)};
}

Page::Page(std::shared_ptr<Doc> aDoc, AppState &appState, GLState &state,
           glm::mat4 &model)
    : Drawable(model), doc(std::move(aDoc)) {
  // interleaved data --
  // https://www.opengl.org/wiki/Vertex_Specification#Interleaved_arrays pos
  // (3-vector), fg color (3-vector), bg color (3-vector),
  //   texture coordinates (2-vector: st) s == x, t == y, bottom left == (0,0)
  //   top right == (1,1)
  // the vertices should be layed out so that each triangle is created in a
  // counter-clockwise order this allows face culling to work. OpenGL will keep
  // a triangle that is "facing" you. It determines this by seeing if its
  // vertices are being drawn clockwise or counter-clockwise. By default,
  // counter-clockwise indicates "its facing you" while clockwise indicates that
  // the back face is pointing at you. Face culling is enabled with
  // glEnable(GL_CULL_FACE)
  /*
  std::array<GLfloat, static_cast<std::size_t>(12 * 4)> vertexData = {
      // left-bottom,  white, black, tex: left-top, layer0
      0, -2.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0,
      // right-bottom, white, black, tex: right-top, layer0
      2.0, -2.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0,
      // left-top, white, black, tex: left-bottom, layer0
      0, 0, 0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0,
      // right-top, white, black, tex: right-bottom, layer0
      2.0, 0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0};
  std::array<GLuint, 6> indexData = {
      0, 1, 2, // (lb, rb, lt) ccw
      3, 2, 1, // (rt, lt, rb) ccw
  };
  */
  auto fontDesc = Pango::FontDescription(appState.defaultFontName);
  auto fonts    = Pango::CairoFontMap::get_default();
  auto ctx      = fonts->create_context();
  ctx->set_font_description(fontDesc);
  auto font = ctx->load_font(fontDesc);
  Glib::ustring text;
  if ("" != doc->docFile) {
    std::string tmpStr;
    std::ifstream istr(doc->docFile, std::ios::ate);
    auto size = std::min(ulong(1 << 16), ulong(istr.tellg()));
    tmpStr.resize(size + 1);
    istr.seekg(0);
    istr.read(tmpStr.data(), size);
    text = std::move(tmpStr);
  } else {
    text = "The quick brown fox jumped over the lazy dog.";
  }
  Pango::AttrList attrs;
  auto fontAttr = Pango::Attribute::create_attr_font_desc(fontDesc);
  attrs.change(fontAttr);
  std::string loc;
  Glib::get_charset(loc);
  /*std::cerr << "text: " << Glib::convert_with_fallback(text.raw(), loc, "UTF-8")
            << " pango attrs: "
            << Glib::convert_with_fallback(attrs.to_string().raw(), loc,
                                           "UTF-8")
            << " valid?: " << bool(attrs) << "\n";*/
  auto lay = Pango::Layout::create(ctx);
  lay->set_font_description(fontDesc);
  lay->set_text(text);
  auto color   = Doc::VBORow::color;
  auto color3  = Doc::VBORow::color3;
  auto layerWH = Doc::VBORow::layerWidthHeight;
  auto xMargin = 2;
  auto yMargin = 2;
  auto layW =
      float(lay->get_logical_extents().get_width()) / PANGO_SCALE + float(xMargin) * 2;
  auto layH = float(lay->get_logical_extents().get_height()) / PANGO_SCALE +
              float(yMargin) * 2;
  std::vector<Doc::VBORow> vertexData = {
      // left-bottom,  white, black, tex: left-top, layer0
      // Doc::VBORow{{0.0, -2.0, 1.0}, color(255), color(0), {0.0, 1.0}, 0},
      // right-bottom, white, black, tex: right-top, layer0
      // Doc::VBORow{{2.0, -2.0, 1.0}, color(255), color(0), {1.0, 1.0}, 0},
      // left-top, white, black, tex: left-bottom, layer0
      Doc::VBORow{{layW / 32.0F, -layH / 48.0F, 0},
                  color(0),
                  color(255),
                  {0, 0},
                  {1, 1},
                  layerWH(0, std::min(4095U, uint(layW)), std::min(4095U, uint(layH)))},
  };
  auto logAttrs = lay->get_log_attrs();
  auto iter     = lay->get_iter();
  std::cout << "char count: " << lay->get_character_count()
            << " attrs size: " << logAttrs.size() << "\n";
  /*for (const auto &attr : logAttrs) {
    std::cout << std::format(
        "Glyph: attr.backspace_deletes_character {}, attr.break_inserts_hyphen "
        "{}, attr.break_removes_preceding {}, attr.is_char_break {}, "
        "attr.is_line_break {}, attr.is_mandatory_break {}, "
        "attr.is_cursor_position {}, attr.is_expandable_space {}, "
        "attr.is_sentence_boundary {}, attr.is_sentence_end {}, "
        "attr.is_sentence_start {}, attr.is_word_boundary {}, attr.is_word_end "
        "{}, attr.is_word_start {}, attr.is_white {}, attr.reserved {}\n",
        attr.backspace_deletes_character, attr.break_inserts_hyphen,
        attr.break_removes_preceding, attr.is_char_break, attr.is_line_break,
        attr.is_mandatory_break, attr.is_cursor_position,
        attr.is_expandable_space, attr.is_sentence_boundary,
        attr.is_sentence_end, attr.is_sentence_start, attr.is_word_boundary,
        attr.is_word_end, attr.is_word_start, attr.is_white, attr.reserved);
  }*/
  int lastIdx  = -1;
  bool plusOne = true;
  auto xpen    = float(xMargin);
  auto ypenF   = [&iter, yMargin]() {
    return (float(iter.get_baseline()) -
            iter.get_line_logical_extents().get_ascent() / 2) /
               PANGO_SCALE +
           float(yMargin * 2);
  };
  auto ypen = ypenF();
  Pango::Rectangle rInk;
  Pango::Rectangle rLog;
  do {
    int idx =
        lastIdx == iter.get_index() ? lay->get_text().size() : iter.get_index();
    /*std::cout << "xoff: " << iter.get_line_logical_extents().get_x()
              << " yoff: " << iter.get_line_logical_extents().get_y()
              << " base: " << iter.get_baseline() << " ypen: " << ypen
              << " ypen calc: " << ypenF() << " log height: "
              << float(iter.get_line_logical_extents().get_height()) /
                     PANGO_SCALE
              << "\n";
    std::cout << lay->get_text().size() << " " << idx
              << (unsigned char)lay->get_text()[idx]
              << (lay->get_text()[idx] == '\n') << "\n";*/
    int len = idx - lastIdx;
    if (-1 != lastIdx && idx != lay->get_text().size() &&
        lay->get_text()
                .substr(lastIdx, len == 0 ? Glib::ustring::npos : len)
                .rfind('\n') != Glib::ustring::npos) {
      //std::cout << "GOT newline\n";
      xpen = float(xMargin);
      ypen = ypenF() + float(lay->get_spacing()) / PANGO_SCALE;
      // idx += 1; // skip
    } else if (-1 != lastIdx) {
      Glib::ustring tmp =
          text.substr(lastIdx, len == 0 ? Glib::ustring::npos : len);
      /*std::cout << "iter index: " << idx - len << " char: " << tmp << " "
                << " cluster width: "
                << iter.get_cluster_ink_extents().get_width() / PANGO_SCALE
                << "\n";*/
      const auto [coords, extents] = state.glyphCache.put(tmp, font);
      /*std::cerr << std::format("coords: pt: {}/{} box: {}/{}\n",
                               coords.topLeft.x, coords.topLeft.y,
                               coords.box.width, coords.box.height);
      std::cerr << std::format("extents: {}/{} {}\n", int(extents.width),
                               int(extents.height), xpen);*/
      auto vec = lwh(layerWH(0, int(extents.width), int(extents.height)));
      vec      = (doc->model * model * glm::vec4(vec, 0));
      /*std::cerr << std::format("vec: {} {}/{} {}\n", vec.x, vec.y, vec.z,
                               -ypen / 30.0F);*/
      xpen += float(int(extents.width)) / 35.0F;
      //std::cout << "xpen sent: " << xpen << "\n";
      vertexData.push_back(
          Doc::VBORow{{xpen, -ypen / 30.0F, 0},
                      color(0),
                      color(255),
                      {coords.topLeft.x, coords.topLeft.y},
                      {coords.box.width, coords.box.height},
                      layerWH(0, uint(extents.width), uint(extents.height))});
      xpen += float(int(extents.width)) / 35.0F;
      //std::cout << "xpen after: " << xpen << "\n";
    }
    lastIdx = idx;
    iter.get_cluster_extents(rInk, rLog);
    iter.next_cluster();
  } while (lastIdx < lay->get_text().size());

#if 0
  auto items = ctx->itemize(text, attrs);
  for (const auto &item : items) {
    Glib::ustring seg{item.get_segment(text)};
    auto glyphStr = item.shape(seg);
    for (auto i = 0UL; i < glyphStr.get_glyphs().size(); i++) {
      std::cerr << std::format("log cluster {}: {}\n", i, glyphStr.gobj()->log_clusters[i]);
    }
    auto glyphs   = glyphStr.get_glyphs();
    std::cerr << "seg: " << Glib::convert_with_fallback(seg.raw(), loc, "UTF-8") << " text: " << Glib::convert_with_fallback(text.raw(), loc, "UTF-8") << " len: " << seg.length() << " " << text.length() << " " << glyphStr.get_width() << " " << glyphs.size() << "\n";
    for (const auto &glyphInfo : glyphs) {
      std::cerr << std::format("got glyph: {} is_cluster_start: {}\n",
                               glyphInfo.get_glyph(),
                               glyphInfo.get_attr().is_cluster_start);
    }
  }
#endif
#if 0
  glGenTextures(1, &tex);
  std::array<unsigned char, 256L * 256> arr;
  std::srand(std::time(nullptr));
  std::ranges::generate(arr, []() { return std::rand() % 255; });
  glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 256, 256, 1, 0, GL_RED,
               GL_UNSIGNED_BYTE, arr.data());
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
#endif
  pageBackingHandle = doc->reservePoints(vertexData.size());
  std::cerr << "page backing handle: vbo: " << pageBackingHandle.vbo.offset
            << " " << pageBackingHandle.vbo.size
            << " ibo: " << pageBackingHandle.ibo.offset << " "
            << pageBackingHandle.ibo.size << "\n"
            << std::flush;
  glBufferSubData(GL_ARRAY_BUFFER, pageBackingHandle.vbo.offset,
                  pageBackingHandle.vbo.size, vertexData.data());
  // glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, pageBackingHandle.ibo.offset,
  //                 pageBackingHandle.ibo.size, indexData.data());
}

void Page::draw(const GLState &state, const glm::mat4 &docModel) {
  // model = glm::rotate(model, glm::radians(1.0F), glm::vec3(0, 0, 1));
  glUniformMatrix4fv(state.programs.at("main")["model"], 1, GL_FALSE,
                     glm::value_ptr(docModel * model));
  glUniform1f(state.programs.at("main")["cubeDepth"], /*2.0F*/ 0);
  /*std::cout << "docModel: " << glm::to_string(docModel)
            << "\npageModel: " << glm::to_string(model)
            << "\nmult: " << glm::to_string(docModel * model) << "\n";*/
  // make the compiler happy, reinterpret_cast<void*> of long would introduce
  // performance penalties apparently
  state.glyphCache.bindTexture();
  glDrawArrays(GL_POINTS,
               (int)pageBackingHandle.vbo.offset / sizeof(Doc::VBORow), 1);
  glUniform1f(state.programs.at("main")["cubeDepth"], 0);
  glUniformMatrix4fv(
      state.programs.at("main")["model"], 1, GL_FALSE,
      glm::value_ptr(docModel * model
                     // glm::translate(model, glm::vec3(-0.1F, 0.1F, 0.1F))
                     ));
  glDrawArrays(GL_POINTS,
               (int)pageBackingHandle.vbo.offset / sizeof(Doc::VBORow) + 1,
               pageBackingHandle.vbo.size / sizeof(Doc::VBORow) - 1);
  GlyphCache::clearTexture();
  // TODO: add glyph boxes
  for (const auto &handle : glyphs) {
  }
}

void Doc::draw(const GLState &state) {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  for (auto &page : pages) {
    page.draw(state, model);
  }
}

Doc::Doc(glm::mat4 model, std::string &fileName,
         [[maybe_unused]] Doc::Private _priv)
    : Doc(model, _priv) {
  docFile = fileName;
}

Doc::Doc(glm::mat4 model, [[maybe_unused]] Doc::Private _priv)
    : Drawable(model),
      VAOSupports(VAOSupports::VAOBuffers(
          VAOSupports::VAOBuffers::Vbo(sizeof(VBORow), 10000000),
          VAOSupports::VAOBuffers::Ibo(sizeof(unsigned int), 1))) {}

void Doc::newPage(AppState &appState, GLState &state) {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  const auto numPages = pages.size();
  glm::mat4 trans     = glm::translate(
      glm::mat4(1.0),
      glm::vec3(0, -5.0F / 16.0F * static_cast<float>(numPages), 0.0F));
  // trans = glm::rotate(trans, glm::radians(20.0F*numPages), glm::vec3(0.5, 1,
  // 0)); trans = glm::scale(trans, glm::vec3(1+numPages, 1+numPages, 1));
  pages.emplace_back(getPtr(), appState, state, trans);
}
