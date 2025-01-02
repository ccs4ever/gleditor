#include "doc.hpp"
#include "state.hpp"
#include "vao_supports.hpp"
#include <GL/glew.h>

#include <array>
#include <cstddef>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
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
  std::array<GLfloat, static_cast<std::size_t>(12 * 4)> vertexData = {
      // left-bottom,  white, black, tex: left-top, layer0
      0, -2.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0,
      // right-bottom, white, black, tex: right-top, layer0
      2.0, -2.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0,
      // left-top, white, black, tex: left-bottom, layer0
      0, 0, 0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0,
      // right-top, white, black, tex: right-bottom, layer0
      2.0, 0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0};
  std::array<GLuint, static_cast<std::size_t>(3 * 4)> indexData = {
      0, 1, 2, // (lb, rb, lt) ccw
      3, 2, 1, // (rt, lt, rb) ccw
  };
  pageBackingHandle = this->doc->reserve(1, 1);
  std::cerr << "page backing handle: vbo: " << pageBackingHandle.vbo.offset
            << " " << pageBackingHandle.vbo.size
            << " ibo: " << pageBackingHandle.ibo.offset << " "
            << pageBackingHandle.ibo.size << "\n"
            << std::flush;
  glBufferSubData(GL_ARRAY_BUFFER, pageBackingHandle.vbo.offset,
                  pageBackingHandle.vbo.size, vertexData.data());
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, pageBackingHandle.ibo.offset,
                  pageBackingHandle.ibo.size, indexData.data());
}

void Page::draw(const AppState &state, const glm::mat4 &docModel) const {
  glUniformMatrix4fv(state.programs.at("main")["model"], 1, GL_FALSE,
                     glm::value_ptr(docModel * model));
  // TODO: add glyph boxes
  for (const auto &handle : glyphs) {
  }
}

void Doc::draw(const AppState &state) const {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  for (const auto &page : pages) {
    page.draw(state, model);
  }
}

Doc::Doc(glm::mat4x4 model, [[maybe_unused]] Doc::Private _priv)
    : Drawable(model),
      VAOSupports(VAOSupports::VAOBuffers(
          VAOSupports::VAOBuffers::Vbo(
              static_cast<unsigned long>(4 * 12) * sizeof(float), 10000),
          VAOSupports::VAOBuffers::Ibo(
              static_cast<unsigned long>(6) * sizeof(unsigned int), 10000))) {}

void Doc::newPage(AppState& state) {
  AutoVAO binder(this);

  AutoProgram progBinder(this, state, "main");

  const auto numPages = pages.size();
  glm::mat4 trans     = glm::translate(
      glm::mat4(), glm::vec3(0.0F, 5.0F * static_cast<float>(numPages), 0.0F));
  pages.emplace_back(getPtr(), trans);

}