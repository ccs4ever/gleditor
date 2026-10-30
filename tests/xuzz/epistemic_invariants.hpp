/**
 * @file epistemic_invariants.hpp
 * @brief The invariants every ingestion step keeps (P19 I3-1b, I3-1c), read
 *        from an agent's arena. Shared by the ingress fuzzer and the
 *        noisy-sensor property tests; free of gtest so the fuzzer can abort.
 */
#ifndef TESTS_XUZZ_EPISTEMIC_INVARIANTS_HPP
#define TESTS_XUZZ_EPISTEMIC_INVARIANTS_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime::epistemic_test {

/// What a refused relayed copy must leave alone.
struct ProfileHealth {
  std::optional<std::int64_t> contentRefusals;
  std::optional<std::int64_t> rejectCount;
  std::optional<double> lastContact;
  bool operator==(const ProfileHealth &) const = default;
};

struct EpistemicSnapshot {
  /// (clause, support, record, status) over the arbitrated predicates, in
  /// rank order: unchanged by any refused or duplicate ingestion.
  std::vector<std::tuple<zigzag::CellRef, zigzag::CellRef, zigzag::CellRef,
                         std::string>>
      beliefs;
  /// By principal token.
  std::map<std::string, ProfileHealth> health;
  bool operator==(const EpistemicSnapshot &) const = default;
};

/// Reads one agent's epistemic state; `predicates` names the arbitrated ones.
class EpistemicView {
public:
  /// @p keyless names the predicates with no key argument: one quantity per
  /// source, so each source supports at most one of their clauses.
  EpistemicView(AgentExecutor &executor, const identity::ActorPid &pid,
                std::span<const std::string_view> predicates,
                std::span<const std::string_view> keyless = {})
      : ctx_(*executor.context(pid)), agent_(*executor.agentCell(pid)),
        dims_(registerDimensions(
            const_cast<zigzag::ArenaManifold &>(ctx_.arena))),
        values_(valueDims(ctx_.core)),
        predicates_(predicates.begin(), predicates.end()),
        keyless_(keyless.begin(), keyless.end()) {}

  [[nodiscard]] EpistemicSnapshot snapshot() const {
    EpistemicSnapshot out;
    forEachRecord([&](zigzag::CellRef clause, zigzag::CellRef support,
                      zigzag::CellRef record) {
      out.beliefs.emplace_back(clause, support, record,
                               text(record, kObsStatus).value_or(""));
    });
    for (const auto profile : profiles())
      out.health.emplace(
          arena().textOf(profile),
          ProfileHealth{integer(profile, kProfileContentRefusals),
                        integer(profile, kProfileRejectCount),
                        real(profile, kProfileLastContact)});
    return out;
  }

  /// The first broken invariant, if any.
  [[nodiscard]] std::optional<std::string> violation() const {
    // One support per (principal, predicate, key), whatever its records say.
    std::set<std::tuple<std::string, std::string, std::string>> supported;
    for (const auto &[name, pred] : predicateCells())
      for (const auto clause :
           zigzag::rankAfter(arena(), pred, ctx_.core.dims().clause)) {
        const auto key =
            keyless_.contains(name) ? std::string{} : keyOf(clause);
        for (const auto support :
             zigzag::rankAfter(arena(), clause, dims_.source)) {
          const auto token = arena().textOf(support);
          if (!supported.emplace(token, name, key).second)
            return "two supports for " + token + " on " + name + "(" + key +
                   ")";
          const auto profile = profileOf(token);
          if (!profile) return "support without a profile: " + token;
          if (flag(*profile, kProfileRetired).value_or(false))
            return "a retired principal still supports " + name;
          std::size_t current = 0;
          for (const auto record :
               zigzag::rankAfter(arena(), support, dims_.epistemic)) {
            if (text(record, kObsStatus) != kObsStatusCurrent) continue;
            ++current;
            if (integer(record, kObsIncarnation) !=
                integer(*profile, kProfileIncarnation))
              return "a current record of another incarnation: " + token;
          }
          if (current > 1)
            return "two current records for " + token + " on " + name;
        }
      }
    // A profile's retired history holds only retired records.
    for (const auto profile : profiles())
      for (const auto record :
           zigzag::rankAfter(arena(), profile, dims_.epistemic))
        if (text(record, kObsStatus) != kObsStatusRetired)
          return "a live record in the retired history of " +
                 arena().textOf(profile);
    return std::nullopt;
  }

  /// The incarnation a principal's profile records, if it has one.
  [[nodiscard]] std::optional<std::int64_t>
  incarnationOf(std::string_view token) const {
    const auto profile = profileOf(token);
    return profile ? integer(*profile, kProfileIncarnation) : std::nullopt;
  }

private:
  const zigzag::ArenaManifold &arena() const { return ctx_.arena; }

  std::vector<std::pair<std::string, zigzag::CellRef>> predicateCells() const {
    std::vector<std::pair<std::string, zigzag::CellRef>> out;
    for (const auto pred : zigzag::rankAfter(arena(), agent_, dims_.belief)) {
      const auto name = arena().textOf(pred);
      if (predicates_.contains(name)) out.emplace_back(name, pred);
    }
    return out;
  }
  template <class F> void forEachRecord(F &&f) const {
    for (const auto &[name, pred] : predicateCells())
      for (const auto clause :
           zigzag::rankAfter(arena(), pred, ctx_.core.dims().clause))
        for (const auto support :
             zigzag::rankAfter(arena(), clause, dims_.source))
          for (const auto record :
               zigzag::rankAfter(arena(), support, dims_.epistemic))
            f(clause, support, record);
  }
  std::vector<zigzag::CellRef> profiles() const {
    return zigzag::rankAfter(arena(), agent_, dims_.trust) |
           std::ranges::to<std::vector<zigzag::CellRef>>();
  }
  std::optional<zigzag::CellRef> profileOf(std::string_view token) const {
    for (const auto profile : profiles())
      if (arena().textOf(profile) == token) return profile;
    return std::nullopt;
  }
  /// Every keyed arbitrated predicate here keys on its first argument.
  std::string keyOf(zigzag::CellRef clause) const {
    const auto &d   = ctx_.core.dims();
    const auto term = arena().linked(clause, d.grab);
    if (!term) return {};
    const auto first = arena().linked(*term, d.grab);
    return first ? arena().textOf(*first) : std::string{};
  }
  std::optional<zigzag::CellRef> field(zigzag::CellRef record,
                                       std::string_view name) const {
    return fieldValue(arena(), values_, record, name);
  }
  std::optional<std::string> text(zigzag::CellRef record,
                                  std::string_view name) const {
    const auto cell = field(record, name);
    return cell ? std::optional{arena().textOf(*cell)} : std::nullopt;
  }
  std::optional<std::int64_t> integer(zigzag::CellRef record,
                                      std::string_view name) const {
    const auto cell = field(record, name);
    return cell ? arena().asInt64(*cell) : std::nullopt;
  }
  std::optional<double> real(zigzag::CellRef record,
                             std::string_view name) const {
    const auto cell = field(record, name);
    return cell ? arena().asDouble(*cell) : std::nullopt;
  }
  std::optional<bool> flag(zigzag::CellRef record,
                           std::string_view name) const {
    const auto cell = field(record, name);
    return cell ? arena().asBool(*cell) : std::nullopt;
  }

  const zigzag::vortex::WorkerContext &ctx_;
  zigzag::CellRef agent_;
  AgentDims dims_;
  ValueDims values_;
  std::set<std::string, std::less<>> predicates_;
  std::set<std::string, std::less<>> keyless_;
};

} // namespace xanadu::vagent::runtime::epistemic_test

#endif // TESTS_XUZZ_EPISTEMIC_INVARIANTS_HPP
