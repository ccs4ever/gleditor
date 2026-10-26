/**
 * @file media_stream.cpp
 * @brief Implementation of byte-accurate media streams and range slicing.
 */
#include <gleditor/media_stream.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

// -- MemoryMediaStream --------------------------------------------------------

MemoryMediaStream::MemoryMediaStream(std::vector<std::byte> bytes)
    : storage(std::make_shared<const std::vector<std::byte>>(std::move(bytes))),
      range(ByteRange{.start = 0, .length = storage->size()}) {}

MemoryMediaStream::MemoryMediaStream(const std::string_view text)
    : storage([text]() {
        auto vec = std::make_shared<std::vector<std::byte>>(text.size());
        if (!text.empty()) {
          std::memcpy(vec->data(), text.data(), text.size());
        }
        return vec;
      }()),
      range(ByteRange{.start = 0, .length = storage->size()}) {}

MemoryMediaStream::MemoryMediaStream(
    std::shared_ptr<const std::vector<std::byte>> buffer,
    const std::uint64_t offset, const std::uint64_t length)
    : storage(std::move(buffer)) {
  const auto bufSize = storage ? storage->size() : 0;
  const auto from    = std::min<std::uint64_t>(offset, bufSize);
  const auto count   = std::min<std::uint64_t>(length, bufSize - from);
  range              = ByteRange{.start = from, .length = count};
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
  std::error_code statusError;
  if (!std::filesystem::is_regular_file(filePath, statusError)) {
    throw std::ios_base::failure(
        "media stream requires a regular file: " + filePath,
        statusError ? statusError
                    : std::make_error_code(std::errc::invalid_argument));
  }
  std::ifstream is(filePath, std::ios::binary | std::ios::ate);
  if (!is.is_open()) {
    throw std::ios_base::failure("cannot open media stream file: " + filePath);
  }
  const auto pos = is.tellg();
  if (pos < 0) {
    throw std::ios_base::failure("cannot determine media stream size: " +
                                 filePath);
  }
  return static_cast<std::uint64_t>(pos);
}

} // namespace

FileMediaStream::FileMediaStream(std::string filePath)
    : path_(std::move(filePath)),
      range(ByteRange{.start = 0, .length = queryFileSize(path_)}) {}

FileMediaStream::FileMediaStream(std::string filePath,
                                 const std::uint64_t fileOffset,
                                 const std::uint64_t length)
    : path_(std::move(filePath)) {
  const auto total = queryFileSize(path_);
  const auto from  = std::min(fileOffset, total);
  const auto count = std::min(length, total - from);
  range            = ByteRange{.start = from, .length = count};
}

std::size_t FileMediaStream::read(const std::uint64_t offset,
                                  std::span<std::byte> dst) {
  if (dst.empty() || offset >= range.length) {
    return 0;
  }
  std::ifstream is(path_, std::ios::binary);
  if (!is.is_open()) {
    throw std::ios_base::failure("cannot read media stream file: " + path_);
  }
  const auto absOffset = range.start + offset;
  is.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
  if (!is.good()) {
    throw std::ios_base::failure("cannot seek media stream file: " + path_);
  }

  const auto toRead = static_cast<std::size_t>(
      std::min<std::uint64_t>(dst.size(), range.length - offset));
  is.read(reinterpret_cast<char *>(dst.data()),
          static_cast<std::streamsize>(toRead));
  if (is.bad() || (is.fail() && !is.eof())) {
    throw std::ios_base::failure("media stream read failed: " + path_);
  }
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
    : readCb(std::move(callback)),
      range(ByteRange{.start = 0, .length = streamSize}) {
  if (!readCb)
    throw std::invalid_argument("media stream requires a read callback");
}

CallbackMediaStream::CallbackMediaStream(ReadCallback callback,
                                         const std::uint64_t streamSize,
                                         const std::uint64_t baseOffset,
                                         const std::uint64_t length)
    : readCb(std::move(callback)) {
  if (!readCb)
    throw std::invalid_argument("media stream requires a read callback");
  const auto from  = std::min(baseOffset, streamSize);
  const auto count = std::min(length, streamSize - from);
  range            = ByteRange{.start = from, .length = count};
}

std::size_t CallbackMediaStream::read(const std::uint64_t offset,
                                      std::span<std::byte> dst) {
  if (dst.empty() || offset >= range.length) {
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
  if (!state || !state->stream || !dataptr || !sizep) return -1;
  try {
    const auto size = state->stream->size();
    *dataptr        = state;
    *sizep          = size;
    state->cursor   = 0;
    return 0;
  } catch (...) {
    return -1; // C callbacks report failures through LibVLC's ABI.
  }
}

std::ptrdiff_t VlcStreamState::read(void *opaque, unsigned char *buf,
                                    const std::size_t len) {
  if (len == 0) return 0;
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (!state || !state->stream || !buf) return -1;
  try {
    const auto readBytes = state->stream->read(
        state->cursor,
        std::span<std::byte>(reinterpret_cast<std::byte *>(buf), len));
    state->cursor += readBytes;
    return static_cast<std::ptrdiff_t>(readBytes);
  } catch (...) {
    return -1;
  }
}

int VlcStreamState::seek(void *opaque, const std::uint64_t offset) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (!state || !state->stream) return -1;
  try {
    if (offset > state->stream->size()) return -1;
    state->cursor = offset;
    return 0;
  } catch (...) {
    return -1;
  }
}

void VlcStreamState::close(void *opaque) {
  auto *state = static_cast<VlcStreamState *>(opaque);
  if (nullptr != state) {
    state->cursor = 0;
  }
}

} // namespace gleditor
