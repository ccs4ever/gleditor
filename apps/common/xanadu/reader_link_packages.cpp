#include "reader_link_packages.hpp"
#include "bencode.hpp"
#include "store.hpp"
#include <fstream>
#include <openssl/sha.h>
#include <tuple>

namespace xanadu {
namespace {
constexpr std::size_t maximumChoices        = 128;
constexpr std::size_t maximumEnabledMembers = 4096;
using V                                     = bencode::Value;
std::size_t members(const LinkPackage &package) {
  std::size_t count = 0;
  for (const auto &link : package.links)
    count += link.left.size() + link.right.size();
  return count;
}
} // namespace
ReaderLinkPackages::ReaderLinkPackages(std::filesystem::path path)
    : file(std::move(path)) {
  if (file.empty()) return;
  try {
    if (std::filesystem::is_symlink(file))
      throw std::runtime_error("symlink preferences");
    if (!std::filesystem::exists(file)) return;
    if (std::filesystem::file_size(file) > 64 * 1024)
      throw std::runtime_error("byte limit");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("unreadable preferences");
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    const auto root    = bencode::decode(bytes);
    const auto version = root.asDict().at("format").asInteger();
    if (version != 1)
      throw std::runtime_error("expected version 1, got " +
                               std::to_string(version));
    if (root.asDict().size() != 2)
      throw std::runtime_error("unexpected fields");
    for (const auto &[hash, value] : root.asDict().at("choices").asDict()) {
      if (value.asInteger() != 0 && value.asInteger() != 1)
        throw std::runtime_error("invalid choice");
      choices.emplace(InfoHash::fromHex(hash), value.asInteger() == 1);
    }
    if (choices.size() > maximumChoices)
      throw std::runtime_error("choice limit");
    for (const auto &[hash, on] : choices) {
      (void)on;
      const auto [entry, inserted] =
          authorities.emplace(keyOf(hash, 0).authority.str(), hash);
      if (!inserted && entry->second != hash)
        throw std::runtime_error("activity authority collision");
    }
  } catch (const std::exception &error) {
    throw ReaderLinkPackagesUnreadable("Reader package preferences format 1: " +
                                       std::string(error.what()));
  }
}
LinkKey ReaderLinkPackages::keyOf(const InfoHash &hash, std::size_t ordinal) {
  // The existing activity authority is 128 bits. Domain-separated immutable
  // authorities retain that shape; retain() checks the verified reverse map.
  const auto name = "xudu-reader-package:1:" + hash.hex();
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(name.data()), name.size(),
         digest.data());
  DocumentId authority;
  (void)DocumentId::fromBytes(
      std::string_view(reinterpret_cast<const char *>(digest.data()), 16),
      authority);
  return {.authority = authority, .id = static_cast<zigzag::CellRef>(ordinal)};
}
std::expected<ReaderLinkPackages *, std::string>
ReaderLinkPackages::retain(const LinkPackage &package,
                           const InfoHash &hash) try {
  reviewLinkPackage(package);
  const std::vector<TorrentContent> files{
      {.path = "links.xanalinks", .data = encodeLinkPackage(package)}};
  if (makeTorrent(files, "link-package").hash != hash)
    throw std::invalid_argument("Package carrier identity mismatch");
  if (const auto entry = authorities.find(keyOf(hash, 0).authority.str());
      entry != authorities.end() && entry->second != hash)
    throw std::invalid_argument("Package activity authority collision");
  if (!retained.contains(hash) && retained.size() >= maximumChoices)
    throw std::invalid_argument("Retained package limit");
  std::size_t total = enabled(hash) ? members(package) : 0;
  for (const auto &[other, pkg] : retained)
    if (other != hash && enabled(other)) total += members(pkg);
  if (total > maximumEnabledMembers)
    throw std::invalid_argument("Enabled package member limit (4096)");
  retained.insert_or_assign(hash, package);
  authorities.insert_or_assign(keyOf(hash, 0).authority.str(), hash);
  return this;
} catch (const std::exception &error) {
  return std::unexpected(std::string(error.what()));
}
bool ReaderLinkPackages::containsAuthority(const DocumentId &authority) const {
  return authorities.contains(authority.str());
}
bool ReaderLinkPackages::enabled(const InfoHash &hash) const {
  const auto found = choices.find(hash);
  return found != choices.end() && found->second;
}
std::expected<ReaderLinkPackages *, std::string>
ReaderLinkPackages::setEnabled(const InfoHash &hash, bool on) try {
  if (on && !retained.contains(hash))
    throw std::invalid_argument(
        "Verify the immutable package before enabling it");
  auto next  = choices;
  next[hash] = on;
  if (next.size() > maximumChoices)
    throw std::invalid_argument("Package choice limit");
  std::size_t total = 0;
  for (const auto &[key, package] : retained)
    if (next.contains(key) && next.at(key)) total += members(package);
  if (total > maximumEnabledMembers)
    throw std::invalid_argument("Enabled package member limit (4096)");
  if (!file.empty()) {
    if (std::filesystem::is_symlink(file) ||
        std::filesystem::is_symlink(file.string() + ".partial"))
      throw ReaderLinkPackagesUnreadable(
          "Reader package preferences format 1: unsafe output path");
    std::filesystem::create_directories(file.parent_path());
    bencode::Dict encoded;
    for (const auto &[key, choice] : next)
      encoded.emplace(key.hex(), V::integer(choice));
    std::ofstream out(file.string() + ".partial",
                      std::ios::binary | std::ios::trunc);
    std::filesystem::permissions(file.string() + ".partial",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write);
    out << V::dict({{"choices", V::dict(std::move(encoded))},
                    {"format", V::integer(1)}})
               .encode();
    out.close();
    if (!out)
      throw std::runtime_error("Cannot retain reader package preferences");
    std::filesystem::rename(file.string() + ".partial", file);
  }
  choices = std::move(next);
  return this;
} catch (const std::exception &error) {
  return std::unexpected(std::string(error.what()));
}
std::vector<LinkKey> ReaderLinkPackages::links() const {
  std::vector<LinkKey> result;
  for (const auto &[hash, pkg] : retained) {
    if (enabled(hash)) {
      const auto authority = keyOf(hash, 0).authority;
      for (std::size_t ordinal = 0; ordinal < pkg.links.size(); ++ordinal)
        result.push_back({.authority = authority,
                          .id        = static_cast<zigzag::CellRef>(ordinal)});
    }
  }
  return result;
}
const LinkPackage *ReaderLinkPackages::package(const LinkKey &key) const {
  const auto authority = authorities.find(key.authority.str());
  if (authority == authorities.end() || !enabled(authority->second))
    return nullptr;
  const auto pkg = retained.find(authority->second);
  return pkg != retained.end() && key.id < pkg->second.links.size()
             ? &pkg->second
             : nullptr;
}
const GlobalLink *ReaderLinkPackages::find(const LinkKey &key) const {
  const auto *pkg = package(key);
  return pkg ? &pkg->links.at(key.id) : nullptr;
}
std::string readerScrollKey(const Store &store, ScrollId slot) {
  if (slot == localScroll) {
    if (const auto scroll = store.scroll(store.publishedLocalScroll()))
      return scrollKey(*scroll);
    if (!store.bootstrapPermascrollKey().empty())
      return store.bootstrapPermascrollKey();
    return store.userPermascroll().globalScrollKey();
  }
  return globalKeyOf(store, {.scroll = slot});
}
PackageOccurrenceIndex::PackageOccurrenceIndex(
    std::span<const PackageDocumentView> docs,
    std::span<const PackageCellView> views)
    : documents(docs.begin(), docs.end()), cells(views.begin(), views.end()) {
  std::map<std::string, std::vector<enfilade::SpanEntry>> entries;
  const auto add = [&](const Store &store, std::span<const PrimediaSpan> pieces,
                       std::uint32_t view, zigzag::CellRef cell, bool isCell) {
    std::uint32_t offset = 0;
    std::map<ScrollId, std::string> keys;
    for (const auto &piece : pieces) {
      if (!keys.contains(piece.scroll))
        keys.emplace(piece.scroll, readerScrollKey(store, piece.scroll));
      const auto &key = keys.at(piece.scroll);
      if (!key.empty() && !piece.empty())
        entries[key].push_back({.start     = piece.start,
                                .length    = piece.length,
                                .docId     = view,
                                .docOffset = offset,
                                .cellDense = cell,
                                .flags = static_cast<std::uint16_t>(isCell)});
      offset += static_cast<std::uint32_t>(piece.length);
    }
  };
  for (std::uint32_t i = 0; i < documents.size(); ++i)
    add(documents[i].store, documents[i].text.pieces(), i, 0, false);
  for (std::uint32_t i = 0; i < cells.size(); ++i)
    for (const auto cell : cells[i].cells)
      add(cells[i].store, cells[i].manifold.contentOf(cell), i, cell, true);
  for (auto &[key, pieces] : entries) scrolls[key].bulkLoad(std::move(pieces));
  for (auto &view : cells) view.cells = {};
}
std::vector<Occurrence>
PackageOccurrenceIndex::occurrences(const GlobalSpan &member) const {
  const auto found = scrolls.find(member.scroll);
  if (found == scrolls.end() || member.length == 0) return {};
  auto hits = found->second.query(member.start, member.end());
  std::ranges::sort(hits, [](const auto &a, const auto &b) {
    return std::tuple{a.flags, a.docId, a.cellDense, a.docOffset} <
           std::tuple{b.flags, b.docId, b.cellDense, b.docOffset};
  });
  std::vector<Occurrence> result;
  for (std::size_t begin = 0; begin < hits.size();) {
    auto end          = begin + 1;
    const auto &first = hits[begin];
    while (end < hits.size() && hits[end].flags == first.flags &&
           hits[end].docId == first.docId &&
           hits[end].cellDense == first.cellDense)
      ++end;
    std::vector<PrimediaSpan> pieces;
    std::uint32_t offset = 0;
    for (auto i = begin; i < end; ++i) {
      const auto &hit = hits[i];
      if (hit.docOffset > offset)
        pieces.push_back(
            {.scroll = 1, .start = 0, .length = hit.docOffset - offset});
      pieces.push_back({.scroll = 0, .start = hit.start, .length = hit.length});
      offset = hit.docOffset + static_cast<std::uint32_t>(hit.length);
    }
    for (const auto &match : exactOccurrences(
             pieces,
             {.scroll = 0, .start = member.start, .length = member.length})) {
      if (first.isCell()) {
        const auto &view = cells[first.docId];
        result.push_back({.site     = CellSite{.store   = view.store.documentId(),
                                               .version = view.version,
                                               .cell    = first.cellDense,
                                               .range   = match.range},
                          .coverage = match.coverage});
      } else {
        const auto &view = documents[first.docId];
        result.push_back({.site = DocumentSite{.store = view.store.documentId(),
                                               .version = view.version,
                                               .range   = match.range},
                          .coverage = match.coverage});
      }
    }
    begin = end;
  }
  return result;
}
std::vector<PieceMatch> packageOccurrences(const Store &store,
                                           std::span<const PrimediaSpan> pieces,
                                           const GlobalSpan &member) {
  Version text;
  text.insertSpans(0, std::vector<PrimediaSpan>(pieces.begin(), pieces.end()));
  const std::vector<PackageDocumentView> docs{{store, {}, text}};
  PackageOccurrenceIndex index(docs, {});
  std::vector<PieceMatch> result;
  for (const auto &occurrence : index.occurrences(member))
    result.push_back({.range    = std::get<DocumentSite>(occurrence.site).range,
                      .coverage = occurrence.coverage});
  return result;
}
LinkOccurrences PackageOccurrenceIndex::resolve(const LinkKey &key,
                                                const LinkPackage &pkg) const {
  const auto &global = pkg.links.at(key.id);
  Link link;
  link.id    = key.id;
  link.type  = global.type;
  link.tier  = ProminenceTier::Curated;
  link.owner = pkg.title + " — curator " + pkg.curator.hex().substr(0, 12) +
               " / " + global.owner;
  link.curator = pkg.curator.hex();
  LinkOccurrences result{.key = key, .link = link};
  for (const auto side : {LinkSide::Left, LinkSide::Right}) {
    auto &members = side == LinkSide::Left ? result.left : result.right;
    auto &spans = side == LinkSide::Left ? result.link.left : result.link.right;
    const auto &source = side == LinkSide::Left ? global.left : global.right;
    for (const auto &span : source) {
      LinkMember member{
          .side  = side,
          .index = static_cast<std::uint32_t>(members.size()),
          .span  = {.scroll = 0, .start = span.start, .length = span.length},
          .occurrences = occurrences(span)};
      spans.push_back(member.span);
      members.push_back(std::move(member));
    }
  }
  return result;
}
LinkOccurrences
resolvePackageLink(const LinkKey &key, const LinkPackage &pkg,
                   std::span<const PackageDocumentView> documents,
                   std::span<const PackageCellView> cells) {
  return PackageOccurrenceIndex(documents, cells).resolve(key, pkg);
}
bool packageReferences(const LinkPackage &package, const Publication &pub) {
  for (const auto &link : package.links)
    for (const auto *side : {&link.left, &link.right})
      for (const auto &span : *side)
        for (const auto &piece : pub.pieces)
          if (span.scroll == piece.scroll && span.start < piece.end() &&
              piece.start < span.end())
            return true;
  return false;
}
} // namespace xanadu
