#ifndef GLEDITOR_VAO_SUPPORTS_H
#define GLEDITOR_VAO_SUPPORTS_H

#include <cstdint>
#include <list>
#include <string>
#include <utility>

struct AppState;

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
  using FreeList = std::list<std::pair<std::uint32_t, std::uint32_t>>;
  struct VAOBuffers {
    struct Vbo {
      Vbo(int stride, long maxQuads) : stride(stride), maxQuads(maxQuads) {}
      int stride{};
      long maxQuads{};
      FreeList free;
    } vbo;
    struct Ibo {
      Ibo(int stride, long maxQuads) : stride(stride), maxQuads(maxQuads) {}
      int stride{};
      long maxQuads{};
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
    AutoProgram(const VAOSupports *aSupport, const AppState &state,
                const std::string &progName)
        : support(aSupport) {
      support->useProgram(state, progName);
    }

    ~AutoProgram() { VAOSupports::clearProgram(); }
  };
  virtual ~VAOSupports() = default;

  unsigned int vao, vbo, ibo;
  Handle reserve(long vboQuads, long iboQuads);
  void useProgram(const AppState &state, const std::string &progName) const;
  static void clearProgram();
  void bindVAO() const;
  static void clearVAO();
  VAOSupports(VAOBuffers bufferInfos);

private:
  VAOBuffers bufferInfos;
  static auto findFreeOffset(FreeList &freeList, long rows);
  void reallocate(long vboQuads, long iboQuads);
  void allocateBuffers();
  void allocateBuffers(unsigned int vboTarget, unsigned int iboTarget);
  void defragmentFreeLists();
};

#endif // GLEDITOR_VAO_SUPPORTS_H
