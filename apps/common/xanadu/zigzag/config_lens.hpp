/**
 * @file config_lens.hpp
 * @brief ConfigLens: a structured config file as a ZigZag projection.
 *
 * JSON first (design/lens-candidates.md section 3.2), self-contained:
 * a strict recursive-descent parser, no new dependency. A member cell's
 * content IS its scalar where JSON has a scalar (typed cells -- VQL math
 * works on them), or its name where it's a container. Keys are
 * path-shaped: cfg:<abspath>:<json-pointer>, so a content edit on the
 * same path is identity-stable across refresh().
 */
#ifndef COMMON_XANADU_ZIGZAG_CONFIG_LENS_HPP
#define COMMON_XANADU_ZIGZAG_CONFIG_LENS_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/zigzag/file_watch.hpp"

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

/// A parsed JSON document: insertion-ordered objects, null as monostate.
struct JsonValue {
  using Array  = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;
  std::variant<std::monostate, bool, std::int64_t, double, std::string, Array,
               Object>
      v;
};

/// Strict RFC 8259: no comments, no trailing commas, no NaN/Infinity.
[[nodiscard]] std::expected<JsonValue, std::string>
parseJson(std::string_view text);

/// A JSON config file as a bounded enumeration projection. Read-only in
/// round one; the unproject half is the trait's refusing default, and
/// the write path is phase 0's session commit()/dirty-diff.
class ConfigLens final : public ProjectionLens {
public:
  /// Reads @p path now; refresh() re-reads it so file edits re-materialize.
  explicit ConfigLens(std::string path);
  /// Build from in-memory text (tests). @p path only names the key space.
  static std::expected<std::unique_ptr<ConfigLens>, std::string>
  fromText(std::string_view path, std::string_view text);

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
  /// mtime+size against the last enumeration's stamp (seeded lenses
  /// never change). The evented hint -- an app funnel calls refresh().
  [[nodiscard]] bool pollChanged() const override;
  /// The inotify fd where there is one (Linux) -- an app event loop can
  /// wait on it rather than poll.
  [[nodiscard]] std::optional<int> waitableFd() const noexcept override {
    return watch_.waitableFd();
  }
  /// Write a scalar edit back to the file at the key's JSON pointer --
  /// the unproject half. Container cells refuse (their content is a
  /// rendered label, not an editable member); the value's JSON type
  /// survives the edit ("9" on a number writes a number).
  [[nodiscard]] std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &node,
                     std::string_view newContent) override;

private:
  /// One buffered node; the tree is bounded so enumerate a snapshot.
  struct CfgNode {
    ExternalKey key;
    NodeContent content;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::string> childKeys; ///< member keys, file order
  };

  static void emitMembers(std::vector<CfgNode> &nodes, std::size_t parentIdx,
                          const JsonValue &value);

  std::string path_;
  std::string fileKey_;
  std::optional<JsonValue> seed_;  ///< fromText documents
  mutable FileWatch watch_{path_}; ///< kernel events; inert on a
                                   ///< nonexistent or seeded path
  /// mtime+size of path_ at the last enumeration (null for seeded).
  mutable std::optional<std::pair<std::int64_t, std::uint64_t>> stamp_;
  /// The epoch's snapshot, rebuilt at the head of each forEachNode --
  /// forEachEdge walks the same snapshot so one refresh is one tree.
  mutable std::vector<CfgNode> nodes_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_CONFIG_LENS_HPP
