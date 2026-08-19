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
};

/// 4-byte magic prefix + 1-byte version for compact binary ops spools (Version
/// 1).
inline constexpr std::string_view binaryOpsMagicV1 = "\x7fXOP\x01";

/// Alias for backwards compatibility.
inline constexpr std::string_view binaryOpsMagic = binaryOpsMagicV1;

/// Human-readable name for an operations spool version.
const char *opsSpoolVersionName(OpsSpoolVersion version);

/// Detect the operations spool format version from a stream (Version 0 = text,
/// Version 1 = binary v1).
OpsSpoolVersion detectOpsSpoolVersion(std::istream &in);

/// Variable-length unsigned integer (LEB128) encoding.
void writeVarint(std::ostream &out, std::uint64_t val);
bool readVarint(std::istream &in, std::uint64_t &val);

/// Compact MicroversionId serialization.
void writeMicroversionId(std::ostream &out, const MicroversionId &id);
bool readMicroversionId(std::istream &in, MicroversionId &id);

/// Write operations in compact binary format.
void writeBinaryOpsSpool(std::ostream &out,
                         const std::map<MicroversionId, Op> &ops);

/// Read operations from compact binary format.
void readBinaryOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

/// Write operations in standard human-readable OSMIC text format.
void writeOsmicTextOpsSpool(std::ostream &out,
                            const std::map<MicroversionId, Op> &ops);

/// Read operations from standard human-readable OSMIC text format.
void readOsmicTextOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

/// Auto-detects binary vs. text format by peeking magic bytes and decodes.
void readOpsSpool(std::istream &in, std::map<MicroversionId, Op> &ops);

} // namespace xudu

#endif // XUDU_BINARY_OPS_HPP
