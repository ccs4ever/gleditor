/**
 * @file binary_ops.hpp
 * @brief Ultra-compact binary serialization for the OSMIC operations spool,
 *        with on-demand standard OSMIC text format generation.
 */
#ifndef XUDU_BINARY_OPS_HPP
#define XUDU_BINARY_OPS_HPP

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "microversion.hpp"
#include "ops.hpp"

namespace xudu {

/// 4-byte magic prefix identifying compact binary operations spools.
inline constexpr std::string_view binaryOpsMagicPrefix = "\x7fXOP";

/// Operations spool format version.
enum class OpsSpoolVersion : std::uint8_t {
  StandardOsmicText =
      0, ///< Standard human-readable OSMIC text format (version 0).
  CompactBinaryV1 =
      1, ///< Compact binary encoding with LEB128 and bitpacking (version 1).
  /**
   * Version 2: the only difference from version 1 is what a segment's
   * branch byte means. Version 1 wrote it as the literal branch letter --
   * 'a'-'z', or ' ' for none -- which is why a branch could only ever be
   * one letter: there was nowhere to put a second. Version 2 writes the
   * ordinal that letter run names instead (0 for none, 1-254 directly, 255
   * as an escape to a following varint for anything past that), which is
   * what lets MicroversionId::branch() take more than 26 values. Every
   * other byte in the format -- op tags, varints, everything -- is
   * unchanged, so this is the smallest version bump that could carry it:
   * old bytes are not reinterpreted as anything new, but the byte that used
   * to be exactly one ASCII letter now is not, which is a real
   * incompatibility and not just an additive one -- see OpKind::PageBreak
   * for the kind of change that did not need a version bump, for contrast.
   */
  CompactBinaryV2 = 2,
};

/// 4-byte magic prefix + 1-byte version for compact binary ops spools (Version
/// 1). Read-only: nothing writes this any more, but files already on disk
/// still open under it.
inline constexpr std::string_view binaryOpsMagicV1 = "\x7fXOP\x01";

/// 4-byte magic prefix + 1-byte version for compact binary ops spools
/// (Version 2). What is written now.
inline constexpr std::string_view binaryOpsMagicV2 = "\x7fXOP\x02";

/// The magic a new store is written with.
inline constexpr std::string_view binaryOpsMagic = binaryOpsMagicV2;

/// Human-readable name for an operations spool version.
const char *opsSpoolVersionName(OpsSpoolVersion version);

/// Detect the operations spool format version from a stream (Version 0 =
/// text, Version 1 = binary v1, Version 2 = binary v2).
OpsSpoolVersion detectOpsSpoolVersion(std::istream &in);

/// Variable-length unsigned integer (LEB128) encoding.
void writeVarint(std::ostream &out, std::uint64_t val);
bool readVarint(std::istream &in, std::uint64_t &val);

/// Compact MicroversionId serialization, version 2: see
/// OpsSpoolVersion::CompactBinaryV2 for the branch encoding this uses.
void writeMicroversionId(std::ostream &out, const MicroversionId &id);
bool readMicroversionId(std::istream &in, MicroversionId &id);

/// Version 1's MicroversionId decoding: a branch byte is the literal ASCII
/// letter, or ' ' for none. Read-only, for opening files version 2 was not
/// written by.
bool readMicroversionIdV1(std::istream &in, MicroversionId &id);

/// Write operations in compact binary format (version 2).
void writeBinaryOpsSpool(std::ostream &out,
                         const std::map<MicroversionId, Op> &ops);

/// Read operations from compact binary format, version 2.
void readBinaryOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

/// Read operations from compact binary format, version 1.
void readBinaryOpsSpoolV1(std::istream &in, std::map<MicroversionId, Op> &ops);

/// Write operations in standard human-readable OSMIC text format.
void writeOsmicTextOpsSpool(std::ostream &out,
                            const std::map<MicroversionId, Op> &ops);

/// Read operations from standard human-readable OSMIC text format.
void readOsmicTextOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

/// Auto-detects binary vs. text format by peeking magic bytes and decodes.
void readOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

} // namespace xudu

#endif // XUDU_BINARY_OPS_HPP
