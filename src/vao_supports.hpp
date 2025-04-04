#ifndef GLEDITOR_VAO_SUPPORTS_H
#define GLEDITOR_VAO_SUPPORTS_H

#include "renderer.hpp"
#include "renderer/buffer.hpp"
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct GLState;

template <typename Elem> class VAOSupports {
public:
  struct Handle {
    struct Vbo {
      long offset;
      long size;
    } vbo;
    struct Ibo {
      long offset;
      long size;
    } ibo;
  };

protected:
  // array elements in uniform blocks are always padded to 16 bytes (sizeof a
  // vec4)
  struct Highlight {
    uint start;
    uint end;
    uint data;
    uint data2{};
  };
  using FreeList = std::list<std::pair<std::uint32_t, std::uint32_t>>;

  VAOSupports(AbstractRendererRef renderer);
  virtual ~VAOSupports();
  void bindVAO() const;
  static void clearVAO();
  Handle reserveTriangles(long triangles);
  Handle reserveQuads(long quads);
  Handle reservePoints(long points);

  unsigned int vao;
  struct TypedBuffer {
    unsigned int type;
    std::shared_ptr<gledit::renderer::AbstractBuffer<T>> buf;
  };
  std::vector<std::shared_ptr<TypedBuffer>> buffers;
  std::shared_ptr<gledit::renderer::AbstractBuffer<Elem>> vbo;
  std::shared_ptr<gledit::renderer::AbstractBuffer<unsigned int>> ibo;
  std::shared_ptr<gledit::renderer::AbstractBuffer<Highlight>> ubo;
  AbstractRendererRef renderer;

private:
  Handle reserve(unsigned int type, long res);
  static auto findFreeOffset(FreeList &freeList, long rows);
  void reallocate(long vertexRes, long indexRes);
  void allocateBuffers();
  void allocateBuffers(unsigned int vboTarget, unsigned int iboTarget);
  void deallocateBuffers();
  void defragmentFreeLists();
};

#endif // GLEDITOR_VAO_SUPPORTS_H
