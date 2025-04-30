#ifndef GLEDITOR_RENDER_SUPPORTS_H
#define GLEDITOR_RENDER_SUPPORTS_H

#include <cstdint>
#include <gleditor/renderer.hpp>
#include <gleditor/renderer/buffer.hpp>
#include <list>
#include <memory>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

struct GLState;

namespace gledit {

class RenderSupports {
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

  RenderSupports(AbstractRendererRef renderer);
  virtual ~RenderSupports();
  void bindVAO() const;
  static void clearVAO();
  Handle reserveTriangles(long triangles);
  Handle reserveQuads(long quads);
  Handle reservePoints(long points);

  unsigned int vao;
  std::vector<std::shared_ptr<renderer::AbstractBuffer>> buffers;
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

} // namespace gledit

#endif // GLEDITOR_RENDER_SUPPORTS_H
