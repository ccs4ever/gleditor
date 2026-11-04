#ifndef COMMON_XANADU_VAGENT_COMPILER_HPP
#define COMMON_XANADU_VAGENT_COMPILER_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vagent {

struct CompilerBinding {
  zigzag::CellRef cell;
  Type type;
  // §10.1's state-side marker bit: set by the binding-table driver when
  // the cell is a state-side checkpointed slot rather than a frame-bound
  // capture — the provenance phase-3 fragment emission records for
  // `closed` re-verification (a linked capture is discoverable by edge;
  // `cells"$cap_N"` anchors resolve only on stable captures).
  bool stateSide{false};
};
using CompilerBindings = std::map<std::string, CompilerBinding, std::less<>>;

enum class ResultUse { Value, Discard };

struct ScalarCompilerLimits {
  std::size_t maxNodes{65'536};
  std::size_t maxDepth{256};
  // §5.5's staged-evaluation budget: the sibling of CheckLimits' bounds —
  // a divergent `static` would otherwise loop the compile forever. Passed
  // to VortexVM::run as the operand's cycle bound per `static` node.
  std::size_t staticFuel{100'000};
};

struct CompiledStatement {
  zigzag::CellRef entry;
  zigzag::CellRef result;
  zigzag::CellRef produced;
  // Absence means statically non-producing (fail), not an untyped value.
  std::optional<Type> resultType;
};

// ---- Phase 3 fragment emission (fragment.cpp consumes these) ---------

// The pipeline-side tables the Emitter resolves code"…" literals and
// `run` callsites through — expand's fragment table and per-literal
// metadata plus the checker's computed FragmentInfo. Defined in
// fragment.hpp; forward-declared so the scalar API needs no
// expand/checker includes.
struct FragmentContext;

// §6.1's marking inventory item — one emitter-minted int64 ref cell
// (a wing slot, a jump/branch/choice `reference`, a ref-box, a
// `dimConstant`, or a callsite convention cell). `name` present makes
// the +d.refs record a NAME-record (+d.name→name, +d.grab→role) whose
// referent re-resolves by label/name at genesis; absent it is a
// REF-record (marker + ref-capture pairing). `referent` classifies the
// content endpoint for the ref-capture record's content channel.
struct EmissionRef {
  enum class Referent : std::uint8_t { Internal, External, Dim };

  zigzag::CellRef cell{};
  std::optional<std::string> name;
  std::string role; // "call" | "run" — only meaningful when name set
  Referent referent{Referent::Internal};
  // The referent's source position or dim arena id — the ref-capture
  // record's int64 content (for Dim, the arena-local id; the record
  // carries +d.name→dimName as the re-resolution channel).
  std::int64_t target{0};
  std::string dimName;
};

// One arena link the fragment emission wrote — a +d.recipe link-tuple's
// source: four +d.step-consecutive members (from, dim, dir, to), the
// `dim` member name-keyed through its own +d.name edge (dim identity
// re-resolves by name at genesis, never by position).
struct EmissionLink {
  zigzag::CellRef from{};
  zigzag::DimRef dim{};
  bool neg{false};
  zigzag::CellRef to{};
  std::string dimName;
};

// A cell the fragment emission minted — replayed as a +d.recipe
// literal record (a marked member naming the source cell by int64
// position). `value` absent means an unvalued cell (a temporary or
// output slot) — the twin mints empty and the runtime writes fill it;
// present, the twin's content is written at replay (from the source
// cell, which carries the same content).
struct EmissionLiteral {
  zigzag::CellRef cell{};
  std::optional<zigzag::vortex::CellValue> value;
};

// §5.3's capture-slot wing shape: the operand wraps the captured
// binding's cell — an int64(binding) slot appended to the binding's
// +d.clone rank tail, decoded at run time like any other slot while
// the clone-rank membership gives reify its append target. Recorded
// as a +d.recipe ref-capture member carrying +d.refFor→slot and
// +d.values→binding.
struct EmissionCapture {
  zigzag::CellRef slot{};
  zigzag::CellRef binding{};
};

// What the scalar Emitter produces for one code"…" body — the graph
// cells compileFragment hangs the head and descriptor on, plus the
// emission bookkeeping the +d.refs/+d.recipe record schema consumes.
struct FragmentBody {
  zigzag::CellRef entry{};
  zigzag::CellRef result{};
  zigzag::CellRef produced{};
  std::optional<Type> resultType;
  std::vector<EmissionRef> refs;
  std::vector<EmissionLink> links;
  std::vector<EmissionLiteral> literals;
  std::vector<EmissionCapture> captures;
};

// A code"…" body ready for the scalar Emitter: declared parameters
// seeded as locals (the prologue's per-name #RESOLVE ops fill them by
// name off the running cursor's d.vars/d.values channel — the VM's
// one vars rank, since a pushed call frame holds only the return op),
// the enclosing binding set for cap$N capture slots, and the
// expansion / check tables nested literals and `run` callsites
// resolve through.
struct FragmentBodyRequest {
  const Expr *body{};
  std::vector<Parameter> parameters{};
  const CompilerBindings *bindings{};
  const FragmentContext *fragments{};
  ResultUse use{ResultUse::Value};
};

// First Vortex-lowering iteration: scalar literals/typed bindings, arithmetic,
// string concatenation, comparisons, &, |, -/not, scalar :=/<-, fail, lexical
// block locals, if, and §5.5 `static` staged evaluation (the operand is
// lowered, run in the compile arena under staticFuel with no mark
// outstanding, and folded in as a literal). A block is one nested expression
// in this API, not a plan-body sequencer: failure rolls the whole statement
// back. Each child statement takes its first result. Conditional conditions
// are pure and test success, not Bool truth. Value-producing conditional
// arms share a scalar type (or fail); discarded arms may differ and produce
// Unit on success. Surviving code"…"/splice nodes are named compile
// refusals — expansion completes before lowering. Unsupported syntax is
// refused; this is not an agent runner or a full compiler.
class Compiler {
public:
  Compiler(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm,
           ScalarCompilerLimits limits = {})
      : core_(core), vm_(vm), limits_(limits) {}

  // All graph cells must be assembled before turn marks or choice points exist.
  // Binding cells must outlive the graph and retain their declared scalar
  // types. Unsupported/name/type/shape errors mint nothing. Allocation-budget
  // failures propagate as resource faults; there is no arena rollback of VM
  // registrations. Discard affects result use, independently of whether the
  // position allows bounded-only operations; successful discarded results are
  // Unit, while failures still produce no value.
  //
  // `fragments` supplies the expansion/check tables a surviving code"…"
  // literal or `run` callsite lowers through; without it both stay the
  // phase-2 named refusals they were.
  //
  // Run entry on the owner's existing cursor. Only after normal Halt may the
  // caller inspect produced (Bool); result is meaningful only when it is true.
  // The graph bounds the first result and preserves the caller's choice depth.
  // A core numeric contract fault must be backtracked/resumed by the adapter,
  // as specified in §4.2; run failure is not a completed statement result.
  [[nodiscard]] std::expected<CompiledStatement, Diagnostic>
  compileStatement(const Expr &statement, const CompilerBindings &bindings = {},
                   ResultUse use                    = ResultUse::Value,
                   const FragmentContext *fragments = nullptr);

  // The scalar-Emitter seam compileFragment wraps: emit one checked
  // code"…" body — parameters seeded as locals whose
  // prologue binds them name-keyed through #RESOLVE ops, spill/publish
  // records for the descriptor's result/produced cells, a #Return tail —
  // and collect the refs/links/literals/captures bookkeeping the head's
  // +d.refs/+d.recipe ranks record (§6.1).
  [[nodiscard]] std::expected<FragmentBody, Diagnostic>
  compileFragmentBody(const FragmentBodyRequest &request);

  // The program-image bookkeeping §6.6's programRef record reads: ops
  // minted in mint order (the canonical post-fold positions for a
  // linearly-appended session image) plus the emission floor — the arena
  // cell count before this compiler's first emission.
  [[nodiscard]] const std::vector<zigzag::CellRef> &programOps() const {
    return programOps_;
  }
  [[nodiscard]] std::size_t imageFloor() const { return imageFloor_; }
  [[nodiscard]] zigzag::vortex::VortexCore &core() const { return core_; }

  // Emission bookkeeping the compiler.cpp Emitter and fragment.cpp
  // record through; only emissions inside a fragment or statement
  // image append — `static` eval residue is not program code.
  void recordProgramOp(zigzag::CellRef op) { programOps_.push_back(op); }
  void noteEmissionStart() {
    if (!imageFloorSet_) {
      imageFloor_    = core_.arena().cellCount();
      imageFloorSet_ = true;
    }
  }

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  ScalarCompilerLimits limits_;
  std::vector<zigzag::CellRef> programOps_;
  std::size_t imageFloor_{0};
  bool imageFloorSet_{false};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_COMPILER_HPP
