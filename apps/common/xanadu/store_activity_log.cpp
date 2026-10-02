#include "common/xanadu/store_activity_log.hpp"

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

constexpr std::string_view kVisits  = "d.activity-visits";
constexpr std::string_view kCurrent = "d.activity-current";

std::string bytesOf(const DocumentId &id) { return id.str().substr(6); }

DocumentId parseId(const std::string &hex) {
  if (hex.size() != 32) throw std::runtime_error("activity: bad document id");
  auto id = DocumentId::parse(hex);
  if (!id) throw std::runtime_error("activity: bad document id");
  return *id;
}

std::int64_t indexOf(const std::optional<std::uint32_t> index) {
  return index ? static_cast<std::int64_t>(*index) : -1;
}

std::optional<std::uint32_t> cursorIndex(const std::int64_t value) {
  if (value < 0) return std::nullopt;
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("activity: cursor out of range");
  }
  return static_cast<std::uint32_t>(value);
}

std::string encode(const Visit &visit) {
  std::ostringstream out;
  const auto parent = visit.parent ? visit.parent->value : 0;
  out << "visit1 " << parent << ' ';
  std::visit(
      [&out](const auto &site) {
        using Site = std::decay_t<decltype(site)>;
        if constexpr (std::is_same_v<Site, DocumentSite>) {
          out << "doc " << bytesOf(site.store) << ' ' << site.version.str()
              << ' ' << site.range.start << ' ' << site.range.end << " 0 ";
        } else {
          out << "cell " << bytesOf(site.store) << ' ' << site.version.str()
              << ' ' << site.range.start << ' ' << site.range.end << ' '
              << site.cell << ' ';
        }
      },
      visit.target);
  out << static_cast<unsigned int>(visit.arrival) << ' '
      << static_cast<bool>(visit.link);
  if (visit.link) {
    const auto &link = *visit.link;
    out << ' ' << bytesOf(link.key.authority) << ' ' << link.key.id << ' '
        << static_cast<unsigned int>(link.active) << ' '
        << indexOf(link.left.member) << ' ' << indexOf(link.left.occurrence)
        << ' ' << indexOf(link.right.member) << ' '
        << indexOf(link.right.occurrence) << ' '
        << (link.origin ? link.origin->value : 0);
  }
  return out.str();
}

Visit decode(const std::string &data) {
  std::istringstream in(data);
  std::string signature, kind, storeId, version;
  std::uint64_t parent{}, cell{}, linkId{}, origin{};
  std::uint32_t start{}, end{};
  unsigned int arrival{}, linked{}, side{};
  if (!(in >> signature >> parent >> kind >> storeId >> version >> start >>
        end >> cell >> arrival >> linked) ||
      signature != "visit1" || (kind != "doc" && kind != "cell") ||
      arrival > 1 || linked > 1 || end < start) {
    throw std::runtime_error("activity: malformed visit");
  }
  const auto siteStore = parseId(storeId);
  const auto at        = MicroversionId::parse(version);
  Visit visit;
  if (parent) visit.parent = VisitId{parent};
  if (kind == "doc") {
    visit.target = DocumentSite{siteStore, at, {start, end}};
  } else {
    if (cell > std::numeric_limits<zigzag::CellRef>::max()) {
      throw std::runtime_error("activity: cell out of range");
    }
    visit.target = CellSite{
        siteStore, at, static_cast<zigzag::CellRef>(cell), {start, end}};
  }
  visit.arrival = static_cast<Arrival>(arrival);
  if (linked) {
    std::string authority;
    std::int64_t lm{}, lo{}, rm{}, ro{};
    if (!(in >> authority >> linkId >> side >> lm >> lo >> rm >> ro >>
          origin) ||
        side > 1 || linkId > std::numeric_limits<zigzag::CellRef>::max()) {
      throw std::runtime_error("activity: malformed link context");
    }
    visit.link = LinkVisitContext{
        .key    = {parseId(authority), static_cast<zigzag::CellRef>(linkId)},
        .active = static_cast<LinkSide>(side),
        .left   = {cursorIndex(lm), cursorIndex(lo)},
        .right  = {cursorIndex(rm), cursorIndex(ro)},
        .origin =
            origin ? std::optional<VisitId>{VisitId{origin}} : std::nullopt};
  }
  std::string extra;
  if (in >> extra) throw std::runtime_error("activity: trailing visit data");
  return visit;
}

} // namespace

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
  appendRecord(kCurrent, std::to_string(id.value));
  selected = id;
}

} // namespace xanadu
