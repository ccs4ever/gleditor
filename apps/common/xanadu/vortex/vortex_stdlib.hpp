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
 */
#ifndef COMMON_XANADU_VORTEX_STDLIB_HPP
#define COMMON_XANADU_VORTEX_STDLIB_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {

struct LogicSolution {
  std::vector<std::pair<CellRef, CellRef>> bindings;
  std::unordered_map<CellRef, CellRef> varMap;
  std::unordered_map<std::string, std::string> formatted;
};

class VortexStdLib {
public:
  VortexStdLib(VortexCore &core, VortexVM &vm);

  // -- System Bootstrap -------------------------------------------------------
  /// Weaves the 7 standard modules into the manifold along +d.stdlib off home.
  void bootstrap();

  // -- Spatial Symbol Resolution (import) -------------------------------------
  /// Resolves "std:<module>/<symbol>" to its entry opcode or "std:<module>" to
  /// module cell.
  [[nodiscard]] CellRef resolve(std::string_view path) const;

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
  CellRef map(CellRef head, DimRef inDim, DimRef outDim,
              std::function<CellValue(const CellValue &)> fn);
  CellRef map(CellRef head, DimRef inDim, DimRef outDim, CellRef fnOp);

  CellRef filter(CellRef head, DimRef inDim, DimRef outDim,
                 std::function<bool(const CellValue &)> pred);
  CellRef filter(CellRef head, DimRef inDim, DimRef outDim, CellRef predOp);

  CellValue
  fold(CellRef head, DimRef inDim, CellValue initial,
       std::function<CellValue(const CellValue &, const CellValue &)> fn);
  CellValue fold(CellRef head, DimRef inDim, CellValue initial, CellRef fnOp);

  CellRef zip(CellRef headA, CellRef headB, DimRef dimA, DimRef dimB,
              DimRef outDim);

  // -- Module 5: std:collections ----------------------------------------------
  CellRef createList(const std::vector<CellValue> &items, DimRef dim = noCell);
  [[nodiscard]] std::vector<CellValue> listToVector(CellRef head,
                                                    DimRef dim = noCell) const;
  void pushBack(CellRef head, const CellValue &val, DimRef dim = noCell);
  CellRef pushFront(CellRef head, const CellValue &val, DimRef dim = noCell);
  std::optional<CellValue> popBack(CellRef head, DimRef dim = noCell);
  CellRef popFront(CellRef head, DimRef dim = noCell);
  [[nodiscard]] std::size_t listLength(CellRef head, DimRef dim = noCell) const;

  CellRef createMap();
  void mapSet(CellRef mapRoot, std::string_view key, const CellValue &val);
  [[nodiscard]] std::optional<CellValue> mapGet(CellRef mapRoot,
                                                std::string_view key) const;
  [[nodiscard]] bool mapHas(CellRef mapRoot, std::string_view key) const;
  [[nodiscard]] std::vector<std::string> mapKeys(CellRef mapRoot) const;

  CellRef createGrid(std::size_t rows, std::size_t cols, DimRef dRow,
                     DimRef dCol,
                     const CellValue &initVal = static_cast<std::int64_t>(0));
  [[nodiscard]] CellValue getGrid(CellRef gridRoot, std::size_t r,
                                  std::size_t c, DimRef dRow,
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
  [[nodiscard]] CellRef deref(CellRef cell) const;
  bool unify(CellRef a, CellRef b);
  [[nodiscard]] std::vector<CellRef> argumentsOf(CellRef term) const;
  [[nodiscard]] std::string functorOf(CellRef term) const;
  [[nodiscard]] std::string renderTerm(CellRef term) const;

  CellRef createPredicate(std::string_view name);
  CellRef addClause(CellRef predCell, CellRef headTerm,
                    std::span<const CellRef> bodyGoals = {});

  [[nodiscard]] CellRef predicateEqual() const noexcept { return predEqual_; }
  [[nodiscard]] CellRef predicateUnify() const noexcept { return predUnify_; }
  [[nodiscard]] CellRef predicateMember() const noexcept { return predMember_; }
  [[nodiscard]] CellRef predicateAppend() const noexcept { return predAppend_; }
  [[nodiscard]] CellRef predicateLength() const noexcept { return predLength_; }

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

private:
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

  VortexCore &core_;
  VortexVM &vm_;

  struct RoutineBinding {
    std::vector<CellRef> inputParams;
    std::vector<CellRef> outputParams;
  };
  std::unordered_map<CellRef, RoutineBinding> routineBindings_;

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
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_STDLIB_HPP
