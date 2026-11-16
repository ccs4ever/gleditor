#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
std::atomic<bool> countAllocations{false};
std::atomic<std::uint64_t> allocationCount{0};
std::atomic<std::uint64_t> requestedBytes{0};

void noteAllocation(std::size_t bytes) noexcept {
  if (countAllocations.load(std::memory_order_relaxed)) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    requestedBytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void *allocate(std::size_t bytes) {
  if (void *memory = std::malloc(std::max(bytes, std::size_t{1}))) {
    noteAllocation(bytes);
    return memory;
  }
  throw std::bad_alloc();
}

void *allocateAligned(std::size_t bytes, std::size_t alignment) {
  // POSIX requires alignment to be a pointer-size multiple; aligned new and
  // PMR callers may request a smaller fundamental alignment.
  const auto providerAlignment = std::max(alignment, sizeof(void *));
#if defined(_WIN32)
  void *memory =
      _aligned_malloc(std::max(bytes, providerAlignment), providerAlignment);
  if (nullptr == memory) throw std::bad_alloc();
#else
  void *memory = nullptr;
  if (0 != posix_memalign(&memory, providerAlignment,
                          std::max(bytes, providerAlignment)))
    throw std::bad_alloc();
#endif
  noteAllocation(bytes);
  return memory;
}

void freeAligned(void *memory) noexcept {
#if defined(_WIN32)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
#endif

struct Options {
  std::string scenario;
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
  std::string measure{"allocations"};
#else
  std::string measure{"timing"};
#endif
  std::size_t size{10000};
  std::uint64_t seed{1};
};

std::uint64_t parseUnsigned(std::string_view value, std::string_view name) {
  std::size_t consumed = 0;
  const auto parsed    = std::stoull(std::string(value), &consumed);
  if (value.empty() || value.front() == '-' || value.front() == '+' ||
      consumed != value.size())
    throw std::invalid_argument("invalid value for " + std::string(name));
  return parsed;
}

Options parseArgs(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (i + 1 == argc)
      throw std::invalid_argument("missing value after " + std::string(arg));
    const std::string_view value(argv[++i]);
    if (arg == "--scenario")
      options.scenario = value;
    else if (arg == "--measure")
      options.measure = value;
    else if (arg == "--size") {
      const auto parsed = parseUnsigned(value, arg);
      if (parsed > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("--size exceeds size_t");
      options.size = static_cast<std::size_t>(parsed);
    } else if (arg == "--seed")
      options.seed = parseUnsigned(value, arg);
    else
      throw std::invalid_argument("unknown option: " + std::string(arg));
  }
  if (options.scenario.empty())
    throw std::invalid_argument("--scenario is required");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
  if (options.measure != "allocations" && options.measure != "both")
    throw std::invalid_argument(
        "allocation probe accepts --measure allocations or both");
#else
  if (options.measure != "timing")
    throw std::invalid_argument("timing probe accepts only --measure timing; "
                                "use performance-probe-counts for allocations");
#endif
  if (options.scenario != "arena-links" &&
      options.scenario != "arena-link-local" &&
      options.scenario != "arena-link-base" &&
      options.scenario != "arena-link-quote-local" &&
      options.scenario != "arena-link-quote-extern" &&
      options.scenario != "arena-link-proxy" &&
      options.scenario != "fold-text" && options.scenario != "fold-structure" &&
      options.scenario != "fold-branch" && options.scenario != "operands" &&
      options.scenario != "vm-snapshots")
    throw std::invalid_argument("scenario not implemented by this probe: " +
                                options.scenario);
  if (options.size == 0) throw std::invalid_argument("--size must be positive");
  if (options.scenario.starts_with("arena-link-") && options.size < 2)
    throw std::invalid_argument(
        "arena-link class scenarios need at least 2 cells");
  if ((options.scenario == "arena-links" ||
       options.scenario.starts_with("arena-link-") ||
       options.scenario == "operands") &&
      options.size >= zigzag::ephemeralBit)
    throw std::invalid_argument("arena workload size exceeds CellRef capacity");
  if (options.scenario.starts_with("fold-") &&
      options.size > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument(
        "fold workload exceeds operation index capacity");
  return options;
}

struct WorkResult {
  std::size_t cells{};
  std::size_t links{};
  std::size_t emptyCells{};
  bool hasStructureCounts{};
  std::size_t traversed{};
  std::optional<std::uint64_t> checksum;
  std::size_t opRange{};
  std::size_t byRefEntries{};
  std::size_t wingArity{};
  std::size_t emptyWings{};
  std::size_t choicePoints{};
  std::size_t snapshotEntries{};
  std::size_t p3Calls{};
  std::size_t p3EligibleCalls{};
  std::uint64_t p3TraversalNs{};
  std::size_t p3Repetitions{};
  std::size_t zeroArityWings{};
  std::size_t eightArityWings{};
  std::size_t nineArityWings{};
  std::uint64_t indexScanNs{};
  std::uint64_t foldKernelNs{};
  std::size_t foldCppNewCalls{};
  std::size_t foldCppNewBytes{};
  std::size_t generatedSiblingCount{};
  std::size_t foldedPathOpCount{};
};

void inspectArenaStructure(const zigzag::ArenaManifold &arena,
                           WorkResult &result) {
  result.cells      = arena.cellCount();
  result.links      = 0;
  result.emptyCells = 0;
  for (std::size_t dense = 0; dense < arena.cellCount(); ++dense) {
    const auto dimensions = arena.dimensionsOf(
        zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(dense)));
    result.links += dimensions.size();
    result.emptyCells += dimensions.empty() ? 1U : 0U;
  }
  result.hasStructureCounts = true;
}

void inspectManifoldStructure(const zigzag::Manifold &manifold,
                              WorkResult &result) {
  result.cells      = manifold.cellCount();
  result.links      = 0;
  result.emptyCells = 0;
  for (const auto &cell : manifold.cells()) {
    result.links += cell.linkCount;
    result.emptyCells += cell.linkCount == 0 ? 1U : 0U;
  }
  result.hasStructureCounts = true;
}

struct IndexScan {
  std::size_t entries{};
  std::size_t range{};
  std::uint64_t elapsedNs{};
};

IndexScan scanByRef(const xanadu::Store &store,
                    const zigzag::Manifold &manifold) {
  const auto started  = std::chrono::steady_clock::now();
  const auto range    = store.segmentedOps().size();
  std::size_t entries = 0;
  for (std::size_t index = 0; index < range; ++index) {
    if (manifold.slot(static_cast<zigzag::CellRef>(index))) ++entries;
  }
  return {.entries   = entries,
          .range     = range,
          .elapsedNs = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count())};
}

WorkResult runArenaLinks(std::size_t size, std::uint64_t seed) {
  zigzag::ArenaManifold arena;
  const auto dimension = arena.makeCell("benchmark.dimension");
  std::vector<zigzag::CellRef> cells;
  cells.reserve(size);
  for (std::size_t i = 0; i < size; ++i) cells.push_back(arena.makeCell());

  for (std::size_t i = 1; i < cells.size(); ++i) {
    if (!arena.link(cells[i - 1], dimension, zigzag::DimVector::POS, cells[i]))
      throw std::runtime_error("engine refused a valid arena link");
  }

  WorkResult result{.cells = arena.cellCount()};
  for (std::size_t i = 0; i < arena.cellCount(); ++i) {
    const auto dimensions = arena.dimensionsOf(
        zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(i)));
    result.links += dimensions.size();
    result.emptyCells += dimensions.empty() ? 1U : 0U;
  }
  result.hasStructureCounts = true;
  auto current              = cells.front();
  std::uint64_t checksum    = 0;
  for (std::size_t i = 0; i < size; ++i) {
    checksum += static_cast<std::uint64_t>(current) ^ seed;
    ++result.traversed;
    const auto next = arena.linked(current, dimension, zigzag::DimVector::POS);
    if (i + 1 == size) {
      if (next)
        throw std::runtime_error("rank unexpectedly has a trailing cell");
    } else {
      if (!next || *next != cells[i + 1])
        throw std::runtime_error(
            "rank traversal disagrees with insertion order");
      current = *next;
    }
  }
  if (result.traversed != size || result.cells != size + 1)
    throw std::runtime_error("arena workload self-check failed");
  result.checksum = checksum;
  return result;
}

WorkResult runFold(std::size_t size, bool textHeavy, bool branched) {
  xanadu::Store store;
  const auto root = textHeavy ? store.makeXanadoc(xanadu::MicroversionId{},
                                                  "performance-probe")
                              : store.sliceGenesis(xanadu::MicroversionId{});
  auto current    = root;
  if (branched) {
    auto leaf = root;
    for (std::size_t i = 0; i < size; ++i)
      leaf = store.makeCell(root, "branch");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    const auto callsBefore = allocationCount.load(std::memory_order_relaxed);
    const auto bytesBefore = requestedBytes.load(std::memory_order_relaxed);
#endif
    const auto foldStart = std::chrono::steady_clock::now();
    auto manifold        = store.rebuildManifold(leaf);
    const auto foldNs    = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - foldStart)
            .count());
    if (manifold.refusedOps() != 0)
      throw std::runtime_error("branch fold refused generated operations");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    const auto foldCalls =
        allocationCount.load(std::memory_order_relaxed) - callsBefore;
    const auto foldBytes =
        requestedBytes.load(std::memory_order_relaxed) - bytesBefore;
#else
    constexpr std::uint64_t foldCalls = 0;
    constexpr std::uint64_t foldBytes = 0;
#endif
    const auto scan = scanByRef(store, manifold);
    WorkResult result{.traversed             = 1,
                      .checksum              = scan.entries,
                      .opRange               = scan.range,
                      .byRefEntries          = scan.entries,
                      .indexScanNs           = scan.elapsedNs,
                      .foldKernelNs          = foldNs,
                      .foldCppNewCalls       = foldCalls,
                      .foldCppNewBytes       = foldBytes,
                      .generatedSiblingCount = size};
    inspectManifoldStructure(manifold, result);
    result.foldedPathOpCount = store.opsFor(leaf).size();
    return result;
  }

  if (textHeavy) {
    for (std::size_t i = 0; i < size; ++i)
      current = store.insert(current, static_cast<std::uint32_t>(i), "x");
    const auto replayedText = store.textOf(current);
    const auto opPath       = store.opsFor(current);
    if (replayedText.size() != size ||
        !std::ranges::all_of(replayedText,
                             [](const char value) { return value == 'x'; }) ||
        opPath.size() != size + 1)
      throw std::runtime_error(
          "text workload did not replay its Xanadoc birth and INSERTs");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    const auto callsBefore = allocationCount.load(std::memory_order_relaxed);
    const auto bytesBefore = requestedBytes.load(std::memory_order_relaxed);
#endif
    const auto foldStart = std::chrono::steady_clock::now();
    auto manifold        = store.rebuildManifold(current);
    const auto foldNs    = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - foldStart)
            .count());
    if (manifold.refusedOps() != 0 || manifold.cellCount() != 0)
      throw std::runtime_error("text fold failed validation");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    const auto foldCalls =
        allocationCount.load(std::memory_order_relaxed) - callsBefore;
    const auto foldBytes =
        requestedBytes.load(std::memory_order_relaxed) - bytesBefore;
#else
    constexpr std::uint64_t foldCalls = 0;
    constexpr std::uint64_t foldBytes = 0;
#endif
    const auto scan = scanByRef(store, manifold);
    WorkResult result{.traversed       = size,
                      .checksum        = scan.entries,
                      .opRange         = scan.range,
                      .byRefEntries    = scan.entries,
                      .indexScanNs     = scan.elapsedNs,
                      .foldKernelNs    = foldNs,
                      .foldCppNewCalls = foldCalls,
                      .foldCppNewBytes = foldBytes};
    inspectManifoldStructure(manifold, result);
    return result;
  }

  auto dimension  = store.makeDimension(current, "benchmark.rank");
  current         = dimension.version;
  const auto head = dimension.dim;
  std::vector<zigzag::CellRef> cells;
  cells.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    current = store.makeCell(current, "member");
    cells.push_back(store.cellRefOf(current).value());
    if (i != 0)
      current = store.setLink(current, cells[i - 1], head,
                              zigzag::DimVector::POS, cells[i]);
  }
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
  const auto callsBefore = allocationCount.load(std::memory_order_relaxed);
  const auto bytesBefore = requestedBytes.load(std::memory_order_relaxed);
#endif
  const auto foldStart = std::chrono::steady_clock::now();
  auto manifold        = store.rebuildManifold(current);
  const auto foldNs    = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - foldStart)
          .count());
  if (manifold.refusedOps() != 0 || manifold.cellCount() < size)
    throw std::runtime_error("generated fold failed validation");
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
  const auto foldCalls =
      allocationCount.load(std::memory_order_relaxed) - callsBefore;
  const auto foldBytes =
      requestedBytes.load(std::memory_order_relaxed) - bytesBefore;
#else
  constexpr std::uint64_t foldCalls = 0;
  constexpr std::uint64_t foldBytes = 0;
#endif
  const auto scan = scanByRef(store, manifold);
  WorkResult result{.traversed       = cells.size(),
                    .checksum        = scan.entries,
                    .opRange         = scan.range,
                    .byRefEntries    = scan.entries,
                    .indexScanNs     = scan.elapsedNs,
                    .foldKernelNs    = foldNs,
                    .foldCppNewCalls = foldCalls,
                    .foldCppNewBytes = foldBytes};
  inspectManifoldStructure(manifold, result);
  return result;
}

WorkResult runArenaLinkClass(std::size_t size, std::uint64_t seed,
                             const std::string_view scenario) {
  (void)seed; // Invocation order is deterministic and identical per cohort.
  const bool local        = scenario == "arena-link-local";
  const bool baseFallback = scenario == "arena-link-base";
  const bool quote        = scenario == "arena-link-quote-local" ||
                     scenario == "arena-link-quote-extern";
  const bool proxy = scenario == "arena-link-proxy";
  if (!(local || baseFallback || quote || proxy))
    throw std::logic_error("unknown arena link class");

  xanadu::Store store;
  auto version         = store.sliceGenesis(xanadu::MicroversionId{});
  const auto dimension = store.makeDimension(version, "performance.axis");
  version              = dimension.version;
  std::vector<zigzag::CellRef> persistent;
  persistent.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    version        = store.makeCell(version, "persistent-member");
    const auto ref = store.cellRefOf(version);
    if (!ref) throw std::runtime_error("persistent cell has no CellRef");
    persistent.push_back(*ref);
    if (i != 0)
      version = store.setLink(version, persistent[i - 1], dimension.dim,
                              zigzag::DimVector::POS, persistent[i]);
  }
  const auto base = store.rebuildManifold(version);
  zigzag::ArenaManifold arena(local ? nullptr : &base,
                              local ? nullptr : &store);
  zigzag::DimRef axis = dimension.dim;
  std::vector<zigzag::CellRef> sources;
  std::vector<zigzag::CellRef> targets;
  sources.reserve(size);
  targets.reserve(size);

  if (local) {
    axis = arena.ensureDimension("performance.axis.local");
    for (std::size_t i = 0; i < size; ++i)
      sources.push_back(arena.makeCell("local-source"));
    for (std::size_t i = 0; i < size; ++i) targets.push_back(sources[i]);
    for (std::size_t i = 1; i < size; ++i) {
      if (!arena.link(sources[i - 1], axis, zigzag::DimVector::POS, sources[i]))
        throw std::runtime_error("arena refused a local rank link");
    }
  } else if (baseFallback) {
    sources = persistent;
    targets = persistent;
  } else {
    const auto space = arena.attach(zigzag::Space{
        .manifold = &base, .store = &store, .label = "performance-probe"});
    axis             = arena.ensureDimension("performance.axis.proxy");
    arena.bindDimension(axis, space, dimension.dim);
    std::vector<zigzag::CellRef> proxies;
    proxies.reserve(size);
    for (const auto ref : persistent)
      proxies.push_back(arena.proxyFor(space, ref));
    if (proxy) {
      sources = proxies;
      targets = proxies;
      for (std::size_t i = 0; i < proxies.size(); ++i) {
        const auto foreign = arena.foreignOf(proxies[i]);
        if (!arena.isProxy(proxies[i]) || !foreign || foreign->space != space ||
            foreign->index != persistent[i] ||
            arena.valueKindOf(proxies[i]) != xanadu::ValueKind::ExternRef)
          throw std::runtime_error("canonical proxy setup failed validation");
      }
    } else {
      std::vector<zigzag::CellRef> occurrences;
      occurrences.reserve(size);
      constexpr zigzag::QuoteViewId view = 1;
      for (const auto ref : proxies)
        occurrences.push_back(arena.quoteOccurrence(view, ref));
      sources = occurrences;
      // A quote's own edge deliberately points at its local occurrence peer.
      // The canonical proxy still names the persistent source whose foreign
      // POS edge points at a different canonical target.
      targets = occurrences;
      for (const auto ref : occurrences) {
        const auto kind = scenario == "arena-link-quote-extern"
                              ? xanadu::ValueKind::ExternRef
                              : xanadu::ValueKind::None;
        if (!arena.setValueBits(ref, kind, 17))
          throw std::runtime_error("could not set quote value marker");
      }
      if (scenario == "arena-link-quote-extern") {
        const auto mark = arena.mark();
        if (!arena.setValueBits(occurrences.front(), xanadu::ValueKind::None,
                                0) ||
            !arena.release(mark) ||
            arena.valueKindOf(occurrences.front()) !=
                xanadu::ValueKind::ExternRef)
          throw std::runtime_error("quote ExternRef mark rollback failed");
      }
      for (std::size_t i = 1; i < size; ++i) {
        if (!arena.link(occurrences[i - 1], axis, zigzag::DimVector::POS,
                        occurrences[i]))
          throw std::runtime_error("arena refused a quote occurrence edge");
      }
    }
  }

  if (sources.size() != size || targets.size() != size)
    throw std::runtime_error("arena link cohort setup has the wrong size");
  for (std::size_t i = 0; i < size; ++i) {
    if (quote && !arena.isQuoteOccurrence(sources[i]))
      throw std::runtime_error("quote scenario lost occurrence identity");
    if (proxy && (!arena.isProxy(sources[i]) || !arena.foreignOf(sources[i])))
      throw std::runtime_error("proxy scenario lost canonical identity");
    if ((local || scenario == "arena-link-quote-local") &&
        (arena.denseOf(sources[i]) == std::nullopt ||
         arena.valueKindOf(sources[i]) == xanadu::ValueKind::ExternRef))
      throw std::runtime_error("eligible scenario is not local non-ExternRef");
    if (scenario == "arena-link-quote-extern" &&
        arena.valueKindOf(sources[i]) != xanadu::ValueKind::ExternRef)
      throw std::runtime_error("quote ExternRef marker is absent");
  }
  for (std::size_t i = 1; i < size; ++i) {
    const auto forward =
        arena.linked(sources[i - 1], axis, zigzag::DimVector::POS);
    const auto reverse = arena.linked(sources[i], axis, zigzag::DimVector::NEG);
    if (forward != targets[i] || reverse != targets[i - 1])
      throw std::runtime_error("arena link cohort edge validation failed");
  }

  // The timed primary performs only the actual reads, required result check,
  // and checksum. Cohort classification/counting is intentionally below and
  // only compiled into the separately collected allocation/count executable.
  WorkResult result{.cells = arena.cellCount()};
  std::uint64_t checksum                  = 0;
  constexpr std::size_t targetLinkedCalls = 32768;
  const std::size_t callsPerPass          = 2 * (size - 1);
  result.p3Repetitions = (targetLinkedCalls + callsPerPass - 1) / callsPerPass;
  const auto visit     = [&](const zigzag::CellRef source,
                         const zigzag::CellRef expected,
                         const zigzag::DimVector dir) {
    const auto actual = arena.linked(source, axis, dir);
    if (actual != expected)
      throw std::runtime_error("arena link cohort traversal failed");
    checksum += static_cast<std::uint64_t>(actual.value_or(0)) ^ seed;
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    ++result.traversed;
#endif
  };
  const auto traversalStarted = std::chrono::steady_clock::now();
  for (std::size_t repetition = 0; repetition < result.p3Repetitions;
       ++repetition) {
    for (std::size_t i = 1; i < size; ++i) {
      visit(sources[i - 1], targets[i], zigzag::DimVector::POS);
      visit(sources[i], targets[i - 1], zigzag::DimVector::NEG);
    }
  }
  result.p3TraversalNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - traversalStarted)
          .count());
#if !defined(GLEDITOR_PERF_ALLOC_PROBE)
  // The primary timing loop avoids per-call telemetry; this exact count is
  // derived from the loop bounds after the measured region.
  result.traversed = result.p3Repetitions * callsPerPass;
#endif
  result.checksum = checksum;
  inspectArenaStructure(arena, result);
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
  // These count real linked() invocations in this generated scenario. The
  // scenario itself supplies the validated path class; this is not a global
  // runtime estimate. Base fallback and proxies are intentionally not P3
  // fast-path eligible. A quote qualifies only when it is arena-owned and its
  // ValueKind is not ExternRef.
  result.p3Calls = result.traversed;
  result.p3EligibleCalls =
      (local || scenario == "arena-link-quote-local") ? result.traversed : 0;
#endif
  return result;
}

WorkResult runOperands(std::size_t size, std::uint64_t seed) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  std::size_t zero     = 0;
  std::size_t eight    = 0;
  std::size_t nine     = 0;
  std::size_t checksum = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const auto arityClass = (i + seed) % 3;
    const auto arity      = arityClass == 0 ? 0U : (arityClass == 1 ? 8U : 9U);
    const auto opcode     = arena.makeCell();
    for (std::size_t operandIndex = 0; operandIndex < arity; ++operandIndex) {
      const auto operand =
          arena.makeScalarCell(static_cast<std::int64_t>(i * 9 + operandIndex));
      core.bindInput(opcode, operand);
    }
    const auto inputs = core.inputsOf(opcode);
    if (inputs.size() != arity)
      throw std::runtime_error("Vortex operand wing failed validation");
    zero += arityClass == 0 ? 1U : 0U;
    eight += arityClass == 1 ? 1U : 0U;
    nine += arityClass == 2 ? 1U : 0U;
    checksum += inputs.size();
  }
  WorkResult result{.traversed       = size,
                    .checksum        = checksum,
                    .wingArity       = checksum,
                    .emptyWings      = zero,
                    .zeroArityWings  = zero,
                    .eightArityWings = eight,
                    .nineArityWings  = nine};
  inspectArenaStructure(arena, result);
  return result;
}

WorkResult runVmSnapshots(std::size_t size) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto alternative = vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt);
  const auto source      = arena.makeCell("snapshot source");
  const auto target      = arena.makeCell();
  const auto copy = vm.mintOpcode(zigzag::vortex::OpcodeKind::CopyCellRefs);
  core.bindInput(copy, source);
  core.bindOutput(copy, target);
  if (!arena.link(copy, core.dims().spin, zigzag::DimVector::POS, alternative))
    throw std::runtime_error("could not connect snapshot setup program");
  const auto cursor = vm.spawnCursor(copy, "performance-probe-snapshot-seed");
  const auto seeded = vm.step(cursor);
  if (!seeded.success || vm.cellReferenceOf(target) != source)
    throw std::runtime_error("could not seed a VM cell-reference register");
  for (std::size_t i = 0; i < size; ++i)
    vm.pushChoicePoint(std::nullopt, alternative);
  WorkResult result{.choicePoints = vm.choiceStack().size()};
  for (const auto &choice : vm.choiceStack()) {
    const auto seededRef = choice.cellReferences.find(target);
    if (choice.cellReferences.size() != 1 ||
        seededRef == choice.cellReferences.end() || seededRef->second != source)
      throw std::runtime_error(
          "choice point did not retain target-to-source cell reference");
    result.snapshotEntries +=
        choice.cellReferences.size() + choice.streams.size() +
        choice.iterators.size() + choice.activationFrames.size() +
        choice.guardStacks.size() + choice.pendingGuardChecks.size() +
        choice.guardScopes.size() + choice.mutationGroups.size();
  }
  result.traversed = vm.choiceStack().size();
  if (result.choicePoints != size || result.snapshotEntries != size)
    throw std::runtime_error("choice stack snapshot counts failed validation");
  result.checksum = result.snapshotEntries;
  inspectArenaStructure(arena, result);
  return result;
}

WorkResult runScenario(const Options &options) {
  if (options.scenario == "arena-links")
    return runArenaLinks(options.size, options.seed);
  if (options.scenario.starts_with("arena-link-"))
    return runArenaLinkClass(options.size, options.seed, options.scenario);
  if (options.scenario == "fold-text")
    return runFold(options.size, true, false);
  if (options.scenario == "fold-structure")
    return runFold(options.size, false, false);
  if (options.scenario == "fold-branch")
    return runFold(options.size, false, true);
  if (options.scenario == "operands")
    return runOperands(options.size, options.seed);
  if (options.scenario == "vm-snapshots") return runVmSnapshots(options.size);
  throw std::logic_error("validated scenario has no workload implementation");
}

} // namespace

#if defined(GLEDITOR_PERF_ALLOC_PROBE)
void *operator new(std::size_t bytes) { return allocate(bytes); }
void *operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}
void *operator new(std::size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void *memory, std::align_val_t) noexcept {
  freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  freeAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  freeAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  freeAligned(memory);
}
#endif

int main(int argc, char **argv) {
  try {
    const auto options = parseArgs(argc, argv);
    std::optional<WorkResult> timed;
    std::optional<WorkResult> allocationRun;
    std::optional<std::uint64_t> elapsed;
#if !defined(GLEDITOR_PERF_ALLOC_PROBE)
    {
#else
    if (options.measure == "both") {
#endif
      const auto started = std::chrono::steady_clock::now();
      timed              = runScenario(options);
      elapsed            = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
    }
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    {
      allocationCount.store(0, std::memory_order_relaxed);
      requestedBytes.store(0, std::memory_order_relaxed);
      countAllocations.store(true, std::memory_order_relaxed);
      allocationRun = runScenario(options);
      countAllocations.store(false, std::memory_order_relaxed);
    }
#endif
    const auto &metrics = allocationRun ? *allocationRun : *timed;

    std::cout << "{\"schema_version\":2,\"scenario\":\"" << options.scenario
              << "\",\"size\":" << options.size << ",\"seed\":" << options.seed
              << ",\"measure\":\"" << options.measure << "\",\"wall_time_ns\":";
    if (elapsed)
      std::cout << *elapsed;
    else
      std::cout << "null";
    std::cout << ",\"fold_kernel_ns\":";
    if (timed && options.scenario.starts_with("fold-"))
      std::cout << timed->foldKernelNs;
    else
      std::cout << "null";
    std::cout << ",\"scenario_total_cpp_new_calls\":";
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    if (allocationRun)
      std::cout << allocationCount.load();
    else
      std::cout << "null";
#else
    std::cout << "null";
#endif
    std::cout << ",\"scenario_total_cpp_new_requested_bytes\":";
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    if (allocationRun)
      std::cout << requestedBytes.load();
    else
      std::cout << "null";
#else
    std::cout << "null";
#endif
    std::cout << ",\"fold_cpp_new_calls\":";
    if (allocationRun && options.scenario.starts_with("fold-"))
      std::cout << metrics.foldCppNewCalls;
    else
      std::cout << "null";
    std::cout << ",\"fold_cpp_new_requested_bytes\":";
    if (allocationRun && options.scenario.starts_with("fold-"))
      std::cout << metrics.foldCppNewBytes;
    else
      std::cout << "null";
    std::cout << ",\"cell_count\":" << metrics.cells
              << ",\"dimension_run_count\":";
    if (metrics.hasStructureCounts)
      std::cout << metrics.links;
    else
      std::cout << "null";
    std::cout << ",\"empty_dimension_run_cells\":";
    if (metrics.hasStructureCounts)
      std::cout << metrics.emptyCells;
    else
      std::cout << "null";
    std::cout << ",\"work_items\":" << metrics.traversed << ",\"op_range\":";
    if (metrics.opRange != 0)
      std::cout << metrics.opRange;
    else
      std::cout << "null";
    std::cout << ",\"by_ref_entries\":";
    if (metrics.opRange != 0)
      std::cout << metrics.byRefEntries;
    else
      std::cout << "null";
    std::cout << ",\"by_ref_per_op_range\":";
    if (metrics.opRange != 0)
      std::cout << static_cast<double>(metrics.byRefEntries) /
                       static_cast<double>(metrics.opRange);
    else
      std::cout << "null";
    std::cout << ",\"by_ref_entries_per_cell\":";
    if (metrics.cells != 0 && metrics.opRange != 0)
      std::cout << static_cast<double>(metrics.byRefEntries) /
                       static_cast<double>(metrics.cells);
    else
      std::cout << "null";
    std::cout << ",\"wing_arity_total\":";
    if (options.scenario == "operands")
      std::cout << metrics.wingArity;
    else
      std::cout << "null";
    std::cout << ",\"empty_wings\":";
    if (options.scenario == "operands")
      std::cout << metrics.emptyWings;
    else
      std::cout << "null";
    std::cout << ",\"wing_arity_histogram\":";
    if (options.scenario == "operands")
      std::cout << "{\"0\":" << metrics.zeroArityWings
                << ",\"8\":" << metrics.eightArityWings
                << ",\"9\":" << metrics.nineArityWings << '}';
    else
      std::cout << "null";
    std::cout << ",\"choice_points\":";
    if (options.scenario == "vm-snapshots")
      std::cout << metrics.choicePoints;
    else
      std::cout << "null";
    std::cout << ",\"selected_snapshot_map_entries\":";
    if (options.scenario == "vm-snapshots")
      std::cout << metrics.snapshotEntries;
    else
      std::cout << "null";
    std::cout << ",\"generated_sibling_count\":";
    if (options.scenario == "fold-branch")
      std::cout << metrics.generatedSiblingCount;
    else
      std::cout << "null";
    std::cout << ",\"folded_path_op_count\":";
    if (options.scenario == "fold-branch")
      std::cout << metrics.foldedPathOpCount;
    else
      std::cout << "null";
    std::cout << ",\"by_ref_index_scan_ns\":";
    if (options.scenario.starts_with("fold-"))
      std::cout << (timed ? timed->indexScanNs : metrics.indexScanNs);
    else
      std::cout << "null";
    std::cout << ",\"p3_repetitions\":";
    if (options.scenario.starts_with("arena-link-"))
      std::cout << metrics.p3Repetitions;
    else
      std::cout << "null";
    std::cout << ",\"p3_link_traversal_ns\":";
    if (timed && options.scenario.starts_with("arena-link-"))
      std::cout << timed->p3TraversalNs;
    else
      std::cout << "null";
    std::cout << ",\"p3_linked_calls\":";
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    if (options.scenario.starts_with("arena-link-"))
      std::cout << metrics.p3Calls;
    else
      std::cout << "null";
#else
    std::cout << "null";
#endif
    std::cout << ",\"p3_fastpath_eligible_calls\":";
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    if (options.scenario.starts_with("arena-link-"))
      std::cout << metrics.p3EligibleCalls;
    else
      std::cout << "null";
#else
    std::cout << "null";
#endif
    std::cout << ",\"p3_eligibility_scope\":\"scenario_only\"";
    std::cout << ",\"p3_eligibility_class\":\"";
    if (options.scenario == "arena-link-local" ||
        options.scenario == "arena-link-quote-local")
      std::cout << "eligible_arena_owned_non_externref";
    else if (options.scenario == "arena-link-base")
      std::cout << "base_fallback_unchanged";
    else if (options.scenario == "arena-link-quote-extern")
      std::cout << "quote_externref_excluded";
    else if (options.scenario == "arena-link-proxy")
      std::cout << "canonical_proxy_slow_classification";
    else
      std::cout << "not_applicable";
    std::cout << "\",\"checksum\":";
    if (metrics.checksum)
      std::cout << *metrics.checksum;
    else
      std::cout << "null";
    std::cout
        << ",\"wall_time_scope\":\"whole scenario including generated input "
           "and post-fold inspection\",\"coverage_note\":\"fold_kernel_ns "
           "times rebuildManifold only; by_ref_index_scan_ns is separate "
           "public-API inspection. Scenario-total allocation includes setup "
           "and inspection; fold allocation is separately windowed around "
           "rebuildManifold. Snapshot inventory counts selected top-level map "
           "entries only: cellReferences, streams, iterators, "
           "activationFrames, guardStacks, pendingGuardChecks, guardScopes, "
           "mutationGroups; it excludes slot sets, Dewey-path entries and "
           "nested payload elements. The timing executable has no global new "
           "replacement; the counts executable's `both` timing replay runs "
           "with its counting flag disabled and is diagnostic only. Requested "
           "C++ new bytes exclude C allocation APIs and "
           "allocator overhead; aligned allocation is paired per platform\"}\n";
    return 0;
  } catch (const std::exception &error) {
#if defined(GLEDITOR_PERF_ALLOC_PROBE)
    countAllocations.store(false, std::memory_order_relaxed);
#endif
    std::cerr << "performance-probe: " << error.what() << '\n';
    return 2;
  }
}
