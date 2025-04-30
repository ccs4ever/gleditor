#ifndef GLEDITOR_GL_BUFFER_H
#define GLEDITOR_GL_BUFFER_H

#include "../renderer/buffer.hpp"
#include <GL/glew.h>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gledit::gl {

template <typename Elem>
class GLBuffer : public renderer::BaseBuffer<Elem> {
public:
  using base   = renderer::BaseBuffer<Elem>;
  using Access = base::Access;
  using Binder = base::Binder;
  using Mapper = base::Mapper;
  using Usage  = base::Usage;

private:
  unsigned int target;
  static const std::unordered_map<int, int> bufTargetToBinding;

  unsigned int asGLAccess(Access access) { return std::to_underlying(access); }

protected:
  void destroy() override {
    glDeleteBuffers(1, &this->handle);
    this->handle = 0;
  }
  void create() override { glGenBuffers(1, &this->handle); }
  Elem *map(Access access, int startElem = 0, int numElems = -1) override {
    const int size = this->buffer.size();
    if (numElems < 0) {
      numElems = this->buffer.size();
    }
    if (startElem < 0 || (startElem + numElems) > size) {
      throw std::out_of_range("Cannot map outside the range of the buffer");
    }
    unsigned int glAccess = asGLAccess(access);
    Elem *ret             = static_cast<Elem *>(
        glMapBufferRange(this->target, startElem * sizeof(Elem),
                                     numElems * sizeof(Elem), glAccess));
    return ret;
  }
  void unmap() override { glUnmapBuffer(this->target); }

public:
  GLBuffer(unsigned int target) : base(), target(target) {}
  GLBuffer(GLBuffer &&oth) noexcept : base(oth) {}
  virtual ~GLBuffer() = default;
  void alloc(int numElems, Usage usage) override {
    this->buffer.reserve(numElems);
    glBufferData(this->target, numElems * sizeof(Elem), nullptr,
                 std::to_underlying(usage));
  }
  void alloc(std::vector<Elem> &elems, Usage usage) override {
    this->buffer = std::move(elems);
    glBufferData(this->target, elems.size() * sizeof(Elem), elems.data(),
                 std::to_underlying(usage));
  }
  void insert(std::vector<Elem> elems, int startElem = 0) override {
    if ((startElem + elems.size()) > this->buffer.size()) {
      int reserve = (this->buffer.size() + elems.size()) * 2;
      realloc(reserve);
    }
    this->buffer.insert(this->buffer.begin() + startElem, elems);
    glBufferSubData(this->target, startElem * sizeof(Elem),
                    elems.size() * sizeof(Elem), elems.data());
  }
  // void dealloc() override {}
  void copy(base &oth) override {

    assert(this->buffer::element_type == oth.buffer::element_type,
           "Cannot copy buffer data from incompatible types");

    int oldReadHandle{};
    int oldOtherHandle{};
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &oldReadHandle);
    glGetIntegerv(bufTargetToBinding.at(oth.target), &oldOtherHandle);
    
    glBindBuffer(GL_COPY_READ_BUFFER, this->handle);
    glBindBuffer(oth.target, oth.handle);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, oth.target, 0, 0,
                        this->bytes());
    
    glBindBuffer(GL_COPY_READ_BUFFER, oldReadHandle);
    glBindBuffer(oth.target, oldOtherHandle);
  }
  void realloc(int numElems) override {
    GLBuffer buf(this->target);
    buf.alloc(numElems);
    this->copy(buf);
    *this = std::move(buf);
  }
  void bind() override { glBindBuffer(this->target, this->handle); }
  void unbind() override { glBindBuffer(this->target, 0); }

};

template <typename Elem>
  const std::unordered_map<int, int> GLBuffer<Elem>::bufTargetToBinding{
{GL_ARRAY_BUFFER, GL_ARRAY_BUFFER_BINDING},
{GL_ATOMIC_COUNTER_BUFFER, GL_ATOMIC_COUNTER_BUFFER_BINDING},
{GL_COPY_READ_BUFFER, GL_COPY_READ_BUFFER_BINDING},
{GL_COPY_WRITE_BUFFER, GL_COPY_WRITE_BUFFER_BINDING},
{GL_DRAW_INDIRECT_BUFFER, GL_DRAW_INDIRECT_BUFFER_BINDING},
{GL_DISPATCH_INDIRECT_BUFFER, GL_DISPATCH_INDIRECT_BUFFER_BINDING},
{GL_ELEMENT_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER_BINDING},
{GL_PIXEL_PACK_BUFFER, GL_PIXEL_PACK_BUFFER_BINDING},
{GL_PIXEL_UNPACK_BUFFER, GL_PIXEL_UNPACK_BUFFER_BINDING},
{GL_SHADER_STORAGE_BUFFER, GL_SHADER_STORAGE_BUFFER_BINDING},
{GL_TRANSFORM_FEEDBACK_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING},
{GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER_BINDING}
  };

} // namespace gledit::gl

#endif // GLEDITOR_GL_BUFFER_H
