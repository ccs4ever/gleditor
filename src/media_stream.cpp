/**
 * @file media_stream.cpp
 * @brief Implementation of byte-accurate media streams and range slicing.
 */
#include <gleditor/media_stream.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gleditor {

// -- MediaStream --------------------------------------------------------------

std::vector<std::byte> MediaStream::readBytes(const std::uint64_t offset,
                                              const std::size_t count) {
  if (0 == count || offset >= size()) {
    return {};
  }
  const auto available =
      static_cast<std::size_t>(std::min<std::uint64_t>(count, size() - offset));
  std::vector<std::byte> buf(available);
  const auto actual = read(offset, buf);
  buf.resize(actual);
  return buf;
}

std::string MediaStream::readString(const std::uint64_t offset,
                                    const std::size_t count) {
  const auto bytes = readBytes(offset, count);
  if (bytes.empty()) {
    return {};
  }
  return std::string(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
}

// -- MemoryMediaStream --------------------------------------------------------

MemoryMediaStream::MemoryMediaStream(std::vector<std::byte> bytes)
    : storage(std::make_shared<const std::vector<std::byte>>(std::move(bytes))),
      range(ByteRange{0, storage->size()}) {}

MemoryMediaStream::MemoryMediaStream(const std::string_view text)
    : storage([text]() {
        auto vec = std::make_shared<std::vector<std::byte>>(text.size());
        if (!text.empty()) {
          std::memcpy(vec->data(), text.data(), text.size());
        }
        return vec;
      }()),
      range(ByteRange{0, storage->size()}) {}

MemoryMediaStream::MemoryMediaStream(
    std::shared_ptr<const std::vector<std::byte>> buffer,
    const std::uint64_t offset, const std::uint64_t length)
    : storage(std::move(buffer)) {
  const auto bufSize = storage ? storage->size() : 0;
  const auto from    = std::min<std::uint64_t>(offset, bufSize);
  const auto count   = std::min<std::uint64_t>(length, bufSize - from);
  range              = ByteRange{from, count};
}

std::size_t MemoryMediaStream::read(const std::uint64_t offset,
                                    std::span<std::byte> dst) {
  if (dst.empty() || offset >= range.length || nullptr == storage) {
    return 0;
  }
  const auto toRead = static_cast<std::size_t>(
      std::min<std::uint64_t>(dst.size(), range.length - offset));
  const auto srcStart = range.start + offset;
  std::memcpy(dst.data(), storage->data() + srcStart, toRead);
  return toRead;
}

std::shared_ptr<MediaStream>
MemoryMediaStream::subspan(const std::uint64_t offset,
                           const std::uint64_t length) {
  const auto clamped = range.slice(offset, length);
  return std::make_shared<MemoryMediaStream>(storage, clamped.start,
                                             clamped.length);
}

const std::byte *MemoryMediaStream::data() const {
  if (nullptr == storage || storage->empty() || 0 == range.length) {
    return nullptr;
  }
  return storage->data() + range.start;
}

// -- FileMediaStream ----------------------------------------------------------

namespace {

std::uint64_t queryFileSize(const std::string &filePath) {
  std::ifstream is(filePath, std::ios::binary | std::ios::ate);
  if (!is.is_open()) {
    return 0;
  }
  const auto pos = is.tellg();
  return pos > 0 ? static_cast<std::uint64_t>(pos) : 0;
}

} // namespace

FileMediaStream::FileMediaStream(std::string filePath)
    : path_(std::move(filePath)), range(ByteRange{0, queryFileSize(path_)}) {}

FileMediaStream::FileMediaStream(std::string filePath,
                                 const std::uint64_t fileOffset,
                                 const std::uint64_t length)
    : path_(std::move(filePath)) {
  const auto total = queryFileSize(path_);
  const auto from  = std::min(fileOffset, total);
  const auto count = std::min(length, total - from);
  range            = ByteRange{from, count};
}

std::size_t FileMediaStream::read(const std::uint64_t offset,
                                  std::span<std::byte> dst) {
  if (dst.empty() || offset >= range.length) {
    return 0;
  }
  std::ifstream is(path_, std::ios::binary);
  if (!is.is_open()) {
    return 0;
  }
  const auto absOffset = range.start + offset;
  is.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
  if (!is.good()) {
    return 0;
  }

  const auto toRead = static_cast<std::size_t>(
      std::min<std::uint64_t>(dst.size(), range.length - offset));
  is.read(reinterpret_cast<char *>(dst.data()),
          static_cast<std::streamsize>(toRead));
  return static_cast<std::size_t>(is.gcount());
}

std::shared_ptr<MediaStream>
FileMediaStream::subspan(const std::uint64_t offset,
                         const std::uint64_t length) {
  const auto clamped = range.slice(offset, length);
  return std::make_shared<FileMediaStream>(path_, clamped.start,
                                           clamped.length);
}

// -- CallbackMediaStream ------------------------------------------------------

CallbackMediaStream::CallbackMediaStream(ReadCallback callback,
                                         const std::uint64_t streamSize)
    : readCb(std::move(callback)), range(ByteRange{0, streamSize}) {}

CallbackMediaStream::CallbackMediaStream(ReadCallback callback,
                                         const std::uint64_t streamSize,
                                         const std::uint64_t baseOffset,
                                         const std::uint64_t length)
    : readCb(std::move(callback)) {
  const auto from  = std::min(baseOffset, streamSize);
  const auto count = std::min(length, streamSize - from);
  range            = ByteRange{from, count};
}

std::size_t CallbackMediaStream::read(const std::uint64_t offset,
                                      std::span<std::byte> dst) {
  if (dst.empty() || offset >= range.length || !readCb) {
    return 0;
  }
  const auto available = range.length - offset;
  const auto toRead    = std::min<std::size_t>(dst.size(), available);
  return readCb(range.start + offset, dst.subspan(0, toRead));
}

std::shared_ptr<MediaStream>
CallbackMediaStream::subspan(const std::uint64_t offset,
                             const std::uint64_t length) {
  const auto clamped = range.slice(offset, length);
  return std::make_shared<CallbackMediaStream>(
      readCb, range.start + range.length, clamped.start, clamped.length);
}

// -- VlcStreamState -----------------------------------------------------------

int VlcStreamState::open(void *opaque, void **dataptr, std::uint64_t *sizep) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (nullptr == state || nullptr == state->stream) {
    return -1;
  }
  *dataptr      = state;
  *sizep        = state->stream->size();
  state->cursor = 0;
  return 0;
}

std::ptrdiff_t VlcStreamState::read(void *opaque, unsigned char *buf,
                                    const std::size_t len) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (nullptr == state || nullptr == state->stream || nullptr == buf ||
      0 == len) {
    return 0;
  }
  const auto readBytes = state->stream->read(
      state->cursor,
      std::span<std::byte>(reinterpret_cast<std::byte *>(buf), len));
  state->cursor += readBytes;
  return static_cast<std::ptrdiff_t>(readBytes);
}

int VlcStreamState::seek(void *opaque, const std::uint64_t offset) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (nullptr == state || nullptr == state->stream) {
    return -1;
  }
  if (offset > state->stream->size()) {
    return -1;
  }
  state->cursor = offset;
  return 0;
}

void VlcStreamState::close(void *opaque) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (nullptr != state) {
    state->cursor = 0;
  }
}

} // namespace gleditor
