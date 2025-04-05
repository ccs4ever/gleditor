#ifndef GLEDITOR_VAO_SUPPORTS_H
#define GLEDITOR_VAO_SUPPORTS_H

#include <cstdint>
#include <gleditor/renderer.hpp>
#include <list>
#include <string>
#include <sys/types.h>
#include <utility>

struct GLState;

class VAOSupports {
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
  struct VAOBuffers {
    struct Vbo {
      Vbo(int stride, long maxQuads) : stride(stride), maxVertices(maxQuads) {}
      int stride{};
      long maxVertices{};
      FreeList free;
    } vbo;
    struct Ibo {
      Ibo(int stride, long maxQuads) : stride(stride), maxIndices(maxQuads) {}
      int stride{};
      long maxIndices{};
      FreeList free;
    } ibo;
    VAOBuffers(Vbo vbo, Ibo ibo) : vbo(std::move(vbo)), ibo(std::move(ibo)) {}
  };
  struct AutoVAO {
    const VAOSupports *support;
    AutoVAO(const VAOSupports *aSupport) : support(aSupport) {
      support->bindVAO();
    }

    ~AutoVAO() { VAOSupports::clearVAO(); }
  };
  struct AutoProgram {
    const VAOSupports *support;
    AutoProgram(const VAOSupports *aSupport, const GLState &state,
                const std::string &progName)
        : support(aSupport) {
      support->useProgram(state, progName);
    }

    ~AutoProgram() { VAOSupports::clearProgram(); }
  };

  VAOSupports(RendererRef renderer, VAOBuffers bufferInfos);
  virtual ~VAOSupports();
  void useProgram(const GLState &state, const std::string &progName) const;
  static void clearProgram();
  void bindVAO() const;
  static void clearVAO();
  Handle reserveTriangles(long triangles);
  Handle reserveQuads(long quads);
  Handle reservePoints(long points);

  unsigned int vao, vbo, ibo, ubo;
  RendererRef renderer;

private:
  VAOBuffers bufferInfos;
  Handle reserve(unsigned int type, long res);
  static auto findFreeOffset(FreeList &freeList, long rows);
  void reallocate(long vertexRes, long indexRes);
  void allocateBuffers();
  void allocateBuffers(unsigned int vboTarget, unsigned int iboTarget);
  void deallocateBuffers();
  void defragmentFreeLists();
};

#endif // GLEDITOR_VAO_SUPPORTS_H
