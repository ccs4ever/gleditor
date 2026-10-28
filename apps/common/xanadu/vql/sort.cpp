#include "common/xanadu/vql/vql_engine.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace xanadu::vql {

std::vector<zigzag::CellRef> VQLEngine::sortInto(const FunctionInvocation &fn) {
  if (fn.args.size() != 2 || fn.argNames.size() != 2 ||
      fn.argNames[1] != "by") {
    throw std::runtime_error(
        "sort_into expects a rank path and a named by: key expression");
  }

  const auto *targetPath =
      std::get_if<std::shared_ptr<PathExpression>>(&fn.args.front().kind);
  if (!targetPath || !*targetPath || (*targetPath)->steps.empty() ||
      !std::holds_alternative<SignedDimensionStep>(
          (*targetPath)->steps.back().selector)) {
    throw std::runtime_error(
        "sort_into requires a path ending in the rank dimension to reorder");
  }
  const auto &dimensionStep =
      std::get<SignedDimensionStep>((*targetPath)->steps.back().selector);
  const auto dimension = resolveDimension(dimensionStep.dimName);
  auto cells           = evaluatePath(**targetPath);
  if (cells.size() < 2) return cells;

  auto &arena = core_->arena();
  std::unordered_set<zigzag::CellRef> selected;
  selected.reserve(cells.size());
  for (const auto cell : cells) {
    arena.consumeWork();
    if (!selected.insert(cell).second) {
      throw std::runtime_error("sort_into target repeats a cell");
    }
  }

  auto head = cells.front();
  std::unordered_set<zigzag::CellRef> seen{head};
  while (const auto previous =
             arena.linkedMinting(head, dimension, zigzag::DimVector::NEG)) {
    arena.consumeWork();
    if (!seen.insert(*previous).second) {
      throw std::runtime_error("sort_into cannot reorder a cyclic rank");
    }
    head = *previous;
  }
  std::vector<zigzag::CellRef> rank;
  rank.push_back(head);
  seen.clear();
  seen.insert(head);
  while (const auto next = arena.linkedMinting(rank.back(), dimension,
                                               zigzag::DimVector::POS)) {
    arena.consumeWork();
    if (!seen.insert(*next).second) {
      throw std::runtime_error("sort_into cannot reorder a cyclic rank");
    }
    rank.push_back(*next);
  }

  std::vector<std::size_t> positions;
  positions.reserve(cells.size());
  for (const auto cell : cells) {
    const auto found = std::ranges::find(rank, cell);
    if (found == rank.end()) {
      throw std::runtime_error("sort_into targets do not share one rank");
    }
    positions.push_back(static_cast<std::size_t>(found - rank.begin()));
  }
  auto sortedPositions = positions;
  std::ranges::sort(sortedPositions);
  for (std::size_t i = 1; i < sortedPositions.size(); ++i) {
    if (sortedPositions[i] != sortedPositions[i - 1] + 1) {
      throw std::runtime_error(
          "sort_into target must be one contiguous section of a rank");
    }
  }
  const auto begin = sortedPositions.front();
  const auto end   = sortedPositions.back();
  const std::optional<zigzag::CellRef> before =
      begin == 0 ? std::nullopt
                 : std::optional<zigzag::CellRef>{rank[begin - 1]};
  const std::optional<zigzag::CellRef> after =
      end + 1 == rank.size() ? std::nullopt
                             : std::optional<zigzag::CellRef>{rank[end + 1]};

  using Key = std::variant<std::int64_t, double, std::string>;
  struct KeyedCell {
    zigzag::CellRef cell;
    Key key;
  };
  std::vector<KeyedCell> keyed;
  keyed.reserve(cells.size());
  bool numericKeys = false;
  bool stringKeys  = false;
  const auto *keyLambda =
      std::get_if<std::shared_ptr<PathLambda>>(&fn.args[1].kind);
  const auto *keyPath =
      std::get_if<std::shared_ptr<PathExpression>>(&fn.args[1].kind);

  for (const auto cell : cells) {
    arena.consumeWork();
    std::optional<zigzag::vortex::CellValue> value;
    if ((keyLambda && *keyLambda) || (keyPath && *keyPath)) {
      const auto results = keyLambda && *keyLambda
                               ? executeLambda(**keyLambda, cell)
                               : evaluatePath(**keyPath, {cell});
      if (results.size() != 1) {
        throw std::runtime_error(
            "sort_into key must return exactly one scalar");
      }
      value = core_->render(results.front());
    } else {
      value = evaluateValueExpr(fn.args[1], cell);
    }
    if (!value) throw std::runtime_error("sort_into key has no value");
    if (const auto number = std::get_if<double>(&*value)) {
      if (std::isnan(*number))
        throw std::runtime_error("sort_into cannot order NaN keys");
      numericKeys = true;
      keyed.push_back({cell, *number});
    } else if (const auto integer = std::get_if<std::int64_t>(&*value)) {
      numericKeys = true;
      keyed.push_back({cell, *integer});
    } else if (const auto text = std::get_if<std::string>(&*value)) {
      stringKeys = true;
      keyed.push_back({cell, *text});
    } else {
      throw std::runtime_error(
          "sort_into supports numeric and string keys only");
    }
  }
  if (numericKeys && stringKeys) {
    throw std::runtime_error("sort_into cannot mix numeric and string keys");
  }

  std::ranges::stable_sort(keyed, [](const KeyedCell &left,
                                     const KeyedCell &right) {
    return std::visit(
        [](const auto &lhs, const auto &rhs) {
          using L = std::decay_t<decltype(lhs)>;
          using R = std::decay_t<decltype(rhs)>;
          if constexpr (std::same_as<L, R>)
            return lhs < rhs;
          else if constexpr (std::is_arithmetic_v<L> && std::is_arithmetic_v<R>)
            return static_cast<long double>(lhs) <
                   static_cast<long double>(rhs);
          else
            return false;
        },
        left.key, right.key);
  });

  const auto link = [&](const zigzag::CellRef from,
                        const zigzag::DimVector direction,
                        const std::optional<zigzag::CellRef> to) {
    const auto result = arena.link(from, dimension, direction, to);
    if (!result) {
      throw std::runtime_error("sort_into could not relink the rank");
    }
  };
  for (std::size_t i = 0; i < keyed.size(); ++i) {
    arena.consumeWork();
    const auto previous =
        i == 0 ? before : std::optional<zigzag::CellRef>{keyed[i - 1].cell};
    const auto next = i + 1 == keyed.size()
                          ? after
                          : std::optional<zigzag::CellRef>{keyed[i + 1].cell};
    link(keyed[i].cell, zigzag::DimVector::NEG, previous);
    link(keyed[i].cell, zigzag::DimVector::POS, next);
  }
  cells.clear();
  for (const auto &entry : keyed) cells.push_back(entry.cell);
  return cells;
}

} // namespace xanadu::vql
