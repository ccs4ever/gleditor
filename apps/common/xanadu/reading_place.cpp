/**
 * @file reading_place.cpp
 * @brief Where a reader was when they stopped, kept as cells in the
 *        reader's activity store so a session resumes there.
 */
#include "reading_place.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "store.hpp"
#include "zigzag/manifold.hpp"

namespace xanadu {

namespace {

// The rank a place hangs from home on, and the ranks hanging from a place.
constexpr std::string_view kPlaces       = "d.places";
constexpr std::string_view kClosedPlaces = "d.closed-places";
constexpr std::string_view kDocuments    = "d.documents";
constexpr std::string_view kVersion      = "d.version";
constexpr std::string_view kCaret        = "d.caret";
constexpr std::string_view kAnchor       = "d.anchor";
constexpr std::string_view kCamera       = "d.camera";
constexpr std::string_view kZigzag       = "d.zigzag";
constexpr std::string_view kActive       = "d.active";
constexpr std::string_view kLink         = "d.selected-link";

// A cursor with nothing chosen is kept as -1, which no index can be.
std::int64_t cursorValue(const std::optional<std::uint32_t> index) {
  return index ? std::int64_t{*index} : -1;
}

std::optional<std::uint32_t>
cursorIndex(const std::optional<std::int64_t> value) {
  if (!value || *value < 0 ||
      *value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

/// Appends operations to one store, keeping the state they build on.
class Writer {
public:
  explicit Writer(Store &aStore) : store(aStore), at(aStore.latest()) {
    if (zigzag::noCell == store.homeCell()) {
      at = store.sliceGenesis(at);
    }
  }

  zigzag::DimRef dimension(const std::string_view name) {
    if (const auto found =
            store.rebuildManifold(at).dimensionNamed(name, store)) {
      return *found;
    }
    const auto minted = store.makeDimension(at, name);
    at                = minted.version;
    return minted.dim;
  }

  zigzag::CellRef text(const std::string_view content) {
    at = store.makeCell(at, content);
    return store.cellRefOf(at);
  }

  template <typename Value> zigzag::CellRef scalar(const Value value) {
    at = store.makeScalarCell(at, value);
    return store.cellRefOf(at);
  }

  void link(const zigzag::CellRef from, const zigzag::DimRef dim,
            const zigzag::CellRef to) {
    at = store.setLink(at, from, dim, zigzag::DimVector::POS, to);
  }

  /// The last cell of @p dim's rank from @p from.
  [[nodiscard]] zigzag::CellRef tail(const zigzag::CellRef from,
                                     const zigzag::DimRef dim) const {
    const auto manifold = store.rebuildManifold(at);
    auto cell           = from;
    for (auto next = manifold.linked(cell, dim); zigzag::noCell != next;
         next      = manifold.linked(cell, dim)) {
      cell = next;
    }
    return cell;
  }

  Store &store;
  MicroversionId at;
};

/// The link a place's d.link rank from @p authority names; nothing for a
/// rank too short to name one or an authority that is not a document id.
std::optional<LinkVisitContext> linkOf(const zigzag::Manifold &manifold,
                                       const Store &store,
                                       const zigzag::OptionalCell authority,
                                       const zigzag::DimRef dim) {
  if (zigzag::noCell == authority) {
    return std::nullopt;
  }
  const auto id = DocumentId::parse(manifold.textOf(authority, store));
  if (!id) {
    return std::nullopt;
  }
  std::array<std::optional<std::int64_t>, 7> values{};
  auto cell = authority;
  for (auto &value : values) {
    cell = manifold.linked(cell, dim);
    if (zigzag::noCell == cell) {
      return std::nullopt;
    }
    value = manifold.asInt64(cell);
  }
  const auto [linkId, side, lm, lo, rm, ro, origin] = values;
  if (!linkId || *linkId < 0 ||
      *linkId > std::numeric_limits<zigzag::CellRef>::max() || !side ||
      (*side != 0 && *side != 1)) {
    return std::nullopt;
  }
  return LinkVisitContext{.key = {*id, static_cast<zigzag::CellRef>(*linkId)},
                          .active = static_cast<LinkSide>(*side),
                          .left   = {cursorIndex(lm), cursorIndex(lo)},
                          .right  = {cursorIndex(rm), cursorIndex(ro)},
                          .origin =
                              origin && *origin > 0
                                  ? std::optional<VisitId>{VisitId{
                                        static_cast<std::uint64_t>(*origin)}}
                                  : std::nullopt};
}

} // namespace

namespace {

MicroversionId writePlace(Store &store, const ReadingPlace &place,
                          const std::string_view rank) {
  Writer out(store);
  const auto places    = out.dimension(rank);
  const auto documents = out.dimension(kDocuments);
  const auto version   = out.dimension(kVersion);
  const auto caret     = out.dimension(kCaret);
  const auto anchor    = out.dimension(kAnchor);
  const auto camera    = out.dimension(kCamera);
  const auto zigzagDim = out.dimension(kZigzag);
  const auto active    = out.dimension(kActive);

  const auto previous = out.tail(store.homeCell(), places);
  const auto here     = out.text("place");
  out.link(previous, places, here);

  auto last = here;
  for (std::size_t i = 0; i < place.documents.size(); ++i) {
    const auto &document = place.documents[i];
    const auto cell      = out.text(document.storePath);
    out.link(last, documents, cell);
    out.link(cell, version, out.text(document.version));
    out.link(cell, caret, out.scalar(std::int64_t{document.caret}));
    out.link(cell, anchor, out.scalar(std::int64_t{document.anchor}));
    if (place.active == i) {
      out.link(here, active, cell);
    }
    last = cell;
  }
  if (place.camera) {
    last = here;
    for (const double value : *place.camera) {
      const auto cell = out.scalar(value);
      out.link(last, camera, cell);
      last = cell;
    }
  }
  if (!place.zigzagStore.empty()) {
    const auto slice = out.text(place.zigzagStore);
    const auto head  = out.text(place.zigzagVersion);
    const auto focus = out.scalar(place.zigzagFocus);
    out.link(here, zigzagDim, slice);
    out.link(slice, zigzagDim, head);
    out.link(head, zigzagDim, focus);
    out.link(focus, zigzagDim, out.scalar(place.zigzagHasKeyboard));
  }
  if (place.link) {
    const auto linkDim = out.dimension(kLink);
    const auto &link   = *place.link;
    const std::array<std::int64_t, 7> values{
        static_cast<std::int64_t>(link.key.id),
        static_cast<std::int64_t>(link.active),
        cursorValue(link.left.member),
        cursorValue(link.left.occurrence),
        cursorValue(link.right.member),
        cursorValue(link.right.occurrence),
        link.origin ? static_cast<std::int64_t>(link.origin->value) : 0};
    last = out.text(link.key.authority.str());
    out.link(here, linkDim, last);
    for (const auto value : values) {
      const auto cell = out.scalar(value);
      out.link(last, linkDim, cell);
      last = cell;
    }
  }
  return out.at;
}

ReadingPlace readPlace(const Store &store, const zigzag::Manifold &manifold,
                       const zigzag::CellRef here) {
  const auto dim = [&](const std::string_view name) {
    return manifold.dimensionNamed(name, store);
  };
  ReadingPlace place;
  const auto documents = dim(kDocuments);
  const auto version   = dim(kVersion);
  const auto caret     = dim(kCaret);
  const auto anchor    = dim(kAnchor);
  const auto active    = dim(kActive);
  const auto along     = [&](const zigzag::CellRef from,
                         const std::optional<zigzag::DimRef> onto) {
    return onto ? manifold.linked(from, *onto) : zigzag::OptionalCell{};
  };
  const auto activeCell = along(here, active);
  for (auto cell = along(here, documents); zigzag::noCell != cell;
       cell      = along(cell, documents)) {
    DocumentPlace document;
    document.storePath = manifold.textOf(cell, store);
    if (const auto v = along(cell, version); zigzag::noCell != v) {
      document.version = manifold.textOf(v, store);
    }
    const auto number = [&](const zigzag::CellRef at) {
      return zigzag::noCell == at ? std::nullopt : manifold.asInt64(at);
    };
    document.caret =
        static_cast<std::uint32_t>(number(along(cell, caret)).value_or(0));
    document.anchor = static_cast<std::uint32_t>(
        number(along(cell, anchor)).value_or(document.caret));
    if (cell == activeCell) {
      place.active = place.documents.size();
    }
    place.documents.push_back(std::move(document));
  }
  if (const auto camera = dim(kCamera)) {
    std::array<double, 4> values{};
    auto cell  = here;
    bool whole = true;
    for (auto &value : values) {
      cell = manifold.linked(cell, *camera);
      // A rank that ends early is no camera, and noCell has no value to read.
      const auto found =
          zigzag::noCell == cell ? std::nullopt : manifold.asDouble(cell);
      whole = whole && found.has_value();
      value = found.value_or(0.0);
    }
    if (whole) {
      place.camera = values;
    }
  }
  if (const auto zigzagDim = dim(kZigzag)) {
    const auto slice = manifold.linked(here, *zigzagDim);
    if (zigzag::noCell != slice) {
      place.zigzagStore = manifold.textOf(slice, store);
      const auto head   = manifold.linked(slice, *zigzagDim);
      if (zigzag::noCell != head) {
        place.zigzagVersion = manifold.textOf(head, store);
      }
      const auto focus = zigzag::noCell == head
                             ? zigzag::OptionalCell{}
                             : manifold.linked(head, *zigzagDim);
      if (zigzag::noCell != focus) {
        place.zigzagFocus = manifold.asInt64(focus).value_or(0);
        const auto keys   = manifold.linked(focus, *zigzagDim);
        place.zigzagHasKeyboard =
            zigzag::noCell != keys && manifold.asBool(keys).value_or(false);
      }
    }
  }
  if (const auto linkDim = dim(kLink)) {
    place.link =
        linkOf(manifold, store, manifold.linked(here, *linkDim), *linkDim);
  }
  return place;
}

} // namespace

MicroversionId recordPlace(Store &store, const ReadingPlace &place) {
  return writePlace(store, place, kPlaces);
}

MicroversionId recordClosedPlace(Store &store, const ReadingPlace &place) {
  if (place.documents.size() != 1 || place.active != 0) {
    throw std::invalid_argument(
        "a closed place needs exactly one active document");
  }
  return writePlace(store, place, kClosedPlaces);
}

std::optional<ReadingPlace> latestPlace(const Store &store) {
  if (store.latest().isZero() || zigzag::noCell == store.homeCell())
    return std::nullopt;
  const auto manifold = store.rebuildManifold(store.latest());
  const auto places   = manifold.dimensionNamed(kPlaces, store);
  if (!places) return std::nullopt;
  auto here = store.homeCell();
  for (auto next = manifold.linked(here, *places); next != zigzag::noCell;
       next      = manifold.linked(here, *places)) {
    here = next;
  }
  if (here == store.homeCell()) return std::nullopt;
  return readPlace(store, manifold, here);
}

std::optional<ReadingPlace> closedPlaceFor(const Store &store,
                                           const std::string_view path) {
  if (store.latest().isZero() || zigzag::noCell == store.homeCell())
    return std::nullopt;
  const auto manifold  = store.rebuildManifold(store.latest());
  const auto places    = manifold.dimensionNamed(kClosedPlaces, store);
  const auto documents = manifold.dimensionNamed(kDocuments, store);
  if (!places || !documents) return std::nullopt;
  std::optional<ReadingPlace> found;
  auto here = manifold.linked(store.homeCell(), *places);
  for (std::size_t left = manifold.cellCount();
       here != zigzag::noCell && left > 0; --left) {
    const auto document = manifold.linked(here, *documents);
    if (document != zigzag::noCell &&
        manifold.textOf(document, store) == path) {
      found = readPlace(store, manifold, here);
    }
    here = manifold.linked(here, *places);
  }
  return found;
}

std::filesystem::path activityDirectory() {
  namespace fs = std::filesystem;
  if (const char *xdgData = std::getenv("XDG_DATA_HOME");
      nullptr != xdgData && '\0' != *xdgData) {
    return fs::path(xdgData) / "xudu" / "activity";
  }
  if (const char *home = std::getenv("HOME");
      nullptr != home && '\0' != *home) {
    return fs::path(home) / ".local" / "share" / "xudu" / "activity";
  }
  return fs::temp_directory_path() / "xudu" / "activity";
}

} // namespace xanadu
