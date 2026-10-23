/**
 * @file journal_lens.hpp
 * @brief JournalLens: systemd's journal (sd-journal) as cursor-keyed
 *        windows, newest first -- the evented log source
 *        (design/lens-candidates.md section 3.4).
 *
 * Where LogLens proves WindowedLens on a file's ordinals, JournalLens
 * proves it on a source whose own identity is already pk-strength: the
 * journal cursor (sd_journal_get_cursor) survives restarts and names
 * one entry exactly, so every line cell's key IS its entry's cursor
 * and every page's key is the cursor of the window's head entry --
 * keyset paging with the source's own key.
 *
 * Pages run newest-first (the tail is page 0, as the tail-is-window
 * spec wants); entries inside a page keep journal (reception) order.
 * One stub trails the materialized horizon -- materializePage fills it
 * and stubs the next-older window in turn.
 *
 * Built under GLEDITOR_HAVE_SYSTEMD (the Makefile's pkg-config probe,
 * same shape as GLEDITOR_SQL_PG); without libsystemd a refusing stub
 * compiles so journal: URIs still parse.
 */
#ifndef COMMON_XANADU_ZIGZAG_JOURNAL_LENS_HPP
#define COMMON_XANADU_ZIGZAG_JOURNAL_LENS_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

/// Opaque libsystemd journal handle (<systemd/sd-journal.h>).
/// Forward-declared so this header compiles on hosts whose build has no
/// libsystemd -- the probe decides which half of journal_lens.cpp the
/// object file carries.
struct sd_journal;

namespace zigzag {

/// systemd's journal as cursor-keyed windows, most recent first.
class JournalLens final : public ProjectionLens, public WindowedLens {
public:
  /// @p unitFilter names a _SYSTEMD_UNIT to match; empty projects the
  /// whole journal. Opens the local journal (SD_JOURNAL_LOCAL_ONLY),
  /// falling back to the calling user's own journal when the open is
  /// refused on permissions. No entries are read until browse().
  explicit JournalLens(std::string_view unitFilter = {},
                       std::size_t pageSize        = 64);
  ~JournalLens() override;
  JournalLens(const JournalLens &)            = delete;
  JournalLens &operator=(const JournalLens &) = delete;

  /// Whether a journal handle was opened. When false, browse() and
  /// materializePage() return openError() -- the lens still projects
  /// its root so a journal: URI answers rather than vanishing.
  [[nodiscard]] bool isOpen() const noexcept { return j_ != nullptr; }
  [[nodiscard]] const std::string &openError() const noexcept {
    return openErr_;
  }

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;
  bool forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
                       emit) const override;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey &key) const override;

  /// Read the journal tail and stage page 0 plus the deferred horizon.
  /// Already-live interior pages keep their windows across a rebrowse:
  /// a cursor-keyed window covers the same entries no matter what was
  /// appended since -- but a window the new tail grew into is dropped
  /// (its lines now live under the tail).
  [[nodiscard]] std::expected<void, std::string> browse();

  /// The pushed evented half: sd_journal_process reports APPEND (new
  /// entries) -- or INVALIDATE (rotation/removal, an equally real move).
  [[nodiscard]] bool pollChanged() const override;
  /// The journal's own change-notice fd (sd_journal_get_fd) -- what an
  /// app event loop should poll, with sd_journal_get_events' mask.
  [[nodiscard]] std::optional<int> waitableFd() const noexcept override;

  [[nodiscard]] std::expected<void, std::string>
  materializePage(const ExternalKey &windowKey) override;
  /// The trailing deferred page's key -- the next-page gesture's target.
  [[nodiscard]] std::optional<ExternalKey> nextStub() const override;
  void discardMaterializedAfter(const ExternalKey &boundaryKey) override;

private:
  struct JournalLine {
    std::string cursor; ///< sd_journal_get_cursor -- the entry's identity
    std::string text;   ///< "<isotime> <unit>: <MESSAGE>" rendering
  };

  struct JournalPage {
    ExternalKey key;        ///< <conn>:w:<cursor of the page's oldest entry>
    std::string headCursor; ///< the window's first entry
    bool stub{true};        ///< deferred until materialized
    std::vector<JournalLine> lines; ///< the window's entries, in order
    std::size_t epoch{0};
  };

  /// Render the entry the read pointer sits on (timestamp+unit+MESSAGE).
  [[nodiscard]] std::expected<JournalLine, std::string> currentLine() const;
  /// Collect up to pageSize_ entries forward from the current position
  /// (which must already sit on the window's head entry).
  [[nodiscard]] std::expected<std::vector<JournalLine>, std::string>
  collectForward();
  /// The head cursor of the window that would sit pageSize_ entries
  /// older than the page whose head is @p headCursor -- the next stub.
  [[nodiscard]] std::optional<std::string>
  olderHead(const std::string &headCursor) const;
  /// Read one field of the current entry, NAME= prefix stripped.
  [[nodiscard]] std::string field(const char *name) const;
  /// Append the stub for the next-older window after @p headCursor's
  /// page, when the journal reaches back that far.
  void appendStubAfter(const std::string &headCursor);

  std::string filter_;
  /// maybe_unused: the no-libsystemd half of the .cpp never reads these.
  [[maybe_unused]] std::size_t pageSize_;
  std::string connKey_;                    ///< journal / journal@<unit>
  sd_journal *j_{nullptr};                 ///< the open journal handle
  std::string openErr_;                    ///< why isOpen() is false
  mutable std::vector<JournalPage> pages_; ///< page 0 (tail) first
  [[maybe_unused]] mutable bool browsed_{
      false}; ///< pollChanged needs a baseline
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_JOURNAL_LENS_HPP
