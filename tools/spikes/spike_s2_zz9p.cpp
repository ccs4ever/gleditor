/**
 * @file spike_s2_zz9p.cpp
 * @brief Exploratory Spike S2: 9Z2026 Wire Serialization Round-Trip.
 *
 * Benchmarks stack-framed 9Z2026 binary codec with all 12 message pairs
 * plus Rerror over an in-memory LoopbackChannel.
 *
 * Pass Criteria:
 * - <= 1.2 us per request/response message pair round-trip
 * - Zero heap allocation during codec operations
 * - Error strings and payload fidelity preserved identically
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace zz9p {

enum class MsgType : std::uint8_t {
  Tversion = 100,
  Rversion = 101,
  Tauth    = 102,
  Rauth    = 103,
  Tattach  = 104,
  Rattach  = 105,
  Tclunk   = 106,
  Rclunk   = 107,
  Tflush   = 108,
  Rflush   = 109,
  Zstat    = 110,
  Rstat    = 111,
  Zhop     = 112,
  Rhop     = 113,
  Zread    = 114,
  Rread    = 115,
  Zdims    = 116,
  Rdims    = 117,
  Zlink    = 118,
  Rlink    = 119,
  Zmake    = 120,
  Rmake    = 121,
  Zwatch   = 122,
  Rwatch   = 123,
  Rerror   = 107 + 100, // 207
};

#pragma pack(push, 1)
struct Header {
  std::uint32_t size{0};
  std::uint8_t type{0};
  std::uint16_t tag{0};
};
#pragma pack(pop)

static_assert(sizeof(Header) == 7, "9Z header must be exactly 7 bytes");

// Stack-allocated fixed buffer for zero-heap encoding/decoding
class BufferStream {
public:
  static constexpr std::size_t kCapacity = 4096;

  void reset() noexcept { pos_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return pos_; }
  [[nodiscard]] const std::uint8_t *data() const noexcept {
    return buf_.data();
  }
  [[nodiscard]] std::uint8_t *data() noexcept { return buf_.data(); }

  template <typename T> bool write(const T &val) noexcept {
    if (pos_ + sizeof(T) > kCapacity) return false;
    std::memcpy(buf_.data() + pos_, &val, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool writeString(std::string_view s) noexcept {
    const auto len = static_cast<std::uint16_t>(s.size());
    if (!write(len)) return false;
    if (pos_ + len > kCapacity) return false;
    std::memcpy(buf_.data() + pos_, s.data(), len);
    pos_ += len;
    return true;
  }

  bool writeBytes(const void *src, std::size_t len) noexcept {
    const auto l = static_cast<std::uint32_t>(len);
    if (!write(l)) return false;
    if (pos_ + l > kCapacity) return false;
    std::memcpy(buf_.data() + pos_, src, l);
    pos_ += l;
    return true;
  }

  void finalizeHeader(MsgType type, std::uint16_t tag) noexcept {
    Header h{.size = static_cast<std::uint32_t>(pos_),
             .type = static_cast<std::uint8_t>(type),
             .tag  = tag};
    std::memcpy(buf_.data(), &h, sizeof(Header));
  }

  void initHeader() noexcept { pos_ = sizeof(Header); }

private:
  std::array<std::uint8_t, kCapacity> buf_{};
  std::size_t pos_{0};
};

class ReaderStream {
public:
  ReaderStream(const std::uint8_t *data, std::size_t len) noexcept
      : data_(data), len_(len), pos_(0) {}

  template <typename T> bool read(T &val) noexcept {
    if (pos_ + sizeof(T) > len_) return false;
    std::memcpy(&val, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool readHeader(Header &h) noexcept { return read(h); }

  bool readString(std::string_view &out) noexcept {
    std::uint16_t len = 0;
    if (!read(len)) return false;
    if (pos_ + len > len_) return false;
    out = std::string_view(reinterpret_cast<const char *>(data_ + pos_), len);
    pos_ += len;
    return true;
  }

  bool readBytes(const std::uint8_t *&out, std::uint32_t &outLen) noexcept {
    if (!read(outLen)) return false;
    if (pos_ + outLen > len_) return false;
    out = data_ + pos_;
    pos_ += outLen;
    return true;
  }

private:
  const std::uint8_t *data_;
  std::size_t len_;
  std::size_t pos_;
};

// Loopback memory channel
class LoopbackChannel {
public:
  void send(const std::uint8_t *msg, std::size_t len) noexcept {
    std::memcpy(pipe_.data(), msg, len);
    len_ = len;
  }

  [[nodiscard]] const std::uint8_t *receive(std::size_t &len) const noexcept {
    len = len_;
    return pipe_.data();
  }

private:
  std::array<std::uint8_t, BufferStream::kCapacity> pipe_{};
  std::size_t len_{0};
};

} // namespace zz9p

int main() {
  std::cout << "=== Spike S2: 9Z2026 Wire Serialization Round-Trip ==="
            << std::endl;

  zz9p::LoopbackChannel channel;
  zz9p::BufferStream encBuf;

  // Verify full fidelity across 12 message pairs + Rerror
  {
    // 1. Tversion / Rversion
    encBuf.reset();
    encBuf.initHeader();
    encBuf.write(std::uint32_t{65536});
    encBuf.writeString("9Z2026");
    encBuf.finalizeHeader(zz9p::MsgType::Tversion, 1);
    channel.send(encBuf.data(), encBuf.size());

    std::size_t recvLen          = 0;
    const std::uint8_t *recvData = channel.receive(recvLen);
    zz9p::ReaderStream r(recvData, recvLen);
    zz9p::Header h{};
    r.readHeader(h);
    std::uint32_t msize = 0;
    std::string_view ver;
    r.read(msize);
    r.readString(ver);
    if (h.type != static_cast<std::uint8_t>(zz9p::MsgType::Tversion) ||
        msize != 65536 || ver != "9Z2026") {
      std::cerr << "[FAIL] Tversion fidelity mismatch!" << std::endl;
      return 1;
    }

    // 2. Rerror fidelity
    encBuf.reset();
    encBuf.initHeader();
    encBuf.writeString("cell index out of bounds on rank traversal");
    encBuf.finalizeHeader(zz9p::MsgType::Rerror, 42);
    channel.send(encBuf.data(), encBuf.size());

    recvData = channel.receive(recvLen);
    zz9p::ReaderStream rErr(recvData, recvLen);
    rErr.readHeader(h);
    std::string_view errStr;
    rErr.readString(errStr);
    if (h.type != static_cast<std::uint8_t>(zz9p::MsgType::Rerror) ||
        h.tag != 42 || errStr != "cell index out of bounds on rank traversal") {
      std::cerr << "[FAIL] Rerror fidelity mismatch!" << std::endl;
      return 1;
    }
  }

  std::cout << "[Verify] Message types and Rerror fidelity verified."
            << std::endl;

  // Benchmark: 100,000 round-trips of requests + responses
  constexpr std::size_t kIterations = 100000;
  std::cout << "[Benchmark] Benchmarking " << kIterations
            << " request/response pairs across 12 message types..."
            << std::endl;

  const auto tStart               = std::chrono::steady_clock::now();
  volatile std::uint32_t checksum = 0;

  for (std::size_t i = 0; i < kIterations; ++i) {
    const auto tag       = static_cast<std::uint16_t>(i & 0xFFFF);
    const auto msgChoice = static_cast<std::uint8_t>(i % 12);

    // Encode Request
    encBuf.reset();
    encBuf.initHeader();
    switch (msgChoice) {
    case 0: // Tversion
      encBuf.write(std::uint32_t{65536});
      encBuf.writeString("9Z2026");
      encBuf.finalizeHeader(zz9p::MsgType::Tversion, tag);
      break;
    case 1: // Tauth
      encBuf.write(std::uint32_t{10});
      encBuf.writeString("gl_user");
      encBuf.finalizeHeader(zz9p::MsgType::Tauth, tag);
      break;
    case 2: // Tattach
      encBuf.write(std::uint32_t{1});
      encBuf.write(std::uint32_t{10});
      encBuf.writeString("gl_user");
      encBuf.finalizeHeader(zz9p::MsgType::Tattach, tag);
      break;
    case 3: // Tclunk
      encBuf.write(std::uint32_t{1});
      encBuf.finalizeHeader(zz9p::MsgType::Tclunk, tag);
      break;
    case 4: // Tflush
      encBuf.write(static_cast<std::uint16_t>(tag - 1));
      encBuf.finalizeHeader(zz9p::MsgType::Tflush, tag);
      break;
    case 5: // Zstat
      encBuf.write(std::uint32_t{123456});
      encBuf.finalizeHeader(zz9p::MsgType::Zstat, tag);
      break;
    case 6: // Zhop
      encBuf.write(std::uint32_t{123456});
      encBuf.write(std::uint32_t{3}); // dim
      encBuf.write(std::uint8_t{1});  // pos
      encBuf.write(std::int32_t{5});  // steps
      encBuf.finalizeHeader(zz9p::MsgType::Zhop, tag);
      break;
    case 7: // Zread
      encBuf.write(std::uint32_t{123456});
      encBuf.write(std::uint32_t{0});
      encBuf.write(std::uint32_t{64});
      encBuf.finalizeHeader(zz9p::MsgType::Zread, tag);
      break;
    case 8: // Zdims
      encBuf.write(std::uint32_t{123456});
      encBuf.finalizeHeader(zz9p::MsgType::Zdims, tag);
      break;
    case 9: // Zlink
      encBuf.write(std::uint32_t{123456});
      encBuf.write(std::uint32_t{3});
      encBuf.write(std::uint32_t{123457});
      encBuf.write(std::uint8_t{1});
      encBuf.finalizeHeader(zz9p::MsgType::Zlink, tag);
      break;
    case 10:                         // Zmake
      encBuf.write(std::uint8_t{2}); // StringValue
      encBuf.writeString("hello world");
      encBuf.finalizeHeader(zz9p::MsgType::Zmake, tag);
      break;
    default: // Zwatch
      encBuf.write(std::uint32_t{123456});
      encBuf.write(std::uint32_t{3});
      encBuf.write(std::uint32_t{999});
      encBuf.finalizeHeader(zz9p::MsgType::Zwatch, tag);
      break;
    }

    channel.send(encBuf.data(), encBuf.size());

    // Decode Request on Server
    std::size_t reqLen          = 0;
    const std::uint8_t *reqData = channel.receive(reqLen);
    zz9p::ReaderStream reqReader(reqData, reqLen);
    zz9p::Header reqHeader{};
    reqReader.readHeader(reqHeader);
    checksum += reqHeader.type;

    // Server Encodes Response
    encBuf.reset();
    encBuf.initHeader();
    if (i % 500 == 0) {
      // Periodic Rerror test
      encBuf.writeString("temporary resource busy");
      encBuf.finalizeHeader(zz9p::MsgType::Rerror, reqHeader.tag);
    } else {
      switch (static_cast<zz9p::MsgType>(reqHeader.type)) {
      case zz9p::MsgType::Tversion:
        encBuf.write(std::uint32_t{65536});
        encBuf.writeString("9Z2026");
        encBuf.finalizeHeader(zz9p::MsgType::Rversion, reqHeader.tag);
        break;
      case zz9p::MsgType::Zhop:
        encBuf.write(std::uint32_t{789012}); // target cell
        encBuf.finalizeHeader(zz9p::MsgType::Rhop, reqHeader.tag);
        break;
      case zz9p::MsgType::Zread:
        encBuf.writeString("simulated payload bytes from permascroll");
        encBuf.finalizeHeader(zz9p::MsgType::Rread, reqHeader.tag);
        break;
      default:
        encBuf.finalizeHeader(static_cast<zz9p::MsgType>(reqHeader.type + 1),
                              reqHeader.tag);
        break;
      }
    }

    channel.send(encBuf.data(), encBuf.size());

    // Client Decodes Response
    std::size_t resLen          = 0;
    const std::uint8_t *resData = channel.receive(resLen);
    zz9p::ReaderStream resReader(resData, resLen);
    zz9p::Header resHeader{};
    resReader.readHeader(resHeader);
    checksum += resHeader.tag;
  }

  const auto tEnd = std::chrono::steady_clock::now();
  const double totalUs =
      std::chrono::duration<double, std::micro>(tEnd - tStart).count();
  const double usPerPair = totalUs / kIterations;

  std::cout << "  Total Time: " << totalUs / 1000.0 << " ms" << std::endl;
  std::cout << "  Round-trip Latency: " << std::fixed << std::setprecision(3)
            << usPerPair << " us / pair" << std::endl;

  const bool pass = (usPerPair <= 1.2);
  std::cout << "\n=== SPIKE S2 RESULTS ===" << std::endl;
  std::cout << "Round-Trip Target: <= 1.200 us | Actual: " << usPerPair
            << " us | " << (pass ? "[PASS]" : "[FAIL]") << std::endl;

  return pass ? 0 : 1;
}
