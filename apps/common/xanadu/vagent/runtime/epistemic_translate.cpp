#include "common/xanadu/vagent/runtime/epistemic_translate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <limits>
#include <ranges>
#include <unordered_set>

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::mesh::ActorError;

constexpr auto kFailed = ActorError::ExecutionFailed;

std::vector<CellRef> ranked(const zigzag::ArenaManifold &arena, CellRef owner,
                            const std::optional<DimRef> &dim) {
  return dim ? zigzag::rankAfter(arena, owner, *dim) |
                   std::ranges::to<std::vector<CellRef>>()
             : std::vector<CellRef>{};
}

bool isToken(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::optional<ValueDims> valueDimsOf(const OwnedGraphDims &dims) {
  if (!dims.vars || !dims.values) return std::nullopt;
  return ValueDims{.vars   = *dims.vars,
                   .values = *dims.values,
                   .grab   = dims.grab.value_or(*dims.vars),
                   .step   = dims.step.value_or(*dims.vars)};
}

std::optional<std::string> textField(const zigzag::ArenaManifold &arena,
                                     const ValueDims &vd, CellRef owner,
                                     std::string_view name) {
  const auto cell = fieldValue(arena, vd, owner, name);
  return cell ? std::optional{arena.textOf(*cell)} : std::nullopt;
}

} // namespace

// -- Keys
// ----------------------------------------------------------------------

std::optional<ArbitrationPolicy>
decodePolicy(const zigzag::ArenaManifold &arena, const ValueDims &vd,
             const CellRef master) {
  const auto cell = [&](std::string_view name) {
    return fieldValue(arena, vd, master, name);
  };
  const auto real = [&](std::string_view name) -> std::optional<double> {
    const auto c = cell(name);
    return c ? arena.asDouble(*c) : std::nullopt;
  };
  const auto integer =
      [&](std::string_view name) -> std::optional<std::int64_t> {
    const auto c = cell(name);
    return c ? arena.asInt64(*c) : std::nullopt;
  };
  const auto flag = [&](std::string_view name) -> std::optional<bool> {
    const auto c = cell(name);
    return c ? arena.asBool(*c) : std::nullopt;
  };
  const auto text = [&](std::string_view name) {
    return textField(arena, vd, master, name);
  };
  ArbitrationPolicy p;
  const auto strategy = text("strategy");
  const auto clock    = text("clock");
  const auto model    = text("model");
  if (!strategy || !clock || !model) return std::nullopt;
  const auto strategies = {ArbitrationStrategy::Truest,
                           ArbitrationStrategy::Ranked,
                           ArbitrationStrategy::Consensus};
  const auto s          = std::ranges::find_if(
      strategies, [&](auto v) { return strategyName(v) == *strategy; });
  if (s == strategies.end()) return std::nullopt;
  p.strategy        = *s;
  const auto clocks = {ObservationClock::Wall, ObservationClock::ActorMapped};
  const auto c      = std::ranges::find_if(
      clocks, [&](auto v) { return clockName(v) == *clock; });
  if (c == clocks.end()) return std::nullopt;
  p.clock                  = *c;
  p.model                  = *model;
  const auto minConfidence = real("min_confidence");
  const auto alpha         = real("prior_alpha");
  const auto beta          = real("prior_beta");
  const auto maxAge        = integer("max_age");
  const auto skew          = integer("clock_skew");
  const auto uncertainty   = integer("max_uncertainty");
  const auto tolerance     = real("score_tolerance");
  const auto untested      = flag("allow_untested");
  const auto continuous    = flag("continuous");
  if (!minConfidence || !alpha || !beta || !maxAge || !skew || !uncertainty ||
      !tolerance || !untested || !continuous)
    return std::nullopt;
  p.minConfidence      = *minConfidence;
  p.priorAlpha         = *alpha;
  p.priorBeta          = *beta;
  p.maxAgeMs           = *maxAge;
  p.clockSkewMs        = *skew;
  p.maxUncertaintyMs   = *uncertainty;
  p.scoreTolerance     = *tolerance;
  p.allowUntested      = *untested;
  p.continuous         = *continuous;
  p.decayMs            = integer("decay");
  p.alignmentMs        = integer("alignment");
  p.clusterTolerance   = real("cluster_tolerance");
  p.frame              = text("frame");
  p.units              = text("units");
  p.minVariance        = real("min_variance");
  p.maxVariance        = real("max_variance");
  p.maxClusterDiameter = real("max_cluster_diameter");
  p.maxSlew            = real("max_slew");
  p.innovation         = real("innovation");
  return p;
}

PredicateKey keyOf(const std::string_view name,
                   const UpgradePredicate &predicate) {
  PredicateKey key{.signature = canonicalPredicateSignature(name, predicate),
                   .policy    = std::nullopt};
  if (predicate.epistemicPolicy) key.policy = predicate.epistemicPolicy->policy;
  return key;
}

std::uint64_t keyDigest(const PredicateKey &key) {
  const auto hex      = xanadu::sha256Hex(key.signature);
  std::uint64_t value = 0;
  std::from_chars(hex.data(), hex.data() + 16, value, 16);
  return value;
}

std::optional<TransferRefusal>
checkPredicateKeys(const zigzag::ArenaManifold &arena, const CellRef agent,
                   const OwnedGraphDims &dims, const UpgradeSchema &into,
                   const TransferRoute route) {
  const auto vd = valueDimsOf(dims);
  if (!vd || !dims.epistemic) return std::nullopt;
  const auto refuse = [&](TransferCode code, RefusalDetail detail) {
    return std::optional{
        TransferRefusal::make(code, route, kFailed, std::move(detail))};
  };
  std::vector<std::string> names;
  std::vector<PredicateKey> keys;
  for (const auto &[name, predicate] : into.predicates) {
    names.push_back(name);
    keys.push_back(keyOf(name, predicate));
  }
  const PredicateMatcher matcher(std::move(keys));
  for (const auto predicate : ranked(arena, agent, dims.belief)) {
    const auto name   = arena.textOf(predicate);
    const auto master = arena.linked(predicate, *dims.epistemic);
    const auto was =
        master ? textField(arena, *vd, *master, "hash") : std::nullopt;
    const auto next = into.predicates.find(name);
    if (next == into.predicates.end()) {
      // A plain predicate is dropped as it always was; one with a policy
      // master holds sources' histories scored under it.
      if (was)
        return refuse(TransferCode::PredicateRemoved,
                      {.subject = name, .saved = was});
      continue;
    }
    const auto now = next->second.epistemicPolicy
                         ? std::optional{next->second.epistemicPolicy->hash}
                         : std::nullopt;
    // A tag change changes the hash too, so the tag is read first: it names
    // the cause where the hash only says that something differs.
    if (was) {
      const auto tag = textField(arena, *vd, *master, "encoding");
      if (tag != std::optional<std::string>{kPolicyEncoding})
        return refuse(TransferCode::PolicyEncodingChanged,
                      {.subject = name,
                       .saved   = tag,
                       .current = std::string(kPolicyEncoding)});
    }
    // A source's history was judged under its policy; a resume under another
    // would score it by rules it was never checked against.
    if (was != now)
      return refuse(TransferCode::PolicyChanged,
                    {.subject = name, .saved = was, .current = now});
    if (!was) continue;
    // The hash is only a claim until the typed cells it summarises agree.
    auto decoded = decodePolicy(arena, *vd, *master);
    if (!decoded || policyHash(*decoded) != *was)
      return refuse(TransferCode::StateInconsistent, {.subject = name});
    const PredicateKey saved{
        .signature = textField(arena, *vd, *master, "signature").value_or(""),
        .policy    = std::move(decoded)};
    const auto expected = keyOf(name, next->second);
    if (matcher.find(saved)) continue;
    if (saved.policy != expected.policy)
      return refuse(TransferCode::PolicyChanged,
                    {.subject = name, .saved = was, .current = now});
    return refuse(TransferCode::PredicateIncompatible,
                  {.subject = name,
                   .saved   = saved.signature.empty()
                                  ? std::nullopt
                                  : std::optional{saved.signature},
                   .current = expected.signature});
  }
  return std::nullopt;
}

// -- Plan
// ----------------------------------------------------------------------

namespace {

/// Bytes of the distinct texts of the cells a plan will copy.
std::uint64_t distinctTextBytes(const zigzag::ArenaManifold &arena,
                                const ValueDims &vd, const OwnedGraphDims &dims,
                                const TranslationPlan &plan) {
  std::unordered_set<CellRef> seen;
  std::unordered_set<std::string> texts;
  std::vector<CellRef> stack;
  const auto admit = [&](CellRef cell) {
    if (seen.insert(cell).second) stack.push_back(cell);
  };
  for (const auto &profile : plan.profiles) {
    admit(profile.cell);
    for (const auto view : profile.views) admit(view);
  }
  for (const auto &record : plan.records) admit(record.cell);
  const std::array<std::optional<DimRef>, 4> reach{vd.vars, vd.values,
                                                   dims.grab, dims.step};
  while (!stack.empty()) {
    const auto cell = stack.back();
    stack.pop_back();
    if (arena.valueKindOf(cell) == xanadu::ValueKind::None)
      texts.insert(arena.textOf(cell));
    for (const auto &dim : reach)
      if (dim)
        if (const auto next = arena.linked(cell, *dim)) admit(*next);
  }
  std::uint64_t bytes = 0;
  for (const auto &text : texts) bytes += text.size();
  return bytes;
}

} // namespace

std::expected<TranslationPlan, TransferRefusal>
planTranslation(const PlanRequest &q) {
  const auto &arena = q.from;
  const auto &dims  = q.fromDims;
  const auto refuse = [&](TransferCode code, RefusalDetail detail,
                          ActorError actor = kFailed) {
    return std::unexpected(
        TransferRefusal::make(code, q.route, actor, std::move(detail)));
  };
  const auto probe = probeEpistemicState(arena, q.fromAgent, dims);
  if (probe.populated && probe.shape && !q.registry.translates(*probe.shape))
    return refuse(TransferCode::NoTranslator,
                  {.subject    = "d.trust",
                   .savedCount = static_cast<std::uint64_t>(*probe.shape)});
  if (auto refused =
          checkPredicateKeys(arena, q.fromAgent, dims, q.intoSchema, q.route))
    return std::unexpected(std::move(*refused));

  TranslationPlan plan;
  const auto vd = valueDimsOf(dims);
  if (!vd) return plan;

  const auto checkDims = [&](CellRef cell) -> std::optional<std::string> {
    for (const auto &link : arena.dimensionsOf(cell)) {
      auto name = arena.textOf(link.dim);
      if (!ownsDimension(name)) return name;
    }
    return std::nullopt;
  };
  std::unordered_set<CellRef> views;
  for (const auto profile : ranked(arena, q.fromAgent, dims.trust)) {
    const auto token = arena.textOf(profile);
    if (!isToken(token))
      return refuse(TransferCode::StateInconsistent, {.subject = token});
    if (const auto stray = checkDims(profile))
      return refuse(TransferCode::UnregisteredOwnedStructure,
                    {.subject = *stray});
    bool retired = false;
    for (const auto &[name, value] : fieldsOf(arena, *vd, profile)) {
      const auto field = arena.textOf(name);
      if (!ownsProfileField(field))
        return refuse(TransferCode::UnregisteredOwnedStructure,
                      {.subject = field});
      if (field == kProfileRetired && value)
        retired = arena.asBool(*value).value_or(false);
    }
    const auto missing = [&](std::string_view field) {
      return refuse(TransferCode::ReplayStateUnavailable,
                    {.subject = token, .saved = std::string(field)});
    };
    if (!fieldValue(arena, *vd, profile, kProfileIncarnation))
      return missing(kProfileIncarnation);
    // A retired principal's tombstone keeps its floor and clears the rest.
    for (const auto field : {kProfileStreams, kProfileDedup})
      if (!retired && !fieldValue(arena, *vd, profile, field))
        return missing(field);
    if (const auto dedup = fieldValue(arena, *vd, profile, kProfileDedup)) {
      if (const auto first =
              dims.grab ? arena.linked(*dedup, *dims.grab) : std::nullopt) {
        const auto check = [&](CellRef entry) {
          return fieldValue(arena, *vd, entry, kObsIncarnation) &&
                 fieldValue(arena, *vd, entry, kObsEventId) &&
                 fieldValue(arena, *vd, entry, kObsDigest);
        };
        if (!check(*first)) return missing(kProfileDedup);
        if (dims.step)
          for (const auto entry : zigzag::rankAfter(arena, *first, *dims.step))
            if (!check(entry)) return missing(kProfileDedup);
      }
    }
    TranslationPlan::Profile entry{.cell = profile, .views = {}};
    for (auto v = dims.clone ? arena.linked(profile, *dims.clone)
                             : std::nullopt;
         v; v   = arena.linked(*v, *dims.clone)) {
      if (!views.insert(*v).second)
        return refuse(TransferCode::StateInconsistent, {.subject = token});
      entry.views.push_back(*v);
    }
    plan.profiles.push_back(std::move(entry));
  }

  const auto addRecords =
      [&](CellRef anchor) -> std::optional<TransferRefusal> {
    for (const auto record : ranked(arena, anchor, dims.epistemic)) {
      if (const auto stray = checkDims(record))
        return TransferRefusal::make(TransferCode::UnregisteredOwnedStructure,
                                     q.route, kFailed, {.subject = *stray});
      for (const auto &[name, value] : fieldsOf(arena, *vd, record)) {
        const auto field = arena.textOf(name);
        if (!ownsRecordField(field))
          return TransferRefusal::make(TransferCode::UnregisteredOwnedStructure,
                                       q.route, kFailed, {.subject = field});
      }
      // A record cannot exist without its profile: its `profile` field's value
      // is the clone occurrence on that profile's chain.
      const auto view = fieldValue(arena, *vd, record, kObsProfile);
      if (!view || !views.contains(*view))
        return TransferRefusal::make(TransferCode::StateInconsistent, q.route,
                                     kFailed,
                                     {.subject = arena.textOf(anchor)});
      plan.records.push_back({.cell = record, .anchor = anchor});
    }
    return std::nullopt;
  };
  for (const auto &profile : plan.profiles)
    if (auto refused = addRecords(profile.cell))
      return std::unexpected(std::move(*refused));
  // Every support's history on an arbitrated predicate: the predicate heads
  // its policy master on `d.epistemic`, which only arbitration mints.
  for (const auto predicate : ranked(arena, q.fromAgent, dims.belief)) {
    if (!dims.epistemic || !arena.linked(predicate, *dims.epistemic)) continue;
    for (const auto clause : ranked(arena, predicate, dims.clause))
      for (const auto support : ranked(arena, clause, dims.source))
        if (auto refused = addRecords(support))
          return std::unexpected(std::move(*refused));
  }

  plan.cells = countOwnedCells(arena, q.fromAgent, dims,
                               std::numeric_limits<std::size_t>::max() - 1);
  plan.work  = plan.cells * kTransferWorkPerCell;
  if (q.allowance) {
    const auto &allow  = *q.allowance;
    const auto ceiling = allow.epistemicCells;
    // The same sixteenth of headroom that lets an isolate land on a full arena.
    const auto allowed = ceiling + ceiling / kHeadroomDivisor;
    const auto peak    = [&](std::uint64_t limit) {
      return refuse(TransferCode::PeakCopyBudget,
                       {.savedCount = plan.cells, .limit = limit},
                       ActorError::ResourceLimit);
    };
    if (plan.cells > allowed) return peak(allowed);
    if (allow.grant) {
      if (plan.cells > allow.grant->maxCells)
        return peak(allow.grant->maxCells);
      if (allow.grant->maxBytes) {
        // The new arena interns each distinct text once; a graph whose
        // distinct texts exceed the scratch an upgrade grant allows cannot be
        // copied however few cells it has.
        const auto bytes = distinctTextBytes(arena, *vd, dims, plan);
        if (bytes > *allow.grant->maxBytes)
          return refuse(TransferCode::PeakCopyBudget,
                        {.subject    = "scratch bytes",
                         .savedCount = bytes,
                         .limit      = *allow.grant->maxBytes},
                        ActorError::ResourceLimit);
      }
      const auto needed = plan.work + q.fixedWork;
      if (needed > allow.grant->instructions)
        return refuse(
            TransferCode::WorkBudget,
            {.savedCount = needed, .limit = allow.grant->instructions},
            ActorError::ResourceLimit);
    }
  }
  return plan;
}

// -- Apply
// ---------------------------------------------------------------------

namespace {

/// Thrown inside the passes and turned into the returned refusal at the edge,
/// so a pass reads as straight-line code.
struct Refused {
  TransferRefusal refusal;
};

class Copier {
public:
  Copier(const PlanRequest &plan, const ApplyRequest &apply)
      : from_(plan.from), into_(apply.into), map_(apply.map), work_(apply.work),
        route_(plan.route), dims_(plan.fromDims), to_(apply.intoDims) {
    fvd_ = *valueDimsOf(dims_);
    const std::array<std::pair<std::optional<DimRef>, DimRef>, 4> value{
        {{dims_.vars, to_.vars},
         {dims_.values, to_.values},
         {dims_.grab, to_.grab},
         {dims_.step, to_.step}}};
    // A dimension the saved graph never links along is absent, and the
    // fallback in `fvd_` must not make it look like `vars`.
    for (const auto &[was, now] : value)
      if (was) pairs_.emplace_back(*was, now);
    ivd_ = ValueDims{.vars   = to_.vars,
                     .values = to_.values,
                     .grab   = to_.grab,
                     .step   = to_.step};
  }

  [[noreturn]] void inconsistent(std::string_view subject,
                                 std::optional<std::uint64_t> copied  = {},
                                 std::optional<std::uint64_t> planned = {}) {
    throw Refused{TransferRefusal::make(TransferCode::StateInconsistent, route_,
                                        kFailed,
                                        {.subject      = std::string(subject),
                                         .savedCount   = copied,
                                         .currentCount = planned})};
  }

  void charge(std::uint64_t amount = 1) {
    if (amount > work_)
      throw Refused{TransferRefusal::make(
          TransferCode::WorkBudget, route_, ActorError::ResourceLimit,
          {.savedCount = charges_ + amount, .limit = charges_ + work_})};
    work_ -= amount;
    charges_ += amount;
    into_.consumeWork();
  }

  /// One cell with the content and the stored value bits of @p old; links are
  /// not touched. The stored bits move as they are, so `-0.0` stays `-0.0`.
  CellRef copyCell(CellRef old) {
    charge();
    const auto made = into_.makeCell();
    // One scratch span per distinct text: field names repeat in every record,
    // and an upgrade grant allows the new arena at most 32 KiB of scratch.
    auto text = from_.textOf(old);
    auto span = spans_.find(text);
    if (span == spans_.end())
      span = spans_.emplace(std::move(text), into_.intern(text)).first;
    const std::array one{span->second};
    zigzag::expectWritten(into_.setContent(made, one));
    const auto kind = from_.valueKindOf(old);
    if (kind == xanadu::ValueKind::OpHandle ||
        kind == xanadu::ValueKind::ForeignOpHandle ||
        kind == xanadu::ValueKind::ExternRef)
      inconsistent(from_.textOf(old));
    if (kind != xanadu::ValueKind::None) {
      const auto slot = from_.slot(old);
      if (!slot) inconsistent(from_.textOf(old));
      zigzag::expectWritten(into_.setValueBits(made, kind, slot->valueBits));
    }
    map_.insert_or_assign(old, made);
    made_.emplace_back(old, made);
    return made;
  }

  /// @p root and everything it reaches along the four value dimensions. A cell
  /// already in the map (a clone occurrence mapped by pass 1, a cell the caller
  /// copied) is linked to and not entered.
  CellRef copyGraph(CellRef root) {
    if (const auto done = map_.find(root); done != map_.end())
      return done->second;
    std::vector<CellRef> order{root};
    copyCell(root);
    for (std::size_t i = 0; i < order.size(); ++i)
      for (const auto &[dim, unused] : pairs_) {
        (void)unused;
        charge();
        if (const auto next = from_.linked(order[i], dim);
            next && !map_.contains(*next)) {
          copyCell(*next);
          order.push_back(*next);
        }
      }
    for (const auto cell : order)
      for (const auto &[was, now] : pairs_) {
        charge();
        if (const auto next = from_.linked(cell, was)) {
          const auto found = map_.find(*next);
          if (found == map_.end()) inconsistent(from_.textOf(cell));
          zigzag::expectWritten(
              into_.link(map_.at(cell), now, DimVector::POS, found->second));
        }
      }
    return map_.at(root);
  }

  void run(const TranslationPlan &plan, Disposition disposition) {
    const auto start = made_.size();
    // Pass 1: profiles, their fields and their clone chains.
    for (const auto &profile : plan.profiles) {
      const auto fresh = copyCell(profile.cell);
      auto previous    = fresh;
      for (const auto view : profile.views) {
        const auto copy = copyCell(view);
        zigzag::expectWritten(
            into_.link(previous, to_.clone, DimVector::POS, copy));
        previous = copy;
      }
      // The name cells are copied, not re-minted by setField, so the map
      // holds every cell of the profile and pass 3 can see each edge.
      for (const auto name :
           zigzag::rankAfter(from_, profile.cell, fvd_.vars)) {
        charge();
        const auto copy = copyCell(name);
        appendToRank(into_, fresh, ivd_.vars, copy);
        if (const auto value = from_.linked(name, fvd_.values))
          zigzag::expectWritten(
              into_.link(copy, ivd_.values, DimVector::POS, copyGraph(*value)));
      }
      appendToRank(into_, intoAgent(), to_.trust, fresh);
    }
    // Pass 2: each record, re-linked in its old order to the mapped support or
    // profile, its `profile` field pointing at the mapped view.
    for (const auto &record : plan.records) {
      charge();
      const auto anchor = map_.find(record.anchor);
      if (anchor == map_.end()) inconsistent(from_.textOf(record.anchor));
      appendToRank(into_, anchor->second, to_.epistemic,
                   copyGraph(record.cell));
    }
    // Pass 3: every edge of every cell this routine made has both ends mapped
    // and the new cell holds the same edge.
    const std::array<std::pair<std::optional<DimRef>, DimRef>, 6> edges{
        {{dims_.vars, to_.vars},
         {dims_.values, to_.values},
         {dims_.grab, to_.grab},
         {dims_.step, to_.step},
         {dims_.clone, to_.clone},
         {dims_.epistemic, to_.epistemic}}};
    for (std::size_t i = start; i < made_.size(); ++i) {
      const auto [old, fresh] = made_[i];
      for (const auto &[was, now] : edges) {
        charge();
        if (!was) continue;
        const auto next = from_.linked(old, *was);
        if (!next) continue;
        const auto found = map_.find(*next);
        if (found == map_.end() || into_.linked(fresh, now) != found->second)
          inconsistent(from_.textOf(old));
      }
    }
    if (made_.size() - start != plan.cells)
      inconsistent("copied", made_.size() - start, plan.cells);
    // Pass 4: liveness stamps are another clock's and start again.
    if (disposition == Disposition::Resume)
      for (const auto &profile : plan.profiles) {
        const auto fresh = map_.at(profile.cell);
        for (const auto field : {kProfileLastReceive, kProfileLastContact}) {
          charge();
          if (fieldValue(into_, ivd_, fresh, field))
            setField(into_, ivd_, fresh, field, std::nullopt);
        }
      }
  }

  [[nodiscard]] std::uint64_t charges() const noexcept { return charges_; }
  [[nodiscard]] std::uint64_t cells() const noexcept { return made_.size(); }
  void bindAgent(CellRef agent) { agent_ = agent; }

private:
  [[nodiscard]] CellRef intoAgent() const { return agent_; }

  const zigzag::ArenaManifold &from_;
  zigzag::ArenaManifold &into_;
  std::unordered_map<CellRef, CellRef> &map_;
  std::uint64_t &work_;
  TransferRoute route_;
  OwnedGraphDims dims_;
  DimBinding to_;
  ValueDims fvd_{}, ivd_{};
  std::vector<std::pair<DimRef, DimRef>> pairs_;
  std::unordered_map<std::string, xanadu::PrimediaSpan> spans_;
  CellRef agent_{};
  std::uint64_t charges_{0};
  std::vector<std::pair<CellRef, CellRef>> made_;
};

} // namespace

std::expected<TranslationReport, TransferRefusal>
applyTranslation(const TranslationPlan &plan, const ApplyRequest &request) {
  if (!valueDimsOf(request.plan.fromDims)) return TranslationReport{};
  try {
    Copier copier(request.plan, request);
    copier.bindAgent(request.intoAgent);
    copier.run(plan, request.plan.disposition);
    return TranslationReport{.cells   = copier.cells(),
                             .charges = copier.charges()};
  } catch (Refused &refused) {
    return std::unexpected(std::move(refused.refusal));
  }
}

EpistemicTranslator defaultEpistemicTranslator() {
  return [](const TranslationPlan &plan, const ApplyRequest &request) {
    return applyTranslation(plan, request);
  };
}

// -- Digest
// --------------------------------------------------------------------

namespace {

class Stream {
public:
  void tag(char c) { out_.push_back(c); }
  void number(std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
  }
  void bytes(std::string_view text) {
    number(text.size());
    out_.append(text);
  }
  [[nodiscard]] const std::string &str() const { return out_; }

private:
  std::string out_;
};

class Hasher {
public:
  Hasher(const zigzag::ArenaManifold &arena, const OwnedGraphDims &dims,
         const std::unordered_map<CellRef, std::size_t> &views)
      : a_(arena), vars_(*dims.vars), values_(*dims.values), grab_(dims.grab),
        step_(dims.step), views_(views) {}

  /// The cell's data, then, when @p fields, its field names with their
  /// values, then its value and its list elements. A name cell is entered
  /// without its own `vars` link, which is its next sibling.
  void cell(CellRef c, bool fields, std::span<const std::string_view> skip = {},
            bool isName = false) {
    if (!seen_.insert(c).second) {
      s_.tag('R');
      return;
    }
    s_.tag('C');
    // A field name is text however it reads: a checkpoint types "0" as a
    // number, and the digest must not call that a different graph.
    const auto kind = isName ? xanadu::ValueKind::None : a_.valueKindOf(c);
    s_.tag(static_cast<char>(kind));
    if (kind == xanadu::ValueKind::Int64 || kind == xanadu::ValueKind::Double ||
        kind == xanadu::ValueKind::Bool)
      s_.number(a_.slot(c)->valueBits);
    else
      s_.bytes(a_.textOf(c));
    if (fields)
      for (const auto name : zigzag::rankAfter(a_, c, vars_)) {
        if (std::ranges::contains(skip, a_.textOf(name))) continue;
        s_.tag('N');
        cell(name, false, {}, true);
      }
    if (const auto v = a_.linked(c, values_)) {
      s_.tag('V');
      if (const auto view = views_.find(*v); view != views_.end()) {
        s_.tag('W');
        s_.number(view->second);
      } else {
        cell(*v, true);
      }
    }
    if (const auto first = grab_ ? a_.linked(c, *grab_) : std::nullopt) {
      s_.tag('L');
      cell(*first, true);
      if (step_)
        for (const auto e : zigzag::rankAfter(a_, *first, *step_))
          cell(e, true);
    }
  }
  Stream &out() { return s_; }

private:
  const zigzag::ArenaManifold &a_;
  DimRef vars_, values_;
  std::optional<DimRef> grab_, step_;
  const std::unordered_map<CellRef, std::size_t> &views_;
  Stream s_;
  std::unordered_set<CellRef> seen_;
};

} // namespace

std::string epistemicStateDigest(const zigzag::ArenaManifold &arena,
                                 const CellRef agent,
                                 const OwnedGraphDims &dims,
                                 const DigestMode mode) {
  const auto vd = valueDimsOf(dims);
  if (!vd) return xanadu::sha256Hex("");
  std::unordered_map<CellRef, std::size_t> views;
  const auto profiles = ranked(arena, agent, dims.trust);
  for (const auto profile : profiles) {
    std::size_t position = 0;
    for (auto v = dims.clone ? arena.linked(profile, *dims.clone)
                             : std::nullopt;
         v; v   = arena.linked(*v, *dims.clone))
      views.emplace(*v, position++);
  }
  Hasher h(arena, dims, views);
  constexpr std::array<std::string_view, 2> clock{kProfileLastReceive,
                                                  kProfileLastContact};
  const auto records = [&](CellRef anchor) {
    for (const auto record : ranked(arena, anchor, dims.epistemic)) {
      h.out().tag('R');
      h.cell(record, true);
    }
  };
  for (const auto profile : profiles) {
    h.out().tag('P');
    h.cell(profile, true,
           mode == DigestMode::ModuloClock
               ? std::span{clock}
               : std::span<const std::string_view>{});
    for (auto v = dims.clone ? arena.linked(profile, *dims.clone)
                             : std::nullopt;
         v; v   = arena.linked(*v, *dims.clone)) {
      h.out().tag('Z');
      h.out().bytes(arena.textOf(*v));
    }
    records(profile);
  }
  for (const auto predicate : ranked(arena, agent, dims.belief)) {
    if (!dims.epistemic || !arena.linked(predicate, *dims.epistemic)) continue;
    h.out().tag('B');
    h.out().bytes(arena.textOf(predicate));
    for (const auto clause : ranked(arena, predicate, dims.clause))
      for (const auto support : ranked(arena, clause, dims.source)) {
        h.out().tag('S');
        h.out().bytes(arena.textOf(support));
        records(support);
      }
  }
  if (mode == DigestMode::Full) {
    const auto probe = probeEpistemicState(arena, agent, dims);
    h.out().tag('H');
    h.out().number(static_cast<std::uint64_t>(probe.shape.value_or(-1)));
  }
  return xanadu::sha256Hex(h.out().str());
}

} // namespace xanadu::vagent::runtime
