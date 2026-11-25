#ifndef XANADU_VORTEX_CAPTURE_CONSTANTS_HPP
#define XANADU_VORTEX_CAPTURE_CONSTANTS_HPP

#include <cstddef>

// Capture's fallback bounds, in one place. Every value is a deliberate
// first-release ceiling, not a measured optimum: M8 moves the configurable
// ones into system://settings (design/vortex-io-implementation-plan.md §13,
// M8) and resolves them from one generation snapshot, at which point these
// become the schema defaults.

namespace xanadu::vortex::capture_defaults {

/// PayloadRanges one capture may select: a selection is a human or script
/// gesture over a handful of runs, and each range pins a chunk lease until
/// commit, so the bound limits lease fan-out rather than expressiveness.
inline constexpr std::size_t maxIntervals = 128;
/// Uncovered bytes one capture may append. One append is one worker turn and
/// one staged buffer held in memory until the permascroll takes it, so the
/// bound is the largest transient buffer capture will hold.
inline constexpr std::size_t maxAppendBytes = std::size_t{1} << 20;
/// Uncovered pieces one capture may carry. Each piece is a translation slot
/// and a journal interval record (49 bytes), so this bounds journal growth
/// per capture to roughly 50 KiB.
inline constexpr std::size_t maxPieces = 1024;
/// Captures that may be live at once. Each holds leases, staged bytes and a
/// journal reservation; the cap keeps all three bounded when a cell never
/// dies.
inline constexpr std::size_t maxPendingCaptures = 32;
/// Live translation slots (committed + pending + pinned). Slots are 56-byte
/// map nodes; 8192 keeps the map under about half a megabyte.
inline constexpr std::size_t maxMapEntries = 8192;
/// Fraction of the translation map evicted at once when it is full of
/// committed translations nothing has hit lately: a one-eighth batch makes
/// each eviction pass (a sort of the committed slots) cost O(1) amortised
/// per capture instead of one pass per refused prepare.
inline constexpr std::size_t evictionBatchDivisor = 8;
/// Journal file budget. The journal stores metadata only, so 4 MiB holds
/// tens of thousands of captures; exhaustion refuses prepare before any
/// append (plan §8.2) rather than after bytes landed.
inline constexpr std::size_t journalMaxBytes = std::size_t{4} << 20;
/// Completion bytes the service reserves for one append request: the
/// outcome carries only a terminal tag and progress, never the receipt span
/// (which the adapter holds), so a small fixed allowance suffices.
inline constexpr std::size_t completionBoundBytes = 256;

} // namespace xanadu::vortex::capture_defaults

#endif
