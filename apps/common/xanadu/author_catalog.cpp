#include "author_catalog.hpp"

#include "bencode.hpp"
#include <algorithm>
#include <libtorrent/hasher.hpp>
#include <limits>
#include <lmdb.h>
#include <mutex>
#include <set>

namespace xanadu {
namespace {
using V = bencode::Value;
bencode::Dict body(const SignedAuthorCatalog &catalog) {
  bencode::List entries;
  for (const auto &entry : catalog.entries) {
    bencode::List scrolls;
    for (const auto &key : entry.scrollKeys) scrolls.push_back(V::string(key));
    bencode::List topics;
    for (const auto &topic : entry.topics) topics.push_back(V::string(topic));
    entries.push_back(
        V::dict({{"kind", V::integer(static_cast<std::int64_t>(entry.kind))},
                 {"scrolls", V::list(std::move(scrolls))},
                 {"hash", V::string(entry.hash.hex())},
                 {"salt", V::string(entry.salt)},
                 {"sequence", V::integer(entry.sequence)},
                 {"title", V::string(entry.title)},
                 {"topics", V::list(std::move(topics))},
                 {"version", V::string(entry.version.str())}}));
  }
  return {{"entries", V::list(std::move(entries))},
          {"format", V::integer(2)},
          {"publisher", V::string(catalog.publisher.hex())},
          {"sequence", V::integer(catalog.sequence)}};
}
std::string signingBytes(const SignedAuthorCatalog &catalog) {
  return "xudu-author-catalog:2:" + V::dict(body(catalog)).encode();
}
void validate(const SignedAuthorCatalog &catalog) {
  if (catalog.publisher.isZero() || catalog.sequence < 1 ||
      catalog.entries.size() > maximumAuthorCatalogEntries)
    throw AuthorCatalogUnreadable(
        "author catalog format 2: invalid identity, sequence or count");
  std::string previous;
  for (const auto &entry : catalog.entries) {
    if (entry.hash.isZero() || entry.sequence < 0 || entry.salt.empty() ||
        entry.salt.size() > 64 || entry.salt == "catalog" ||
        (!previous.empty() && entry.salt <= previous) || entry.title.empty() ||
        entry.title.size() > 1024 || entry.topics.size() > 32)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: invalid or duplicate entry");
    if (entry.kind != CatalogEntryKind::Document &&
        entry.kind != CatalogEntryKind::LinkPackage)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: unknown entry kind");
    if ((entry.kind == CatalogEntryKind::Document &&
         !entry.scrollKeys.empty()) ||
        (entry.kind == CatalogEntryKind::LinkPackage &&
         entry.scrollKeys.empty()) ||
        entry.scrollKeys.size() > 256 ||
        !std::ranges::is_sorted(entry.scrollKeys) ||
        std::adjacent_find(entry.scrollKeys.begin(), entry.scrollKeys.end()) !=
            entry.scrollKeys.end())
      throw AuthorCatalogUnreadable(
          "author catalog format 2: invalid referenced scroll keys");
    for (const auto &key : entry.scrollKeys)
      if (key.empty() || key.size() > 256 ||
          key.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw AuthorCatalogUnreadable(
            "author catalog format 2: invalid scroll key");
    std::set<std::string> topics;
    for (const auto &topic : entry.topics)
      if (topic != canonicalPublicationTopic(topic) ||
          !topics.insert(topic).second)
        throw AuthorCatalogUnreadable("author catalog format 2: invalid topic");
    previous = entry.salt;
  }
}
void checkDb(int rc) {
  if (rc != MDB_SUCCESS)
    throw std::runtime_error("author catalog: " +
                             std::string(mdb_strerror(rc)));
}
std::mutex &catalogWriters() {
  static std::mutex guard;
  return guard;
}
struct CatalogDb {
  std::unique_lock<std::mutex> lock{catalogWriters()};
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env{nullptr,
                                                         mdb_env_close};
  std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn{nullptr,
                                                         mdb_txn_abort};
  MDB_dbi db{};
  explicit CatalogDb(const std::filesystem::path &directory) {
    if (directory.empty())
      throw std::invalid_argument("Author catalog directory is required");
    std::filesystem::create_directories(directory);
    MDB_env *raw{};
    checkDb(mdb_env_create(&raw));
    env.reset(raw);
    checkDb(mdb_env_set_mapsize(raw, 64ULL * 1024 * 1024));
    checkDb(mdb_env_open(raw, directory.string().c_str(), 0, 0600));
    MDB_txn *transaction{};
    checkDb(mdb_txn_begin(raw, nullptr, 0, &transaction));
    txn.reset(transaction);
    checkDb(mdb_dbi_open(transaction, nullptr, 0, &db));
  }
  void commit() { checkDb(mdb_txn_commit(txn.release())); }
};
const V &field(const V &value, std::string_view name) {
  const auto &fields = value.asDict();
  const auto found   = fields.find(std::string(name));
  if (found == fields.end())
    throw AuthorCatalogUnreadable("author catalog format 2: missing field " +
                                  std::string(name));
  return found->second;
}
} // namespace

std::string canonicalPublicationTopic(std::string_view topic) {
  const auto topics = publicationTopics(topic);
  if (topics.size() != 1 || topics.front().size() > 128 ||
      std::ranges::any_of(topics.front(), [](unsigned char byte) {
        return byte < 32 || byte == 127;
      }))
    throw std::invalid_argument("Enter one topic, up to 128 bytes");
  return topics.front();
}
InfoHash publicationTopicTarget(std::string_view topic) {
  const auto name = "xudu:topic:" + canonicalPublicationTopic(topic);
  libtorrent::hasher hash(name.data(), static_cast<int>(name.size()));
  const auto digest = hash.final();
  InfoHash result;
  std::copy_n(reinterpret_cast<const std::uint8_t *>(digest.data()),
              result.bytes.size(), result.bytes.begin());
  return result;
}
std::string encodeAuthorCatalog(const SignedAuthorCatalog &catalog) {
  validate(catalog);
  auto fields = body(catalog);
  fields.emplace("signature",
                 V::string(std::string(reinterpret_cast<const char *>(
                                           catalog.signature.bytes.data()),
                                       catalog.signature.bytes.size())));
  auto encoded = V::dict(std::move(fields)).encode();
  if (encoded.size() > maximumAuthorCatalogBytes)
    throw AuthorCatalogUnreadable(
        "author catalog format 2: byte limit exceeded");
  return encoded;
}
SignedAuthorCatalog signAuthorCatalog(SignedAuthorCatalog catalog,
                                      const MutableKeys &keys) {
  catalog.publisher = keys.publicKey;
  validate(catalog);
  catalog.signature = signMutableItem(signingBytes(catalog), keys);
  (void)encodeAuthorCatalog(catalog);
  return catalog;
}
SignedAuthorCatalog decodeAuthorCatalog(std::string_view bytes) {
  if (bytes.size() > maximumAuthorCatalogBytes)
    throw AuthorCatalogUnreadable(
        "author catalog format 2: byte limit exceeded");
  try {
    const auto root   = bencode::decode(bytes);
    const auto format = field(root, "format").asInteger();
    if (format != 2)
      throw AuthorCatalogUnreadable("author catalog version 2 expected, got " +
                                    std::to_string(format));
    if (root.asDict().size() != 5)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: unexpected fields");
    SignedAuthorCatalog catalog;
    const auto key = PublicKey::parseHex(field(root, "publisher").asString());
    if (!key)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: invalid publisher");
    catalog.publisher     = *key;
    catalog.sequence      = field(root, "sequence").asInteger();
    const auto &signature = field(root, "signature").asString();
    if (signature.size() != catalog.signature.bytes.size())
      throw AuthorCatalogUnreadable(
          "author catalog format 2: invalid signature size");
    std::copy(signature.begin(), signature.end(),
              catalog.signature.bytes.begin());
    const auto &entries = field(root, "entries").asList();
    if (entries.size() > maximumAuthorCatalogEntries)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: entry limit exceeded");
    for (const auto &entry : entries) {
      if (entry.asDict().size() != 8)
        throw AuthorCatalogUnreadable(
            "author catalog format 2: unexpected entry fields");
      AuthorCatalogEntry item;
      const auto hash = InfoHash::parseHex(field(entry, "hash").asString());
      if (!hash)
        throw AuthorCatalogUnreadable("author catalog format 2: invalid hash");
      item.hash     = *hash;
      item.salt     = field(entry, "salt").asString();
      item.title    = field(entry, "title").asString();
      item.version  = MicroversionId::parse(field(entry, "version").asString());
      item.sequence = field(entry, "sequence").asInteger();
      const auto kind = field(entry, "kind").asInteger();
      if (kind < 0 || kind > 1)
        throw AuthorCatalogUnreadable(
            "author catalog format 2: unknown entry kind");
      item.kind = static_cast<CatalogEntryKind>(kind);
      for (const auto &key : field(entry, "scrolls").asList())
        item.scrollKeys.push_back(key.asString());
      for (const auto &topic : field(entry, "topics").asList())
        item.topics.push_back(topic.asString());
      catalog.entries.push_back(std::move(item));
    }
    validate(catalog);
    if (encodeAuthorCatalog(catalog) != bytes ||
        !verifyMutableItem(signingBytes(catalog), catalog.signature,
                           catalog.publisher))
      throw AuthorCatalogUnreadable(
          "author catalog format 2: signature or canonical encoding failed");
    return catalog;
  } catch (const AuthorCatalogUnreadable &) {
    throw;
  } catch (const std::exception &error) {
    throw AuthorCatalogUnreadable("author catalog format 2: " +
                                  std::string(error.what()));
  }
}

namespace {
SignedAuthorCatalog updateEntry(const std::filesystem::path &directory,
                                const AuthorCatalogEntry &incoming,
                                const MutableKeys &keys) {
  CatalogDb storage(directory);
  auto name = keys.publicKey.hex();
  MDB_val key{name.size(), name.data()}, value{};
  SignedAuthorCatalog catalog;
  const auto rc = mdb_get(storage.txn.get(), storage.db, &key, &value);
  if (rc == MDB_SUCCESS) {
    catalog = decodeAuthorCatalog(
        {static_cast<const char *>(value.mv_data), value.mv_size});
    if (catalog.publisher != keys.publicKey)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: stored publisher mismatch");
  } else if (rc != MDB_NOTFOUND)
    checkDb(rc);
  auto found = std::ranges::find(catalog.entries, incoming.salt,
                                 &AuthorCatalogEntry::salt);
  if (found != catalog.entries.end()) {
    if (incoming == *found || incoming.sequence < found->sequence)
      return catalog;
    if (incoming.sequence == found->sequence)
      throw std::invalid_argument(
          "Different publication owns this author catalog sequence");
    *found = incoming;
  } else
    catalog.entries.push_back(incoming);
  std::ranges::sort(catalog.entries, {}, &AuthorCatalogEntry::salt);
  if (catalog.sequence == std::numeric_limits<std::int64_t>::max())
    throw std::overflow_error("Author catalog sequence exhausted");
  ++catalog.sequence;
  catalog      = signAuthorCatalog(std::move(catalog), keys);
  auto encoded = encodeAuthorCatalog(catalog);
  value        = MDB_val{encoded.size(), encoded.data()};
  checkDb(mdb_put(storage.txn.get(), storage.db, &key, &value, 0));
  storage.commit();
  return catalog;
}
} // namespace
SignedAuthorCatalog updateAuthorCatalog(const std::filesystem::path &directory,
                                        const Publication &pub,
                                        const InfoHash &hash,
                                        const MutableKeys &keys) {
  if (directory.empty() || keys.publicKey != pub.publisher ||
      !verifyPublication(pub))
    throw std::invalid_argument(
        "Author catalog requires the signed author's publication");
  return updateEntry(
      directory,
      {hash, pub.salt, pub.title, pub.topics, pub.version, pub.sequence}, keys);
}
SignedAuthorCatalog updateAuthorCatalog(const std::filesystem::path &directory,
                                        const LinkPackage &pkg,
                                        const InfoHash &hash,
                                        const MutableKeys &keys) {
  reviewLinkPackage(pkg);
  if (keys.publicKey != pkg.curator)
    throw std::invalid_argument(
        "Catalog signing key differs from package curator");
  AuthorCatalogEntry entry;
  entry.hash       = hash;
  entry.salt       = pkg.salt;
  entry.title      = pkg.title;
  entry.sequence   = pkg.sequence;
  entry.kind       = CatalogEntryKind::LinkPackage;
  entry.scrollKeys = linkPackageScrollKeys(pkg);
  return updateEntry(directory, entry, keys);
}
PublicationEntry catalogPublicationEntry(const SignedAuthorCatalog &catalog,
                                         const AuthorCatalogEntry &entry) {
  MutableLink link;
  link.key                = catalog.publisher;
  link.salt               = entry.salt;
  link.currentWhenWritten = entry.hash;
  PublicationEntry result;
  result.infoHash          = entry.hash.hex();
  result.bep46Uri          = link.uri();
  result.title             = entry.title;
  result.authorName        = catalog.publisher.hex().substr(0, 12);
  result.authorFingerprint = catalog.publisher.hex();
  result.topics            = entry.topics;
  result.sequence          = static_cast<std::uint64_t>(entry.sequence);
  result.microversions     = 0;
  result.abstractText =
      "Signed author metadata; enrollment not verified. Selected version " +
      entry.version.str();
  return result;
}

bool retainAuthorCatalog(const std::filesystem::path &directory,
                         const SignedAuthorCatalog &catalog) {
  auto encoded = encodeAuthorCatalog(catalog);
  (void)decodeAuthorCatalog(encoded);
  CatalogDb storage(directory);
  auto name = catalog.publisher.hex();
  MDB_val key{name.size(), name.data()}, value{};
  const auto rc = mdb_get(storage.txn.get(), storage.db, &key, &value);
  if (rc == MDB_SUCCESS) {
    const auto previous = decodeAuthorCatalog(
        {static_cast<const char *>(value.mv_data), value.mv_size});
    if (previous.publisher != catalog.publisher)
      throw AuthorCatalogUnreadable(
          "author catalog format 2: retained publisher mismatch");
    if (previous.sequence > catalog.sequence) return false;
    if (previous.sequence == catalog.sequence) {
      if (encodeAuthorCatalog(previous) != encoded)
        throw AuthorCatalogUnreadable("Author catalog has conflicting signed "
                                      "content at the same sequence");
      return true;
    }
    // A catalog update cannot roll back one of the author's document names.
    for (const auto &entry : previous.entries) {
      const auto current = std::ranges::find(catalog.entries, entry.salt,
                                             &AuthorCatalogEntry::salt);
      if (current != catalog.entries.end() &&
          (current->sequence < entry.sequence ||
           (current->sequence == entry.sequence && *current != entry)))
        throw AuthorCatalogUnreadable("Author catalog rolls back or conflicts "
                                      "with an observed publication");
    }
  } else if (rc != MDB_NOTFOUND)
    checkDb(rc);
  value = MDB_val{encoded.size(), encoded.data()};
  checkDb(mdb_put(storage.txn.get(), storage.db, &key, &value, 0));
  storage.commit();
  return true;
}
std::vector<SignedAuthorCatalog>
retainedAuthorCatalogs(const std::filesystem::path &directory) {
  CatalogDb storage(directory);
  MDB_cursor *raw{};
  checkDb(mdb_cursor_open(storage.txn.get(), storage.db, &raw));
  const std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> cursor(
      raw, mdb_cursor_close);
  MDB_val key{}, value{};
  std::vector<SignedAuthorCatalog> result;
  int rc;
  while ((rc = mdb_cursor_get(raw, &key, &value, MDB_NEXT)) == MDB_SUCCESS) {
    auto catalog = decodeAuthorCatalog(
        {static_cast<const char *>(value.mv_data), value.mv_size});
    if (std::string_view(static_cast<const char *>(key.mv_data), key.mv_size) !=
        catalog.publisher.hex())
      throw AuthorCatalogUnreadable(
          "author catalog format 2: retained key mismatch");
    result.push_back(std::move(catalog));
  }
  if (rc != MDB_NOTFOUND) checkDb(rc);
  return result;
}
} // namespace xanadu
