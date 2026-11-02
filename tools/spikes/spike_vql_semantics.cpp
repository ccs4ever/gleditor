/**
 * Scoped VQL performance experiment; see design/vql-performance-spike.md.
 * Native C++ is needed to observe allocation calls and compiler ABI layout.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace allocation_probe {
struct Counts {
  std::uint64_t calls{}, bytes{}, deletes{};
};
thread_local Counts *active = nullptr;
void *allocate(std::size_t size, std::size_t alignment = 0) {
  void *ptr = nullptr;
  if (alignment == 0) {
    ptr = std::malloc(std::max(size, std::size_t{1}));
  } else if (posix_memalign(&ptr, alignment, std::max(size, std::size_t{1})) !=
             0) {
    ptr = nullptr;
  }
  if (ptr == nullptr) throw std::bad_alloc();
  if (active != nullptr) {
    ++active->calls;
    active->bytes += size;
  }
  return ptr;
}
void release(void *ptr) noexcept {
  if (active != nullptr && ptr != nullptr) ++active->deletes;
  std::free(ptr);
}
struct Scope {
  explicit Scope(Counts &counts) { active = &counts; }
  ~Scope() { active = nullptr; }
};
} // namespace allocation_probe

void *operator new(std::size_t n) { return allocation_probe::allocate(n); }
void *operator new[](std::size_t n) { return allocation_probe::allocate(n); }
void *operator new(std::size_t n, std::align_val_t a) {
  return allocation_probe::allocate(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a) {
  return allocation_probe::allocate(n, static_cast<std::size_t>(a));
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](n);
  } catch (...) {
    return nullptr;
  }
}
void *operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n, a);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](n, a);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *p) noexcept { allocation_probe::release(p); }
void operator delete[](void *p) noexcept { allocation_probe::release(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept {
  ::operator delete[](p);
}
void operator delete(void *p, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, std::align_val_t) noexcept {
  ::operator delete[](p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  ::operator delete[](p);
}
void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}
void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  ::operator delete(p);
}
void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}

namespace {
using namespace xanadu::vql;
using zigzag::CellRef;
using Clock = std::chrono::steady_clock;

// These deliberately contain no runtime behavior: sizes prove layout only.
struct PrototypeHyperHandle {
  std::uint64_t identity, context;
};
struct PrototypeCurriedMorphism {
  PrototypeHyperHandle subject;
  std::uint64_t operation;
};
struct PrototypeDynamicLensMatrix {
  PrototypeHyperHandle axes[4];
};
static_assert(sizeof(PrototypeHyperHandle) == 16);
static_assert(sizeof(PrototypeCurriedMorphism) == 24);
static_assert(sizeof(PrototypeDynamicLensMatrix) == 64);

struct Options {
  std::size_t cells{1000}, repetitions{21}, dimensions{2}, warmups{3};
};
std::size_t positive(std::string_view text, bool zeroAllowed = false) {
  std::size_t used{};
  const auto result = std::stoull(std::string(text), &used);
  if (used != text.size() || text.empty() || text.front() == '-' ||
      (!zeroAllowed && result == 0) ||
      result > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("invalid nonnegative option value");
  return static_cast<std::size_t>(result);
}
Options options(int argc, char **argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help") {
      std::cout << "vql-spike [--cells N] [--repetitions N] [--dimensions N] "
                   "[--warmups N]\n";
      std::exit(0);
    }
    if (i + 1 == argc) throw std::invalid_argument("missing option value");
    const std::string_view value(argv[++i]);
    if (arg == "--cells")
      result.cells = positive(value);
    else if (arg == "--repetitions")
      result.repetitions = positive(value);
    else if (arg == "--dimensions")
      result.dimensions = positive(value);
    else if (arg == "--warmups")
      result.warmups = positive(value, true);
    else
      throw std::invalid_argument("unknown option: " + std::string(arg));
  }
  if (result.dimensions > 64 || result.cells > 10000000 ||
      result.cells > 10000000 / (result.dimensions + 1) ||
      result.repetitions > 1000000 || result.warmups > 1000000)
    throw std::invalid_argument(
        "experiment exceeds bounded fixture/sample limits");
  return result;
}

struct Signature {
  std::size_t count{};
  std::uint64_t checksum{1469598103934665603ULL};
  void add(CellRef cell) {
    ++count;
    checksum = (checksum ^ cell) * 1099511628211ULL;
  }
  bool operator==(const Signature &) const = default;
};
Signature signature(const std::vector<CellRef> &cells) {
  Signature result;
  for (auto cell : cells) result.add(cell);
  return result;
}
void require(bool condition, std::string_view reason) {
  if (!condition) throw std::runtime_error(std::string(reason));
}

struct Fixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  zigzag::DimRef items{engine.resolveDimension("d.items")};
  std::vector<zigzag::DimRef> hops;
  std::string query{"##/+d.items"}, declaration, functorQuery;
  Signature expected;

  explicit Fixture(const Options &opts) {
    for (std::size_t d = 0; d < opts.dimensions; ++d) {
      const auto name = "d.hop" + std::to_string(d);
      hops.push_back(engine.resolveDimension(name));
      query += "/+" + name;
      declaration += "let $h" + std::to_string(d) + " := /+" + name + "; ";
    }
    declaration += "let $walk := $h0";
    for (std::size_t d = 1; d < opts.dimensions; ++d)
      declaration += " / $h" + std::to_string(d);
    declaration += ";";
    functorQuery     = "##/+d.items / $walk";
    CellRef previous = core.home();
    for (std::size_t i = 0; i < opts.cells; ++i) {
      CellRef cell = arena.makeCell();
      zigzag::expectWritten(arena.link(previous, items, false, cell));
      previous = cell;
      for (auto dim : hops) {
        const auto next = arena.makeCell();
        zigzag::expectWritten(arena.link(cell, dim, false, next));
        cell = next;
      }
      expected.add(cell);
    }
  }

  Signature visit() const {
    Signature result;
    auto cell = arena.linked(core.home(), items, false);
    while (cell) {
      auto target = cell;
      for (auto dim : hops) {
        require(target.has_value(), "native traversal lost an endpoint");
        target = arena.linked(*target, dim, false);
      }
      require(target.has_value(), "native traversal lost an endpoint");
      result.add(*target);
      cell = arena.linked(*cell, items, false);
    }
    return result;
  }
};

struct Sample {
  double ns{};
  allocation_probe::Counts allocations;
};
template <class F> Sample sample(F &&fn) {
  Sample result;
  {
    allocation_probe::Scope scope(result.allocations);
    const auto start = Clock::now();
    fn();
    result.ns =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  }
  return result;
}
void report(std::string_view name, std::vector<Sample> samples) {
  std::ranges::sort(samples, {}, &Sample::ns);
  double calls{}, bytes{}, deletes{};
  for (const auto &s : samples) {
    calls += static_cast<double>(s.allocations.calls);
    bytes += static_cast<double>(s.allocations.bytes);
    deletes += static_cast<double>(s.allocations.deletes);
  }
  const auto n   = static_cast<double>(samples.size());
  const auto p95 = static_cast<std::size_t>(std::ceil(n * 0.95)) - 1;
  std::cout << name << " samples=" << samples.size()
            << " min_ns=" << samples.front().ns
            << " median_ns=" << samples[samples.size() / 2].ns
            << " p95_ns=" << samples[p95].ns << " new_calls_mean=" << calls / n
            << " requested_bytes_mean=" << bytes / n
            << " delete_calls_mean=" << deletes / n << '\n';
}
template <class Setup, class Run, class Check>
void benchmark(std::string_view name, const Options &opts, Setup &&setup,
               Run &&run, Check &&check) {
  std::vector<Sample> measured;
  measured.reserve(opts.repetitions);
  for (std::size_t i = 0; i < opts.warmups + opts.repetitions; ++i) {
    setup();
    auto observation = sample(run);
    check();
    if (i >= opts.warmups) measured.push_back(observation);
  }
  report(name, std::move(measured));
}

void checkAllocationProbe() {
  allocation_probe::Counts counts;
  {
    allocation_probe::Scope scope(counts);
    const std::nothrow_t noThrow;
    const auto alignment      = std::align_val_t{64};
    void *scalar              = ::operator new(17);
    void *array               = ::operator new[](17);
    void *aligned             = ::operator new(17, alignment);
    void *alignedArray        = ::operator new[](17, alignment);
    void *nothrowScalar       = ::operator new(17, noThrow);
    void *nothrowArray        = ::operator new[](17, noThrow);
    void *nothrowAligned      = ::operator new(17, alignment, noThrow);
    void *nothrowAlignedArray = ::operator new[](17, alignment, noThrow);
    ::operator delete(scalar, std::size_t{17});
    ::operator delete[](array, std::size_t{17});
    ::operator delete(aligned, std::size_t{17}, alignment);
    ::operator delete[](alignedArray, std::size_t{17}, alignment);
    ::operator delete(nothrowScalar, noThrow);
    ::operator delete[](nothrowArray, noThrow);
    ::operator delete(nothrowAligned, alignment, noThrow);
    ::operator delete[](nothrowAlignedArray, alignment, noThrow);
  }
  require(counts.calls == 8 && counts.bytes == 136 && counts.deletes == 8,
          "allocation instrumentation self-check failed");
  std::cout << "allocation_probe_self_check=pass\n";
}

void layouts() {
#define LAYOUT(T)                                                              \
  std::cout << "layout " #T " size=" << sizeof(T) << " align=" << alignof(T)   \
            << '\n'
  LAYOUT(CellRef);
  LAYOUT(zigzag::CellSlot);
  LAYOUT(zigzag::DimLink);
  LAYOUT(SignedDimensionStep);
  LAYOUT(PathStep);
  LAYOUT(PathExpression);
  LAYOUT(QueryExpression);
  LAYOUT(VQLEngine);
  LAYOUT(PrototypeHyperHandle);
  LAYOUT(PrototypeCurriedMorphism);
  LAYOUT(PrototypeDynamicLensMatrix);
#undef LAYOUT
}
} // namespace

int main(int argc, char **argv) {
  try {
    const auto opts = options(argc, argv);
    std::cout << std::fixed << std::setprecision(1)
              << "compiler=" << __VERSION__ << " cplusplus=" << __cplusplus
              << " steady_clock=" << Clock::is_steady << '\n'
              << "cells=" << opts.cells << " dimensions=" << opts.dimensions
              << " repetitions=" << opts.repetitions
              << " warmups=" << opts.warmups << '\n';
    checkAllocationProbe();
    layouts();
    Fixture fixture(opts);
    const auto initialCells = fixture.arena.cellCount();
    std::cout << "query=" << fixture.query
              << "\nfunctor=" << fixture.declaration << ' '
              << fixture.functorQuery << "\nfixture_cells=" << initialCells
              << " expected_count=" << fixture.expected.count
              << " expected_checksum=" << fixture.expected.checksum << '\n';
    std::vector<CellRef> result;
    auto check = [&] {
      require(signature(result) == fixture.expected,
              "VQL count/checksum mismatch");
      require(fixture.arena.cellCount() == initialCells,
              "read-only evaluation grew arena");
      result.clear();
    };
    report("parse_eval_first_use",
           {sample([&] { result = fixture.engine.execute(fixture.query); })});
    check();
    QueryExpression parsed;
    report("parse_first_observed",
           {sample([&] { parsed = Parser(fixture.query).parseQuery(); })});
    benchmark(
        "parse_only_warm", opts, [] {},
        [&] { parsed = Parser(fixture.query).parseQuery(); }, [] {});
    std::unique_ptr<VQLEngine> fresh;
    benchmark(
        "parse_eval_fresh_engine", opts,
        [&] { fresh = std::make_unique<VQLEngine>(fixture.core); },
        [&] { result = fresh->execute(fixture.query); }, check);
    benchmark(
        "eval_parsed_fresh_engine", opts,
        [&] { fresh = std::make_unique<VQLEngine>(fixture.core); },
        [&] { result = fresh->execute(parsed); }, check);
    benchmark(
        "eval_parsed_warm_engine", opts, [] {},
        [&] { result = fixture.engine.execute(parsed); }, check);
    fixture.engine.execute(fixture.declaration);
    const auto functor = Parser(fixture.functorQuery).parseQuery();
    benchmark(
        "eval_bound_functor_warm", opts, [] {},
        [&] { result = fixture.engine.execute(functor); }, check);
    Signature visited;
    benchmark(
        "native_link_visitor", opts, [] {}, [&] { visited = fixture.visit(); },
        [&] {
          require(visited == fixture.expected,
                  "native count/checksum mismatch");
        });

    // Fresh runtime per compilation: emitted instructions must not accumulate.
    std::unique_ptr<zigzag::ArenaManifold> arena;
    std::unique_ptr<zigzag::vortex::VortexCore> core;
    std::unique_ptr<zigzag::vortex::VortexVM> vm;
    std::unique_ptr<VQLCompiler> compiler;
    CompilationResult compiled;
    auto setupCompiler = [&] {
      compiler.reset();
      vm.reset();
      core.reset();
      arena.reset();
      arena    = std::make_unique<zigzag::ArenaManifold>();
      core     = std::make_unique<zigzag::vortex::VortexCore>(*arena);
      vm       = std::make_unique<zigzag::vortex::VortexVM>(*core);
      compiler = std::make_unique<VQLCompiler>(*core, *vm);
    };
    const auto identity = Parser("##").parseQuery();
    benchmark(
        "compile_identity_fresh_runtime", opts, setupCompiler,
        [&] { compiled = compiler->compile(identity); },
        [&] { require(compiled.success, "identity subset must compile"); });
    const auto cursors = vm->activeCursors();
    require(cursors.size() == 1, "compiled identity must spawn one cursor");
    const auto terminal      = compiled.generatedOpcodes.back();
    const auto identityCells = core->inputsOf(terminal);
    require(identityCells == std::vector<CellRef>{core->home()},
            "compiled identity output differs");
    require(vm->run(cursors.front()).success,
            "compiled identity execution failed");
    std::cout << "compiled_identity count=1 checksum="
              << signature(identityCells).checksum
              << " execution=success (only_common_subset_measured)\n";
    setupCompiler();
    const auto rankCompilation = compiler->compile(parsed);
    require(!rankCompilation.success,
            "compiler unexpectedly accepted whole-rank traversal; revalidate "
            "equivalence");
    std::cout << "compiled_rank refused=" << rankCompilation.errorMessage
              << '\n';
    setupCompiler();
    const auto functorCompilation =
        compiler->compile("let $up := /+d.hop0 return $up");
    require(!functorCompilation.success,
            "compiler unexpectedly accepted functor; revalidate equivalence");
    require(functorCompilation.errorMessage.find("functor") !=
                std::string::npos,
            "functor rejection did not name unsupported functor lowering");
    std::cout << "compiled_functor refused=" << functorCompilation.errorMessage
              << '\n'
              << "correctness=pass arena_growth=0\n";
  } catch (const std::exception &error) {
    std::cerr << "vql-spike: " << error.what() << '\n';
    return 1;
  }
}
