/**
 * @file lens_uri.cpp
 * @brief openLensUri: one URI scheme -> its lens.
 */

#include "common/xanadu/zigzag/lens_uri.hpp"

#include <filesystem>

#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/filesystem_lens.hpp"
#include "common/xanadu/zigzag/git_lens.hpp"
#include "common/xanadu/zigzag/journal_lens.hpp"
#include "common/xanadu/zigzag/log_lens.hpp"
#include "common/xanadu/zigzag/router_lens.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"
#include "common/xanadu/zigzag/union_lens.hpp"

namespace zigzag {

std::expected<std::unique_ptr<ProjectionLens>, std::string>
openLensUri(const std::string_view uri) {
  // union:a|b|c -- multi-lens composition over the same epoch; '|' is
  // the delimiter (URIs/paths that legitimately carry one would
  // percent-encode it anyway).
  if (uri.starts_with("union:")) {
    auto u   = std::make_unique<UnionLens>();
    auto rem = uri.substr(6);
    while (!rem.empty()) {
      const auto bar  = rem.find('|');
      const auto part = rem.substr(0, bar);
      rem = bar == std::string_view::npos ? "" : rem.substr(bar + 1);
      if (part.empty()) {
        continue;
      }
      auto child = openLensUri(part);
      if (!child) {
        return std::unexpected(child.error());
      }
      u->add(std::move(*child));
    }
    return std::unique_ptr<ProjectionLens>{std::move(u)};
  }
  if (uri.starts_with("sql:") ||
      (!uri.starts_with('/') && uri.find(':') == std::string_view::npos &&
       (uri.ends_with(".db") || uri.ends_with(".sqlite") ||
        uri.ends_with(".sqlite3")))) {
    auto lens = openSqlUri(uri);
    if (!lens) {
      return std::unexpected(lens.error());
    }
    return std::unique_ptr<ProjectionLens>{std::move(*lens)};
  }
  if (uri.starts_with("cfg:")) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<ConfigLens>(std::string{uri.substr(4)})};
  }
  if (uri.starts_with("log:")) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<LogLens>(std::string{uri.substr(4)})};
  }
  // journal: is the whole local journal; journal:<unit> matches one
  // _SYSTEMD_UNIT. A refused open still yields a lens (the root cell
  // projects; browse() carries the error), like the sql:pg: stub.
  if (uri.starts_with("journal:")) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<JournalLens>(uri.substr(8))};
  }
  if (uri.starts_with("git:")) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<GitLens>(std::string{uri.substr(4)})};
  }
  // A directory opens through RouterLens so the open/back gestures can
  // mount delegate lenses under its cells; a bare file path keeps the
  // plain FilesystemLens it always was.
  if (uri.starts_with("dir:")) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<RouterLens>(std::string{uri.substr(4)})};
  }
  std::error_code ec;
  if (std::filesystem::is_directory(std::filesystem::path{uri}, ec)) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<RouterLens>(std::string{uri})};
  }
  return std::unique_ptr<ProjectionLens>{
      std::make_unique<FilesystemLens>(std::string{uri})};
}

} // namespace zigzag
