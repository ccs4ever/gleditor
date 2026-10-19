/**
 * @file zz9p.hpp
 * @brief The 9Z2026 Wire Protocol: Plan 9 Styx Reimagined for ZigZag Space.
 *
 * Implements the 12-message pair wire codec replacing hierarchical 9P2000
 * file operations (walk, open, read, write) with multidimensional topological
 * operations (hop, read, dims, link, make, watch).
 *
 * Transport:
 * - Direct binary framing (7-byte header: size[4], type[1], tag[2]) for
 * low-latency local sockets and memory rings.
 * - Bencode serialization dictionary for BEP 10 actor mesh transport
 * (SpoolMesh).
 *
 * Strict Compliance:
 * - Bounded frame sizes (kMaxPayloadBytes = 64 KB).
 * - Zero heap allocation on hot framing paths.
 * - Universal Rerror string fidelity.
 */
#ifndef ZIGZAG_ZZ9P_HPP
#define ZIGZAG_ZZ9P_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag::zz9p {

inline constexpr std::size_t kMaxPayloadBytes = 64 * 1024; // 64 KiB
inline constexpr std::uint16_t kNoTag         = 0xFFFFU;
inline constexpr std::uint32_t kNoFid         = 0xFFFFFFFFU;

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
  Rerror   = 207,
};

#pragma pack(push, 1)
struct Header {
  std::uint32_t size{0};
  std::uint8_t type{0};
  std::uint16_t tag{0};
};
#pragma pack(pop)

static_assert(sizeof(Header) == 7, "9Z header must be exactly 7 bytes");

// Message Payload Structs
struct MsgTversion {
  std::uint32_t msize{65536};
  std::string version{"9Z2026"};
};

struct MsgRversion {
  std::uint32_t msize{65536};
  std::string version{"9Z2026"};
};

struct MsgTauth {
  std::uint32_t afid{0};
  std::string uname;
  std::string aname;
};

struct MsgRauth {
  std::uint32_t aqid{0};
};

struct MsgTattach {
  std::uint32_t fid{0};
  std::uint32_t afid{kNoFid};
  std::string uname;
  std::string aname;
};

struct MsgRattach {
  std::uint32_t qid{0};
  CellRef rootCell{noCell};
};

struct MsgTclunk {
  std::uint32_t fid{0};
};

struct MsgRclunk {};

struct MsgTflush {
  std::uint16_t oldtag{0};
};

struct MsgRflush {};

struct MsgZstat {
  CellRef cell{noCell};
};

struct MsgRstat {
  CellRef cell{noCell};
  std::uint32_t flags{0};
  std::uint64_t spanLength{0};
  std::uint32_t linkCount{0};
};

struct MsgZhop {
  CellRef startCell{noCell};
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  std::uint32_t hops{1};
};

struct MsgRhop {
  CellRef targetCell{noCell};
};

struct MsgZread {
  CellRef cell{noCell};
  std::uint64_t offset{0};
  std::uint32_t count{0};
};

struct MsgRread {
  std::vector<std::uint8_t> data;
};

struct MsgZdims {
  CellRef cell{noCell};
};

struct MsgRdims {
  std::vector<DimRef> dimensions;
};

struct MsgZlink {
  CellRef fromCell{noCell};
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  CellRef toCell{noCell};
};

struct MsgRlink {
  bool success{true};
};

struct MsgZmake {
  CellRef suggestedId{noCell};
  std::uint8_t valueKind{0};
  std::uint64_t valueBits{0};
};

struct MsgRmake {
  CellRef newCell{noCell};
};

struct MsgZwatch {
  CellRef cell{noCell};
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  std::uint32_t mask{0};
};

struct MsgRwatch {
  std::uint32_t watchId{0};
};

struct MsgRerror {
  std::string ename;
};

using MessagePayload =
    std::variant<MsgTversion, MsgRversion, MsgTauth, MsgRauth, MsgTattach,
                 MsgRattach, MsgTclunk, MsgRclunk, MsgTflush, MsgRflush,
                 MsgZstat, MsgRstat, MsgZhop, MsgRhop, MsgZread, MsgRread,
                 MsgZdims, MsgRdims, MsgZlink, MsgRlink, MsgZmake, MsgRmake,
                 MsgZwatch, MsgRwatch, MsgRerror>;

struct Message {
  MsgType type{MsgType::Tversion};
  std::uint16_t tag{0};
  MessagePayload payload;
};

/**
 * @class Codec
 * @brief Low-latency binary serializer/deserializer and Bencode adaptor for 9Z.
 */
class Codec {
public:
  /**
   * @brief Encode a 9Z Message into contiguous raw wire format bytes.
   */
  [[nodiscard]] static std::vector<std::uint8_t> encode(const Message &msg);

  /**
   * @brief Decode a 9Z Message from raw wire format bytes.
   */
  [[nodiscard]] static std::expected<Message, std::string>
  decode(std::span<const std::uint8_t> bytes);

  /**
   * @brief Convert a 9Z Message to a Bencode dictionary for SpoolMesh
   * transport.
   */
  [[nodiscard]] static xanadu::bencode::Value toBencode(const Message &msg);

  /**
   * @brief Parse a 9Z Message from a Bencode dictionary.
   */
  [[nodiscard]] static std::expected<Message, std::string>
  fromBencode(const xanadu::bencode::Value &val);
};

} // namespace zigzag::zz9p

#endif // ZIGZAG_ZZ9P_HPP
