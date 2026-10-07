#ifndef XANADU_READER_LINK_PACKAGES_HPP
#define XANADU_READER_LINK_PACKAGES_HPP

#include "enfilade/spanfilade.hpp"
#include "link_occurrences.hpp"
#include "link_package.hpp"
#include <filesystem>

namespace xanadu {
class ReaderLinkPackagesUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
/// Preferences name immutable carriers, never mutable curator names. Retaining
/// a newer package cannot turn it on or change an earlier link's identity.
class ReaderLinkPackages {
public:
  explicit ReaderLinkPackages(std::filesystem::path file);
  [[nodiscard]] std::expected<ReaderLinkPackages *, std::string>
  retain(const LinkPackage &package, const InfoHash &hash);
  [[nodiscard]] std::expected<ReaderLinkPackages *, std::string>
  setEnabled(const InfoHash &hash, bool enabled);
  [[nodiscard]] bool containsAuthority(const DocumentId &authority) const;
  [[nodiscard]] bool enabled(const InfoHash &hash) const;
  [[nodiscard]] std::vector<LinkKey> links() const;
  [[nodiscard]] const GlobalLink *find(const LinkKey &key) const;
  [[nodiscard]] const LinkPackage *package(const LinkKey &key) const;
  [[nodiscard]] static LinkKey keyOf(const InfoHash &hash, std::size_t ordinal);
  [[nodiscard]] const std::map<InfoHash, bool> &preferences() const {
    return choices;
  }

private:
  std::filesystem::path file;
  std::map<InfoHash, bool> choices;
  std::map<InfoHash, LinkPackage> retained;
  std::map<std::string, InfoHash> authorities;
};
struct PackageDocumentView {
  const Store &store;
  MicroversionId version;
  const Version &text;
};
struct PackageCellView {
  const Store &store;
  MicroversionId version;
  const zigzag::Manifold &manifold;
  std::span<const zigzag::CellRef> cells;
};
/// Canonical global-scroll interval indexes, built once for a view generation.
/// Queries touch overlapping pieces rather than rescanning every open cell.
class PackageOccurrenceIndex {
public:
  PackageOccurrenceIndex(std::span<const PackageDocumentView> documents,
                         std::span<const PackageCellView> cells);
  [[nodiscard]] std::vector<Occurrence>
  occurrences(const GlobalSpan &member) const;
  [[nodiscard]] LinkOccurrences resolve(const LinkKey &key,
                                        const LinkPackage &package) const;

private:
  std::vector<PackageDocumentView> documents;
  std::vector<PackageCellView> cells;
  std::map<std::string, enfilade::ScrollSpanfilade> scrolls;
};
/// Read-only slot-zero/imported binding lookup; no deployment slots are added.
[[nodiscard]] std::string readerScrollKey(const Store &store, ScrollId slot);
[[nodiscard]] std::vector<PieceMatch>
packageOccurrences(const Store &store, std::span<const PrimediaSpan> pieces,
                   const GlobalSpan &member);
[[nodiscard]] LinkOccurrences
resolvePackageLink(const LinkKey &key, const LinkPackage &package,
                   std::span<const PackageDocumentView> documents,
                   std::span<const PackageCellView> cells);
/// Rendezvous is only a candidate filter. Relevance is an actual byte overlap;
/// retain the whole link, including members outside the chosen publication.
[[nodiscard]] bool packageReferences(const LinkPackage &package,
                                     const Publication &publication);
} // namespace xanadu
#endif
