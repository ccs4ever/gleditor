/**
 * @file vagent_epistemic_policy_test.cpp
 * @brief P19 I3-1a: the `arbitrated by` policy, its checker and lowering.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;

constexpr std::string_view kContinuous =
    R"(predicate at(key who: Str, x: Float, y: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.7,
    prior_alpha: 1, prior_beta: 1.5, clock: "actor_mapped", max_age: 3s,
    decay: 1500ms, cluster_tolerance: 0.25, frame: "map_enu",
    units: "metres", min_variance: 0.0001, max_variance: 100.0,
    max_slew: 15))";

constexpr std::string_view kDiscrete =
    R"(predicate door(key id: Str, open: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s))";

std::expected<CheckedUnit, std::vector<CheckError>>
checkSource(std::string_view source) {
  auto unit = parseUnit(source);
  EXPECT_TRUE(unit) << (unit ? "" : formatParseError(unit.error()));
  if (!unit) return std::unexpected(std::vector<CheckError>{});
  return check(*unit);
}

std::vector<CheckErrorCode> codes(std::string_view source) {
  std::vector<CheckErrorCode> out;
  auto checked = checkSource(source);
  if (checked) return out;
  for (const auto &e : checked.error()) out.push_back(e.code);
  return out;
}

bool refuses(std::string_view source, CheckErrorCode code) {
  return std::ranges::contains(codes(source), code);
}

std::string withAgent(std::string_view decls, std::string_view body = "") {
  return std::string(decls) + "\nagent A {\n" + std::string(body) + "\n}\n";
}

const ArbitrationPolicy &policyOf(const CheckedUnit &unit,
                                  const std::string &predicate) {
  return *unit.predicates.at(predicate).policy;
}

TEST(VagentEpistemicPolicy, ContinuousPolicyIsTypedAcrossLines) {
  auto checked = checkSource(withAgent(kContinuous));
  ASSERT_TRUE(checked);
  const auto &p = policyOf(*checked, "at");
  EXPECT_EQ(p.strategy, ArbitrationStrategy::Truest);
  EXPECT_TRUE(p.continuous);
  EXPECT_EQ(p.model, "beta_reputation");
  EXPECT_DOUBLE_EQ(p.minConfidence, 0.7);
  // An integer literal widens to the Float constant it denotes.
  EXPECT_DOUBLE_EQ(p.priorAlpha, 1.0);
  EXPECT_EQ(p.clock, ObservationClock::ActorMapped);
  EXPECT_EQ(p.maxAgeMs, 3000);
  EXPECT_EQ(p.decayMs, 1500);
  EXPECT_EQ(p.clockSkewMs, 0);
  EXPECT_EQ(p.frame, "map_enu");
  EXPECT_EQ(p.maxSlew, 15.0);
  EXPECT_FALSE(p.innovation);
}

TEST(VagentEpistemicPolicy, DiscretePolicyAndPlainPredicatesAreUnchanged) {
  auto checked = checkSource(
      withAgent(std::string(kDiscrete) + "\npredicate plain(x: Int)"));
  ASSERT_TRUE(checked);
  EXPECT_FALSE(policyOf(*checked, "door").continuous);
  EXPECT_EQ(policyOf(*checked, "door").clock, ObservationClock::Wall);
  EXPECT_FALSE(checked->predicates.at("plain").policy);
}

TEST(VagentEpistemicPolicy, ContextualWordsStayUsableAsNames) {
  // `arbitrated` and the strategy words mean something only after a
  // predicate's parameter list.
  EXPECT_TRUE(checkSource(withAgent("predicate arbitrated(x: Int)\n"
                                    "predicate truest(x: Int)\n"
                                    "predicate consensus(x: Int)")));
}

TEST(VagentEpistemicPolicy, HashIgnoresKeyOrderAndTracksEveryValue) {
  auto a = checkSource(withAgent(kDiscrete));
  auto b = checkSource(withAgent(
      R"(predicate door(key id: Str, open: Bool)
  arbitrated by ranked(max_age: 10s, clock: "wall", prior_beta: 2,
    prior_alpha: 2.0, min_confidence: 0.5, model: "beta_reputation"))"));
  auto c = checkSource(withAgent(
      R"(predicate door(key id: Str, open: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 11s))"));
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(policyOf(*a, "door"), policyOf(*b, "door"));
  EXPECT_EQ(policyHash(policyOf(*a, "door")), policyHash(policyOf(*b, "door")));
  EXPECT_NE(policyHash(policyOf(*a, "door")), policyHash(policyOf(*c, "door")));
  EXPECT_EQ(policyHash(policyOf(*a, "door")).size(), 64U);
}

TEST(VagentEpistemicPolicy, EveryKeyRuleRefusesWithItsCode) {
  const std::string base = R"(predicate door(key id: Str, open: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s)";
  const auto bad         = [&](std::string_view extra) {
    return withAgent(base + std::string(extra) + ")");
  };
  constexpr auto invalid = CheckErrorCode::InvalidEpistemicPolicy;
  EXPECT_TRUE(refuses(bad(", colour: \"red\""), invalid));        // unknown
  EXPECT_TRUE(refuses(bad(", max_age: 5s"), invalid));            // duplicate
  EXPECT_TRUE(refuses(bad(", clock_skew: 3"), invalid));          // type
  EXPECT_TRUE(refuses(bad(", score_tolerance: \"x\""), invalid)); // type
  EXPECT_TRUE(refuses(bad(", frame: \"map\""), invalid)); // continuous-only
  EXPECT_TRUE(refuses(bad(", max_slew: 1.0"), invalid));  // continuous-only
  const auto replaced = [&](std::string_view from, std::string_view to) {
    auto text = base;
    text.replace(text.find(from), from.size(), to);
    return withAgent(text + ")");
  };
  EXPECT_TRUE(refuses(replaced("0.5", "1.5"), invalid));
  EXPECT_TRUE(refuses(replaced("prior_alpha: 2", "prior_alpha: 0"), invalid));
  EXPECT_TRUE(refuses(replaced("\"wall\"", "\"gps\""), invalid));
  EXPECT_TRUE(refuses(replaced("\"beta_reputation\"", "\"sgd\""), invalid));
  EXPECT_TRUE(refuses(replaced("ranked", "loudest"), invalid));
  EXPECT_TRUE(refuses(replaced("ranked", "consensus"), invalid)); // discrete
  EXPECT_TRUE(refuses(replaced(", max_age: 10s", ""), invalid));  // missing
  EXPECT_TRUE(refuses(replaced(", clock: \"wall\"", ""), invalid));
  EXPECT_TRUE(refuses(replaced("prior_beta: 2, ", ""), invalid));
}

TEST(VagentEpistemicPolicy, ContinuousRequirementsAndVarianceDomain) {
  const std::string head = "predicate at(key who: Str, x: Float)\n"
                           "  arbitrated by consensus(model: "
                           "\"beta_reputation\", min_confidence: 0.5, "
                           "prior_alpha: 1, prior_beta: 1, clock: \"wall\", "
                           "max_age: 1s";
  const auto with        = [&](std::string_view keys) {
    return withAgent(head + std::string(keys) + ")");
  };
  const std::string full = ", cluster_tolerance: 0.5, frame: \"enu\", "
                           "units: \"m\", min_variance: 0.01, "
                           "max_variance: 10.0";
  EXPECT_TRUE(checkSource(with(full)));
  EXPECT_TRUE(refuses(with(", frame: \"enu\", units: \"m\", "
                           "min_variance: 0.01, max_variance: 10.0"),
                      CheckErrorCode::MissingClusterTolerance));
  EXPECT_TRUE(refuses(with(", cluster_tolerance: 0.5, units: \"m\", "
                           "min_variance: 0.01, max_variance: 10.0"),
                      CheckErrorCode::InvalidEpistemicPolicy));
  constexpr auto invalid = CheckErrorCode::InvalidEpistemicPolicy;
  const auto variance    = [&](std::string_view lo, std::string_view hi) {
    return with(", cluster_tolerance: 0.5, frame: \"enu\", units: \"m\", "
                      "min_variance: " +
                   std::string(lo) + ", max_variance: " + std::string(hi));
  };
  EXPECT_TRUE(refuses(variance("1e-200", "1.0"), invalid));
  EXPECT_TRUE(refuses(variance("1.0", "1e200"), invalid));
  EXPECT_TRUE(refuses(variance("2.0", "1.0"), invalid));
  EXPECT_TRUE(checkSource(variance("1e-150", "1e150")));
  EXPECT_TRUE(refuses(with(full + ", max_cluster_diameter: 0.1"), invalid));
}

TEST(VagentEpistemicPolicy, ProgramWritesAndAliasFiltersAreIngressOnly) {
  constexpr auto ingress = CheckErrorCode::EpistemicIngressOnly;
  const std::string decls =
      std::string(kDiscrete) + "\npredicate plain(id: Str)\nevent Go { }";
  const auto on = [&](std::string_view body) {
    return withAgent(decls, "on Go { " + std::string(body) + " }");
  };
  EXPECT_TRUE(refuses(on("believe door(\"a\", true)"), ingress));
  EXPECT_TRUE(refuses(on("forget door(\"a\", true)"), ingress));
  EXPECT_TRUE(refuses(on("revise door(\"a\", false)"), ingress));
  EXPECT_TRUE(
      refuses(withAgent(decls, "on Initialize { believe door(\"a\", true) }"),
              ingress));
  EXPECT_TRUE(
      refuses(on("if believes door(\"a\", ?o) from self then ()"), ingress));
  // Reading and plain predicates are untouched.
  EXPECT_TRUE(checkSource(on("if believes door(\"a\", ?o) then ()")));
  EXPECT_TRUE(checkSource(on("believe plain(\"a\")")));
  EXPECT_TRUE(checkSource(on("if believes plain(?p) from self then ()")));
  EXPECT_TRUE(checkSource(withAgent(decls, "on +door(?i, ?o) { () }")));
}

TEST(VagentEpistemicPolicy, CompiledPredicateHeadsItsPolicyMaster) {
  auto program = CompiledProgram::fromSource(withAgent(kContinuous));
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  const auto *context = host.executor().context(*pid);
  const auto agent    = host.executor().agentCell(*pid);
  ASSERT_TRUE(context && agent);
  auto &arena     = const_cast<zigzag::ArenaManifold &>(context->arena);
  const auto dims = registerDimensions(arena);
  std::optional<zigzag::CellRef> predicate;
  for (const auto cell : zigzag::rankAfter(arena, *agent, dims.belief))
    if (arena.textOf(cell) == "at") predicate = cell;
  ASSERT_TRUE(predicate);
  const auto master = arena.linked(*predicate, dims.epistemic);
  ASSERT_TRUE(master);
  EXPECT_EQ(arena.textOf(*master), "EpistemicPolicy");
  std::map<std::string, zigzag::CellRef> fields;
  auto name = arena.linked(*master, context->core.dims().vars);
  while (name) {
    if (const auto value = arena.linked(*name, context->core.dims().values))
      fields.emplace(arena.textOf(*name), *value);
    name = arena.linked(*name, context->core.dims().vars);
  }
  EXPECT_EQ(arena.textOf(fields.at("strategy")), "truest");
  EXPECT_EQ(arena.asDouble(fields.at("min_confidence")), 0.7);
  EXPECT_EQ(arena.asInt64(fields.at("max_age")), 3000);
  EXPECT_EQ(arena.asBool(fields.at("continuous")), true);
  EXPECT_FALSE(fields.contains("innovation")); // absent, not a sentinel
  auto checked = checkSource(withAgent(kContinuous));
  ASSERT_TRUE(checked);
  EXPECT_EQ(arena.textOf(fields.at("hash")),
            policyHash(policyOf(*checked, "at")));
}

TEST(VagentEpistemicPolicy, PeerTellCannotWriteAnArbitratedPredicate) {
  constexpr auto source = R"vagent(
predicate ready(key who: Str, ok: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 5s)
agent Worker {
  on Initialize { tell parent ready("x", true) }
}
agent Hub {
  var plus: Int := 0
  on Initialize { spawn Worker() in inner }
  on +ready(?w, ?o) { plus := plus + 1 }
}
)vagent";
  auto program          = CompiledProgram::fromSource(source);
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto hub = host.launch("Hub", "hub");
  ASSERT_TRUE(hub);
  host.pump();
  const auto *context = host.executor().context(*hub);
  const auto agent    = host.executor().agentCell(*hub);
  ASSERT_TRUE(context && agent);
  const auto dims = valueDims(context->core);
  const auto plus = fieldValue(context->arena, dims, *agent, "plus");
  ASSERT_TRUE(plus);
  EXPECT_EQ(context->arena.asInt64(*plus), 0);
}

// Every key of a continuous policy, so each one can be varied or dropped.
std::map<std::string, std::string> fullPolicy() {
  return {{"model", "\"beta_reputation\""},
          {"min_confidence", "0.7"},
          {"prior_alpha", "1"},
          {"prior_beta", "2"},
          {"clock", "\"wall\""},
          {"max_age", "3s"},
          {"clock_skew", "10ms"},
          {"decay", "1s"},
          {"alignment", "50ms"},
          {"score_tolerance", "0.001"},
          {"allow_untested", "true"},
          {"cluster_tolerance", "0.25"},
          {"frame", "\"enu\""},
          {"units", "\"m\""},
          {"min_variance", "0.01"},
          {"max_variance", "10.0"},
          {"max_cluster_diameter", "1.0"},
          {"max_slew", "15"},
          {"innovation", "5.99"}};
}

std::string render(const std::map<std::string, std::string> &keys,
                   std::string_view strategy = "truest") {
  std::string args;
  for (const auto &[key, value] : keys)
    args += (args.empty() ? "" : ", ") + key + ": " + value;
  return withAgent("predicate at(key who: Str, x: Float)\n  arbitrated by " +
                   std::string(strategy) + "(" + args + ")");
}

std::optional<std::string> hashOf(const std::string &source) {
  auto checked = checkSource(source);
  if (!checked) return std::nullopt;
  return policyHash(policyOf(*checked, "at"));
}

TEST(VagentEpistemicPolicy, HashChangesWithEveryFieldAndOptionalPresence) {
  const auto base = hashOf(render(fullPolicy()));
  ASSERT_TRUE(base);
  const std::map<std::string, std::string> changed{
      {"min_confidence", "0.71"},
      {"prior_alpha", "1.5"},
      {"prior_beta", "3"},
      {"clock", "\"actor_mapped\""},
      {"max_age", "4s"},
      {"clock_skew", "11ms"},
      // Absent from the base, so this also tests its presence.
      {"max_uncertainty", "2s"},
      {"decay", "2s"},
      {"alignment", "60ms"},
      {"score_tolerance", "0.002"},
      {"allow_untested", "false"},
      {"cluster_tolerance", "0.3"},
      {"frame", "\"ned\""},
      {"units", "\"km\""},
      {"min_variance", "0.02"},
      {"max_variance", "11.0"},
      {"max_cluster_diameter", "1.5"},
      {"max_slew", "16"},
      {"innovation", "7.81"}};
  for (const auto &[key, value] : changed) {
    auto keys    = fullPolicy();
    keys[key]    = value;
    const auto h = hashOf(render(keys));
    ASSERT_TRUE(h) << key;
    EXPECT_NE(*h, *base) << key;
  }
  auto ranked = hashOf(render(fullPolicy(), "ranked"));
  ASSERT_TRUE(ranked);
  EXPECT_NE(*ranked, *base);
  // Dropping an optional key changes the hash unless its resolved default
  // equals the value dropped.
  for (const auto *key :
       {"clock_skew", "decay", "alignment", "score_tolerance", "allow_untested",
        "max_cluster_diameter", "max_slew", "innovation"}) {
    auto keys = fullPolicy();
    keys.erase(key);
    const auto h = hashOf(render(keys));
    ASSERT_TRUE(h) << key;
    EXPECT_NE(*h, *base) << key;
  }
  auto explicitDefaults               = fullPolicy();
  explicitDefaults["score_tolerance"] = "1e-9";
  explicitDefaults["clock_skew"]      = "0s";
  explicitDefaults["max_uncertainty"] = "3s"; // max_age
  explicitDefaults["allow_untested"]  = "false";
  auto omitted                        = fullPolicy();
  for (const auto *key : {"score_tolerance", "clock_skew", "allow_untested"})
    omitted.erase(key);
  EXPECT_EQ(hashOf(render(explicitDefaults)), hashOf(render(omitted)));
  // An Int literal and the Float it denotes are one constant.
  auto asFloat           = fullPolicy();
  asFloat["prior_alpha"] = "1.0";
  EXPECT_EQ(hashOf(render(asFloat)), base);
  // +0.0 and -0.0 are one constant too.
  auto zero = fullPolicy(), negativeZero = fullPolicy();
  zero["score_tolerance"]         = "0.0";
  negativeZero["score_tolerance"] = "-0.0";
  EXPECT_EQ(hashOf(render(zero)), hashOf(render(negativeZero)));
}

TEST(VagentEpistemicPolicy, EveryDomainBoundaryAcceptsOrRefuses) {
  struct Case {
    const char *key;
    const char *value;
    bool accepted;
  };
  const Case cases[] = {
      {"min_confidence", "0", true},
      {"min_confidence", "1", true},
      {"min_confidence", "-0.1", false},
      {"min_confidence", "1.000001", false},
      {"prior_alpha", "1e12", true},
      {"prior_alpha", "0", false},
      {"prior_alpha", "-1", false},
      {"prior_alpha", "1.1e12", false},
      {"prior_beta", "0.5", true},
      {"prior_beta", "0", false},
      {"max_age", "1ms", true},
      {"max_age", "0s", false},
      {"max_age", "-1s", false},
      {"max_age", "1000000000001ms", false},
      {"clock_skew", "0s", true},
      {"clock_skew", "-1ms", false},
      {"max_uncertainty", "3s", true}, // max_age
      {"max_uncertainty", "1ms", true},
      {"max_uncertainty", "3001ms", false},
      {"max_uncertainty", "0s", false},
      {"decay", "0s", false},
      {"alignment", "0s", true},
      {"alignment", "-1ms", false},
      {"score_tolerance", "0", true},
      {"score_tolerance", "-1e-9", false},
      {"cluster_tolerance", "0", false},
      {"cluster_tolerance", "-1", false},
      {"max_cluster_diameter", "0.25", true},
      {"max_cluster_diameter", "0.2", false},
      {"max_slew", "0", false},
      {"max_slew", "-1", false},
      {"innovation", "0", false},
      {"frame", "\"\"", false},
      {"units", "\"\"", false},
      {"allow_untested", "1", false},
      {"clock", "\"gps\"", false},
      // Integers widen only while the double is exact (2^53 + 1 is not).
      {"max_slew", "9007199254740993", false},
      {"max_slew", "9007199254740992", true},
  };
  for (const auto &c : cases) {
    auto keys         = fullPolicy();
    keys[c.key]       = c.value;
    const auto source = render(keys);
    if (c.accepted) {
      EXPECT_TRUE(checkSource(source)) << c.key << " " << c.value;
    } else {
      EXPECT_TRUE(refuses(source, CheckErrorCode::InvalidEpistemicPolicy))
          << c.key << " " << c.value;
    }
  }
  // Consensus can never expose untested hypotheses.
  EXPECT_TRUE(refuses(render(fullPolicy(), "consensus"),
                      CheckErrorCode::InvalidEpistemicPolicy));
  auto tested              = fullPolicy();
  tested["allow_untested"] = "false";
  EXPECT_TRUE(checkSource(render(tested, "consensus")));
  // A non-numeric value cannot be negated.
  auto negatedText     = fullPolicy();
  negatedText["frame"] = "-\"enu\"";
  EXPECT_FALSE(parseUnit(render(negatedText)));
}

TEST(VagentEpistemicPolicy, PolicyMasterSurvivesResumeAndSamePolicyUpgrade) {
  const std::string source =
      withAgent(kDiscrete, "var n: Int := 1\n  on Ping { n := n + 1 }") +
      "event Ping { }\n";
  auto program = CompiledProgram::fromSource(source);
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  const auto masterOf =
      [](LocalHost &h,
         const xanadu::identity::ActorPid &p) -> std::optional<std::string> {
    const auto *context = h.executor().context(p);
    const auto agent    = h.executor().agentCell(p);
    if (!context || !agent) return std::nullopt;
    auto &arena     = const_cast<zigzag::ArenaManifold &>(context->arena);
    const auto dims = registerDimensions(arena);
    for (const auto cell : zigzag::rankAfter(arena, *agent, dims.belief))
      if (arena.textOf(cell) == "door")
        if (const auto master = arena.linked(cell, dims.epistemic))
          return arena.textOf(*master);
    return std::nullopt;
  };
  EXPECT_EQ(masterOf(host, *pid), "EpistemicPolicy");
  // The same program as a new version keeps its policy, so it upgrades.
  auto same = CompiledProgram::fromSource(source + "\n");
  ASSERT_TRUE(same) << same.error();
  auto held = host.retainProgram(std::move(*same));
  ASSERT_TRUE(host.upgrade(*pid, held));
  EXPECT_EQ(masterOf(host, *pid), "EpistemicPolicy");
  // A changed policy is a breaking change, refused without mutation.
  std::string changed = source;
  changed.replace(changed.find("10s"), 3, "12s");
  auto next = CompiledProgram::fromSource(changed);
  ASSERT_TRUE(next) << next.error();
  auto heldNext     = host.retainProgram(std::move(*next));
  const auto before = host.executor().context(*pid);
  EXPECT_FALSE(host.upgrade(*pid, heldNext));
  EXPECT_EQ(host.executor().context(*pid), before);
  // Resume on another host recompiles the master beside the predicate.
  const auto checkpoint = host.executor().checkpoint(*pid);
  ASSERT_TRUE(checkpoint);
  auto again = CompiledProgram::fromSource(source + "\n");
  ASSERT_TRUE(again) << again.error();
  LocalHost later(std::move(*again));
  const auto resumed = later.executor().resume(*checkpoint);
  ASSERT_TRUE(resumed);
  later.pump();
  EXPECT_EQ(masterOf(later, *resumed), "EpistemicPolicy");
}
TEST(VagentEpistemicPolicy, UpgradeSchemaCarriesAndComparesThePolicy) {
  // `CheckedUnit` refers to its parsed unit, which must outlive the schema
  // builder that reads it.
  const auto schemaOf = [](std::string_view source) {
    auto unit = parseUnit(source);
    EXPECT_TRUE(unit);
    auto checked = check(*unit);
    EXPECT_TRUE(checked);
    auto schema = makeUpgradeSchema(*checked);
    EXPECT_TRUE(schema);
    return *schema;
  };
  const auto before = schemaOf(withAgent(kDiscrete));
  ASSERT_TRUE(before.predicates.at("door").epistemicPolicy);
  EXPECT_EQ(before.predicates.at("door").epistemicPolicy->hash.size(), 64U);
  const auto policyChanges = [](const auto &changes) {
    return std::ranges::count_if(changes, [](const UpgradeChange &c) {
      return c.kind == UpgradeChangeKind::EpistemicPolicy;
    });
  };
  EXPECT_EQ(policyChanges(compareUpgradeSchemas(before, before)), 0);
  std::string changed(kDiscrete);
  changed.replace(changed.find("10s"), 3, "12s");
  const auto after =
      compareUpgradeSchemas(before, schemaOf(withAgent(changed)));
  ASSERT_EQ(policyChanges(after), 1);
  const auto change = std::ranges::find(
      after, UpgradeChangeKind::EpistemicPolicy, &UpgradeChange::kind);
  EXPECT_TRUE(upgradeChangeRequiresRefusal(*change));
  EXPECT_NE(change->oldHash, change->newHash);
  // Dropping the policy is a change too, never a silent downgrade.
  const auto dropped = compareUpgradeSchemas(
      before, schemaOf(withAgent("predicate door(key id: Str, open: Bool)")));
  EXPECT_EQ(policyChanges(dropped), 1);
}

} // namespace
