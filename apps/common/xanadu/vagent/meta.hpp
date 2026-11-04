/**
 * @file meta.hpp
 * @brief The `std:meta` non-mutation surface, engine level
 *        (design/vagent-metaprogramming.md §6.1, §6.2, §6.7, §7.2 —
 *        phase 3 of design/vagent-metaprogramming-implementation.md).
 *
 * `std:meta` is a prelude in the `std:agent` style: its routines are Vortex
 * walks over program structure against roots the *adapter* binds (V10's
 * mechanism — data-minted cells and dimensions, never C++ registration).
 * This TU is the kernel-side implementation those routines run: the read
 * group (`program` through `label_of`, `validate`, `weight_of`, `value_of`,
 * `next_rand`), `run`'s `#Call` frame convention, and `reify`'s `+d.recipe`
 * replay. The mutation verbs (`graft_*`, `learn_*`, `prune`, `unlearn`,
 * `set_weight`, `sprout_synapse`) are phase 4 and are not here.
 *
 * The bind set is harness-supplied in this phase: `mintMetaBindSet` performs
 * the no-marks mint the adapter performs at genesis — the twelve apparatus
 * dims filed on the `d.dims` rank, the `validate` constants as data cells,
 * plus the two non-dim roots (program root, agent cell) bound by hand.
 * Resolvers never mint except through the documented session channels
 * (memo records, wrapper heads, `d.validate` tag cells, `d.clone` capture
 * appends, `d.vars` records and the serial counter).
 */
#ifndef COMMON_XANADU_VAGENT_META_HPP
#define COMMON_XANADU_VAGENT_META_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

/// The durable monotone serial's `d.vars` record key on the agent cell —
/// the one counter `reify`'s `<serial>`, `patch:<n>`'s `<n>` and
/// `validate`'s pass serial all draw (§6.1, §6.7, §7.2). A `+d.vars → [key]
/// +d.values → Int64` record, minted on first draw.
inline constexpr std::string_view kMetaSerialKey = "meta.serial";

/// The twelve data-minted apparatus dimensions §6.1 binds — data, never
/// `SystemDimensions` fields, minted through the V10 mechanism on the
/// no-marks path and filed on the persistent `d.dims` rank.
struct MetaDimensions {
  DimRef code{noCell};      ///< d.code — agent → registered fragment heads
  DimRef patch{noCell};     ///< d.patch — agent → patch records (learn EDL)
  DimRef weight{noCell};    ///< d.weight — junction/record scalar edge
  DimRef synapse{noCell};   ///< d.synapse — agent's junction rank
  DimRef entry{noCell};     ///< d.entry — head → entry op
  DimRef patchHead{noCell}; ///< d.patch-head — patch → head-name chain
  DimRef validate{noCell};  ///< d.validate — validate()'s tag-cell dim
  DimRef refs{noCell};      ///< d.refs — head's marking inventory rank
  DimRef ref{noCell};       ///< d.ref — record → the cell it marks
  DimRef recipe{noCell};    ///< d.recipe — head's replay template rank
  DimRef refFor{noCell};    ///< d.refFor — ref-capture record → slot
  DimRef meshRelocation{noCell}; ///< d.mesh.relocation — "cell" marker edge
};

/// `validate`'s adapter-bound constants (§6.1: the enum's integers are
/// serialized data, so they arrive as bound cells, not prelude literals):
///
/// - `linkedCeiling` — an Int64 scalar cell holding the `OpcodeKind::Linked`
///   ordinal, the `int64`-decodes-in-`[0, Linked]` test's bound.
/// - `arityTable` — a cell whose `+d.step` rank lists one `(op, in, out)`
///   Int64 triple per op kind, in the recipe-tuple convention: three
///   consecutive members name the op ordinal, the input-wing arity and the
///   output-wing arity. A visited op kind absent from the table fails.
/// - `impureKinds` — a cell whose `+d.step` rank lists the Int64 op ordinals
///   a `pure`-flagged fragment may not contain.
struct MetaValidateConstants {
  CellRef linkedCeiling{noCell};
  CellRef arityTable{noCell};
  CellRef impureKinds{noCell};
};

/// `std:meta`'s runtime roots — everything the resolvers start from.
struct MetaBindSet {
  CellRef programRoot{noCell}; ///< `program()` — the program root cell.
  CellRef agentCell{noCell};   ///< `agent_cell()` — this agent's own cell.
  MetaDimensions dims{};
  MetaValidateConstants validate{};
};

/// The phase-3 harness mint: files the twelve apparatus dims (V10's
/// mechanism — `findOrMintDimension` per name, which resolves a filed name
/// and mints only the missing), then mints the three `validate` constant
/// cells. Refused with a named error while any mark is outstanding:
/// `arenaDims_` is an untrailed map (`arena_manifold.cpp:78-105`), so a
/// session-side mint under live marks desyncs the dim table on release
/// (the P17-class hazard §6.5 names). `arena` must therefore be quiescent.
/// The two non-dim roots arrive by argument — the harness owns which cells
/// play program and agent.
[[nodiscard]] std::expected<MetaBindSet, std::string>
mintMetaBindSet(zigzag::ArenaManifold &arena, VortexCore &core,
                CellRef programRoot, CellRef agentCell);

/// A `run` call's outcome (§6.2). `produced` false is soft expression
/// failure inside the caller's choice structure; `refusal` names the
/// headless/arity/descriptor refusal classes, which are refusals, not
/// failures.
struct MetaRunOutcome {
  enum class Refusal : std::uint8_t {
    None,
    NotAHead,      ///< operand carries no +d.entry / +d.values descriptor
    NoDescriptor,  ///< descriptor lacks the result/produced records
    ArityMismatch, ///< args.size() != descriptor's declared arity
    VmFault,       ///< the VM run itself did not succeed
  };
  Refusal refusal{Refusal::None};
  bool produced{false};
  std::optional<CellValue> result;
  std::string detail;
};

/// `validate`'s report (§7.2): `ok` false names the first failed clause.
struct MetaValidateReport {
  bool ok{false};
  std::string detail;
};

/// The options §7.2's walk needs from the calling context: whether the
/// graph is `run`-reachable (the Return-tail and bound-output-New checks)
/// and whether the fragment's descriptor flags it `pure` (the
/// impure-kind-set check).
struct MetaValidateOptions {
  bool runReachable{true};
  bool expectPure{false};
};

/**
 * @brief `std:meta`'s engine-level routines over one bound root set.
 *
 * Every resolver is a read over `binds_` plus the arena; the documented
 * writes are the session channels the spec assigns: memo records and
 * wrapper heads (`test_of`/`body_of`), `d.validate` tag cells
 * (`validate`), `d.vars` records and the serial counter (`weight_of`'s
 * keyed read is a read — `nextSerial` is the only writer of the record),
 * and `reify`'s fresh copy. Refusals come back as `std::expected` errors
 * or `MetaRunOutcome::Refusal`, never as thrown `Diagnostic`s.
 */
class MetaRuntime {
public:
  MetaRuntime(VortexCore &core, VortexVM &vm, MetaBindSet binds)
      : core_(core), vm_(vm), binds_(binds) {}

  [[nodiscard]] const MetaBindSet &binds() const noexcept { return binds_; }
  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] VortexVM &vm() noexcept { return vm_; }
  [[nodiscard]] zigzag::ArenaManifold &arena() const noexcept {
    return core_.arena();
  }

  // -- Roots -------------------------------------------------------------
  [[nodiscard]] CellRef program() const noexcept { return binds_.programRoot; }
  [[nodiscard]] CellRef agentCell() const noexcept { return binds_.agentCell; }

  // -- The reflect family (§6.1/§6.7) --------------------------------------

  /// `module_named(path)`: the module cell carrying @p path verbatim on the
  /// program root's `+d.stdlib` rank (the `findModule` shape), or absent.
  [[nodiscard]] std::optional<CellRef> moduleNamed(std::string_view path) const;

  /// `symbol_named(owner, name)`: the `d.vars` symbol cell on the module
  /// @p owner names whose content is @p name verbatim (the `exportSymbol`
  /// shape — the name lives as cell content, not a `+d.name` edge).
  [[nodiscard]] std::optional<CellRef> symbolNamed(std::string_view owner,
                                                   std::string_view name) const;

  /// `clauses_of(cell)`: the cell's `+d.clause` rank — plans of a goal,
  /// handlers of an event type.
  [[nodiscard]] std::vector<CellRef> clausesOf(CellRef cell) const;

  /// `test_of(clause)` / `body_of(clause)`: wrap the clause's `+d.grab` /
  /// `+d.spin` graph in a session-side head (`+d.entry → the wrapped root
  /// op`), memoized clause-side per §6.1: the `clause +d.code → +d.step`
  /// record chain `{+d.values → wrapper, +d.name → kind label, +d.grab →
  /// scalar key cell}`, keyed by the attach cell's `+d.name` text. A
  /// label-less attach never memo-hits and gets no record.
  [[nodiscard]] std::optional<CellRef> testOf(CellRef clause);
  [[nodiscard]] std::optional<CellRef> bodyOf(CellRef clause);

  /// `plan_named(qualified)`: first-`:` split — the qualifier resolves as a
  /// module through `moduleNamed`, narrowing the clause-rank walk to that
  /// module; otherwise the whole string is the name and every module's
  /// clause ranks are walked. Either arm matches each clause's `+d.name`
  /// label *verbatim* against the full input string (declared labels are
  /// the qualified spelling; learned clauses carry `patch:<n>`).
  [[nodiscard]] std::optional<CellRef>
  planNamed(std::string_view qualified) const;

  /// `code_named(label)`: the agent's `+d.code` rank first (the
  /// `learn`-registered heads), then on a miss the program-image label
  /// walk — a bounded outbound walk over the program's structural dim
  /// vocabulary for a `+d.entry`-bearing head whose `+d.name` text is
  /// @p label verbatim. No first-`:` split: head labels only.
  [[nodiscard]] std::optional<CellRef> codeNamed(std::string_view label) const;

  /// `label_of(cell)`: the cell's `+d.name` label text, or absent.
  [[nodiscard]] std::optional<std::string> labelOf(CellRef cell) const;

  // -- Reads ---------------------------------------------------------------

  /// `validate(c)`: the §7.2 bounded well-formedness walk — op `int64`s
  /// decode in `[0, ceiling]` off the bound ceiling cell, wing arities
  /// match the bound table, control-op targets resolve to ops, no
  /// bound-output `New` in `run`-reachable graphs, `+d.spin` tails are
  /// `Return`, and a `pure`-flagged graph holds no bound-impure op.
  /// Tag-cells on `d.validate` carry the pass serial off the agent
  /// cell's `d.vars` counter.
  [[nodiscard]] MetaValidateReport
  validate(CellRef graphRoot, MetaValidateOptions options = {}) const;

  /// `weight_of(cell)`: a junction reads its own `+d.weight` scalar; a
  /// `+d.name`-bearing non-junction reads the agent's `d.vars` record
  /// keyed by the label verbatim; everything else disengages.
  [[nodiscard]] std::optional<double> weightOf(CellRef cell) const;

  /// `value_of(cell)`: the cell's rendered content — the `Code<Cell>`
  /// companion read.
  [[nodiscard]] std::optional<CellValue> valueOf(CellRef cell) const;

  /// `next_rand(seed)`: the §8.6 modulus LCG over the seed cell's `Int`
  /// content — 32-bit halves reassembled, no step exceeding `Int64`
  /// (the C++ form of the prelude's `DivMod`/`Mod` routine). A
  /// non-`Int` seed cell draws the mix of 0.
  [[nodiscard]] std::int64_t nextRand(CellRef seed) const;

  // -- run / reify ---------------------------------------------------------

  /// The head-vs-op discriminator (§6.1 :1141-1144): a clause's attach
  /// cell resolves to its graph root directly when it decodes as an op
  /// (`int64` in `[0, Linked]` against the bound ceiling), and by
  /// following `+d.entry` exactly once when it is a head. Everything else
  /// — a bare data cell — is absent.
  [[nodiscard]] std::optional<CellRef> graphRoot(CellRef attach) const;

  /// `run(c, args)`'s engine half: operand is the *head* (the `Code`
  /// value's referent — the caller's slot stays head-valued per §6.2;
  /// `runBox` takes the ref-box and derefs its `int64`). Refused when the
  /// operand carries no `+d.entry`/`+d.values` descriptor (the
  /// headless-`run` arm — a `def` symbol's `+d.values` names an entry op,
  /// never a head), on descriptor arity mismatch, or when the descriptor
  /// lacks the `result`/`produced` records. Spills `result`/`produced`,
  /// pushes the `#Call` frame (the `vortex_vm.cpp:854-893` shape: frame
  /// `Int64` = successor op, pushed on the cursor's `+d.stack`), binds
  /// each argument by parameter name on the frame's `d.vars`/`d.values`,
  /// jumps to `+d.entry`, then restores the spill and reports `produced`.
  /// `cursor` absent spawns a fresh cursor — the kernel-simulable shape.
  [[nodiscard]] MetaRunOutcome
  runFragment(CellRef head, const std::vector<CellValue> &args,
              std::optional<CellRef> cursor = std::nullopt,
              std::size_t maxCycles         = 100'000);
  /// The ref-box spelling: `int64` referent of @p box must name a head.
  [[nodiscard]] MetaRunOutcome
  runBox(CellRef box, const std::vector<CellValue> &args,
         std::optional<CellRef> cursor = std::nullopt,
         std::size_t maxCycles         = 100'000);

  /// `reify(c)`'s recipe replay (§6.1): walks the head's `+d.recipe`
  /// chain — `(from, dim, dir, to)` tuples anchored on the `+d.name`-
  /// bearing `dim` member, literal-content members, `+d.refFor` ref-
  /// capture records (internal referents rebound through the old→new
  /// map, external verbatim, `+d.name`-keyed dims re-resolved through
  /// `ensureDimension`'s name arm) — rebuilding every copied cell
  /// `New`/`Link`/`Value` (`Clone` is never used — §11's refusal covers
  /// it as a graph primitive, and the `cap$N` re-clone append is `New` +
  /// `Link` onto the binding's `d.clone` tail). Regenerates the copy's
  /// `+d.refs` (name-records re-link to fresh verbatim name cells;
  /// ref-records re-point at the mapped cells, each carrying a fresh
  /// `d.mesh.relocation → "cell"` marker) and a self-describing
  /// `+d.recipe`, and mints the copy's `reify:<src>:<serial>` label off
  /// the agent cell's `d.vars` counter.
  ///
  /// Refused on: operand not a `+d.entry` head; a recipe field naming a
  /// dim the arena cannot resolve (and `ensureDimension`'s name arm is
  /// *resolve-only under live marks* — a would-be mint with marks
  /// outstanding is this refusal, the `arenaDims_` desync §6.5 rules
  /// out); a recipe member outside the stated taxonomy.
  [[nodiscard]] std::expected<CellRef, std::string> reify(CellRef head);

  /// The durable serial off the agent cell's `d.vars` counter —
  /// `kMetaSerialKey`'s record, minted at 0 on first draw, post-increment
  /// per call. The one counter `reify:<src>:<serial>`, `patch:<n>` and
  /// `validate`'s pass serial share.
  [[nodiscard]] std::expected<std::int64_t, std::string> nextSerial() const;

private:
  /// A descriptor's `+d.vars` record whose content is @p key verbatim.
  [[nodiscard]] std::optional<CellRef>
  descriptorField(CellRef descriptor, std::string_view key) const;
  /// The descriptor's parameter cells — `+d.step` chain off `+d.grab` in
  /// declaration order — each `+d.name`-labelled, `+d.values`-typed.
  [[nodiscard]] std::vector<CellRef> parameterCells(CellRef descriptor) const;
  /// Whether @p cell decodes as an op under the bound ceiling.
  [[nodiscard]] bool isOpCell(CellRef cell) const;
  /// `ensureDimension`'s name arm, marks-aware: resolves a filed/bound
  /// name; mints only with no marks outstanding; else a named refusal.
  [[nodiscard]] std::expected<DimRef, std::string>
  resolveDimension(std::string_view name) const;
  /// Mint or find the `+d.vars` record keyed @p key on @p owner.
  [[nodiscard]] std::optional<CellRef>
  varsRecord(CellRef owner, std::string_view key, bool mint) const;
  /// The `test_of`/`body_of` shared wrap+memo walk.
  [[nodiscard]] std::optional<CellRef> wrap(CellRef clause, DimRef attachDim,
                                            std::string_view memoKind);

  VortexCore &core_;
  VortexVM &vm_;
  MetaBindSet binds_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_META_HPP
