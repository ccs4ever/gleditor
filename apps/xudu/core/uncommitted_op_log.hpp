/**
 * @file uncommitted_op_log.hpp
 * @brief Transaction log for uncommitted interactive edits with algebraic op
 *        coalescing and annihilation.
 */
#ifndef XUDU_UNCOMMITTED_OP_LOG_HPP
#define XUDU_UNCOMMITTED_OP_LOG_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ops.hpp"
#include "spool.hpp"

namespace xudu {

enum class UncommittedKind : std::uint8_t {
  Insert,
  Delete,
};

struct UncommittedEntry {
  UncommittedKind kind{UncommittedKind::Insert};
  std::uint32_t at{0};
  std::string text; ///< Inserted text, or captured erased text for delete
  std::uint32_t length{0}; ///< Length in bytes
  std::chrono::steady_clock::time_point timestamp{};
};

struct CompactedOp {
  OpKind kind{OpKind::Insert};
  std::uint32_t at{0};
  std::string text;                       ///< If insert: text to be spanned
  std::uint32_t length{0};                ///< If delete: length to erase
  std::optional<PrimediaSpan> reusedSpan; ///< Set if span was deduplicated
};

/**
 * @class UncommittedOpLog
 * @brief Collects interactive user edits and algebraically compacts them on
 *        commit boundaries into minimal canonical operations.
 */
class UncommittedOpLog {
public:
  UncommittedOpLog() = default;

  void recordInsert(std::uint32_t at, std::string_view utf8);
  void recordErase(std::uint32_t at, std::string_view removed);

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
  [[nodiscard]] const std::vector<UncommittedEntry> &
  rawEntries() const noexcept {
    return entries;
  }

  [[nodiscard]] std::chrono::steady_clock::time_point
  lastActivity() const noexcept {
    return entries.empty() ? std::chrono::steady_clock::time_point{}
                           : entries.back().timestamp;
  }

  void clear() noexcept { entries.clear(); }

  /**
   * @brief Compact the recorded operations using algebraic coalescing and
   *        annihilation rules.
   *
   * 1. Contiguous sequential inserts are merged into single spans.
   * 2. Consecutive backspaces (left-deletes) and forward deletes are merged
   * into single delete ranges.
   * 3. Insertions immediately followed by backspaces on the newly inserted text
   *    are truncated or annihilated.
   */
  [[nodiscard]] std::vector<CompactedOp> compact() const;

private:
  std::vector<UncommittedEntry> entries;
};

} // namespace xudu

#endif // XUDU_UNCOMMITTED_OP_LOG_HPP
