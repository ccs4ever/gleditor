/**
 * @file log_lens.hpp
 * @brief LogLens: a log file's tail as a windowed projection.
 *
 * The second WindowedLens (design/lens-candidates.md section 3.4) --
 * the cheapest possible proof that materializePage/
 * discardMaterializedAfter generalize past SQL. Time-ordered lines are
 * a natural rank; "most recent N" is literally a window, so page 0 is
 * the file's tail and older pages materialize as stubs on demand.
 *
 * Identity is positional: a line's key is its byte-0-ordinal line
 * number, honest d.keystab=ord -- an append keeps every existing line's
 * ordinal stable, and a rewrite that shifts lines changes their keys
 * the way the ladder says it must.
 */
#ifndef COMMON_XANADU_ZIGZAG_LOG_LENS_HPP
#define COMMON_XANADU_ZIGZAG_LOG_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// A text log file as ordinal-paged windows, most recent first.
class LogLens final : public ProjectionLens, public WindowedLens {
public:
  /// @p path names the log file; @p pageSize bounds each window's lines.
  /// Reads lazily: no file IO until materializePage (or browse()).
  explicit LogLens(std::string path, std::size_t pageSize = 64);

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

  /// Read the file tail and stage page 0 plus the deferred horizon.
  /// Already-live interior pages keep their lines across a rebrowse --
  /// a tail append shifts the newest window, not the file's middle.
  [[nodiscard]] std::expected<void, std::string> browse();

  /// The pulled evented hint: the file's mtime+size against the last
  /// browse's stamp. The app's funnel calls refresh() when true.
  [[nodiscard]] bool pollChanged() const override;

  [[nodiscard]] std::expected<void, std::string>
  materializePage(const ExternalKey &windowKey) override;
  void discardMaterializedAfter(const ExternalKey &boundaryKey) override;

private:
  struct LogPage {
    ExternalKey key;                ///< log:<path>:w:<firstLine>
    std::size_t firstLine{0};       ///< ordinal of the page's head
    std::size_t count{0};           ///< lines in the window
    bool stub{true};                ///< deferred until materialized
    std::vector<std::string> lines; ///< the window's text, in order
    std::size_t epoch{0};
  };

  /// One buffered file snapshot: line count + the reads that answered
  /// materializePage calls this epoch.
  [[nodiscard]] std::expected<std::uint64_t, std::string> lineCount() const;
  /// Read lines [first, first+count) -- or fewer at the file's end.
  [[nodiscard]] std::expected<std::vector<std::string>, std::string>
  readLines(std::size_t first, std::size_t count) const;

  /// mtime+size of the file at the last browse; different = moved.
  [[nodiscard]] std::optional<std::pair<std::int64_t, std::uint64_t>>
  fileStamp() const;

  std::string path_;
  std::size_t pageSize_;
  std::string connKey_;                ///< log:<path> the file node
  mutable std::vector<LogPage> pages_; ///< page 0 first, older after
  mutable std::uint64_t totalLines_{0};
  mutable std::optional<std::pair<std::int64_t, std::uint64_t>> stamp_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_LOG_LENS_HPP
