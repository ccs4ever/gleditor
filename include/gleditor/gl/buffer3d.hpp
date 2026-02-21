//
// Created by ccs4ever on 11/23/25.
//

/**
 * @file buffer3d.hpp
 * @brief Provides a wrapper for OpenGL buffer objects.
 */

#ifndef GLEDITOR_BUFFER3D_HPP
#define GLEDITOR_BUFFER3D_HPP
#include <GL/glew.h>
#include <bitset>
#include <span>
#include <stdexcept>
#include <typeinfo>

/**
 * @brief Template class for typed 3D buffers.
 * @tparam Elem The type of elements in the buffer.
 */
template <typename Elem> class Buffer3DTyped;

/**
 * @brief Base class for 3D buffers.
 */
class Buffer3D {

public:
  /**
   * @brief Enumeration of buffer types.
   */
  enum class Type : std::uint8_t {
    Frame,             ///< Framebuffer
    Vertex,            ///< Vertex buffer
    Element,           ///< Element (index) buffer
    Query,             ///< Query buffer
    TransformFeedback, ///< Transform feedback buffer
    Uniform,           ///< Uniform buffer
    Shader,            ///< Shader storage buffer
    PixelPack,         ///< Pixel pack buffer
    PixelUnpack,       ///< Pixel unpack buffer
    Read,              ///< Copy read buffer
    Write              ///< Copy write buffer
  };

  /**
   * @brief Enumeration of access types for mapping buffers.
   */
  enum class Access : std::uint8_t {
    Read,     ///< Read-only access
    Write,    ///< Write-only access
    ReadWrite ///< Read and write access
  };

  /**
   * @brief Enumeration of usage flags for buffer storage.
   */
  enum class Usage : unsigned char {
    Dynamic    = 1,  ///< Contents can be updated after creation
    Read       = 2,  ///< Buffer will be mapped for reading
    Write      = 4,  ///< Buffer will be mapped for writing
    Coherent   = 8,  ///< Map should be coherent
    Persistent = 16, ///< Map should be persistent
    Client     = 32, ///< Prefer client-side storage
    End        = 64  ///< End marker for validation
  };

  /**
   * @brief Constructs a Buffer3D.
   * @param type The type of the buffer.
   * @param initialNumElems Initial number of elements.
   */
  Buffer3D(const Type type, const long initialNumElems)
      : type(type), maxElems(initialNumElems) {}

  /**
   * @brief Virtual destructor.
   */
  virtual ~Buffer3D() = default;

  /**
   * @brief Casts this buffer to a typed buffer.
   * @tparam Elem The type of elements in the buffer.
   * @return A reference to the typed buffer.
   * @throws std::bad_cast if the buffer is not of the specified type.
   */
  template <typename Elem> Buffer3DTyped<Elem> &typed() {
    return dynamic_cast<Buffer3DTyped<Elem> &>(*this);
  }

protected:
  Type type;     ///< The type of the buffer.
  long maxElems; ///< Maximum number of elements in the buffer.
};

/**
 * @brief Template class for typed 3D buffers.
 * @tparam Elem The type of elements in the buffer.
 */
template <typename Elem> class Buffer3DTyped : public Buffer3D {
public:
  using base = Buffer3D;

  /**
   * @brief Constructs a Buffer3DTyped.
   * @param type The type of the buffer.
   * @param initialNumElems Initial number of elements.
   */
  Buffer3DTyped(const Type type, const long initialNumElems)
      : Buffer3D(type, initialNumElems) {}

  /**
   * @brief Virtual destructor.
   */
  ~Buffer3DTyped() override = default;

  /**
   * @brief Maps a range of the buffer into the client's address space.
   * @param startElem The starting element index.
   * @param numElems The number of elements to map.
   * @param access The access mode for the mapping.
   * @return A span representing the mapped range.
   */
  virtual std::span<Elem> map(int startElem, int numElems, Access access) = 0;

private:
  const int elemSize = sizeof(Elem); ///< Size of a single element in bytes.

protected:
  /**
   * @brief Calculates the number of bytes for a given number of elements.
   * @param numElems The number of elements.
   * @return The number of bytes.
   */
  [[nodiscard]] long bytes(const int numElems) const {
    return numElems * elemSize;
  }

  /**
   * @brief Gets the total size of the buffer in bytes.
   * @return The size of the buffer in bytes.
   */
  [[nodiscard]] long size() const { return this->bytes(maxElems); }
};

/**
 * @brief OpenGL implementation of Buffer3DTyped.
 * @tparam Elem The type of elements in the buffer.
 */
template <typename Elem> class Buffer3DGL : public Buffer3DTyped<Elem> {
public: // types
  using base   = Buffer3DTyped<Elem>;
  using Type   = base::Type;
  using Access = base::Access;
  using Usage  = base::Usage;

private:
  GLuint id{}; ///< OpenGL buffer object ID.

  /**
   * @brief Gets the OpenGL target for the current buffer type.
   * @return The OpenGL buffer target.
   */
  GLuint target() { return gl(this->type); }

  /**
   * @brief Translates Type to OpenGL target.
   * @param type The buffer type.
   * @return The OpenGL buffer target.
   */
  GLuint gl(Type type) {
    switch (type) {
    case Type::Vertex:
      return GL_ARRAY_BUFFER;
    case Type::Uniform:
      return GL_UNIFORM_BUFFER;
    case Type::Shader:
      return GL_SHADER_STORAGE_BUFFER;
    case Type::PixelPack:
      return GL_PIXEL_PACK_BUFFER;
    case Type::PixelUnpack:
      return GL_PIXEL_UNPACK_BUFFER;
    case Type::Read:
      return GL_COPY_READ_BUFFER;
    case Type::Write:
      return GL_COPY_WRITE_BUFFER;
    case Type::Query:
      return GL_QUERY_BUFFER;
    case Type::TransformFeedback:
      return GL_TRANSFORM_FEEDBACK_BUFFER;
    default:
      throw std::runtime_error("Unexpected GL target type: " +
                               std::to_string(this->type));
    }
  }

  /**
   * @brief Translates Access to OpenGL access flags.
   * @param access The access type.
   * @return The OpenGL access flags.
   */
  GLuint gl(Access access) {
    switch (access) {
    case Access::Read:
      return GL_READ_ONLY;
    case Access::Write:
      return GL_WRITE_ONLY;
    case Access::ReadWrite:
      return GL_READ_WRITE;
    default:
      throw std::runtime_error("Unexpected GL access type: " +
                               std::to_string(access));
    }
  }

  /**
   * @brief Translates Usage to OpenGL storage flags.
   * @param usage The usage flags.
   * @return The OpenGL storage flags.
   */
  GLuint gl(Usage usage) {
    GLuint ret{};
    if (0 == usage || usage >= Usage::End)
      throw std::runtime_error("Unexpected GL usage: " + std::to_string(usage));
    if (Usage::Persistent & usage &&
        !(Usage::Read & usage || Usage::Write & usage))
      throw std::runtime_error(
          "Persistent buffer must also be at least one of Read or Write");
    if (Usage::Coherent & usage && !(Usage::Persistent & usage))
      throw std::runtime_error("Coherent buffer must also be Persistent");
    if (Usage::Dynamic & usage) ret |= GL_DYNAMIC_STORAGE_BIT;
    if (Usage::Read & usage) ret |= GL_MAP_READ_BIT;
    if (Usage::Write & usage) ret |= GL_MAP_WRITE_BIT;
    if (Usage::Coherent & usage) ret |= GL_MAP_COHERENT_BIT;
    if (Usage::Persistent & usage) ret |= GL_MAP_PERSISTENT_BIT;
    if (Usage::Client & usage) ret |= GL_CLIENT_STORAGE_BIT;
    return ret;
  }

  /**
   * @brief Implementation of buffer mapping.
   * @tparam N The number of elements to map.
   * @param startElem The starting element index.
   * @param access The access mode.
   * @return A span representing the mapped range.
   */
  template <std::size_t N>
  std::span<Elem, N> map_impl(const int startElem, const Access access) {
    if (0 == N || std::dynamic_extent == N) N = this->size();
    if (Type::Frame == this->type)
      throw std::runtime_error("GL Framebuffer is not mappable");
    Elem *mapped = static_cast<Elem *>(glMapNamedBufferRange(
        id, this->bytes(startElem), this->bytes(N), gl(access)));
    if (nullptr == mapped)
      throw std::runtime_error(
          "Failed to map buffer range: " + std::to_string(this->type) +
          " start: " + startElem + " length: " + N);
    return std::span<Elem, N>(mapped, N);
  }

public:
  /**
   * @brief Constructs a Buffer3DGL.
   * @param type The type of the buffer.
   * @param anUsage The usage flags for the buffer.
   * @param initialNumElems Initial number of elements.
   */
  Buffer3DGL(Type type, Usage anUsage, long initialNumElems)
      : Buffer3DTyped<Elem>(type, initialNumElems) {
    glCreateBuffers(1, &id);
    glNamedBufferStorage(id, this->bytes(initialNumElems), nullptr, gl(anUsage));
  }

  /**
   * @brief Destructor. Deletes the OpenGL buffer object.
   */
  ~Buffer3DGL() override { glDeleteBuffers(1, &id); }

  /**
   * @brief Binds the buffer to its target.
   */
  void makeReady() { glBindBuffer(target(), id); }

  /**
   * @brief Unbinds the buffer from its target.
   */
  void done() { glBindBuffer(target(), 0); }

  /**
   * @brief Maps a range of the buffer.
   * @param startElem The starting element index.
   * @param numElems The number of elements to map.
   * @param access The access mode.
   * @return A span representing the mapped range.
   */
  std::span<Elem> map(const int startElem, const int numElems,
                      const Access access) override {
    return map_impl<numElems>(startElem, access);
  }

  /**
   * @brief Unmaps the buffer.
   * @param _ Unused span reference (placeholder for API consistency).
   */
  void unmap(std::span<Elem> &_) { glUnmapNamedBuffer(id); }
};

/**
 * @brief Bitwise OR operator for Buffer3D::Usage.
 */
constexpr Buffer3D::Usage operator|(Buffer3D::Usage lhs, Buffer3D::Usage rhs) {
  return static_cast<Buffer3D::Usage>(
      static_cast<std::underlying_type_t<Buffer3D::Usage>>(lhs) |
      static_cast<std::underlying_type_t<Buffer3D::Usage>>(rhs));
}

/**
 * @brief Bitwise AND operator for Buffer3D::Usage.
 */
constexpr Buffer3D::Usage operator&(Buffer3D::Usage lhs, Buffer3D::Usage rhs) {
  return static_cast<Buffer3D::Usage>(
      static_cast<std::underlying_type_t<Buffer3D::Usage>>(lhs) &
      static_cast<std::underlying_type_t<Buffer3D::Usage>>(rhs));
}

/**
 * @brief Bitwise NOT operator for Buffer3D::Usage.
 */
constexpr Buffer3D::Usage operator~(Buffer3D::Usage rhs) {
  return static_cast<Buffer3D::Usage>(
      ~static_cast<std::underlying_type_t<Buffer3D::Usage>>(rhs));
}

#endif // GLEDITOR_BUFFER3D_HPP
