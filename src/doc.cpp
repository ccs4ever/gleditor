#include "doc.hpp"
#include "GLState.hpp"
#include "SDL_video.h"
#include "cairomm/surface.h"
#include "glibmm/convert.h"
#include "glibmm/refptr.h"
#include "glibmm/unicode.h"
#include "glibmm/ustring.h"
#include "pango/pango-types.h"
#include "pango/pango-utils.h"
#include "pango/pangocairo.h"
#include "pangomm/attributes.h"
#include "pangomm/fontdescription.h"
#include "pangomm/layout.h"
#include "vao_supports.hpp"
#include <GL/glew.h>
#include <cstring>
#include <format>
#include <giomm.h>
#include <glibmm.h>
#include <glm/common.hpp>
#include <glm/ext/scalar_common.hpp>
#include <iterator>
#include <locale>
#include <ostream>
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
#include <string_view>

glm::vec3 lwh(uint i) {
  return {i >> uint(24), (i >> uint(12)) & uint(4095), i & uint(4095)};
}

Page::Page(std::shared_ptr<Doc> aDoc, AppState &appState, GLState &state,
           glm::mat4 &model, Glib::RefPtr<Pango::Layout> &layout)
    : Drawable(model), doc(std::move(aDoc)) {
  static_assert(sizeof(Doc::VBORow) == 40);
  this->layout      = layout;
  const auto &line  = layout->get_const_line(layout->get_line_count() - 1);
  int len           = line->get_length();
  const int charCnt = line->get_start_index() + (0 == len ? 1 : len);
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
  auto color   = Doc::VBORow::color;
  auto color3  = Doc::VBORow::color3;
  auto layerWH = Doc::VBORow::layerWidthHeight;
  auto xMargin = 2;
  auto yMargin = 2;
  auto layW = float(layout->get_logical_extents().get_width()) / PANGO_SCALE +
              float(xMargin) * 2;
  auto layH = float(layout->get_logical_extents().get_height()) / PANGO_SCALE +
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
                  layerWH(0, std::min(16383U, uint(layW)),
                          std::min(16383U, uint(layH)))},
  };
  auto vertMax = std::min(charCnt, 100000);
  vertexData.reserve(vertMax);
  auto logAttrs = layout->get_log_attrs();
  auto iter     = layout->get_iter();
  /*std::cout << "char count: " << charCnt << " attrs size: " << logAttrs.size()
            << "\n";*/
  // auto textIter = text.begin();
  /*for (const auto &attr : logAttrs) {
    if (textIter == text.end()) break;
    std::cout << std::format(
        "Glyph: {} attr.backspace_deletes_character {}, "
        "attr.break_inserts_hyphen "
        "{}, attr.break_removes_preceding {}, attr.is_char_break {}, "
        "attr.is_line_break {}, attr.is_mandatory_break {}, "
        "attr.is_cursor_position {}, attr.is_expandable_space {}, "
        "attr.is_sentence_boundary {}, attr.is_sentence_end {}, "
        "attr.is_sentence_start {}, attr.is_word_boundary {}, attr.is_word_end "
        "{}, attr.is_word_start {}, attr.is_white {}, attr.reserved {}\n",
        (char)*(textIter == text.begin() ? textIter : textIter++),
        attr.backspace_deletes_character, attr.break_inserts_hyphen,
        attr.break_removes_preceding, attr.is_char_break, attr.is_line_break,
        attr.is_mandatory_break, attr.is_cursor_position,
        attr.is_expandable_space, attr.is_sentence_boundary,
        attr.is_sentence_end, attr.is_sentence_start, attr.is_word_boundary,
        attr.is_word_end, attr.is_word_start, attr.is_white, attr.reserved);
    if (textIter == text.begin()) textIter++;
  }*/
  /*for (const auto &line : layout->get_const_lines()) {
    const auto startIdx = line->get_start_index();
    const auto endIdx   = startIdx + line->get_length();
    const auto runs     = line->get_x_ranges(startIdx, endIdx);
    for (const auto &run : runs) {
      int runStartIdx;
      int runStartTrailing;
      int runEndIdx;
      int runEndTrailing;
      bool inOrOutStart =
          line->x_to_index(run.first, runStartIdx, runStartTrailing);
      bool inOrOutEnd = line->x_to_index(run.second, runEndIdx, runEndTrailing);
      std::cout << " st: " << runStartIdx << " " << runStartTrailing
                << " en: " << runEndIdx << " " << runEndTrailing << "\n";
      std::cout << " NEW run: "
                << text.substr(runStartIdx, runEndIdx - runStartIdx) << "\n";
      int idx = runStartIdx;
      int idxTr = runStartTrailing;

    }
  }*/
  int lastIdx = 0;
  int idx     = 0;
  int vboPos  = 0;
  auto xpen   = float(xMargin);
  auto ypenF  = [&iter, yMargin]() {
    return (float(iter.get_baseline()) -
            iter.get_line_logical_extents().get_ascent() / 2) /
               PANGO_SCALE +
           float(yMargin * 2);
  };
  auto ypen = ypenF();
  Pango::Rectangle rInk;
  Pango::Rectangle rLog;
  auto vertIter     = vertexData.begin() + 1;
  pageBackingHandle = doc->reservePoints(charCnt + charCnt / 4);
  auto text         = layout->get_text().raw();
  auto font = layout->get_context()->load_font(layout->get_font_description());
  iter.next_cluster();
  while (lastIdx != (idx = iter.get_index())) {
    iter.get_cluster_extents(rInk, rLog);
    /*std::cout << "xoff: " << iter.get_line_logical_extents().get_x()
              << " yoff: " << iter.get_line_logical_extents().get_y()
              << " base: " << iter.get_baseline() << " ypen: " << ypen
              << " ypen calc: " << ypenF() << " log height: "
              << float(iter.get_line_logical_extents().get_height()) /
                     PANGO_SCALE
              << "\n";
    std::cout << size << " " << idx
              << (unsigned char)text[idx]
              << (text[idx] == '\n') << "\n";*/
    bool hasNewLine = text.at(idx - 1) == '\n';
    std::string_view tmp(
        text.cbegin() + lastIdx,
        text.cbegin() + ((idx-lastIdx) > 3 ? (lastIdx + 3)
                                             : (hasNewLine ? (idx - 1) : idx)));
    if (idx-lastIdx > 3) {
    /*std::string_view tmp2(
        text.cbegin() + lastIdx,
        text.cbegin() + (hasNewLine ? (idx - 1) : idx));
      std::cout << std::format("lidx: {} idx: {} sz: {} clust: {}\n", lastIdx,
                               idx, idx - lastIdx, tmp2);*/
    }
    /*std::cout << "has nl: " << hasNewLine << " cluster: [" << tmp << "] "
              << " end: [" << text.at(idx-1) << "]\n";*/
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
    /*auto vec = lwh(layerWH(0, int(extents.width), int(extents.height)));
    vec      = (doc->model * model * glm::vec4(vec, 0));*/
    /*std::cerr << std::format("vec: {} {}/{} {}\n", vec.x, vec.y, vec.z,
                             -ypen / 30.0F);*/
    xpen += float(int(extents.width)) / 35.0F;
    // std::cout << "xpen sent: " << xpen << "\n";
    vboPos++;
    *vertIter =
        Doc::VBORow({xpen, -ypen / 30.0F, 0}, color(0), color(255),
                    {coords.topLeft.x, coords.topLeft.y},
                    {coords.box.width, coords.box.height},
                    layerWH(0, uint(extents.width), uint(extents.height)));
    vertIter++;
    xpen += float(int(extents.width)) / 35.0F;
    // std::cout << "xpen after: " << xpen << "\n";
    if (hasNewLine) {
      // std::cout << "GOT newline: [" << tmp << "]\n";
      xpen = float(xMargin);
      ypen = ypenF() + float(layout->get_spacing()) / PANGO_SCALE;
    }
    // std::cout << "distance: " << std::distance(vertexData.begin(), vertIter)
    //           << "\n";
    if (std::distance(vertexData.begin(), vertIter) == vertMax) {
      /*std::cout << "OUT calling glBufferSubData: " << vertMax
                << " dist: " << std::distance(vertexData.begin(), vertIter)
                << " pos: " << vboPos << "\n";*/
      vertIter = vertexData.begin();
      glBufferSubData(GL_ARRAY_BUFFER,
                      pageBackingHandle.vbo.offset +
                          (vboPos - (vertMax - 1)) * sizeof(Doc::VBORow),
                      vertMax * sizeof(Doc::VBORow), vertexData.data());
    }
    lastIdx = idx;
    iter.next_cluster();
  }
  if (vertexData.cbegin() != vertIter) {
    const auto dist = std::distance(vertexData.begin(), vertIter);
    /*std::cout << "remain OUT calling glBufferSubData: " << vertMax
              << " dist: " << dist << " pos: " << vboPos << "\n";*/
    vertIter = vertexData.begin();
    glBufferSubData(GL_ARRAY_BUFFER,
                    pageBackingHandle.vbo.offset +
                        (vboPos - (dist - 1)) * sizeof(Doc::VBORow),
                    dist * sizeof(Doc::VBORow), vertexData.data());
  }

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
  /*std::cerr << "page backing handle: vbo: " << pageBackingHandle.vbo.offset
            << " " << pageBackingHandle.vbo.size
            << " ibo: " << pageBackingHandle.ibo.offset << " "
            << pageBackingHandle.ibo.size << "\n"
            << std::flush;*/
  // glBufferSubData(GL_ARRAY_BUFFER, pageBackingHandle.vbo.offset,
  //                 pageBackingHandle.vbo.size, vertexData.data());
  //  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, pageBackingHandle.ibo.offset,
  //                  pageBackingHandle.ibo.size, indexData.data());
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

Doc::Doc(glm::mat4 model, AppState &appState, GLState &glState,
         std::string &fileName, [[maybe_unused]] Doc::Private _priv)
    : Doc(model, appState, _priv) {
  docFile = fileName;
}

void Doc::makePages(AppState &appState, GLState &glState) {
  auto fontDesc = Pango::FontDescription(appState.defaultFontName);
  auto fonts    = Pango::CairoFontMap::get_default();
  auto ctx      = fonts->create_context();
  ctx->set_font_description(fontDesc);
  auto font           = ctx->load_font(fontDesc);
  std::string tmpText = Glib::file_get_contents(docFile);
  std::cout << std::format("bom: {:x} {:x} {:x}\n", tmpText[0], tmpText[1],
                           tmpText[2]);
  if (tmpText.size() >= 3 && (unsigned char)tmpText[0] == 0xEF &&
      (unsigned char)tmpText[1] == 0xBB && (unsigned char)tmpText[2] == 0xBF) {
    std::cout << "found utf8 bom: " << tmpText.size() << " "
              << tmpText.capacity() << "\n";
    text = Glib::ustring(tmpText.data() + 3, tmpText.data() + tmpText.size());
    auto iter = text.begin();
    std::cout << "validate: " << text.validate(iter) << " "
              << (text == text.make_valid() ? "" : text.make_valid()) << "\n";
    std::cout << "first bad: " << *iter << "\n";
  } else if (tmpText.size() >= 4 &&
             ((/*utf32BE*/ (int)tmpText[0] == 0x0 && (int)tmpText[1] == 0x0 &&
               (int)tmpText[2] == 0xFE && (int)tmpText[3] == 0xFF) ||
              (/*utf32LE*/ (int)tmpText[0] == 0xFF && (int)tmpText[1] == 0xFE &&
               (int)tmpText[2] == 0x0 && (int)tmpText[3] == 0x0))) {
    throw std::logic_error("utf32 not supported yet");
  } else if (tmpText.size() >= 2 && ((/*utf16BE*/ (int)tmpText[0] == 0xFE &&
                                      (int)tmpText[1] == 0xFF) ||
                                     (/*utf16LE*/ (int)tmpText[0] == 0xFF &&
                                      (int)tmpText[1] == 0xFE))) {
    throw std::logic_error("utf16 not supported yet");
  } else {
    text = tmpText;
  }
  auto iter = text.begin();
  std::cout << "validate: " << text.validate(iter) << " "
            << (text == text.make_valid()) << "\n";
  std::cout << "first bad: " << *iter << "\n";
  std::cout << "bom: " << text[0] << " " << text[1] << " " << text[2] << "\n";
  Pango::AttrList attrs;
  auto fontAttr = Pango::Attribute::create_attr_font_desc(fontDesc);
  attrs.change(fontAttr);
  std::string loc;
  Glib::get_charset(loc);
  auto tSize      = 0UL;
  const char *txt = text.raw().c_str();
  while (tSize < text.bytes()) {
    /*std::cout << "BEGIN layout creation: " << offset << " " << text.bytes()
              << "\n";*/
    auto lay = Pango::Layout::create(ctx);
    lay->set_font_description(fontDesc);
    lay->set_single_paragraph_mode(false);
    lay->set_height(std::ceil(139.70 * 11 * PANGO_SCALE));
    lay->set_width(std::ceil(139.70 * 8.5 * PANGO_SCALE));
    lay->set_ellipsize(Pango::EllipsizeMode::END);
    pango_layout_set_text(lay->gobj(), txt + tSize, text.bytes() - tSize);
    const auto &line = lay->get_const_line(lay->get_line_count() - 1);
    /*std::cout << "START: " << tSize << " " << line->get_start_index() << " "
              << line->get_length() << "\n"
              << std::flush;*/
    newPage(appState, glState, lay);
    draw(glState);
    SDL_GL_SwapWindow(SDL_GL_GetCurrentWindow());
    int len = line->get_length();
    tSize += line->get_start_index() + (0 == len ? 1 : len);
  }
#if 0
    for (const auto &line : lay->get_const_lines()) {
      /*std::cout << "line: " << line->get_start_index() << " "
                << line->get_length() << " str: ("
                << std::string(txt + tSize + line->get_start_index(), txt + tSize + line->get_start_index() + line->get_length())
                << ") ";*/
      auto xs =
          line->get_x_ranges(line->get_start_index(),
                             line->get_start_index() + line->get_length());
      /*std::ranges::transform(
          xs, std::ostream_iterator<std::string>(std::cout, ", "),
          [&lay, &line](const auto &t) {
            int idx, trail, idx2, trail2;
            bool xin  = line->x_to_index(t.first, idx, trail);
            bool xin2 = line->x_to_index(t.second - 1, idx2, trail2);
            return std::format("{}*{}*{}/{}*{}*{}%{:.02f}@{:.02f}", idx, trail,
                               xin, idx2, trail2, xin2,
                               float(t.first) / PANGO_SCALE,
                               float(t.second) / PANGO_SCALE);
          });
      std::cout << "\n";*/
      int len = line->get_length();
      offset = line->get_start_index() + (0 == len ? 1 : len);
    }
    /*std::cout << "END layout creation: " << offset << " " << text.bytes()
              << "\n";*/
    //txt += offset;
    tSize += offset;
  }
  /*while (offset != size) {
    pango_find_paragraph_boundary(text.c_str() + offset, -1, &paraEnd,
                                  &paraNextStart);
    auto str = Glib::ustring(text.cbegin() + offset,
                                text.cbegin() + offset + paraEnd);
    std::cout << "para: " << str << "\n";
    lay->set_text(text);
    offset += paraNextStart;
  }*/
  std::exit(1);
#endif
  /*std::cerr << "text: " << Glib::convert_with_fallback(text.raw(), loc,
     "UTF-8")
            << " pango attrs: "
            << Glib::convert_with_fallback(attrs.to_string().raw(), loc,
                                           "UTF-8")
            << " valid?: " << bool(attrs) << "\n";*/
}

Doc::Doc(glm::mat4 model, [[maybe_unused]] AppState &appState,
         [[maybe_unused]] Doc::Private _priv)
    : Drawable(model),
      VAOSupports(VAOSupports::VAOBuffers(
          VAOSupports::VAOBuffers::Vbo(sizeof(VBORow), 10000000),
          VAOSupports::VAOBuffers::Ibo(sizeof(unsigned int), 1))) {}

void Doc::newPage(AppState &appState, GLState &state,
                  Glib::RefPtr<Pango::Layout> &layout) {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  const auto numPages = pages.size();
  glm::mat4 trans     = glm::translate(
      glm::mat4(1.0), glm::vec3(0, -100 * static_cast<float>(numPages), 0.0F));
  // trans = glm::rotate(trans, glm::radians(20.0F*numPages), glm::vec3(0.5, 1,
  // 0)); trans = glm::scale(trans, glm::vec3(1+numPages, 1+numPages, 1));
  pages.emplace_back(getPtr(), appState, state, trans, layout);
}
