/**
 * @file lmdb_cache.hpp
 * @brief Zero-copy, reference-counted LMDB content and virtual address cache.
 *
 * Separates storage into:
 * 1. Raw verified text/piece store ("pieces"): immutable byte payloads indexed
 *    by canonical content address (InfoHash, piece_index) with embedded
 *    reference counters.
 * 2. Virtual address lookup stores:
 *    - "vspans" for Xanadoc primedia scroll coordinates (scroll_id, start,
 * len).
 *    - "ext_spans" for external torrent stream coordinates (InfoHash, offset,
 * len).
 *
 * Sub-spans and slices point into existing piece entries without duplicating
 * text. When all virtual references to a piece drop to zero, the piece is
 * automatically evicted.
 */
#ifndef XUDU_LMDB_CACHE_HPP
#define XUDU_LMDB_CACHE_HPP

#include "spool.hpp"   // for PrimediaSpan, ScrollId
#include "torrent.hpp" // for InfoHash
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <lmdb.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace xudu {

/// Key for a verified torrent piece in the raw content store.
struct PieceKey {
  std::array<std::uint8_t, 20> hash{};
  std::uint32_t piece_index{};

  bool operator==(const PieceKey &) const = default;
};

/// Key for a virtual Xanadoc primedia scroll span.
struct VSpanKey {
  std::uint32_t scroll_id{};
  std::uint32_t padding{};
  std::uint64_t start{};
  std::uint64_t length{};

  bool operator==(const VSpanKey &) const = default;
};

/// Key for an external content reference (torrent stream coordinates).
struct ExtSpanKey {
  std::array<std::uint8_t, 20> hash{};
  std::uint32_t padding{};
  std::uint64_t stream_offset{};
  std::uint64_t length{};

  bool operator==(const ExtSpanKey &) const = default;
};

/// Lightweight descriptor pointing to a slice inside a verified piece.
struct VSpanRecord {
  std::array<std::uint8_t, 20> hash{};
  std::uint32_t piece_index{};
  std::uint32_t chunk_offset{};
  std::uint32_t length{};

  bool operator==(const VSpanRecord &) const = default;
};

/// Header preceding raw text bytes in the "pieces" database.
struct TextHeader {
  std::uint32_t ref_count{1};
  std::uint32_t length{0};
};

/// Transcopyright unlocked Content Encryption Key record.
struct CekRecord {
  std::array<std::uint8_t, 32> cek{};
  std::uint64_t unlockedTimestamp{0};
  std::uint64_t pricePaid{0};
  std::array<char, 8> currency{};

  bool operator==(const CekRecord &) const = default;
};

class LMDBContentCache {
public:
  explicit LMDBContentCache(const std::filesystem::path &cache_dir,
                            std::size_t map_size_bytes = 1024ULL * 1024 *
                                                         1024) {
    if (!std::filesystem::exists(cache_dir)) {
      std::filesystem::create_directories(cache_dir);
    }

    if (const int rc = mdb_env_create(&env_); rc != MDB_SUCCESS) {
      throw std::runtime_error("mdb_env_create failed: " +
                               std::string(mdb_strerror(rc)));
    }

    mdb_env_set_maxdbs(env_, 8);
    mdb_env_set_mapsize(env_, map_size_bytes);

    const unsigned int env_flags = MDB_NOTLS | MDB_NOSYNC | MDB_NOMETASYNC;

    if (const int rc =
            mdb_env_open(env_, cache_dir.string().c_str(), env_flags, 0644);
        rc != MDB_SUCCESS) {
      mdb_env_close(env_);
      env_ = nullptr;
      throw std::runtime_error("mdb_env_open failed: " +
                               std::string(mdb_strerror(rc)));
    }

    open_databases();
  }

  ~LMDBContentCache() {
    if (env_) {
      mdb_env_close(env_);
      env_ = nullptr;
    }
  }

  LMDBContentCache(const LMDBContentCache &)            = delete;
  LMDBContentCache &operator=(const LMDBContentCache &) = delete;
  LMDBContentCache(LMDBContentCache &&other) noexcept
      : env_(other.env_), dbi_pieces_(other.dbi_pieces_),
        dbi_vspans_(other.dbi_vspans_), dbi_ext_spans_(other.dbi_ext_spans_),
        dbi_ceks_(other.dbi_ceks_) {
    other.env_ = nullptr;
  }
  LMDBContentCache &operator=(LMDBContentCache &&other) noexcept {
    if (this != &other) {
      if (env_) {
        mdb_env_close(env_);
      }
      env_           = other.env_;
      dbi_pieces_    = other.dbi_pieces_;
      dbi_vspans_    = other.dbi_vspans_;
      dbi_ext_spans_ = other.dbi_ext_spans_;
      dbi_ceks_      = other.dbi_ceks_;
      other.env_     = nullptr;
    }
    return *this;
  }

  // -- Raw Piece Operations --------------------------------------------------

  /// Store a raw verified piece payload under (hash, piece_index).
  bool put_piece(const InfoHash &hash, const std::uint32_t piece_index,
                 const std::string_view bytes,
                 const std::uint32_t initial_ref_count = 1) {
    if (bytes.empty() || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto pkey = PieceKey{hash.bytes, piece_index};
    MDB_val k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};

    // Check if piece already exists: if so, copy buffer and increment refcount
    MDB_val existing_v{};
    if (mdb_get(txn, dbi_pieces_, &k, &existing_v) == MDB_SUCCESS &&
        existing_v.mv_size >= sizeof(TextHeader)) {
      std::vector<char> buf(existing_v.mv_size);
      std::memcpy(buf.data(), existing_v.mv_data, existing_v.mv_size);
      auto *hdr = reinterpret_cast<TextHeader *>(buf.data());
      hdr->ref_count += initial_ref_count;
      MDB_val update_v{buf.size(), buf.data()};
      mdb_put(txn, dbi_pieces_, &k, &update_v, 0);
      return mdb_txn_commit(txn) == MDB_SUCCESS;
    }

    std::vector<char> buffer(sizeof(TextHeader) + bytes.size());
    auto *hdr      = reinterpret_cast<TextHeader *>(buffer.data());
    hdr->ref_count = initial_ref_count;
    hdr->length    = static_cast<std::uint32_t>(bytes.size());
    std::memcpy(buffer.data() + sizeof(TextHeader), bytes.data(), bytes.size());

    MDB_val v{buffer.size(), buffer.data()};
    if (mdb_put(txn, dbi_pieces_, &k, &v, 0) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Retrieve full raw piece payload.
  [[nodiscard]] bool get_piece(const InfoHash &hash,
                               const std::uint32_t piece_index,
                               std::string &out) const {
    if (!env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto pkey = PieceKey{hash.bytes, piece_index};
    MDB_val k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val v{};

    if (mdb_get(txn, dbi_pieces_, &k, &v) == MDB_SUCCESS &&
        v.mv_size >= sizeof(TextHeader)) {
      const auto *hdr = static_cast<const TextHeader *>(v.mv_data);
      const char *text_ptr =
          static_cast<const char *>(v.mv_data) + sizeof(TextHeader);
      out.assign(text_ptr, hdr->length);
      mdb_txn_abort(txn);
      return true;
    }
    mdb_txn_abort(txn);
    return false;
  }

  /// Get active reference count for a raw piece.
  [[nodiscard]] std::uint32_t ref_count(const InfoHash &hash,
                                        const std::uint32_t piece_index) const {
    if (!env_) {
      return 0;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return 0;
    }
    const auto pkey = PieceKey{hash.bytes, piece_index};
    MDB_val k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val v{};
    std::uint32_t count = 0;
    if (mdb_get(txn, dbi_pieces_, &k, &v) == MDB_SUCCESS &&
        v.mv_size >= sizeof(TextHeader)) {
      count = static_cast<const TextHeader *>(v.mv_data)->ref_count;
    }
    mdb_txn_abort(txn);
    return count;
  }

  // -- Virtual Xanadoc / Scroll Spans ("vspans") -----------------------------

  /// Register a virtual span mapping to a slice of a verified piece.
  bool put_vspan(const PrimediaSpan &span, const InfoHash &hash,
                 const std::uint32_t piece_index,
                 const std::uint32_t chunk_offset, const std::uint32_t length) {
    if (span.empty() || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto vkey = VSpanKey{static_cast<std::uint32_t>(span.scroll), 0,
                               span.start, span.length};
    MDB_val k{sizeof(vkey), const_cast<VSpanKey *>(&vkey)};

    const auto vrec =
        VSpanRecord{hash.bytes, piece_index, chunk_offset, length};
    MDB_val v{sizeof(vrec), const_cast<VSpanRecord *>(&vrec)};

    if (mdb_put(txn, dbi_vspans_, &k, &v, 0) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Retrieve virtual span record descriptor.
  [[nodiscard]] bool get_vspan(const PrimediaSpan &span,
                               VSpanRecord &out) const {
    if (span.empty() || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto vkey = VSpanKey{static_cast<std::uint32_t>(span.scroll), 0,
                               span.start, span.length};
    MDB_val k{sizeof(vkey), const_cast<VSpanKey *>(&vkey)};
    MDB_val v{};

    if (mdb_get(txn, dbi_vspans_, &k, &v) == MDB_SUCCESS &&
        v.mv_size == sizeof(VSpanRecord)) {
      std::memcpy(&out, v.mv_data, sizeof(VSpanRecord));
      mdb_txn_abort(txn);
      return true;
    }
    mdb_txn_abort(txn);
    return false;
  }

  /// Map a sub-range to an existing parent span's piece without copying text.
  bool put_subspan(const PrimediaSpan &subspan,
                   const PrimediaSpan &parent_span) {
    if (subspan.empty() || parent_span.empty() || !env_) {
      return false;
    }
    if (subspan.scroll != parent_span.scroll ||
        subspan.start < parent_span.start ||
        subspan.end() > parent_span.end()) {
      return false;
    }

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    // 1. Fetch parent virtual record (copy by value!)
    const auto parent_k =
        VSpanKey{static_cast<std::uint32_t>(parent_span.scroll), 0,
                 parent_span.start, parent_span.length};
    MDB_val pk{sizeof(parent_k), const_cast<VSpanKey *>(&parent_k)};
    MDB_val pv{};
    if (mdb_get(txn, dbi_vspans_, &pk, &pv) != MDB_SUCCESS ||
        pv.mv_size != sizeof(VSpanRecord)) {
      mdb_txn_abort(txn);
      return false;
    }
    const auto parent_rec = *static_cast<const VSpanRecord *>(pv.mv_data);

    // 2. Increment backing piece ref_count (copy buffer!)
    const auto pkey = PieceKey{parent_rec.hash, parent_rec.piece_index};
    MDB_val piecek{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val piecev{};
    if (mdb_get(txn, dbi_pieces_, &piecek, &piecev) != MDB_SUCCESS ||
        piecev.mv_size < sizeof(TextHeader)) {
      mdb_txn_abort(txn);
      return false;
    }
    std::vector<char> piece_buf(piecev.mv_size);
    std::memcpy(piece_buf.data(), piecev.mv_data, piecev.mv_size);
    auto *hdr = reinterpret_cast<TextHeader *>(piece_buf.data());
    hdr->ref_count++;
    MDB_val update_v{piece_buf.size(), piece_buf.data()};
    mdb_put(txn, dbi_pieces_, &piecek, &update_v, 0);

    // 3. Register subspan record with calculated offset
    const auto offset_delta =
        static_cast<std::uint32_t>(subspan.start - parent_span.start);
    const auto new_offset = parent_rec.chunk_offset + offset_delta;
    const auto sub_rec =
        VSpanRecord{parent_rec.hash, parent_rec.piece_index, new_offset,
                    static_cast<std::uint32_t>(subspan.length)};

    const auto sub_k = VSpanKey{static_cast<std::uint32_t>(subspan.scroll), 0,
                                subspan.start, subspan.length};
    MDB_val subk_val{sizeof(sub_k), const_cast<VSpanKey *>(&sub_k)};
    MDB_val subv_val{sizeof(sub_rec), const_cast<VSpanRecord *>(&sub_rec)};

    if (mdb_put(txn, dbi_vspans_, &subk_val, &subv_val, 0) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  // -- External Content Spans ("ext_spans") ----------------------------------

  /// Register an external stream coordinate mapping to a verified piece slice.
  bool put_ext_span(const InfoHash &hash, const std::uint64_t stream_offset,
                    const std::uint64_t length, const std::uint32_t piece_index,
                    const std::uint32_t chunk_offset) {
    if (0 == length || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto ext_k = ExtSpanKey{hash.bytes, 0, stream_offset, length};
    MDB_val k{sizeof(ext_k), const_cast<ExtSpanKey *>(&ext_k)};

    const auto vrec = VSpanRecord{hash.bytes, piece_index, chunk_offset,
                                  static_cast<std::uint32_t>(length)};
    MDB_val v{sizeof(vrec), const_cast<VSpanRecord *>(&vrec)};

    if (mdb_put(txn, dbi_ext_spans_, &k, &v, 0) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Retrieve external span record descriptor.
  [[nodiscard]] bool get_ext_span(const InfoHash &hash,
                                  const std::uint64_t stream_offset,
                                  const std::uint64_t length,
                                  VSpanRecord &out) const {
    if (0 == length || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto ext_k = ExtSpanKey{hash.bytes, 0, stream_offset, length};
    MDB_val k{sizeof(ext_k), const_cast<ExtSpanKey *>(&ext_k)};
    MDB_val v{};

    if (mdb_get(txn, dbi_ext_spans_, &k, &v) == MDB_SUCCESS &&
        v.mv_size == sizeof(VSpanRecord)) {
      std::memcpy(&out, v.mv_data, sizeof(VSpanRecord));
      mdb_txn_abort(txn);
      return true;
    }
    mdb_txn_abort(txn);
    return false;
  }

  // -- High-Level Combined API for Resolver ----------------------------------

  /// Cache text under a virtual primedia span.
  void put(const PrimediaSpan &span, const std::string_view text) {
    if (span.empty() || text.empty() || !env_) {
      return;
    }
    // Derive a deterministic synthetic hash for standalone/local scroll spans
    InfoHash synth_hash{};
    std::memcpy(synth_hash.bytes.data(), &span.scroll, sizeof(std::uint32_t));
    std::memcpy(synth_hash.bytes.data() + 4, &span.start,
                sizeof(std::uint64_t));

    put_piece(synth_hash, 0, text, 1);
    put_vspan(span, synth_hash, 0, 0, static_cast<std::uint32_t>(text.size()));
  }

  /// Resolve text for a virtual primedia span by reading the backing piece
  /// slice.
  [[nodiscard]] bool get(const PrimediaSpan &span, std::string &out) const {
    if (span.empty() || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto vkey = VSpanKey{static_cast<std::uint32_t>(span.scroll), 0,
                               span.start, span.length};
    MDB_val k{sizeof(vkey), const_cast<VSpanKey *>(&vkey)};
    MDB_val v{};

    if (mdb_get(txn, dbi_vspans_, &k, &v) != MDB_SUCCESS ||
        v.mv_size != sizeof(VSpanRecord)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto *vrec = static_cast<const VSpanRecord *>(v.mv_data);
    const auto pkey  = PieceKey{vrec->hash, vrec->piece_index};
    MDB_val piece_k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val piece_v{};

    if (mdb_get(txn, dbi_pieces_, &piece_k, &piece_v) != MDB_SUCCESS ||
        piece_v.mv_size < sizeof(TextHeader)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto *hdr = static_cast<const TextHeader *>(piece_v.mv_data);
    if (vrec->chunk_offset + vrec->length > hdr->length) {
      mdb_txn_abort(txn);
      return false;
    }

    const char *text_ptr = static_cast<const char *>(piece_v.mv_data) +
                           sizeof(TextHeader) + vrec->chunk_offset;
    out.assign(text_ptr, vrec->length);
    mdb_txn_abort(txn);
    return true;
  }

  /// Cache text under external stream coordinates.
  void put_external(const InfoHash &hash, const std::uint64_t stream_offset,
                    const std::string_view text) {
    if (text.empty() || !env_) {
      return;
    }
    put_piece(hash, 0, text, 1);
    put_ext_span(hash, stream_offset, text.size(), 0, 0);
  }

  /// Resolve text for external stream coordinates.
  [[nodiscard]] bool get_external(const InfoHash &hash,
                                  const std::uint64_t stream_offset,
                                  const std::uint64_t length,
                                  std::string &out) const {
    if (0 == length || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto ext_k = ExtSpanKey{hash.bytes, 0, stream_offset, length};
    MDB_val k{sizeof(ext_k), const_cast<ExtSpanKey *>(&ext_k)};
    MDB_val v{};

    if (mdb_get(txn, dbi_ext_spans_, &k, &v) != MDB_SUCCESS ||
        v.mv_size != sizeof(VSpanRecord)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto *vrec = static_cast<const VSpanRecord *>(v.mv_data);
    const auto pkey  = PieceKey{vrec->hash, vrec->piece_index};
    MDB_val piece_k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val piece_v{};

    if (mdb_get(txn, dbi_pieces_, &piece_k, &piece_v) != MDB_SUCCESS ||
        piece_v.mv_size < sizeof(TextHeader)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto *hdr = static_cast<const TextHeader *>(piece_v.mv_data);
    if (vrec->chunk_offset + vrec->length > hdr->length) {
      mdb_txn_abort(txn);
      return false;
    }

    const char *text_ptr = static_cast<const char *>(piece_v.mv_data) +
                           sizeof(TextHeader) + vrec->chunk_offset;
    out.assign(text_ptr, vrec->length);
    mdb_txn_abort(txn);
    return true;
  }

  // -- Reference-Counted Eviction --------------------------------------------

  /// Erase virtual Xanadoc span, decrementing ref_count and evicting piece if
  /// 0.
  bool erase(const PrimediaSpan &span) {
    if (span.empty() || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto vkey = VSpanKey{static_cast<std::uint32_t>(span.scroll), 0,
                               span.start, span.length};
    MDB_val k{sizeof(vkey), const_cast<VSpanKey *>(&vkey)};
    MDB_val v{};

    if (mdb_get(txn, dbi_vspans_, &k, &v) != MDB_SUCCESS ||
        v.mv_size != sizeof(VSpanRecord)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto vrec = *static_cast<const VSpanRecord *>(v.mv_data);
    mdb_del(txn, dbi_vspans_, &k, nullptr);

    // Decrement piece ref_count
    const auto pkey = PieceKey{vrec.hash, vrec.piece_index};
    MDB_val piece_k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val piece_v{};

    if (mdb_get(txn, dbi_pieces_, &piece_k, &piece_v) == MDB_SUCCESS &&
        piece_v.mv_size >= sizeof(TextHeader)) {
      std::vector<char> piece_buf(piece_v.mv_size);
      std::memcpy(piece_buf.data(), piece_v.mv_data, piece_v.mv_size);
      auto *hdr = reinterpret_cast<TextHeader *>(piece_buf.data());
      if (hdr->ref_count <= 1) {
        // Zero references left: evict raw piece data
        mdb_del(txn, dbi_pieces_, &piece_k, nullptr);
      } else {
        hdr->ref_count--;
        MDB_val update_v{piece_buf.size(), piece_buf.data()};
        mdb_put(txn, dbi_pieces_, &piece_k, &update_v, 0);
      }
    }

    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Erase external content span, decrementing ref_count and evicting piece if
  /// 0.
  bool erase_external(const InfoHash &hash, const std::uint64_t stream_offset,
                      const std::uint64_t length) {
    if (0 == length || !env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    const auto ext_k = ExtSpanKey{hash.bytes, 0, stream_offset, length};
    MDB_val k{sizeof(ext_k), const_cast<ExtSpanKey *>(&ext_k)};
    MDB_val v{};

    if (mdb_get(txn, dbi_ext_spans_, &k, &v) != MDB_SUCCESS ||
        v.mv_size != sizeof(VSpanRecord)) {
      mdb_txn_abort(txn);
      return false;
    }

    const auto vrec = *static_cast<const VSpanRecord *>(v.mv_data);
    mdb_del(txn, dbi_ext_spans_, &k, nullptr);

    // Decrement piece ref_count
    const auto pkey = PieceKey{vrec.hash, vrec.piece_index};
    MDB_val piece_k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    MDB_val piece_v{};

    if (mdb_get(txn, dbi_pieces_, &piece_k, &piece_v) == MDB_SUCCESS &&
        piece_v.mv_size >= sizeof(TextHeader)) {
      std::vector<char> piece_buf(piece_v.mv_size);
      std::memcpy(piece_buf.data(), piece_v.mv_data, piece_v.mv_size);
      auto *hdr = reinterpret_cast<TextHeader *>(piece_buf.data());
      if (hdr->ref_count <= 1) {
        mdb_del(txn, dbi_pieces_, &piece_k, nullptr);
      } else {
        hdr->ref_count--;
        MDB_val update_v{piece_buf.size(), piece_buf.data()};
        mdb_put(txn, dbi_pieces_, &piece_k, &update_v, 0);
      }
    }

    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Explicitly evict a piece from the raw text store.
  bool evict_piece(const InfoHash &hash, const std::uint32_t piece_index) {
    if (!env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }
    const auto pkey = PieceKey{hash.bytes, piece_index};
    MDB_val k{sizeof(pkey), const_cast<PieceKey *>(&pkey)};
    const int rc = mdb_del(txn, dbi_pieces_, &k, nullptr);
    if (rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Sweep and remove all unreferenced pieces (ref_count == 0).
  std::size_t evict_unreferenced() {
    if (!env_) {
      return 0;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return 0;
    }

    MDB_cursor *cursor = nullptr;
    if (mdb_cursor_open(txn, dbi_pieces_, &cursor) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return 0;
    }

    std::size_t evicted = 0;
    MDB_val k{};
    MDB_val v{};

    while (mdb_cursor_get(cursor, &k, &v, MDB_NEXT) == MDB_SUCCESS) {
      if (v.mv_size >= sizeof(TextHeader)) {
        const auto *hdr = static_cast<const TextHeader *>(v.mv_data);
        if (0 == hdr->ref_count) {
          mdb_cursor_del(cursor, 0);
          evicted++;
        }
      }
    }

    mdb_cursor_close(cursor);
    mdb_txn_commit(txn);
    return evicted;
  }

  // -- Transcopyright CEK (Content Encryption Key) Operations ----------------

  /// Store a verified Content Encryption Key (CEK) under keyId (32 bytes).
  bool put_cek(const std::array<std::uint8_t, 32> &keyId,
               const CekRecord &record) {
    if (!env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    MDB_val k{keyId.size(), const_cast<std::uint8_t *>(keyId.data())};
    MDB_val v{sizeof(record), const_cast<CekRecord *>(&record)};

    if (mdb_put(txn, dbi_ceks_, &k, &v, 0) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

  /// Retrieve a stored CEK by keyId.
  [[nodiscard]] bool get_cek(const std::array<std::uint8_t, 32> &keyId,
                             CekRecord &out) const {
    if (!env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
      return false;
    }

    MDB_val k{keyId.size(), const_cast<std::uint8_t *>(keyId.data())};
    MDB_val v{};

    if (mdb_get(txn, dbi_ceks_, &k, &v) != MDB_SUCCESS ||
        v.mv_size != sizeof(CekRecord)) {
      mdb_txn_abort(txn);
      return false;
    }

    std::memcpy(&out, v.mv_data, sizeof(CekRecord));
    mdb_txn_abort(txn);
    return true;
  }

  /// Check whether a CEK exists in the local cache.
  [[nodiscard]] bool has_cek(const std::array<std::uint8_t, 32> &keyId) const {
    CekRecord record;
    return get_cek(keyId, record);
  }

  /// Delete a CEK from the local cache.
  bool erase_cek(const std::array<std::uint8_t, 32> &keyId) {
    if (!env_) {
      return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
      return false;
    }

    MDB_val k{keyId.size(), const_cast<std::uint8_t *>(keyId.data())};
    if (mdb_del(txn, dbi_ceks_, &k, nullptr) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return false;
    }
    return mdb_txn_commit(txn) == MDB_SUCCESS;
  }

private:
  void open_databases() {
    MDB_txn *txn = nullptr;
    if (const int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
        rc != MDB_SUCCESS) {
      throw std::runtime_error("Failed to begin setup transaction: " +
                               std::string(mdb_strerror(rc)));
    }

    mdb_dbi_open(txn, "pieces", MDB_CREATE, &dbi_pieces_);
    mdb_dbi_open(txn, "vspans", MDB_CREATE, &dbi_vspans_);
    mdb_dbi_open(txn, "ext_spans", MDB_CREATE, &dbi_ext_spans_);
    mdb_dbi_open(txn, "ceks", MDB_CREATE, &dbi_ceks_);

    mdb_txn_commit(txn);
  }

  MDB_env *env_{nullptr};
  MDB_dbi dbi_pieces_{0};
  MDB_dbi dbi_vspans_{0};
  MDB_dbi dbi_ext_spans_{0};
  MDB_dbi dbi_ceks_{0};
};

} // namespace xudu

#endif // XUDU_LMDB_CACHE_HPP
