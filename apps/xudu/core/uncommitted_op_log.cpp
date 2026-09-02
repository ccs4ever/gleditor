/**
 * @file uncommitted_op_log.cpp
 * @brief Implementation of UncommittedOpLog algebraic reduction and compaction.
 */
#include "uncommitted_op_log.hpp"

#include <algorithm>

namespace xudu {

namespace {

/// Whether @p at is the start of a character in @p text, so cutting there
/// leaves valid UTF-8 on both sides. Continuation bytes are 10xxxxxx.
bool isUtf8Boundary(const std::string &text, const std::size_t at) noexcept {
  if (at >= text.size()) {
    return at == text.size();
  }
  return (static_cast<unsigned char>(text[at]) & 0xC0U) != 0x80U;
}

} // namespace

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

    // Delete handling
    bool handled = false;
    while (!out.empty() && entry.length > 0) {
      if (out.back().kind == OpKind::Insert) {
        const auto insAt  = out.back().at;
        const auto insLen = static_cast<std::uint32_t>(out.back().text.size());
        const auto insEnd = insAt + insLen;
        const auto delEnd = entry.at + entry.length;

        // Check if delete hits within the tail of the recent insert
        if (delEnd == insEnd && entry.at >= insAt) {
          const auto remainingLen = entry.at - insAt;
          // These offsets are byte offsets -- recordInsert takes length from
          // utf8.size() -- so a cut can land inside a multi-byte character.
          // Coalescing is an optimisation, and an optimisation that can
          // produce invalid UTF-8 is not one: leave the ops uncombined and
          // let them apply in sequence, which is always correct.
          if (isUtf8Boundary(out.back().text, remainingLen)) {
            out.back().text.resize(remainingLen);
            if (out.back().text.empty()) {
              out.pop_back();
            }
            handled = true;
          }
          break;
        }

        // Check if delete completely subsumes the insert
        if (entry.at <= insAt && delEnd >= insEnd) {
          const auto extraBefore = insAt - entry.at;
          const auto extraAfter  = delEnd - insEnd;
          out.pop_back();
          entry.length = extraBefore + extraAfter;
          if (entry.length == 0) {
            handled = true;
            break;
          }
          // Continue looping to merge remaining delete into earlier ops
          continue;
        }

        break;
      }

      if (out.back().kind == OpKind::Delete) {
        // Sequential left-delete (Backspace): e.g. Backspace at 9 then at 8
        if (entry.at + entry.length == out.back().at) {
          out.back().at = entry.at;
          out.back().length += entry.length;
          handled = true;
          break;
        }

        // Sequential right-delete (Delete key): e.g. Delete at 5 then Delete at
        // 5
        if (entry.at == out.back().at) {
          out.back().length += entry.length;
          handled = true;
          break;
        }

        break;
      }

      break;
    }

    if (!handled && entry.length > 0) {
      out.push_back(CompactedOp{
          .kind       = OpKind::Delete,
          .at         = entry.at,
          .text       = {},
          .length     = entry.length,
          .reusedSpan = std::nullopt,
      });
    }
  }

  return out;
}

} // namespace xudu
