/**
 * @file vql-rank-benchmark.cpp
 * @brief Measure VQL rank query compilation and repeated VM execution.
 *
 * Arena construction and input generation are outside both timed regions.
 * The benchmark reports timings only; it makes no allocation claim.
 */
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct Options {
  std::size_t rankSize{256};
  std::size_t repeats{30};
  std::size_t cycles{1'000'000};
  bool cancel{false};
  bool functor{false};
  bool annotated{false};
};

bool parseSize(const std::string_view value, std::size_t &result) {
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return error == std::errc{} && end == value.data() + value.size();
}

std::optional<Options> parseOptions(const int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help") {
      std::cout << "Usage: vql-rank-benchmark [--rank-size N] [--repeats R] "
                   "[--cycles N] [--cancel] [--functor] [--annotated]\n";
      return std::nullopt;
    }
    if (arg == "--cancel") {
      options.cancel = true;
      continue;
    }
    if (arg == "--functor") {
      options.functor = true;
      continue;
    }
    if (arg == "--annotated") {
      options.annotated = true;
      continue;
    }
    if (arg != "--rank-size" && arg != "--repeats" && arg != "--cycles") {
      std::cerr << "unknown option: " << arg << '\n';
      return std::nullopt;
    }
    if (++i == argc) {
      std::cerr << "missing value for " << arg << '\n';
      return std::nullopt;
    }
    std::size_t value = 0;
    if (!parseSize(argv[i], value)) {
      std::cerr << "invalid nonnegative integer for " << arg << '\n';
      return std::nullopt;
    }
    if (arg == "--rank-size") {
      options.rankSize = value;
    } else if (arg == "--repeats") {
      options.repeats = value;
    } else {
      options.cycles = value;
    }
  }
  if (options.repeats == 0 || options.cycles == 0) {
    std::cerr << "--repeats and --cycles must be positive\n";
    return std::nullopt;
  }
  return options;
}

double microseconds(const Clock::duration duration) {
  return std::chrono::duration<double, std::micro>{duration}.count();
}

std::string jsonEscape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') escaped.push_back('\\');
    if (ch == '\n') {
      escaped += "\\n";
    } else if (ch == '\r') {
      escaped += "\\r";
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

std::size_t percentileIndex(const std::size_t count, const std::size_t p) {
  return std::min(count - 1, (count * p + 99) / 100 - 1);
}

} // namespace

int main(const int argc, char **argv) {
  const auto parsed = parseOptions(argc, argv);
  if (!parsed) return argc > 1 && std::string_view{argv[1]} == "--help" ? 0 : 2;
  const auto options = *parsed;

  zigzag::ArenaManifold arena;
  VortexCore core(arena);
  VortexVM vm(core);
  xanadu::vql::VQLCompiler compiler(core, vm);

  const DimRef dimension = compiler.resolveDimension("d.bench");
  std::vector<CellRef> expected;
  expected.reserve(options.rankSize);
  CellRef tail = core.home();
  for (std::size_t i = 0; i < options.rankSize; ++i) {
    const CellRef member = arena.makeCell();
    zigzag::expectWritten(arena.link(tail, dimension, DimVector::POS, member));
    expected.push_back(member);
    tail = member;
  }

  const bool usesFunctor = options.functor || options.annotated;
  std::string query      = "##/+d.bench";
  if (usesFunctor) {
    query = options.annotated ? "let $f := /+d.bench; ##/$f[true][1,-1]"
                              : "let $f := /+d.bench; ##/$f";
  }

  const std::size_t cellsBeforeCompile = arena.cellCount();
  const auto compileStart              = Clock::now();
  const auto compiled                  = compiler.compile(query);
  const auto compileDuration           = Clock::now() - compileStart;
  const std::size_t cellGrowthDuringCompile =
      arena.cellCount() - cellsBeforeCompile;
  if (!compiled.success || !compiled.entryOpcode) {
    std::cerr << "compile failed: " << compiled.errorMessage << '\n';
    return 1;
  }

  const auto cursors = vm.activeCursors();
  if (cursors.empty()) {
    std::cerr << "compiler did not create an execution cursor\n";
    return 1;
  }
  const CellRef cursor  = cursors.back();
  const auto haltInputs = core.inputsOf(compiled.generatedOpcodes.back());
  if (haltInputs.size() != 1) {
    std::cerr << "compiled rank query has unexpected result register count: "
              << haltInputs.size() << '\n';
    return 1;
  }

  std::stop_source stop;
  if (options.cancel) stop.request_stop();
  std::vector<double> runTimes;
  std::vector<std::uint64_t> cycleCounts;
  runTimes.reserve(options.repeats);
  cycleCounts.reserve(options.repeats);
  std::string status = "success";
  std::string error;
  std::size_t outputCount       = 0;
  constexpr std::size_t warmups = 5;
  const std::size_t totalRuns   = warmups + options.repeats;
  for (std::size_t runIndex = 0; runIndex < totalRuns; ++runIndex) {
    vm.setCursorOpcode(cursor, *compiled.entryOpcode);
    const auto cyclesBefore = vm.totalCycles();
    const auto start        = Clock::now();
    const auto result       = vm.run(cursor, options.cycles, stop.get_token());
    const auto duration     = Clock::now() - start;
    const auto cycles       = vm.totalCycles() - cyclesBefore;
    if (!result.success) {
      error  = result.errorMessage;
      status = error.find("Cancelled") != std::string::npos
                   ? "cancelled"
                   : (error.find("cycles") != std::string::npos ||
                              error.find("budget") != std::string::npos
                          ? "budget-exceeded"
                          : "runtime-error");
      break;
    }
    const auto output = vm.resultCells(haltInputs.front());
    if (output != expected) {
      std::cerr << "rank output mismatch on run " << runIndex << ": expected "
                << expected.size() << " members, got " << output.size() << '\n';
      return 1;
    }
    outputCount = output.size();
    if (runIndex >= warmups) {
      runTimes.push_back(microseconds(duration));
      cycleCounts.push_back(cycles);
    }
  }

  std::sort(runTimes.begin(), runTimes.end());
  const double median =
      runTimes.empty() ? 0.0 : runTimes[percentileIndex(runTimes.size(), 50)];
  const double p95 =
      runTimes.empty() ? 0.0 : runTimes[percentileIndex(runTimes.size(), 95)];
  std::uint64_t minCycles = 0;
  std::uint64_t maxCycles = 0;
  if (!cycleCounts.empty()) {
    const auto [minIt, maxIt] =
        std::minmax_element(cycleCounts.begin(), cycleCounts.end());
    minCycles = *minIt;
    maxCycles = *maxIt;
  }

  std::cout << "{\"status\":\"" << status
            << "\",\"rank_size\":" << options.rankSize
            << ",\"functor\":" << (usesFunctor ? "true" : "false")
            << ",\"annotated\":" << (options.annotated ? "true" : "false")
            << ",\"output_count\":" << outputCount
            << ",\"opcode_count\":" << compiled.generatedOpcodes.size()
            << ",\"compile_cell_growth\":" << cellGrowthDuringCompile
            << ",\"compile_us\":" << microseconds(compileDuration)
            << ",\"median_run_us\":" << median << ",\"p95_run_us\":" << p95
            << ",\"repeats_completed\":" << runTimes.size()
            << ",\"cycles_per_run_min\":" << minCycles
            << ",\"cycles_per_run_max\":" << maxCycles
            << ",\"allocations_measured\":false,\"error\":\""
            << jsonEscape(error) << "\"}\n";
  return status == "success" ? 0 : 3;
}
