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

  this->bufferInfos.vbo.free.emplace_back(0, this->bufferInfos.vbo.maxQuads);
  this->bufferInfos.ibo.free.emplace_back(0, this->bufferInfos.ibo.maxQuads);

  genVertexArrays(&vao);

  allocateBuffers();
}
void VAOSupports::allocateBuffers() {
  
  genBuffers(&vbo, &ibo);

  AutoVAO binder(this);
  
  glBufferData(GL_ARRAY_BUFFER,
               bufferInfos.vbo.maxQuads * bufferInfos.vbo.stride, nullptr,
               GL_STATIC_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               bufferInfos.ibo.maxQuads * bufferInfos.ibo.stride, nullptr,
               GL_STATIC_DRAW);
}

void VAOSupports::defragmentFreeLists() {}

/** vboQuads/iboQuads of the failed allocation, use as hints for reallocating
 * buffers */
void VAOSupports::reallocate(long vboQuads, long iboQuads) {

  const unsigned int origVbo = vbo;
  const unsigned int origIbo = ibo;

  const long origVboMaxQuads = bufferInfos.vbo.maxQuads;
  const long origIboMaxQuads = bufferInfos.ibo.maxQuads;
  long origVboEnd            = origVboMaxQuads * bufferInfos.vbo.stride;
  long origIboEnd            = origIboMaxQuads * bufferInfos.ibo.stride;

  // XXX: naively double the space for now
  bufferInfos.vbo.maxQuads *= 2;
  bufferInfos.ibo.maxQuads *= 2;

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

  bufferInfos.vbo.free.emplace_back(origVboEnd + 1,
                                    bufferInfos.vbo.maxQuads - origVboMaxQuads);
  bufferInfos.ibo.free.emplace_back(origIboEnd + 1,
                                    bufferInfos.ibo.maxQuads - origIboMaxQuads);

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
  //std::cerr << std::format("setting up {} attribs\n", progName);
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
                     [](const std::pair<std::string, GLState::Loc> &loc) {
                       return loc.second.type == "in";
                     })) {
      //std::cerr << "setting up vertex attrib: " << int(loc) << " size: " << loc.size << " stride: " << bufferInfos.vbo.stride << " offset: " << offset << "\n";
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, loc.size, GL_FLOAT, GL_FALSE,
                            bufferInfos.vbo.stride,
                            /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
                            reinterpret_cast<void *>(offset));
      offset += loc.size * 4;
    }
  } else {
    throw std::runtime_error(
        "useProgram: No attribs to set: unknown program: " + progName);
  }
  //std::cerr << std::format("setup {} attribs\n", progName);
}

VAOSupports::Handle VAOSupports::reserve(long vboQuads, long iboQuads) {

  auto vboIt = findFreeOffset(bufferInfos.vbo.free, vboQuads);
  auto iboIt = findFreeOffset(bufferInfos.ibo.free, iboQuads);

  if (bufferInfos.vbo.free.cend() == vboIt ||
      bufferInfos.ibo.free.cend() == iboIt) {
    reallocate(vboQuads, iboQuads);
    vboIt = findFreeOffset(bufferInfos.vbo.free, vboQuads);
    iboIt = findFreeOffset(bufferInfos.ibo.free, iboQuads);
  }

  auto ret = VAOSupports::Handle{
      {static_cast<long>(vboIt->first * bufferInfos.vbo.stride),
       vboQuads * bufferInfos.vbo.stride},
      {static_cast<long>(iboIt->first * bufferInfos.ibo.stride),
       iboQuads * bufferInfos.ibo.stride}};

  vboIt->first += vboQuads;
  vboIt->second -= vboQuads;
  iboIt->first += iboQuads;
  iboIt->second -= iboQuads;

  if (vboIt->second <= 0) {
    bufferInfos.vbo.free.erase(vboIt);
  }
  if (iboIt->second <= 0) {
    bufferInfos.ibo.free.erase(iboIt);
  }
  return ret;
}
