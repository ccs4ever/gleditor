#include "common/xanadu/store_activity_log.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
namespace {

constexpr std::string_view kVisits      = "d.activity-visits";
constexpr std::string_view kCurrent     = "d.activity-current";
constexpr std::string_view kReferences  = "d.activity-references";
constexpr std::string_view kAnnotations = "d.activity-annotations";

std::uint64_t recordNumber(std::string_view text, std::string_view dimension) {
  std::uint64_t value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    throw ActivityRecordUnreadable(std::string(dimension) +
                                   ": invalid unsigned record number");
  return value;
}

VisitId referenceOf(std::string_view record) {
  constexpr std::string_view prefix = "reference1 ";
  if (!record.starts_with(prefix))
    throw ActivityRecordUnreadable(
        "d.activity-references: expected reference1");
  record.remove_prefix(prefix.size());
  return VisitId{recordNumber(record, kReferences)};
}

std::pair<VisitId, std::string> annotationOf(std::string_view record) {
  constexpr std::string_view prefix = "annotation1 ";
  if (!record.starts_with(prefix))
    throw ActivityRecordUnreadable(
        "d.activity-annotations: expected annotation1");
  record.remove_prefix(prefix.size());
  const auto space   = record.find(' ');
  const auto newline = record.find('\n');
  if (space == std::string_view::npos || newline == std::string_view::npos ||
      space >= newline)
    throw ActivityRecordUnreadable("d.activity-annotations: malformed header");
  const VisitId visit{recordNumber(record.substr(0, space), kAnnotations)};
  const auto length =
      recordNumber(record.substr(space + 1, newline - space - 1), kAnnotations);
  const auto text = record.substr(newline + 1);
  if (length != text.size() || text.empty())
    throw ActivityRecordUnreadable("d.activity-annotations: wrong text length");
  return {visit, std::string{text}};
}

std::string bytesOf(const DocumentId &id) { return id.str().substr(6); }

DocumentId parseId(const std::string &hex) {
  if (hex.size() != 32)
    throw ActivityRecordUnreadable("activity: bad document id");
  std::array<char, 16> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    unsigned int high{};
    unsigned int low{};
    const auto parse = [](const char digit) -> unsigned int {
      if (digit >= '0' && digit <= '9') return digit - '0';
      if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
      throw ActivityRecordUnreadable("activity: bad document id");
    };
    high     = parse(hex[2 * i]);
    low      = parse(hex[2 * i + 1]);
    bytes[i] = static_cast<char>((high << 4U) | low);
  }
  DocumentId id;
  if (!DocumentId::fromBytes({bytes.data(), bytes.size()}, id)) {
    throw ActivityRecordUnreadable("activity: bad document id");
  }
  return id;
}

std::int64_t indexOf(const std::optional<std::uint32_t> index) {
  return index ? static_cast<std::int64_t>(*index) : -1;
}

std::optional<std::uint32_t> cursorIndex(const std::int64_t value) {
  if (value == -1) return std::nullopt;
  if (value < -1)
    throw ActivityRecordUnreadable("activity: invalid unset cursor");
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ActivityRecordUnreadable("activity: cursor out of range");
  }
  return static_cast<std::uint32_t>(value);
}

// Versioned activity payloads carry the exact selection-start place. They
// remain primedia-backed records; CompactOpNode and its disk layout do not
// change.
void writeSite(std::ostream &out, const OccurrenceSite &target) {
  std::visit(
      [&out](const auto &site) {
        using Site = std::decay_t<decltype(site)>;
        if constexpr (std::is_same_v<Site, DocumentSite>) {
          out << "doc " << bytesOf(site.store) << ' ' << site.version.str()
              << ' ' << site.range.start << ' ' << site.range.end << " 0";
        } else {
          out << "cell " << bytesOf(site.store) << ' ' << site.version.str()
              << ' ' << site.range.start << ' ' << site.range.end << ' '
              << site.cell;
        }
      },
      target);
}

std::uint64_t readNumber(
    std::istream &in, const std::string_view field,
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
  std::string token;
  if (!(in >> token))
    throw ActivityRecordUnreadable("activity: missing " + std::string{field});
  const auto value = recordNumber(token, field);
  if (value > maximum)
    throw ActivityRecordUnreadable("activity: " + std::string{field} +
                                   " out of range");
  return value;
}

OccurrenceSite readSite(std::istream &in) {
  std::string kind, storeId, version;
  if (!(in >> kind >> storeId >> version) || (kind != "doc" && kind != "cell"))
    throw ActivityRecordUnreadable("activity: malformed occurrence site");
  const auto start = static_cast<std::uint32_t>(
      readNumber(in, "range start", std::numeric_limits<std::uint32_t>::max()));
  const auto end = static_cast<std::uint32_t>(
      readNumber(in, "range end", std::numeric_limits<std::uint32_t>::max()));
  const auto cell =
      readNumber(in, "cell", std::numeric_limits<zigzag::CellRef>::max());
  if (end < start || (kind == "doc" && cell != 0))
    throw ActivityRecordUnreadable("activity: malformed occurrence site");
  const auto store = parseId(storeId);
  MicroversionId at;
  try {
    at = MicroversionId::parse(version);
  } catch (const std::exception &error) {
    throw ActivityRecordUnreadable("activity: invalid occurrence version: " +
                                   std::string{error.what()});
  }
  if (kind == "doc") return DocumentSite{store, at, {start, end}};
  return CellSite{store, at, static_cast<zigzag::CellRef>(cell), {start, end}};
}

void checkPin(const SideCursor &cursor,
              const std::optional<OccurrencePin> &pin) {
  if (bool(cursor.occurrence) != bool(pin) ||
      (cursor.occurrence && !cursor.member))
    throw ActivityRecordUnreadable(
        "activity: chosen occurrence needs exact pin");
  if (pin && (pin->member.length == 0 ||
              pin->member.start > std::numeric_limits<std::uint64_t>::max() -
                                      pin->member.length))
    throw ActivityRecordUnreadable("activity: invalid pinned member range");
}

void writePin(std::ostream &out, const std::optional<OccurrencePin> &pin) {
  out << ' ' << bool(pin);
  if (!pin) return;
  out << ' ' << pin->member.scroll << ' ' << pin->member.start << ' '
      << pin->member.length << ' '
      << static_cast<unsigned int>(pin->occurrence.coverage) << ' ';
  writeSite(out, pin->occurrence.site);
}

std::optional<OccurrencePin> readPin(std::istream &in) {
  if (!readNumber(in, "occurrence pin flag", 1)) return std::nullopt;
  const auto scroll =
      readNumber(in, "pinned scroll", std::numeric_limits<ScrollId>::max());
  const auto start    = readNumber(in, "pinned member start");
  const auto length   = readNumber(in, "pinned member length");
  const auto coverage = readNumber(in, "pinned coverage", 1);
  return OccurrencePin{
      .member     = {static_cast<ScrollId>(scroll), start, length},
      .occurrence = {.site     = readSite(in),
                     .coverage = static_cast<Coverage>(coverage)}};
}

void writeContext(std::ostream &out, const LinkVisitContext &link) {
  checkPin(link.left, link.leftPin);
  checkPin(link.right, link.rightPin);
  out << bytesOf(link.key.authority) << ' ' << link.key.id << ' '
      << static_cast<unsigned int>(link.active) << ' '
      << indexOf(link.left.member) << ' ' << indexOf(link.left.occurrence)
      << ' ' << indexOf(link.right.member) << ' '
      << indexOf(link.right.occurrence) << ' '
      << (link.origin ? link.origin->value : 0) << ' '
      << static_cast<bool>(link.originCheckpoint);
  if (link.originCheckpoint) {
    out << ' ';
    writeSite(out, *link.originCheckpoint);
  }
  writePin(out, link.leftPin);
  writePin(out, link.rightPin);
}

LinkVisitContext readContext(std::istream &in) {
  std::string authority;
  if (!(in >> authority))
    throw ActivityRecordUnreadable("activity: missing link authority");
  const auto id =
      readNumber(in, "link id", std::numeric_limits<zigzag::CellRef>::max());
  const auto side = readNumber(in, "link side", 1);
  std::int64_t lm{}, lo{}, rm{}, ro{};
  if (!(in >> lm >> lo >> rm >> ro))
    throw ActivityRecordUnreadable("activity: malformed link cursors");
  const auto origin     = readNumber(in, "origin visit");
  const auto checkpoint = readNumber(in, "origin checkpoint flag", 1);
  LinkVisitContext link{
      .key    = {parseId(authority), static_cast<zigzag::CellRef>(id)},
      .active = static_cast<LinkSide>(side),
      .left   = {cursorIndex(lm), cursorIndex(lo)},
      .right  = {cursorIndex(rm), cursorIndex(ro)},
      .origin = origin ? std::optional{VisitId{origin}} : std::nullopt};
  if (checkpoint) link.originCheckpoint = readSite(in);
  link.leftPin  = readPin(in);
  link.rightPin = readPin(in);
  checkPin(link.left, link.leftPin);
  checkPin(link.right, link.rightPin);
  return link;
}

std::string encode(const Visit &visit) {
  std::ostringstream out;
  out << "visit3 " << (visit.parent ? visit.parent->value : 0) << ' ';
  writeSite(out, visit.target);
  out << ' ' << static_cast<unsigned int>(visit.arrival) << ' '
      << static_cast<bool>(visit.link);
  if (visit.link) {
    out << ' ';
    writeContext(out, *visit.link);
  }
  return out.str();
}

Visit decode(const std::string &data) {
  std::istringstream in(data);
  std::string signature, extra;
  if (!(in >> signature) || signature != "visit3")
    throw ActivityRecordUnreadable("activity: expected visit3");
  const auto parent = readNumber(in, "parent visit");
  Visit visit{.target = readSite(in)};
  const auto arrival = readNumber(in, "arrival", 1);
  const auto linked  = readNumber(in, "link flag", 1);
  if (parent) visit.parent = VisitId{parent};
  visit.arrival = static_cast<Arrival>(arrival);
  if (linked) visit.link = readContext(in);
  if (in >> extra)
    throw ActivityRecordUnreadable("activity: trailing visit3 data");
  return visit;
}

} // namespace

std::string encodeLinkContext(const LinkVisitContext &link) {
  std::ostringstream out;
  out << "selection3 ";
  writeContext(out, link);
  return out.str();
}

LinkVisitContext decodeLinkContext(const std::string_view text) {
  std::istringstream in{std::string{text}};
  std::string signature, extra;
  if (!(in >> signature) || signature != "selection3")
    throw ActivityRecordUnreadable("activity: expected selection3");
  auto link = readContext(in);
  if (in >> extra)
    throw ActivityRecordUnreadable("activity: trailing selection3 data");
  return link;
}

StoreActivityLog::StoreActivityLog(Store *aStore,
                                   std::filesystem::path aDirectory)
    : store(aStore), directory(std::move(aDirectory)) {
  if (!store || store->homeCell() == zigzag::noCell) return;
  const auto manifold = store->rebuildManifold(store->latest());
  if (const auto dim = manifold.dimensionNamed(kVisits, *store)) {
    for (auto cell = manifold.linked(store->homeCell(), *dim);
         cell != zigzag::noCell; cell = manifold.linked(cell, *dim)) {
      auto visit = decode(manifold.textOf(cell, *store));
      visit.id   = VisitId{visits.size() + 1};
      if (visit.parent && visit.parent->value >= visit.id.value) {
        throw std::runtime_error("activity: invalid visit parent");
      }
      visits.push_back(std::move(visit));
    }
  }
  if (const auto dim = manifold.dimensionNamed(kCurrent, *store)) {
    auto cell = store->homeCell();
    for (auto next = manifold.linked(cell, *dim); next != zigzag::noCell;
         next      = manifold.linked(cell, *dim)) {
      cell = next;
    }
    if (cell != store->homeCell()) {
      const auto stored = manifold.textOf(cell, *store);
      std::uint64_t id{};
      const auto [end, error] =
          std::from_chars(stored.data(), stored.data() + stored.size(), id);
      if (error != std::errc{} || end != stored.data() + stored.size() ||
          id < 1 || id > visits.size()) {
        throw std::runtime_error("activity: invalid current visit");
      }
      selected = VisitId{id};
    }
  }
  if (const auto dim = manifold.dimensionNamed(kReferences, *store)) {
    for (auto cell = manifold.linked(store->homeCell(), *dim);
         cell != zigzag::noCell; cell = manifold.linked(cell, *dim)) {
      const auto visit = referenceOf(manifold.textOf(cell, *store));
      if (!find(visit))
        throw ActivityRecordUnreadable("d.activity-references: unknown visit " +
                                       std::to_string(visit.value));
      if (std::ranges::find(references, visit) == references.end())
        references.push_back(visit);
    }
  }
  if (const auto dim = manifold.dimensionNamed(kAnnotations, *store)) {
    for (auto cell = manifold.linked(store->homeCell(), *dim);
         cell != zigzag::noCell; cell = manifold.linked(cell, *dim)) {
      auto [visit, text] = annotationOf(manifold.textOf(cell, *store));
      if (!find(visit))
        throw ActivityRecordUnreadable(
            "d.activity-annotations: unknown visit " +
            std::to_string(visit.value));
      annotations.push_back({visit, std::move(text)});
    }
  }
}

void StoreActivityLog::appendRecord(const std::string_view dimension,
                                    const std::string_view text) {
  if (!store) return;
  auto at = store->latest();
  if (store->homeCell() == zigzag::noCell) at = store->sliceGenesis(at);
  auto dim = store->rebuildManifold(at).dimensionNamed(dimension, *store);
  if (!dim) {
    const auto made = store->makeDimension(at, dimension);
    at              = made.version;
    dim             = made.dim;
  }
  auto manifold = store->rebuildManifold(at);
  auto tail     = store->homeCell();
  for (auto next = manifold.linked(tail, *dim); next != zigzag::noCell;
       next      = manifold.linked(tail, *dim)) {
    tail = next;
  }
  at              = store->makeCell(at, text);
  const auto cell = store->cellRefOf(at);
  at = store->setLink(at, tail, *dim, zigzag::DimVector::POS, cell);
  std::filesystem::create_directories(directory);
  store->save(directory.string());
}

VisitId StoreActivityLog::append(Visit visit) {
  visit.id = VisitId{visits.size() + 1};
  appendRecord(kVisits, encode(visit));
  visits.push_back(std::move(visit));
  select(visits.back().id);
  return visits.back().id;
}

gleditor::cpp26::optional<const Visit &>
StoreActivityLog::find(const VisitId id) const {
  if (id.value == 0 || id.value > visits.size())
    return gleditor::cpp26::nullopt;
  return visits[id.value - 1];
}

std::vector<VisitId> StoreActivityLog::children(const VisitId parent) const {
  std::vector<VisitId> found;
  for (const auto &visit : visits) {
    if (visit.parent == parent) found.push_back(visit.id);
  }
  return found;
}

void StoreActivityLog::select(const VisitId id) {
  if (!find(id)) throw std::runtime_error("activity: unknown visit");
  if (selected == id) return;
  appendRecord(kCurrent, std::to_string(id.value));
  selected = id;
}

std::vector<WalkEntry> StoreActivityLog::walkEntries() const {
  std::vector<WalkEntry> result;
  result.reserve(visits.size());
  for (const auto &visit : visits) {
    WalkEntry entry{.visit      = visit,
                    .referenced = std::ranges::find(references, visit.id) !=
                                  references.end()};
    for (const auto &annotation : annotations)
      if (annotation.visit == visit.id)
        entry.annotations.push_back(annotation.text);
    result.push_back(std::move(entry));
  }
  return result;
}

std::expected<StoreActivityLog *, ActivityError>
StoreActivityLog::reference(const VisitId id) {
  if (!find(id)) return std::unexpected(ActivityError::UnknownVisit);
  if (std::ranges::find(references, id) != references.end()) return this;
  appendRecord(kReferences, "reference1 " + std::to_string(id.value));
  references.push_back(id);
  return this;
}

std::expected<StoreActivityLog *, ActivityError>
StoreActivityLog::annotate(const VisitId id, const std::string_view text) {
  if (!find(id)) return std::unexpected(ActivityError::UnknownVisit);
  if (text.empty()) return std::unexpected(ActivityError::EmptyAnnotation);
  appendRecord(kAnnotations, "annotation1 " + std::to_string(id.value) + " " +
                                 std::to_string(text.size()) + "\n" +
                                 std::string{text});
  annotations.push_back({id, std::string{text}});
  return this;
}

} // namespace xanadu
