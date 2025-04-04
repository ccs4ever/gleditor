#ifndef GLEDITOR_RENDERER_BUFFER_H
#define GLEDITOR_RENDERER_BUFFER_H

#include <concepts>
#include <cstdint>
#include <vector>

namespace gledit::renderer {

class AbstractBuffer {
public:
  // XXX: GL bias: Maps identically to glMapBufferRange access enum values,
  // other backends need to translate
  enum class Access : std::uint8_t {
    READ             = 1,
    WRITE            = 2,
    READWRITE        = 3,
    INVALIDATE_RANGE = 4,
    INVALIDATE       = 8,
    EXPLICIT_FLUSH   = 16,
    UNSYNC           = 32,
    PERSIST          = 64,
    COHERENT         = 128
  };
  enum class Usage : std::uint16_t {
    STREAM_DRAW  = 0x88E0,
    STREAM_READ  = 0x88E1,
    STREAM_COPY  = 0x88E2,
    STATIC_DRAW  = 0x88E4,
    STATIC_READ  = 0x88E5,
    STATIC_COPY  = 0x88E6,
    DYNAMIC_DRAW = 0x88E8,
    DYNAMIC_READ = 0x88E9,
    DYNAMIC_COPY = 0x88EA
  };

protected:
public:
  struct Binder {
    AbstractBuffer *self;
    Binder(AbstractBuffer *self) : self(self) { self->bind(); }
    ~Binder() { self->unbind(); }
  };
  struct Mapper {
    AbstractBuffer *self;
    template <typename Elem>
    Mapper(AbstractBuffer* self, Access access, int startElem = 0,
           int numElems = -1);
    ~Mapper();
  };
  virtual void bind()   = 0;
  virtual void unbind() = 0;
  template <typename Self>
  void bound(this Self &&self, std::invocable<void(Self *)> auto fun) {
    Binder bind(self);
    fun(self);
  }
  virtual void alloc(int numElems, Usage usage)                   = 0;
  virtual void alloc(std::vector<Elem> elems, Usage usage)        = 0;
  virtual void insert(std::vector<Elem> elems, int startElem = 0) = 0;
  virtual void dealloc()                                          = 0;
  virtual void realloc(int numElems)                              = 0;
  virtual void copy(AbstractBuffer &oth)                          = 0;
  virtual ~AbstractBuffer()                                       = default;
};

template <typename Elem, typename Handle> class BaseBuffer : AbstractBuffer {
public:
  using base   = AbstractBuffer;
  using Access = base::Access;
  using Binder = base::Binder;
  using Mapper = base::Mapper;
  using Usage  = base::Usage;

protected:
  Handle handle;
  std::vector<Elem> buffer;
  virtual void destroy()                                                 = 0;
  virtual void create()                                                  = 0;
  virtual Elem *map(Access access, int startElem = 0, int numElems = -1) = 0;
  virtual void unmap()                                                   = 0;

public:
  virtual operator bool() { return !buffer.empty(); }
  [[nodiscard]] constexpr int numElems() const noexcept {
    return buffer.size();
  }
  [[nodiscard]] constexpr int bytes() const noexcept {
    return buffer.size() * sizeof(Elem);
  }
  [[nodiscard]] constexpr int stride() const noexcept { return sizeof(Elem); }
  BaseBuffer(BaseBuffer &&oth) noexcept {
    destroy();
    this->handle = oth.handle;
    this->buffer = std::move(oth.buffer);
    oth.handle   = 0;
  }
  virtual void operator=(BaseBuffer &&oth) noexcept {
    destroy();
    this->handle = oth.handle;
    this->buffer = std::move(oth.buffer);
    oth.handle   = 0;
  }
  BaseBuffer() { create(); }
  ~BaseBuffer() override {
    this->unbind();
    destroy();
  }
};

} // namespace gledit::renderer

#endif // GLEDITOR_RENDERER_BUFFER_H
