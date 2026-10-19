#include "publication_inbox.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <lmdb.h>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "bencode.hpp"
#include "store.hpp"
#include "user_permascroll.hpp"

namespace xanadu {
namespace {
using namespace std::chrono_literals;
using V                                   = bencode::Value;
constexpr std::string_view journalKey     = "requests";
constexpr std::size_t maximumJournalBytes = 4 * 1024 * 1024;
// The title is stored both in the request pin and the visible status record.
// Keep their combined worst case below the bounded journal envelope.
constexpr std::size_t maximumTitleBytes = maximumJournalBytes / 2 - 4096;
std::string boundedDiagnostic(std::string message) {
  constexpr std::size_t maximumBytes = 4096;
  if (message.size() <= maximumBytes) return message;
  constexpr std::string_view suffix = " [truncated at 4096 bytes]";
  message.resize(maximumBytes - suffix.size());
  message.append(suffix);
  return message;
}
void dbCheck(int rc) {
  if (rc != MDB_SUCCESS)
    throw std::runtime_error("Publication inbox: " +
                             std::string(mdb_strerror(rc)));
}
const V &field(const V &value, std::string_view name) {
  const auto &dict = value.asDict();
  const auto found = dict.find(std::string(name));
  if (found == dict.end())
    throw PublicationInboxUnreadable("Publication inbox format 1: missing " +
                                     std::string(name));
  return found->second;
}
bool flag(const V &value) {
  if (!value.isInteger() || (value.asInteger() != 0 && value.asInteger() != 1))
    throw PublicationInboxUnreadable(
        "Publication inbox format 1: invalid boolean");
  return value.asInteger() != 0;
}
std::string hashText(const InfoHash &hash) { return hash.hex(); }
InfoHash parseHash(const V &value) {
  if (!value.isString())
    throw PublicationInboxUnreadable(
        "Publication inbox format 1: invalid hash field");
  const auto parsed = InfoHash::parseHex(value.asString());
  if (!parsed)
    throw PublicationInboxUnreadable(
        "Publication inbox format 1: invalid hash");
  return *parsed;
}
void checkCancelled(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error("Download cancelled");
}

std::runtime_error filesystemFailure(std::string_view operation,
                                     const std::filesystem::path &path) {
  return std::runtime_error("Cannot " + std::string(operation) + " " +
                            path.string() + ": " + std::strerror(errno));
}

void syncRegularFile(const std::filesystem::path &path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status))
    throw std::runtime_error("Unsafe non-regular publication file: " +
                             path.string());
#ifdef _WIN32
  const auto fd = _wopen(path.c_str(), _O_RDWR | _O_BINARY | _O_NOINHERIT);
  if (fd < 0) throw filesystemFailure("open for sync", path);
  const auto result = _commit(fd);
  const auto saved  = errno;
  _close(fd);
  errno = saved;
  if (result != 0) throw filesystemFailure("sync", path);
#else
  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const auto fd = ::open(path.c_str(), flags);
  if (fd < 0) throw filesystemFailure("open for sync", path);
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    const auto saved = errno;
    ::close(fd);
    errno = saved;
    throw std::runtime_error("Unsafe non-regular publication file: " +
                             path.string());
  }
  int result{};
  do {
    result = ::fsync(fd);
  } while (result != 0 && errno == EINTR);
  const auto saved = errno;
  ::close(fd);
  errno = saved;
  if (result != 0) throw filesystemFailure("sync", path);
#endif
}

void syncDirectory(const std::filesystem::path &path) {
#ifdef _WIN32
  // Windows does not expose a portable directory-handle equivalent to
  // fsync(2). File contents are committed separately; directory rename uses
  // MOVEFILE_WRITE_THROUGH below, without claiming POSIX power-loss semantics.
  (void)path;
#else
  int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const auto fd = ::open(path.c_str(), flags);
  if (fd < 0) throw filesystemFailure("open directory for sync", path);
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
    const auto saved = errno;
    ::close(fd);
    errno = saved;
    throw std::runtime_error("Unsafe publication directory: " + path.string());
  }
  int result{};
  do {
    result = ::fsync(fd);
  } while (result != 0 && errno == EINTR);
  const auto saved = errno;
  ::close(fd);
  errno = saved;
  if (result != 0) throw filesystemFailure("sync directory", path);
#endif
}

void syncDirectoryChain(std::filesystem::path path) {
  while (!path.empty()) {
    syncDirectory(path);
    if (path == path.root_path()) break;
    const auto parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
}

void renameInstalledRoot(const std::filesystem::path &from,
                         const std::filesystem::path &to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot install publication snapshot: Windows " +
                             std::to_string(GetLastError()));
#else
  std::filesystem::rename(from, to);
#endif
}

void syncStagedTree(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> directories{root};
  for (std::filesystem::recursive_directory_iterator it(root), end; it != end;
       ++it) {
    const auto status = it->symlink_status();
    if (std::filesystem::is_symlink(status))
      throw std::runtime_error("Symlink in staged publication: " +
                               it->path().string());
    if (std::filesystem::is_regular_file(status))
      syncRegularFile(it->path());
    else if (std::filesystem::is_directory(status))
      directories.push_back(it->path());
    else
      throw std::runtime_error("Special file in staged publication: " +
                               it->path().string());
  }
  for (auto it = directories.rbegin(); it != directories.rend(); ++it)
    syncDirectory(*it);
}

class DownloadTransport final : public PublicationDownloadTransport {
public:
  DownloadTransport(SwarmContentSource::Options options,
                    std::vector<std::pair<std::string, std::uint16_t>> nodes,
                    std::vector<std::pair<std::string, std::uint16_t>> peers,
                    std::chrono::milliseconds timeout)
      : source_(options), nodes_(std::move(nodes)), peers_(std::move(peers)),
        timeout_(timeout) {}

  MutablePointer resolve(const MutableLink &link,
                         std::stop_token stop) override {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    do {
      checkCancelled(stop);
      for (const auto &[host, port] : nodes_) source_.addDhtNode(host, port);
      if (auto pointer = source_.resolveMutable(link, 250ms)) return *pointer;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(
        "No signed DHT pointer received; retry when the author is reachable");
  }

  void fetch(const InfoHash &hash, const std::filesystem::path &directory,
             std::uint64_t maximumBytes, std::stop_token stop) override {
    checkCancelled(stop);
    std::filesystem::create_directories(directory);
    (void)source_.addMagnet("magnet:?xt=urn:btih:" + hash.hex(),
                            directory.string(), true);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    do {
      checkCancelled(stop);
      // Pinned snapshots and packages reach fetch without resolving a name.
      // Introduce bootstrap nodes after asynchronous DHT startup here too.
      for (const auto &[host, port] : nodes_) source_.addDhtNode(host, port);
      for (const auto &[host, port] : peers_)
        source_.connectPeer(hash, host, port);
      if (source_.waitForMetadata(hash, 250ms)) break;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Publication torrent metadata unavailable: " +
                                 hash.hex());
    } while (true);
    const auto meta    = source_.metainfo(hash);
    const auto encoded = source_.torrentMetadata(hash);
    if (!meta || !encoded || meta->totalLength() > maximumBytes)
      throw std::runtime_error(
          "Publication download exceeds configured byte limit");
    // Publication carriers use small Merkle pieces. Refuse a hostile piece
    // layout before any read can allocate the entire admitted byte budget.
    constexpr std::uint64_t maximumPieceBytes = 16 * 1024 * 1024;
    if (meta->pieceLength() > maximumPieceBytes)
      throw std::runtime_error(
          "Publication piece exceeds 16 MiB transfer limit");
    const auto safe = [](const std::filesystem::path &path) {
      if (path.empty() || path.has_root_path() ||
          path.string().find_first_of("\\") != std::string::npos ||
          path.string().find('\0') != std::string::npos)
        return false;
      return std::ranges::none_of(
          path, [](const auto &part) { return part == "." || part == ".."; });
    };
    if (!safe(meta->name()) ||
        std::filesystem::path(meta->name()).has_parent_path() ||
        meta->pieceCount() !=
            meta->totalLength() / meta->pieceLength() +
                (meta->totalLength() % meta->pieceLength() != 0))
      throw std::runtime_error(
          "Unsafe or incomplete publication torrent metadata");
    std::set<std::string> paths;
    std::uint64_t totalBytes{};
    for (const auto &file : meta->files()) {
      if (!safe(file.path) || !paths.insert(file.path).second)
        throw std::runtime_error("Unsafe publication torrent file path");
      if (file.length > maximumBytes - totalBytes)
        throw std::runtime_error(
            "Publication file sizes exceed download byte limit");
      totalBytes += file.length;
    }
    source_.startDownload(hash);
    for (std::size_t piece = 0; piece < meta->pieceCount(); ++piece) {
      const auto pieceDeadline = std::chrono::steady_clock::now() + timeout_;
      do {
        checkCancelled(stop);
        const auto bytes = source_.readStream(hash, piece * meta->pieceLength(),
                                              meta->lengthOfPiece(piece));
        if (meta->verifyPiece(piece, bytes)) {
          source_.discardCachedPieces(hash);
          break;
        }
        if (std::chrono::steady_clock::now() >= pieceDeadline)
          throw std::runtime_error("Publication piece unavailable: " +
                                   hash.hex());
      } while (true);
    }
    const auto flushDeadline = std::chrono::steady_clock::now() + timeout_;
    while (!source_.flushDownload(hash, 250ms)) {
      checkCancelled(stop);
      if (std::chrono::steady_clock::now() >= flushDeadline)
        throw std::runtime_error("Publication files did not finish flushing");
    }
    checkCancelled(stop);
    std::ofstream retained(directory / "metainfo.torrent", std::ios::binary);
    retained << *encoded;
    retained.close();
    if (!retained)
      throw std::runtime_error("Cannot retain publication metainfo");
  }

private:
  SwarmContentSource source_;
  std::vector<std::pair<std::string, std::uint16_t>> nodes_, peers_;
  std::chrono::milliseconds timeout_;
};
} // namespace

std::string_view publicationDownloadPhaseName(PublicationDownloadPhase phase) {
  switch (phase) {
  case PublicationDownloadPhase::Queued:
    return "Queued";
  case PublicationDownloadPhase::Resolving:
    return "Resolving signed DHT name";
  case PublicationDownloadPhase::Downloading:
    return "Downloading dependencies";
  case PublicationDownloadPhase::Verifying:
    return "Verifying complete store";
  case PublicationDownloadPhase::Ready:
    return "Ready to open";
  case PublicationDownloadPhase::Failed:
    return "Download failed";
  case PublicationDownloadPhase::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

struct PublicationInbox::Impl {
  struct Job {
    MutableLink link;
    std::optional<MutablePointer> minimum;
    std::optional<PublicationPin> pin;
    // Caller submission mode; mutable requests may freeze a pin after signed
    // manifest validation and intentionally retry against that frozen pin.
    bool exactPin{};
    std::uint64_t attempt{};
    bool cancellationRequested{};
    bool verifyRetainedOnly{};
    PublicationDownloadStatus status;
    std::stop_source cancel;
  };
  Options options;
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env{nullptr,
                                                         mdb_env_close};
  MDB_dbi db{};
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::map<std::string, Job> jobs;
  std::jthread worker;

  static V encodeJob(const Job &job) {
    const auto &status = job.status;
    const auto &pin    = job.pin;
    if (status.id.size() != 64 || !PublicKey::parseHex(status.id) ||
        job.attempt > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()) ||
        status.error.size() > 4096 || status.title.size() > maximumTitleBytes ||
        (pin && pin->title.size() > maximumTitleBytes))
      throw std::runtime_error(
          "Publication inbox request metadata exceeds its format limit");
    const auto minimumHash = job.minimum ? job.minimum->hash : InfoHash{};
    return V::dict({
        {"attempt", V::integer(static_cast<std::int64_t>(job.attempt))},
        {"cancel_requested", V::integer(job.cancellationRequested)},
        {"exact", V::integer(job.exactPin)},
        {"error", V::string(status.error)},
        {"id", V::string(status.id)},
        {"manifest", V::string(hashText(status.manifestHash))},
        {"minimum_hash", V::string(hashText(minimumHash))},
        {"minimum_sequence",
         V::integer(job.minimum ? job.minimum->sequence : -1)},
        {"phase", V::integer(static_cast<int>(status.phase))},
        {"pin_hash", V::string(pin ? hashText(pin->hash) : std::string{})},
        {"pin_publisher",
         V::string(pin ? pin->publisher.hex() : PublicKey{}.hex())},
        {"pin_salt", V::string(pin ? pin->salt : std::string{})},
        {"pin_sequence", V::integer(pin ? pin->sequence : -1)},
        {"pin_title", V::string(pin ? pin->title : std::string{})},
        {"pin_version", V::string(pin ? pin->version.str() : std::string{})},
        {"pinned", V::integer(pin.has_value())},
        {"sequence", V::integer(status.sequence)},
        {"title", V::string(status.title)},
        {"uri", V::string(status.uri)},
        {"verification", V::integer(static_cast<int>(status.verification))},
        {"version", V::string(status.version.str())},
    });
  }

  void validateRetainedPaths(const std::filesystem::path &root) const {
    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory ||
        rootStatus.type() == std::filesystem::file_type::not_found)
      return;
    if (error || std::filesystem::is_symlink(rootStatus) ||
        !std::filesystem::is_directory(rootStatus))
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: unsafe retained snapshot root");
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end;
         ++it) {
      const auto status = it->symlink_status();
      if (std::filesystem::is_symlink(status))
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: symlink in retained snapshot");
      if (!std::filesystem::is_directory(status) &&
          !std::filesystem::is_regular_file(status))
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: special file in retained snapshot");
    }
  }

  Job decodeJob(const V &record) const {
    if (!record.isDict() || record.asDict().size() != 21)
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: invalid request fields");
    Job job;
    auto &status        = job.status;
    status.id           = field(record, "id").asString();
    status.uri          = field(record, "uri").asString();
    job.link            = MutableLink::parse(status.uri);
    const auto parsedId = PublicKey::parseHex(status.id);
    if (!parsedId || parsedId->hex() != status.id || job.link.key.isZero() ||
        job.link.salt.empty() || job.link.salt.size() > 64)
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: invalid request identity");
    status.title    = field(record, "title").asString();
    status.sequence = field(record, "sequence").asInteger();
    status.version = MicroversionId::parse(field(record, "version").asString());
    status.manifestHash     = parseHash(field(record, "manifest"));
    status.error            = field(record, "error").asString();
    const auto phase        = field(record, "phase").asInteger();
    const auto verification = field(record, "verification").asInteger();
    const auto attempt      = field(record, "attempt").asInteger();
    if (status.error.size() > 4096 || status.title.size() > maximumTitleBytes ||
        status.sequence < -1 || phase < 0 ||
        phase > static_cast<int>(PublicationDownloadPhase::Cancelled) ||
        verification < 0 ||
        verification >
            static_cast<int>(PublicationDownloadVerification::Complete) ||
        attempt < 0)
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: invalid phase or attempt");
    status.phase = static_cast<PublicationDownloadPhase>(phase);
    status.verification =
        static_cast<PublicationDownloadVerification>(verification);
    job.attempt                = static_cast<std::uint64_t>(attempt);
    job.cancellationRequested  = flag(field(record, "cancel_requested"));
    job.exactPin               = flag(field(record, "exact"));
    const bool pinned          = flag(field(record, "pinned"));
    const auto minimumSequence = field(record, "minimum_sequence").asInteger();
    const auto minimumHash     = parseHash(field(record, "minimum_hash"));
    if (minimumSequence >= 0) {
      if (minimumHash.isZero())
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: invalid mutable lower bound");
      job.minimum = MutablePointer{minimumHash, minimumSequence};
    } else if (minimumSequence != -1 || !minimumHash.isZero()) {
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: invalid empty lower bound");
    }
    if (pinned) {
      const auto publisher =
          PublicKey::parseHex(field(record, "pin_publisher").asString());
      if (!publisher)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: invalid pinned publisher");
      PublicationPin pin{.publisher = *publisher,
                         .salt      = field(record, "pin_salt").asString(),
                         .hash      = parseHash(field(record, "pin_hash")),
                         .sequence  = field(record, "pin_sequence").asInteger(),
                         .version   = MicroversionId::parse(
                             field(record, "pin_version").asString()),
                         .title = field(record, "pin_title").asString()};
      if (pin.publisher != job.link.key || pin.salt != job.link.salt ||
          pin.hash.isZero() || pin.sequence < 0 || pin.salt.empty() ||
          pin.salt.size() > 64 || pin.title.size() > maximumTitleBytes)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: invalid publication pin");
      job.pin    = pin;
      status.pin = pin;
    } else if (job.exactPin || !field(record, "pin_hash").asString().empty() ||
               !field(record, "pin_salt").asString().empty() ||
               field(record, "pin_sequence").asInteger() != -1 ||
               !field(record, "pin_title").asString().empty() ||
               !field(record, "pin_version").asString().empty()) {
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: inconsistent empty publication pin");
    }
    if (job.exactPin && !pinned)
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: exact request lacks pin");
    if ((status.verification != PublicationDownloadVerification::Unchecked &&
         (!job.pin || status.manifestHash.isZero() ||
          status.manifestHash != job.pin->hash ||
          status.title != job.pin->title ||
          status.sequence != job.pin->sequence ||
          status.version != job.pin->version)) ||
        (!status.manifestHash.isZero() &&
         (!job.pin || status.manifestHash != job.pin->hash)) ||
        (!status.title.empty() && job.pin && status.title != job.pin->title) ||
        (status.sequence >= 0 && job.pin &&
         status.sequence != job.pin->sequence) ||
        (status.version != MicroversionId{} && job.pin &&
         status.version != job.pin->version) ||
        (!job.pin && (!status.title.empty() || status.sequence != -1 ||
                      status.version != MicroversionId{})) ||
        (job.minimum && job.pin &&
         (job.minimum->hash != job.pin->hash ||
          job.minimum->sequence != job.pin->sequence)) ||
        ((status.phase == PublicationDownloadPhase::Ready) !=
         (status.verification == PublicationDownloadVerification::Complete)))
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: inconsistent request pin status");
    job.verifyRetainedOnly = false;
    const auto root        = options.directory / status.id;
    validateRetainedPaths(root);
    const auto manifest    = root / "publication.xanadoc";
    job.verifyRetainedOnly = std::filesystem::exists(manifest);
    if (job.verifyRetainedOnly) {
      status.storePath = options.directory / status.id / "store";
      if (!job.pin)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: retained request lacks exact pin");
      if (!std::filesystem::exists(root / "store" / "store.tables"))
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: retained store tables are missing");
    }
    return job;
  }

  std::string encodeJournal() const {
    bencode::List records;
    for (const auto &[id, job] : jobs) {
      (void)id;
      records.push_back(encodeJob(job));
    }
    return "XPI1" +
           V::dict({{"requests", V::list(std::move(records))}}).encode();
  }

  void persist() {
    const auto encoded = encodeJournal();
    if (encoded.size() > maximumJournalBytes)
      throw std::runtime_error("Publication inbox request journal is full");
    MDB_txn *raw{};
    dbCheck(mdb_txn_begin(env.get(), nullptr, 0, &raw));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(raw, mdb_txn_abort);
    std::string key(journalKey);
    MDB_val dbKey{key.size(), key.data()};
    MDB_val value{encoded.size(), const_cast<char *>(encoded.data())};
    dbCheck(mdb_put(txn.get(), db, &dbKey, &value, 0));
    dbCheck(mdb_txn_commit(txn.release()));
  }

  explicit Impl(Options supplied) : options(std::move(supplied)) {
    if (options.directory.empty() || !options.maximumManifestBytes ||
        !options.maximumDependencyBytes || !options.maximumDependencies ||
        !options.maximumRequests)
      throw std::invalid_argument("Invalid publication inbox options");
    if (std::filesystem::is_symlink(options.directory))
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: unsafe cache root");
    std::filesystem::create_directories(options.directory);
    // Resolve configured ancestor aliases once. Directory syncing must target
    // the real directories (for example, macOS /var and /tmp aliases), while
    // the configured inbox leaf itself remains forbidden from being a link.
    options.directory = std::filesystem::canonical(options.directory);
    // The journal's containing directory can itself be newly created. Sync
    // its ancestor chain bottom-up so the first Ready record cannot outrun
    // the directory entries that make the inbox reachable.
    syncDirectoryChain(options.directory);
    if (std::filesystem::is_symlink(options.directory / "data.mdb") ||
        std::filesystem::is_symlink(options.directory / "lock.mdb"))
      throw PublicationInboxUnreadable(
          "Publication inbox format 1: unsafe LMDB file");
    MDB_env *raw{};
    dbCheck(mdb_env_create(&raw));
    env.reset(raw);
    dbCheck(mdb_env_set_mapsize(raw, 16ULL * 1024 * 1024));
    dbCheck(mdb_env_open(raw, options.directory.string().c_str(), 0, 0600));
    syncDirectoryChain(options.directory);
    MDB_txn *readRaw{};
    dbCheck(mdb_txn_begin(raw, nullptr, MDB_RDONLY, &readRaw));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> readTxn(readRaw,
                                                               mdb_txn_abort);
    dbCheck(mdb_dbi_open(readTxn.get(), nullptr, 0, &db));
    std::string key(journalKey);
    MDB_val dbKey{key.size(), key.data()}, value{};
    const auto found = mdb_get(readTxn.get(), db, &dbKey, &value);
    bool hadJournal  = found == MDB_SUCCESS;
    if (hadJournal) {
      const std::string_view bytes(static_cast<const char *>(value.mv_data),
                                   value.mv_size);
      if (bytes.size() < 4 || !bytes.starts_with("XPI"))
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: XPI signature missing");
      if (bytes[3] != '1')
        throw PublicationInboxUnreadable(
            "Publication inbox version 1 expected, got byte " +
            std::to_string(static_cast<unsigned char>(bytes[3])));
      if (bytes.size() > maximumJournalBytes)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: journal byte limit exceeded");
      try {
        const auto root = bencode::decode(bytes.substr(4));
        if (!root.isDict() || root.asDict().size() != 1)
          throw std::runtime_error("unexpected journal fields");
        const auto &records = field(root, "requests");
        if (!records.isList() ||
            records.asList().size() > options.maximumRequests)
          throw std::runtime_error("request count limit exceeded");
        for (const auto &record : records.asList()) {
          auto job = decodeJob(record);
          if (!jobs.emplace(job.status.id, std::move(job)).second)
            throw std::runtime_error("duplicate request ID");
        }
      } catch (const PublicationInboxUnreadable &) {
        throw;
      } catch (const std::exception &error) {
        throw PublicationInboxUnreadable("Publication inbox format 1: " +
                                         std::string(error.what()));
      }
    } else if (found != MDB_NOTFOUND) {
      dbCheck(found);
    }
    readTxn.reset();

    // Import completed snapshots created before the request journal existed.
    // Their identities come only from their signed manifest and carrier hash.
    for (const auto &entry :
         std::filesystem::directory_iterator(options.directory)) {
      if (entry.is_symlink())
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: symlink in cache root");
      const auto manifestPath = entry.path() / "publication.xanadoc";
      if (!entry.is_directory()) continue;
      const auto id            = entry.path().filename().string();
      const auto attemptMarker = id.find(".attempt-");
      if (attemptMarker != std::string::npos) {
        const auto requestId     = id.substr(0, attemptMarker);
        const auto attemptText   = id.substr(attemptMarker + 9);
        const auto parsedRequest = PublicKey::parseHex(requestId);
        std::uint64_t attemptNumber{};
        const auto parsedAttempt = std::from_chars(
            attemptText.data(), attemptText.data() + attemptText.size(),
            attemptNumber);
        if (parsedRequest && parsedRequest->hex() == requestId &&
            jobs.contains(requestId) && !attemptText.empty() &&
            parsedAttempt.ec == std::errc{} &&
            parsedAttempt.ptr == attemptText.data() + attemptText.size() &&
            std::to_string(attemptNumber) == attemptText &&
            attemptNumber <= jobs.at(requestId).attempt) {
          validateRetainedPaths(entry.path());
          continue;
        }
      }
      const auto parsedId = PublicKey::parseHex(id);
      if (!parsedId || parsedId->hex() != id)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: invalid retained snapshot directory");
      validateRetainedPaths(entry.path());
      if (!std::filesystem::exists(manifestPath)) continue;
      if (std::filesystem::file_size(manifestPath) >
          options.maximumManifestBytes)
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: retained manifest exceeds byte limit");
      std::ifstream manifest(manifestPath, std::ios::binary);
      const std::string encoded{std::istreambuf_iterator<char>(manifest),
                                std::istreambuf_iterator<char>()};
      const auto pub = decodePublication(encoded);
      if (!pub ||
          !std::filesystem::exists(entry.path() / "store" / "store.tables"))
        throw PublicationInboxUnreadable(
            "Publication inbox format 1: invalid retained signed snapshot");
      const std::vector<TorrentContent> files{
          {.path = "publication.xanadoc", .data = encoded}};
      const auto hash = makeTorrent(files, "publication").hash;
      Job job;
      job.link.key           = pub->publisher;
      job.link.salt          = pub->salt;
      job.status.id          = id;
      job.status.uri         = job.link.uri();
      job.status.title       = pub->title;
      job.pin                = PublicationPin{.publisher = pub->publisher,
                                              .salt      = pub->salt,
                                              .hash      = hash,
                                              .sequence  = pub->sequence,
                                              .version   = pub->version,
                                              .title     = pub->title};
      job.status.pin         = job.pin;
      job.verifyRetainedOnly = true;
      job.status.phase       = PublicationDownloadPhase::Queued;
      job.status.verification =
          PublicationDownloadVerification::ManifestVerified;
      job.status.storePath    = entry.path() / "store";
      job.status.version      = pub->version;
      job.status.sequence     = pub->sequence;
      job.status.manifestHash = hash;
      const auto foundJob     = jobs.find(id);
      if (foundJob == jobs.end()) {
        if (jobs.size() >= options.maximumRequests)
          throw PublicationInboxUnreadable(
              "Publication inbox format 1: request count limit exceeded");
        jobs.emplace(id, std::move(job));
      } else {
        if (!foundJob->second.pin || *foundJob->second.pin != *job.pin)
          throw PublicationInboxUnreadable(
              "Publication inbox format 1: installed snapshot does not match "
              "request pin");
        foundJob->second.verifyRetainedOnly  = true;
        foundJob->second.status.storePath    = entry.path() / "store";
        foundJob->second.status.version      = pub->version;
        foundJob->second.status.sequence     = pub->sequence;
        foundJob->second.status.manifestHash = hash;
        if (foundJob->second.status.phase != PublicationDownloadPhase::Failed &&
            foundJob->second.status.phase !=
                PublicationDownloadPhase::Cancelled) {
          foundJob->second.status.phase = PublicationDownloadPhase::Queued;
          foundJob->second.status.verification =
              PublicationDownloadVerification::ManifestVerified;
          foundJob->second.status.error.clear();
        }
      }
    }
    for (auto &[id, job] : jobs) {
      (void)id;
      if (job.status.phase == PublicationDownloadPhase::Ready &&
          !job.verifyRetainedOnly) {
        job.status.phase        = PublicationDownloadPhase::Failed;
        job.status.verification = PublicationDownloadVerification::Unchecked;
        job.status.error =
            "Installed publication snapshot is missing; retained request "
            "was not opened";
      }
    }
    if (!hadJournal && !jobs.empty()) persist();
    if (hadJournal) {
      for (auto &[id, job] : jobs) {
        (void)id;
        if (job.verifyRetainedOnly) {
          if (job.status.phase != PublicationDownloadPhase::Failed &&
              job.status.phase != PublicationDownloadPhase::Cancelled) {
            job.status.phase = PublicationDownloadPhase::Queued;
            job.status.verification =
                PublicationDownloadVerification::ManifestVerified;
            job.status.error.clear();
          }
        } else if (job.status.phase != PublicationDownloadPhase::Ready &&
                   job.status.phase != PublicationDownloadPhase::Failed &&
                   job.status.phase != PublicationDownloadPhase::Cancelled) {
          job.status.phase = PublicationDownloadPhase::Cancelled;
          job.status.error =
              "Interrupted when the application closed; retry explicitly";
        }
      }
      persist();
    }
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  ~Impl() {
    {
      const std::scoped_lock lock(guard);
      worker.request_stop();
      for (auto &[id, job] : jobs) job.cancel.request_stop();
    }
    changed.notify_all();
    worker.join();
  }
  template <class Fun>
  void update(const std::string &id, Fun fun, bool durable = true,
              std::optional<std::uint64_t> expectedAttempt = std::nullopt) {
    const std::scoped_lock lock(guard);
    auto &job = jobs.at(id);
    if (expectedAttempt && job.attempt != *expectedAttempt) return;
    const auto prior = job.status;
    fun(job.status);
    if (durable) {
      try {
        persist();
      } catch (...) {
        job.status = prior;
        throw;
      }
    }
    changed.notify_all();
  }
  template <class Fun>
  void mutate(const std::string &id, Fun fun,
              std::optional<std::uint64_t> expectedAttempt = std::nullopt) {
    const std::scoped_lock lock(guard);
    auto &job = jobs.at(id);
    if (expectedAttempt && job.attempt != *expectedAttempt) return;
    const auto prior = job;
    fun(job);
    try {
      persist();
    } catch (...) {
      job = prior;
      throw;
    }
    changed.notify_all();
  }
  void download(Job job) {
    const auto id   = job.status.id;
    const auto stop = job.cancel.get_token();
    const auto root = options.directory / id;
    const auto attemptRoot =
        options.directory / (id + ".attempt-" + std::to_string(job.attempt));
    try {
      checkCancelled(stop);
      if (job.verifyRetainedOnly) {
        const auto manifestPath = root / "publication.xanadoc";
        std::ifstream manifest(manifestPath, std::ios::binary);
        const std::string encoded{std::istreambuf_iterator<char>(manifest),
                                  std::istreambuf_iterator<char>()};
        const auto pub = decodePublication(encoded);
        if (!pub || !job.pin || pub->publisher != job.pin->publisher ||
            pub->salt != job.pin->salt || pub->sequence != job.pin->sequence ||
            pub->version != job.pin->version || pub->title != job.pin->title)
          throw std::runtime_error(
              "Retained publication manifest no longer matches its citation");
        const std::vector<TorrentContent> files{
            {.path = "publication.xanadoc", .data = encoded}};
        if (makeTorrent(files, "publication").hash != job.pin->hash)
          throw std::runtime_error(
              "Retained publication carrier hash does not match its citation");
        update(
            id,
            [&](auto &status) {
              status.verification =
                  PublicationDownloadVerification::ManifestVerified;
            },
            true, job.attempt);
        const auto published = root / "store" / "published";
        const auto dependencies =
            reviewPublicationDependencies(*pub, {published});
        if (dependencies.size() > options.maximumDependencies)
          throw std::runtime_error(
              "Retained publication has too many dependencies");
        auto remaining = options.maximumDependencyBytes;
        for (const auto &dependency : dependencies) {
          if (dependency.bytes > remaining)
            throw std::runtime_error(
                "Retained publication dependencies exceed configured byte "
                "limit");
          remaining -= dependency.bytes;
        }
        DirectoryContentSource source;
        for (const auto &dependency : dependencies)
          (void)source.add(dependency.metainfo, dependency.savePath.string());
        auto canonical = restorePublication(
            *pub, source, std::make_shared<UserPermascroll>());
        Store restored;
        restored.load((root / "store").string());
        const auto retainedTables =
            readStoreTables(root / "store" / "store.tables");
        if (restored.documentId() != pub->storeId ||
            retainedTables.documentId != pub->storeId ||
            restored.publishedLocalScroll() !=
                canonical->publishedLocalScroll() ||
            retainedTables.publishedLocalScroll !=
                canonical->publishedLocalScroll() ||
            restored.opCount() != canonical->opCount() ||
            !restored.getOp(job.pin->version) ||
            restored.textOf(job.pin->version) !=
                canonical->textOf(job.pin->version))
          throw std::runtime_error(
              "Retained native store does not match the signed publication");
        for (std::uint64_t index = 1; index <= canonical->opCount(); ++index) {
          const auto nativeId =
              restored.segmentedOps().idOf(static_cast<std::uint32_t>(index));
          const auto signedId =
              canonical->segmentedOps().idOf(static_cast<std::uint32_t>(index));
          if (nativeId != signedId ||
              restored.getOp(nativeId) != canonical->getOp(signedId))
            throw std::runtime_error(
                "Retained native operation history does not match the signed "
                "publication");
        }
        const auto &nativeScrolls    = restored.scrolls();
        const auto &canonicalScrolls = canonical->scrolls();
        if (nativeScrolls.size() != canonicalScrolls.size())
          throw std::runtime_error(
              "Retained native scroll table does not match the signed "
              "publication");
        for (std::size_t index = 0; index < nativeScrolls.size(); ++index) {
          const auto &nativeScroll    = nativeScrolls[index];
          const auto &canonicalScroll = canonicalScrolls[index];
          if (nativeScroll.publisher != canonicalScroll.publisher ||
              nativeScroll.salt != canonicalScroll.salt ||
              nativeScroll.defaultMimeType != canonicalScroll.defaultMimeType ||
              nativeScroll.segments != canonicalScroll.segments)
            throw std::runtime_error(
                "Retained native scroll table does not match the signed "
                "publication");
        }
        std::vector<ScrollSegment> expectedDeploymentSegments;
        for (const auto &scroll : canonicalScrolls) {
          for (const auto &segment : scroll.segments) {
            if ((!segment.path.empty() || !segment.torrent.isZero()) &&
                std::ranges::find(expectedDeploymentSegments, segment) ==
                    expectedDeploymentSegments.end())
              expectedDeploymentSegments.push_back(segment);
          }
        }
        if (retainedTables.localSegments != expectedDeploymentSegments)
          throw std::runtime_error(
              "Retained native deployment segments do not match the signed "
              "publication");
        syncStagedTree(root);
        syncDirectoryChain(options.directory);
        checkCancelled(stop);
        update(
            id,
            [&](auto &status) {
              checkCancelled(stop);
              syncDirectory(options.directory);
              status.phase        = PublicationDownloadPhase::Ready;
              status.verification = PublicationDownloadVerification::Complete;
              status.storePath    = root / "store";
            },
            true, job.attempt);
        return;
      }
      if (!options.makeTransport)
        throw std::runtime_error(
            "No publication connection is configured. Content availability "
            "has not been checked.");
      // Each attempt owns a fresh session and cache. A failed attempt cannot
      // share active torrent writers with verified installation or a retry.
      if (std::filesystem::exists(root))
        throw std::runtime_error(
            "An installed publication already occupies this request ID");
      validateRetainedPaths(attemptRoot);
      std::filesystem::create_directories(attemptRoot);
      auto transport = options.makeTransport();
      if (!transport)
        throw std::runtime_error("Publication transport unavailable");
      const auto pointer =
          job.pin ? MutablePointer{job.pin->hash, job.pin->sequence}
                  : transport->resolve(job.link, stop);
      if (job.minimum && (pointer.sequence < job.minimum->sequence ||
                          (pointer.sequence == job.minimum->sequence &&
                           pointer.hash != job.minimum->hash)))
        throw std::runtime_error("Publication pointer rolls back or conflicts "
                                 "with the subscription's observed sequence");
      if (!job.pin)
        mutate(
            id, [&](auto &current) { current.minimum = pointer; }, job.attempt);
      checkCancelled(stop);
      const auto cache = attemptRoot / "cache";
      transport->fetch(pointer.hash, cache / pointer.hash.hex(),
                       options.maximumManifestBytes, stop);
      const auto seed =
          reviewPublicationSeed(pointer.hash, cache / pointer.hash.hex());
      if (seed.bytes > options.maximumManifestBytes)
        throw std::runtime_error(
            "Publication manifest exceeds configured byte limit");
      DirectoryContentSource source;
      source.add(seed.metainfo, seed.savePath.string());
      const auto encoded = source.readStream(pointer.hash, 0, seed.bytes);
      const auto pub     = decodePublication(encoded);
      if (!pub || pub->publisher != job.link.key ||
          pub->salt != job.link.salt || pub->sequence != pointer.sequence)
        throw std::runtime_error("Publication signature, name or sequence does "
                                 "not match the signed DHT pointer");
      if (job.pin &&
          (pub->version != job.pin->version || pub->title != job.pin->title))
        throw std::runtime_error(
            "Publication version or title differs from the pinned citation");
      const PublicationPin verifiedPin{.publisher = pub->publisher,
                                       .salt      = pub->salt,
                                       .hash      = pointer.hash,
                                       .sequence  = pub->sequence,
                                       .version   = pub->version,
                                       .title     = pub->title};
      mutate(
          id,
          [&](auto &current) {
            current.pin                 = verifiedPin;
            current.status.pin          = verifiedPin;
            current.status.title        = pub->title;
            current.status.sequence     = pub->sequence;
            current.status.version      = pub->version;
            current.status.manifestHash = pointer.hash;
            current.status.verification =
                PublicationDownloadVerification::ManifestVerified;
          },
          job.attempt);
      checkCancelled(stop);
      std::set<InfoHash> dependencies;
      for (const auto &[name, scroll] : pub->scrolls) {
        (void)name;
        for (const auto &segment : scroll.segments)
          dependencies.insert(segment.torrent);
      }
      for (const auto &segment : pub->opsSegments)
        dependencies.insert(segment.torrent);
      if (dependencies.size() > options.maximumDependencies)
        throw std::runtime_error("Publication has too many dependencies");
      update(
          id,
          [&](auto &status) {
            status.title           = pub->title;
            status.phase           = PublicationDownloadPhase::Downloading;
            status.dependencyCount = dependencies.size();
          },
          true, job.attempt);
      auto remaining = options.maximumDependencyBytes;
      for (const auto &hash : dependencies) {
        checkCancelled(stop);
        transport->fetch(hash, cache / hash.hex(), remaining, stop);
        const auto dependency = reviewPublicationSeed(hash, cache / hash.hex());
        if (dependency.bytes > remaining)
          throw std::runtime_error(
              "Publication dependencies exceed configured byte limit");
        remaining -= dependency.bytes;
        update(
            id, [](auto &status) { ++status.completedDependencies; }, false,
            job.attempt);
      }
      // Destroy the session before copying retained files: libtorrent flushes
      // disk writes at shutdown, and the installer requires stable bytes.
      transport.reset();
      checkCancelled(stop);
      update(
          id,
          [](auto &status) {
            status.phase = PublicationDownloadPhase::Verifying;
          },
          true, job.attempt);
      (void)installPublication(*pub, {cache},
                               std::make_shared<UserPermascroll>(),
                               attemptRoot / "store");
      checkCancelled(stop);
      std::filesystem::remove_all(cache);
      std::ofstream manifest(attemptRoot / "publication.xanadoc.partial",
                             std::ios::binary);
      manifest << encoded;
      manifest.close();
      if (!manifest)
        throw std::runtime_error("Cannot retain signed publication manifest");
      syncStagedTree(attemptRoot);
      update(
          id,
          [&](auto &status) {
            checkCancelled(stop);
            renameInstalledRoot(attemptRoot / "publication.xanadoc.partial",
                                attemptRoot / "publication.xanadoc");
            syncDirectory(attemptRoot);
            if (std::filesystem::exists(root))
              throw std::runtime_error(
                  "Publication snapshot root already exists");
            renameInstalledRoot(attemptRoot, root);
            syncDirectory(options.directory);
            status.phase        = PublicationDownloadPhase::Ready;
            status.verification = PublicationDownloadVerification::Complete;
            status.version      = pub->version;
            status.sequence     = pub->sequence;
            status.manifestHash = pointer.hash;
            status.storePath    = root / "store";
          },
          true, job.attempt);
    } catch (const std::exception &error) {
      try {
        mutate(
            id,
            [&](auto &current) {
              auto &status = current.status;
              if (status.phase == PublicationDownloadPhase::Ready) return;
              status.phase = stop.stop_requested()
                                 ? PublicationDownloadPhase::Cancelled
                                 : PublicationDownloadPhase::Failed;
              status.error = stop.stop_requested()
                                 ? (current.cancellationRequested
                                        ? "Cancelled by reader"
                                        : "Interrupted when the application "
                                          "closed; retry explicitly")
                                 : boundedDiagnostic(error.what());
            },
            job.attempt);
      } catch (const std::exception &persistError) {
        const std::scoped_lock lock(guard);
        auto &status = jobs.at(id).status;
        if (status.phase != PublicationDownloadPhase::Ready) {
          const bool readerCancelled = jobs.at(id).cancellationRequested;
          status.phase               = stop.stop_requested()
                                           ? PublicationDownloadPhase::Cancelled
                                           : PublicationDownloadPhase::Failed;
          status.error =
              stop.stop_requested()
                  ? (readerCancelled ? "Cancelled by reader; request state "
                                       "could not be saved"
                                     : "Interrupted when the application "
                                       "closed; request state could not be "
                                       "saved")
                  : boundedDiagnostic(std::string(error.what()) +
                                      "; request state could not be saved: " +
                                      persistError.what());
        }
        changed.notify_all();
      }
    }
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(guard);
      changed.wait(lock, [&] {
        return stop.stop_requested() ||
               std::ranges::any_of(jobs, [](const auto &item) {
                 return item.second.status.phase ==
                        PublicationDownloadPhase::Queued;
               });
      });
      if (stop.stop_requested()) return;
      const auto next = std::ranges::find_if(jobs, [](const auto &item) {
        return item.second.status.phase == PublicationDownloadPhase::Queued;
      });
      next->second.status.phase = next->second.verifyRetainedOnly
                                      ? PublicationDownloadPhase::Verifying
                                      : PublicationDownloadPhase::Resolving;
      try {
        persist();
      } catch (const std::exception &error) {
        next->second.status.phase = PublicationDownloadPhase::Failed;
        next->second.status.error = boundedDiagnostic(
            std::string("Cannot save publication request state: ") +
            error.what());
        changed.notify_all();
        continue;
      }
      const auto job = next->second;
      changed.notify_all();
      lock.unlock();
      download(job);
    }
  }
};

PublicationInbox::PublicationInbox(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PublicationInbox::~PublicationInbox() = default;
std::string PublicationInbox::submit(const MutableLink &link,
                                     std::optional<MutablePointer> minimum) {
  if (link.key.isZero() || link.salt.empty() || link.salt.size() > 64)
    throw std::invalid_argument(
        "A publication needs a nonzero author key and document salt");
  if (minimum && (minimum->sequence < 0 || minimum->hash.isZero()))
    throw std::invalid_argument("Invalid subscription pointer lower bound");
  const std::scoped_lock lock(impl_->guard);
  if (impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::length_error("Publication inbox request limit reached");
  std::string id;
  do {
    id = createMutableKeys().publicKey.hex();
  } while (impl_->jobs.contains(id) ||
           std::filesystem::exists(impl_->options.directory / id));
  Impl::Job job;
  job.link       = link;
  job.minimum    = minimum;
  job.status.id  = id;
  job.status.uri = link.uri();
  impl_->jobs.emplace(id, std::move(job));
  try {
    impl_->persist();
  } catch (...) {
    impl_->jobs.erase(id);
    throw;
  }
  impl_->changed.notify_all();
  return id;
}
std::string PublicationInbox::submitPinned(const PublicationPin &pin) {
  if (pin.publisher.isZero() || pin.salt.empty() || pin.salt.size() > 64 ||
      pin.hash.isZero() || pin.sequence < 0 ||
      pin.title.size() > maximumTitleBytes)
    throw std::invalid_argument("Invalid pinned publication");
  const std::scoped_lock lock(impl_->guard);
  for (const auto &[existingId, job] : impl_->jobs) {
    if (job.status.pin == pin) return existingId;
  }
  if (impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::length_error("Publication inbox request limit reached");
  std::string id;
  do {
    id = createMutableKeys().publicKey.hex();
  } while (impl_->jobs.contains(id) ||
           std::filesystem::exists(impl_->options.directory / id));
  Impl::Job job;
  job.link.key                = pin.publisher;
  job.link.salt               = pin.salt;
  job.link.currentWhenWritten = pin.hash;
  job.pin                     = pin;
  job.status.id               = id;
  job.status.uri              = job.link.uri();
  job.status.pin              = pin;
  job.exactPin                = true;
  impl_->jobs.emplace(id, std::move(job));
  try {
    impl_->persist();
  } catch (...) {
    impl_->jobs.erase(id);
    throw;
  }
  impl_->changed.notify_all();
  return id;
}
void PublicationInbox::cancel(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase == PublicationDownloadPhase::Ready ||
      job.status.phase == PublicationDownloadPhase::Failed ||
      job.status.phase == PublicationDownloadPhase::Cancelled)
    return;
  const auto prior  = job;
  const bool queued = job.status.phase == PublicationDownloadPhase::Queued;
  job.cancellationRequested = true;
  if (queued) job.status.phase = PublicationDownloadPhase::Cancelled;
  job.status.error = queued ? "Cancelled by reader" : "Cancellation requested";
  try {
    impl_->persist();
  } catch (...) {
    job = prior;
    throw;
  }
  job.cancel.request_stop();
  impl_->changed.notify_all();
}
void PublicationInbox::retry(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase != PublicationDownloadPhase::Failed &&
      job.status.phase != PublicationDownloadPhase::Cancelled)
    throw std::logic_error("Only failed or cancelled downloads can be retried");
  if (job.attempt >=
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::overflow_error("Publication retry counter exhausted");
  const auto prior                 = job;
  job.cancel                       = std::stop_source{};
  job.cancellationRequested        = false;
  job.status.phase                 = PublicationDownloadPhase::Queued;
  job.status.completedDependencies = 0;
  job.status.dependencyCount       = 0;
  job.status.verification          = PublicationDownloadVerification::Unchecked;
  if (!job.verifyRetainedOnly) job.status.manifestHash = {};
  job.status.error.clear();
  ++job.attempt;
  try {
    impl_->persist();
  } catch (...) {
    job = prior;
    throw;
  }
  impl_->changed.notify_all();
}
std::vector<PublicationDownloadStatus> PublicationInbox::statuses() const {
  const std::scoped_lock lock(impl_->guard);
  std::vector<PublicationDownloadStatus> result;
  for (const auto &[id, job] : impl_->jobs) result.push_back(job.status);
  return result;
}
PublicationDownloadStatus PublicationInbox::status(std::string_view id) const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->jobs.at(std::string(id)).status;
}
bool PublicationInbox::waitFor(std::string_view id,
                               PublicationDownloadPhase phase,
                               std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).status.phase == phase;
  });
}
std::function<std::unique_ptr<PublicationDownloadTransport>()>
publicationDownloadSwarmTransport(
    SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes,
    std::vector<std::pair<std::string, std::uint16_t>> peers,
    std::chrono::milliseconds timeout) {
  options.enableLocalDiscovery = false;
  options.enableTrackers       = false;
  options.readTimeout          = 250ms;
  options.metadataTimeout      = 250ms;
  return [options, nodes = std::move(nodes), peers = std::move(peers),
          timeout] {
    return std::make_unique<DownloadTransport>(options, nodes, peers, timeout);
  };
}
} // namespace xanadu
