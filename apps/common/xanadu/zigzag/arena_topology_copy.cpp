#include "common/xanadu/zigzag/arena_topology_copy.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <unordered_map>

namespace zigzag {

bool EdgeSelection::selects(const DirectedDim edge) const noexcept {
  if (std::ranges::find(add, edge) != add.end()) return true;
  return everything && std::ranges::find(remove, edge) == remove.end();
}

std::string toString(const TopologyCopyRefused &refused) {
  return std::format("cell {} cannot be copied exactly: {}", refused.cell,
                     toString(refused.reason));
}

namespace {

constexpr std::array<DimVector, 2> bothDirections{DimVector::POS,
                                                  DimVector::NEG};

struct Edge {
  DimRef dim{noCell};
  DimVector dir{DimVector::POS};
  std::size_t target{0};
};

/// Everything needed to mint one copy, read before anything is minted.
struct Node {
  CellRef source{noCell};
  std::vector<xanadu::PrimediaSpan> run;
  std::optional<std::uint32_t> quoteSpace;
  xanadu::ValueKind kind{xanadu::ValueKind::None};
  std::uint64_t bits{0};
  std::vector<Edge> edges;
};

[[nodiscard]] bool isHandle(const xanadu::ValueKind kind) noexcept {
  return kind == xanadu::ValueKind::OpHandle ||
         kind == xanadu::ValueKind::ExternRef ||
         kind == xanadu::ValueKind::ForeignOpHandle;
}

/// The typed value of @p source, read where it lives: a proxy's is its
/// foreign cell's, since the proxy's own slot carries none.
void readValue(const ArenaManifold &arena, const CellRef source, Node &node) {
  const CellSlot *held = nullptr;
  if (const auto foreign = arena.foreignOf(source)) {
    if (const auto space = arena.spaceAt(foreign->space);
        space && space->manifold)
      if (const auto theirs = space->manifold->slot(foreign->index))
        held = &*theirs;
  } else if (const auto own = arena.slot(source)) {
    held = &*own;
  }
  if (held == nullptr) return;
  node.kind = static_cast<xanadu::ValueKind>(held->valueKind);
  node.bits = held->valueBits;
}

/// The attached space whose store @p store is, if any: the one a quote cell
/// must name so its spans keep reading from that store's scrolls.
std::optional<std::uint32_t> spaceOfStore(const ArenaManifold &arena,
                                          const xanadu::Store *store) {
  if (store == nullptr) return std::nullopt;
  for (std::uint32_t id = 1; id <= arena.spaceCount(); ++id)
    if (const auto space = arena.spaceAt(id); space && space->store == store)
      return id;
  return std::nullopt;
}

} // namespace

std::expected<std::vector<CellRef>, TopologyCopyRefused>
copyTopology(ArenaManifold &arena, const std::span<const CellRef> subjects,
             const EdgeSelection &selection, const DimRef cloneDim) {
  const auto refuse = [](const TopologyCopyRefusal why, const CellRef cell) {
    return std::unexpected{TopologyCopyRefused{.reason = why, .cell = cell}};
  };

  // A wildcard can reach a dimension no rule names, so the candidates are the
  // arena's named dimensions plus whatever each visited cell links on (a
  // core-minted dimension has no arena name).
  std::vector<DimRef> named;
  if (selection.everything) named = arena.dimensions();
  std::vector<DimRef> fixed;
  for (const auto &edge : selection.add)
    if (std::ranges::find(fixed, edge.dim) == fixed.end())
      fixed.push_back(edge.dim);

  std::vector<Node> nodes;
  std::unordered_map<CellRef, std::size_t> indexOf;
  std::vector<std::size_t> roots;

  const auto discover = [&](const CellRef source)
      -> std::expected<std::size_t, TopologyCopyRefused> {
    if (const auto found = indexOf.find(source); found != indexOf.end())
      return found->second;
    arena.consumeWork();
    if (!arena.contains(source))
      return refuse(TopologyCopyRefusal::UnknownSubject, source);
    if (arena.isQuoteOccurrence(source))
      return refuse(TopologyCopyRefusal::QuoteOccurrence, source);
    Node node{.source = source};
    readValue(arena, source, node);
    if (isHandle(node.kind))
      return refuse(TopologyCopyRefusal::HandleValue, source);
    const CellRef authority =
        arena.cloneMaster(source, cloneDim).value_or(source);
    const auto spans = arena.contentOf(authority);
    node.run.assign(spans.begin(), spans.end());
    if (const auto quote = arena.quotedContent(authority)) {
      node.quoteSpace = spaceOfStore(arena, quote->store);
      // The spans are addressed in the store's own scroll namespace, which is
      // what a quote cell is read through.
      if (node.quoteSpace) node.run = quote->spans;
    }
    nodes.push_back(std::move(node));
    indexOf.emplace(source, nodes.size() - 1);
    return nodes.size() - 1;
  };

  for (const CellRef subject : subjects) {
    const auto root = discover(subject);
    if (!root) return std::unexpected{root.error()};
    roots.push_back(*root);
  }

  // Breadth first: `nodes` grows as neighbours are discovered, so a plain
  // index loop is the queue. Edges are recorded by node index so cycles and
  // shared neighbours need no second lookup.
  for (std::size_t at = 0; at < nodes.size(); ++at) {
    const CellRef source           = nodes[at].source;
    std::vector<DimRef> candidates = fixed;
    if (selection.everything) {
      for (const auto dim : named)
        if (std::ranges::find(candidates, dim) == candidates.end())
          candidates.push_back(dim);
      for (const auto &link : arena.dimensionsOf(source))
        if (std::ranges::find(candidates, link.dim) == candidates.end())
          candidates.push_back(link.dim);
    }
    for (const DimRef dim : candidates) {
      for (const DimVector dir : bothDirections) {
        if (!selection.selects(DirectedDim{dim, dir})) continue;
        arena.consumeWork();
        const auto neighbour = arena.linkedMinting(source, dim, dir);
        if (!neighbour) continue;
        const auto target = discover(*neighbour);
        if (!target) return std::unexpected{target.error()};
        nodes[at].edges.push_back(
            Edge{.dim = dim, .dir = dir, .target = *target});
      }
    }
  }

  if (static_cast<std::size_t>(arena.cellCount()) + nodes.size() >
      arena.allocationLimit())
    return refuse(TopologyCopyRefusal::AllocationLimit,
                  subjects.empty() ? noCell : subjects.front());

  std::vector<CellRef> copies;
  copies.reserve(nodes.size());
  for (const auto &node : nodes) {
    const CellRef fresh = node.quoteSpace
                              ? arena.makeQuote(*node.quoteSpace, node.run)
                              : arena.makeCell();
    if (!node.quoteSpace && !node.run.empty())
      expectWritten(arena.setContent(fresh, node.run));
    if (node.kind != xanadu::ValueKind::None || node.bits != 0)
      expectWritten(arena.setValueBits(fresh, node.kind, node.bits));
    copies.push_back(fresh);
  }
  for (std::size_t at = 0; at < nodes.size(); ++at)
    for (const auto &edge : nodes[at].edges)
      expectWritten(
          arena.link(copies[at], edge.dim, edge.dir, copies[edge.target]));

  std::vector<CellRef> result;
  result.reserve(roots.size());
  for (const auto root : roots) result.push_back(copies[root]);
  return result;
}

} // namespace zigzag
