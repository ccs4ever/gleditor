/**
 * @file filesystem_lens.cpp
 * @brief Implementation of the directory-tree projection lens.
 */
#include "common/xanadu/zigzag/filesystem_lens.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "common/xanadu/magic_mime.hpp"

namespace zigzag {

namespace {

// The stable key for an entry: "fs:<dev>:<ino>" so a rename / intra-device move
// preserves identity (§5.2).
ExternalKey keyForStat(const struct stat &st) {
  return std::format("fs:{}:{}", static_cast<std::uint64_t>(st.st_dev),
                     static_cast<std::uint64_t>(st.st_ino));
}

} // namespace

FilesystemLens::FilesystemLens(std::filesystem::path root)
    : root_(std::move(root)) {
  walk();
}

void FilesystemLens::walk() {
  xanadu::MagicMimeDetector mime;

  // A deterministic sorted depth-first walk (invariant 6): at each directory,
  // sort children by filename so one tree always projects to one cell layout.
  struct Frame {
    std::filesystem::path path;
    std::optional<ExternalKey> parentKey;
  };

  auto statOf = [](const std::filesystem::path &p, struct stat &st) -> bool {
    return ::lstat(p.c_str(), &st) == 0;
  };

  auto makeEntry = [&](const std::filesystem::path &p,
                       std::optional<ExternalKey> parent,
                       std::uint64_t order) -> std::optional<Entry> {
    struct stat st{};
    if (!statOf(p, st)) return std::nullopt;
    Entry e;
    e.key    = keyForStat(st);
    e.name   = p.filename().empty() ? p.string() : p.filename().string();
    e.parent = std::move(parent);
    e.order  = order;
    e.size   = static_cast<std::int64_t>(st.st_size);
    e.mtime  = static_cast<std::int64_t>(st.st_mtime);
    e.perm   = static_cast<std::int64_t>(st.st_mode & 07777);
    if (S_ISDIR(st.st_mode)) {
      e.role = "dir";
      e.mime = "inode/directory";
    } else if (S_ISLNK(st.st_mode)) {
      e.role = "symlink";
      e.mime = "inode/symlink";
      std::error_code ec;
      auto tgt = std::filesystem::read_symlink(p, ec);
      if (!ec) {
        // Resolve the target's key if it exists and is stat-able.
        auto abs = tgt.is_absolute() ? tgt : (p.parent_path() / tgt);
        struct stat tst{};
        if (statOf(abs, tst)) e.symTarget = keyForStat(tst);
      }
    } else {
      e.role = "file";
      std::error_code ec;
      e.mime = mime.identifyFile(p.string());
      if (e.mime.empty()) e.mime = "application/octet-stream";
    }
    return e;
  };

  // Root entry.
  {
    auto root = makeEntry(root_, std::nullopt, 0);
    if (!root) return; // root unreadable -> empty projection
    entries_.push_back(*root);
  }

  // Iterative DFS so a deep tree does not blow the stack. Each frame expands a
  // directory's sorted children.
  std::vector<Frame> stack;
  stack.push_back({root_, entries_.front().key});
  while (!stack.empty()) {
    Frame f = std::move(stack.back());
    stack.pop_back();

    struct stat st{};
    if (!statOf(f.path, st) || !S_ISDIR(st.st_mode)) continue;

    std::vector<std::filesystem::path> children;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(f.path, ec), end;
         !ec && it != end; it.increment(ec)) {
      children.push_back(it->path());
    }
    std::ranges::sort(children, [](const auto &a, const auto &b) {
      return a.filename().string() < b.filename().string();
    });

    std::uint64_t order = 0;
    for (const auto &child : children) {
      auto e = makeEntry(child, f.parentKey, order++);
      if (!e) continue;
      ExternalKey childKey            = e->key;
      std::filesystem::path childPath = child;
      std::string role                = e->role;
      entries_.push_back(std::move(*e));
      if (role == "dir") stack.push_back({childPath, childKey});
    }
  }
}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
FilesystemLens::dimensions() const {
  return {
      {std::string(genericDimName(GenericDim::Containment)),
       GenericDim::Containment},
      {std::string(genericDimName(GenericDim::Sequence)), GenericDim::Sequence},
      {std::string(genericDimName(GenericDim::Reference)),
       GenericDim::Reference},
      {"d.role", std::nullopt},
      {"d.mime", std::nullopt},
      {"d.size", std::nullopt},
      {"d.mtime", std::nullopt},
      {"d.perm", std::nullopt},
      {"d.name", std::nullopt},
  };
}

void FilesystemLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  for (const auto &e : entries_) {
    SourceNode node;
    node.key     = e.key;
    node.content = e.name; // the cell's text is the entry name
    node.order   = e.order;
    node.attributes.emplace_back("d.role", e.role);
    node.attributes.emplace_back("d.mime", e.mime);
    node.attributes.emplace_back("d.size", std::to_string(e.size));
    node.attributes.emplace_back("d.mtime", std::to_string(e.mtime));
    node.attributes.emplace_back("d.perm", std::format("{:o}", e.perm));
    emit(node);
  }
}

void FilesystemLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  // Containment: parent -> FIRST child only (a dimension gives each cell one
  // posward neighbour; siblings chain on d.seq, Xanadu's rank model). Sequence:
  // chain siblings in sorted order. Reference: symlink -> target.
  std::unordered_map<ExternalKey, ExternalKey> lastSiblingOf;
  for (const auto &e : entries_) {
    if (e.parent) {
      const auto it = lastSiblingOf.find(*e.parent);
      if (it == lastSiblingOf.end()) {
        // First child of this parent: the containment edge.
        emit(SourceEdge{
            .from      = *e.parent,
            .to        = e.key,
            .dimension = std::string(genericDimName(GenericDim::Containment)),
            .dir       = Posward});
      } else {
        // A later sibling: chain it after the previous one on d.seq.
        emit(SourceEdge{.from = it->second,
                        .to   = e.key,
                        .dimension =
                            std::string(genericDimName(GenericDim::Sequence)),
                        .dir = Posward});
      }
      lastSiblingOf[*e.parent] = e.key;
    }
    if (e.symTarget) {
      emit(SourceEdge{.from = e.key,
                      .to   = *e.symTarget,
                      .dimension =
                          std::string(genericDimName(GenericDim::Reference)),
                      .dir = Posward});
    }
  }
}

} // namespace zigzag
