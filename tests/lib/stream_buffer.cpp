#include <gleditor/render/gl/stream_buffer.hpp>
#include <gtest/gtest.h>
#include <vector>

namespace render::gl {
namespace {

// Simulated GL state for unit testing StreamBufferGL logic without a GPU
// context
struct FakeGLContext {
  std::vector<std::uint8_t> bufferData;
  GLuint nextBufferId{1};
  std::uintptr_t nextFenceId{100};
  std::vector<GLsync> activeFences;
  std::vector<GLsync> waitedFences;
  std::vector<GLsync> deletedFences;

  static FakeGLContext *current;
};

FakeGLContext *FakeGLContext::current = nullptr;

GLApi createFakeGLApi() {
  GLApi api{};

  api.GenBuffers = [](GLsizei n, GLuint *buffers) {
    for (GLsizei i = 0; i < n; i++) {
      buffers[i] = FakeGLContext::current->nextBufferId++;
    }
  };

  api.DeleteBuffers = [](GLsizei /*n*/, const GLuint * /*buffers*/) {};
  api.BindBuffer    = [](GLenum /*target*/, GLuint /*buffer*/) {};

  api.BufferData = [](GLenum /*target*/, GLsizeiptr size, const void * /*data*/,
                      GLenum /*usage*/) {
    FakeGLContext::current->bufferData.resize(static_cast<std::size_t>(size),
                                              0);
  };

  api.MapBufferRange = [](GLenum /*target*/, GLintptr offset, GLsizeiptr size,
                          GLbitfield /*access*/) -> void * {
    if (static_cast<std::size_t>(offset + size) >
        FakeGLContext::current->bufferData.size()) {
      return nullptr;
    }
    return FakeGLContext::current->bufferData.data() + offset;
  };

  api.FlushMappedBufferRange = [](GLenum /*target*/, GLintptr /*offset*/,
                                  GLsizeiptr /*length*/) {};
  api.UnmapBuffer = [](GLenum /*target*/) -> GLboolean { return GL_TRUE; };

  api.FenceSync = [](GLenum /*condition*/, GLbitfield /*flags*/) -> GLsync {
    const auto sync =
        reinterpret_cast<GLsync>(FakeGLContext::current->nextFenceId++);
    FakeGLContext::current->activeFences.push_back(sync);
    return sync;
  };

  api.ClientWaitSync = [](GLsync sync, GLbitfield /*flags*/,
                          GLuint64 /*timeout*/) -> GLenum {
    FakeGLContext::current->waitedFences.push_back(sync);
    return GL_CONDITION_SATISFIED;
  };

  api.DeleteSync = [](GLsync sync) {
    FakeGLContext::current->deletedFences.push_back(sync);
  };

  return api;
}

class StreamBufferGLTest : public ::testing::Test {
protected:
  FakeGLContext fakeContext;
  GLApi fakeApi;

  void SetUp() override {
    FakeGLContext::current = &fakeContext;
    fakeApi                = createFakeGLApi();
  }

  void TearDown() override { FakeGLContext::current = nullptr; }
};

TEST_F(StreamBufferGLTest, SequentialAllocationsAlignProperly) {
  StreamBufferGL buffer(fakeApi, GL_ARRAY_BUFFER, 1024);

  const auto chunk1 = buffer.allocate(100, 64);
  EXPECT_NE(chunk1.ptr, nullptr);
  EXPECT_EQ(chunk1.offset, 0U);
  buffer.flushAndUnmap(chunk1.offset, 100);

  // Next allocation must be 64-byte aligned (128)
  const auto chunk2 = buffer.allocate(50, 64);
  EXPECT_NE(chunk2.ptr, nullptr);
  EXPECT_EQ(chunk2.offset, 128U);
  buffer.flushAndUnmap(chunk2.offset, 50);

  // Next allocation with 256-byte alignment
  const auto chunk3 = buffer.allocate(30, 256);
  EXPECT_NE(chunk3.ptr, nullptr);
  EXPECT_EQ(chunk3.offset, 256U);
  buffer.flushAndUnmap(chunk3.offset, 30);
}

TEST_F(StreamBufferGLTest, WrapsAroundWhenExceedingCapacity) {
  StreamBufferGL buffer(fakeApi, GL_ARRAY_BUFFER, 512);

  // Allocate 400 bytes at offset 0
  const auto chunk1 = buffer.allocate(400, 64);
  EXPECT_EQ(chunk1.offset, 0U);
  buffer.flushAndUnmap(chunk1.offset, 400);

  // Next 200 bytes will not fit in remaining 112 bytes -> wraps to head 0
  const auto chunk2 = buffer.allocate(200, 64);
  EXPECT_EQ(chunk2.offset, 0U);
  buffer.flushAndUnmap(chunk2.offset, 200);

  // Fence for chunk1 should have been waited on during wrap-around overlap
  EXPECT_FALSE(fakeContext.waitedFences.empty());
}

TEST_F(StreamBufferGLTest, RejectsAllocationLargerThanCapacity) {
  StreamBufferGL buffer(fakeApi, GL_ARRAY_BUFFER, 256);

  EXPECT_THROW((void)buffer.allocate(512, 64), std::runtime_error);
}

TEST_F(StreamBufferGLTest, RejectsZeroCapacity) {
  EXPECT_THROW(StreamBufferGL(fakeApi, GL_ARRAY_BUFFER, 0),
               std::invalid_argument);
}

TEST_F(StreamBufferGLTest, WaitAllReleasesActiveFences) {
  {
    StreamBufferGL buffer(fakeApi, GL_ARRAY_BUFFER, 1024);
    const auto chunk = buffer.allocate(64, 64);
    buffer.flushAndUnmap(chunk.offset, 64);
    EXPECT_EQ(fakeContext.activeFences.size(), 1U);
    EXPECT_EQ(fakeContext.deletedFences.size(), 0U);
    buffer.waitAll();
    EXPECT_EQ(fakeContext.deletedFences.size(), 1U);
  }
}

} // namespace
} // namespace render::gl
