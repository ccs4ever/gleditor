/**
 * @file reading_place.cpp
 * @brief Where a reader was when they stopped, kept as cells in the
 *        reader's activity store so a session resumes there.
 */
#include "reading_place.hpp"

#include <cstdlib>
#include <string_view>
#include <utility>

#include "store.hpp"
#include "zigzag/manifold.hpp"

namespace xanadu {

namespace {

// The rank a place hangs from home on, and the ranks hanging from a place.
constexpr std::string_view kPlaces    = "d.places";
constexpr std::string_view kDocuments = "d.documents";
constexpr std::string_view kVersion   = "d.version";
constexpr std::string_view kCaret     = "d.caret";
constexpr std::string_view kAnchor    = "d.anchor";
constexpr std::string_view kCamera    = "d.camera";
constexpr std::string_view kZigzag    = "d.zigzag";
constexpr std::string_view kActive    = "d.active";

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

} // namespace

MicroversionId recordPlace(Store &store, const ReadingPlace &place) {
  Writer out(store);
  const auto places    = out.dimension(kPlaces);
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
  return out.at;
}

std::optional<ReadingPlace> latestPlace(const Store &store) {
  const auto head = store.latest();
  if (head.isZero() || zigzag::noCell == store.homeCell()) {
    return std::nullopt;
  }
  const auto manifold = store.rebuildManifold(head);
  const auto dim      = [&](const std::string_view name) {
    return manifold.dimensionNamed(name, store);
  };
  const auto places = dim(kPlaces);
  if (!places) {
    return std::nullopt;
  }
  auto here = store.homeCell();
  for (auto next = manifold.linked(here, *places); zigzag::noCell != next;
       next      = manifold.linked(here, *places)) {
    here = next;
  }
  if (here == store.homeCell()) {
    return std::nullopt;
  }

  ReadingPlace place;
  const auto documents = dim(kDocuments);
  const auto version   = dim(kVersion);
  const auto caret     = dim(kCaret);
  const auto anchor    = dim(kAnchor);
  const auto active    = dim(kActive);
  const auto along     = [&](const zigzag::CellRef from,
                             const std::optional<zigzag::DimRef> onto) {
    return onto ? manifold.linked(from, *onto) : zigzag::noCell;
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
                             ? zigzag::noCell
                             : manifold.linked(head, *zigzagDim);
      if (zigzag::noCell != focus) {
        place.zigzagFocus = manifold.asInt64(focus).value_or(0);
        const auto keys   = manifold.linked(focus, *zigzagDim);
        place.zigzagHasKeyboard =
            zigzag::noCell != keys && manifold.asBool(keys).value_or(false);
      }
    }
  }
  return place;
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
