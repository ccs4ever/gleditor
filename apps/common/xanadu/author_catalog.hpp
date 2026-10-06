#ifndef XUDU_AUTHOR_CATALOG_HPP
#define XUDU_AUTHOR_CATALOG_HPP

#include "publication.hpp"
#include "publication_ledger.hpp"
#include <filesystem>
#include <stdexcept>

namespace xanadu {
inline constexpr std::size_t maximumAuthorCatalogBytes   = 512 * 1024;
inline constexpr std::size_t maximumAuthorCatalogEntries = 1024;

struct AuthorCatalogEntry {
  InfoHash hash;
  std::string salt;
  std::string title;
  std::vector<std::string> topics;
  MicroversionId version;
  std::int64_t sequence{};
  bool operator==(const AuthorCatalogEntry &) const = default;
};
struct SignedAuthorCatalog {
  PublicKey publisher;
  std::int64_t sequence{};
  std::vector<AuthorCatalogEntry> entries;
  Signature signature;
};
class AuthorCatalogUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string
encodeAuthorCatalog(const SignedAuthorCatalog &catalog);
/// Refuses noncanonical bytes, unsupported formats and invalid signatures.
[[nodiscard]] SignedAuthorCatalog decodeAuthorCatalog(std::string_view bytes);
[[nodiscard]] SignedAuthorCatalog signAuthorCatalog(SignedAuthorCatalog catalog,
                                                    const MutableKeys &keys);
/// A transaction preserves every name and reserves the next catalog sequence.
/// Repeated publication of the same signed document is idempotent.
[[nodiscard]] SignedAuthorCatalog
updateAuthorCatalog(const std::filesystem::path &directory,
                    const Publication &publication, const InfoHash &hash,
                    const MutableKeys &keys);
[[nodiscard]] PublicationEntry
catalogPublicationEntry(const SignedAuthorCatalog &catalog,
                        const AuthorCatalogEntry &entry);
/// Cache only signed snapshots, retaining the highest observed sequence.
/// Returns false for an older snapshot; equal-sequence conflicts are refused.
bool retainAuthorCatalog(const std::filesystem::path &directory,
                         const SignedAuthorCatalog &catalog);
[[nodiscard]] std::vector<SignedAuthorCatalog>
retainedAuthorCatalogs(const std::filesystem::path &directory);
[[nodiscard]] std::string canonicalPublicationTopic(std::string_view topic);
[[nodiscard]] InfoHash publicationTopicTarget(std::string_view topic);
} // namespace xanadu
#endif
