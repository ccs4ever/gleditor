/**
 * @file lens_uri.cpp
 * @brief openLensUri: one URI scheme -> its lens.
 */

#include "common/xanadu/zigzag/lens_uri.hpp"

#include <algorithm>
#include <cctype>
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
  // Refuse any unrecognized `scheme:` prefix (ALPHA *(ALPHA / DIGIT /
  // '+' / '.' / '-') ':') before the dispatch below. `meta:` and
  // friends used to fall through to FilesystemLens and read a file
  // literally named "meta:x" -- and `union:meta:x|dir:/y` recursed
  // into the same hole; the union: branch propagates a child refusal,
  // so this one guard closes both. A file genuinely named
  // "weird:name" now refuses too -- a documented regression; '/'- or
  // './'-leading paths still escape the check.
  if (const auto colon = uri.find(':'); colon != std::string_view::npos) {
    const auto scheme = uri.substr(0, colon);
    const bool shaped =
        !scheme.empty() &&
        std::isalpha(static_cast<unsigned char>(scheme.front())) &&
        std::ranges::all_of(scheme.substr(1), [](const char c) {
          const auto u = static_cast<unsigned char>(c);
          return std::isalnum(u) || c == '+' || c == '.' || c == '-';
        });
    constexpr std::string_view kSchemes[]{"union",   "sql", "cfg", "log",
                                          "journal", "git", "dir"};
    if (shaped &&
        std::ranges::find(kSchemes, scheme) == std::ranges::end(kSchemes)) {
      return std::unexpected("unrecognized lens scheme '" +
                             std::string{scheme} + ":' in '" +
                             std::string{uri} + "'");
    }
  }
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
  const std::filesystem::path path{uri};
  if (std::filesystem::is_directory(path, ec)) {
    return std::unique_ptr<ProjectionLens>{
        std::make_unique<RouterLens>(std::string{uri})};
  }
  // A file the router would classify opens its own lens -- the
  // picker's choice of a .json/.log/.db/.service opens the same
  // projection the mount gesture would mount under it. Files with
  // no delegate keep the single-cell FilesystemLens.
  if (std::filesystem::exists(path, ec)) {
    if (auto delegate = RouterLens::delegateFor(path); delegate) {
      return std::unique_ptr<ProjectionLens>{std::move(*delegate)};
    }
  }
  return std::unique_ptr<ProjectionLens>{
      std::make_unique<FilesystemLens>(std::string{uri})};
}

} // namespace zigzag
