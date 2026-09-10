/**
 * @file segmented_ops_spool.hpp
 * @brief Contiguous multi-segment virtual memory operations spool and tree.
 */
#ifndef XUDU_SEGMENTED_OPS_SPOOL_HPP
#define XUDU_SEGMENTED_OPS_SPOOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "compact_op.hpp"
#include "microversion.hpp"
#include "virtual_memory_arena.hpp"

namespace xanadu {

/**
 * @brief The twelve bytes every operations segment file opens with.
 *
 * PNG's signature trick, copied rather than shortened, because every byte of
 * it does a job that a longer random constant would not. 0x89 has its high bit
 * set, so a transfer that strips to seven bits destroys it. XUDUOPS is what
 * `file` and `less` show a person. \\r\\n is destroyed by any transport that
 * translates CRLF to LF and the trailing \\n by one translating the other way.
 * \\x1a is DOS end-of-file, so TYPE on Windows stops rather than spraying the
 * terminal with node bytes.
 *
 * The file it exists to reject is guaranteed to be rejected: a segment written
 * before this header existed opens with the root operation's parentIndex,
 * which is zero, so its first four bytes are 00 00 00 00 and cannot be
 * mistaken for \\x89XUD.
 */
inline constexpr std::array<std::uint8_t, 12> opsSegmentSignature{
    0x89, 'X', 'U', 'D', 'U', 'O', 'P', 'S', 0x0d, 0x0a, 0x1a, 0x0a};

/// Bumped per R11 whenever the shape of what follows changes. A reader that
/// does not know a version reads nothing rather than guessing.
///
/// This is also what catches a file written on a machine of the other
/// endianness. The nodes are stored as they sit in memory, so an ops segment
/// is host-ordered by construction; the signature is a byte array and would
/// pass, but a version of 1 read the wrong way round is 16,777,216, which is
/// not a version this reader knows.
inline constexpr std::uint32_t opsSegmentFormatVersion = 1;

/**
 * @brief Bytes from the start of the file to the first node. 65,536.
 *
 * Forced rather than chosen. adoptSegmentNodes() maps a sealed segment with
 * mapFileFixed(), and mmap requires a page-aligned file offset -- so this must
 * be a multiple of the page size of every machine that will ever open the
 * file: 4 KiB on x86-64, 16 KiB on Apple silicon, 64 KiB on some POWER and ARM
 * configurations. A 4 KiB header would silently lose zero-copy mapping on a
 * 16 KiB-page machine and fall back to the read() path, which is correct but
 * no longer zero-copy, and nothing would say so.
 *
 * 64 KiB is the smallest size that is a multiple of all of them, and it is
 * already this tree's unit: the BitTorrent v2 Merkle piece size the
 * permascroll's segments are aligned to, which gives 65536 / 64 = 1024
 * operations per piece. So the header is piece 0 and the nodes are pieces
 * 1..N, node boundaries still land on piece boundaries, and a peer that
 * fetches the first piece of a segment learns the format before fetching
 * anything else.
 *
 * The cost is 64 KiB per segment file including a freshly opened one holding
 * no operations. Everything that writes a header leaves the rest of it a hole,
 * so on a filesystem with sparse files it is address space rather than disk --
 * but `ls` will report it, and it is a floor rather than an overhead that
 * amortises.
 */
inline constexpr std::uint32_t opsSegmentHeaderBytes = 65536;

/// OpsSegmentHeader::flags: merkleRoot holds a root rather than zeroes.
/// Nothing computes one yet, so nothing sets this; it is what stops an unset
/// root ever being read as a real one.
inline constexpr std::uint32_t opsSegmentFlagMerkleRoot = 0x1U;

/**
 * @brief What an operations segment file says about itself before its nodes.
 *
 * Migration step 1 moved a field inside CompactOpNode and every sample store
 * on disk went on loading, meaning something else: an operation read as a
 * Rearrange of zero bytes, and fifteen tests quietly started asserting on the
 * empty string. There was nothing to refuse it with, because a segment file
 * was a bare run of nodes with no header at all -- so R11's "bump the version"
 * had nothing to bump. This is that header.
 *
 * Laid out with no implicit padding anywhere, and every offset asserted below:
 * this is a file format, and a compiler quietly inserting four bytes between
 * two fields is the same class of silent change the header exists to catch.
 */
struct OpsSegmentHeader {
  std::array<std::uint8_t, 12> signature{}; ///< opsSegmentSignature
  std::uint32_t formatVersion{};            ///< opsSegmentFormatVersion
  std::uint32_t headerBytes{};              ///< opsSegmentHeaderBytes
  /// sizeof(CompactOpNode). The step-1 failure, caught: this turns "the file
  /// means something else now" into "this file says its nodes are 64 bytes
  /// and mine are 72".
  std::uint32_t nodeSize{};
  std::uint32_t flags{};
  /// The spool index the first node in this file held when it was written.
  /// The loader re-derives this as opCount + 1; recording it is what lets a
  /// segment loaded out of order say so, rather than failing later and less
  /// clearly because a parent turned out to be missing.
  std::uint32_t firstOpIndex{};
  /// Operations in this file. The file's own length says this too, and the
  /// two are cross-checked -- see nodesInSegment() for which wins and why.
  std::uint64_t nodeCount{};
  /// Over the node pieces, not over the header, so that a segment fetched
  /// from a swarm can be checked before it is mapped. Zero and meaningless
  /// unless opsSegmentFlagMerkleRoot is set, which nothing sets yet.
  std::array<std::uint8_t, 32> merkleRoot{};
  std::uint64_t reservedZero{}; ///< written zero, read and required to be zero
};

static_assert(sizeof(OpsSegmentHeader) == 80);
static_assert(alignof(OpsSegmentHeader) == 8);
static_assert(sizeof(OpsSegmentHeader) < opsSegmentHeaderBytes);
// Every field's offset, because a size assertion alone cannot catch two
// changes that cancel out, and because these are the bytes on disk.
static_assert(offsetof(OpsSegmentHeader, signature) == 0);
static_assert(offsetof(OpsSegmentHeader, formatVersion) == 12);
static_assert(offsetof(OpsSegmentHeader, headerBytes) == 16);
static_assert(offsetof(OpsSegmentHeader, nodeSize) == 20);
static_assert(offsetof(OpsSegmentHeader, flags) == 24);
static_assert(offsetof(OpsSegmentHeader, firstOpIndex) == 28);
static_assert(offsetof(OpsSegmentHeader, nodeCount) == 32);
static_assert(offsetof(OpsSegmentHeader, merkleRoot) == 40);
static_assert(offsetof(OpsSegmentHeader, reservedZero) == 72);
// The header is one Merkle piece and the nodes are the pieces after it, so a
// node boundary is never a piece boundary's problem.
static_assert(opsSegmentHeaderBytes % sizeof(CompactOpNode) == 0);

/**
 * @class OpsSegmentUnreadable
 * @brief Thrown when a file is not an operations segment this build can read.
 *
 * The loud half of R14. A file whose signature is wrong, whose version is not
 * known, whose nodes are not the size this build compiles, or which is cut
 * short is refused with a diagnostic rather than read into nonsense -- and
 * refusing is a throw rather than a false because "this is not the file you
 * think it is" is not a question the caller was asking.
 *
 * Distinct from a segment being refused by returning false, which means the
 * file is a perfectly good segment that does not belong where it was offered:
 * it starts at the wrong operation index, names parents this spool does not
 * hold, or would file a state twice. That is a fact about the order things
 * were loaded in, not about the file.
 */
class OpsSegmentUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Address space the operations arena asks for, which on a 64-bit machine is
/// not memory: VirtualMemoryArena::reserve maps it PROT_NONE, and a page costs
/// nothing until ensureCommitted() makes it readable. 8 GiB is 134,217,727
/// operations, which is what the ceiling should be measured in -- a document
/// that has had a hundred million edits is a real thing to run out of room in,
/// where 8 GiB of RAM is not a thing to ask for.
///
/// Where a pointer is 32 bits there is no address space to be casual with, so
/// the old 512 MiB stands.
inline constexpr std::size_t defaultOpsReservation = [] {
  if constexpr (sizeof(void *) >= 8) {
    return std::size_t{8} * 1024 * 1024 * 1024;
  } else {
    return std::size_t{512} * 1024 * 1024;
  }
}();

/// The smallest reservation a spool will settle for before giving up. A
/// process under an RLIMIT_AS, or one whose address space is already carved
/// up, can fail to get 8 GiB of it; refusing to open a document at all over
/// that would be worse than opening one with the ceiling it used to have.
inline constexpr std::size_t minOpsReservation = std::size_t{512} * 1024 * 1024;

/**
 * @class SpoolExhausted
 * @brief Thrown when an operation cannot be recorded because the spool's
 *        address-space reservation has no room for it.
 *
 * Deliberately not std::bad_alloc, which the arena would otherwise raise for
 * this: the two say different things and want different answers. bad_alloc
 * means the machine could not give us memory, and nothing the caller does
 * about this document will change that. This means the document has reached
 * the end of the range it was given, which is a fact about the document -- it
 * can be sealed, split, or reopened against a larger reservation. A caller
 * that wants to see it coming reads opCapacityRemaining() rather than waiting
 * for the throw.
 */
class SpoolExhausted : public std::runtime_error {
public:
  SpoolExhausted(std::uint32_t heldOps, std::uint32_t capacityOps);

  /// Operations the spool already holds -- which is its capacity, since that
  /// is the only condition under which this is thrown.
  [[nodiscard]] std::uint32_t held() const noexcept { return heldOps_; }

  /// Operations the reservation can hold in total.
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacityOps_; }

private:
  std::uint32_t heldOps_;
  std::uint32_t capacityOps_;
};

/**
 * @class SegmentedOpsSpool
 * @brief Contiguous virtual memory operations tree spanning sealed and active
 *        torrent segments.
 */
class SegmentedOpsSpool {
public:
  struct SegmentInfo {
    std::uint32_t startOpIndex{0};
    std::uint32_t opCount{0};
    std::string path;
    int fd{-1};
    bool isReadOnly{true};
  };

  SegmentedOpsSpool() : SegmentedOpsSpool(defaultOpsReservation) {}

  /// A spool whose arena reserves @p reservationBytes of address space rather
  /// than the default. The reservation is the ceiling on how many operations
  /// this spool can ever hold, so this exists for tests that want to reach
  /// that ceiling without appending 134 million operations to get there.
  explicit SegmentedOpsSpool(std::size_t reservationBytes);

  ~SegmentedOpsSpool();

  SegmentedOpsSpool(const SegmentedOpsSpool &)            = delete;
  SegmentedOpsSpool &operator=(const SegmentedOpsSpool &) = delete;
  SegmentedOpsSpool(SegmentedOpsSpool &&) noexcept;
  SegmentedOpsSpool &operator=(SegmentedOpsSpool &&) noexcept;

  /**
   * @brief Append a new operation to the active segment.
   * @param node The operation payload.
   * @param produces The microversion state produced.
   * @return The 1-based index of the new operation.
   * @throws SpoolExhausted when the reservation is full, which is a fact
   *         about this document. Nothing is appended and the spool is left
   *         exactly as it was.
   * @throws std::bad_alloc when there is room in the reservation but the
   *         machine will not back it, which is a fact about the machine.
   */
  std::uint32_t append(CompactOpNode node, const MicroversionId &produces);

  /**
   * @brief Get a pointer to an operation by its 1-based index.
   */
  [[nodiscard]] const CompactOpNode *get(std::uint32_t index) const;
  [[nodiscard]] CompactOpNode *get(std::uint32_t index);

  /**
   * @brief Get an operation by the microversion it produces.
   */
  [[nodiscard]] const CompactOpNode *get(const MicroversionId &id) const;

  /**
   * @brief Check whether an operation exists for a microversion.
   */
  [[nodiscard]] bool contains(const MicroversionId &id) const;

  /**
   * @brief Find the 1-based index for a microversion.
   */
  [[nodiscard]] std::uint32_t indexOf(const MicroversionId &id) const;

  /**
   * @brief Get the microversion produced by an operation index.
   */
  [[nodiscard]] MicroversionId idOf(std::uint32_t index) const;

  /// Total number of operations recorded.
  [[nodiscard]] std::size_t size() const { return opCount; }

  /// Whether the spool contains zero operations.
  [[nodiscard]] bool empty() const { return 0 == opCount; }

  /// Operations the reservation can hold, which is one short of the nodes
  /// that fit in it: index 0 is the state-zero slot and holds no operation.
  ///
  /// This is what the arena actually got rather than what it asked for, so a
  /// spool that had to step down to a smaller reservation reports the smaller
  /// ceiling instead of promising room it does not have.
  [[nodiscard]] std::uint32_t opCapacity() const noexcept;

  /// Operations that can still be appended before append() throws
  /// SpoolExhausted. Reading this is how a caller sees the wall coming; the
  /// throw is what happens to one that did not look.
  [[nodiscard]] std::uint32_t opCapacityRemaining() const noexcept;

  /**
   * @brief Get the direct children indices branching off @p index.
   */
  [[nodiscard]] std::vector<std::uint32_t>
  childrenOf(std::uint32_t index) const;

  /**
   * @brief Compute the ancestral index path to rebuild @p targetIndex from
   * root.
   */
  [[nodiscard]] std::vector<std::uint32_t>
  ancestralPath(std::uint32_t targetIndex) const;

  /**
   * @brief Add a sealed read-only segment file, mapped in after what is
   *        already held.
   *
   * An OpsSegmentHeader, then a run of CompactOpNodes. No state-zero slot and
   * no names: the names are worked out from the nodes, each one saying which
   * index produced it and by which branch ordinal, so its microversion follows
   * from its parent's and a parent always sits at a lower index than its
   * children. Segments are therefore written and read back in the same order,
   * and the indices inside them are the ones they had when they were sealed
   * rather than positions within the file.
   *
   * @return false if the file cannot be opened, starts at an operation index
   *         other than the one this spool is at, names a parent this spool
   *         does not hold, or would file one state twice.
   * @throws OpsSegmentUnreadable if the file is not an operations segment this
   *         build can read at all. See that class for why the two are
   *         different answers.
   */
  bool addSealedSegment(const std::filesystem::path &path);

  /**
   * @brief Open the writable segment that new operations are appended to.
   *
   * A file that does not exist yet, or exists and is empty, is created with a
   * header and nothing after it. A file that already holds nodes is adopted
   * rather than overwritten, so reopening a spool picks up where it left off.
   * Anything appended after that is written to this file by flush().
   *
   * @throws OpsSegmentUnreadable as addSealedSegment() does.
   */
  bool openActiveSegment(const std::filesystem::path &path);

  /**
   * @brief Write every operation held to @p path as a whole segment file.
   *
   * The one place a complete segment file is produced, which is the point:
   * Store::save() used to write ops.nodes itself with its own ofstream while
   * this class read it, so the format was agreed between two pieces of code
   * rather than known by one. That is exactly the arrangement that let a
   * layout change go unnoticed in migration step 1, and a header only fixes it
   * if there is one writer to put a header in.
   *
   * Does not disturb the active segment's own bookkeeping, so writing the
   * active segment's path is safe: this writes every node, and a later flush()
   * would rewrite the tail with the identical bytes at the identical offsets.
   */
  [[nodiscard]] bool writeSegmentFile(const std::filesystem::path &path) const;

  /**
   * @brief Seal the active segment and start a new one at @p newActivePath.
   *
   * The operations already in hand do not move: sealing writes out whatever
   * has not been written yet and re-files the range as a read-only segment.
   * It does not read them back in -- they are already here.
   */
  bool sealActive(const std::filesystem::path &newActivePath);

  /// Write operations appended since the last flush to the active segment
  /// file. Only the tail is written, so this costs what was appended rather
  /// than what is held.
  bool flush();

  /// Reset to empty state.
  void clear();

  [[nodiscard]] const std::vector<SegmentInfo> &segments() const {
    return segmentList;
  }

  /// Direct raw pointer to the base of the contiguous CompactOpNode array.
  [[nodiscard]] const CompactOpNode *rawOps() const {
    return reinterpret_cast<const CompactOpNode *>(arena.base());
  }

  /// The downward edges of the operation tree, held beside the nodes rather
  /// than inside them.
  ///
  /// An operation node is immutable once written, which is what publication
  /// semantics already assumed and what mapping a sealed segment PROT_READ
  /// already enforced at the hardware level. A child edge is not a fact about
  /// the parent operation, though; it is a fact about the tree the parent
  /// turned out to be in, and it is only ever learnt after the parent was
  /// stored. Keeping it here costs the same eight bytes per operation it cost
  /// inside the node and makes the immutability true. See design R10.
  struct TreeLinks {
    std::uint32_t firstChild{0};
    std::uint32_t nextSibling{0};
  };

  /// The tree edges for @p index, or a zeroed pair if there is no such node.
  [[nodiscard]] TreeLinks treeLinksOf(std::uint32_t index) const {
    return index < tree.size() ? tree[index] : TreeLinks{};
  }

private:
  bool ensureCommitted(std::size_t requiredBytes);

  /// Take the arena's address space, stepping the request down by halves to
  /// minOpsReservation before giving up. Records what was actually obtained
  /// in reservationBytes, so every ceiling reported afterwards is the real
  /// one.
  bool reserveArena();

  /// File the node already stored at @p index under its parent, appending it
  /// to the end of that parent's child list. Parents always sit at a lower
  /// index than their children, so calling this in ascending index order
  /// rebuilds exactly the sibling order append() produced.
  void linkIntoTree(std::uint32_t index);

  VirtualMemoryArena arena;

  /// Address space the arena holds, or -- before it has been taken, and after
  /// clear() has given it back -- the amount that will be asked for. Not
  /// arena.capacity(): the arena is released on clear() and re-taken lazily,
  /// and the ceiling has to be answerable in between.
  std::size_t reservationBytes;

  std::vector<SegmentInfo> segmentList;
  std::uint32_t opCount{0};
  std::size_t committedBytes{0};

  /// index -> the microversion produced at that index. What every lookup
  /// below is ultimately answered from; a spool index is meaningless on its
  /// own, so this is the one place that names it.
  std::vector<MicroversionId> indexLookup;

  /// index -> that node's downward tree edges, index-aligned with the node
  /// array and with indexLookup. Element 0 is the state-zero slot, which has
  /// no edges of its own: top-level chains are found by scanning parentIndex,
  /// exactly as they were before.
  std::vector<TreeLinks> tree;

  /// microversion -> index, without holding a second copy of the id to
  /// compare against: indexLookup already has one at the index this points
  /// to, so a candidate slot is verified against that instead. Open
  /// addressing with linear probing and no tombstones, which append-only
  /// storage can get away with -- nothing is ever removed from a slot except
  /// by clear() resetting the whole table, so "empty" only ever means
  /// "nothing has been inserted here yet" and probing always terminates.
  ///
  /// This is the difference between the ~73 bytes per operation an
  /// unordered_map<std::string, uint32_t> cost here (a heap-allocated hash
  /// node plus a rendered id string per entry) and the few bytes a slot
  /// array costs: nothing is stored per entry that indexLookup was not
  /// storing already.
  std::vector<std::uint32_t> idHashSlots; // 0 = empty; else a 1-based index
  std::uint32_t idHashCount{0};

  void idHashInsert(const MicroversionId &id, std::uint32_t index);
  [[nodiscard]] std::uint32_t idHashFind(const MicroversionId &id) const;
  void idHashRehash(std::size_t newCapacity);

  /// Place @p nodeCount nodes from @p fd into the arena after what is already
  /// held, work out the microversion each produces, and index them. Shared by
  /// addSealedSegment() and openActiveSegment(), which differ only in whether
  /// the range may be mapped read-only and in what is recorded about it.
  ///
  /// Reads from opsSegmentHeaderBytes rather than from the start of the file,
  /// both when mapping and when falling back to read().
  bool adoptSegmentNodes(int fd, std::uint32_t nodeCount, bool mayMap);

  int activeFd{-1};
  std::string activePath;
  /// Where the active segment starts, and how much of it flush() has already
  /// written. The file holds the range [activeStartIndex, activeStartIndex +
  /// activeFlushedOps), so a flush writes from where it left off.
  std::uint32_t activeStartIndex{1};
  std::uint32_t activeFlushedOps{0};
};

} // namespace xanadu

#endif // XUDU_SEGMENTED_OPS_SPOOL_HPP
