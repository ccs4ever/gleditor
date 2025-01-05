#include "doc.hpp"
#include "GLState.hpp"
#include "vao_supports.hpp"
#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <glm/ext.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/trigonometric.hpp>
#include <iostream>
#include <memory>

Page::Page(std::shared_ptr<Doc> aDoc, glm::mat4 &model)
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
  auto color                            = Doc::VBORow::color;
  auto color3                           = Doc::VBORow::color3;
  std::array<Doc::VBORow, 1> vertexData = {
      // left-bottom,  white, black, tex: left-top, layer0
      // Doc::VBORow{{0.0, -2.0, 1.0}, color(255), color(0), {0.0, 1.0}, 0},
      // right-bottom, white, black, tex: right-top, layer0
      // Doc::VBORow{{2.0, -2.0, 1.0}, color(255), color(0), {1.0, 1.0}, 0},
      // left-top, white, black, tex: left-bottom, layer0
      Doc::VBORow{{0, 0, 0}, color(255), color(0), {0.0, 1.0}, 0} //,
      // right-top, white, black, tex: right-bottom, layer0
      // Doc::VBORow{{2.0, 0, 1.0}, color(255), color(0), {1.0, 0.0}, 0}
  };
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
  pageBackingHandle = this->doc->reservePoints(1);
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

void Page::draw(const GLState &state, const glm::mat4 &docModel) const {
  glUniformMatrix4fv(state.programs.at("main")["model"], 1, GL_FALSE,
                     glm::value_ptr(docModel * model));
  /*std::cout << "docModel: " << glm::to_string(docModel)
            << "\npageModel: " << glm::to_string(model)
            << "\nmult: " << glm::to_string(docModel * model) << "\n";*/
  // make the compiler happy, reinterpret_cast<void*> of long would introduce
  // performance penalties apparently
  glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
  glDrawArrays(GL_POINTS,
               (int)pageBackingHandle.vbo.offset / sizeof(Doc::VBORow), 1);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  // TODO: add glyph boxes
  for (const auto &handle : glyphs) {
  }
}

void Doc::draw(const GLState &state) const {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  for (const auto &page : pages) {
    page.draw(state, model);
  }
}

Doc::Doc(glm::mat4x4 model, [[maybe_unused]] Doc::Private _priv)
    : Drawable(model),
      VAOSupports(VAOSupports::VAOBuffers(
          VAOSupports::VAOBuffers::Vbo(sizeof(VBORow), 10000),
          VAOSupports::VAOBuffers::Ibo(sizeof(unsigned int), 15000))) {}

void Doc::newPage(GLState &state) {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  const auto numPages = pages.size();
  glm::mat4 trans     = glm::translate(
      glm::mat4(1.0), glm::vec3(0, -5.0F * static_cast<float>(numPages), 0.0F));
  trans = glm::rotate(trans, glm::radians(5.0F*numPages), glm::vec3(0, 1, 0));
  trans = glm::scale(trans, glm::vec3(1+numPages, 1+numPages, 1));
  pages.emplace_back(getPtr(), trans);
}
