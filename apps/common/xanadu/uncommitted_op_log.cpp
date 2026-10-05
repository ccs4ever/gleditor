/**
 * @file uncommitted_op_log.cpp
 * @brief Implementation of UncommittedOpLog algebraic reduction and compaction.
 */
#include "uncommitted_op_log.hpp"

#include <algorithm>

namespace xanadu {

// UncommittedOpLog is written in C++ rather than Vortex because it operates
// directly on raw character input keystrokes on the interactive frame loop and
// input callback path (Session::textInserted / Session::textErased /
// Session::tick), requiring zero-allocation compaction and deterministic tight
// latency before committing transactions to the Xanadu store.

void UncommittedOpLog::recordInsert(const std::uint32_t at,
                                    const std::string_view utf8) {
  if (utf8.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  entries.push_back(UncommittedEntry{
      .kind      = UncommittedKind::Insert,
      .at        = at,
      .text      = std::string(utf8),
      .length    = static_cast<std::uint32_t>(utf8.size()),
      .timestamp = now,
  });
}

void UncommittedOpLog::recordErase(const std::uint32_t at,
                                   const std::string_view removed) {
  if (removed.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  entries.push_back(UncommittedEntry{
      .kind      = UncommittedKind::Delete,
      .at        = at,
      .text      = std::string(removed),
      .length    = static_cast<std::uint32_t>(removed.size()),
      .timestamp = now,
  });
}

std::vector<CompactedOp> UncommittedOpLog::compact() const {
  std::vector<CompactedOp> out;
  out.reserve(entries.size());

  for (auto entry : entries) {
    if (entry.kind == UncommittedKind::Insert) {
      if (!out.empty() && out.back().kind == OpKind::Insert) {
        const auto insEnd =
            out.back().at + static_cast<std::uint32_t>(out.back().text.size());
        if (insEnd == entry.at) {
          out.back().text += entry.text;
          continue;
        }
      }
      out.push_back(CompactedOp{
          .kind       = OpKind::Insert,
          .at         = entry.at,
          .text       = std::move(entry.text),
          .length     = 0,
          .reusedSpan = std::nullopt,
      });
      continue;
    }

    // Delete handling: consolidate contiguous and sequential deletes into
    // single delete ranges without truncating or annihilating preceding
    // inserts. In Xanadu's hypertime and permascroll model, every typed
    // character belongs to immutable history so that users can scrub backward
    // in hypertime to retrieve deleted text. Truncating inserts during playback
    // compaction would destroy that history.
    if (entry.length == 0) {
      continue;
    }

    if (!out.empty() && out.back().kind == OpKind::Delete) {
      // Sequential left-delete (Backspace): e.g. Backspace at 9 then at 8
      if (entry.at + entry.length == out.back().at) {
        out.back().at = entry.at;
        out.back().length += entry.length;
        continue;
      }

      // Sequential right-delete (Delete key): e.g. Delete at 5 then Delete at 5
      if (entry.at == out.back().at) {
        out.back().length += entry.length;
        continue;
      }

      // Adjacent forward delete: e.g. delete range [at, at+len) followed by
      // [at+len, ...)
      if (out.back().at + out.back().length == entry.at) {
        out.back().length += entry.length;
        continue;
      }
    }

    out.push_back(CompactedOp{
        .kind       = OpKind::Delete,
        .at         = entry.at,
        .text       = {},
        .length     = entry.length,
        .reusedSpan = std::nullopt,
    });
  }

  return out;
}

} // namespace xanadu
