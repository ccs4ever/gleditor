/**
 * @file noisy_sensor_sim.hpp
 * @brief A seeded, deterministic simulator of noisy sensors reporting one
 *        target's position to an arbitrated `at(key who: Str, x: Float)`
 *        (P19 I3-1b, I3-1c acceptance; I4 reuses its ground truth).
 *
 * Its distributions are stated assumptions, not a model of any deployed
 * sensor:
 *
 * - GPS: Gaussian noise of the variance it reports, dropouts, and multipath
 *   spikes it does not admit to.
 * - Odometry: a random-walk drift whose reported variance grows each step,
 *   until it passes the policy's ceiling.
 * - Stuck: the same reading, honestly reported, forever.
 * - Every sensor: a fixed clock skew, rare large forward skews, reboots that
 *   restart its sequence, bump its incarnation and silence it for a step,
 *   relayed duplicates, and bursts that outrun its ingress share.
 *
 * Uniform and Gaussian draws are built from `std::mt19937_64`'s raw output,
 * whose sequence the standard fixes, so a seed means the same run with every
 * standard library.
 */
#ifndef TESTS_XUZZ_NOISY_SENSOR_SIM_HPP
#define TESTS_XUZZ_NOISY_SENSOR_SIM_HPP

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"

namespace xanadu::vagent::runtime::epistemic_test {

/// The host should bind @p principal at @p incarnation: the sensor rebooted.
struct Rebind {
  std::string principal;
  std::uint64_t incarnation{};
};
/// Pump the executor: everything before this was delivered together.
struct Pump {};
struct Report {
  CheckedObservation observation;
  /// A copy of an earlier report, through a relay.
  bool relayCopy{false};
};
using SimEvent = std::variant<Report, Rebind, Pump>;

enum class SensorKind : std::uint8_t { Gps, Odometry, Stuck };

struct SimSensor {
  SensorKind kind;
  std::string principal;
  std::string alias;
  std::uint64_t incarnation{1};
  std::uint64_t sequence{0};
  std::uint64_t events{0};
  double skew{};
  double drift{};
  double variance{};
  std::optional<double> stuckAt;
};

class NoisySensorSim {
public:
  /// Simulated seconds between steps.
  static constexpr double kStep = 0.05;
  // Per-step probabilities; small enough that most steps are ordinary.
  static constexpr double kDropout     = 0.10;
  static constexpr double kMultipath   = 0.05;
  static constexpr double kForwardJump = 0.02;
  static constexpr double kReboot      = 0.04;
  static constexpr double kRelay       = 0.10;
  static constexpr double kBurst       = 0.03;
  /// Above the default per-source ingress share of 8.
  static constexpr int kBurstLength = 12;

  /// @p wallStart is the wall second of the simulation's first step; the
  /// caller keeps the run inside the policy's `max_age` of it.
  NoisySensorSim(std::uint64_t seed, double wallStart)
      : rng_(seed), wallStart_(wallStart) {
    sensors_ = {
        SimSensor{SensorKind::Gps, std::string(64, 'a'), "sim:gps"},
        SimSensor{SensorKind::Odometry, std::string(64, 'b'), "sim:odometry"},
        SimSensor{SensorKind::Stuck, std::string(64, 'c'), "sim:stuck"},
    };
    for (auto &s : sensors_) s.skew = 0.01 * (uniform() - 0.5);
    sensors_[1].variance = 1.0;
  }

  [[nodiscard]] const std::vector<SimSensor> &sensors() const {
    return sensors_;
  }
  /// The target's true position at the current step.
  [[nodiscard]] double truth() const { return 3.0 + 1.5 * now(); }

  /// One step: every sensor's reports, ending in a pump.
  std::vector<SimEvent> step() {
    std::vector<SimEvent> out;
    for (auto &s : sensors_) {
      if (chance(kReboot)) {
        ++s.incarnation;
        s.sequence = 0;
        out.emplace_back(Rebind{s.principal, s.incarnation});
        // Booting takes the rest of the step, so the advance alone must
        // retire the old boot's readings.
        continue;
      }
      const int n = chance(kBurst) ? kBurstLength : 1;
      for (int i = 0; i < n; ++i) {
        auto report = read(s);
        if (!report) continue;
        const bool relay = chance(kRelay);
        out.emplace_back(Report{*report});
        if (relay) {
          auto copy      = *report;
          copy.relayedBy = std::string(64, 'f');
          out.emplace_back(Report{std::move(copy), true});
        }
      }
    }
    out.emplace_back(Pump{});
    ++step_;
    return out;
  }

private:
  double now() const { return kStep * static_cast<double>(step_); }
  double uniform() { return static_cast<double>(rng_() >> 11) * 0x1.0p-53; }
  bool chance(double p) { return uniform() < p; }
  double gaussian() {
    // Box-Muller; 1 - u keeps the logarithm finite.
    const double u = 1.0 - uniform(), v = uniform();
    return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * std::numbers::pi * v);
  }

  std::optional<CheckedObservation> read(SimSensor &s) {
    double x        = truth();
    double variance = 0.0;
    switch (s.kind) {
    case SensorKind::Gps:
      if (chance(kDropout)) return std::nullopt;
      variance = 4.0;
      x += 2.0 * gaussian();
      if (chance(kMultipath)) x += 30.0;
      break;
    case SensorKind::Odometry:
      s.drift += 0.1 * gaussian();
      s.variance *= 1.15;
      variance = s.variance;
      x += s.drift;
      break;
    case SensorKind::Stuck:
      if (!s.stuckAt) s.stuckAt = x;
      x        = *s.stuckAt;
      variance = 1.0;
      break;
    }
    CheckedObservation o;
    o.principal   = s.principal;
    o.incarnation = s.incarnation;
    o.eventId     = s.alias + ":" + std::to_string(s.incarnation) + ":" +
                std::to_string(++s.events);
    o.sequence  = ++s.sequence;
    o.predicate = "at";
    o.tuple     = {WireValue::text("target"), WireValue::real(x)};
    o.acquiredAt =
        wallStart_ + now() + s.skew + (chance(kForwardJump) ? 30.0 : 0.0);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = variance;
    return o;
  }

  std::mt19937_64 rng_;
  double wallStart_;
  std::uint64_t step_{0};
  std::vector<SimSensor> sensors_;
};

} // namespace xanadu::vagent::runtime::epistemic_test

#endif // TESTS_XUZZ_NOISY_SENSOR_SIM_HPP
