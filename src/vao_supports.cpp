#include <gleditor/gl/state.hpp>      // for GLState
#include <gleditor/renderer.hpp>      // for Renderer, RendererRef
#include <gleditor/vao_supports.hpp>  // IWYU pragma: associated
#include <GL/glew.h>                  // for glBindBuffer, glGetIntegerv
#include <algorithm>                  // for for_each, __sort_fn, sort
#include <array>                      // for array
#include <format>                     // for format
#include <iostream>                   // for basic_ostream, operator<<, basi...
#include <ranges>                     // for _Filter, _Partial, operator|
#include <stdexcept>                  // for runtime_error
#include <string>                     // for char_traits, basic_string, oper...
#include <utility>                    // for pair, move
#include <vector>                     // for vector
#include <functional>                 // for less
#include <iterator>                   // for distance
#include <list>                       // for _List_iterator, operator==
#include <memory>                     // for __shared_ptr_access
#include <unordered_map>              // for unordered_map, operator==

/// Utilities
template <typename... Args> inline void genBuffers(Args... args) {
  (glGenBuffers(1, args), ...);
}
template <typename... Args> inline void delBuffers(Args... args) {
  (glDeleteBuffers(1, &args), ...);
}
template <typename... Args> inline void genVertexArrays(Args... args) {
  (glGenVertexArrays(1, args), ...);
}
template <typename... Args> inline void clearBuffers(Args... args) {
  ((glBindBuffer(args, 0)), ...);
}
///

VAOSupports::VAOSupports(RendererRef renderer, VAOBuffers bufferInfos)
    : renderer(std::move(renderer)), bufferInfos(std::move(bufferInfos)) {

  this->bufferInfos.vbo.free.emplace_back(0, this->bufferInfos.vbo.maxVertices);
  this->bufferInfos.ibo.free.emplace_back(0, this->bufferInfos.ibo.maxIndices);

  this->renderer->run([this] { allocateBuffers(); });
}
VAOSupports::~VAOSupports() {
  renderer->run([this] { deallocateBuffers(); });
}
void VAOSupports::allocateBuffers() {

  genVertexArrays(&vao);
  genBuffers(&vbo, &ibo, &ubo);

  AutoVAO binder(this);

  glBufferData(GL_ARRAY_BUFFER,
               bufferInfos.vbo.maxVertices * bufferInfos.vbo.stride, nullptr,
               GL_STATIC_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               bufferInfos.ibo.maxIndices * bufferInfos.ibo.stride, nullptr,
               GL_STATIC_DRAW);
#define STR(x) #x

  int ret;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ret);
  std::cout << STR(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT) ": " << ret << "\n";
  glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &ret);
  std::cout << STR(GL_MAX_UNIFORM_BLOCK_SIZE) ": " << ret << "\n";
  glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &ret);
  std::cout << STR(GL_MAX_VERTEX_UNIFORM_BLOCKS) ": " << ret << "\n";
  glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &ret);
  std::cout << STR(GL_MAX_FRAGMENT_UNIFORM_BLOCKS) ": " << ret << "\n";
  glGetIntegerv(GL_MAX_GEOMETRY_UNIFORM_BLOCKS, &ret);
  std::cout << STR(GL_MAX_GEOMETRY_UNIFORM_BLOCKS) ": " << ret << "\n";
  glGenBuffers(1, &ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(Highlight) * 2048, nullptr,
               GL_STATIC_DRAW);
  std::vector<Highlight> high{
      {{0U, 4U, 0x0000ff00U}, {5U, 8U, 0x00ff0000U}, {0U, 0U, 0U}}};
  std::for_each(high.begin(), high.end(), [](auto i) {
    std::cout << std::format("start: {} end: {} data: {:x}\n", i.start, i.end,
                             i.data);
  });
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(Highlight) * high.size(),
                  high.data());
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void VAOSupports::deallocateBuffers() {

  clearBuffers();

  delBuffers(vbo, ibo, ubo);

  glDeleteVertexArrays(1, &vao);

  vao = vbo = ibo = 0;
}

void VAOSupports::defragmentFreeLists() {}

/** vboQuads/iboQuads of the failed allocation, use as hints for reallocating
 * buffers */
void VAOSupports::reallocate(long vertexRes, long indexRes) {

  const unsigned int origVbo = vbo;
  const unsigned int origIbo = ibo;

  const long origVboMaxVertices = bufferInfos.vbo.maxVertices;
  const long origIboMaxIndices  = bufferInfos.ibo.maxIndices;
  long origVboEnd               = origVboMaxVertices * bufferInfos.vbo.stride;
  long origIboEnd               = origIboMaxIndices * bufferInfos.ibo.stride;

  // XXX: naively double the space for now
  bufferInfos.vbo.maxVertices *= 2;
  bufferInfos.ibo.maxIndices *= 2;

  renderer->run([=, this]() {
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
  });

  bufferInfos.vbo.free.emplace_back(
      origVboEnd + 1, bufferInfos.vbo.maxVertices - origVboMaxVertices);
  bufferInfos.ibo.free.emplace_back(origIboEnd + 1, bufferInfos.ibo.maxIndices -
                                                        origIboMaxIndices);

  defragmentFreeLists();
}

unsigned int fixupAttr(const auto &pair) {
  static constexpr std::array<std::string, 6> arr = {
      "position", "fgcolor", "bgcolor", "texcoord", "texBox", "layer"};
  return std::distance(arr.begin(), std::ranges::find(arr, pair.first));
}

auto VAOSupports::findFreeOffset(FreeList &freeList, long rows) {
  return std::ranges::find_if(
      freeList, [rows](auto &pair) { return pair.second >= rows; });
}
void VAOSupports::clearVAO() {
  glBindVertexArray(0);
  clearBuffers(GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_UNIFORM_BUFFER);
}
void VAOSupports::bindVAO() const {
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo);
}
void VAOSupports::clearProgram() { glUseProgram(0); }
void VAOSupports::useProgram(const GLState &state,
                             const std::string &progName) const {
  // std::cerr << std::format("setting up {} attribs\n", progName);
  if ("main" == progName) {
    auto program = state.programs.at("main");
    glUseProgram(program.id);
    int hBlockIdx = glGetUniformBlockIndex(program.id, "Highlights");
    glUniformBlockBinding(program.id, hBlockIdx,
                          0); // not strictly necessary as 0 is the default

    std::vector<std::pair<std::string, GLState::Loc>> pairs(
        program.locs.begin(), program.locs.end());
    std::ranges::sort(pairs, {}, [](const auto &par) {
      return -1 == par.second ? fixupAttr(par) : par.second.loc;
    });
    GLint offset = 0;
    for (const auto &[attr, loc] :
         pairs | std::views::filter(
                     [](const std::pair<std::string, GLState::Loc> &pair) {
                       return pair.second.type == "in";
                     })) {
      // std::cout << "offset: " << offset << "\n";
      if (-1 != loc) {
        GLenum typ;
        if (loc.varType == "uint" || loc.varType.contains("uvec")) {
          typ = GL_UNSIGNED_INT;
        } else if (loc.varType == "int" || loc.varType.contains("ivec")) {
          typ = GL_INT;
        } else if (loc.varType == "bool" || loc.varType.contains("bvec")) {
          typ = GL_BOOL;
          //} else if ("position" == attr && loc.varType == "vec") {
          //  typ = GL_HALF_FLOAT;
        } else {
          typ = GL_FLOAT;
        }
        /*std::cerr << "setting up vertex attrib: " << int(loc)
                  << " size: " << loc.size
                  << " stride: " << bufferInfos.vbo.stride
                  << " offset: " << offset << " varType: " << loc.varType <<
           "\n";*/
        glEnableVertexAttribArray(loc);
        if (typ != GL_FLOAT && typ != GL_HALF_FLOAT) {
          glVertexAttribIPointer(loc, loc.size, typ, bufferInfos.vbo.stride,
                                 /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
                                 reinterpret_cast<void *>(offset));
        } else {
          glVertexAttribPointer(loc, loc.size, typ, GL_FALSE,
                                bufferInfos.vbo.stride,
                                /* NOLINTNEXTLINE(performance-no-int-to-ptr) */
                                reinterpret_cast<void *>(offset));
        }
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
  return reserve(GL_TRIANGLES, triangles);
}
VAOSupports::Handle VAOSupports::reserveQuads(long quads) {
  return reserve(GL_QUADS, quads);
}
VAOSupports::Handle VAOSupports::reservePoints(long points) {
  return reserve(GL_POINTS, points);
}
constexpr int typeToSize(const GLenum type) {
  switch (type) {
  case GL_POINTS:
    return 1;
  case GL_TRIANGLES:
    return 3;
  case GL_QUADS:
    return 4;
  default:
    throw std::runtime_error(std::format(
        "Unexpected OpenGL type: {}. Must be GL_POINTS/GL_TRIANGLES/GL_QUADS.",
        type));
  }
}

VAOSupports::Handle VAOSupports::reserve(unsigned int type, long res) {

  const int pointsPer  = typeToSize(type);
  const long numPoints = pointsPer * res;
  auto vboIt           = findFreeOffset(bufferInfos.vbo.free, numPoints);
  auto iboIt           = GL_QUADS != type ? bufferInfos.ibo.free.end()
                                          : findFreeOffset(bufferInfos.ibo.free, 6 * res);

  if (bufferInfos.vbo.free.cend() == vboIt ||
      (GL_QUADS == type && bufferInfos.ibo.free.cend() == iboIt)) {
    reallocate(numPoints, GL_QUADS == type ? 6 * res : 0);
    vboIt = findFreeOffset(bufferInfos.vbo.free, numPoints);
    iboIt = GL_QUADS != type ? bufferInfos.ibo.free.end()
                             : findFreeOffset(bufferInfos.ibo.free, 6 * res);
  }

  VAOSupports::Handle ret;
  ret.vbo = {static_cast<long>(vboIt->first * bufferInfos.vbo.stride),
             numPoints * bufferInfos.vbo.stride};
  if (GL_QUADS != type) {
    ret.ibo = {0, 0};
  } else {
    const auto iboQuadStride = 6 * res * bufferInfos.ibo.stride;
    ret.ibo = {static_cast<long>(iboIt->first * bufferInfos.ibo.stride),
               static_cast<long>(iboQuadStride)};
    iboIt->first += iboQuadStride;
    iboIt->second -= iboQuadStride;
  }

  vboIt->first += numPoints;
  vboIt->second -= numPoints;

  if (vboIt->second <= 0) {
    bufferInfos.vbo.free.erase(vboIt);
  }
  if (GL_QUADS == type && iboIt->second <= 0) {
    bufferInfos.ibo.free.erase(iboIt);
  }
  return ret;
}
