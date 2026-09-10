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
 * Versions 1 and 2 existed and are gone. Under R11 a format here is free to
 * change shape on the condition that the version is bumped and **the old
 * reader is deleted** -- keeping one only so that a file already on disk still
 * parses is a permanent tax paid to protect data nobody has. Version 1 wrote a
 * branch as a literal ASCII letter, which is why a branch could only ever have
 * one; version 2 wrote the ordinal instead. Both are refused now, by number,
 * rather than read.
 */
enum class OpsSpoolVersion : std::uint8_t {
  StandardOsmicText =
      0, ///< Standard human-readable OSMIC text format (version 0).
  /**
   * Version 3: the operation tag byte's kind field is four bits wide rather
   * than three, and every flag above it moved up one place to make room.
   *
   * Three bits held eight kinds and seven were spoken for, so OSMIC's sixth
   * hyperop -- OpKind::Structure, which migration step 12 adds as
   * BinStructure = 7 -- would have filled the field exactly and left nothing
   * for whatever comes after it. Widening now costs one version bump; leaving
   * it would have cost one anyway, later, with a kind already wedged into the
   * last slot.
   *
   * The whole byte is now spoken for: four bits of kind and four flags. A
   * further flag needs another version or a second byte, which is the price
   * of the room and is recorded rather than regretted -- the three flags that
   * exist are all read by Insert and Delete only, so it is kinds that this
   * format has ever run out of, not flags.
   *
   * Nothing else about the encoding moves. Varints, the branch-ordinal
   * escape, the field order after the tag: all unchanged from version 2.
   */
  CompactBinaryV3 = 3,
};

/// 4-byte magic prefix + 1-byte version for compact binary ops spools. What is
/// written now, and the only binary version that is read.
inline constexpr std::string_view binaryOpsMagicV3 = "\x7fXOP\x03";

/// The magic a new store is written with.
inline constexpr std::string_view binaryOpsMagic = binaryOpsMagicV3;

/// Human-readable name for an operations spool version.
const char *opsSpoolVersionName(OpsSpoolVersion version);

/// Detect the operations spool format version from a stream: version 0 is the
/// OSMIC text format, version 3 the compact binary one.
/// @throws std::runtime_error naming the version, for a binary spool this
///         build does not read -- which includes every version 1 and 2 file.
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
};

/// Write operations in compact binary format (version 3).
void writeBinaryOpsSpool(std::ostream &out, const std::vector<OpRecord> &ops);

/// Read operations from compact binary format, version 3, in the order the
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
