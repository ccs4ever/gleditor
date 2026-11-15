#include "common/xanadu/vortex/mesh_result.hpp"

#include <concepts>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"

namespace zigzag::vortex::mesh {
namespace {

using xanadu::cellRelocationDimension;
using xanadu::cellRelocationMarker;
using xanadu::ValueKind;

constexpr std::string_view itemsName{"d.mesh.items"};
constexpr std::string_view valueName{"d.mesh.value"};
constexpr std::string_view countName{"d.mesh.count"};

template <typename Graph> class Reader {
public:
  Reader(const Graph &graph, const xanadu::SpanReader *spanReader,
         ResultReadLimits limits)
      : graph_(graph), spanReader_(spanReader), limits_(limits) {}

  std::expected<std::string, std::string> text(CellRef ref) {
    if (!graph_.contains(ref))
      return std::unexpected("mesh result: missing metadata cell");
    std::size_t length = 0;
    for (const auto &span : graph_.contentOf(ref)) {
      if (!charge())
        return std::unexpected("mesh result: metadata work budget exceeded");
      const auto remaining = limits_.maxMetadataTextBytes - textBytes_;
      if (span.length > remaining || length > remaining - span.length)
        return std::unexpected("mesh result: metadata text budget exceeded");
      length += static_cast<std::size_t>(span.length);
    }
    if (!chargeWork(length))
      return std::unexpected("mesh result: metadata work budget exceeded");
    textBytes_ += length;
    if constexpr (std::same_as<Graph, ArenaManifold>)
      return graph_.textOf(ref, spanReader_);
    else
      return graph_.textOf(ref, *spanReader_);
  }

  std::expected<std::optional<DimRef>, std::string>
  optionalDimension(CellRef owner, std::string_view name) {
    std::optional<DimRef> match;
    for (const auto &edge : graph_.dimensionsOf(owner)) {
      if (!charge())
        return std::unexpected("mesh result: metadata work budget exceeded");
      auto label = text(edge.dim);
      if (!label) return std::unexpected(label.error());
      if (*label != name) continue;
      if (match)
        return std::unexpected("mesh result: duplicate schema dimension");
      match = edge.dim;
    }
    return match;
  }

  std::expected<CellRef, std::string> occurrence(CellRef wrapper) {
    if (!graph_.contains(wrapper) ||
        graph_.valueKindOf(wrapper) != ValueKind::OpHandle)
      return std::unexpected("mesh result: value wrapper is not an OpHandle");
    const auto target = graph_.handleTarget(wrapper);
    if (!target || !graph_.contains(*target))
      return std::unexpected("mesh result: value wrapper target is absent");
    auto relocation = optionalDimension(wrapper, cellRelocationDimension);
    if (!relocation) return std::unexpected(relocation.error());
    if (!*relocation)
      return std::unexpected("mesh result: occurrence marker is missing");
    if (!charge())
      return std::unexpected("mesh result: metadata work budget exceeded");
    const auto marker = graph_.linked(wrapper, **relocation, DimVector::POS);
    if (!marker || graph_.linked(wrapper, **relocation, DimVector::NEG) ||
        graph_.linked(*marker, **relocation, DimVector::POS) ||
        graph_.linked(*marker, **relocation, DimVector::NEG) != wrapper)
      return std::unexpected("mesh result: malformed occurrence marker rank");
    auto markerText = text(*marker);
    if (!markerText) return std::unexpected(markerText.error());
    if (*markerText != cellRelocationMarker)
      return std::unexpected("mesh result: invalid occurrence marker");
    return *target;
  }

  bool chargeWork(std::size_t units = 1) {
    if constexpr (std::same_as<Graph, ArenaManifold>)
      for (std::size_t i = 0; i < units; ++i) graph_.consumeWork();
    return true;
  }

  bool charge() {
    if (relations_ == limits_.maxMetadataRelations) return false;
    ++relations_;
    return chargeWork();
  }

private:
  const Graph &graph_;
  const xanadu::SpanReader *spanReader_;
  ResultReadLimits limits_;
  std::size_t relations_{0};
  std::size_t textBytes_{0};
};

template <typename Graph>
std::expected<std::vector<CellRef>, std::string>
readCells(const Graph &graph, CellRef root,
          const xanadu::SpanReader *spanReader, ResultReadLimits limits) {
  if (!graph.contains(root))
    return std::unexpected("mesh result: root is absent");
  Reader<Graph> reader{graph, spanReader, limits};
  auto label = reader.text(root);
  if (!label) return std::unexpected(label.error());
  if (*label != "mesh:result/v2")
    return std::unexpected("mesh result: unsupported result root version "
                           "(expected mesh:result/v2)");

  auto count = reader.optionalDimension(root, countName);
  if (!count) return std::unexpected(count.error());
  if (!*count)
    return std::unexpected("mesh result: item count dimension is missing");
  const auto countCell = graph.linked(root, **count, DimVector::POS);
  if (!countCell || graph.linked(root, **count, DimVector::NEG) ||
      graph.linked(*countCell, **count, DimVector::NEG) != root ||
      graph.linked(*countCell, **count, DimVector::POS))
    return std::unexpected("mesh result: missing or malformed item count");
  const auto countValue = graph.asInt64(*countCell);
  if (!countValue || *countValue < 0 ||
      static_cast<std::uint64_t>(*countValue) > limits.maxItems)
    return std::unexpected("mesh result: item count limit or type failure");
  const auto expectedItems = static_cast<std::size_t>(*countValue);
  if (expectedItems > limits.maxMetadataRelations ||
      !reader.chargeWork(expectedItems))
    return std::unexpected("mesh result: item count work budget exceeded");

  auto items = reader.optionalDimension(root, itemsName);
  if (!items) return std::unexpected(items.error());
  if (expectedItems != 0 && !*items)
    return std::unexpected("mesh result: item dimension is missing");

  if (!reader.charge())
    return std::unexpected("mesh result: metadata work budget exceeded");
  CellRef previous = root;
  auto row         = *items ? graph.linked(root, **items, DimVector::POS)
                            : std::optional<CellRef>{};
  if (*items && graph.linked(root, **items, DimVector::NEG))
    return std::unexpected("mesh result: malformed item rank origin");
  if (expectedItems == 0 && row)
    return std::unexpected("mesh result: rows exceed declared item count");

  std::vector<CellRef> result;
  result.reserve(expectedItems);
  std::unordered_set<CellRef> rows;
  std::unordered_set<CellRef> wrappers;
  for (std::size_t index = 0; index < expectedItems; ++index) {
    if (!reader.charge())
      return std::unexpected("mesh result: metadata work budget exceeded");
    if (!row || !graph.contains(*row))
      return std::unexpected("mesh result: item rank is shorter than declared");
    if (!rows.insert(*row).second)
      return std::unexpected("mesh result: row cycle or duplicate row");
    if (graph.linked(*row, **items, DimVector::NEG) != previous)
      return std::unexpected("mesh result: incomplete item rank");
    auto value = reader.optionalDimension(*row, valueName);
    if (!value) return std::unexpected(value.error());
    if (!*value)
      return std::unexpected("mesh result: row value dimension is missing");
    auto rowItems = reader.optionalDimension(*row, itemsName);
    if (!rowItems) return std::unexpected(rowItems.error());
    if (!*rowItems || **rowItems != **items)
      return std::unexpected(
          "mesh result: row uses a different item dimension");
    const auto wrapper = graph.linked(*row, **value, DimVector::POS);
    if (!wrapper || graph.linked(*row, **value, DimVector::NEG) ||
        graph.linked(*wrapper, **value, DimVector::NEG) != *row ||
        graph.linked(*wrapper, **value, DimVector::POS))
      return std::unexpected("mesh result: row must have one value occurrence");
    if (!wrappers.insert(*wrapper).second)
      return std::unexpected("mesh result: duplicate occurrence wrapper");
    auto target = reader.occurrence(*wrapper);
    if (!target) return std::unexpected(target.error());
    result.push_back(*target);
    previous = *row;
    row      = graph.linked(*row, **items, DimVector::POS);
  }
  if (row)
    return std::unexpected("mesh result: rows exceed declared item count");
  return result;
}

template <typename Graph>
std::expected<CellRef, std::string>
occurrenceTarget(const Graph &graph, CellRef wrapper,
                 const xanadu::SpanReader *spanReader,
                 ResultReadLimits limits) {
  Reader<Graph> reader{graph, spanReader, limits};
  return reader.occurrence(wrapper);
}

template <typename Function> auto catchingReader(Function &&function) {
  try {
    return function();
  } catch (const std::exception &error) {
    using Result = decltype(function());
    return Result{std::unexpected(std::string(error.what()))};
  }
}

} // namespace

std::expected<CellRef, std::string>
readOccurrenceTarget(const ArenaManifold &arena, CellRef wrapper,
                     ResultReadLimits limits) {
  return catchingReader(
      [&] { return occurrenceTarget(arena, wrapper, nullptr, limits); });
}

std::expected<CellRef, std::string>
readOccurrenceTarget(const Manifold &manifold, CellRef wrapper,
                     const xanadu::SpanReader &reader,
                     ResultReadLimits limits) {
  return catchingReader(
      [&] { return occurrenceTarget(manifold, wrapper, &reader, limits); });
}

std::expected<std::vector<CellRef>, std::string>
readResultCells(const ArenaManifold &arena, CellRef root,
                ResultReadLimits limits) {
  return catchingReader(
      [&] { return readCells(arena, root, nullptr, limits); });
}

std::expected<std::vector<CellRef>, std::string>
readResultCells(const Manifold &manifold, CellRef root,
                const xanadu::SpanReader &reader, ResultReadLimits limits) {
  return catchingReader(
      [&] { return readCells(manifold, root, &reader, limits); });
}

std::expected<std::vector<CellRef>, std::string>
readResultCells(const EvaluatedValue &value, ResultReadLimits limits) {
  if (!value.context)
    return std::unexpected("mesh result: evaluation context is absent");
  return readResultCells(value.context->arena, value.root, limits);
}

} // namespace zigzag::vortex::mesh
