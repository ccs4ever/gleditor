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
 * `next_rand`), `run`'s `#Call` frame convention, `reify`'s `+d.recipe`
 * replay, and phase 4's mutation surface — the `graft_*` session verbs,
 * their durable `learn_*` twins, `prune`/`unlearn`, `set_weight` and
 * `sprout_synapse`. The `learn_*` verbs write `d.patch` records and
 * `d.code` registrations an incarnation-local EDL can replay; crossing
 * a store boundary is phase 6's transport seam, not this surface.
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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vagent/p16.hpp"
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
  DimRef lineage{noCell};        ///< d.lineage — reify twin → source head rank
  DimRef kindRegistered{
      noCell};               ///< d.kind-registered — durable registered heads
  DimRef pendingReg{noCell}; ///< d.pending-reg — trailed pending reg queue
  DimRef astKinds{noCell};   ///< d.ast.kinds — R1 AST kind vocabulary rank
};

struct MetaRuntimeLimits {
  std::size_t maxRegisterOps{512};
  std::size_t maxPendingRegisters{64};
  std::size_t maxRegisteredHeads{256};
  std::size_t maxReplayFailures{1};
};

/// `validate`'s adapter-bound constants (§6.1: the enum's integers are
/// serialized data, so they arrive as bound cells, not prelude literals):
///
/// - `linkedCeiling` — an Int64 scalar cell holding the enum's last
///   ordinal, the `int64`-decodes-as-an-op-kind test's bound.
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

/// The program-lineage record `learn_*`'s sideness gate consumes
/// (§6.6): `programRef` is the phase-3 bind record's `program()` — a
/// persistent image ref no later than `imageExtent`, the image's op
/// extent — and `imageExtent` is the bound image refs compare against.
/// An ephemeral arena model compares its dense id instead; a
/// persistent incarnation compares the spool position. Absent, every
/// `learn_*` refuses closed as "no program lineage".
struct MetaLineage {
  CellRef programRef{noCell};
  std::uint32_t imageExtent{0};
};

/// `std:meta`'s runtime roots — everything the resolvers start from.
struct MetaBindSet {
  CellRef programRoot{noCell}; ///< `program()` — the program root cell.
  CellRef agentCell{noCell};   ///< `agent_cell()` — this agent's own cell.
  /// The agent's own module cell — the home bare `goal`/`owner` names in
  /// `graft_*`/`learn_*` resolve against before any `module:` split.
  CellRef ownModule{noCell};
  MetaDimensions dims{};
  MetaValidateConstants validate{};
  MetaRuntimeLimits limits{};
  /// `learn_*`'s sideness/closedness gate — absent binds refuse the
  /// durable verbs fail-closed (§6.6).
  std::optional<MetaLineage> lineage;
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

/// `verifyClosed`'s report (§6.6): `closed` false carries the first
/// refusal class in `detail` (an unnamed program-side reference, a
/// fail-forever run-record); `notes` collect the admitted-but-flagged
/// classes — fail-until-resume name-records ride to genesis, they are
/// not refusals.
struct MetaClosedReport {
  bool closed{false};
  std::string detail;
  std::vector<std::string> notes;
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
  [[nodiscard]] const MetaRuntimeLimits &limits() const noexcept {
    return binds_.limits;
  }
  MetaRuntime *setLimits(MetaRuntimeLimits limits) noexcept {
    binds_.limits = limits;
    return this;
  }
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

  // -- The belief store (actor-executor §3.1/§3.8)
  // ------------------------------
  //
  // Facts are clause cells on a `d.belief` predicate bank — the same
  // shape the event/goal clause banks use: `agent +d.belief → predicate
  // cell +d.clause → fact`, the fact's head term on `+d.grab`, provenance
  // on `+d.source` (a {peer, turn} record), an optional `+d.evidence`
  // edge to the journaled percept. These are the §3.8 host-routine
  // verbs: the statement-position `believes`/`believe`/`forget`/`revise`
  // call them by name; the fragment-position inlined clause-walk is the
  // compiler's lowering, not duplicated here.

  /// The predicate cell on the agent's `+d.belief` rank whose content is
  /// @p predicate, minted on miss when @p mint.
  [[nodiscard]] std::optional<CellRef> beliefBank(std::string_view predicate,
                                                  bool mint = false);
  /// The fact clauses on @p predicate's bank — the bank's `+d.clause` rank.
  [[nodiscard]] std::vector<CellRef>
  beliefsOf(std::string_view predicate) const;
  /// `believes`: a fact on the bank whose `+d.grab` term equals @p term.
  /// V1 matching is structural term equality (content + `+d.step`
  /// children, recursively); unification of variable-bearing patterns is
  /// the fragment-lowering path's job (§3.8's inlined clause walk).
  [[nodiscard]] bool believes(std::string_view predicate, CellRef term) const;
  /// The same structural equality `believes` uses, for callers matching
  /// marshaled terms outside the belief bank (the wake match's
  /// structural `await price("oil", 4)`).
  [[nodiscard]] bool termEquals(CellRef a, CellRef b) const;
  /// Pattern matching for `believes`-style holds queries: '?'-prefixed
  /// pattern cells (marshaled logic variables) match any fact cell.
  /// Existence checking only — no substitution is recorded.
  [[nodiscard]] bool termUnifies(CellRef pattern, CellRef fact) const;
  /// The capturing form: a '?name' cell whose name has no env entry
  /// records the fact child's value under it; one already bound
  /// (a plan param or an earlier conjunct's capture) constrains the
  /// match to that value. '?_' stays a pure wildcard.
  [[nodiscard]] bool
  unifyCollect(CellRef pattern, CellRef fact,
               std::map<std::string, CellValue, std::less<>> &env) const;
  /// A fact/payload cell's bound value: typed bits first, then text
  /// coerced to a literal kind — marshalTerm's text-cells convention
  /// makes "4" and 4 indistinguishable to a bound param without the
  /// coerce. Under the flat +d.step sibling-chain encoding a leaf's
  /// step edge is its next sibling, so step links cannot mark a cell
  /// compound — a nested term resolves to its functor's text.
  [[nodiscard]] CellValue factValue(CellRef cell) const;
  /// `believe`: append the fact clause — `+d.grab → term`, `+d.source →`
  /// a `{peer, turn}` record, `+d.evidence →` the journaled percept when
  /// supplied. Clause-bank bounds (`vagent.max_clauses_per_predicate`)
  /// are the caller's to enforce — the verb has no settings channel.
  [[nodiscard]] std::expected<CellRef, std::string>
  believe(std::string_view predicate, CellRef term, CellRef peer,
          std::int64_t turn, std::optional<CellRef> evidence = std::nullopt);
  /// `forget`: unlink every fact clause whose `+d.grab` term equals
  /// @p term; the unlinked count comes back.
  [[nodiscard]] std::size_t forget(std::string_view predicate, CellRef term);
  /// `revise`: forget the old term, believe the new — same clause
  /// contract, one call.
  [[nodiscard]] std::expected<CellRef, std::string>
  revise(std::string_view predicate, CellRef oldTerm, CellRef newTerm,
         CellRef peer, std::int64_t turn,
         std::optional<CellRef> evidence = std::nullopt);
  /// §belief-trigger staging: walk the agent's and mounted behaviors'
  /// "beliefHandlers" tables for clauses keyed `sign`+`predicate`,
  /// unify each clause's `pattern` against the fact's head, and queue
  /// a +d.pending record per match — bindings marshaled as ordered
  /// +d.step children in the `?` names' first-appearance order. The
  /// handler runs under the settle's own drain, never inside the
  /// asserting step.
  void stageBeliefTriggers(std::string_view predicate, std::string_view sign,
                           CellRef factClause);

  // -- §6.3-amended selection (actor-executor §3.4)
  // -----------------------------
  //
  // Virtual weight-order: the weight ledger rides the agent's keyed
  // `d.vars` records, so ordering is a snapshot taken once per
  // selection — O(V + C) over the rank — never an eager order rank.
  // Candidates sort by (weight descending, `+d.clause` rank position
  // ascending); the first not in @p tried whose `+d.grab` head produces
  // a truthy cell is selected. `where` evaluation runs each guard
  // through `runFragment` under @p whereFuel cycles — a refusal counts
  // as a failed guard and skips the clause, never faults selection.
  //
  // `tried` is the caller's — an intention's tried-set in M4; the
  // empty default means nothing tried. `args` binds the host call's
  // values into each candidate's where-guard positionally (clause
  // params are the plan's own bindings — names can differ from the
  // goal's declared parameters, so position is the contract).
  //
  // The result pairs the clause with its runtime owner — the mounted
  // behavior instance whose table contributed it, `noCell` for the
  // agent's own. Two mounts of one behavior share a clause cell, so
  // the owner travels with the candidate, not in a clause-keyed map.
  [[nodiscard]] std::expected<std::optional<std::pair<CellRef, CellRef>>,
                              std::string>
  selectClause(CellRef host, const std::unordered_set<CellRef> &tried = {},
               std::size_t whereFuel              = 256,
               const std::vector<CellValue> &args = {});

  // -- §3.5 intentions — data, not cursors
  // --------------------------------------
  //
  // An intention is a record cell on the agent's `+d.intention` rank —
  // the spec'd closed schema {id,kind,goal,plan,tried,step,waiting,
  // ready,blocked_until,faulted,frame,delegator} as `d.vars` fields —
  // plus `bodyName`, the registered-head name §3.5's resume identity
  // checks against `plan`'s `+d.spin` target on every resume.
  //
  // `adoptGoal` selects (§3.4's order, empty tried-set — a fresh
  // adoption retries everything) and mints the record: `ready=1`,
  // `step=0`, the `bodyName` snapshot taken at adoption. `std::nullopt`
  // when no clause holds; the caller bounds concurrent intentions
  // (`vagent.max_intentions`) since the verb sees no settings. `args`
  // are the goal call's bound values — recorded on the intention's
  // `args` chain in order and bound positionally when any of the
  // clause's heads (guard, condition tests, spine fragments) run.
  /// `argRefs`, when given, aligns with `args`: a ref arg's source
  /// cell so the chain stores the relocation-marked position — a
  /// structured term arg's Int64 dead-reads verbatim across a fold
  /// otherwise. Empty means no arg is a reference.
  [[nodiscard]] std::expected<CellRef, std::string>
  adoptGoal(CellRef goal, std::int64_t turn, std::size_t whereFuel = 256,
            const std::vector<CellValue> &args = {},
            std::span<const CellRef> argRefs   = {});
  /// The intention record's integer field — `step`, `ready`, `faulted`,
  /// `waiting`, `blocked_until`, `id` — absent field is `std::nullopt`.
  [[nodiscard]] std::optional<std::int64_t>
  intentionField(CellRef intention, std::string_view field) const;
  /// Mint-and-repoint the integer field — the serial idiom, never an
  /// in-place scalar write.
  void setIntentionField(CellRef intention, std::string_view field,
                         std::int64_t value);
  /// The ref-valued twin: the field's Int64 names a live cell — a
  /// delegating intention, a remote member — so it mints through
  /// `refCell`, carrying the relocation manifest that restates the
  /// position at its referent's persistent twin across a fold.
  void setIntentionRef(CellRef intention, std::string_view field,
                       CellRef referent);

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

  /// The head's declared result type spelling off its `+d.values`
  /// descriptor — `std::nullopt` when the head carries no descriptor.
  /// The executor reads it to classify a produced Int64 as a cell
  /// position versus a plain scalar at the durable-store boundary —
  /// `CellValue` alone cannot say which side the bits sit on.
  [[nodiscard]] std::optional<std::string> resultTypeOf(CellRef head) const;

  // -- Mutation surface (§6.1's phase-4 half) ------------------------------

  /// Unit-returning mutation arms name their refusal class in the
  /// error string, never a thrown `Diagnostic`.
  using MetaMutation = std::expected<void, std::string>;

  /// `graft_plan(goal, name, test, body)`: append a `+d.name`-labelled
  /// clause at the goal symbol's `+d.clause` rank tail — `+d.grab` the
  /// test head, `+d.spin` the body head. The test must be pure and its
  /// descriptor signature must match the goal's canonical clause
  /// formals; the body must be `Code<()>`-shaped. Session-only: no
  /// `d.patch` record, no `d.code` registration.
  [[nodiscard]] std::expected<CellRef, std::string>
  graftPlan(std::string_view goal, std::string_view name, CellRef test,
            CellRef body);

  /// `graft_handler(event, name, guard, body, priority)`: the event arm
  /// of `graft_plan` — appends additive (no alias dedupe) and carries
  /// `priority` as a `d.vars` record on the minted clause.
  [[nodiscard]] std::expected<CellRef, std::string>
  graftHandler(std::string_view event, std::string_view name, CellRef guard,
               CellRef body, std::int64_t priority);

  /// `graft_where(clause, test)`: one trailed re-point of the clause's
  /// `+d.grab` at the test head. The old graph stays allocated but
  /// detached. The test is pure-required.
  [[nodiscard]] MetaMutation graftWhere(CellRef clause, CellRef test);

  /// `graft_body(clause, body)`: one trailed re-point of `+d.spin` at
  /// the `Code<()>`-shaped body head.
  [[nodiscard]] MetaMutation graftBody(CellRef clause, CellRef body);

  /// `graft_method(owner, name, code)`: unlink any existing
  /// `owner.name` symbol on the resolved module's `d.vars` (redefinition
  /// is replacement — symbol lookup is first-match-wins), then mint a
  /// symbol cell whose content is the dotted `Agent.member` spelling and
  /// whose `+d.values` names the head.
  [[nodiscard]] std::expected<CellRef, std::string>
  graftMethod(std::string_view owner, std::string_view name, CellRef code);

  /// `prune(clause)`: unlink a clause off its `d.clause` rank with
  /// predecessor→successor repair; the clause and its attach graphs
  /// stay allocated but unranked.
  [[nodiscard]] MetaMutation prune(CellRef clause);

  /// `learn_plan(goal, alias, test, body)`: the durable twin —
  /// re-verify lineage and `closed`, reify a registered head per
  /// argument (reused when the argument is already `d.code`-registered)
  /// plus a fresh installed copy, dedupe the alias verbatim against the
  /// goal's own clause labels, install under a minted `patch:<n>`
  /// label, and append a `d.patch` record carrying the alias.
  [[nodiscard]] std::expected<CellRef, std::string>
  learnPlan(std::string_view goal, std::string_view alias, CellRef test,
            CellRef body);

  /// `learn_handler(event, alias, guard, body, priority)`: additive
  /// like `graft_handler`; the record carries `priority` beside the
  /// alias.
  [[nodiscard]] std::expected<CellRef, std::string>
  learnHandler(std::string_view event, std::string_view alias, CellRef guard,
               CellRef body, std::int64_t priority);

  /// `learn_where(clause, test)`: reify a registered plus an installed
  /// test copy, re-point `+d.grab` at the copy, and record the install
  /// — the patch's `+d.patch-head` chain names the registered head and
  /// any prior learned head the same clause carried. A clause with no
  /// `+d.name` is refused (the durable record needs a resolvable key).
  [[nodiscard]] MetaMutation learnWhere(CellRef clause, CellRef test);

  /// `learn_body(clause, body)`: the `+d.spin` twin of `learn_where`.
  [[nodiscard]] MetaMutation learnBody(CellRef clause, CellRef body);

  /// `learn_method(owner, name, code)`: `graft_method`'s durable twin —
  /// `name` is the functional lookup name verbatim (never minted), the
  /// record's target is the composed `module:Agent.member` spelling.
  [[nodiscard]] std::expected<CellRef, std::string>
  learnMethod(std::string_view owner, std::string_view name, CellRef code);

  /// `unlearn(clause_or_symbol)`: undo the latest `d.patch` whose
  /// recorded target is the argument's name — a `+d.name`-bearing cell
  /// reads as a clause key, a `d.vars` symbol composes `module:content`
  /// off its rank head. Refuses a `d.patch` record argument by its
  /// `-d.patch` backlink. Clause arms unlink the learned clause or
  /// symbol; replace arms re-point through a reified prior head (a
  /// program-original attachment simply detaches — genesis restores
  /// it), then the patch drops and the `d.code` registration it solely
  /// named follows. A dead patch drops with a fault record.
  [[nodiscard]] MetaMutation unlearn(CellRef clauseOrSymbol);

  /// `set_weight(cell, w)`: a `+d.weight`-bearing cell takes a fresh
  /// scalar on that edge; a `+d.name`-bearing cell finds-or-mints the
  /// agent's `d.vars` record keyed by the label and re-points its value
  /// slot — the mint-and-repoint discipline, never an in-place write.
  /// A name-less, weight-less target is refused.
  [[nodiscard]] MetaMutation setWeight(CellRef cell, double weight);

  /// `sprout_synapse(from, to, w)`: mint a junction on the agent's
  /// `+d.synapse` rank carrying `from`/`to` qualified-name strings and
  /// its own `+d.weight` scalar.
  [[nodiscard]] std::expected<CellRef, std::string>
  sproutSynapse(std::string_view from, std::string_view to, double weight);

  /// `learn_*`'s closedness re-verification (§6.6): walks the head's
  /// `+d.refs`/`+d.recipe` frontier — name-records resolve by label,
  /// ref-records must name internal recipe positions or state-side
  /// cells, cap$N bindings must be state-side. Run-role records
  /// classify fail-until-resume (admitted, rides to genesis) versus
  /// fail-forever (refused — a guaranteed-dead patch).
  [[nodiscard]] MetaClosedReport verifyClosed(CellRef head) const;

  // -- Stage 2 & Stage 4 Metacircular Registration & Synthesis ---------------

  /// Whether @p head is committed on `agentCell +d.kind-registered`.
  [[nodiscard]] bool kindRegistered(CellRef head) const;

  /// `registration(h)`: `"absent"` | `"pending"` | `"committed"`.
  [[nodiscard]] std::string_view registration(CellRef head) const;

  /// `dim_named(name)`: resolve a filed dimension by name without minting.
  [[nodiscard]] std::optional<CellRef> dimNamed(std::string_view name) const;

  /// `as_body(c)`: narrow a `Code<T>` head to `Code<()>` after verifying its
  /// descriptor declares `arity == 0` and `result == "()"`.
  [[nodiscard]] std::optional<CellRef> asBody(CellRef head) const;

  /// `var_of(owner, key)`: read `owner`'s `+d.vars` value cell for `key`.
  [[nodiscard]] std::optional<CellRef> varOf(CellRef owner,
                                             std::string_view key) const;

  /// Collect and verify the op cells covered by @p head's `+d.recipe` and
  /// `+d.refs` against its `+d.entry` control-flow reachability.
  [[nodiscard]] std::expected<std::vector<CellRef>, std::string>
  collectCoveredOps(CellRef head) const;

  /// `register(h)`: atomic verification (`headShape` + `validate` +
  /// `verifyClosed` + recipe coverage) and registration. Queues onto
  /// `agentCell +d.pending-reg` when marks or scoped frames are live;
  /// commits immediately to `d.kind-registered`, `d.code`, and `opcodeMap_`
  /// when quiescent.
  [[nodiscard]] std::expected<CellRef, std::string> registerCode(CellRef head);

  /// `register_all(hs)`: mutually-recursive atomic batch registration.
  [[nodiscard]] std::expected<std::vector<CellRef>, std::string>
  registerAll(std::span<const CellRef> heads);

  /// Two-pass post-commit drain of `agentCell +d.pending-reg` at `marks == 0`.
  [[nodiscard]] std::size_t drainPendingRegisters();

  /// Stage 4 2-degree-safe batched value/link join verb (`join_batch`).
  [[nodiscard]] bool joinBatch(CellRef batch);

  /// Stage 4 R2 fragment apparatus builder (`build_fragment`).
  [[nodiscard]] std::optional<CellRef> buildFragmentFromSpec(CellRef spec);

  /// 2-degree AST & metadata navigation and annotation.
  [[nodiscard]] std::optional<CellRef> grabOf(CellRef cell) const;
  [[nodiscard]] std::optional<CellRef> stepOf(CellRef cell) const;
  [[nodiscard]] std::optional<CellRef> valuesOf(CellRef cell) const;
  bool setValues(CellRef cell, CellRef val);
  bool setVar(CellRef owner, std::string_view key, CellRef val);

  /// Append @p head on the agent's `d.code` rank; refuses a second
  ///  head carrying an existing `d.name` label.
  [[nodiscard]] MetaMutation registerHead(CellRef head) const;

private:
  /// A descriptor's declared signature: ordered `(name, type)` pairs
  /// off the `+d.grab → +d.step` parameter chain, the `result` record's
  /// `+d.name` type spelling, and the `pure` flag.
  struct MetaSignature {
    bool descriptor{false};
    std::vector<std::pair<std::string, std::string>> params;
    std::string resultType;
    bool pure{false};
  };
  /// The two reified copies a `learn_*` installs: `registered` lives on
  /// the agent's `d.code`, `installed` is the session-attached twin.
  struct MetaLearnCopies {
    CellRef registered{noCell};
    CellRef installed{noCell};
  };

  /// Read @p head's descriptor signature; `descriptor` false when the
  /// head carries no `+d.values` descriptor (a reflect wrapper).
  [[nodiscard]] MetaSignature signatureOf(CellRef head) const;
  /// Resolve a `goal`/`event` name to its clause-rank host: first-`:`
  /// split when the qualifier resolves as a module, else the bare name
  /// on the agent's own module's `d.vars`.
  [[nodiscard]] std::expected<CellRef, std::string>
  clauseHost(std::string_view name) const;
  /// Resolve a method `owner` (`Agent` or `module:Agent`) to
  /// `{module, agentName}` — bare owners take the agent's own module.
  [[nodiscard]] std::expected<std::pair<CellRef, std::string>, std::string>
  agentOwner(std::string_view owner) const;
  /// The shared fragment-head shape gate: the operand must be an arena
  /// cell, must carry `+d.entry` (a head, never an op-cell attach — op
  /// wings are single-occupancy), and `pureRequired` arms need the
  /// descriptor's `pure` flag.
  [[nodiscard]] MetaMutation headShape(CellRef head, bool pureRequired,
                                       std::string_view role) const;
  /// `graft_*`'s head gate: `headShape` plus M14's consumption test and
  /// `validate` — the run-reachability arm belongs here, not in
  /// `learnGate`, because a grafted head runs in-session while a
  /// learned one's unresolved `run` records are §6.6's
  /// fail-until-resume class.
  [[nodiscard]] MetaMutation graftable(CellRef head, bool pureRequired,
                                       std::string_view role) const;
  /// `learn_*`'s head gate: `headShape`, the M14 consumption test
  /// (`learn` of an installed head refuses exactly like a second
  /// `graft_*`), and `learnable` — lineage plus closedness
  /// re-verified at accept (§6.6). Deliberately no `validate`: a
  /// `run`-role name-record unresolved in-session is deferred work
  /// the durable arm admits, never the structural invalidity
  /// `validate` judges.
  [[nodiscard]] MetaMutation learnGate(CellRef head, bool pureRequired,
                                       std::string_view role) const;
  /// The goal/event's canonical clause formals: the first clause whose
  /// `+d.grab` attach carries a descriptor — a plan's clauses share one
  /// formal list. `descriptor` false when nothing comparable exists.
  [[nodiscard]] MetaSignature clauseFormals(CellRef host) const;
  /// A replace-arm signature check: the candidate's declared params
  /// and result must match the clause's current attach on @p dim when
  /// that attach carries a descriptor (the clause's standard formals);
  /// a descriptor-less attach leaves the check unverifiable and the
  /// candidate's own declaration stands.
  [[nodiscard]] MetaMutation matchAttach(CellRef clause, DimRef dim,
                                         CellRef head,
                                         std::string_view role) const;
  /// Whether @p head is already installed — M14's occupancy test
  /// verbatim: any of the `-d.grab`/`-d.spin`/`-d.values` install-dim
  /// backlinks is occupied. The `+d.values` arm is the `graft_method`
  /// symbol convention (and a `test_of` memo record's `+d.values →
  /// wrapper` edge marks the wrapper installed — correct, since a
  /// reflect wrapper is already wired in). A `code"…"` literal's head
  /// keeps no backlink: emission anchors it through an int64 ref-box,
  /// never a direct `+d.values` edge, so a fresh literal stays
  /// graftable. Replacement (`learn_method`'s unlink-first) does not
  /// free the consumed head: the dead symbol's stale `+d.values` edge
  /// keeps the backlink occupied, which is exactly "install is
  /// occupancy, not destruction".
  [[nodiscard]] bool installed(CellRef head) const;
  /// @p head on the agent's `+d.code` rank?
  [[nodiscard]] bool codeRegistered(CellRef head) const;
  /// The install candidate: reify first when @p head is `d.code`-
  /// registered (a registered head is never mutated in place).
  [[nodiscard]] std::expected<CellRef, std::string> installable(CellRef head);
  /// `learn_*`'s two reifications — the `d.code`-registered argument
  /// reuse carve-out installs a single fresh copy.
  [[nodiscard]] std::expected<MetaLearnCopies, std::string>
  learnCopies(CellRef head);
  /// The durable-verb preconditions: program lineage, then per-head
  /// `closed` re-verification.
  [[nodiscard]] MetaMutation learnable(CellRef head) const;
  /// `true` while a bound lineage record names an image-bound `program()`.
  [[nodiscard]] bool hasProgramLineage() const;
  /// An image-side ref — inside `imageExtent` (dense id for ephemeral
  /// refs, the persistent spool position otherwise).
  [[nodiscard]] bool imageRef(CellRef cell) const;
  /// The program-side cells: `imageRef` plus everything `program()`
  /// reaches outbound over the structural-dimension vocabulary.
  [[nodiscard]] std::set<CellRef> programCells() const;
  /// Unlink @p cell off a same-dim rank with predecessor→successor
  /// repair (the `prune`/`unlearn` unlink idiom).
  void unlinkRank(CellRef cell, DimRef dim) const;
  /// Append a `+d.name → "patch:<n>"` record on the agent's `d.patch`
  /// rank: descriptor `{target, verb, parent?}`, `+d.patch-head` chain
  /// of name records, `d.vars` `{ordinal, alias?, priority?}`. The
  /// caller draws the serial so a clause-installing patch shares one
  /// `patch:<n>` label with its clause.
  [[nodiscard]] std::expected<CellRef, std::string>
  mintPatch(std::string_view label, std::int64_t ordinal, std::string_view verb,
            std::string_view target, std::optional<std::string_view> parent,
            std::optional<std::string_view> alias,
            std::optional<std::int64_t> priority,
            const std::vector<std::string> &headNames);
  /// The patch record's `d.vars`/`+d.values` field texts, and the
  /// `+d.patch-head` chain's recorded head names in order.
  [[nodiscard]] std::optional<std::string>
  patchField(CellRef patch, std::string_view key, bool descriptor) const;
  [[nodiscard]] std::vector<std::string> patchHeadNames(CellRef patch) const;
  /// The registered head a prior patch on @p clauseName left attached on
  /// @p attachDim (`d.grab`/`d.spin`), for `learn_where`/`learn_body`'s
  /// prior-head recording.
  [[nodiscard]] std::optional<std::string>
  priorAttachName(std::string_view clauseName, DimRef attachDim) const;
  /// Undo one patch: clause arms unlink, replace arms re-point through
  /// a reified prior or detach, then the patch drops and un-named
  /// `d.code` registrations follow.
  [[nodiscard]] MetaMutation undoPatch(CellRef patch);
  /// `name` reserved for apparatus labels (`patch:<n>` is minted, never
  /// an argument).
  [[nodiscard]] static bool reservedName(std::string_view name) noexcept {
    return name.starts_with("patch:");
  }

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
  /// Tombstone @p label: mint the `unlearn:<label>` record the genesis
  /// tombstone arm and the name surfaces consult. Promote never commits a
  /// removal, so an unlearned patch/clause/symbol physically resurrects on
  /// the next fold — this record, not the unlink, is the durable retirement.
  void markUnlearned(std::string_view label) const;
  /// Whether @p label carries an `unlearn:` tombstone.
  [[nodiscard]] bool unlearned(std::string_view label) const;
  /// The `test_of`/`body_of` shared wrap+memo walk.
  [[nodiscard]] std::optional<CellRef> wrap(CellRef clause, DimRef attachDim,
                                            std::string_view memoKind);

  VortexCore &core_;
  VortexVM &vm_;
  MetaBindSet binds_;
};

// ---------------------------------------------------------------------------
// P16 — the checkpoint ceremony's construction seam
// ---------------------------------------------------------------------------
//
// `vagent/p16.hpp` pins the hook contract the mesh's `pumpOne` drives
// (flag → re-kind → publish → restore, plus the every-exit-path drain);
// p16.cpp holds the concrete object this factory hands back. The adapter
// installs one per actor (`MeshActorRuntime::setP16Ceremony`) and drives
// the three hooks from its `Executor` callback frame.

/// The publish command `onFlaggedTurnEnd` issues synchronously from the
/// flagged turn's end-of-callback window — the mesh binds its own
/// `publishValue` (lineage `Store`, parent id, state root, keys, origin)
/// at install. The command runs while the marked ref cells are
/// `OpHandle`-kind, so the checkpoint's handles name promoted positions.
using P16Publish = std::function<std::expected<void, std::string>()>;

/// Construct the session-side ceremony object over one bound set.
///
/// The marked inventory mirrors genesis (e)'s sweep exactly: the
/// `+d.refs` ref-record chains (records carrying no `+d.name`) of every
/// `+d.code`-registered head on `binds.agentCell`, plus
/// `installedCopies` — the session-side half of the set (e) draws from
/// its replay — empty sweeps the `+d.code` rank alone. The object needs
/// no more: `name`/`step` come from `core.dims()`, `code`/`refs`/`ref`
/// from `binds.dims`.
///
/// `publish` must marshal the arena reachable state while the marked
/// cells are `OpHandle`-kind — `onFlaggedTurnEnd` issues it *before* its
/// `Int64(*handleTarget)` restore, so a publish that runs after the
/// restore would ship unrestored positions.
[[nodiscard]] std::unique_ptr<P16Ceremony>
makeP16Ceremony(zigzag::ArenaManifold &arena, VortexCore &core,
                const MetaBindSet &binds, P16Publish publish,
                std::vector<CellRef> installedCopies = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_META_HPP
