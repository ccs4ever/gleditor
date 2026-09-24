/**
 * @file binary_ops.hpp
 * @brief Ultra-compact binary serialization for the OSMIC operations spool,
 *        with on-demand standard OSMIC text format generation.
 */
#ifndef XUDU_BINARY_OPS_HPP
#define XUDU_BINARY_OPS_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "microversion.hpp"
#include "ops.hpp"

namespace xanadu {

/// 4-byte magic prefix identifying compact binary operations spools.
inline constexpr std::string_view binaryOpsMagicPrefix = "\x7fXOP";

/**
 * @brief Operations spool format version.
 *
 * Versions 1, 2, and 3 existed and are gone. Under R11 a format here is free to
 * change shape on the condition that the version is bumped and **the old
 * reader is deleted** -- keeping one only so that a file already on disk still
 * parses is a permanent tax paid to protect data nobody has. Version 1 wrote a
 * branch as a literal ASCII letter, which is why a branch could only ever have
 * one; version 2 wrote the ordinal instead; version 3 wrote Structure
 * operations with local spool indices for link endpoints and source. All three
 * are refused now, by number, rather than read.
 */
enum class OpsSpoolVersion : std::uint8_t {
  StandardOsmicText =
      0, ///< Standard human-readable OSMIC text format (version 0).
  /**
   * Version 4: Structure addresses (source, SetLink dimension and target, and
   * OpHandle value target) travel by microversion name rather than local spool
   * index, surviving publication and foreign branch renumbering. Splice
   * carries at and length offsets.
   */
  CompactBinaryV4 = 4,
};

/// 4-byte magic prefix + 1-byte version for compact binary ops spools. What is
/// written now, and the only binary version that is read.
inline constexpr std::string_view binaryOpsMagicV4 = "\x7fXOP\x04";

/// The magic a new store is written with.
inline constexpr std::string_view binaryOpsMagic = binaryOpsMagicV4;

/// Human-readable name for an operations spool version.
const char *opsSpoolVersionName(OpsSpoolVersion version);

/// Detect the operations spool format version from a stream: version 0 is the
/// OSMIC text format, version 4 the compact binary one.
/// @throws std::runtime_error naming the version, for a binary spool this
///         build does not read -- which includes versions 1, 2, and 3.
OpsSpoolVersion detectOpsSpoolVersion(std::istream &in);

/// Variable-length unsigned integer (LEB128) encoding.
void writeVarint(std::ostream &out, std::uint64_t val);
bool readVarint(std::istream &in, std::uint64_t &val);

/// Compact MicroversionId serialization. A branch travels as the ordinal its
/// letter run names: 0 for none, 1-254 directly, and 255 as an escape to a
/// varint that follows.
void writeMicroversionId(std::ostream &out, const MicroversionId &id);
bool readMicroversionId(std::istream &in, MicroversionId &id);

/**
 * @struct OpRecord
 * @brief One operation as the serializers see it: the state it produces, and
 *        the operation that produced it.
 *
 * A sequence rather than a map, because the order records are written in is
 * part of the format and not an incidental consequence of how they were held.
 * FLAG_SEQUENTIAL drops a record's name entirely when it continues the one
 * before it, so what is adjacent to what decides the size of the file; see
 * Store::opRecords(), which is where the order is chosen.
 */
struct OpRecord {
  MicroversionId produces;
  Op op;
  MicroversionId structureDimension;
  MicroversionId structureTarget;
  MicroversionId structureValueTarget;

  bool operator==(const OpRecord &) const = default;
};

/// Write operations in compact binary format (version 4).
void writeBinaryOpsSpool(std::ostream &out, const std::vector<OpRecord> &ops);

/// Read operations from compact binary format, version 4, in the order the
/// file holds them -- which the caller needs, since a record may name its
/// state only relative to the one before it.
void readBinaryOpsSpool(std::istream &in, std::vector<OpRecord> &ops);

/// Write operations in standard human-readable OSMIC text format.
void writeOsmicTextOpsSpool(std::ostream &out,
                            const std::vector<OpRecord> &ops);

/// Read operations from standard human-readable OSMIC text format.
void readOsmicTextOpsSpool(std::istream &in, std::vector<OpRecord> &ops);

/// Auto-detects binary vs. text format by peeking magic bytes and decodes.
void readOpsSpool(std::istream &in, std::vector<OpRecord> &ops);

} // namespace xanadu

#endif // XUDU_BINARY_OPS_HPP
