/**
 * @file vagent_epistemic_noise_test.cpp
 * @brief P19 I3-1b/1c acceptance: ingestion's invariants hold for seeded
 *        noisy sensors (GPS, drifting odometry, a stuck sensor, skew,
 *        reboots, relays and bursts).
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "epistemic_invariants.hpp"
#include "noisy_sensor_sim.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;

constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
agent A {
  var adds: Int := 0
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

constexpr std::string_view kPredicates[] = {"at"};
/// An ingestion turn is about 12k VM cycles, so the default run is short;
/// GLEDITOR_NOISE_SEEDS widens it for acceptance evidence.
constexpr std::uint64_t kDefaultSeeds = 3;
/// Enough steps for odometry's growing variance to pass the policy's
/// ceiling, and short enough to stay inside `max_age`.
constexpr int kSteps = 40;

std::uint64_t seeds() {
  const char *env = std::getenv("GLEDITOR_NOISE_SEEDS");
  if (env == nullptr) return kDefaultSeeds;
  const auto n = std::strtoull(env, nullptr, 10);
  return n == 0 ? kDefaultSeeds : n;
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string codeOf(const IngestOutcome &o) {
  switch (o.kind) {
  case IngestOutcome::Kind::Pending:
    return "Pending";
  case IngestOutcome::Kind::Committed:
    return "Committed";
  case IngestOutcome::Kind::Duplicate:
    return "Duplicate";
  case IngestOutcome::Kind::Refused:
    return o.code.value_or("Refused");
  }
  return "?";
}

TEST(VagentEpistemicNoise, InvariantsHoldForEverySeed) {
  std::map<std::string, std::size_t> seen;
  for (std::uint64_t seed = 1, n = seeds(); seed <= n; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    LocalHost host(std::move(*program));
    const auto pid = host.launch("A", "a");
    ASSERT_TRUE(pid);
    host.pump();
    auto &executor = host.executor();
    executor.setWallClockSynchronised(true);
    NoisySensorSim sim(seed, wallSeconds() - 5.0);
    for (const auto &s : sim.sensors())
      ASSERT_TRUE(
          executor.bindSource(*pid, {s.alias, s.principal, s.incarnation, "at",
                                     std::chrono::hours(1)}));
    host.pump();
    for (int step = 0; step < kSteps; ++step) {
      const EpistemicView view(executor, *pid, kPredicates);
      const auto before = view.snapshot();
      std::vector<Report> sent;
      bool rebound = false;
      for (auto &event : sim.step()) {
        if (const auto *r = std::get_if<Report>(&event)) {
          const auto receipt = executor.observe(*pid, r->observation);
          if (receipt)
            sent.push_back(*r);
          else
            ++seen[std::string(epistemicRefusalName(receipt.error()))];
        } else if (const auto *b = std::get_if<Rebind>(&event)) {
          const auto alias = [&] {
            for (const auto &s : sim.sensors())
              if (s.principal == b->principal) return s.alias;
            return std::string{};
          }();
          ASSERT_TRUE(
              executor.bindSource(*pid, {alias, b->principal, b->incarnation,
                                         "at", std::chrono::hours(1)}));
          rebound = true;
        } else {
          host.pump();
        }
      }
      const EpistemicView after(executor, *pid, kPredicates);
      const auto broken = after.violation();
      ASSERT_FALSE(broken) << "step " << step << ": " << *broken;
      // Every queued report has a typed outcome once the pump returns.
      std::map<std::string, IngestOutcome::Kind> committedBy;
      bool anyCommitted = false;
      for (const auto &r : sent) {
        const auto &o = r.observation;
        const auto outcome =
            executor.ingestOutcome(*pid, o.principal, o.incarnation, o.eventId);
        ASSERT_TRUE(outcome) << o.eventId;
        ASSERT_NE(outcome->kind, IngestOutcome::Kind::Pending) << o.eventId;
        ++seen[codeOf(*outcome)];
        // A relayed copy shares its original's key, and a committed entry
        // stays committed; the copy's own turn is counted as a duplicate.
        anyCommitted |= outcome->kind == IngestOutcome::Kind::Committed ||
                        outcome->kind == IngestOutcome::Kind::Duplicate;
      }
      // A step that committed nothing and advanced no incarnation changed
      // no belief, support or record.
      if (!anyCommitted && !rebound)
        EXPECT_EQ(after.snapshot().beliefs, before.beliefs) << "step " << step;
    }
    // Relayed copies of committed reports were recognised as retries.
    seen["Duplicate"] +=
        executor.stat(*pid, std::string(kStatRefusedPrefix) +
                                std::string(kTurnResultDuplicate));
    const EpistemicView view(executor, *pid, kPredicates);
    for (const auto &s : sim.sensors())
      if (const auto recorded = view.incarnationOf(s.principal))
        EXPECT_EQ(*recorded, static_cast<std::int64_t>(s.incarnation))
            << s.alias;
    EXPECT_LE(*executor.epistemicCells(*pid), AgentSettings{}.epistemic.cells);
  }
  // Every kind of noise the simulator makes reached the executor somewhere
  // across the seeds, and real readings still got through.
  for (const auto *code : {"Committed", "Duplicate", "InvalidVariance",
                           "ObservationTimeMismatch", "IngressBackpressure"})
    EXPECT_GT(seen[code], 0U) << code;
  EXPECT_EQ(seen["IngestionFault"], 0U);
  EXPECT_EQ(seen["Pending"], 0U);
  for (const auto &[code, n] : seen)
    std::cout << "[ noise    ] " << code << ": " << n << "\n";
}

} // namespace
