/**
 * @file logic_relations_arena.cpp
 * @brief Card P4: arena relation providers (cell_value, cell_link, cell_span,
 * bridge_edge, lens and SQL fact enumeration).
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/logic_relations_internal.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex::logic {

namespace {

// =============================================================================
// cell_value/2
// =============================================================================

class CellValueProvider : public CoreBoundProvider {
public:
  explicit CellValueProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::span<const ArgKind> signature() const noexcept override {
    static constexpr std::array<ArgKind, 2> kKinds{ArgKind::Integer,
                                                   ArgKind::Any};
    return kKinds;
  }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;

    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);

    const auto mintValCell = [this](CellRef target) -> CellRef {
      if (const auto d = core_->arena().asDouble(target); d.has_value()) {
        return core_->arena().makeScalarCell(*d);
      }
      if (const auto i = core_->arena().asInt64(target); i.has_value()) {
        return core_->arena().makeScalarCell(*i);
      }
      if (const auto b = core_->arena().asBool(target); b.has_value()) {
        return core_->arena().makeScalarCell(*b);
      }
      return core_->arena().makeCell(core_->arena().textOf(target));
    };

    if (!isVar0) {
      // Point lookup by bound CellId
      if (request.position > 0) return std::nullopt;

      const auto target = cellRefArgument(core_->arena(), *bound0);
      if (!target) return std::nullopt;

      const CellRef idCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(*target));
      const CellRef valCell = mintValCell(*target);
      return RelationAnswer{
          .nextPosition = 1,
          .limit        = 1,
          .cells        = {idCell, valCell},
      };
    }

    // Unbound CellId: scan all arena cells up to snapshot limit
    const auto limit = scanEnd();
    const std::size_t start =
        std::max<std::size_t>(scanBegin(), request.position);

    for (std::size_t c = start; c < limit; ++c) {
      core_->arena().consumeWork();
      const CellRef target =
          zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
      if (!core_->arena().contains(target)) continue;

      const CellRef idCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(target));
      const CellRef valCell = mintValCell(target);
      return RelationAnswer{
          .nextPosition = c + 1,
          .limit        = limit,
          .cells        = {idCell, valCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

// =============================================================================
// cell_link/4
// =============================================================================

struct LinkEntry {
  CellRef from{};
  std::string dimName;
  std::string dir;
  CellRef to{};
};

class CellLinkProvider : public CoreBoundProvider {
public:
  explicit CellLinkProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 4; }

  [[nodiscard]] std::span<const ArgKind> signature() const noexcept override {
    static constexpr std::array<ArgKind, 4> kKinds{
        ArgKind::Integer, ArgKind::Atom, ArgKind::Atom, ArgKind::Integer};
    return kKinds;
  }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;

    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);
    const auto bound3 = stdlib_.deref(request.arguments[3]);
    const bool isVar3 = !bound3 || stdlib_.isVar(*bound3);

    const auto matchesFilter = [&](const LinkEntry &l) -> bool {
      if (!isVar1 && core_->arena().textOf(*bound1) != l.dimName) return false;
      if (!isVar2 && core_->arena().textOf(*bound2) != l.dir) return false;
      if (!isVar3) {
        const auto toTarget = cellRefArgument(core_->arena(), *bound3);
        if (!toTarget || *toTarget != l.to) return false;
      }
      return true;
    };

    const auto linksForCell = [this](CellRef from) -> std::vector<LinkEntry> {
      std::vector<LinkEntry> entries;
      for (const auto &dl : core_->arena().dimensionsOf(from)) {
        core_->arena().consumeWork();
        std::string dimName = core_->arena().textOf(dl.dim);
        if (dimName.empty()) dimName = "d." + std::to_string(dl.dim);
        if (dl.pos != noCell) {
          entries.push_back({from, dimName, "pos", dl.pos});
        }
        if (dl.neg != noCell) {
          entries.push_back({from, dimName, "neg", dl.neg});
        }
      }
      return entries;
    };

    const auto makeAnswer = [this](const LinkEntry &l, std::size_t nextPos,
                                   std::size_t limit) -> RelationAnswer {
      const CellRef fromRefCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(l.from));
      const CellRef dimCell = core_->arena().makeCell(l.dimName);
      const CellRef dirCell = core_->arena().makeCell(l.dir);
      const CellRef toRefCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(l.to));
      return RelationAnswer{
          .nextPosition = nextPos,
          .limit        = limit,
          .cells        = {fromRefCell, dimCell, dirCell, toRefCell},
      };
    };

    if (!isVar0) {
      // Bound FromId
      const auto fromCell = cellRefArgument(core_->arena(), *bound0);
      if (!fromCell) return std::nullopt;

      const auto links = linksForCell(*fromCell);
      for (std::size_t i = request.position; i < links.size(); ++i) {
        if (!matchesFilter(links[i])) continue;
        return makeAnswer(links[i], i + 1, links.size());
      }
      return std::nullopt;
    }

    // Unbound FromId: scan all cells
    const auto limit               = scanEnd();
    constexpr unsigned kSubBits    = 7;
    constexpr std::size_t kSubMask = (1U << kSubBits) - 1U;

    const std::size_t startC =
        std::max<std::size_t>(scanBegin(), request.position >> kSubBits);
    const std::size_t startSub = request.position & kSubMask;

    for (std::size_t c = startC; c < limit; ++c) {
      core_->arena().consumeWork();
      const CellRef from =
          zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
      if (!core_->arena().contains(from)) continue;

      const auto links         = linksForCell(from);
      const std::size_t firstK = (c == startC ? startSub : 0);
      for (std::size_t k = firstK; k < links.size(); ++k) {
        if (!matchesFilter(links[k])) continue;
        const std::size_t nextPos = (c << kSubBits) | (k + 1);
        return makeAnswer(links[k], nextPos, limit);
      }
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

// =============================================================================
// cell_span/2
// =============================================================================

class CellSpanProvider : public CoreBoundProvider {
public:
  explicit CellSpanProvider(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::span<const ArgKind> signature() const noexcept override {
    static constexpr std::array<ArgKind, 2> kKinds{ArgKind::Integer,
                                                   ArgKind::Atom};
    return kKinds;
  }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;

    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);

    const auto matchesSpan = [&](const std::string &spanStr) -> bool {
      if (!isVar1 && core_->arena().textOf(*bound1) != spanStr) return false;
      return true;
    };

    if (!isVar0) {
      // Bound CellId
      const auto cell = cellRefArgument(core_->arena(), *bound0);
      if (!cell) return std::nullopt;

      const auto spans = core_->arena().contentOf(*cell);
      for (std::size_t i = request.position; i < spans.size(); ++i) {
        core_->arena().consumeWork();
        const std::string spanStr = "[" + std::to_string(spans[i].start) + "," +
                                    std::to_string(spans[i].length) + "]";
        if (!matchesSpan(spanStr)) continue;

        const CellRef cCell =
            core_->arena().makeScalarCell(static_cast<std::int64_t>(*cell));
        const CellRef spCell = core_->arena().makeCell(spanStr);
        return RelationAnswer{
            .nextPosition = i + 1,
            .limit        = spans.size(),
            .cells        = {cCell, spCell},
        };
      }
      return std::nullopt;
    }

    // Unbound CellId
    const auto limit               = scanEnd();
    constexpr unsigned kSubBits    = 7;
    constexpr std::size_t kSubMask = (1U << kSubBits) - 1U;

    const std::size_t startC =
        std::max<std::size_t>(scanBegin(), request.position >> kSubBits);
    const std::size_t startSub = request.position & kSubMask;

    for (std::size_t c = startC; c < limit; ++c) {
      core_->arena().consumeWork();
      const CellRef target =
          zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
      if (!core_->arena().contains(target)) continue;

      const auto spans         = core_->arena().contentOf(target);
      const std::size_t firstK = (c == startC ? startSub : 0);
      for (std::size_t k = firstK; k < spans.size(); ++k) {
        core_->arena().consumeWork();
        const std::string spanStr = "[" + std::to_string(spans[k].start) + "," +
                                    std::to_string(spans[k].length) + "]";
        if (!matchesSpan(spanStr)) continue;

        const CellRef cCell =
            core_->arena().makeScalarCell(static_cast<std::int64_t>(target));
        const CellRef spCell      = core_->arena().makeCell(spanStr);
        const std::size_t nextPos = (c << kSubBits) | (k + 1);
        return RelationAnswer{
            .nextPosition = nextPos,
            .limit        = limit,
            .cells        = {cCell, spCell},
        };
      }
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

// =============================================================================
// bridge_edge/2 and bridge_edge/3
// =============================================================================

class BridgeEdgeProvider2 : public CoreBoundProvider {
public:
  explicit BridgeEdgeProvider2(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 2; }

  [[nodiscard]] std::span<const ArgKind> signature() const noexcept override {
    static constexpr std::array<ArgKind, 2> kKinds{ArgKind::Integer,
                                                   ArgKind::Integer};
    return kKinds;
  }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;

    const DimRef cloneDim = core_->dims().clone;
    const auto bound0     = stdlib_.deref(request.arguments[0]);
    const bool isVar0     = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1     = stdlib_.deref(request.arguments[1]);
    const bool isVar1     = !bound1 || stdlib_.isVar(*bound1);

    if (!isVar0) {
      // Bound PresentationCell
      if (request.position > 0) return std::nullopt;

      const auto pres = cellRefArgument(core_->arena(), *bound0);
      if (!pres) return std::nullopt;

      const auto docCell =
          core_->arena().linked(*pres, cloneDim, DimVector::POS);
      if (!docCell) return std::nullopt;

      if (!isVar1) {
        const auto docTarget = cellRefArgument(core_->arena(), *bound1);
        if (!docTarget || *docTarget != *docCell) return std::nullopt;
      }

      const CellRef pCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(*pres));
      const CellRef dCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(*docCell));
      return RelationAnswer{
          .nextPosition = 1,
          .limit        = 1,
          .cells        = {pCell, dCell},
      };
    }

    // Unbound PresentationCell
    const auto limit = scanEnd();
    const std::size_t start =
        std::max<std::size_t>(scanBegin(), request.position);

    for (std::size_t c = start; c < limit; ++c) {
      core_->arena().consumeWork();
      const CellRef presCell =
          zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
      if (!core_->arena().contains(presCell)) continue;

      const auto docCell =
          core_->arena().linked(presCell, cloneDim, DimVector::POS);
      if (!docCell) continue;

      if (!isVar1) {
        const auto docTarget = cellRefArgument(core_->arena(), *bound1);
        if (!docTarget || *docTarget != *docCell) continue;
      }

      const CellRef pCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(presCell));
      const CellRef dCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(*docCell));
      return RelationAnswer{
          .nextPosition = c + 1,
          .limit        = limit,
          .cells        = {pCell, dCell},
      };
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

struct EdgeEntry {
  CellRef from{};
  std::string dimName;
  CellRef to{};
};

class BridgeEdgeProvider3 : public CoreBoundProvider {
public:
  explicit BridgeEdgeProvider3(VortexStdLib &stdlib) : stdlib_(stdlib) {}

  [[nodiscard]] std::size_t arity() const noexcept override { return 3; }

  [[nodiscard]] std::span<const ArgKind> signature() const noexcept override {
    static constexpr std::array<ArgKind, 3> kKinds{
        ArgKind::Integer, ArgKind::Atom, ArgKind::Integer};
    return kKinds;
  }

  [[nodiscard]] std::optional<RelationAnswer>
  next(const RelationRequest &request) override {
    if (!core_) return std::nullopt;

    const auto bound0 = stdlib_.deref(request.arguments[0]);
    const bool isVar0 = !bound0 || stdlib_.isVar(*bound0);
    const auto bound1 = stdlib_.deref(request.arguments[1]);
    const bool isVar1 = !bound1 || stdlib_.isVar(*bound1);
    const auto bound2 = stdlib_.deref(request.arguments[2]);
    const bool isVar2 = !bound2 || stdlib_.isVar(*bound2);

    const std::string reqDimName =
        (!isVar1) ? core_->arena().textOf(*bound1) : "";

    const auto matchesFilter = [&](const EdgeEntry &e) -> bool {
      if (!reqDimName.empty() && e.dimName != reqDimName) return false;
      if (!isVar2) {
        const auto toTarget = cellRefArgument(core_->arena(), *bound2);
        if (!toTarget || *toTarget != e.to) return false;
      }
      return true;
    };

    const auto edgesFrom =
        [this, &reqDimName](CellRef from) -> std::vector<EdgeEntry> {
      std::vector<EdgeEntry> entries;
      if (!core_->arena().contains(from)) return entries;
      for (const auto &dl : core_->arena().dimensionsOf(from)) {
        core_->arena().consumeWork();
        if (dl.pos == noCell) continue;
        std::string dimName = core_->arena().textOf(dl.dim);
        if (dimName.empty()) dimName = "d." + std::to_string(dl.dim);
        if (!reqDimName.empty() && dimName != reqDimName) continue;
        entries.push_back({from, dimName, dl.pos});
      }
      return entries;
    };

    const auto makeAnswer = [this](const EdgeEntry &e, std::size_t nextPos,
                                   std::size_t limit) -> RelationAnswer {
      const CellRef uCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(e.from));
      const CellRef dCell = core_->arena().makeCell(e.dimName);
      const CellRef vCell =
          core_->arena().makeScalarCell(static_cast<std::int64_t>(e.to));
      return RelationAnswer{
          .nextPosition = nextPos,
          .limit        = limit,
          .cells        = {uCell, dCell, vCell},
      };
    };

    if (!isVar0) {
      // Bound U
      const auto from = cellRefArgument(core_->arena(), *bound0);
      if (!from) return std::nullopt;

      const auto edges = edgesFrom(*from);
      for (std::size_t i = request.position; i < edges.size(); ++i) {
        if (!matchesFilter(edges[i])) continue;
        return makeAnswer(edges[i], i + 1, edges.size());
      }
      return std::nullopt;
    }

    // Unbound U
    const auto limit               = scanEnd();
    constexpr unsigned kSubBits    = 7;
    constexpr std::size_t kSubMask = (1U << kSubBits) - 1U;

    const std::size_t startC =
        std::max<std::size_t>(scanBegin(), request.position >> kSubBits);
    const std::size_t startSub = request.position & kSubMask;

    for (std::size_t c = startC; c < limit; ++c) {
      core_->arena().consumeWork();
      const CellRef fromCell =
          zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
      if (!core_->arena().contains(fromCell)) continue;

      const auto edges         = edgesFrom(fromCell);
      const std::size_t firstK = (c == startC ? startSub : 0);
      for (std::size_t k = firstK; k < edges.size(); ++k) {
        if (!matchesFilter(edges[k])) continue;
        const std::size_t nextPos = (c << kSubBits) | (k + 1);
        return makeAnswer(edges[k], nextPos, limit);
      }
    }
    return std::nullopt;
  }

private:
  VortexStdLib &stdlib_;
};

} // namespace

void registerArenaRelations(RelationRegistry &registry, VortexStdLib &stdlib) {
  registry.add("cell_value", std::make_unique<CellValueProvider>(stdlib));
  registry.add("cell_link", std::make_unique<CellLinkProvider>(stdlib));
  registry.add("cell_span", std::make_unique<CellSpanProvider>(stdlib));
  registry.add("bridge_edge", std::make_unique<BridgeEdgeProvider2>(stdlib));
  registry.add("bridge_edge", std::make_unique<BridgeEdgeProvider3>(stdlib));
}

} // namespace zigzag::vortex::logic
