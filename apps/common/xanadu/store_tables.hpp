/**
 * @file store_tables.hpp
 * @brief A store's side tables, in one versioned binary container.
 *
 * The scroll registry and the link table were two plaintext files, parsed line
 * by line with a `throw` per malformed field. R11 says there is no reason they
 * were separate artefacts other than that they were written at different
 * times: both are per-store side tables, replayed at load, meaning nothing
 * without the operations they sit beside. So they are sections of one file
 * now, and the plaintext parsers are gone.
 *
 * Plaintext was buying exactly one thing -- being able to read a store with
 * `less` -- and R11's answer is that debuggability is a tool's job:
 * `tools/xudu-dump --section=scrolls` and `--section=links` render this, and
 * render it in the same shape they rendered the plaintext, so the conversion
 * is diffable rather than merely asserted.
 *
 * **Not native-endian, unlike ops.nodes.** The operations file is a run of
 * structs copied from memory because it is large, hot, and never leaves the
 * machine. These tables are small and are read once at load, so they are
 * bencode: a byte stream with no word order, which costs nothing at this size
 * and removes a whole class of question about who wrote the file.
 */
#ifndef XUDU_STORE_TABLES_HPP
#define XUDU_STORE_TABLES_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ops.hpp"
#include "scroll.hpp"

namespace xanadu {

/// The name of the container inside a store directory.
inline constexpr std::string_view storeTablesName = "store.tables";

/**
 * @brief The twelve bytes the container opens with.
 *
 * The same PNG-derived construction ops.nodes uses, for the same reasons: a
 * high bit that a seven-bit transfer destroys, a name a person sees in `file`
 * and `less`, a CRLF and an LF that any line-ending translation ruins, and a
 * DOS end-of-file so `TYPE` stops rather than spraying the terminal.
 */
inline constexpr std::array<std::uint8_t, 12> storeTablesSignature{
    0x89, 'X', 'U', 'D', 'U', 'T', 'B', 'L', 0x0d, 0x0a, 0x1a, 0x0a};

/// Bumped per R11 when the shape below changes. A reader that does not know a
/// version reads nothing rather than guessing.
inline constexpr std::uint32_t storeTablesFormatVersion = 1;

/**
 * @class StoreTablesUnreadable
 * @brief Thrown when a file is not a store table container this build reads.
 *
 * The same split OpsSegmentUnreadable draws: this is "the file is not what you
 * think it is", which is not a question the caller was asking, so it is a
 * throw rather than a false.
 */
class StoreTablesUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/**
 * @brief What a store keeps beside its operations.
 *
 * Held as one value so that reading and writing are each one call and cannot
 * disagree about which tables exist.
 */
struct StoreTables {
  /// The scroll registry, indexed from one: entry i is ScrollId i + 1.
  std::vector<Scroll> scrolls;
  /// The local spool's own segments. Not a Scroll: scroll zero is this
  /// machine's permascroll and has no entry in the registry to hold them.
  std::vector<ScrollSegment> localSegments;
  /// Links by id.
  std::map<std::uint64_t, Link> links;
};

/// Write @p tables to @p path, header and all.
/// @throws StoreTablesUnreadable if the file cannot be written.
void writeStoreTables(const std::filesystem::path &path,
                      const StoreTables &tables);

/// Read back what writeStoreTables() wrote.
/// @throws StoreTablesUnreadable naming what was wrong: a missing signature, a
///         version this build does not know, or a section it cannot parse.
[[nodiscard]] StoreTables readStoreTables(const std::filesystem::path &path);

} // namespace xanadu

#endif // XUDU_STORE_TABLES_HPP
