#include "common/xanadu/evidence/evidence_case.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <boost/interprocess/sync/file_lock.hpp>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "common/xanadu/spool_write.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::evidence {
namespace {
constexpr std::size_t maximumReferenceDimensions = 4096;

bool validReferenceDimension(std::string_view name) {
  return !name.empty() && name.size() <= 128 &&
         std::ranges::all_of(name, [](const unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                  ch == '_';
         });
}

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr std::string_view schemaVersion     = "1";
constexpr std::string_view recordsDimension  = "d.evidence.records";
constexpr std::string_view childrenDimension = "d.evidence.children";
constexpr std::string_view recordSequenceDimension =
    "d.evidence.record-sequence";
constexpr std::string_view childSequenceDimension = "d.evidence.child-sequence";
constexpr std::string_view attributeSequenceDimension =
    "d.evidence.attribute-sequence";
constexpr std::string_view quoteSequenceDimension = "d.evidence.quote-sequence";
constexpr std::string_view attributesDimension    = "d.evidence.attributes";
constexpr std::string_view kindDimension          = "d.evidence.kind";
constexpr std::string_view labelDimension         = "d.evidence.label";
constexpr std::string_view attributeKeyDimension  = "d.evidence.attribute-key";
constexpr std::string_view attributeValueDimension =
    "d.evidence.attribute-value";
constexpr std::string_view targetDimension        = "d.evidence.target";
constexpr std::string_view quoteDimension         = "d.evidence.quote";
constexpr std::string_view quoteItemsDimension    = "d.evidence.quote-items";
constexpr std::string_view bodyDimension          = "d.evidence.body";
constexpr std::string_view rawValueDimension      = "d.evidence.raw-value";
constexpr std::string_view fieldIdentityDimension = "d.evidence.field-identity";
constexpr std::string_view exportManifestDimension =
    "d.evidence.export-manifest";

[[nodiscard]] std::string typedKey(const CapturedValue &value) {
  return storageName(value.storage) + ":" + displayValue(value);
}

[[nodiscard]] std::optional<std::size_t>
columnIndex(const std::vector<Column> &columns, const std::string_view name) {
  const auto it = std::ranges::find(columns, name, &Column::name);
  if (it == columns.end()) return std::nullopt;
  return static_cast<std::size_t>(it - columns.begin());
}

[[nodiscard]] std::string decodeHex(std::string_view encoded) {
  if (!encoded.starts_with("0x") || encoded.size() % 2 != 0) {
    throw std::runtime_error("invalid retained blob parameter");
  }
  std::string decoded;
  decoded.reserve((encoded.size() - 2) / 2);
  for (std::size_t i = 2; i < encoded.size(); i += 2) {
    unsigned byte = 0;
    const auto [end, error] =
        std::from_chars(encoded.data() + i, encoded.data() + i + 2, byte, 16);
    if (error != std::errc{} || end != encoded.data() + i + 2) {
      throw std::runtime_error("invalid retained blob parameter");
    }
    decoded.push_back(static_cast<char>(byte));
  }
  return decoded;
}

[[nodiscard]] std::filesystem::path
ownerLockPath(const std::filesystem::path &casePath) {
  auto canonical = std::filesystem::weakly_canonical(casePath);
  canonical += ".owner.lock";
  return canonical;
}

std::mutex processOwnerMutex;
std::set<std::string> processOwnerPaths;

struct CaseOwnerLock {
  std::string key;
  std::unique_ptr<boost::interprocess::file_lock> fileLock;
  bool locked{};
  bool registered{};

  ~CaseOwnerLock() {
    if (locked) {
      try {
        fileLock->unlock();
      } catch (...) {
        // Closing the lock handle below still releases the kernel lock.
      }
    }
    fileLock.reset();
    if (registered) {
      const std::scoped_lock guard(processOwnerMutex);
      processOwnerPaths.erase(key);
    }
  }
};

[[nodiscard]] std::unique_ptr<CaseOwnerLock>
acquireOwnerLock(const std::filesystem::path &casePath) {
  const auto lockPath = ownerLockPath(casePath);
  auto owner          = std::make_unique<CaseOwnerLock>();
  owner->key          = lockPath.string();
  {
    const std::scoped_lock guard(processOwnerMutex);
    if (!processOwnerPaths.insert(owner->key).second) {
      throw std::runtime_error("evidence case already has an active writer");
    }
    owner->registered = true;
  }
#ifdef _WIN32
  const int fd = ::_open(lockPath.string().c_str(), _O_CREAT | _O_RDWR,
                         _S_IREAD | _S_IWRITE);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot create evidence case owner lock");
  }
  const auto privateMode = _S_IREAD | _S_IWRITE;
  if (::_chmod(lockPath.string().c_str(), privateMode) != 0) {
    const auto error = errno;
    ::_close(fd);
    throw std::system_error(error, std::generic_category(),
                            "cannot make evidence case owner lock private");
  }
  ::_close(fd);
#else
  const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot create evidence case owner lock");
  }
  if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "cannot make evidence case owner lock private");
  }
  if (::close(fd) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot close evidence case owner lock");
  }
#endif
  owner->fileLock = std::make_unique<boost::interprocess::file_lock>(
      lockPath.string().c_str());
  if (!owner->fileLock->try_lock()) {
    throw std::runtime_error("evidence case already has an active writer");
  }
  owner->locked = true;
  return owner;
}

struct StagingDirectory {
  std::filesystem::path path;

  ~StagingDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void syncPath(const std::filesystem::path &path, const bool directory) {
#ifdef _WIN32
  if (directory) {
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "cannot open evidence case directory for sync");
    }
    if (!::FlushFileBuffers(handle)) {
      const auto error = ::GetLastError();
      ::CloseHandle(handle);
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "cannot sync evidence case directory");
    }
    ::CloseHandle(handle);
    return;
  }
  const int fd = ::_open(path.string().c_str(), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot open evidence case file for sync");
  }
  if (::_commit(fd) != 0) {
    const auto error = errno;
    ::_close(fd);
    throw std::system_error(error, std::generic_category(),
                            "cannot sync evidence case file");
  }
  ::_close(fd);
#else
  const auto flags = O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0);
  const int fd     = ::open(path.c_str(), flags);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot open evidence case path for sync");
  }
  if (::fsync(fd) != 0) {
    const auto error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "cannot sync evidence case path");
  }
  if (::close(fd) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot close evidence case path after sync");
  }
#endif
}

void atomicReplace(const std::filesystem::path &staged,
                   const std::filesystem::path &destination) {
#ifdef _WIN32
  if (!::MoveFileExW(staged.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(),
                            "cannot atomically replace evidence case file");
  }
#else
  if (::rename(staged.c_str(), destination.c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "cannot atomically replace evidence case file");
  }
#endif
}

template <typename T>
[[nodiscard]] bool hasPrefix(const std::vector<T> &prefix,
                             const std::vector<T> &whole) {
  return prefix.size() <= whole.size() &&
         std::equal(prefix.begin(), prefix.end(), whole.begin());
}

[[nodiscard]] bool extendsTables(const StoreTables &oldTables,
                                 const StoreTables &newTables) {
  if (oldTables.documentId != newTables.documentId ||
      oldTables.publishedLocalScroll != newTables.publishedLocalScroll ||
      !hasPrefix(oldTables.localSegments, newTables.localSegments) ||
      oldTables.deployedScrolls.size() > newTables.deployedScrolls.size()) {
    return false;
  }
  for (std::size_t i = 0; i < oldTables.deployedScrolls.size(); ++i) {
    const auto &oldScroll = oldTables.deployedScrolls[i];
    const auto &newScroll = newTables.deployedScrolls[i];
    if (oldScroll.publisher != newScroll.publisher ||
        oldScroll.salt != newScroll.salt ||
        oldScroll.defaultMimeType != newScroll.defaultMimeType ||
        !hasPrefix(oldScroll.segments, newScroll.segments)) {
      return false;
    }
  }
  return true;
}

void installStoreSnapshot(Store &store, const std::filesystem::path &directory,
                          const std::shared_ptr<UserPermascroll> &permascroll,
                          const EvidenceCase::SavePhaseHookForTesting &hook) {
  if (!permascroll->flush()) {
    throw std::runtime_error("cannot durably flush the author's permascroll");
  }

  auto stage =
      directory.parent_path() / (directory.filename().string() + ".stage-" +
                                 UserPermascroll::generateUniqueDeviceId());
  if (!std::filesystem::create_directory(stage)) {
    throw std::runtime_error("cannot create evidence save staging directory");
  }
  StagingDirectory cleanup{stage};
  if (::chmod(stage.c_str(), S_IRWXU) != 0) {
    throw std::runtime_error("cannot make evidence staging directory private");
  }

  store.save(stage.string());
  if (!permascroll->flush()) {
    throw std::runtime_error("cannot durably flush the author's permascroll");
  }
  syncPath(stage / "ops.nodes", false);
  syncPath(stage / storeTablesName, false);
  syncPath(stage, true);
  if (hook) hook(SavePhaseForTesting::snapshotStaged);

  const auto finalOps    = directory / "ops.nodes";
  const auto finalTables = directory / storeTablesName;
  const bool hasOps      = std::filesystem::exists(finalOps);
  const bool hasTables   = std::filesystem::exists(finalTables);
  if (hasOps != hasTables) {
    throw std::runtime_error("evidence case has an incomplete store snapshot");
  }

  const bool initialInstall = !hasOps;
  bool tablesEqual          = false;
  bool tableExtension       = false;
  if (!initialInstall) {
    const auto oldTables = readStoreTables(finalTables);
    const auto newTables = readStoreTables(stage / storeTablesName);
    tablesEqual =
        serializeStoreTables(oldTables) == serializeStoreTables(newTables);
    tableExtension = extendsTables(oldTables, newTables);
    if (!tablesEqual && !tableExtension) {
      throw std::runtime_error(
          "evidence save would change existing store table bindings");
    }
  }

  if (initialInstall || !tablesEqual) {
    atomicReplace(stage / storeTablesName, finalTables);
    syncPath(directory, true);
  } else {
    // The existing table is already the exact value this snapshot needs.
    syncPath(finalTables, false);
  }
  if (hook) hook(SavePhaseForTesting::tablesCommitted);

  atomicReplace(stage / "ops.nodes", finalOps);
  syncPath(directory, true);
  if (initialInstall) {
    const auto parent = directory.parent_path().empty()
                            ? std::filesystem::path(".")
                            : directory.parent_path();
    syncPath(parent, true);
  }
  if (hook) hook(SavePhaseForTesting::operationsCommitted);
}

} // namespace

bool detail::extendsTablesForSave(const StoreTables &oldTables,
                                  const StoreTables &newTables) {
  return extendsTables(oldTables, newTables);
}

struct EvidenceCase::Impl {
  explicit Impl(std::shared_ptr<UserPermascroll> scroll,
                SavePhaseHookForTesting savePhaseHook)
      : permascroll(std::move(scroll)),
        savePhaseHook(std::move(savePhaseHook)) {
    if (!permascroll) {
      throw std::invalid_argument(
          "evidence case needs the author's permascroll");
    }
  }

  struct Metadata {
    std::map<std::string, std::string> attributes;
  };

  std::shared_ptr<UserPermascroll> permascroll;
  SavePhaseHookForTesting savePhaseHook;
  std::unique_ptr<CaseOwnerLock> ownerLock;
  std::unique_ptr<Store> store;
  std::filesystem::path directory;
  MicroversionId head;
  CellRef root{zigzag::noCell};
  mutable std::optional<zigzag::Manifold> folded;
  mutable MicroversionId foldedAt;
  std::unordered_map<CellRef, Metadata> metadata;
  std::map<std::pair<CellRef, std::string>, CellRef> rankTails;
  std::map<std::string, DimRef> dimensions;

  [[nodiscard]] DimRef dim(const std::string_view name) const {
    const auto it = dimensions.find(std::string(name));
    if (it == dimensions.end()) {
      throw std::runtime_error("evidence case schema is missing dimension " +
                               std::string(name));
    }
    return it->second;
  }

  void advanceTo(const MicroversionId &version) {
    if (folded && foldedAt != version) {
      if (foldedAt.isAncestorOf(version)) {
        // Most evidence writes mint one Structure operation. `advance`
        // detects a non-step and refolds that exceptional multi-op edit.
        folded->advanceOrRefold(*store, version);
      } else {
        folded = store->rebuildManifold(version);
      }
    }
    head = version;
    if (folded) foldedAt = version;
  }

  [[nodiscard]] const zigzag::Manifold &manifold() const {
    if (!folded || foldedAt != head) {
      folded   = store->rebuildManifold(head);
      foldedAt = head;
    }
    return *folded;
  }

  [[nodiscard]] std::optional<CellRef>
  property(const CellRef record, const std::string_view name) const {
    return manifold().linked(record, dim(name), DimVector::POS);
  }

  [[nodiscard]] std::string text(const CellRef cell) const {
    if (cell == zigzag::noCell) return {};
    const auto &folded = manifold();
    return folded.textOf(cell, *store);
  }

  [[nodiscard]] std::string propertyText(const CellRef record,
                                         const std::string_view name) const {
    const auto cell = property(record, name);
    return cell ? text(*cell) : std::string{};
  }

  [[nodiscard]] CellRef appendTextCell(const std::string_view value) {
    advanceTo(store->makeCell(head, value));
    return store->cellRefOf(head).value();
  }

  [[nodiscard]] CellRef appendSpanCell(const PrimediaSpan &span) {
    advanceTo(store->makeCell(head, span));
    return store->cellRefOf(head).value();
  }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    advanceTo(store->setLink(head, from, dim(dimension), DimVector::POS, to));
  }

  void linkReference(const CellRef owner, const std::string_view dimension,
                     const CellRef target, const std::string_view label) {
    const auto reference =
        makeRecord("reference", label, std::to_string(target), owner, false);
    link(owner, dimension, reference);
  }

  [[nodiscard]] std::optional<CellRef>
  referenceTarget(const CellRef owner, const std::string_view dimension) const {
    const auto reference = property(owner, dimension);
    if (!reference) return std::nullopt;
    const auto value = text(*reference);
    CellRef target{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), target);
    if (error != std::errc{} || end != value.data() + value.size()) {
      throw std::runtime_error("invalid evidence reference facet");
    }
    return target;
  }

  void appendRank(const CellRef anchor, const std::string_view dimension,
                  const std::string_view sequenceDimension,
                  const CellRef item) {
    auto &tail = rankTails[{anchor, std::string(dimension)}];
    if (tail == zigzag::noCell) {
      link(anchor, dimension, item);
    } else {
      link(tail, sequenceDimension, item);
    }
    tail = item;
  }

  [[nodiscard]] CellRef makeRecordCell(const std::string_view value) {
    const auto span =
        requireSpoolWrite(permascroll->append(value), "append evidence");
    return appendSpanCell(span);
  }

  [[nodiscard]] std::vector<PrimediaSpan> spansOf(const CellRef cell) const {
    const auto spans = manifold().contentOf(cell);
    return {spans.begin(), spans.end()};
  }

  [[nodiscard]] CellRef
  appendSharedSpans(const std::span<const PrimediaSpan> spans) {
    if (spans.empty()) return appendTextCell({});
    advanceTo(store->makeCell(head, spans.front()));
    const auto cell      = store->cellRefOf(head).value();
    std::uint64_t offset = spans.front().length;
    for (std::size_t i = 1; i < spans.size(); ++i) {
      advanceTo(store->spliceCellSpan(head, cell, offset, 0, spans[i]));
      offset += spans[i].length;
    }
    return cell;
  }

  [[nodiscard]] CellRef
  makeRecordWithSpans(const std::string_view kind, const std::string_view label,
                      const std::span<const PrimediaSpan> spans,
                      const std::optional<CellRef> parent = {},
                      const bool retainParent             = true) {
    const auto cell      = appendSharedSpans(spans);
    const auto kindCell  = appendTextCell(kind);
    const auto labelCell = appendTextCell(label);
    link(cell, kindDimension, kindCell);
    link(cell, labelDimension, labelCell);
    metadata.try_emplace(cell);
    appendRank(store->homeCell().value(), recordsDimension,
               recordSequenceDimension, cell);
    if (parent) {
      appendRank(*parent, childrenDimension, childSequenceDimension, cell);
      if (retainParent)
        putAttribute(cell, "parent-id", std::to_string(*parent));
    }
    return cell;
  }

  [[nodiscard]] CellRef makeRecord(const std::string_view kind,
                                   const std::string_view label,
                                   const std::string_view value,
                                   const std::optional<CellRef> parent = {},
                                   const bool retainParent             = true) {
    const auto cell      = makeRecordCell(value);
    const auto kindCell  = appendTextCell(kind);
    const auto labelCell = appendTextCell(label);
    link(cell, kindDimension, kindCell);
    link(cell, labelDimension, labelCell);
    metadata.try_emplace(cell);
    appendRank(store->homeCell().value(), recordsDimension,
               recordSequenceDimension, cell);
    if (parent) {
      appendRank(*parent, childrenDimension, childSequenceDimension, cell);
      if (retainParent)
        putAttribute(cell, "parent-id", std::to_string(*parent));
    }
    return cell;
  }

  [[nodiscard]] std::vector<CellRef> allRecords() const {
    const auto &folded = manifold();
    const auto home    = folded.home();
    return home ? rank(*home, recordsDimension) : std::vector<CellRef>{};
  }

  [[nodiscard]] std::optional<CellRef>
  findSubject(const CellRef source, const std::string_view incarnation,
              const std::string_view identity) const {
    for (const auto id : allRecords()) {
      const auto record = readRecord(id);
      if (record.kind == "subject" && record.parent == source &&
          record.attributes.contains("identity-scope") &&
          record.attributes.at("identity-scope") ==
              "declared-key-with-incarnation-witness" &&
          record.attributes.at("identity") == identity &&
          record.attributes.contains("source-incarnation") &&
          record.attributes.at("source-incarnation") == incarnation) {
        return id;
      }
    }
    return std::nullopt;
  }

  void putAttribute(const CellRef owner, const std::string_view key,
                    const std::string_view value) {
    auto attr = makeRecord("attribute", key, value, owner, false);
    appendRank(owner, attributesDimension, attributeSequenceDimension, attr);
    metadata.at(owner).attributes[std::string(key)] = std::string(value);
    const auto k                                    = appendTextCell(key);
    const auto spans                                = spansOf(attr);
    const auto v                                    = appendSharedSpans(spans);
    link(attr, attributeKeyDimension, k);
    link(attr, attributeValueDimension, v);
  }

  void putAttributeFromCell(const CellRef owner, const std::string_view key,
                            const CellRef valueCell) {
    const auto spans = spansOf(valueCell);
    const auto value = text(valueCell);
    const auto attr =
        makeRecordWithSpans("attribute", key, spans, owner, false);
    appendRank(owner, attributesDimension, attributeSequenceDimension, attr);
    metadata.at(owner).attributes[std::string(key)] = value;
    const auto k                                    = appendTextCell(key);
    const auto v                                    = appendSharedSpans(spans);
    link(attr, attributeKeyDimension, k);
    link(attr, attributeValueDimension, v);
  }

  [[nodiscard]] std::vector<CellRef>
  rank(const CellRef anchor, const std::string_view dimension) const {
    std::vector<CellRef> result;
    const auto &folded = manifold();
    auto current       = folded.linked(anchor, dim(dimension), DimVector::POS);
    std::unordered_set<CellRef> seen;
    const auto sequence =
        dimension == recordsDimension      ? recordSequenceDimension
        : dimension == childrenDimension   ? childSequenceDimension
        : dimension == attributesDimension ? attributeSequenceDimension
                                           : quoteSequenceDimension;
    while (current && seen.insert(*current).second) {
      result.push_back(*current);
      current = folded.linked(*current, dim(sequence), DimVector::POS);
    }
    return result;
  }

  [[nodiscard]] Record readRecord(const CellRef id) const {
    const auto &folded = manifold();
    if (!folded.contains(id) || zigzag::isEphemeral(id)) {
      throw std::out_of_range("unknown evidence record id");
    }
    Record result{.id    = id,
                  .kind  = propertyText(id, kindDimension),
                  .label = propertyText(id, labelDimension),
                  .text  = folded.textOf(id, *store)};
    for (const auto attr : rank(id, attributesDimension)) {
      result.attributes.emplace(propertyText(attr, attributeKeyDimension),
                                propertyText(attr, attributeValueDimension));
    }
    if (result.attributes.contains("parent-id")) {
      CellRef owner     = zigzag::noCell;
      const auto &value = result.attributes.at("parent-id");
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), owner);
      if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid evidence parent identity");
      }
      result.parent = owner;
    } else {
      auto first = id;
      std::unordered_set<CellRef> seen{id};
      while (true) {
        const auto previous =
            folded.linked(first, dim(childSequenceDimension), DimVector::NEG);
        if (!previous || !seen.insert(*previous).second) break;
        first = *previous;
      }
      const auto owner =
          folded.linked(first, dim(childrenDimension), DimVector::NEG);
      if (owner) result.parent = *owner;
    }
    if (result.attributes.contains("display")) {
      result.text = result.attributes.at("display");
    }
    return result;
  }

  void ensureDimensions() {
    constexpr std::array names{recordsDimension,
                               childrenDimension,
                               recordSequenceDimension,
                               childSequenceDimension,
                               attributeSequenceDimension,
                               quoteSequenceDimension,
                               attributesDimension,
                               kindDimension,
                               labelDimension,
                               attributeKeyDimension,
                               attributeValueDimension,
                               targetDimension,
                               quoteDimension,
                               quoteItemsDimension,
                               bodyDimension,
                               rawValueDimension,
                               fieldIdentityDimension};
    for (const auto name : names) {
      const auto minted = store->makeDimension(head, name);
      head              = minted.version;
      dimensions.emplace(std::string(name), minted.dim);
    }
  }

  void restore() {
    const auto &folded = manifold();
    for (const auto &[name, unused] : std::array{
             std::pair{recordsDimension, 0}, std::pair{childrenDimension, 0},
             std::pair{recordSequenceDimension, 0},
             std::pair{childSequenceDimension, 0},
             std::pair{attributeSequenceDimension, 0},
             std::pair{quoteSequenceDimension, 0},
             std::pair{attributesDimension, 0}, std::pair{kindDimension, 0},
             std::pair{labelDimension, 0}, std::pair{attributeKeyDimension, 0},
             std::pair{attributeValueDimension, 0},
             std::pair{targetDimension, 0}, std::pair{quoteDimension, 0},
             std::pair{quoteItemsDimension, 0}, std::pair{bodyDimension, 0},
             std::pair{rawValueDimension, 0},
             std::pair{fieldIdentityDimension, 0}}) {
      static_cast<void>(unused);
      const auto found = folded.dimensionNamed(name, *store);
      if (!found) {
        throw std::runtime_error("unsupported evidence case schema: missing " +
                                 std::string(name));
      }
      dimensions.emplace(std::string(name), *found);
    }
    if (folded.dimensions().size() > maximumReferenceDimensions)
      throw std::runtime_error("evidence reference dimension budget exceeded");
    for (const auto dimension : folded.dimensions()) {
      const auto name = folded.textOf(dimension, *store);
      if (validReferenceDimension(name)) dimensions.emplace(name, dimension);
    }
    const auto home = folded.home();
    if (!home) {
      throw std::runtime_error(
          "unsupported evidence case schema: missing home");
    }
    const auto schema =
        folded.linked(*home, dim(recordsDimension), DimVector::POS);
    if (!schema || propertyText(*schema, kindDimension) != "case" ||
        propertyText(*schema, labelDimension) != "evidence-case" ||
        text(*schema) != schemaVersion) {
      throw std::runtime_error("unsupported evidence case schema version");
    }
    root           = *schema;
    const auto all = rank(*home, recordsDimension);
    if (!all.empty()) root = all.front();
    for (const auto id : all) {
      const auto rec  = readRecord(id);
      auto &meta      = metadata[id];
      meta.attributes = rec.attributes;
      for (const auto name :
           {recordsDimension, childrenDimension, attributesDimension}) {
        const auto values = rank(id, name);
        if (!values.empty()) rankTails[{id, std::string(name)}] = values.back();
      }
    }
    if (!all.empty())
      rankTails[{*home, std::string(recordsDimension)}] = all.back();
    advanceTo(store->latest());
  }

  [[nodiscard]] std::vector<PrimediaSpan>
  selectionSpans(const CellRef target, const SpanSelection selection) const {
    if (selection.length == 0 ||
        selection.offset >
            std::numeric_limits<std::uint64_t>::max() - selection.length) {
      throw std::invalid_argument(
          "evidence span selection is empty or overflows");
    }
    const auto &folded = manifold();
    auto evidenceCell  = target;
    for (const auto relation : {rawValueDimension, bodyDimension}) {
      const auto raw = folded.linked(target, dim(relation), DimVector::POS);
      if (raw) {
        evidenceCell = *raw;
        break;
      }
    }
    const auto spans    = folded.contentOf(evidenceCell);
    std::uint64_t total = 0;
    for (const auto &span : spans) {
      if (span.length > std::numeric_limits<std::uint64_t>::max() - total) {
        throw std::overflow_error("evidence target span length overflows");
      }
      total += span.length;
    }
    if (selection.offset + selection.length > total) {
      throw std::out_of_range("evidence span selection exceeds target content");
    }
    std::vector<PrimediaSpan> selected;
    auto offset    = selection.offset;
    auto remaining = selection.length;
    for (const auto &span : spans) {
      if (offset >= span.length) {
        offset -= span.length;
        continue;
      }
      const auto take = std::min(remaining, span.length - offset);
      selected.push_back(span.slice(offset, take));
      remaining -= take;
      offset = 0;
      if (remaining == 0) break;
    }
    return selected;
  }
};

EvidenceCase::EvidenceCase(std::shared_ptr<UserPermascroll> permascroll)
    : EvidenceCase(std::move(permascroll), {}) {}
EvidenceCase::EvidenceCase(std::shared_ptr<UserPermascroll> permascroll,
                           SavePhaseHookForTesting savePhaseHookForTesting)
    : impl_(std::make_unique<Impl>(std::move(permascroll),
                                   std::move(savePhaseHookForTesting))) {}
EvidenceCase::~EvidenceCase() = default;

void EvidenceCase::create(const std::filesystem::path &path,
                          std::string_view title) {
  if (impl_->store) throw std::logic_error("evidence case is already open");
  if (title.empty())
    throw std::invalid_argument("evidence case title is empty");
  const bool pathExisted = std::filesystem::exists(path);
  if (std::filesystem::exists(path) && !std::filesystem::is_empty(path)) {
    throw std::runtime_error("evidence case destination is not empty");
  }
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  impl_->ownerLock = acquireOwnerLock(path);
  try {
    std::filesystem::create_directories(path);
    if (::chmod(path.c_str(), S_IRWXU) != 0) {
      throw std::runtime_error("cannot make evidence case directory private");
    }
    impl_->store = std::make_unique<Store>(impl_->permascroll);
    impl_->head  = impl_->store->sliceGenesis(MicroversionId{}, "evidence");
    impl_->ensureDimensions();
    impl_->root = impl_->makeRecord("case", "evidence-case", schemaVersion);
    impl_->putAttribute(impl_->root, "title", title);
    impl_->advanceTo(impl_->store->latest());
    impl_->folded    = impl_->store->rebuildManifold(impl_->head);
    impl_->foldedAt  = impl_->head;
    impl_->directory = std::filesystem::weakly_canonical(path);
    save();
  } catch (...) {
    impl_->store.reset();
    impl_->ownerLock.reset();
    impl_->directory.clear();
    impl_->folded.reset();
    impl_->metadata.clear();
    impl_->rankTails.clear();
    impl_->dimensions.clear();
    impl_->root = zigzag::noCell;
    std::error_code ignored;
    if (pathExisted) {
      std::filesystem::remove(path / "ops.nodes", ignored);
      std::filesystem::remove(path / storeTablesName, ignored);
    } else {
      std::filesystem::remove_all(path, ignored);
    }
    throw;
  }
}

void EvidenceCase::open(const std::filesystem::path &path) {
  if (impl_->store) throw std::logic_error("evidence case is already open");
  if (!std::filesystem::is_directory(path)) {
    throw std::runtime_error("evidence case directory does not exist");
  }
  impl_->ownerLock = acquireOwnerLock(path);
  try {
    if (::chmod(path.c_str(), S_IRWXU) != 0) {
      throw std::runtime_error(
          "cannot enforce private evidence case permissions");
    }
    impl_->store = std::make_unique<Store>(impl_->permascroll);
    impl_->store->load(path.string());
    impl_->directory = std::filesystem::weakly_canonical(path);
    impl_->advanceTo(impl_->store->latest());
    impl_->restore();
  } catch (...) {
    impl_->store.reset();
    impl_->ownerLock.reset();
    impl_->directory.clear();
    impl_->folded.reset();
    impl_->foldedAt = {};
    impl_->metadata.clear();
    impl_->rankTails.clear();
    impl_->dimensions.clear();
    impl_->root = zigzag::noCell;
    throw;
  }
}

void EvidenceCase::close() {
  if (!impl_->store) return;
  durableSave();
  impl_->store.reset();
  impl_->ownerLock.reset();
  impl_->dimensions.clear();
  impl_->metadata.clear();
  impl_->rankTails.clear();
  impl_->folded.reset();
  impl_->foldedAt = {};
  impl_->root     = zigzag::noCell;
  impl_->head     = {};
  impl_->directory.clear();
}

void EvidenceCase::save() {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  impl_->advanceTo(impl_->store->latest());
  if (::chmod(impl_->directory.c_str(), S_IRWXU) != 0) {
    throw std::runtime_error(
        "cannot enforce private evidence case permissions");
  }
  installStoreSnapshot(*impl_->store, impl_->directory, impl_->permascroll,
                       impl_->savePhaseHook);
}

void EvidenceCase::durableSave() {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  save();
}

bool EvidenceCase::isOpen() const { return static_cast<bool>(impl_->store); }

std::string EvidenceCase::title() const {
  if (!impl_->store) return {};
  const auto root = impl_->readRecord(impl_->root);
  return root.attributes.at("title");
}

std::filesystem::path EvidenceCase::path() const {
  return impl_->store ? impl_->directory : std::filesystem::path{};
}

std::vector<Record> EvidenceCase::records() const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  std::vector<Record> result;
  const auto &folded = impl_->manifold();
  const auto home    = folded.home();
  if (!home) return result;
  for (const auto id : impl_->rank(*home, recordsDimension)) {
    result.push_back(impl_->readRecord(id));
  }
  return result;
}

RecordIdPage EvidenceCase::recordIds(const std::size_t maximum) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  if (!maximum)
    throw std::invalid_argument("record scan limit must be positive");
  const auto &folded = impl_->manifold();
  RecordIdPage result;
  const auto home = folded.home();
  if (!home) {
    result.complete = true;
    return result;
  }
  auto current =
      folded.linked(*home, impl_->dim(recordsDimension), DimVector::POS);
  std::unordered_set<RecordId> seen;
  while (current) {
    if (!seen.insert(*current).second)
      throw std::runtime_error("cycle in evidence record sequence");
    if (result.ids.size() == maximum) return result;
    result.ids.push_back(*current);
    current = folded.linked(*current, impl_->dim(recordSequenceDimension),
                            DimVector::POS);
  }
  result.complete = true;
  return result;
}

Record EvidenceCase::inspect(const RecordId id) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  return impl_->readRecord(id);
}

std::string EvidenceCase::rawText(const RecordId id) const {
  return rawText(id, std::numeric_limits<std::size_t>::max());
}

std::string EvidenceCase::rawText(const RecordId id,
                                  const std::size_t maximumBytes) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  const auto &folded = impl_->manifold();
  if (!folded.contains(id) || zigzag::isEphemeral(id))
    throw std::out_of_range("unknown evidence record id");
  auto content = id;
  for (const auto relation : {rawValueDimension, bodyDimension}) {
    const auto linked = folded.linked(id, impl_->dim(relation), DimVector::POS);
    if (linked) {
      content = *linked;
      break;
    }
  }
  std::string result;
  for (const auto &span : folded.contentOf(content)) {
    if (span.length > maximumBytes - result.size())
      throw std::length_error("retained text exceeds its read byte limit");
    result += impl_->permascroll->read(span);
  }
  return result;
}

Source EvidenceCase::source(const RecordId id) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  const auto record = impl_->readRecord(id);
  if (record.kind != "connection")
    throw std::invalid_argument("record is not a connection");
  Source result;
  result.name            = record.label;
  result.kind            = record.attributes.at("source-kind") == "rest"
                               ? SourceKind::Rest
                               : SourceKind::SQLite;
  result.locator         = record.text;
  result.query           = record.attributes.at("query");
  result.keyColumn       = record.attributes.at("key-column");
  result.limits.maxRows  = std::stoull(record.attributes.at("limits-rows"));
  result.limits.maxPages = std::stoull(record.attributes.at("limits-pages"));
  result.limits.maxColumns =
      std::stoull(record.attributes.at("limits-columns"));
  result.limits.maxBytes  = std::stoull(record.attributes.at("limits-bytes"));
  result.limits.timeoutMs = std::stol(record.attributes.at("timeout-ms"));
  for (std::size_t i = 0;; ++i) {
    const auto valueKey = "parameter-" + std::to_string(i);
    if (!record.attributes.contains(valueKey)) break;
    const auto &text = record.attributes.at(valueKey);
    const auto &type = record.attributes.at(valueKey + "-type");
    if (type == "NULL") {
      result.parameters.emplace_back(std::monostate{});
    } else if (type == "integer") {
      std::int64_t value = 0;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid retained integer parameter");
      result.parameters.emplace_back(value);
    } else if (type == "real") {
      std::size_t parsed = 0;
      const double value = std::stod(text, &parsed);
      if (parsed != text.size())
        throw std::runtime_error("invalid retained real parameter");
      result.parameters.emplace_back(value);
    } else if (type == "text") {
      result.parameters.emplace_back(text);
    } else if (type == "blob") {
      result.parameters.emplace_back(Binary{decodeHex(text)});
    } else {
      throw std::runtime_error("unknown retained parameter type");
    }
  }
  return result;
}

RecordId EvidenceCase::addSource(const Source &source) {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  if (const auto problem = sourceProblem(source)) {
    throw std::invalid_argument(*problem);
  }
  if (source.name.empty() || source.locator.empty()) {
    throw std::invalid_argument("evidence source needs a name and locator");
  }
  for (const auto id : impl_->allRecords()) {
    const auto record = impl_->readRecord(id);
    if (record.kind == "connection" && record.label == source.name) {
      throw std::invalid_argument("evidence source names must be unique");
    }
  }
  const auto id =
      impl_->makeRecord("connection", source.name, source.locator, impl_->root);
  impl_->putAttribute(id, "source-kind",
                      source.kind == SourceKind::Rest ? "rest" : "sqlite");
  impl_->putAttribute(id, "query", source.query);
  impl_->putAttribute(id, "key-column", source.keyColumn);
  impl_->putAttribute(id, "limits-rows", std::to_string(source.limits.maxRows));
  impl_->putAttribute(id, "limits-pages",
                      std::to_string(source.limits.maxPages));
  impl_->putAttribute(id, "limits-columns",
                      std::to_string(source.limits.maxColumns));
  impl_->putAttribute(id, "limits-bytes",
                      std::to_string(source.limits.maxBytes));
  impl_->putAttribute(id, "timeout-ms",
                      std::to_string(source.limits.timeoutMs));
  for (std::size_t i = 0; i < source.parameters.size(); ++i) {
    CapturedValue parameter;
    parameter.value   = source.parameters[i];
    parameter.storage = std::visit(
        [](const auto &value) -> StorageClass {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::monostate>)
            return StorageClass::Null;
          if constexpr (std::is_same_v<T, std::int64_t>)
            return StorageClass::Integer;
          if constexpr (std::is_same_v<T, double>) return StorageClass::Real;
          if constexpr (std::is_same_v<T, std::string>)
            return StorageClass::Text;
          if constexpr (std::is_same_v<T, Binary>) return StorageClass::Blob;
          return StorageClass::Null;
        },
        source.parameters[i]);
    impl_->putAttribute(id, "parameter-" + std::to_string(i) + "-type",
                        storageName(parameter.storage));
    impl_->putAttribute(id, "parameter-" + std::to_string(i),
                        displayValue(parameter));
  }
  impl_->advanceTo(impl_->store->latest());
  return id;
}

RecordId EvidenceCase::appendRecord(
    const std::string_view kind, const std::string_view label,
    const std::string_view text, const std::optional<RecordId> parent,
    std::map<std::string, std::string> attributes,
    std::vector<std::pair<std::string, RecordId>> references) {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  if (kind.empty())
    throw std::invalid_argument("evidence record kind is empty");
  if (label.empty())
    throw std::invalid_argument("evidence record label is empty");

  if (parent) static_cast<void>(impl_->readRecord(*parent));

  std::set<std::string> referenceDimensions;
  std::vector<std::string> newDimensions;
  for (const auto &[dimension, target] : references) {
    if (!validReferenceDimension(dimension))
      throw std::invalid_argument("evidence reference dimension is invalid");
    if (!referenceDimensions.insert(dimension).second) {
      throw std::invalid_argument(
          "evidence record repeats a reference dimension");
    }
    if (!impl_->dimensions.contains(dimension))
      newDimensions.push_back(dimension);
    static_cast<void>(impl_->readRecord(target));
  }
  if (newDimensions.size() > maximumReferenceDimensions ||
      impl_->manifold().dimensions().size() >
          maximumReferenceDimensions - newDimensions.size())
    throw std::length_error("evidence reference dimension budget exceeded");
  if (attributes.contains("parent-id")) {
    throw std::invalid_argument("parent-id is a reserved evidence attribute");
  }

  for (const auto &name : newDimensions) {
    const auto minted = impl_->store->makeDimension(impl_->head, name);
    impl_->advanceTo(minted.version);
    impl_->dimensions.emplace(name, minted.dim);
  }
  const auto id = impl_->makeRecord(kind, label, text, parent);
  for (const auto &[key, value] : attributes) {
    impl_->putAttribute(id, key, value);
  }
  for (const auto &[dimension, target] : references) {
    impl_->linkReference(id, dimension, target, dimension);
  }
  impl_->advanceTo(impl_->store->latest());
  return id;
}

RecordId EvidenceCase::retain(const RecordId sourceId,
                              const CaptureResult &capture) {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  if (impl_->readRecord(sourceId).kind != "connection") {
    throw std::invalid_argument("capture source record is invalid");
  }
  const auto source = this->source(sourceId);
  if (capture.source.locator != source.locator ||
      capture.source.kind != source.kind ||
      capture.source.query != source.query ||
      capture.source.name != source.name ||
      capture.source.keyColumn != source.keyColumn ||
      capture.source.parameters != source.parameters ||
      capture.source.limits.maxRows != source.limits.maxRows ||
      capture.source.limits.maxColumns != source.limits.maxColumns ||
      capture.source.limits.maxPages != source.limits.maxPages ||
      capture.source.limits.maxBytes != source.limits.maxBytes ||
      capture.source.limits.timeoutMs != source.limits.timeoutMs) {
    throw std::invalid_argument("capture does not match its immutable source");
  }
  const auto observation = impl_->makeRecord(
      "observation", source.name,
      capture.startedAt + " .. " + capture.finishedAt, sourceId);
  impl_->putAttribute(observation, "capture-state", stateName(capture.state));
  impl_->putAttribute(observation, "coverage", capture.coverage);
  impl_->putAttribute(observation, "error", capture.error);
  impl_->putAttribute(observation, "started-at", capture.startedAt);
  impl_->putAttribute(observation, "finished-at", capture.finishedAt);
  const auto schemaEdition = [&] {
    std::string schema;
    for (const auto &column : capture.columns) {
      schema += std::to_string(column.name.size()) + ":" + column.name +
                std::to_string(column.declaredType.size()) + ":" +
                column.declaredType;
    }
    return schema;
  }();
  impl_->putAttribute(observation, "schema-edition", schemaEdition);
  impl_->putAttribute(observation, "key-column", source.keyColumn);
  impl_->putAttribute(observation, "source-incarnation",
                      capture.incarnation.value_or("unknown"));
  impl_->putAttribute(observation, "incarnation-witness",
                      capture.incarnation
                          ? "SQLite canonical file path/device/inode witness; "
                            "not proof of entity continuity"
                          : "unknown; continuity not established");

  for (std::size_t i = 0; i < capture.columns.size(); ++i) {
    const auto field =
        impl_->makeRecord("field", capture.columns[i].name,
                          capture.columns[i].declaredType, observation);
    impl_->putAttribute(field, "schema-edition", schemaEdition);
    impl_->putAttribute(field, "column-index", std::to_string(i));
    CellRef fieldIdentity = zigzag::noCell;
    for (const auto candidate : impl_->rank(sourceId, childrenDimension)) {
      const auto candidateRecord = impl_->readRecord(candidate);
      if (candidateRecord.kind == "field-identity" &&
          candidateRecord.label == capture.columns[i].name &&
          candidateRecord.attributes.at("schema-edition") == schemaEdition &&
          candidateRecord.attributes.at("column-index") == std::to_string(i)) {
        fieldIdentity = candidate;
        break;
      }
    }
    if (fieldIdentity == zigzag::noCell) {
      fieldIdentity =
          impl_->makeRecord("field-identity", capture.columns[i].name,
                            capture.columns[i].declaredType, sourceId);
      impl_->putAttribute(fieldIdentity, "schema-edition", schemaEdition);
      impl_->putAttribute(fieldIdentity, "column-index", std::to_string(i));
    }
    impl_->linkReference(field, fieldIdentityDimension, fieldIdentity,
                         "field identity");
  }

  const auto keyIndex = columnIndex(capture.columns, source.keyColumn);
  const auto keyNameCount =
      std::ranges::count_if(capture.columns, [&](const Column &column) {
        return column.name == source.keyColumn;
      });
  bool keysUsable = keyIndex.has_value() && keyNameCount == 1 &&
                    capture.incarnation.has_value() &&
                    capture.state == CaptureState::Complete;
  std::unordered_map<std::string, std::size_t> keyCounts;
  if (keysUsable) {
    for (const auto &row : capture.rows) {
      if (*keyIndex >= row.values.size() ||
          row.values[*keyIndex].storage == StorageClass::Null ||
          std::holds_alternative<std::monostate>(row.values[*keyIndex].value)) {
        keysUsable = false;
        break;
      }
      ++keyCounts[typedKey(row.values[*keyIndex])];
    }
    keysUsable =
        keysUsable && std::ranges::all_of(keyCounts, [](const auto &item) {
          return item.second == 1;
        });
  }
  impl_->putAttribute(observation, "stable-keys",
                      keysUsable ? "verified-unique" : "observation-local");

  for (std::size_t rowIndex = 0; rowIndex < capture.rows.size(); ++rowIndex) {
    const auto &row = capture.rows[rowIndex];
    if (row.values.size() > capture.columns.size()) {
      throw std::runtime_error("capture row exceeds its schema columns");
    }
    const auto rowRecord =
        impl_->makeRecord("row", std::to_string(rowIndex), {}, observation);
    impl_->putAttribute(rowRecord, "result-position", std::to_string(rowIndex));
    std::string subjectKey;
    if (keysUsable) {
      subjectKey = typedKey(row.values[*keyIndex]);
    }
    auto subject =
        keysUsable
            ? impl_->findSubject(sourceId, *capture.incarnation, subjectKey)
            : std::optional<CellRef>{};
    if (!subject) {
      subject = impl_->makeRecord(
          "subject", subjectKey.empty() ? std::to_string(rowIndex) : subjectKey,
          {}, sourceId);
      impl_->putAttribute(*subject, "identity-scope",
                          keysUsable ? "declared-key-with-incarnation-witness"
                                     : "observation-local");
      impl_->putAttribute(*subject, "source-incarnation",
                          capture.incarnation.value_or("unknown"));
      impl_->putAttribute(*subject, "continuity-claim",
                          "declared key plus matching file witness; key reuse "
                          "remains possible");
      impl_->putAttribute(*subject, "identity",
                          subjectKey.empty()
                              ? std::to_string(observation) + ":" +
                                    std::to_string(rowIndex)
                              : subjectKey);
    }
    impl_->linkReference(rowRecord, targetDimension, *subject, "row subject");
    for (std::size_t column = 0; column < capture.columns.size(); ++column) {
      const bool missing = column >= row.values.size();
      const CapturedValue missingValue{.storage = StorageClass::Null,
                                       .value   = std::monostate{}};
      const auto &value         = missing ? missingValue : row.values[column];
      const auto columnName     = column < capture.columns.size()
                                      ? capture.columns[column].name
                                      : "column-" + std::to_string(column);
      CellRef field             = zigzag::noCell;
      const auto existingFields = impl_->rank(observation, childrenDimension);
      for (const auto candidate : existingFields) {
        const auto candidateRecord = impl_->readRecord(candidate);
        if (candidateRecord.kind == "field" &&
            candidateRecord.label == columnName &&
            candidateRecord.attributes.at("column-index") ==
                std::to_string(column)) {
          field = candidate;
          break;
        }
      }
      if (field == zigzag::noCell) {
        throw std::runtime_error("capture row exceeds its schema columns");
      }
      std::string rendered = missing ? "<missing>" : displayValue(value);
      CellRef cell         = zigzag::noCell;
      if (value.storage == StorageClass::Integer &&
          std::holds_alternative<std::int64_t>(value.value)) {
        const auto scalar = std::get<std::int64_t>(value.value);
        impl_->advanceTo(impl_->store->makeScalarCell(impl_->head, scalar));
        cell = impl_->store->cellRefOf(impl_->head).value();
      } else if (value.storage == StorageClass::Real &&
                 std::holds_alternative<double>(value.value)) {
        const auto scalar = std::get<double>(value.value);
        impl_->advanceTo(impl_->store->makeScalarCell(impl_->head, scalar));
        cell = impl_->store->cellRefOf(impl_->head).value();
      } else if (value.storage == StorageClass::Blob &&
                 std::holds_alternative<Binary>(value.value)) {
        const auto bytes = std::get<Binary>(value.value).bytes;
        const auto span = requireSpoolWrite(
            impl_->permascroll->append(bytes), "append evidence blob");
        cell             = impl_->appendSpanCell(span);
      } else {
        const auto raw =
            missing || value.storage == StorageClass::Null ? std::string_view{}
            : value.storage == StorageClass::Text &&
                    std::holds_alternative<std::string>(value.value)
                ? std::string_view(std::get<std::string>(value.value))
                : std::string_view(rendered);
        cell = impl_->makeRecordCell(raw);
      }
      const auto valueRecord =
          impl_->makeRecord("value", columnName, {}, rowRecord);
      impl_->linkReference(valueRecord, targetDimension, field,
                           "field occurrence");
      const auto identityField =
          impl_->referenceTarget(field, fieldIdentityDimension);
      if (identityField) {
        impl_->linkReference(valueRecord, fieldIdentityDimension,
                             *identityField, "field identity");
      }
      impl_->link(valueRecord, rawValueDimension, cell);
      impl_->putAttribute(valueRecord, "storage-class",
                          storageName(value.storage));
      impl_->putAttribute(valueRecord, "value-state",
                          missing                               ? "missing"
                          : value.storage == StorageClass::Null ? "null"
                                                                : "present");
      if (value.storage == StorageClass::Text &&
          std::holds_alternative<std::string>(value.value)) {
        impl_->putAttributeFromCell(valueRecord, "display", cell);
        impl_->putAttributeFromCell(valueRecord, "exact-text", cell);
      } else {
        impl_->putAttribute(valueRecord, "display", rendered);
      }
    }
  }
  for (std::size_t i = 0; i < capture.pages.size(); ++i) {
    const auto &page = capture.pages[i];
    const auto pageRecord =
        impl_->makeRecord("page", std::to_string(i), page.locator, observation);
    impl_->putAttribute(pageRecord, "state", stateName(page.state));
    impl_->putAttribute(pageRecord, "status", std::to_string(page.status));
    impl_->putAttribute(pageRecord, "content-type", page.contentType);
    impl_->putAttribute(pageRecord, "etag", page.etag);
    impl_->putAttribute(pageRecord, "last-modified", page.lastModified);
    impl_->putAttribute(pageRecord, "next", page.next);
    impl_->putAttribute(pageRecord, "error", page.error);
    impl_->putAttribute(pageRecord, "representation",
                        page.redacted ? "withheld-derivative"
                                      : "http-entity-body");
    impl_->putAttribute(pageRecord, "redacted",
                        page.redacted ? "true" : "false");
    const auto span = requireSpoolWrite(
        impl_->permascroll->append(page.body), "append evidence page");
    const auto body = impl_->appendSpanCell(span);
    impl_->link(pageRecord, bodyDimension, body);
  }
  impl_->advanceTo(impl_->store->latest());
  return observation;
}

RecordId EvidenceCase::annotate(const RecordId target,
                                const std::string_view note,
                                const std::optional<SpanSelection> span) {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  const auto targetRecord = impl_->readRecord(target);
  const auto selected =
      span ? impl_->selectionSpans(target, *span) : std::vector<PrimediaSpan>{};
  const auto noteId = impl_->makeRecord("note", "note", note, impl_->root);
  impl_->linkReference(noteId, targetDimension, target, "annotation target");
  if (span) {
    const auto quote =
        impl_->makeRecord("quote", "selected-evidence", {}, noteId);
    impl_->linkReference(noteId, quoteDimension, quote, "quoted evidence");
    for (const auto &sourceSpan : selected) {
      const auto cell = impl_->appendSpanCell(sourceSpan);
      impl_->appendRank(quote, quoteItemsDimension, quoteSequenceDimension,
                        cell);
    }
    impl_->putAttribute(quote, "offset", std::to_string(span->offset));
    impl_->putAttribute(quote, "length", std::to_string(span->length));
    const auto redacted = targetRecord.attributes.find("redacted");
    const bool derivative =
        redacted != targetRecord.attributes.end() &&
        (redacted->second == "true" || redacted->second == "yes");
    impl_->putAttribute(quote, "source-representation",
                        derivative ? "withheld-derivative" : "retained-text");
    impl_->putAttribute(quote, "redacted", derivative ? "true" : "false");
    impl_->putAttribute(noteId, "anchor-policy",
                        derivative ? "pinned-derivative"
                                   : "pinned-observation");
  } else {
    impl_->putAttribute(noteId, "anchor-policy", "pinned-record");
  }
  impl_->putAttribute(noteId, "target-kind", targetRecord.kind);
  impl_->advanceTo(impl_->store->latest());
  return noteId;
}

std::vector<std::string> EvidenceCase::compare(const RecordId before,
                                               const RecordId after) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  const auto oldRecord = impl_->readRecord(before);
  const auto newRecord = impl_->readRecord(after);
  if (oldRecord.kind != "observation" || newRecord.kind != "observation") {
    throw std::invalid_argument("comparison requires two observations");
  }
  std::vector<std::string> changes;
  const auto appendChangedAttribute = [&](const std::string_view key,
                                          const std::string_view label) {
    const auto oldValue = oldRecord.attributes.at(std::string(key));
    const auto newValue = newRecord.attributes.at(std::string(key));
    if (oldValue != newValue) {
      changes.push_back(std::string(label) + " changed: " + oldValue + " -> " +
                        newValue);
    }
  };
  appendChangedAttribute("capture-state", "capture state");
  appendChangedAttribute("coverage", "coverage");
  appendChangedAttribute("error", "capture error");

  if (oldRecord.parent != newRecord.parent) {
    changes.emplace_back(
        "observations have different source identities; correspondence is "
        "uncertain");
    return changes;
  }

  const auto oldChildren = impl_->rank(before, childrenDimension);
  const auto newChildren = impl_->rank(after, childrenDimension);
  const auto pages       = [](const std::vector<CellRef> &children,
                        const Impl &impl) {
    std::vector<CellRef> result;
    for (const auto id : children) {
      if (impl.readRecord(id).kind == "page") result.push_back(id);
    }
    return result;
  };
  const auto oldPages = pages(oldChildren, *impl_);
  const auto newPages = pages(newChildren, *impl_);
  if (!oldPages.empty() || !newPages.empty()) {
    changes.emplace_back(
        "REST page correspondence is observation-local by ordinal; locator "
        "does not prove durable resource identity");
    const auto count       = std::max(oldPages.size(), newPages.size());
    const auto pageDetails = [&](const CellRef id) {
      const auto page     = impl_->readRecord(id);
      const auto body     = rawText(id);
      const auto bodyCell = impl_->property(id, bodyDimension);
      std::ostringstream detail;
      detail << "page #" << id << " body#"
             << (bodyCell ? std::to_string(*bodyCell) : "missing")
             << " locator=\"" << page.text
             << "\" status=" << page.attributes.at("status")
             << " state=" << page.attributes.at("state") << " etag=\""
             << page.attributes.at("etag") << "\" last-modified=\""
             << page.attributes.at("last-modified") << "\" content-type=\""
             << page.attributes.at("content-type") << "\" next=\""
             << page.attributes.at("next") << "\" error=\""
             << page.attributes.at("error") << "\" body-bytes=" << body.size()
             << " body-hex=";
      constexpr char hex[] = "0123456789abcdef";
      for (const unsigned char byte : body) {
        detail << hex[byte >> 4] << hex[byte & 0x0f];
      }
      return detail.str();
    };
    const auto samePage = [&](const CellRef oldId, const CellRef newId) {
      const auto oldPage = impl_->readRecord(oldId);
      const auto newPage = impl_->readRecord(newId);
      return oldPage.text == newPage.text &&
             oldPage.attributes.at("status") ==
                 newPage.attributes.at("status") &&
             oldPage.attributes.at("state") == newPage.attributes.at("state") &&
             oldPage.attributes.at("etag") == newPage.attributes.at("etag") &&
             oldPage.attributes.at("last-modified") ==
                 newPage.attributes.at("last-modified") &&
             oldPage.attributes.at("content-type") ==
                 newPage.attributes.at("content-type") &&
             oldPage.attributes.at("next") == newPage.attributes.at("next") &&
             oldPage.attributes.at("error") == newPage.attributes.at("error") &&
             rawText(oldId) == rawText(newId);
    };
    for (std::size_t i = 0; i < count; ++i) {
      if (i >= oldPages.size()) {
        changes.push_back("page added at ordinal " + std::to_string(i) + ": " +
                          pageDetails(newPages[i]));
      } else if (i >= newPages.size()) {
        changes.push_back("page removed at ordinal " + std::to_string(i) +
                          ": " + pageDetails(oldPages[i]));
      } else if (!samePage(oldPages[i], newPages[i])) {
        changes.push_back("page changed at ordinal " + std::to_string(i) +
                          " before {" + pageDetails(oldPages[i]) + "} after {" +
                          pageDetails(newPages[i]) + "}");
      }
    }
    return changes;
  }

  if (oldRecord.attributes.at("schema-edition") !=
      newRecord.attributes.at("schema-edition")) {
    changes.emplace_back("schema changed; field continuity is uncertain");
  }
  const auto oldIncarnation = oldRecord.attributes.at("source-incarnation");
  const auto newIncarnation = newRecord.attributes.at("source-incarnation");
  if (oldIncarnation != newIncarnation) {
    changes.emplace_back("source incarnation changed or is unknown (" +
                         oldIncarnation + " -> " + newIncarnation +
                         "); row identity continuity is uncertain");
  }
  const bool stable =
      oldRecord.attributes.at("stable-keys") == "verified-unique" &&
      newRecord.attributes.at("stable-keys") == "verified-unique" &&
      oldRecord.attributes.at("capture-state") == "complete" &&
      newRecord.attributes.at("capture-state") == "complete" &&
      oldRecord.attributes.at("schema-edition") ==
          newRecord.attributes.at("schema-edition") &&
      oldIncarnation != "unknown" && oldIncarnation == newIncarnation;
  if (!stable) {
    changes.emplace_back(
        "row correspondence is positional only; subject identity is uncertain");
  }

  const auto rowKey = [&](const CellRef row,
                          const std::string_view keyColumn) -> std::string {
    for (const auto child : impl_->rank(row, childrenDimension)) {
      const auto value = impl_->readRecord(child);
      if (value.kind == "value" && value.label == keyColumn) {
        return value.attributes.at("storage-class") + ":" +
               value.attributes.at("display");
      }
    }
    return {};
  };
  std::vector<CellRef> oldRows;
  std::vector<CellRef> newRows;
  std::map<std::string, CellRef> oldByKey;
  std::map<std::string, CellRef> newByKey;
  for (const auto row : oldChildren) {
    if (impl_->readRecord(row).kind != "row") continue;
    oldRows.push_back(row);
    if (stable)
      oldByKey[rowKey(row, oldRecord.attributes.at("key-column"))] = row;
  }
  for (const auto row : newChildren) {
    if (impl_->readRecord(row).kind != "row") continue;
    newRows.push_back(row);
    if (stable)
      newByKey[rowKey(row, newRecord.attributes.at("key-column"))] = row;
  }

  const auto compareRows = [&](const CellRef oldRow, const CellRef newRow,
                               const std::string &identity) {
    const auto oldValues     = impl_->rank(oldRow, childrenDimension);
    const auto newValues     = impl_->rank(newRow, childrenDimension);
    const auto fieldIdentity = [&](const CellRef value) {
      return impl_->referenceTarget(value, fieldIdentityDimension);
    };
    const auto rowLabel = identity + " (row #" + std::to_string(oldRow) +
                          " -> #" + std::to_string(newRow) + ")";
    for (const auto oldValueId : oldValues) {
      const auto oldValue = impl_->readRecord(oldValueId);
      if (oldValue.kind != "value") continue;
      const auto oldField = fieldIdentity(oldValueId);
      const auto found = std::ranges::find_if(newValues, [&](const CellRef id) {
        const auto value = impl_->readRecord(id);
        return value.kind == "value" && oldField &&
               fieldIdentity(id) == oldField;
      });
      if (found == newValues.end()) {
        changes.push_back("field removed for " + rowLabel + ": " +
                          oldValue.label + " value#" +
                          std::to_string(oldValueId));
        continue;
      }
      const auto newValue = impl_->readRecord(*found);
      if (oldValue.attributes.at("display") !=
              newValue.attributes.at("display") ||
          oldValue.attributes.at("storage-class") !=
              newValue.attributes.at("storage-class") ||
          oldValue.attributes.at("value-state") !=
              newValue.attributes.at("value-state")) {
        changes.push_back("changed " + rowLabel + "." + oldValue.label +
                          ": value#" + std::to_string(oldValueId) + " [" +
                          oldValue.attributes.at("storage-class") + "/" +
                          oldValue.attributes.at("value-state") + "] \"" +
                          oldValue.attributes.at("display") + "\" -> value#" +
                          std::to_string(*found) + " [" +
                          newValue.attributes.at("storage-class") + "/" +
                          newValue.attributes.at("value-state") + "] \"" +
                          newValue.attributes.at("display") + "\"");
      }
    }
    for (const auto newValueId : newValues) {
      const auto value = impl_->readRecord(newValueId);
      if (value.kind != "value") continue;
      const auto newField = fieldIdentity(newValueId);
      const bool existed =
          std::ranges::any_of(oldValues, [&](const CellRef id) {
            const auto prior = impl_->readRecord(id);
            return prior.kind == "value" && newField &&
                   fieldIdentity(id) == newField;
          });
      if (!existed) {
        changes.push_back("field added for " + rowLabel + ": " + value.label +
                          " value#" + std::to_string(newValueId) + " [" +
                          value.attributes.at("storage-class") + "/" +
                          value.attributes.at("value-state") + "] \"" +
                          value.attributes.at("display") + "\"");
      }
    }
  };

  if (stable) {
    for (const auto &[key, row] : oldByKey) {
      if (!newByKey.contains(key)) {
        changes.push_back("removed row " + key + " record#" +
                          std::to_string(row));
      } else {
        compareRows(row, newByKey.at(key), "key " + key);
      }
    }
    for (const auto &[key, row] : newByKey) {
      if (!oldByKey.contains(key)) {
        changes.push_back("added row " + key + " record#" +
                          std::to_string(row));
      }
    }
  } else {
    const auto common = std::min(oldRows.size(), newRows.size());
    for (std::size_t i = 0; i < common; ++i) {
      compareRows(oldRows[i], newRows[i],
                  "uncertain position " + std::to_string(i));
    }
    for (std::size_t i = common; i < oldRows.size(); ++i) {
      changes.push_back("unmatched before row at position " +
                        std::to_string(i) + " record#" +
                        std::to_string(oldRows[i]) + "; identity uncertain");
    }
    for (std::size_t i = common; i < newRows.size(); ++i) {
      changes.push_back("unmatched after row at position " + std::to_string(i) +
                        " record#" + std::to_string(newRows[i]) +
                        "; identity uncertain");
    }
  }
  return changes;
}

void EvidenceCase::exportSelection(
    const std::span<const RecordId> selected,
    const std::filesystem::path &destination) const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  if (selected.empty())
    throw std::invalid_argument("export selection is empty");
  const auto &sourceFold = impl_->manifold();
  std::unordered_set<CellRef> closure;
  std::unordered_set<CellRef> shallowRecords;
  const auto appendClosure = [&](auto &&self, const CellRef id) -> void {
    if (id == zigzag::noCell || !sourceFold.contains(id) ||
        !closure.insert(id).second) {
      return;
    }
    for (const auto child : impl_->rank(id, childrenDimension)) {
      self(self, child);
    }
    for (const auto attribute : impl_->rank(id, attributesDimension)) {
      self(self, attribute);
    }
    for (const auto item : impl_->rank(id, quoteItemsDimension)) {
      self(self, item);
    }
    if (shallowRecords.contains(id)) return;
    for (const auto relation :
         {kindDimension, labelDimension, attributeKeyDimension,
          attributeValueDimension, targetDimension, quoteDimension,
          fieldIdentityDimension, rawValueDimension, bodyDimension}) {
      const auto related =
          sourceFold.linked(id, impl_->dim(relation), DimVector::POS);
      if (related) self(self, *related);
    }
  };
  for (const auto id : selected) {
    static_cast<void>(impl_->readRecord(id));
    appendClosure(appendClosure, id);
  }
  const auto appendShallowRecord = [&](const CellRef id,
                                       const auto &allowedAttributes,
                                       const auto &allowedReferences) {
    if (id == zigzag::noCell || !sourceFold.contains(id)) return;
    shallowRecords.insert(id);
    closure.insert(id);
    for (const auto relation : {kindDimension, labelDimension}) {
      const auto related =
          sourceFold.linked(id, impl_->dim(relation), DimVector::POS);
      if (related) appendClosure(appendClosure, *related);
    }
    for (const auto attribute : impl_->rank(id, attributesDimension)) {
      const auto key = impl_->propertyText(attribute, attributeKeyDimension);
      if (key == "parent-id" || allowedAttributes.empty() ||
          allowedAttributes.contains(key))
        appendClosure(appendClosure, attribute);
    }
    for (const auto relation : allowedReferences) {
      const auto reference =
          sourceFold.linked(id, impl_->dim(relation), DimVector::POS);
      if (reference) appendClosure(appendClosure, *reference);
    }
  };
  std::unordered_set<CellRef> shallowQueryRuns;
  std::unordered_map<CellRef, CellRef> queryInputsByObservation;
  std::unordered_map<CellRef, CellRef> supportParent;
  for (const auto id : selected) {
    const auto record = impl_->readRecord(id);
    if (!record.parent) continue;
    std::vector<CellRef> witnessOwners{id};
    auto runId = *record.parent;
    if (record.kind == "query-member" || record.kind == "query-exclusion") {
      const auto window = impl_->readRecord(runId);
      if (window.kind != "query-window" || !window.parent) continue;
      appendShallowRecord(runId, std::unordered_set<std::string>{},
                          std::vector<std::string_view>{});
      witnessOwners.push_back(runId);
      runId = *window.parent;
    } else if (record.kind == "query-window") {
      for (const auto child : impl_->rank(id, childrenDimension)) {
        const auto kind = impl_->readRecord(child).kind;
        if (kind == "query-member" || kind == "query-exclusion")
          witnessOwners.push_back(child);
      }
    } else if (record.kind != "query-result") {
      continue;
    }
    if (impl_->readRecord(runId).kind != "query-run") continue;
    if (shallowQueryRuns.insert(runId).second) {
      appendShallowRecord(runId, std::unordered_set<std::string>{},
                          std::vector<std::string_view>{});
      for (const auto input : impl_->rank(runId, childrenDimension)) {
        if (impl_->readRecord(input).kind == "query-input") {
          appendShallowRecord(input, std::unordered_set<std::string>{},
                              std::vector<std::string_view>{targetDimension});
          const auto observation =
              impl_->referenceTarget(input, targetDimension);
          if (observation)
            queryInputsByObservation.emplace(*observation, input);
        }
      }
    }
    const auto addWitnessTargets = [&](const CellRef owner,
                                       const std::string_view kind) {
      for (const auto witness : impl_->rank(owner, childrenDimension)) {
        const auto witnessRecord = impl_->readRecord(witness);
        if (witnessRecord.kind != kind) continue;
        appendClosure(appendClosure, witness);
        const auto target = impl_->referenceTarget(witness, targetDimension);
        if (!target) continue;
        const auto targetRecord = impl_->readRecord(*target);
        if (targetRecord.kind == "row") {
          appendShallowRecord(
              *target, std::unordered_set<std::string>{"result-position"},
              std::vector<std::string_view>{});
          const auto &row = targetRecord;
          if (row.parent && queryInputsByObservation.contains(*row.parent)) {
            supportParent[*target] = queryInputsByObservation.at(*row.parent);
          }
        } else if (targetRecord.kind == "observation" &&
                   witnessRecord.label == "time value") {
          appendShallowRecord(*target,
                              std::unordered_set<std::string>{
                                  "started-at", "watermark-at-arrival"},
                              std::vector<std::string_view>{});
          if (queryInputsByObservation.contains(*target))
            supportParent[*target] = queryInputsByObservation.at(*target);
        } else if (targetRecord.kind == "field") {
          appendShallowRecord(*target,
                              std::unordered_set<std::string>{"column-index"},
                              std::vector<std::string_view>{});
          if (targetRecord.parent &&
              queryInputsByObservation.contains(*targetRecord.parent)) {
            supportParent[*target] =
                queryInputsByObservation.at(*targetRecord.parent);
          }
        } else {
          const auto &value = targetRecord;
          if (value.kind != "value") continue;
          appendShallowRecord(
              *target,
              std::unordered_set<std::string>{"storage-class", "value-state",
                                              "display", "exact-text"},
              std::vector<std::string_view>{targetDimension});
          const auto raw = sourceFold.linked(
              *target, impl_->dim(rawValueDimension), DimVector::POS);
          if (raw) appendClosure(appendClosure, *raw);
          const auto field = impl_->referenceTarget(*target, targetDimension);
          if (field && impl_->readRecord(*field).kind == "field") {
            appendShallowRecord(*field,
                                std::unordered_set<std::string>{"column-index"},
                                std::vector<std::string_view>{});
            const auto fieldRecord = impl_->readRecord(*field);
            if (fieldRecord.parent &&
                queryInputsByObservation.contains(*fieldRecord.parent)) {
              supportParent[*field] =
                  queryInputsByObservation.at(*fieldRecord.parent);
            }
          }
        }
      }
    };
    for (const auto owner : witnessOwners) {
      addWitnessTargets(owner, "query-lineage");
      addWitnessTargets(owner, "query-key-witness");
      addWitnessTargets(owner, "query-field-witness");
    }
  }
  // Notes live under the private case, so include only those anchored within
  // the selected closure and their pinned targets.
  for (const auto id : impl_->allRecords()) {
    const auto record = impl_->readRecord(id);
    const auto target = impl_->referenceTarget(id, targetDimension);
    if (record.kind == "note" && target && closure.contains(*target)) {
      appendClosure(appendClosure, id);
    }
  }

  auto outputPerma = impl_->permascroll;
  Store output(outputPerma);
  auto outHead = output.sliceGenesis(MicroversionId{}, "selected evidence");
  std::map<std::string, DimRef> outDims;
  for (const auto name :
       {recordsDimension, childrenDimension, recordSequenceDimension,
        childSequenceDimension, attributesDimension, attributeSequenceDimension,
        quoteSequenceDimension, kindDimension, labelDimension,
        attributeKeyDimension, attributeValueDimension, targetDimension,
        quoteDimension, quoteItemsDimension, bodyDimension, rawValueDimension,
        fieldIdentityDimension, exportManifestDimension}) {
    const auto made = output.makeDimension(outHead, name);
    outHead         = made.version;
    outDims.emplace(std::string(name), made.dim);
  }
  for (const auto id : closure) {
    for (const auto &edge : sourceFold.dimensionsOf(id)) {
      if (!closure.contains(edge.pos) && !closure.contains(edge.neg)) continue;
      const auto name = sourceFold.textOf(edge.dim, *impl_->store);
      if (!validReferenceDimension(name) || outDims.contains(name)) continue;
      const auto made = output.makeDimension(outHead, name);
      outHead         = made.version;
      outDims.emplace(name, made.dim);
    }
  }
  std::string manifest =
      "selected evidence; source-document=" + impl_->store->documentId().str() +
      "; case-root=" + std::to_string(impl_->root) + "; selected=";
  for (const auto id : selected) manifest += std::to_string(id) + ",";
  if (!shallowQueryRuns.empty()) {
    manifest +=
        "; query-support=query-run,query-input,key-row,key-value,key-field,"
        "window,time-value,arrival-value"
        "; omissions=unselected-sibling-results,unwitnessed-values,connections";
  }
  const auto manifestVersion = output.makeCell(outHead, manifest);
  outHead                    = manifestVersion;
  const auto manifestCell    = output.cellRefOf(manifestVersion).value();
  outHead               = output.setLink(outHead, output.homeCell().value(),
                                         outDims.at(std::string(exportManifestDimension)),
                                         DimVector::POS, manifestCell);
  const auto allRecords = impl_->allRecords();
  const std::unordered_set<CellRef> recordSet(allRecords.begin(),
                                              allRecords.end());
  std::unordered_map<CellRef, CellRef> mapped;
  std::vector<CellRef> ordered(closure.begin(), closure.end());
  std::ranges::sort(ordered);
  for (const auto id : ordered) {
    const auto spans     = sourceFold.contentOf(id);
    const auto valueKind = sourceFold.valueKindOf(id);
    std::optional<std::uint64_t> scalarBits;
    if (valueKind == ValueKind::Int64) {
      if (const auto value = sourceFold.asInt64(id))
        scalarBits = std::bit_cast<std::uint64_t>(*value);
    } else if (valueKind == ValueKind::Double) {
      if (const auto value = sourceFold.asDouble(id))
        scalarBits = std::bit_cast<std::uint64_t>(*value);
    } else if (valueKind == ValueKind::Bool) {
      if (const auto value = sourceFold.asBool(id))
        scalarBits = *value ? 1ULL : 0ULL;
    }
    if (spans.empty() && scalarBits) {
      if (valueKind == ValueKind::Int64)
        outHead = output.makeScalarCell(
            outHead, std::bit_cast<std::int64_t>(*scalarBits));
      else if (valueKind == ValueKind::Double)
        outHead =
            output.makeScalarCell(outHead, std::bit_cast<double>(*scalarBits));
      else
        outHead = output.makeScalarCell(outHead, *scalarBits != 0);
      mapped[id] = output.cellRefOf(outHead).value();
      continue;
    }
    if (spans.empty()) {
      outHead = output.makeCell(outHead, "");
    } else {
      outHead = output.makeCell(outHead, spans.front());
    }
    const auto cell = output.cellRefOf(outHead).value();
    mapped[id]      = cell;
    if (scalarBits)
      outHead =
          output.setValue(outHead, cell, spans.front(), valueKind, *scalarBits);
    std::uint64_t offset = spans.empty() ? 0 : spans.front().length;
    for (std::size_t i = 1; i < spans.size(); ++i) {
      outHead = output.spliceCellSpan(outHead, cell, offset, 0, spans[i]);
      offset += spans[i].length;
    }
  }
  if (!shallowQueryRuns.empty()) {
    manifest += "; source-map=";
    bool first = true;
    for (const auto id : ordered) {
      if (!shallowRecords.contains(id)) continue;
      if (!first) manifest.push_back(',');
      first = false;
      manifest += std::to_string(id) + ":" + std::to_string(mapped.at(id));
    }
    outHead = output.setCellText(outHead, manifestCell, manifest);
  }
  // Copy only evidence edges whose other endpoint was selected. Global record
  // chronology is rebuilt below from this closure alone.
  for (const auto id : ordered) {
    const auto from = mapped.at(id);
    for (const auto &edge : sourceFold.dimensionsOf(id)) {
      const auto name = sourceFold.textOf(edge.dim, *impl_->store);
      const auto dim  = outDims.find(name);
      if (dim == outDims.end()) continue;
      if (name == recordsDimension || name == recordSequenceDimension ||
          name == childrenDimension || name == childSequenceDimension ||
          name == attributesDimension || name == attributeSequenceDimension ||
          name == quoteItemsDimension || name == quoteSequenceDimension) {
        continue;
      }
      if (edge.pos != zigzag::noCell && mapped.contains(edge.pos)) {
        outHead = output.setLink(outHead, from, dim->second, DimVector::POS,
                                 mapped.at(edge.pos));
      }
      if (edge.neg != zigzag::noCell && mapped.contains(edge.neg)) {
        outHead = output.setLink(outHead, from, dim->second, DimVector::NEG,
                                 mapped.at(edge.neg));
      }
    }
  }
  const auto linkRank = [&](const CellRef outputOwner,
                            const std::vector<CellRef> &items,
                            const std::string_view dimension,
                            const std::string_view sequence) {
    CellRef previous = outputOwner;
    bool first       = true;
    for (const auto item : items) {
      if (!mapped.contains(item)) continue;
      outHead =
          output.setLink(outHead, previous,
                         outDims.at(std::string(first ? dimension : sequence)),
                         DimVector::POS, mapped.at(item));
      previous = mapped.at(item);
      first    = false;
    }
  };
  std::vector<CellRef> exportedRecords;
  for (const auto id : allRecords) {
    if (closure.contains(id)) exportedRecords.push_back(id);
  }
  linkRank(output.homeCell().value(), exportedRecords, recordsDimension,
           recordSequenceDimension);
  std::unordered_map<CellRef, std::vector<CellRef>> supportChildren;
  for (const auto &[child, parent] : supportParent)
    supportChildren[parent].push_back(child);
  for (auto &[unusedParent, children] : supportChildren) {
    static_cast<void>(unusedParent);
    std::ranges::sort(children);
  }
  for (const auto owner : ordered) {
    if (!recordSet.contains(owner)) continue;
    auto children = impl_->rank(owner, childrenDimension);
    if (const auto extra = supportChildren.find(owner);
        extra != supportChildren.end()) {
      children.insert(children.end(), extra->second.begin(),
                      extra->second.end());
    }
    linkRank(mapped.at(owner), children, childrenDimension,
             childSequenceDimension);
    linkRank(mapped.at(owner), impl_->rank(owner, attributesDimension),
             attributesDimension, attributeSequenceDimension);
    linkRank(mapped.at(owner), impl_->rank(owner, quoteItemsDimension),
             quoteItemsDimension, quoteSequenceDimension);
  }
  for (const auto id : ordered) {
    if (!recordSet.contains(id)) continue;
    const auto record = impl_->readRecord(id);
    if (record.kind == "reference") {
      CellRef oldTarget = zigzag::noCell;
      const auto [end, error] =
          std::from_chars(record.text.data(),
                          record.text.data() + record.text.size(), oldTarget);
      if (error == std::errc{} &&
          end == record.text.data() + record.text.size()) {
        const auto target = mapped.contains(oldTarget)
                                ? std::to_string(mapped.at(oldTarget))
                                : impl_->store->documentId().str() +
                                      "#cell=" + std::to_string(oldTarget);
        outHead           = output.setCellText(outHead, mapped.at(id), target);
      }
    }
    if (record.parent) {
      for (const auto attribute : impl_->rank(id, attributesDimension)) {
        if (impl_->propertyText(attribute, attributeKeyDimension) !=
            "parent-id")
          continue;
        const auto value = impl_->property(attribute, attributeValueDimension);
        const auto sourceParent =
            supportParent.contains(id) ? supportParent.at(id) : *record.parent;
        const auto parent = mapped.contains(sourceParent)
                                ? mapped.at(sourceParent)
                                : manifestCell;
        if (value && mapped.contains(*value)) {
          outHead = output.setCellText(outHead, mapped.at(*value),
                                       std::to_string(parent));
        }
      }
    }
  }
  outHead = output.latest();
  if (std::filesystem::exists(destination) &&
      !std::filesystem::is_empty(destination)) {
    throw std::runtime_error("evidence export destination is not empty");
  }
  std::filesystem::create_directories(destination);
  if (::chmod(destination.c_str(), S_IRWXU) != 0) {
    throw std::runtime_error("cannot make evidence export private");
  }
  output.save(destination.string());
}

const Store &EvidenceCase::store() const {
  if (!impl_->store) throw std::logic_error("evidence case is not open");
  return *impl_->store;
}

} // namespace xanadu::evidence
