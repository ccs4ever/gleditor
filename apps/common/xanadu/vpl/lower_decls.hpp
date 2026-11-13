/**
 * @file lower_decls.hpp
 * @brief Seams between the VPL compiler's dispatch and the per-card lowering
 *        files.
 *
 * compiler.cpp decides which verb or form it is looking at and calls exactly
 * one function declared here. Each function lives in its own lower_*.cpp, so a
 * card that lowers a verb edits only that file. Every function receives the
 * AST node plus whatever the dispatch site had already computed, and returns
 * what the site returns. Until its card lands, each one throws the message the
 * dispatch site threw before the split, under the same condition.
 *
 * Cards do not add members to VPLCompiler or functions to this header; if a
 * card needs another accessor on LowerContext, that is a finding to report.
 */
#ifndef COMMON_XANADU_VPL_LOWER_DECLS_HPP
#define COMMON_XANADU_VPL_LOWER_DECLS_HPP

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/compiler.hpp"

namespace xanadu::vpl {

/**
 * Narrow public view of a VPLCompiler for the lowering files. Forwarders only:
 * no lowering policy lives here. Cheap to construct, so each dispatch site
 * makes its own.
 */
class LowerContext {
public:
  explicit LowerContext(VPLCompiler &compiler) noexcept : compiler_(compiler) {}

  [[nodiscard]] zigzag::vortex::VortexCore &core() const noexcept {
    return compiler_.core_;
  }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() const noexcept {
    return compiler_.vm_;
  }
  [[nodiscard]] bool optimizing() const noexcept { return compiler_.optimize_; }

  // Compile a sub-expression through the full dispatch.
  std::optional<zigzag::CellRef> compileNode(const AstNode &node) const {
    return compiler_.compileNode(node);
  }

  // Emission. emitOp appends to the instruction chain at once; mintOpcode
  // makes an opcode that is not in the chain yet (a branch target), and
  // appendOpcode puts a minted one at the chain's tail.
  zigzag::CellRef emitOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {}) const {
    return compiler_.emitOp(kind, label);
  }
  zigzag::CellRef mintOpcode(zigzag::vortex::OpcodeKind kind,
                             std::string_view label = {}) const {
    return compiler_.mintOpcode(kind, label);
  }
  void appendOpcode(zigzag::CellRef op) const { compiler_.appendOpcode(op); }
  void emitInput(zigzag::CellRef op, zigzag::CellRef operand) const {
    compiler_.emitInput(op, operand);
  }
  void emitOutput(zigzag::CellRef op, zigzag::CellRef target) const {
    compiler_.emitOutput(op, target);
  }
  zigzag::CellRef emitConstant(
      const zigzag::vortex::CellValue &value,
      std::optional<zigzag::CellRef> designatedCell = std::nullopt) const {
    return compiler_.emitConstant(value, designatedCell);
  }
  zigzag::CellRef
  emitInputConstant(zigzag::CellRef op,
                    const zigzag::vortex::CellValue &value) const {
    return compiler_.emitInputConstant(op, value);
  }
  [[nodiscard]] zigzag::DimRef resolveDimension(std::string_view name) const {
    return compiler_.resolveDimension(name);
  }

  // Static array facts. Valence is axes().size(); a cell with no entry is a
  // scalar.
  [[nodiscard]] bool isRank(zigzag::CellRef cell) const {
    return compiler_.rankAxes_.contains(cell);
  }
  [[nodiscard]] const std::vector<zigzag::DirectedDim> *
  axesOf(zigzag::CellRef cell) const {
    const auto found = compiler_.rankAxes_.find(cell);
    return found == compiler_.rankAxes_.end() ? nullptr : &found->second;
  }
  /// The registers holding each axis's stop cell (parallel to axesOf), or
  /// nullptr when the value is unbounded on every axis.
  [[nodiscard]] const AxisStops *stopsOf(zigzag::CellRef cell) const {
    const auto found = compiler_.rankStops_.find(cell);
    return found == compiler_.rankStops_.end() ? nullptr : &found->second;
  }
  /// The stop register of axis `index` of `cell`, absent when unbounded.
  [[nodiscard]] std::optional<zigzag::CellRef> stopOf(zigzag::CellRef cell,
                                                      std::size_t index) const {
    const auto *stops = stopsOf(cell);
    return stops != nullptr && index < stops->size() ? (*stops)[index]
                                                     : std::nullopt;
  }
  [[nodiscard]] std::size_t valenceOf(zigzag::CellRef cell) const {
    const auto *axes = axesOf(cell);
    return axes == nullptr ? 0U : axes->size();
  }
  [[nodiscard]] bool isEnclosed(zigzag::CellRef cell) const {
    return compiler_.enclosedCells_.contains(cell);
  }
  void markEnclosed(zigzag::CellRef cell) const {
    compiler_.enclosedCells_.insert(cell);
  }
  [[nodiscard]] const std::vector<zigzag::DirectedDim> *
  dimensionValuesOf(zigzag::CellRef cell) const {
    const auto found = compiler_.dimensionValues_.find(cell);
    return found == compiler_.dimensionValues_.end() ? nullptr : &found->second;
  }
  void setDimensionValues(zigzag::CellRef cell,
                          std::vector<zigzag::DirectedDim> dims) const {
    compiler_.dimensionValues_[cell] = std::move(dims);
  }
  /// Records that the program asked for `⎕ x`; surfaces as
  /// CompilationResult::resultPrint.
  void requestPrint() const { compiler_.printRequested_ = true; }

  // The side tables themselves, for code that already writes them directly.
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::DirectedDim>> &
  rankAxes() const {
    return compiler_.rankAxes_;
  }
  std::unordered_map<zigzag::CellRef, AxisStops> &rankStops() const {
    return compiler_.rankStops_;
  }
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>> &
  rankDependencies() const {
    return compiler_.rankDependencies_;
  }
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>> &
  cellDependencies() const {
    return compiler_.cellDependencies_;
  }
  [[nodiscard]] std::vector<zigzag::CellRef>
  dependenciesOf(zigzag::CellRef cell) const {
    return compiler_.dependenciesOf(cell);
  }
  [[nodiscard]] std::vector<zigzag::CellRef>
  rankDependenciesOf(zigzag::CellRef cell) const {
    return compiler_.rankDependenciesOf(cell);
  }

private:
  VPLCompiler &compiler_;
};

/// Appends the cells of `from` that `into` lacks, keeping first-seen order.
inline void mergeDependencies(std::vector<zigzag::CellRef> &into,
                              const std::vector<zigzag::CellRef> &from) {
  for (const auto cell : from) {
    if (std::ranges::find(into, cell) == into.end()) into.push_back(cell);
  }
}

inline zigzag::CellRef
requireCompiledCell(const std::optional<zigzag::CellRef> result) {
  if (!result) {
    throw std::invalid_argument("VPL expression has no compiled cell");
  }
  return *result;
}

/// True for a bare dimension (`d.1`) or a nonempty vector of dimensions
/// (`(d.1 d.2 d.3)`): the right operand shape of the view form of dyadic ⍴.
[[nodiscard]] inline bool isDimensionOperand(const AstNode *node) {
  if (node == nullptr) return false;
  if (dynamic_cast<const DimensionExpr *>(node) != nullptr) return true;
  const auto *vec = dynamic_cast<const VectorExpr *>(node);
  if (vec == nullptr || vec->elements().empty()) return false;
  return std::ranges::all_of(vec->elements(), [](const auto &element) {
    return element && dynamic_cast<const DimensionExpr *>(element.get());
  });
}

namespace lower {

using zigzag::CellRef;
using Maybe = std::optional<zigzag::CellRef>;

/// The refusal texts shared by many stubs. One copy each, so a card cannot
/// drift a duplicate; tests match substrings of these.
namespace msg {
inline constexpr std::string_view monadicUnsupported =
    "unsupported VPL monadic verb in VM compiler";
inline constexpr std::string_view monadicOnRank =
    "VPL scalar monadic arithmetic on a rank requires array lowering";
inline constexpr std::string_view dyadicUnsupported =
    "unsupported VPL dyadic verb in VM compiler";
inline constexpr std::string_view dyadicOnRank =
    "VPL scalar dyadic arithmetic on ranks requires array lowering";
inline constexpr std::string_view customUnsupported =
    "unsupported VPL custom verb in VM compiler";
inline constexpr std::string_view reduceScanVerbs =
    "VPL VM reduce/scan supports only +, *, maximum, and minimum";
inline constexpr std::string_view eachUnsupported =
    "VPL VM each adverb is unsupported";
inline constexpr std::string_view dimensionValues =
    "VPL dimension values require typed-handle VM lowering";
inline constexpr std::string_view indexing =
    "VPL indexing is unsupported by the VM";
inline constexpr std::string_view conjunctions =
    "VPL conjunctions are unsupported by the VM";
inline constexpr std::string_view quad = "VPL quad IO is unsupported by the VM";
inline constexpr std::string_view nestedRanks =
    "nested VPL ranks require explicit array lowering";
} // namespace msg

[[noreturn]] inline void refuse(std::string_view message) {
  throw std::invalid_argument(std::string(message));
}

/// Post-operand monadic refusal: the rank message for a rank operand, else the
/// generic unsupported-verb message.
[[noreturn]] inline void refuseMonadicOperand(const LowerContext &ctx,
                                              CellRef operand) {
  refuse(ctx.isRank(operand) ? msg::monadicOnRank : msg::monadicUnsupported);
}

/// Post-operand dyadic refusal: the rank message if either operand is a rank.
[[noreturn]] inline void refuseDyadicOperands(const LowerContext &ctx,
                                              CellRef left, CellRef right) {
  refuse(ctx.isRank(left) || ctx.isRank(right) ? msg::dyadicOnRank
                                               : msg::dyadicUnsupported);
}

// ---------------------------------------------------------------------------
// Monadic verbs refused before the operand is compiled (compileMonadic).
// The stub receives the unevaluated node: compile expr.right() yourself.
// ---------------------------------------------------------------------------
Maybe lowerIotaMonadic(LowerContext &ctx, const MonadicExpr &expr);      // V3
Maybe lowerRhoMonadic(LowerContext &ctx, const MonadicExpr &expr);       // V3
Maybe lowerTransposeMonadic(LowerContext &ctx, const MonadicExpr &expr); // V4
/// ReverseFirst (⌽) and ReverseLast (⊖); tell them apart by expr.verb().
Maybe lowerReverseMonadic(LowerContext &ctx, const MonadicExpr &expr);  // V4
Maybe lowerPowerMonadic(LowerContext &ctx, const MonadicExpr &expr);    // V6
Maybe lowerSignumMonadic(LowerContext &ctx, const MonadicExpr &expr);   // V6
Maybe lowerFloorMonadic(LowerContext &ctx, const MonadicExpr &expr);    // V6
Maybe lowerCeilingMonadic(LowerContext &ctx, const MonadicExpr &expr);  // V6
Maybe lowerRavelMonadic(LowerContext &ctx, const MonadicExpr &expr);    // V8
Maybe lowerEncloseMonadic(LowerContext &ctx, const MonadicExpr &expr);  // V10
Maybe lowerDiscloseMonadic(LowerContext &ctx, const MonadicExpr &expr); // V10

// ---------------------------------------------------------------------------
// Monadic verbs after the operand is compiled (rCell). Each stub handles the
// rank operand and the scalar operand alike: today it throws the rank message
// when ctx.isRank(rCell), otherwise the generic unsupported-verb message.
// ---------------------------------------------------------------------------
Maybe lowerTallyMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell); // V7
/// GradeUp (⍋) and GradeDown (⍒); tell them apart by expr.verb().
Maybe lowerGradeMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell); // V11
Maybe lowerWhereMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell); // V11
Maybe lowerMatchMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell); // V11
Maybe lowerHypertimeMonadic(LowerContext &ctx, const MonadicExpr &expr,
                            CellRef rCell); // V12
Maybe lowerTranscludeMonadic(LowerContext &ctx, const MonadicExpr &expr,
                             CellRef rCell); // V12
Maybe lowerScrubMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell); // V13
/// Minus, Magnitude, Not, Divide on a rank operand. Scalar operands stay
/// lowered in compileMonadic; this is called only when ctx.isRank(rCell).
Maybe lowerMonadicArithmeticOnRank(LowerContext &ctx, const MonadicExpr &expr,
                                   CellRef rCell); // V6

// ---------------------------------------------------------------------------
// Monadic custom verbs (expr.hasCustomVerb()).
// ---------------------------------------------------------------------------
/// `f¨ x` (adverb Each). Today: the reduce/scan verb check, then the each
/// refusal.
Maybe lowerEachMonadic(LowerContext &ctx, const MonadicExpr &expr,
                       const AdverbExpr &adverb); // V6
/// `f/ x`, `f\ x`, and every custom verb that is not Each or a conjunction.
/// The reduce/scan lowering that exists today lives here, unchanged.
Maybe lowerFoldMonadic(LowerContext &ctx, const MonadicExpr &expr); // V7
Maybe lowerOuterProductMonadic(LowerContext &ctx, const MonadicExpr &expr,
                               const ConjunctionExpr &conjunction); // V9
/// Every other conjunction: RankOp (⍤), Key (⌸), ...
Maybe lowerConjunctionMonadic(LowerContext &ctx, const MonadicExpr &expr,
                              const ConjunctionExpr &conjunction); // V13

// ---------------------------------------------------------------------------
// Dyadic verbs refused before the operands are compiled (compileDyadic).
// ---------------------------------------------------------------------------
CellRef lowerIotaDyadic(LowerContext &ctx, const DyadicExpr &expr); // V3
/// Dyadic ⍴ whose right operand is a dimension or vector of dimensions
/// (isDimensionOperand): `H ⍴ (d.1 d.2 d.3)`.
///
/// The test is purely syntactic. `H ⍴ D` with D bound to a dimension value, or
/// a vector mixing dimensions and other elements, takes the other branch and
/// reaches lowerRhoDyadic, which must consult ctx.dimensionValuesOf(rCell)
/// after compiling its operands and, for a dimension value, call
/// lowerRhoViewCompiled.
CellRef lowerRhoViewDyadic(LowerContext &ctx, const DyadicExpr &expr); // V4
/// V4's view logic on already compiled operands (rCell carries dimension
/// values). The entry point V9 calls from lowerRhoDyadic so it never edits
/// V4's file; lowerRhoViewDyadic may call it too. Today: throws
/// msg::dyadicUnsupported.
CellRef lowerRhoViewCompiled(LowerContext &ctx, const DyadicExpr &expr,
                             CellRef lCell, CellRef rCell); // V4
/// Every other dyadic ⍴: link form and reshape form. Must route a right
/// operand with dimension values to lowerRhoViewCompiled (see above).
CellRef lowerRhoDyadic(LowerContext &ctx, const DyadicExpr &expr);      // V9
CellRef lowerTakeDyadic(LowerContext &ctx, const DyadicExpr &expr);     // V5
CellRef lowerDropDyadic(LowerContext &ctx, const DyadicExpr &expr);     // V5
CellRef lowerPowerDyadic(LowerContext &ctx, const DyadicExpr &expr);    // V6
CellRef lowerCatenateDyadic(LowerContext &ctx, const DyadicExpr &expr); // V8
CellRef lowerScrubDyadic(LowerContext &ctx, const DyadicExpr &expr);    // V13

// ---------------------------------------------------------------------------
// Dyadic verbs after both operands are compiled (lCell, rCell). Same rule as
// the monadic post-operand group: rank message if either operand is a rank,
// else the generic unsupported-verb message.
// ---------------------------------------------------------------------------
CellRef lowerTransposeDyadic(LowerContext &ctx, const DyadicExpr &expr,
                             CellRef lCell, CellRef rCell); // V4
CellRef lowerMemberDyadic(LowerContext &ctx, const DyadicExpr &expr,
                          CellRef lCell, CellRef rCell); // V11
CellRef lowerMatchDyadic(LowerContext &ctx, const DyadicExpr &expr,
                         CellRef lCell, CellRef rCell); // V11
/// `mask ⌿ w` parses as a plain DyadicExpr with verb Compress.
CellRef lowerCompressDyadic(LowerContext &ctx, const DyadicExpr &expr,
                            CellRef lCell, CellRef rCell); // V13
/// Plus, Minus, Times, Divide, Magnitude, Ceiling, Floor, Equal, NotEqual,
/// LessThan, LessEqual, GreaterThan, GreaterEqual, And, Or when either
/// operand is a rank. Two scalars stay lowered in compileDyadic.
CellRef lowerDyadicArithmeticOnRank(LowerContext &ctx, const DyadicExpr &expr,
                                    CellRef lCell, CellRef rCell); // V6

// ---------------------------------------------------------------------------
// Dyadic custom verbs (expr.hasCustomVerb()), checked first in compileDyadic.
// Operands are compiled and checked exactly as for a plain verb, then one of
// these runs; each throws the rank or generic message as above.
// ---------------------------------------------------------------------------
CellRef lowerEachDyadic(LowerContext &ctx, const DyadicExpr &expr,
                        const AdverbExpr &adverb, CellRef lCell,
                        CellRef rCell); // V6
/// Adverb Reduce or Scan with a left operand.
CellRef lowerFoldDyadic(LowerContext &ctx, const DyadicExpr &expr,
                        const AdverbExpr &adverb, CellRef lCell,
                        CellRef rCell); // V7
CellRef lowerOuterProductDyadic(LowerContext &ctx, const DyadicExpr &expr,
                                const ConjunctionExpr &conjunction,
                                CellRef lCell, CellRef rCell); // V9
CellRef lowerConjunctionDyadic(LowerContext &ctx, const DyadicExpr &expr,
                               const ConjunctionExpr &conjunction,
                               CellRef lCell, CellRef rCell); // V13

// ---------------------------------------------------------------------------
// Other forms.
// ---------------------------------------------------------------------------
/// A bare dimension `d.1`. A dimension value has no cells, so this returns an
/// alias register (emit::aliasRegister of a cleared register) on which it calls
/// ctx.setDimensionValues({dimension, POS}). Today: throws
/// msg::dimensionValues.
CellRef lowerDimension(LowerContext &ctx, const DimensionExpr &expr); // V4
/// A literal vector whose every compiled element carries dimension values
/// (`(d.1 d.2 d.3)` standalone). Returns an alias register carrying the
/// concatenated dimension values via ctx.setDimensionValues. Unreachable until
/// V4 makes lowerDimension succeed (the element compile throws first); today it
/// throws msg::dimensionValues, the same message the element compile gives.
Maybe lowerDimensionVector(LowerContext &ctx, const VectorExpr &expr,
                           const std::vector<CellRef> &cells); // V4
/// A literal vector with at least one element that is itself a rank. `cells`
/// are the compiled elements, in order.
Maybe lowerVectorOfRanks(LowerContext &ctx, const VectorExpr &expr,
                         const std::vector<CellRef> &cells);      // V8
Maybe lowerIndexing(LowerContext &ctx, const IndexingExpr &expr); // V11
/// A standalone ConjunctionExpr (`A ⍤ 1 B`, `W ⌸ f W`).
CellRef lowerConjunctionExpr(LowerContext &ctx,
                             const ConjunctionExpr &expr); // V13
/// ⎕ (print marker), ⎕READ and ⎕SPLIT: tell them apart by expr.quadKind().
Maybe lowerQuad(LowerContext &ctx, const QuadExpr &expr); // V13

} // namespace lower
} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_LOWER_DECLS_HPP
