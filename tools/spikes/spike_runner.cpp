/**
 * @file spike_runner.cpp
 * @brief Unified Spike Test Runner for Milestone M0.
 *
 * Orchestrates and executes all six exploratory spikes (S1-S6) in parallel
 * where independent, asserts all pass criteria, and logs an exhaustive report.
 */

#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct SpikeRunResult {
  std::string name;
  std::string binary;
  int exitCode{0};
  double elapsedSec{0.0};
  std::string output;
};

SpikeRunResult runSpikeBinary(const std::string &name,
                              const std::string &binaryPath) {
  SpikeRunResult res;
  res.name   = name;
  res.binary = binaryPath;

  const auto t0 = std::chrono::steady_clock::now();
  // Open pipe to capture output
  FILE *pipe = popen(binaryPath.c_str(), "r");
  if (!pipe) {
    res.exitCode = -1;
    res.output   = "Failed to launch binary: " + binaryPath;
    return res;
  }

  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    res.output += buffer;
  }
  const int status = pclose(pipe);
  const auto t1    = std::chrono::steady_clock::now();

  res.exitCode   = WEXITSTATUS(status);
  res.elapsedSec = std::chrono::duration<double>(t1 - t0).count();
  return res;
}

int main() {
  std::cout
      << "================================================================"
      << std::endl;
  std::cout << "  MILESTONE M0: EXPLORATORY SPIKES SUITE (PARALLEL EXECUTION)  "
            << std::endl;
  std::cout
      << "================================================================"
      << std::endl;

  const std::vector<std::pair<std::string, std::string>> spikes = {
      {"Spike S1 (Poomfilade Rank Indexing)", "./build/spike-s1"},
      {"Spike S2 (9Z2026 Wire Codec)", "./build/spike-s2"},
      {"Spike S3 (CRDT-ZZ Conflict Promotion)", "./build/spike-s3"},
      {"Spike S4 (Layoutfilade Stabbing)", "./build/spike-s4"},
      {"Spike S5 (ArenaManifold Epoch Toss)", "./build/spike-s5"},
      {"Spike S6 (Treefilade Fleet Rollup)", "./build/spike-s6"},
  };

  std::cout << "[Runner] Launching " << spikes.size()
            << " spikes concurrently via std::async..." << std::endl;
  const auto tSuiteStart = std::chrono::steady_clock::now();

  std::vector<std::future<SpikeRunResult>> futures;
  futures.reserve(spikes.size());

  for (const auto &[name, bin] : spikes) {
    futures.push_back(
        std::async(std::launch::async, runSpikeBinary, name, bin));
  }

  bool allPassed = true;
  std::vector<SpikeRunResult> results;
  results.reserve(futures.size());

  for (auto &f : futures) {
    results.push_back(f.get());
  }

  const auto tSuiteEnd = std::chrono::steady_clock::now();
  const double totalSuiteSec =
      std::chrono::duration<double>(tSuiteEnd - tSuiteStart).count();

  std::cout
      << "\n================================================================"
      << std::endl;
  std::cout
      << "                   INDIVIDUAL SPIKE REPORTS                     "
      << std::endl;
  std::cout
      << "================================================================\n"
      << std::endl;

  for (const auto &r : results) {
    std::cout
        << "----------------------------------------------------------------"
        << std::endl;
    std::cout << ">>> " << r.name << " (" << std::fixed << std::setprecision(2)
              << r.elapsedSec
              << "s) - Status: " << (r.exitCode == 0 ? "[PASS]" : "[FAIL]")
              << std::endl;
    std::cout
        << "----------------------------------------------------------------"
        << std::endl;
    std::cout << r.output << std::endl;
    if (r.exitCode != 0) {
      allPassed = false;
    }
  }

  std::cout
      << "================================================================"
      << std::endl;
  std::cout
      << "                       M0 SUITE SUMMARY                         "
      << std::endl;
  std::cout
      << "================================================================"
      << std::endl;
  for (const auto &r : results) {
    std::cout << "  * " << std::left << std::setw(42) << r.name << " : "
              << (r.exitCode == 0 ? "[PASS]" : "[FAIL]") << " (" << std::fixed
              << std::setprecision(2) << r.elapsedSec << "s)" << std::endl;
  }
  std::cout << "Total Wall-Clock Time: " << std::fixed << std::setprecision(2)
            << totalSuiteSec << " seconds (parallel execution)" << std::endl;
  std::cout << "Overall Milestone M0 Status: "
            << (allPassed ? "[ALL GATES PASSED]" : "[FAILED GATES]")
            << std::endl;
  std::cout
      << "================================================================"
      << std::endl;

  return allPassed ? 0 : 1;
}
