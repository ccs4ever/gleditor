/**
 * @file vortex_stdlib.hpp
 * @brief Vortex Standard Library Architecture & Living Manifold Module System.
 *
 * Implements Section 7 of the Vortex Hyperstructural Runtime specification.
 * The standard library lives as zzstructures in the manifold off `d.stdlib`
 * with spatial symbol resolution ("std:<module>/<symbol>") and provides the
 * 7 fundamental modules:
 * 1. std:memoize    (topological caching, LRU, atomic flush, retirement)
 * 2. std:contract   (reusable precondition and postcondition assertion
 * templates)
 * 3. std:pipeline   (parameter pre/postprocessing pipelines along +d.spin)
 * 4. std:functional (map, filter, fold, zip combinators)
 * 5. std:collections (sequences/lists, associative maps, 2D matrix grids)
 * 6. std:math       (abs, min, max, clamp, gcd, lcm, pow, sqrt)
 * 7. std:string     (split, join, starts_with, ends_with, to_upper, to_lower,
 * trim)
 * 8. std:logic      (unification, terms, variables, choice points, resolution)
 * 9. std:goal       (vicon's goal-directed evaluation: generators, outcomes,
 * alternation, limitation, and the d.scan string-scanning environment)
 */
#ifndef COMMON_XANADU_VORTEX_STDLIB_HPP
#define COMMON_XANADU_VORTEX_STDLIB_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/stdlib_image_format.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"
#include "common/xanadu/zigzag/vlog.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"
#include <gleditor/cpp26.hpp>

namespace xanadu {
class Store;
}

namespace zigzag::vortex {

class VortexWorkerPool;
struct StdlibImage;

struct LogicSolution {
  std::vector<std::pair<CellRef, CellRef>> bindings;
  std::unordered_map<CellRef, CellRef> varMap;
  std::unordered_map<std::string, std::string> formatted;
};

/// A grid's head plus its shape. An empty dimension has no head, but retains
/// both requested dimensions so empty grids remain distinguishable values.
struct GridResult {
  std::optional<CellRef> head;
  std::size_t rows{};
  std::size_t cols{};
};

class VortexStdLib {
public:
  enum class Bootstrap : std::uint8_t { Automatic, Manual };

  VortexStdLib(VortexCore &core, VortexVM &vm,
               Bootstrap bootstrapMode = Bootstrap::Automatic);

  // -- System Bootstrap -------------------------------------------------------
  /// Weaves the canonical standard modules along +d.stdlib off home.
  void bootstrap();

  /// The deterministic order used by bootstrap and future module generators.
  [[nodiscard]] static std::span<const std::string_view>
  canonicalModuleOrder() noexcept;
  /// Start an image-construction session and create its sole ops rank head.
  [[nodiscard]] std::expected<VortexStdLib *, std::string> beginImageOps();
  /// Build one canonical module by name, refusing unknown or repeated names.
  [[nodiscard]] std::expected<CellRef, std::string>
  buildModule(std::string_view moduleName);
  /// Finish the private image manifest and the single ops enumeration.
  [[nodiscard]] std::expected<VortexStdLib *, std::string> endImageManifest();
  /// Rebuild this instance's bindings and goal refs from existing image data.
  [[nodiscard]] std::expected<VortexStdLib *, std::string> rebindInstance();
  /// Grafts a validated stdlib image into this arena inside one mark, then
  /// rebinds this instance. The arena is left untouched on refusal.
  [[nodiscard]] std::expected<VortexStdLib *, std::string>
  graftImage(const StdlibImage &image);

  // -- Spatial Symbol Resolution (import) -------------------------------------
  /// Resolves "std:<module>/<symbol>" to its entry opcode or "std:<module>" to
  /// module cell.
  /// The cell @p path names -- a module ("std:list") or the entry op of one of
  /// its symbols ("std:list/map") -- or nullopt.
  [[nodiscard]] std::optional<CellRef> resolve(std::string_view path) const;

  /// Checks whether a module or symbol path exists in the manifold.
  [[nodiscard]] bool has(std::string_view path) const;

  /// Lists all registered module paths (e.g., "std:math", "std:string", ...).
  [[nodiscard]] std::vector<std::string> modules() const;

  /// Lists all exported symbol names within a module.
  [[nodiscard]] std::vector<std::string>
  symbolsInModule(std::string_view modulePath) const;

  // -- Function Invocation Helpers --------------------------------------------
  /// Calls a standard library routine by path with arguments.
  std::vector<CellValue> call(std::string_view path,
                              const std::vector<CellValue> &args);

  /// Calls an opcode or subroutine with arguments.
  std::vector<CellValue> call(CellRef fnOp, const std::vector<CellValue> &args);

  // -- Module 1: std:memoize --------------------------------------------------
  CellRef memoize(CellRef op, std::string_view cacheKey,
                  std::size_t capacity = 100);
  bool flushMemo(std::string_view cacheKey);
  bool retireMemo(std::string_view cacheKey);
  [[nodiscard]] std::size_t memoEntryCount(std::string_view cacheKey) const;

  // -- Module 2: std:contract -------------------------------------------------
  CellRef createRequirePositive(CellRef inParam);
  CellRef createRequireNonNegative(CellRef inParam);
  CellRef createRequireRange(CellRef inParam, double lo, double hi);
  CellRef createRequireNonEmpty(CellRef inParam);
  CellRef createEnsureGrowth(CellRef outParam, CellRef inParam);

  // -- Module 3: std:pipeline -------------------------------------------------
  CellRef createTrimPipeline();
  CellRef createToLowerPipeline();
  CellRef createToUpperPipeline();
  CellRef createClampPipeline(double lo, double hi);
  CellRef createParseNumPipeline();
  CellRef createFormatCurrencyPipeline();

  // -- Module 4: std:functional -----------------------------------------------
  std::optional<CellRef>
  map(CellRef head, DimRef inDim, DimRef outDim,
      gleditor::cpp26::function_ref<CellValue(const CellValue &)> fn);
  std::optional<CellRef> map(CellRef head, DimRef inDim, DimRef outDim,
                             CellRef fnOp);

  std::optional<CellRef>
  filter(CellRef head, DimRef inDim, DimRef outDim,
         gleditor::cpp26::function_ref<bool(const CellValue &)> pred);
  std::optional<CellRef> filter(CellRef head, DimRef inDim, DimRef outDim,
                                CellRef predOp);

  CellValue fold(CellRef head, DimRef inDim, CellValue initial,
                 gleditor::cpp26::function_ref<CellValue(const CellValue &,
                                                         const CellValue &)>
                     fn);
  CellValue fold(CellRef head, DimRef inDim, CellValue initial, CellRef fnOp);

  std::optional<CellRef> zip(CellRef headA, CellRef headB, DimRef dimA,
                             DimRef dimB, DimRef outDim);

  // -- Stage 2: data-parallel execution hook ----------------------------------
  /// Attach a worker pool so map/filter/fold fan their per-element function
  /// across workers when the input length reaches @p threshold. Both the pool
  /// and threshold come from the host, which reads vortex.worker_threads and
  /// vortex.parallel_threshold from system://settings (no magic numbers,
  /// vortex-multithreading-study.md §3.2/§6.2). Null pool or a length below
  /// the threshold runs serially, so the default is byte-identical to before.
  /// Setter chains, as the engine's setters do.
  VortexStdLib *setWorkerPool(VortexWorkerPool *pool,
                              std::size_t threshold) noexcept {
    workerPool_        = pool;
    parallelThreshold_ = threshold;
    return this;
  }

  // -- Module 5: std:collections ----------------------------------------------
  /// The head of a non-empty list; nullopt is a valid empty list.
  std::optional<CellRef> createList(const std::vector<CellValue> &items,
                                    std::optional<DimRef> dim = std::nullopt);
  [[nodiscard]] std::vector<CellValue>
  listToVector(CellRef head, std::optional<DimRef> dim = std::nullopt) const;
  void pushBack(CellRef head, const CellValue &val,
                std::optional<DimRef> dim = std::nullopt);
  CellRef pushFront(CellRef head, const CellValue &val,
                    std::optional<DimRef> dim = std::nullopt);
  std::optional<CellValue> popBack(CellRef head,
                                   std::optional<DimRef> dim = std::nullopt);
  std::optional<CellRef> popFront(std::optional<CellRef> head,
                                  std::optional<DimRef> dim = std::nullopt);
  [[nodiscard]] std::size_t
  listLength(CellRef head, std::optional<DimRef> dim = std::nullopt) const;

  /// The first cell and dimensions; a zero-sized grid has no head but retains
  /// its shape in the result.
  GridResult
  createGrid(std::size_t rows, std::size_t cols, DimRef dRow, DimRef dCol,
             const CellValue &initVal = static_cast<std::int64_t>(0));
  [[nodiscard]] std::optional<CellValue> getGrid(CellRef gridRoot,
                                                 std::size_t r, std::size_t c,
                                                 DimRef dRow,
                                                 DimRef dCol) const;
  void setGrid(CellRef gridRoot, std::size_t r, std::size_t c, DimRef dRow,
               DimRef dCol, const CellValue &val);

  // -- Module 6: std:math -----------------------------------------------------
  static CellValue mathAbs(const CellValue &x);
  static CellValue mathMin(const CellValue &a, const CellValue &b);
  static CellValue mathMax(const CellValue &a, const CellValue &b);
  static CellValue mathClamp(const CellValue &x, const CellValue &lo,
                             const CellValue &hi);
  static std::int64_t mathGcd(std::int64_t a, std::int64_t b);
  static std::int64_t mathLcm(std::int64_t a, std::int64_t b);
  static double mathPow(double base, double exp);
  static double mathSqrt(double x);

  // -- Module 7: std:string ---------------------------------------------------
  static std::string strToUpper(std::string_view s);
  static std::string strToLower(std::string_view s);
  static bool strStartsWith(std::string_view s, std::string_view prefix);
  static bool strEndsWith(std::string_view s, std::string_view suffix);
  static std::vector<std::string> strSplit(std::string_view s,
                                           std::string_view delim);
  static std::string strJoin(const std::vector<std::string> &parts,
                             std::string_view delim);
  static std::string strTrim(std::string_view s);

  // -- Module 8: std:logic ----------------------------------------------------
  CellRef makeVar(std::string_view name = {});
  CellRef makeTerm(std::string_view functor,
                   std::initializer_list<CellRef> args);
  CellRef makeTerm(std::string_view functor, std::span<const CellRef> args);
  CellRef makeCons(CellRef head, CellRef tail);
  CellRef makeList(std::initializer_list<CellRef> elements);
  CellRef makeList(std::span<const CellRef> elements);
  [[nodiscard]] bool isVar(CellRef cell) const;
  [[nodiscard]] std::optional<CellRef> deref(CellRef cell) const;
  zigzag::UnifyResult unify(CellRef a, CellRef b);
  [[nodiscard]] std::vector<CellRef> argumentsOf(CellRef term) const;
  /// Absence means an invalid reference; an empty atom is an engaged string.
  [[nodiscard]] std::optional<std::string> functorOf(CellRef term) const;
  /// Absence means an invalid term or child reference; empty text stays valid.
  [[nodiscard]] std::optional<std::string> renderTerm(CellRef term) const;

  CellRef createPredicate(std::string_view name);
  CellRef addClause(CellRef predCell, CellRef headTerm,
                    std::span<const CellRef> bodyGoals = {});

  void setBoundStore(xanadu::Store *store) noexcept { boundStore_ = store; }
  [[nodiscard]] xanadu::Store *boundStore() noexcept { return boundStore_; }
  [[nodiscard]] const xanadu::Store *boundStore() const noexcept {
    return boundStore_;
  }

  [[nodiscard]] CellRef predicateEqual() const noexcept { return predEqual_; }
  [[nodiscard]] CellRef predicateUnify() const noexcept { return predUnify_; }
  [[nodiscard]] CellRef predicateMember() const noexcept { return predMember_; }
  [[nodiscard]] CellRef predicateAppend() const noexcept { return predAppend_; }
  [[nodiscard]] CellRef predicateLength() const noexcept { return predLength_; }
  [[nodiscard]] CellRef predicateSetting() const noexcept {
    return predSetting_;
  }
  [[nodiscard]] CellRef predicateSettingShape() const noexcept {
    return predSettingShape_;
  }
  [[nodiscard]] CellRef predicateSettingDefault() const noexcept {
    return predSettingDefault_;
  }
  [[nodiscard]] CellRef predicateCellValue() const noexcept {
    return predCellValue_;
  }
  [[nodiscard]] CellRef predicateCellLink() const noexcept {
    return predCellLink_;
  }
  [[nodiscard]] CellRef predicateTransclude() const noexcept {
    return predTransclude_;
  }
  [[nodiscard]] CellRef predicateXanalink() const noexcept {
    return predXanalink_;
  }
  [[nodiscard]] CellRef predicateCellSpan() const noexcept {
    return predCellSpan_;
  }
  [[nodiscard]] CellRef predicateBridgeEdge() const noexcept {
    return predBridgeEdge_;
  }

  bool solveOnce(CellRef goal, std::span<const CellRef> customPredicates = {});
  bool solveOnce(std::span<const CellRef> goals,
                 std::span<const CellRef> customPredicates = {});

  std::vector<LogicSolution>
  solveQuery(CellRef goal, std::span<const CellRef> customPredicates = {},
             std::size_t maxSolutions = 100);
  std::vector<LogicSolution>
  solveQuery(std::span<const CellRef> goals,
             std::span<const CellRef> customPredicates = {},
             std::size_t maxSolutions                  = 100);

  bool solve(CellRef goal,
             std::function<bool(const LogicSolution &)> onSolution,
             std::span<const CellRef> customPredicates = {},
             std::size_t maxSolutions                  = 100);
  bool solve(std::span<const CellRef> goals,
             std::function<bool(const LogicSolution &)> onSolution,
             std::span<const CellRef> customPredicates = {},
             std::size_t maxSolutions                  = 100);

  // -- Module 9: sys:array ----------------------------------------------------
  /// Nullopt is the valid empty array for n == 0. A supplied origin must exist.
  std::optional<CellRef>
  arrayIota(std::size_t n, std::optional<DimRef> dim = std::nullopt,
            std::optional<CellRef> origin = std::nullopt);
  /// Absence means the origin is invalid; a present vector is its extents.
  [[nodiscard]] std::optional<std::vector<std::size_t>>
  arrayShape(CellRef origin, std::span<const DimRef> dims) const;
  /// Nullopt is a valid empty result when count is zero or the source rank is
  /// empty. Invalid origins throw std::invalid_argument.
  std::optional<CellRef> arrayTake(CellRef origin, DimRef dim,
                                   std::size_t count);
  std::optional<CellRef> arrayDrop(CellRef origin, DimRef dim,
                                   std::size_t count);
  /// Nullopt is a valid empty result for an empty source rank.
  std::optional<CellRef> arrayReverse(CellRef origin, DimRef dim);
  /// Absence means an invalid origin; a present zero remains a valid tally.
  [[nodiscard]] std::optional<std::size_t> arrayTally(CellRef origin,
                                                      DimRef dim) const;

  // -- Module 10: std:zigzag --------------------------------------------------
  [[nodiscard]] std::optional<CellRef> zzStep(std::optional<CellRef> cursor,
                                              DimRef dim,
                                              DimVector dir = DimVector::POS);
  std::optional<CellRef> zzInsert(CellRef cursor, DimRef dim, DimVector dir,
                                  std::string_view text);
  std::optional<CellRef> zzUnlink(CellRef cursor, DimRef dim,
                                  DimVector dir = DimVector::POS);
  std::optional<CellRef> zzLink(CellRef cellA, CellRef cellB, DimRef dim,
                                DimVector dir = DimVector::POS);
  std::optional<CellRef> zzDelete(CellRef cell);
  std::optional<CellRef> zzCloneToChain(CellRef symbolOp, CellRef targetCell);
  std::optional<CellRef> zzDuplicate(CellRef cell);
  std::optional<CellRef> zzMakeCell(std::string_view text);
  [[nodiscard]] std::optional<std::string> zzTextOf(CellRef cell);
  bool zzSetText(CellRef cell, std::string_view text);
  bool zzSetValue(CellRef cell, const CellValue &val);
  [[nodiscard]] std::optional<DimRef> zzResolveDim(std::string_view name);
  std::optional<std::string>
  zzPromote(CellRef root, std::optional<std::size_t> maxOps = std::nullopt);

  // -- Module 11: std:env -----------------------------------------------------
  void setArgs(std::vector<std::string> args) { cliArgs_ = std::move(args); }
  [[nodiscard]] const std::vector<std::string> &args() const noexcept {
    return cliArgs_;
  }
  std::optional<CellRef> envArgs();
  [[nodiscard]] std::optional<std::string> envGetVar(std::string_view key);

  // -- Module 12: std:io ------------------------------------------------------
  [[nodiscard]] std::optional<std::string> ioReadFile(std::string_view path);
  [[nodiscard]] std::optional<std::string> ioReadStdin();
  void ioPrint(std::string_view text);
  void ioEprint(std::string_view text);

  // -- Module 13: std:map -----------------------------------------------------
  CellRef createMap();
  bool mapSet(CellRef mapRoot, const CellValue &key, const CellValue &val);
  void mapSet(CellRef mapRoot, std::string_view key, const CellValue &val);
  void mapSet(CellRef mapRoot, const char *key, const CellValue &val) {
    mapSet(mapRoot, std::string_view(key), val);
  }
  [[nodiscard]] std::optional<CellValue> mapGet(CellRef mapRoot,
                                                const CellValue &key) const;
  [[nodiscard]] std::optional<CellValue> mapGet(CellRef mapRoot,
                                                std::string_view key) const;
  [[nodiscard]] std::optional<CellValue> mapGet(CellRef mapRoot,
                                                const char *key) const {
    return mapGet(mapRoot, std::string_view(key));
  }
  [[nodiscard]] bool mapHas(CellRef mapRoot, const CellValue &key) const;
  [[nodiscard]] bool mapHas(CellRef mapRoot, std::string_view key) const;
  [[nodiscard]] bool mapHas(CellRef mapRoot, const char *key) const {
    return mapHas(mapRoot, std::string_view(key));
  }
  [[nodiscard]] std::vector<CellValue> mapKeys(CellRef mapRoot) const;
  [[nodiscard]] std::size_t mapLen(CellRef mapRoot) const;
  [[nodiscard]] std::optional<CellRef> mapKeyCell(CellRef mapRoot,
                                                  const CellValue &key) const;
  [[nodiscard]] std::optional<CellRef> mapKeyCell(CellRef mapRoot,
                                                  std::string_view key) const;
  [[nodiscard]] std::optional<CellRef> mapKeyCell(CellRef mapRoot,
                                                  const char *key) const {
    return mapKeyCell(mapRoot, std::string_view(key));
  }

  // -- Module 14: std:hypertime -----------------------------------------------
  [[nodiscard]] std::optional<std::string> hyperLca(std::string_view a,
                                                    std::string_view b);
  [[nodiscard]] std::vector<std::string> hyperPath(std::string_view v);
  [[nodiscard]] bool hyperIsAncestorOf(std::string_view a, std::string_view d);
  [[nodiscard]] std::optional<std::int64_t> hyperDepth(std::string_view v);
  [[nodiscard]] std::optional<std::string>
  hyperJumpAncestor(std::string_view v, std::int64_t steps);
  [[nodiscard]] std::optional<std::string> hyperParent(std::string_view v);
  [[nodiscard]] std::optional<std::string> hyperNext(std::string_view v);
  [[nodiscard]] std::vector<std::string> hyperChildren(std::string_view v);
  [[nodiscard]] std::optional<std::string> hyperBranch(std::string_view v,
                                                       std::int64_t ordinal);
  [[nodiscard]] std::string hyperBranchLetters(std::int64_t ordinal);
  [[nodiscard]] std::optional<std::string> hyperLatest();
  [[nodiscard]] std::optional<std::string> hyperHead();
  [[nodiscard]] std::vector<std::string> hyperHeads();
  bool hyperRepointHead(std::string_view v);
  [[nodiscard]] std::optional<std::string> hyperToStr(std::string_view v);
  [[nodiscard]] std::optional<std::string> hyperParse(std::string_view s);

  // -- Module 11: std:gc ------------------------------------------------------
  std::size_t gcSweep();

  // -- Module 12: std:ui ------------------------------------------------------
  static void swapAxes(ViewAxisBinding &axes);
  static void cycleDims(ViewAxisBinding &axes, bool forward = true);
  static void applyBundle(ViewAxisBinding &axes, DimensionBundle bundle);
  static void setView(ViewAxisBinding &axes, std::string_view dimX,
                      std::string_view dimY, std::string_view dimZ);

  // -- Module 13: std:nav -----------------------------------------------------
  std::optional<CellRef> hopHead(CellRef cursor, DimRef dim);
  std::optional<CellRef> hopTail(CellRef cursor, DimRef dim);
  [[nodiscard]] CellRef jumpHome() const noexcept;

  // -- Module 14: std:bridge --------------------------------------------------
  /// Returns primary document text from the store as an ephemeral cell.
  CellRef bridgeDocText(const xanadu::Store &store);
  /// Returns primary version string of the store as an ephemeral cell.
  CellRef bridgeStoreVersion(const xanadu::Store &store);
  /// Links or transcludes a Zigzag cell into a Xanadoc document span.
  bool bridgeCellToDoc(xanadu::Store &store, CellRef cell,
                       std::uint32_t docOffset);
  /// Transcludes a document span into a new Zigzag cell.
  CellRef bridgeDocToCell(xanadu::Store &store, std::uint32_t docOffset,
                          std::uint32_t length);
  /// Queries transcopyright royalty for a Zigzag cell.
  static std::optional<xanadu::TranscopyrightDescriptor>
  bridgeCellRoyalty(const xanadu::ZigzagPresentationSurface &surface,
                    CellRef cell);
  /// Unlocks a transcopyright-locked Zigzag cell, paying royalty via state
  /// channel.
  static bool bridgeCellUnlock(xanadu::ZigzagPresentationSurface &surface,
                               CellRef cell);

  // -- Module 15: std:goal (vicon goal-directed evaluation) -------------------
  //
  // design/vicon-implementation.md's export table, over the suspension
  // protocol it pins: a generator produces by writing its `out` operand cell
  // and jumping to the op its `cont` operand names (prefilled with
  // goalHaltOp()); resuming lands the cursor on a #Fail op (goalFailOp()),
  // which pops the generator's top choice point, releases its Mark, and
  // continues at the recorded altOp; exhaustion is a #Fail finding no choice
  // point, answered as std::nullopt -- Icon failure as a value, never an
  // error (spec section 4.1).

  /// The shared suspension point every std:goal producer's `cont` operand
  /// names by default: run() stops on it without executing, which *is*
  /// Icon's suspension.
  [[nodiscard]] CellRef goalHaltOp() const noexcept { return goalHalt_; }
  /// The shared resume op: landing a suspended cursor on it backtracks into
  /// the generator's top choice point.
  [[nodiscard]] CellRef goalFailOp() const noexcept { return goalFail_; }
  /// The Halt a host-driven activation pushes beneath its generator's frames
  /// so a drained sequence lands on it instead of consuming choice points
  /// that are not its own.
  [[nodiscard]] CellRef goalSentinelOp() const noexcept {
    return goalSentinel_;
  }

  /// Spawn a cursor at @p entryOp, guard it with goalSentinelOp(), and run
  /// to the first produce -- PC == @p haltOp -- or to exhaustion. The live
  /// cursor on produce (its `out` operand holds the result, and the cursor
  /// may be resumed); nullopt is Icon failure, not an error.
  [[nodiscard]] std::optional<CellRef>
  goalProduce(CellRef entryOp, CellRef haltOp, std::string_view name = {});
  /// Resume @p cursor's generator once: land it on @p failOp and run to the
  /// next produce (PC == @p haltOp). nullopt when the #Fail found nothing --
  /// the sequence ran dry.
  [[nodiscard]] std::optional<CellRef>
  goalResume(CellRef cursor, CellRef failOp, CellRef haltOp);

  /// Bounded first produce of the generator at @p entryOp: on produce the
  /// result is read from @p outCell and the live choice points are committed
  /// (discarded, keeping the writes); an absent @p outCell is unit (`true`),
  /// while a present false, zero or empty text value stays present. On
  /// exhaustion the activation is
  /// unwound. This is section 4.4's bounded expression as a host call.
  [[nodiscard]] std::optional<CellValue>
  goalBoundedFirst(CellRef entryOp, std::optional<CellRef> outCell,
                   CellRef haltOp);
  /// Every produce of @p entryOp, collected eagerly -- the synchronous form
  /// of `every e` (spec section 5.1); absent @p outCell produces unit (`true`).
  /// The lazy resume-per-result form is what the vicon compiler emits inline.
  [[nodiscard]] std::vector<CellValue>
  goalResults(CellRef entryOp, std::optional<CellRef> outCell, CellRef haltOp);

  /// The operand cells bound for routine @p entryOp: the argument cells
  /// call() writes, in contract order. Empty for an unregistered op.
  [[nodiscard]] std::span<const CellRef> routineInputs(CellRef entryOp) const;
  /// Its result cells (what call() and goal drivers read back).
  [[nodiscard]] std::span<const CellRef> routineOutputs(CellRef entryOp) const;

  // -- d.scan: the &subject/&pos scanning environment (spec section 6.1) ------
  /// Push a scanning cursor for @p subject onto the tail of home's +d.scan
  /// rank -- the posward-most cursor is the current one. The cursor's content
  /// is &pos (1-based, as Icon); its +d.grab is the subject cell. Answers the
  /// cursor, or nullopt when @p subject is not in the arena.
  std::optional<CellRef> scanPush(CellRef subject);
  /// Drop @p cursor out of the d.scan rank, healing the gap so the enclosing
  /// scan's cursor becomes current again.
  void scanPop(CellRef cursor);
  /// The current scanning cursor: the posward-most cell on home's +d.scan
  /// rank, or nullopt when no scan is open.
  [[nodiscard]] std::optional<CellRef> scanCursor() const;
  /// &subject: the current cursor's +d.grab cell.
  [[nodiscard]] std::optional<CellRef> scanSubject() const;
  /// &pos: the current cursor's content as a 1-based position.
  [[nodiscard]] std::optional<std::int64_t> scanPos() const;
  /// Set &pos on the current cursor. A write under a live mark is trailed,
  /// so an enclosing #Fail restores it (spec section 6.2). false when no
  /// scan is open.
  bool scanSetPos(std::int64_t pos);

  // String Scanning goal routines (spec section 6.2)
  std::optional<std::string> goalTab(std::int64_t targetPos);
  std::optional<std::string> goalMove(std::int64_t delta);
  std::optional<std::int64_t> goalMatch(std::string_view prefix);
  std::optional<std::int64_t> goalMany(std::string_view charset);
  std::optional<std::int64_t> goalUpto(std::string_view charset);
  std::optional<std::int64_t> goalAny(std::string_view charset);
  std::optional<std::int64_t> goalFind(std::string_view needle);
  std::optional<std::int64_t>
  goalPos(std::optional<std::int64_t> target = std::nullopt);
  std::optional<std::string> goalSubjectText();

  // -- Sovereign Store Library Packaging (Zero YAML) --------------------------
  bool exportModuleToStore(std::string_view modulePath,
                           xanadu::Store &destStore) const;
  [[nodiscard]] std::expected<xanadu::MicroversionId, std::string>
  exportImageToStore(
      xanadu::Store &destStore,
      std::int64_t contentVersion = kStdlibContentVersion) const;
  bool exportStandardLibraryToStore(xanadu::Store &destStore) const;
  std::optional<CellRef> importModuleFromStore(const xanadu::Store &srcStore);

private:
  /// Policy-driven ensure: image graft when the resolved source allows it,
  /// C++ bootstrap otherwise. Runs once per instance.
  void ensureStdlib();
  /// The texts of the cells on @p from's rank along @p dim, after @p from.
  [[nodiscard]] std::vector<std::string> namesAfter(CellRef from,
                                                    DimRef dim) const;
  /// A fresh arena cell holding @p val.
  CellRef valueCell(const CellValue &val);
  /// A fresh cell with @p source's value: its typed bits when it has them,
  /// its rendering otherwise.
  CellRef copyValueCell(CellRef source);
  /// @p dim, or d.step when a list builtin was handed no dimension.
  [[nodiscard]] DimRef listDim(std::optional<DimRef> dim) const noexcept;
  /// The cell @p r hops along @p dRow then @p c along @p dCol from the root.
  [[nodiscard]] std::optional<CellRef> gridCell(CellRef gridRoot, std::size_t r,
                                                std::size_t c, DimRef dRow,
                                                DimRef dCol) const;

  CellRef getOrCreateModule(std::string_view modulePath);
  void exportSymbol(CellRef moduleCell, std::string_view symbolName,
                    CellRef entryOp);

  void buildMathModule(CellRef mod);
  void buildStringModule(CellRef mod);
  void buildContractModule(CellRef mod);
  void buildPipelineModule(CellRef mod);
  void buildMemoizeModule(CellRef mod);
  void buildFunctionalModule(CellRef mod);
  void buildCollectionsModule(CellRef mod);
  void buildLogicModule(CellRef mod);
  void buildGoalModule(CellRef mod);
  void buildArrayModule(CellRef mod);
  void buildZigzagModule(CellRef mod);
  void buildGCModule(CellRef mod);
  void buildUiModule(CellRef mod);
  void buildNavModule(CellRef mod);
  void buildBridgeModule(CellRef mod);
  void buildXuzzModule(CellRef mod);
  void buildXuduModule(CellRef mod);
  void buildEnvModule(CellRef mod);
  void buildIoModule(CellRef mod);
  void buildMapModule(CellRef mod);
  void buildHypertimeModule(CellRef mod);
  struct ModuleBuilder {
    std::string_view name;
    void (VortexStdLib::*build)(CellRef);
  };
  inline static constexpr std::array moduleBuilders_{
      ModuleBuilder{"std:math", &VortexStdLib::buildMathModule},
      ModuleBuilder{"std:string", &VortexStdLib::buildStringModule},
      ModuleBuilder{"std:contract", &VortexStdLib::buildContractModule},
      ModuleBuilder{"std:pipeline", &VortexStdLib::buildPipelineModule},
      ModuleBuilder{"std:memoize", &VortexStdLib::buildMemoizeModule},
      ModuleBuilder{"std:functional", &VortexStdLib::buildFunctionalModule},
      ModuleBuilder{"std:collections", &VortexStdLib::buildCollectionsModule},
      ModuleBuilder{"std:logic", &VortexStdLib::buildLogicModule},
      ModuleBuilder{"std:goal", &VortexStdLib::buildGoalModule},
      ModuleBuilder{"sys:array", &VortexStdLib::buildArrayModule},
      ModuleBuilder{"std:zigzag", &VortexStdLib::buildZigzagModule},
      ModuleBuilder{"std:gc", &VortexStdLib::buildGCModule},
      ModuleBuilder{"std:ui", &VortexStdLib::buildUiModule},
      ModuleBuilder{"std:nav", &VortexStdLib::buildNavModule},
      ModuleBuilder{"std:bridge", &VortexStdLib::buildBridgeModule},
      ModuleBuilder{"std:xuzz", &VortexStdLib::buildXuzzModule},
      ModuleBuilder{"std:xudu", &VortexStdLib::buildXuduModule},
      ModuleBuilder{"std:env", &VortexStdLib::buildEnvModule},
      ModuleBuilder{"std:io", &VortexStdLib::buildIoModule},
      ModuleBuilder{"std:map", &VortexStdLib::buildMapModule},
      ModuleBuilder{"std:hypertime", &VortexStdLib::buildHypertimeModule},
  };
  [[nodiscard]] std::expected<CellRef, std::string>
  keyRecord(CellRef owner, std::string_view key, CellRef value);
  [[nodiscard]] std::expected<void, std::string>
  persistModuleBindings(CellRef moduleCell);
  [[nodiscard]] std::expected<void, std::string>
  appendModuleOpcodes(std::size_t firstCell);
  [[nodiscard]] std::expected<void, std::string>
  persistRoutineBinding(CellRef symbol, CellRef entryOp);
  VortexCore &core_;
  VortexVM &vm_;
  Bootstrap bootstrapMode_{Bootstrap::Automatic};
  bool imageOpsBegun_{false};
  bool imageManifestEnded_{false};
  bool bootstrapComplete_{false};
  CellRef imageOpsHead_{noCell};
  CellRef imageOpsTail_{noCell};
  std::unordered_set<std::string> builtModules_;
  xanadu::Store *boundStore_{nullptr};
  std::vector<std::string> cliArgs_;

  // Stage 2 data-parallel hook: borrowed, not owned. Null => serial.
  VortexWorkerPool *workerPool_{nullptr};
  std::size_t parallelThreshold_{1024};

  struct RoutineBinding {
    std::vector<CellRef> inputParams;
    std::vector<CellRef> outputParams;
  };
  std::unordered_map<CellRef, RoutineBinding> routineBindings_;

  // std:goal's shared suspension plumbing, minted by buildGoalModule().
  CellRef goalHalt_{noCell};     ///< the produce-side Jump target
  CellRef goalFail_{noCell};     ///< the resume-side backtrack entry
  CellRef goalSentinel_{noCell}; ///< activation floor for host-driven drains

  CellRef predEqual_{noCell};
  CellRef predUnify_{noCell};
  CellRef predMember_{noCell};
  CellRef predAppend_{noCell};
  CellRef predLength_{noCell};
  CellRef predVortexFunction_{noCell};
  CellRef predVortexModule_{noCell};
  CellRef predVortexInstruction_{noCell};
  CellRef predVortexContract_{noCell};
  CellRef predVortexParam_{noCell};
  CellRef predSetting_{noCell};
  CellRef predSettingShape_{noCell};
  CellRef predSettingDefault_{noCell};
  CellRef predCellValue_{noCell};
  CellRef predCellLink_{noCell};
  CellRef predTransclude_{noCell};
  CellRef predXanalink_{noCell};
  CellRef predCellSpan_{noCell};
  CellRef predBridgeEdge_{noCell};
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_STDLIB_HPP
