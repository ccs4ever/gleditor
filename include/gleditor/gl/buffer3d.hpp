//
// Created by ccs4ever on 11/23/25.
//

#ifndef GLEDITOR_BUFFER3D_HPP
#define GLEDITOR_BUFFER3D_HPP
#include <GL/glew.h>
#include <bitset>
#include <glibmm-2.68/glibmm/unicode.h>
#include <memory>
#include <span>
#include <stdexcept>

class Buffer3D {

public:
  enum class Type : int {
    Frame, Vertex, Element, Query, TransformFeedback, Uniform, Shader, PixelPack, PixelUnpack, Read, Write
  };
  enum class Access : int { Read, Write, ReadWrite };
  enum class Usage : unsigned char { Dynamic = 1, Read, Write, Coherent, Persistent, Client };
  Buffer3D(Type type, long initialNumElems) : type(type), maxElems(initialNumElems) {}
  virtual ~Buffer3D() = default;
protected:
  Type type;
  long maxElems;

};


template <typename Elem>
class Buffer3DTyped : public Buffer3D {
public: // types
using base = Buffer3D;
private:
  int elemSize = sizeof(Elem);
protected:
  inline long bytes(int numElems) const { return numElems * elemSize; }
  inline long size() const { return this->bytes(maxElems); }

public:
  Buffer3DTyped(Type type, long initialNumElems) : Buffer3D(type, initialNumElems) {}
  ~Buffer3DTyped() override = default;

  //virtual std::unique_ptr <Elem> map(int startElem, int numElems) = 0;
};


template <typename Elem>
class Buffer3DGL : public Buffer3DTyped<Elem> {
public: // types
  using base = Buffer3DTyped<Elem>;
  using Type = typename base::Type;
  using Access = typename base::Access;
  using Usage = typename base::Usage;

private:
  GLuint id;

  inline GLuint target() { return target(this->type); }
  inline GLuint target(Type type) {
    switch (type) {
      case Type::Vertex: return GL_ARRAY_BUFFER;
      case Type::Uniform: return GL_UNIFORM_BUFFER;
      case Type::Shader: return GL_SHADER_STORAGE_BUFFER;
      case Type::PixelPack: return GL_PIXEL_PACK_BUFFER;
      case Type::PixelUnpack: return GL_PIXEL_UNPACK_BUFFER;
      case Type::Read: return GL_COPY_READ_BUFFER;
      case Type::Write: return GL_COPY_WRITE_BUFFER;
      case Type::Query: return GL_QUERY_BUFFER;
      case Type::TransformFeedback: return GL_TRANSFORM_FEEDBACK_BUFFER;
      default: throw new std::runtime_error("Unexpected GL target type: " + std::to_string(this->type));
      }
  }
  inline GLuint access(Access access) {
    switch (access) {
      case Access::Read: return GL_READ_ONLY;
      case Access::Write: return GL_WRITE_ONLY;
      case Access::ReadWrite: return GL_READ_WRITE;
    default: throw new std::runtime_error("Unexpected GL access type: " + std::to_string(access));
    }
  }

  inline GLuint usage(Usage usage) {
    GLuint ret{};
    if (Usage::Dynamic & usage) ret |= GL_DYNAMIC_STORAGE_BIT;
    if (Usage::Read & usage) ret |= GL_MAP_READ_BIT;
    if (Usage::Write & usage) ret |= GL_MAP_WRITE_BIT;
    if (Usage::Coherent & usage) ret |= GL_MAP_COHERENT_BIT;
    if (Usage::Persistent & usage) ret |= GL_MAP_PERSISTENT_BIT;
    switch (usage) {

      default: throw std::runtime_error("Unexpected GL usage: " + std::to_string(usage));
    }
  }

public:

  Buffer3DGL(Type type, Usage anUsage, long initialNumElems) : Buffer3DTyped<Elem>(type, initialNumElems) {
    glCreateBuffers(1, &id);
    glNamedBufferStorage(id, this->bytes(initialNumElems), nullptr, usage(anUsage));
  }
  ~Buffer3DGL() override {
    glDeleteBuffers(1, &id);
  }

  void makeReady() {
    glBindBuffer(target(), id);
  }

  void done() {
    glBindBuffer(target(), 0);
  }

  std::span<Elem> map(int startElem, int numElems, Access access) {
    if (Type::Frame == this->type)
      throw new std::runtime_error("GL Framebuffer is not mappable");
    Elem* mapped = static_cast<Elem*>(glMapNamedBufferRange(id,
      this->bytes(startElem), this->bytes(numElems), access(access)));
    if (nullptr == mapped) throw new std::runtime_error("Failed to map buffer range: " +
      std::to_string(this->type) + " start: " + startElem + " length: " + numElems);
    return std::span<Elem>(mapped, numElems);;
  }

  void unmap(std::span<Elem>& mapped) {
    glUnmapNamedBuffer(id);
  }
};

#endif // GLEDITOR_BUFFER3D_HPP
