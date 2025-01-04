#include "vao_supports.hpp"
#include "GLState.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <utility>

/// Utilities
template <typename... Args> inline void genBuffers(Args... args) {
  (glGenBuffers(1, args), ...);
}
template <typename... Args> inline void genVertexArrays(Args... args) {
  (glGenVertexArrays(1, args), ...);
}
template <typename... Args> inline void clearBuffers(Args... args) {
  ((glBindBuffer(args, 0)), ...);
}
///

VAOSupports::VAOSupports(VAOBuffers bufferInfos)
    : bufferInfos(std::move(bufferInfos)) {

  this->bufferInfos.vbo.free.emplace_back(0, this->bufferInfos.vbo.maxVertices);
  this->bufferInfos.ibo.free.emplace_back(0,
                                          this->bufferInfos.ibo.maxTriangles);

  genVertexArrays(&vao);

  allocateBuffers();
}
void VAOSupports::allocateBuffers() {

  genBuffers(&vbo, &ibo);

  AutoVAO binder(this);

  glBufferData(GL_ARRAY_BUFFER,
               bufferInfos.vbo.maxVertices * bufferInfos.vbo.stride, nullptr,
               GL_STATIC_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               bufferInfos.ibo.maxTriangles * bufferInfos.ibo.stride, nullptr,
               GL_STATIC_DRAW);
}

void VAOSupports::defragmentFreeLists() {}

/** vboQuads/iboQuads of the failed allocation, use as hints for reallocating
 * buffers */
void VAOSupports::reallocate(long vboQuads, long iboQuads) {

  const unsigned int origVbo = vbo;
  const unsigned int origIbo = ibo;

  const long origVboMaxVertices  = bufferInfos.vbo.maxVertices;
  const long origIboMaxTriangles = bufferInfos.ibo.maxTriangles;
  long origVboEnd                = origVboMaxVertices * bufferInfos.vbo.stride;
  long origIboEnd                = origIboMaxTriangles * bufferInfos.ibo.stride;

  // XXX: naively double the space for now
  bufferInfos.vbo.maxVertices *= 2;
  bufferInfos.ibo.maxTriangles *= 2;

  allocateBuffers();

  AutoVAO binder(this);

  // GL_COPY_{READ,WRITE}_BUFFER are just slots to hold buffers, with no
  // semantic meaning to OpenGL, hence why we can write to a "read" buffer
  glBindBuffer(GL_COPY_READ_BUFFER, origVbo);
  glBindBuffer(GL_COPY_WRITE_BUFFER, origIbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ARRAY_BUFFER, 0, 0, origVboEnd);
  glCopyBufferSubData(GL_COPY_WRITE_BUFFER, GL_ELEMENT_ARRAY_BUFFER, 0, 0,
                      origIboEnd);
  clearBuffers(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER);
  std::array<unsigned int, 2> origBufs = {origVbo, origIbo};
  glDeleteBuffers(2, origBufs.data());

  bufferInfos.vbo.free.emplace_back(
      origVboEnd + 1, bufferInfos.vbo.maxVertices - origVboMaxVertices);
  bufferInfos.ibo.free.emplace_back(
      origIboEnd + 1, bufferInfos.ibo.maxTriangles - origIboMaxTriangles);

  defragmentFreeLists();
}

auto VAOSupports::findFreeOffset(FreeList &freeList, long rows) {
  return std::ranges::find_if(
      freeList, [rows](auto &pair) { return pair.second >= rows; });
}
void VAOSupports::clearVAO() {
  glBindVertexArray(0);
  clearBuffers(GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER);
}
void VAOSupports::bindVAO() const {
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
}
void VAOSupports::clearProgram() { glUseProgram(0); }
void VAOSupports::useProgram(const GLState &state,
                             const std::string &progName) const {
  // std::cerr << std::format("setting up {} attribs\n", progName);
  if ("main" == progName) {
    auto program = state.programs.at("main");
    glUseProgram(program.id);
    std::vector<std::pair<std::string, GLState::Loc>> pairs(
        program.locs.begin(), program.locs.end());
    std::ranges::sort(pairs, {},
                      [](const auto &par) { return par.second.loc; });
    GLint offset = 0;
    for (const auto &[attr, loc] :
         pairs | std::views::filter(
                     [](const std::pair<std::string, GLState::Loc> &pair) {
                       return pair.second.type == "in";
                     })) {
      GLenum typ;
      if (loc.varType == "uint" || loc.varType.contains("uvec")) {
        typ = GL_UNSIGNED_INT;
      } else if (loc.varType == "int" || loc.varType.contains("ivec")) {
        typ = GL_INT;
      } else if (loc.varType == "double" || loc.varType.contains("dvec")) {
        typ = GL_DOUBLE;
      } else if (loc.varType == "bool" || loc.varType.contains("bvec")) {
        typ = GL_BOOL;
      } else {
        typ = GL_FLOAT;
      }
      /*std::cerr << "setting up vertex attrib: " << int(loc)
                << " size: " << loc.size
                << " stride: " << bufferInfos.vbo.stride
                << " offset: " << offset << " varType: " << loc.varType
                << " isInt: " << isInt << "\n";*/
      glEnableVertexAttribArray(loc);
      if (typ != GL_FLOAT) {
        glVertexAttribIPointer(loc, loc.size, typ,
                               bufferInfos.vbo.stride,
                               /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
                               reinterpret_cast<void *>(offset));
      } else {
        glVertexAttribPointer(loc, loc.size, GL_FLOAT, GL_FALSE,
                              bufferInfos.vbo.stride,
                              /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
                              reinterpret_cast<void *>(offset));
      }
      offset += loc.size * 4;
    }
  } else {
    throw std::runtime_error(
        "useProgram: No attribs to set: unknown program: " + progName);
  }
  // std::cerr << std::format("setup {} attribs\n", progName);
}

VAOSupports::Handle VAOSupports::reserveTriangles(long triangles) {
  return reserve(triangles * 3, triangles);
}
VAOSupports::Handle VAOSupports::reserveQuads(long quads) {
  return reserve(quads * 4, quads * 2);
}

VAOSupports::Handle VAOSupports::reserve(long vboVertices, long iboTriangles) {

  auto vboIt = findFreeOffset(bufferInfos.vbo.free, vboVertices);
  auto iboIt = findFreeOffset(bufferInfos.ibo.free, iboTriangles);

  if (bufferInfos.vbo.free.cend() == vboIt ||
      bufferInfos.ibo.free.cend() == iboIt) {
    reallocate(vboVertices, iboTriangles);
    vboIt = findFreeOffset(bufferInfos.vbo.free, vboVertices);
    iboIt = findFreeOffset(bufferInfos.ibo.free, iboTriangles);
  }

  auto ret = VAOSupports::Handle{
      {static_cast<long>(vboIt->first * bufferInfos.vbo.stride),
       vboVertices * bufferInfos.vbo.stride},
      {static_cast<long>(iboIt->first * bufferInfos.ibo.stride),
       iboTriangles * bufferInfos.ibo.stride}};

  vboIt->first += vboVertices;
  vboIt->second -= vboVertices;
  iboIt->first += iboTriangles;
  iboIt->second -= iboTriangles;

  if (vboIt->second <= 0) {
    bufferInfos.vbo.free.erase(vboIt);
  }
  if (iboIt->second <= 0) {
    bufferInfos.ibo.free.erase(iboIt);
  }
  return ret;
}
