/**
 * @file compiler.cpp
 * @brief vagent compiler, iteration 5 (see compiler.hpp for the machine it
 *        targets and why).
 *
 * Plan suspension and resume encoding (§6.5, §6.6, §9.1):
 *  - Every plan body is compiled with a persistent activation frame
 *    (`PlanInfo::plan` with `cyclic = true`) stored on the intention cell's
 *    `frame` field (`kIntentionFrame`). All parameters, `let` bindings, and
 *    temporaries inside a plan are frame-backed handles (`frameH_`), so they
 *    survive across suspension turns without copying registers.
 *  - An intention's `step` field (`kIntentionStep`, 0-indexed `Int`) records
 *    where `continue(i)` resumes:
 *      - `step == 0` starts the selected plan body at `stepEntries[0]`.
 *      - The `k`-th lexical suspension point (`achieve`, effect call, `yield`,
 *        `await`, `ask`) in a plan body is assigned resume ordinal `k + 1` and
 *        a resume label `stepEntries[k + 1]`. Before yielding the turn
 * (`Halt`), the suspension site sets `intention.step = k + 1`, pops the plan
 * frame off `d.stack` (leaving it attached to `intention.frame`), runs
 *        `rtSettle_`, and jumps to `haltOp_`.
 *      - On resume, `continueEntry` (`emitContinue`) reads `intention.plan` and
 *        `intention.step`, re-adopts `intention.frame` onto `d.stack`, rebinds
 *        the plan's frame handles (`bindEntry`), and dispatches to
 *        `stepEntries[step]`. An out-of-range `step` fails the intention with
 *        `cause = "bad_step"`.
 *
 * Deviations from the design, each retired by a later iteration or by an
 * approved core change:
 *  - The dispatcher is emitted as opcodes per event type here, not written in
 *    a `std:agent` prelude (§10). The program-side ranks of §5.7 are built
 *    anyway so tools can walk them.
 *  - A frame is a cell the compiled code mints itself and keeps the top of in
 *    a handle (`frameH_`), chained on `d.stack` like `#Call`'s, not the cell
 *    `#Call` pushes: compiled code has no way to name the cursor, so it cannot
 *    read that frame. Arguments and locals hang on it as §5.8 says, and every
 *    write that moves the handle is trailed, so `backtrack` unwinds a failed
 *    call by itself.
 *  - Scalar agent fields are read and written through their static value
 *    cell; record and list fields are looked up through their name cell on
 *    every access, so they may be replaced by relinking.
 */
#include "common/xanadu/vagent/runtime/compiler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/prelude.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

std::string_view compileErrorName(CompileError code) noexcept {
  switch (code) {
  case CompileError::Unsupported:
    return "Unsupported";
  case CompileError::UnknownAgent:
    return "UnknownAgent";
  case CompileError::UnknownName:
    return "UnknownName";
  case CompileError::UnknownType:
    return "UnknownType";
  case CompileError::DuplicateName:
    return "DuplicateName";
  case CompileError::BadArity:
    return "BadArity";
  case CompileError::TypeMismatch:
    return "TypeMismatch";
  }
  return "Unknown";
}

namespace {

struct Bail {
  CompileFailure failure;
};

[[noreturn]] void bail(CompileError code, std::string detail,
                       SourceLocation loc = {}) {
  throw Bail{CompileFailure{code, std::move(detail), loc}};
}

[[noreturn]] void unsupported(std::string_view what, SourceLocation loc) {
  bail(CompileError::Unsupported,
       std::string(what) + " is not supported in this iteration", loc);
}

// -- Static types -------------------------------------------------------------

struct Ty {
  enum class K : std::uint8_t {
    Int,
    Float,
    Bool,
    Str,
    Unit,
    Record,
    List,
    Opt ///< laid out as a list of at most one element (§5.9)
  } k{K::Unit};
  std::string name;     ///< Record: the declared type
  std::vector<Ty> args; ///< List, Opt: the element type
  [[nodiscard]] bool composite() const {
    return k == K::Record || k == K::List || k == K::Opt;
  }
};

Ty tyOf(Ty::K k) {
  Ty t;
  t.k = k;
  return t;
}

// Dimension-independent protocol constants
// -------------------------------------

constexpr std::int64_t kPos                  = 1;
constexpr std::int64_t kNeg                  = -1;
constexpr std::string_view kEventFieldOfTurn = kTurnEvent;

/// An operand: a static cell, or (`pointer`) the cell whose ref is held in the
/// register `cell`.
struct Opd {
  CellRef cell;
  bool pointer{false};
  /// The handle itself is meant, so a ref local's record is wanted as a cell
  /// and not as a value.
  bool raw{false};
};
Opd S(CellRef c) { return Opd{c, false, false}; }
Opd D(CellRef p) { return Opd{p, true, false}; }
Opd Hd(CellRef c) { return Opd{c, false, true}; }

struct Local {
  CellRef reg{};
  Ty ty;
};

struct FieldInfo {
  CellRef nameCell{};
  CellRef valueCell{};
  Ty ty;
};

/// The slots a routine's activation frame holds (§5.8). Locals are a name cell
/// on the frame's `d.vars` with the value on `+d.values`; a temporary that has
/// to survive a re-entrant call is one cell on the frame's `d.step` rank. Each
/// slot is reached through a static handle, rebound whenever the frame it
/// names changes.
struct FramePlan {
  struct LocalSlot {
    CellRef handle{};
    std::string name;
  };
  std::vector<LocalSlot> locals;
  std::vector<CellRef> temps;
  /// A routine that can re-enter itself keeps its temporaries on the frame.
  bool cyclic{false};
  /// Temporaries created before the last re-entrant call site: the only ones
  /// that can be live across it, so the only ones the push allocates.
  std::size_t pushTemps{0};
  [[nodiscard]] bool empty() const { return locals.empty() && pushTemps == 0; }
};

struct Container;

struct MethodInfo {
  /// The skill whose method this is; null for the agent's own.
  Container *owner{nullptr};
  const MethodDef *def{nullptr};
  FramePlan plan;
  std::vector<CellRef> paramHandles;
  CellRef entry{};
  CellRef bodyStart{};
  CellRef pro{};
  CellRef epi{};
  CellRef retReg{};
  std::vector<CellRef> argRegs;
  std::vector<Ty> paramTys;
  Ty ret;
  bool cyclic{false};
};

struct HandlerInfo {
  /// The behavior whose handler this is; null for the agent's own.
  Container *owner{nullptr};
  const HandlerDef *def{nullptr};
  std::string eventType;
  std::optional<std::string> binder;
  std::int64_t priority{0};
  std::size_t index{0};
  /// Absent for a handler with no guard.
  std::optional<CellRef> guardEntry;
  CellRef bodyEntry{};
  /// The bind, guard and body routines share one frame (the belief variables
  /// the first declares are read by the others), pushed by the dispatcher.
  FramePlan plan;
  /// Absent for a handler with no frame to push.
  std::optional<CellRef> pushEntry;
  /// Belief triggers (`on +t` / `on -t`): the trigger term, the routine that
  /// matches it against the pending event's fact, and the locals its `?`
  /// variables declare for the guard and body.
  const Term *beliefTerm{nullptr};
  /// Absent for a handler on an ordinary event.
  std::optional<CellRef> bindEntry;
  std::vector<std::pair<std::string, Local>> beliefLocals;
};

struct GoalInfo {
  const GoalDecl *def{nullptr};
  Container *owner{nullptr};
  std::string name;
  GoalDecl::Kind kind{GoalDecl::Kind::Achieve};
  std::vector<std::pair<std::string, Ty>> params;
  Ty result{tyOf(Ty::K::Unit)};
  FramePlan condPlan;
  std::vector<CellRef> condParamHandles;
  CellRef condPushEntry{};
  CellRef succeedEntry{};
  CellRef dropEntry{};
  CellRef holdsEntry{};
};

struct PlanInfo {
  const PlanDecl *def{nullptr};
  Container *owner{nullptr};
  std::string qualName;
  std::string goalName;
  FramePlan plan;
  std::vector<std::pair<std::string, Local>> paramLocals;
  CellRef makeEntry{};
  CellRef bindEntry{};
  CellRef whereEntry{};
  std::vector<CellRef> stepEntries;
  CellRef epi{};
};

struct QuestionInfo {
  const QuestionDecl *def{nullptr};
  std::string name;
  std::vector<std::pair<std::string, Ty>> params;
  Ty result{tyOf(Ty::K::Unit)};
};

struct AnswerInfo {
  const AnswerDef *def{nullptr};
  Container *owner{nullptr};
  std::string question;
  FramePlan plan;
  std::vector<std::pair<std::string, Local>> paramLocals;
  CellRef pushEntry{};
  CellRef guardEntry{};
  CellRef bodyEntry{};
};

/// A skill or behavior reachable from the agent (§5.3, §5.7). Its cells are
/// minted at compile time like the agent's own, because no opcode addresses a
/// cell minted at run time (compiler.hpp); `install` and `mount` link them
/// into the agent's `d.skill` / `d.behavior` rank and `uninstall` / `unmount`
/// unlink them, so membership of that rank is the only installed-state there
/// is.
struct Container {
  enum class K : std::uint8_t { Skill, Behavior } k{K::Skill};
  std::string name;
  const std::vector<Param> *params{nullptr};
  const std::vector<Member> *members{nullptr};
  SourceLocation loc{};
  CellRef cell{}; ///< skill frame / behavior instance, holds the fields
  std::optional<CellRef> lastField;
  std::unordered_map<std::string, FieldInfo> fields;
  std::map<std::string, MethodInfo> methods;
  std::vector<std::string> uses;
  /// Skill: every capacity it implements, extended ones included.
  std::vector<std::string> capacities;
  std::vector<CellRef> instances; ///< one per capacity, on d.skill when live
  /// The cell whose rank membership means "installed": the first instance, or
  /// the frame itself for a skill that implements no capacity.
  CellRef member{};
  CellRef prepLabel{};      ///< uninstalls what shares a capacity
  CellRef installLabel{};   ///< install / mount body
  CellRef uninstallLabel{}; ///< uninstall / unmount body
  CellRef upgradeLabel{};   ///< upgrade body
  bool native{false};
};

// -- Child walker (call graph) ------------------------------------------------

template <typename F> void forEachChild(const Expr &e, F &&f) {
  const auto opt = [&](const ExprPtr &p) {
    if (p) f(*p);
  };
  std::visit(
      [&](const auto &n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, EUnary>) {
          opt(n.operand);
        } else if constexpr (std::is_same_v<T, EBinary>) {
          opt(n.left);
          opt(n.right);
        } else if constexpr (std::is_same_v<T, ERange>) {
          opt(n.lo);
          opt(n.hi);
          opt(n.by);
        } else if constexpr (std::is_same_v<T, ECall>) {
          opt(n.callee);
          for (const auto &a : n.args) opt(a);
        } else if constexpr (std::is_same_v<T, EIndex>) {
          opt(n.base);
          opt(n.index);
        } else if constexpr (std::is_same_v<T, EField>) {
          opt(n.base);
        } else if constexpr (std::is_same_v<T, EList>) {
          for (const auto &a : n.elems) opt(a);
        } else if constexpr (std::is_same_v<T, ERecord>) {
          for (const auto &a : n.fields) opt(a.value);
        } else if constexpr (std::is_same_v<T, EBlock>) {
          for (const auto &s : n.stmts) opt(s.value);
        } else if constexpr (std::is_same_v<T, EIf>) {
          opt(n.cond);
          opt(n.thenBranch);
          opt(n.elseBranch);
        } else if constexpr (std::is_same_v<T, EWhile>) {
          opt(n.cond);
          opt(n.body);
        } else if constexpr (std::is_same_v<T, EFor>) {
          opt(n.iterable);
          if (n.believesTerm) {
            for (const auto &a : n.believesTerm->args) opt(a.expr);
          }
          opt(n.body);
        } else if constexpr (std::is_same_v<T, EEmit>) {
          opt(n.event);
          opt(n.space);
          opt(n.recipients);
        } else if constexpr (std::is_same_v<T, ESpawn>) {
          for (const auto &a : n.args) opt(a);
          opt(n.space);
        } else if constexpr (std::is_same_v<T, EReturn>) {
          opt(n.value);
        } else if constexpr (std::is_same_v<T, EBelieves>) {
          for (const auto &a : n.term.args) opt(a.expr);
          opt(n.from);
        } else if constexpr (std::is_same_v<T, EFindall>) {
          opt(n.templ.expr);
          for (const auto &a : n.goal.args) opt(a.expr);
        } else if constexpr (std::is_same_v<T, EBeliefChange>) {
          for (const auto &a : n.fact.args) opt(a.expr);
        } else if constexpr (std::is_same_v<T, EInstall>) {
          for (const auto &a : n.args) opt(a);
        } else if constexpr (std::is_same_v<T, ETell>) {
          opt(n.to);
          for (const auto &a : n.fact.args) opt(a.expr);
        } else if constexpr (std::is_same_v<T, EStop>) {
          opt(n.target);
        } else if constexpr (std::is_same_v<T, EAdopt>) {
          for (const auto &a : n.goal.args) opt(a);
        } else if constexpr (std::is_same_v<T, EAchieve>) {
          for (const auto &a : n.goal.args) opt(a);
          opt(n.by);
          opt(n.within);
          opt(n.orElse);
        } else if constexpr (std::is_same_v<T, EAsk>) {
          opt(n.question);
          opt(n.within);
          opt(n.orElse);
        } else if constexpr (std::is_same_v<T, EAwait>) {
          if (n.term) {
            for (const auto &a : n.term->args) opt(a.expr);
          }
          opt(n.guard);
          opt(n.within);
          opt(n.orElse);
        } else if constexpr (std::is_same_v<T, EDrop>) {
          opt(n.target);
        }
      },
      e.node);
}

void collectCallees(const Expr &e, std::set<std::string> &out) {
  if (const auto *c = std::get_if<ECall>(&e.node)) {
    if (const auto *nm = std::get_if<EName>(&c->callee->node)) {
      out.insert(nm->name);
    }
  }
  forEachChild(e, [&](const Expr &c) { collectCallees(c, out); });
}

// -- The generator -----------------------------------------------------------

class Gen {
public:
  Gen(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm,
      const Unit &unit, std::string_view agentType,
      std::size_t beliefEventDepth   = 8,
      std::uint64_t conditionFuel    = kDefaultConditionFuel,
      std::int64_t maintainBackoffMs = kDefaultMaintainBackoffMs)
      : core_(core), vm_(vm), arena_(core.arena()), unit_(unit),
        agentType_(agentType), conditionFuel_(conditionFuel),
        maintainBackoffMs_(maintainBackoffMs),
        beliefEventDepth_(beliefEventDepth) {}

  CompiledAgent run();
  std::map<std::string, FrameBody, std::less<>> takeFrameBodies() {
    return std::move(frameBodies_);
  }

private:
  // -- primitives ------------------------------------------------------------
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  zigzag::ArenaManifold &arena_;
  const Unit &unit_;
  std::string agentType_;
  AgentDims dims_{};

  /// The last op emitted on the current path; empty after a jump, so the
  /// next op starts a new straight-line run instead of falling through.
  std::optional<CellRef> tail_;

  std::map<std::int64_t, CellRef> intConsts_;
  std::map<std::string, CellRef> strConsts_;
  std::map<CellRef, CellRef> oprefs_;
  // Minted by run() once collect() has validated the unit, so a refused unit
  // mints nothing; nothing reads them before then, and none ever means "no
  // cell". Absence elsewhere in this generator is std::optional.
  CellRef zero_{}, one_{}, negOne_{}, unit_c_{}, true_{}, false_{};
  CellRef a0_{}, a1_{}, a2_{}, r0_{}, r1_{};
  CellRef curEvent_{}, scratchG_{}, refTmp_{};
  CellRef failStep_{};
  CellRef haltOp_{}, failOp_{};
  CellRef rtFind_{}, rtAppendField_{}, rtListPush_{}, rtOutbox_{}, rtCopy_{};
  FramePlan copyPlan_, equalPlan_;
  CellRef copyPro_{}, copyBody_{};
  // Emitted only for a unit that calls them: the first call site mints the
  // label and queues the emitter, which run() drains after the main pass.
  using Emitter = void (Gen::*)();
  std::vector<Emitter> pendingRoutines_;
  std::optional<CellRef> rtSetField_, rtClearField_, rtEqual_, rtRaiseHostEv_,
      rtStrLen_;
  CellRef routineLabel(std::optional<CellRef> &label, Emitter emit) {
    if (!label) {
      label = newLabel();
      pendingRoutines_.push_back(emit);
    }
    return *label;
  }
  /// The intention runtime (find, end, cancel a wait, reconsider, match
  /// awaits) is not lowered by this compiler; a unit that needs it is refused
  /// here instead of calling a routine that does not exist.
  /// A plan's failure label; reached only from plan code, which this
  /// compiler refuses until plan lowering exists.
  CellRef planLabel(const std::optional<CellRef> &label) {
    if (!label) bail(CompileError::Unsupported, "plan lowering");
    return *label;
  }
  [[noreturn]] void intentionRuntime(std::string_view routine) {
    bail(CompileError::Unsupported,
         "the intention runtime routine " + std::string(routine));
  }

  // -- activation frames (§5.8) ----------------------------------------------
  /// What a handle cell stands for. A `Temp` and a `Local` read and write like
  /// a register; a `RefLocal` is a composite local: its handle names the
  /// record, which hangs on the frame's name cell by `+d.values` so promotion
  /// remaps it, and reading it as a value yields the record's ref.
  enum class Backing : std::uint8_t { Temp, Local, RefLocal };
  std::unordered_map<CellRef, Backing> backed_;
  /// Cells that are agent state: the agent cell, its fields, predicates, and
  /// the cells of skills and behaviors. Compiled code names them through a
  /// handle, so a promotion that carries program and state together would
  /// remap every one of those refs instead of leaving integers that name
  /// whatever the old number names in the new arena.
  std::unordered_set<CellRef> stateCells_;
  std::unordered_map<CellRef, CellRef> stateHandles_;
  FramePlan *frame_{nullptr};
  CellRef frameH_{}; ///< the handle naming the top frame
  CellRef walkH_{};  ///< a walker along a frame's name cells
  static constexpr std::size_t kPoolSize    = 4;
  static constexpr std::size_t kScratchSize = 3;
  /// Handles an operand "pointer" (`D`) is bound to just before its use.
  std::array<CellRef, kPoolSize> pool_{};
  /// Where an op's output goes when the cell it was asked to fill is a handle
  /// (outputs never follow handles, so the value is moved afterwards).
  std::array<CellRef, kScratchSize> scratch_{};
  /// Compiled bodies a host can run against a frame it names (iteration 4c).
  std::map<std::string, FrameBody, std::less<>> frameBodies_;

  CellRef agentCell_{};
  std::unordered_map<std::string, FieldInfo> fields_;
  std::optional<CellRef> lastFieldName_;

  std::unordered_map<std::string, const EventDecl *> events_;
  std::unordered_map<std::string, const RecordDecl *> records_;
  std::unordered_map<std::string, const AgentDecl *> agents_;
  const AgentDecl *agent_{nullptr};

  std::map<std::string, MethodInfo> methods_;
  std::vector<HandlerInfo> handlers_;
  std::map<std::string, GoalInfo> goals_;
  std::vector<PlanInfo> plans_;
  std::map<std::string, QuestionInfo> questions_;
  std::vector<AnswerInfo> answers_;

  // -- skills, behaviors and capacities (§5.3, §5.7); see the section below
  std::map<std::string, Container> skills_;
  std::map<std::string, Container> behaviors_;
  std::unordered_map<std::string, const CapacityDecl *> capacities_;
  std::vector<std::string> agentUses_;
  /// The skill or behavior whose code is being emitted; null for the agent's.
  Container *own_{nullptr};
  CellRef rtCapFind_{};

  // -- goals, plans, conditions, and scanning (§6.2–§6.6, §10) ----------------
  std::uint64_t conditionFuel_{kDefaultConditionFuel};
  std::int64_t maintainBackoffMs_{kDefaultMaintainBackoffMs};
  CellRef curIntention_{};
  // Condition and plan state, minted by run() holding 0: no lowering this
  // compiler implements raises the flags, so conditions and plan contracts
  // stay dormant rather than reading a cell that was never minted.
  CellRef inCondG_{}, condFuelG_{}, condContractD_{}, condD_{};
  CellRef inPlanG_{}, planContractDG_{};
  CellRef scanSubjG_{}, scanPosG_{}, scanLenG_{};

  // Per-routine emission state.
  std::vector<std::unordered_map<std::string, Local>> scopes_;
  std::vector<CellRef> dRegs_;
  std::size_t stmtDepth_{0};
  struct Ctx {
    CellRef retReg{};
    CellRef d0{};
    CellRef epi{};
    std::optional<Ty> eventTy;
    std::optional<std::string> binder;
    bool inHandler{false};
    bool inPlan{false};
    PlanInfo *planInfo{nullptr};
    /// Set only by plan lowering, which this compiler does not implement.
    std::optional<CellRef> planFail;
    std::optional<CellRef> planContractFail;
  } ctx_;

  // -- cell and op construction
  // ------------------------------------------------
  CellRef mkOp(OpcodeKind k, std::string_view label = {}) {
    return vm_.mintOpcode(k, label);
  }
  /// The cached cell for @p key, minted by @p mint on first use.
  template <typename Map, typename Key, typename Mint>
  static CellRef cached(Map &cache, const Key &key, Mint &&mint) {
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    return cache.emplace(key, mint()).first->second;
  }
  CellRef cInt(std::int64_t v) {
    return cached(intConsts_, v, [&] { return arena_.makeScalarCell(v); });
  }
  CellRef cStr(const std::string &s) {
    return cached(strConsts_, s, [&] { return arena_.makeCell(s); });
  }
  CellRef cFloat(double v) { return arena_.makeScalarCell(v); }
  CellRef cBool(bool v) { return v ? true_ : false_; }
  CellRef opref(CellRef target) {
    return cached(oprefs_, target, [&] {
      return arena_.makeScalarCell(static_cast<std::int64_t>(target));
    });
  }
  /// A static cell. In a routine that can re-enter itself it is a frame
  /// temporary instead, so a recursive activation cannot overwrite it.
  CellRef reg() {
    if (frame_ != nullptr && frame_->cyclic) {
      const CellRef h = mintHandle();
      frame_->temps.push_back(h);
      backed_.emplace(h, Backing::Temp);
      return h;
    }
    return arena_.makeCell();
  }
  CellRef global() { return arena_.makeCell(); }
  /// A pipeline's parameter cell: written by a `Link` and read at once, so it
  /// never lives across a call.
  CellRef transient() { return arena_.makeCell(); }

  /// A handle, marked for relocation so promotion remaps its target, naming
  /// @p pointedAt, or a private cell until it is first pointed.
  CellRef mintHandle(std::optional<CellRef> pointedAt = std::nullopt) {
    const CellRef h      = arena_.makeCell();
    const CellRef target = pointedAt ? *pointedAt : arena_.makeCell();
    zigzag::expectWritten(
        arena_.setValueBits(h, xanadu::ValueKind::OpHandle, target));
    const CellRef marker = arena_.makeCell(xanadu::cellRelocationMarker);
    zigzag::expectWritten(
        arena_.link(h, arena_.ensureDimension(xanadu::cellRelocationDimension),
                    DimVector::POS, marker));
    return h;
  }
  CellRef stateHandle(CellRef cell) {
    return cached(stateHandles_, cell, [&] { return mintHandle(cell); });
  }
  /// Put the ref of state cell @p cell in register @p dst.
  void loadRef(CellRef dst, CellRef cell) {
    emit(OpcodeKind::Deref, "sref", {S(cell)}, {dst});
  }
  [[nodiscard]] std::optional<Backing> backingOf(CellRef c) const {
    const auto it = backed_.find(c);
    return it == backed_.end() ? std::nullopt : std::optional{it->second};
  }
  [[nodiscard]] bool isRefLocal(CellRef c) const {
    return backingOf(c) == Backing::RefLocal;
  }

  void spin(CellRef from, CellRef to) {
    zigzag::expectWritten(
        arena_.link(from, core_.dims().spin, DimVector::POS, to));
  }
  void append(CellRef op) {
    if (tail_) spin(*tail_, op);
    tail_ = op;
  }
  CellRef newLabel() { return mkOp(OpcodeKind::Nop, "L"); }
  void place(CellRef label) { append(label); }
  void kill() { tail_.reset(); }

  struct Poke {
    CellRef handle; ///< the pool handle the operation reads through
    CellRef ptr;    ///< the register holding the ref it is to name
  };

  /// A pointer operand names the pool handle for its position; the handle is
  /// pointed at the register's cell by `flushPokes`, one cycle, without
  /// editing the program graph.
  CellRef addSlot(CellRef op, const Opd &o, bool input, std::vector<Poke> *pk) {
    CellRef target = o.cell;
    if (!o.pointer && stateCells_.contains(o.cell)) {
      target = stateHandle(o.cell);
    } else if (o.pointer) {
      if (isRefLocal(o.cell)) {
        bail(CompileError::Unsupported,
             "internal: a ref local used as a pointer");
      }
      target = pool_.at(pk != nullptr ? pk->size() : 0);
      if (pk != nullptr) pk->push_back({target, o.cell});
    } else if (input && !o.raw && isRefLocal(o.cell)) {
      bail(CompileError::Unsupported, "internal: a ref local read as a value");
    }
    const CellRef slot =
        arena_.makeScalarCell(static_cast<std::int64_t>(target));
    const DimVector side = input ? DimVector::POS : DimVector::NEG;
    const auto first     = arena_.linked(op, core_.dims().grab, side);
    if (!first) {
      zigzag::expectWritten(arena_.link(op, core_.dims().grab, side, slot));
    } else {
      zigzag::expectWritten(
          arena_.link(zigzag::rankTail(arena_, *first, core_.dims().step),
                      core_.dims().step, DimVector::POS, slot));
    }
    return slot;
  }

  /// `#Point out, ref` where @p ref is a register holding a ref.
  void pointOp(CellRef out, CellRef ref) {
    const CellRef op = mkOp(OpcodeKind::Point, "pt");
    std::vector<Poke> none;
    addSlot(op, S(ref), true, &none);
    core_.bindOutput(op, out);
    append(op);
  }

  void flushPokes(const std::vector<Poke> &pk) {
    for (const Poke &p : pk) pointOp(p.handle, p.ptr);
  }

  /// Point @p out at the neighbour of @p owner along @p dim: the `Link` read
  /// is the pipeline of the `Point`, so it costs one cycle.
  void pointAtLink(CellRef out, Opd owner, DimRef dim, bool negward = false) {
    std::vector<Poke> pk;
    const CellRef lk = mkOp(OpcodeKind::Link, "pl");
    const CellRef p  = transient();
    addSlot(lk, owner, true, &pk);
    addSlot(lk, S(cInt(dim)), true, &pk);
    addSlot(lk, S(cInt(negward ? kNeg : kPos)), true, &pk);
    flushPokes(pk);
    zigzag::expectWritten(
        arena_.link(p, core_.dims().spin, DimVector::POS, lk));
    pointOp(out, p);
  }

  CellRef emit(OpcodeKind k, std::string_view label,
               std::initializer_list<Opd> ins,
               std::initializer_list<CellRef> outs = {}) {
    const CellRef op = mkOp(k, label);
    std::vector<Poke> pk;
    for (const Opd &o : ins) addSlot(op, o, true, &pk);
    std::vector<std::pair<CellRef, CellRef>> after;
    for (const CellRef o : outs) {
      if (backed_.contains(o) || stateCells_.contains(o)) {
        const CellRef sc = scratch_.at(after.size());
        core_.bindOutput(op, sc);
        after.emplace_back(o, sc);
      } else {
        core_.bindOutput(op, o);
      }
    }
    flushPokes(pk);
    append(op);
    for (const auto &[handle, sc] : after) store(handle, sc);
    return op;
  }

  /// Write the value in @p src to a register that may be a handle.
  void store(CellRef dst, CellRef src) {
    if (isRefLocal(dst)) {
      assignRef(dst, src, false);
    } else {
      emit(OpcodeKind::Value, "mov", {S(dst), S(zero_), S(negOne_), S(src)});
    }
  }

  void mov(Opd dst, Opd src) {
    if (!dst.pointer && isRefLocal(dst.cell)) {
      if (src.pointer) {
        emit(OpcodeKind::Value, "mov", {S(refTmp_), S(zero_), S(negOne_), src});
        assignRef(dst.cell, refTmp_, false);
      } else {
        assignRef(dst.cell, src.cell, false);
      }
      return;
    }
    if (!src.pointer && isRefLocal(src.cell)) {
      if (!dst.pointer) {
        emit(OpcodeKind::Deref, "ref", {Hd(src.cell)}, {dst.cell});
      } else {
        emit(OpcodeKind::Deref, "ref", {Hd(src.cell)}, {refTmp_});
        emit(OpcodeKind::Value, "mov", {dst, S(zero_), S(negOne_), S(refTmp_)});
      }
      return;
    }
    emit(OpcodeKind::Value, "mov", {dst, S(zero_), S(negOne_), src});
  }
  void movC(CellRef dst, CellRef constant) { mov(S(dst), S(constant)); }
  void alloc(CellRef dst) {
    emit(OpcodeKind::Value, "alloc", {S(unit_c_), S(zero_), S(zero_)}, {dst});
  }
  void jump(CellRef target) {
    emit(OpcodeKind::Jump, "jmp", {S(opref(target))});
    kill();
  }
  void branch(Opd cond, CellRef target) {
    emit(OpcodeKind::Branch, "br", {cond, S(opref(target))});
  }
  void failNow() {
    emit(OpcodeKind::Fail, "fail", {});
    kill();
  }
  void callLabel(CellRef target) {
    emit(OpcodeKind::Call, "call", {S(opref(target))});
    emit(OpcodeKind::Nop, "ret", {});
  }
  void linkOp(Opd a, DimRef dim, Opd b) {
    emit(OpcodeKind::Link, "link", {a, S(cInt(dim)), S(one_), b});
  }

  /// Read the neighbour of `owner` along `dim` through a parameter pipeline.
  /// The result is a fresh register holding the neighbour's ref, or 0.
  CellRef rd(Opd owner, DimRef dim, bool negward = false) {
    const CellRef p  = transient();
    const CellRef lk = mkOp(OpcodeKind::Link, "rd");
    std::vector<Poke> pk;
    addSlot(lk, owner, true, &pk);
    addSlot(lk, S(cInt(dim)), true, &pk);
    addSlot(lk, S(cInt(negward ? kNeg : kPos)), true, &pk);
    flushPokes(pk);
    zigzag::expectWritten(
        arena_.link(p, core_.dims().spin, DimVector::POS, lk));
    // The copy runs the pipeline once and leaves a cell with none: `p` would
    // read the neighbour again at every later use, through a pool handle that
    // has been pointed elsewhere by then.
    const CellRef r = transient();
    emit(OpcodeKind::Value, "rd", {S(r), S(zero_), S(negOne_), S(p)});
    return r;
  }

  // -- activation frames (§5.8) ---------------------------------------------
  /// Store the ref held in @p src as the value of the ref local @p h: the
  /// record hangs on the frame's name cell by `+d.values`, which a promotion
  /// remaps, unlike a ref kept as an integer. A value cell has one `-d.values`
  /// neighbour, so a record some other name already owns is copied first.
  void assignRef(CellRef h, CellRef src, bool fresh) {
    if (src != refTmp_) {
      emit(OpcodeKind::Value, "mov",
           {S(refTmp_), S(zero_), S(negOne_), S(src)});
    }
    if (!fresh) {
      const CellRef owned = newLabel();
      branchIfZero(rd(D(refTmp_), core_.dims().values, true), owned);
      callCopy(refTmp_, refTmp_);
      place(owned);
    }
    const CellRef name = rd(Hd(h), core_.dims().values, true);
    linkOp(D(name), core_.dims().values, D(refTmp_));
    pointOp(h, refTmp_);
  }

  /// A local of the routine being emitted: a slot on its frame, reached
  /// through a handle. Outside a framed routine it is a plain register.
  CellRef declareLocal(const std::string &name, const Ty &ty) {
    if (frame_ == nullptr) return reg();
    const CellRef h = mintHandle();
    frame_->locals.push_back({h, name});
    backed_.emplace(h, ty.composite() ? Backing::RefLocal : Backing::Local);
    return h;
  }

  /// A routine that runs under a frame of its own and returns with `#Return`.
  struct Framed {
    FramePlan plan;
    CellRef pro{}, bodyStart{}, epi{};
  };
  /// The frame is built last, once the body has said how many slots it needs.
  void openFramed(Framed &f, CellRef entry) {
    f.pro       = newLabel();
    f.bodyStart = newLabel();
    f.epi       = newLabel();
    frame_      = &f.plan;
    place(entry);
    jump(f.pro);
    place(f.bodyStart);
  }
  void closeFramed(Framed &f) {
    place(f.epi);
    if (!f.plan.empty()) emitFramePop();
    ret();
    place(f.pro);
    if (!f.plan.empty()) emitFramePush(f.plan);
    jump(f.bodyStart);
    frame_ = nullptr;
  }

  /// Mint the frame and its slots and point every handle at them.
  void emitFramePush(const FramePlan &p) {
    const DimRef stack = core_.dims().stack;
    const DimRef vars  = core_.dims().vars;
    const DimRef vals  = core_.dims().values;
    const DimRef step  = core_.dims().step;
    emit(OpcodeKind::New, "fnew", {S(frameH_), S(cInt(stack)), S(negOne_)});
    pointAtLink(frameH_, S(frameH_), stack, true);
    for (std::size_t k = 0; k < p.pushTemps; ++k) {
      const CellRef owner = k == 0 ? frameH_ : p.temps[k - 1];
      emit(OpcodeKind::New, "tnew", {S(owner), S(cInt(step)), S(one_)});
      pointAtLink(p.temps[k], S(owner), step);
    }
    for (std::size_t i = 0; i < p.locals.size(); ++i) {
      const CellRef owner = i == 0 ? frameH_ : walkH_;
      emit(OpcodeKind::New, "vnew", {S(owner), S(cInt(vars)), S(one_)});
      pointAtLink(walkH_, S(owner), vars);
      if (!p.locals[i].name.empty()) {
        mov(S(walkH_), S(cStr(p.locals[i].name)));
      }
      emit(OpcodeKind::New, "wnew", {S(walkH_), S(cInt(vals)), S(one_)});
      pointAtLink(p.locals[i].handle, S(walkH_), vals);
    }
  }

  void emitFramePop() {
    pointAtLink(frameH_, S(frameH_), core_.dims().stack);
    emit(OpcodeKind::Break, "fbrk",
         {S(frameH_), S(cInt(core_.dims().stack)), S(negOne_)});
  }

  /// Point the handles of the first @p nLocals locals and @p nTemps
  /// temporaries at the frame `frameH_` names: after a call that may have
  /// re-entered this routine, and when a body is resumed on a frame.
  void emitFrameBind(const FramePlan &p, std::size_t nLocals,
                     std::size_t nTemps) {
    const DimRef vars = core_.dims().vars;
    const DimRef vals = core_.dims().values;
    const DimRef step = core_.dims().step;
    for (std::size_t i = 0; i < nLocals; ++i) {
      pointAtLink(walkH_, S(i == 0 ? frameH_ : walkH_), vars);
      pointAtLink(p.locals[i].handle, S(walkH_), vals);
    }
    for (std::size_t k = 0; k < nTemps; ++k) {
      pointAtLink(p.temps[k], S(k == 0 ? frameH_ : p.temps[k - 1]), step);
    }
  }

  /// A call to code that may run this routine again.
  void callUser(CellRef label) {
    callLabel(label);
    if (frame_ != nullptr && frame_->cyclic) {
      frame_->pushTemps = frame_->temps.size();
      emitFrameBind(*frame_, frame_->locals.size(), frame_->temps.size());
    }
  }

  CellRef boolReg() { return reg(); }

  /// `if reg == 0 goto label`
  void branchIfZero(CellRef r, CellRef label) {
    const CellRef t = reg();
    emit(OpcodeKind::Eq, "z", {S(r), S(zero_)}, {t});
    branch(S(t), label);
  }

  void emitAppendTail(Opd owner, DimRef firstDim, DimRef nextDim,
                      CellRef fresh) {
    const CellRef t         = reg();
    const CellRef x         = reg();
    const CellRef linkFirst = newLabel();
    const CellRef linkTail  = newLabel();
    const CellRef loop      = newLabel();
    const CellRef done      = newLabel();
    mov(S(t), S(rd(owner, firstDim)));
    branchIfZero(t, linkFirst);
    place(loop);
    mov(S(x), S(rd(D(t), nextDim)));
    branchIfZero(x, linkTail);
    mov(S(t), S(x));
    jump(loop);
    place(linkFirst);
    linkOp(owner, firstDim, D(fresh));
    jump(done);
    place(linkTail);
    linkOp(D(t), nextDim, D(fresh));
    place(done);
  }

  // -- shared runtime routines ------------------------------------------------
  void beginInternal(CellRef label) {
    tail_  = std::nullopt;
    frame_ = nullptr;
    place(label);
  }
  void ret() {
    emit(OpcodeKind::Return, "return", {});
    kill();
  }
  void emitRtFind();
  void emitRtAppendField();
  void emitRtSetField();
  void emitRtClearField();
  void emitRtListPush();
  void emitRtOutbox();
  void emitRtCopy();
  void emitRtEqual();
  void emitRtRaiseHostEv();
  void emitRtStrLen();
  void callFind(CellRef owner, const std::string &name, CellRef &vp,
                CellRef &np);
  /// Heads a freshly allocated record's `+d.vars` rank with `kRecordMarker`.
  void markRecord(CellRef record);
  void callAppendField(CellRef owner, const std::string &name,
                       std::optional<CellRef> existing, CellRef &vc);
  void callSetField(CellRef owner, const std::string &name,
                    std::optional<CellRef> existing, CellRef &vc);
  void callClearField(CellRef owner, const std::string &name);
  void putScalar(CellRef owner, std::string_view fname, Opd value);
  void callListPush(CellRef list, CellRef elem);
  CellRef callOutbox(std::string_view kind);
  void callCopy(CellRef src, CellRef dst);
  void callEqual(CellRef a, CellRef b, CellRef out);
  void callRaiseHostEv(CellRef ev);
  void callStrLen(CellRef s, CellRef out);
  void emitCondTick();
  void emitContractViolation();

  // -- types -----------------------------------------------------------------
  Ty fromType(const TypeExpr &t, SourceLocation loc);
  /// A prelude field's type (`Opt` for `source` and `space`).
  Ty preludeTy(const PreludeField &f);
  void appendImplicitFields(std::vector<std::pair<std::string, Ty>> &out);
  /// Reading an `Opt<T>` succeeds with its value or fails when it is `none`
  /// (§8.2): the value of the optional held in @p optReg goes to @p out.
  Ty unwrapOpt(CellRef optReg, const Ty &opt, CellRef out, SourceLocation loc);
  /// Wraps a `T` stored where `Opt<T>` is declared; @p v is replaced.
  void coerceInto(const Ty &target, const Ty &actual, CellRef v);
  /// The declared type of an assignment target, when it is known statically.
  std::optional<Ty> staticTy(const Expr &e);
  std::vector<std::pair<std::string, Ty>> fieldsOf(const std::string &type);
  std::optional<Ty> fieldTy(const std::string &type, const std::string &f);
  std::vector<std::string> supertypesOf(const std::string &type);

  // -- fields
  // ------------------------------------------------------------------
  CellRef initialValueCell(const Ty &t);
  void addField(const std::string &name, const Ty &ty);
  void storeField(const FieldInfo &f, CellRef valueReg);
  CellRef loadFieldInto(const FieldInfo &f, CellRef out);

  // -- scopes ----------------------------------------------------------------
  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  Local *findLocal(const std::string &n) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      if (auto f = it->find(n); f != it->end()) return &f->second;
    }
    return nullptr;
  }
  CellRef dRegAt(std::size_t depth) {
    while (dRegs_.size() <= depth) dRegs_.push_back(reg());
    return dRegs_[depth];
  }

  // -- statements and expressions ---------------------------------------------
  template <typename F> void stmtWrap(F &&body) {
    const std::size_t curDepth = stmtDepth_;
    const CellRef d            = dRegAt(curDepth);
    ++stmtDepth_;
    emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
    if (ctx_.inPlan && curDepth == 0) {
      emit(OpcodeKind::Choice, "pcontract",
           {S(opref(planLabel(ctx_.planContractFail)))});
      emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
      emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
    } else {
      emit(OpcodeKind::Choice, "sfail", {S(opref(failStep_))});
    }
    body();
    emit(OpcodeKind::Cut, "cut", {S(d)});
    --stmtDepth_;
  }
  template <typename F> void bounded(F &&body) {
    const CellRef d = dRegAt(stmtDepth_);
    ++stmtDepth_;
    emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
    body();
    emit(OpcodeKind::Cut, "cut", {S(d)});
    --stmtDepth_;
  }
  static bool isPlace(const Expr &e) {
    if (std::holds_alternative<EName>(e.node)) return true;
    if (std::holds_alternative<EField>(e.node)) return true;
    if (std::holds_alternative<EIndex>(e.node)) return true;
    if (const auto *s = std::get_if<ESpecial>(&e.node)) {
      return s->kind == ESpecial::Kind::Occurrence;
    }
    return false;
  }

  Ty expr(const Expr &e, CellRef out);
  Ty valueInto(const Expr &e, CellRef out);
  Ty literal(const ELiteral &l, CellRef out, SourceLocation loc);
  Ty name(const EName &n, CellRef out, SourceLocation loc);
  Ty special(const ESpecial &s, CellRef out, SourceLocation loc);
  Ty unary(const EUnary &u, CellRef out, SourceLocation loc);
  Ty binary(const EBinary &b, CellRef out, SourceLocation loc);
  Ty comparison(OpcodeKind k, const EBinary &b, CellRef out);
  Ty assign(const EBinary &b, CellRef out, SourceLocation loc);
  Ty range(const ERange &r, CellRef out);
  Ty call(const ECall &c, CellRef out, SourceLocation loc);
  /// Built-in capacities `Schedules`, `Clock`, `Logging`, `Accepts` (§10): a
  /// call on `Cap.method`. Nullopt when the callee is not one of them.
  std::optional<Ty> capacityCall(const ECall &c, CellRef out,
                                 SourceLocation loc);
  Ty index(const EIndex &i, CellRef out);
  Ty field(const EField &f, CellRef out, SourceLocation loc);
  Ty list(const EList &l, CellRef out);
  Ty record(const ERecord &r, CellRef out, SourceLocation loc);
  Ty block(const EBlock &b, CellRef out);
  Ty ifExpr(const EIf &i, CellRef out);
  Ty whileExpr(const EWhile &w, CellRef out);
  Ty forExpr(const EFor &f, CellRef out, SourceLocation loc);
  Ty emitExpr(const EEmit &e, CellRef out, SourceLocation loc);
  Ty spawnExpr(const ESpawn &s, CellRef out, SourceLocation loc);
  Ty concat(const EBinary &b, CellRef out);
  void rangeTest(CellRef cur, CellRef hi, Opd by, CellRef done);
  void listWalk(CellRef listReg, const std::function<void(CellRef elem)> &each);
  void storeInto(const Expr &target, CellRef valueReg, const Ty &valueTy,
                 SourceLocation loc);

  // -- goals, plans, intentions, suspension points (§6.2–§6.6, §10) -----------
  Ty adoptExpr(const EAdopt &a, CellRef out, SourceLocation loc);
  Ty achieveExpr(const EAchieve &a, CellRef out, SourceLocation loc);
  Ty askExpr(const EAsk &a, CellRef out, SourceLocation loc);
  Ty awaitExpr(const EAwait &a, CellRef out, SourceLocation loc);
  Ty dropExpr(const EDrop &d, CellRef out, SourceLocation loc);
  Ty yieldExpr(const EYield &y, CellRef out, SourceLocation loc);
  Ty effectCall(const std::string &skillName, const MethodSig &sig,
                const std::vector<ExprPtr> &args, CellRef out,
                SourceLocation loc);
  CellRef buildGoalArgs(const GoalInfo &gi, const std::vector<ExprPtr> &args,
                        SourceLocation loc);
  CellRef mintIntentionCell(const GoalInfo &gi, CellRef argsRec, bool setReady);
  void prepareGoalsPlansAnswers(const std::vector<std::string> &behaviorOrder);
  void emitGoalRoutines(GoalInfo &g);
  void emitPlanRoutines(PlanInfo &p);
  void emitAnswerRoutines(AnswerInfo &a);
  void emitIsolatedCond(CellRef condEntry, CellRef goalNameReg,
                        CellRef condKindReg, CellRef okOut);
  void emitRtFindIntention();
  void emitRtCancelWait();
  void emitRtEndIntention();
  void emitRtReconsider();
  void emitRtMatchAwaits();
  void emitContinue(CellRef entryLabel);
  void emitFailStep(CellRef entryLabel);
  void emitAnswer(CellRef entryLabel);
  struct AchieveRecvSeq {
    const GoalInfo *goal{nullptr};
    CellRef label{};
  };
  std::vector<AchieveRecvSeq> emitAchieveRecvTests(CellRef ty, CellRef eq);
  void emitAchieveRecvBodies(const std::vector<AchieveRecvSeq> &seqs,
                             CellRef end);

  // -- declarations
  // ------------------------------------------------------------
  void collect();
  void mintAgentCell();
  void prepareMethods();
  void emitMethod(MethodInfo &m);
  void emitHandlerRoutines(HandlerInfo &h);
  void emitHandlerRun(const HandlerInfo &h, bool checkConsumed, CellRef end);
  void emitHandlerFrame(HandlerInfo &h);
  void emitFrameBody(MethodInfo &m);
  void emitInit(CellRef entryLabel);
  CellRef emitFieldDefault(const VarDecl &field, Container *owner);
  CellRef emitUpgradeHook(Container &owner);
  void emitDispatch(CellRef entryLabel);
  void buildProgramStructure();
  std::vector<const HandlerInfo *> mergedFor(const std::string &type);
  void beginHandlerCtx(HandlerInfo &h);

  // -- beliefs (§6.1, §6.5)
  // ----------------------------------------------------
  /// A declared predicate: its cell on the agent's `d.belief` rank and the
  /// argument types the checker already verified.
  struct PredInfo {
    CellRef cell{};
    std::vector<Ty> argTys;
    std::size_t nkeys{0};
    /// Only checked ingestion writes an arbitrated predicate's clauses.
    bool arbitrated{false};
    /// The policy's clock skew in seconds, for ingestion's order check.
    double clockSkewSeconds{0.0};
  };
  /// How one argument of a term participates in a match or a build.
  struct ArgPlan {
    enum class K : std::uint8_t { Value, Var, Dup, Wild } k{K::Wild};
    CellRef reg{};         ///< Value: the evaluated argument
    std::size_t first{0};  ///< Dup: index of the variable it repeats
    bool composite{false}; ///< Value: a list, compared and copied as a term
  };
  struct PlanVar {
    CellRef cap{};   ///< holds the ref of the clause argument it matched
    CellRef local{}; ///< the local the variable declares
    std::string name;
    Ty ty;
  };
  struct TermPlan {
    const PredInfo *pred{nullptr};
    std::string name;
    std::vector<ArgPlan> args;
    std::vector<PlanVar> vars;
  };

  std::size_t beliefEventDepth_;
  std::unordered_map<std::string, PredInfo> preds_;
  std::optional<CellRef> lastPred_;
  CellRef a3_{}, a4_{}, roundsG_{};
  /// Set only by `rtObserve` around its `rtBSet` call: carry the replaced
  /// support's observation history over to the surviving support.
  CellRef keepHistG_{};
  CellRef rtBRaise_{}, rtHasSrc_{}, rtBDrop_{}, rtBSet_{}, rtSettle_{};

  void mintPredicates();
  const PredInfo &predOf(const Term &t);
  void breakOp(Opd owner, DimRef dim);
  void unlinkFromRank(CellRef xReg, DimRef dim);
  void emitRtBRaise();
  void emitRtHasSrc();
  void emitRtBDrop();
  void emitRtBSet();
  void emitObserve(CellRef entryLabel);
  void emitObserveRefusal(CellRef entryLabel);
  void emitSourceChange(CellRef entryLabel);
  /// Re-attach a detached history (@p moved, zero when none) to support
  /// @p sc, and return @p sc in r1.
  void finishSupport(CellRef sc, CellRef moved);
  /// Leave @p code in `$turn_result` and halt; the adapter commits only
  /// `Committed` and undoes every other code.
  void endObservation(std::string_view code);
  void emitSettle();
  TermPlan planTerm(const Term &t, bool allowVars);
  void declarePlanVars(const TermPlan &pl);
  CellRef buildTerm(const TermPlan &pl);
  void requireTrue(CellRef cond, CellRef orElse);
  void emitMatch(const TermPlan &pl, CellRef termReg, CellRef noMatch);
  void bindVars(const TermPlan &pl);
  CellRef snapshotClauses(const PredInfo &p);
  void pushTail(CellRef listReg, CellRef tailReg, CellRef cell);
  void selfName(CellRef out);
  Ty believesExpr(const EBelieves &b, CellRef out);
  void mintPolicyMaster(CellRef predicate, const ArbitrationPolicy &policy);
  Ty findallExpr(const EFindall &f, CellRef out, SourceLocation loc);
  Ty forBelieves(const EFor &f, CellRef out);
  Ty beliefChange(EBeliefChange::Kind kind, const Term &t, CellRef out);
  void emitBeliefBind(HandlerInfo &h);

  // -- capacities, skills and behaviors (§5.3, §5.7)
  // ---------------------------------
  void prepareMethodSet(std::map<std::string, MethodInfo> &set,
                        const std::vector<Member> &members, Container *owner);
  void prepareExtensions();
  void reachFrom(const std::vector<Member> &members,
                 std::vector<std::string> &skillOrder,
                 std::vector<std::string> &behaviorOrder);
  void reachExpr(const Expr &e, std::vector<std::string> &skillOrder,
                 std::vector<std::string> &behaviorOrder);
  Container &makeContainer(Container::K k, const std::string &name,
                           const std::vector<Param> &params,
                           const std::vector<Member> &members,
                           SourceLocation loc);
  std::vector<std::string>
  capacityClosure(const std::vector<std::string> &names) const;
  void addFieldTo(CellRef owner, std::optional<CellRef> &last,
                  std::unordered_map<std::string, FieldInfo> &into,
                  const std::string &fname, const Ty &ty);
  [[nodiscard]] const FieldInfo *findField(const std::string &n) const;
  [[nodiscard]] MethodInfo *findMethod(const std::string &n);
  [[nodiscard]] std::optional<std::string>
  capacityOfMethod(const std::string &method) const;
  std::vector<const MethodSig *> capacityMethods(const std::string &cap) const;
  Ty capacityCall(const std::string &cap, const std::string &method,
                  const std::vector<ExprPtr> &args, CellRef out,
                  SourceLocation loc);
  void emitRtCapFind();
  void emitContainerRoutines();
  void forEachInstanceRef(Container &c, const std::function<void(CellRef)> &fn);
  void emitLifecycle(Container &c, bool uninstall);
  void emitUpgradeLifecycle(Container &c);
  void emitContainerInits(Container &c);
  void beginRoutine(CellRef label, Container *owner, Framed *fr = nullptr);
  Ty installByName(EInstall::Kind kind, const std::string &name,
                   const std::vector<ExprPtr> &args, CellRef out,
                   SourceLocation loc);
  HandlerInfo makeHandlerInfo(const HandlerDef &h, std::size_t index,
                              Container *owner);

  // -- communication and stopping (§4.5, §7.1) --------------------------------
  Ty tellExpr(const ETell &t, CellRef out, SourceLocation loc);
  Ty stopExpr(const EStop &s, CellRef out, SourceLocation loc);
  Ty terminateExpr(const ETerminate &t, CellRef out);
  /// The receiver half of `tell`/`untell`, reached from the dispatcher.
  struct TellSeq {
    const std::string *pred{nullptr};
    const PredInfo *info{nullptr};
    bool untell{false};
    CellRef label{};
  };
  std::vector<TellSeq> emitTellTests(CellRef ty, CellRef eq);
  void emitTellBodies(const std::vector<TellSeq> &seqs, CellRef end);
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

Ty Gen::fromType(const TypeExpr &t, SourceLocation loc) {
  switch (t.kind) {
  case TypeExpr::Kind::Unit:
    return tyOf(Ty::K::Unit);
  case TypeExpr::Kind::List: {
    Ty l = tyOf(Ty::K::List);
    if (t.args.empty())
      bail(CompileError::UnknownType, "list without element", loc);
    l.args.push_back(fromType(t.args[0], loc));
    return l;
  }
  case TypeExpr::Kind::Named:
    break;
  }
  const std::string &n = t.name;
  if (n == "Int" || n == "Time" || n == "Duration" || n == "Intention" ||
      n == "Timer") {
    return tyOf(Ty::K::Int);
  }
  if (n == "Float") return tyOf(Ty::K::Float);
  if (n == "Bool") return tyOf(Ty::K::Bool);
  if (n == "Str" || n == "Agent" || n == "Space") return tyOf(Ty::K::Str);
  if (n == "Opt") {
    if (t.args.size() != 1) {
      bail(CompileError::TypeMismatch, "Opt takes one type argument", loc);
    }
    Ty o = tyOf(Ty::K::Opt);
    o.args.push_back(fromType(t.args[0], loc));
    return o;
  }
  if (events_.contains(n) || records_.contains(n) || findPreludeEvent(n)) {
    Ty r   = tyOf(Ty::K::Record);
    r.name = n;
    return r;
  }
  bail(CompileError::UnknownType, "unknown type " + n, loc);
}

std::vector<std::string> Gen::supertypesOf(const std::string &type) {
  std::vector<std::string> out;
  std::string cur = type;
  std::unordered_set<std::string> seen;
  while (true) {
    const auto it = events_.find(cur);
    if (it == events_.end() || !it->second->extends ||
        !seen.insert(cur).second) {
      break;
    }
    cur = *it->second->extends;
    if (!events_.contains(cur)) {
      bail(CompileError::UnknownType, "unknown supertype " + cur,
           it->second->loc);
    }
    out.push_back(cur);
  }
  return out;
}

std::vector<std::pair<std::string, Ty>> Gen::fieldsOf(const std::string &type) {
  std::vector<std::pair<std::string, Ty>> out;
  if (const auto r = records_.find(type); r != records_.end()) {
    for (const Field &f : r->second->fields) {
      out.emplace_back(f.name, fromType(f.type, f.loc));
    }
    return out;
  }
  if (events_.contains(type)) {
    auto chain = supertypesOf(type);
    std::ranges::reverse(chain);
    chain.push_back(type);
    for (const auto &t : chain) {
      for (const Field &f : events_.at(t)->fields) {
        out.emplace_back(f.name, fromType(f.type, f.loc));
      }
    }
    appendImplicitFields(out);
    return out;
  }
  if (const PreludeEvent *pe = findPreludeEvent(type)) {
    for (const PreludeField &f : pe->fields) {
      out.emplace_back(std::string(f.name), preludeTy(f));
    }
    appendImplicitFields(out);
  }
  return out;
}

Ty Gen::preludeTy(const PreludeField &f) {
  TypeExpr te;
  te.kind = TypeExpr::Kind::Named;
  te.name = std::string(f.type);
  Ty t    = fromType(te, {});
  if (!f.optional) return t;
  Ty o = tyOf(Ty::K::Opt);
  o.args.push_back(std::move(t));
  return o;
}

void Gen::appendImplicitFields(std::vector<std::pair<std::string, Ty>> &out) {
  for (const PreludeField &f : preludeImplicitFields()) {
    out.emplace_back(std::string(f.name), preludeTy(f));
  }
}

std::optional<Ty> Gen::fieldTy(const std::string &type, const std::string &f) {
  for (auto &[n, t] : fieldsOf(type)) {
    if (n == f) return t;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Shared routines
// ---------------------------------------------------------------------------

void Gen::callFind(CellRef owner, const std::string &fname, CellRef &vp,
                   CellRef &np) {
  mov(S(a0_), S(owner));
  movC(a1_, cStr(fname));
  callLabel(rtFind_);
  vp = reg();
  np = reg();
  mov(S(vp), S(r0_));
  mov(S(np), S(r1_));
}

void Gen::callAppendField(CellRef owner, const std::string &fname,
                          std::optional<CellRef> existing, CellRef &vc) {
  mov(S(a0_), S(owner));
  movC(a1_, cStr(fname));
  if (existing) {
    mov(S(a2_), S(*existing));
  } else {
    movC(a2_, zero_);
  }
  callLabel(rtAppendField_);
  vc = reg();
  mov(S(vc), S(r0_));
}

void Gen::callSetField(CellRef owner, const std::string &fname,
                       std::optional<CellRef> existing, CellRef &vc) {
  mov(S(a0_), S(owner));
  movC(a1_, cStr(fname));
  if (existing) {
    mov(S(a2_), S(*existing));
  } else {
    movC(a2_, zero_);
  }
  callLabel(routineLabel(rtSetField_, &Gen::emitRtSetField));
  vc = reg();
  mov(S(vc), S(r0_));
}

/// Write a scalar field in place. `callSetField` on a field that exists mints
/// a value cell and orphans the old one, a slot per write that excision
/// never returns; a counter or timestamp written every ingestion would leak.
void Gen::putScalar(CellRef owner, std::string_view fname, Opd value) {
  const CellRef vc = reg(), absent = newLabel(), done = newLabel();
  mov(S(a0_), S(owner));
  movC(a1_, cStr(std::string(fname)));
  callLabel(rtFind_);
  mov(S(vc), S(r0_));
  branchIfZero(vc, absent);
  mov(D(vc), value);
  jump(done);
  place(absent);
  CellRef fresh;
  callSetField(owner, std::string(fname), std::nullopt, fresh);
  mov(D(fresh), value);
  place(done);
}

void Gen::callClearField(CellRef owner, const std::string &fname) {
  mov(S(a0_), S(owner));
  movC(a1_, cStr(fname));
  callLabel(routineLabel(rtClearField_, &Gen::emitRtClearField));
}

void Gen::markRecord(CellRef record) {
  CellRef ignored;
  callAppendField(record, std::string(kRecordMarker), std::nullopt, ignored);
}

void Gen::callListPush(CellRef listReg, CellRef elem) {
  mov(S(a0_), S(listReg));
  mov(S(a1_), S(elem));
  callLabel(rtListPush_);
}

CellRef Gen::callOutbox(std::string_view kind) {
  movC(a0_, cStr(std::string(kind)));
  callLabel(rtOutbox_);
  const CellRef en = reg();
  mov(S(en), S(r0_));
  return en;
}

void Gen::callCopy(CellRef src, CellRef dst) {
  mov(S(a0_), S(src));
  callLabel(rtCopy_);
  mov(S(dst), S(r0_));
}

void Gen::callEqual(CellRef a, CellRef b, CellRef out) {
  mov(S(a0_), S(a));
  mov(S(a1_), S(b));
  callLabel(routineLabel(rtEqual_, &Gen::emitRtEqual));
  mov(S(out), S(r0_));
}

void Gen::callRaiseHostEv(CellRef ev) {
  mov(S(a0_), S(ev));
  callLabel(routineLabel(rtRaiseHostEv_, &Gen::emitRtRaiseHostEv));
}

void Gen::callStrLen(CellRef s, CellRef out) {
  mov(S(a0_), S(s));
  callLabel(routineLabel(rtStrLen_, &Gen::emitRtStrLen));
  mov(S(out), S(r0_));
}

void Gen::emitCondTick() {
  const CellRef skip = newLabel(), ok = newLabel();
  branchIfZero(inCondG_, skip);
  const CellRef exhausted = reg();
  emit(OpcodeKind::Lte, "fuel<=0", {S(condFuelG_), S(zero_)}, {exhausted});
  branchIfZero(exhausted, ok);
  emit(OpcodeKind::Cut, "cut", {S(condContractD_)});
  failNow();
  place(ok);
  emit(OpcodeKind::Sub, "fuel-", {S(condFuelG_), S(one_)}, {condFuelG_});
  place(skip);
}

void Gen::emitContractViolation() {
  const CellRef notCond = newLabel(), notPlan = newLabel();
  branchIfZero(inCondG_, notCond);
  emit(OpcodeKind::Cut, "cut", {S(condD_)});
  failNow();
  place(notCond);
  branchIfZero(inPlanG_, notPlan);
  emit(OpcodeKind::Cut, "cut", {S(planContractDG_)});
  failNow();
  place(notPlan);
  failNow();
}

void Gen::emitRtFind() {
  beginInternal(rtFind_);
  const CellRef n = reg(), eqName = reg();
  const CellRef loop = newLabel(), found = newLabel(), missing = newLabel();
  mov(S(n), S(rd(D(a0_), core_.dims().vars)));
  place(loop);
  branchIfZero(n, missing);
  emit(OpcodeKind::Eq, "name?", {D(n), S(a1_)}, {eqName});
  branch(S(eqName), found);
  mov(S(n), S(rd(D(n), core_.dims().vars)));
  jump(loop);
  place(found);
  mov(S(r1_), S(n));
  mov(S(r0_), S(rd(D(n), core_.dims().values)));
  ret();
  place(missing);
  movC(r0_, zero_);
  movC(r1_, zero_);
  ret();
}

void Gen::emitRtAppendField() {
  beginInternal(rtAppendField_);
  const CellRef nn = reg(), vc = reg();
  const CellRef fresh = newLabel(), cont = newLabel();
  alloc(nn);
  mov(D(nn), S(a1_));
  emitAppendTail(D(a0_), core_.dims().vars, core_.dims().vars, nn);
  branchIfZero(a2_, fresh);
  mov(S(vc), S(a2_));
  jump(cont);
  place(fresh);
  alloc(vc);
  place(cont);
  linkOp(D(nn), core_.dims().values, D(vc));
  mov(S(r0_), S(vc));
  mov(S(r1_), S(nn));
  ret();
}

void Gen::emitRtSetField() {
  beginInternal(*rtSetField_);
  const CellRef owner = reg(), fname = reg(), ex = reg(), np = reg(),
                vc        = reg();
  const CellRef appendNew = newLabel(), fresh = newLabel(), cont = newLabel();
  mov(S(owner), S(a0_));
  mov(S(fname), S(a1_));
  mov(S(ex), S(a2_));
  callLabel(rtFind_);
  mov(S(np), S(r1_));
  branchIfZero(np, appendNew);
  branchIfZero(ex, fresh);
  mov(S(vc), S(ex));
  jump(cont);
  place(fresh);
  alloc(vc);
  place(cont);
  linkOp(D(np), core_.dims().values, D(vc));
  mov(S(r0_), S(vc));
  mov(S(r1_), S(np));
  ret();
  place(appendNew);
  mov(S(a0_), S(owner));
  mov(S(a1_), S(fname));
  mov(S(a2_), S(ex));
  callLabel(rtAppendField_);
  ret();
}

void Gen::emitRtClearField() {
  beginInternal(*rtClearField_);
  const CellRef np   = reg();
  const CellRef done = newLabel();
  callLabel(rtFind_);
  mov(S(np), S(r1_));
  branchIfZero(np, done);
  breakOp(D(np), core_.dims().values);
  place(done);
  ret();
}

void Gen::emitRtListPush() {
  beginInternal(rtListPush_);
  emitAppendTail(D(a0_), core_.dims().grab, core_.dims().step, a1_);
  ret();
}

void Gen::emitRtOutbox() {
  beginInternal(rtOutbox_);
  const CellRef entry = reg();
  alloc(entry);
  mov(D(entry), S(a0_));
  emitAppendTail(S(agentCell_), dims_.outbox, dims_.outbox, entry);
  mov(S(r0_), S(entry));
  ret();
}

void Gen::emitRtCopy() {
  // Recursive through list elements and field values, so its temporaries live
  // on a frame per activation like a cyclic method's.
  tail_            = std::nullopt;
  copyPlan_.cyclic = true;
  frame_           = &copyPlan_;
  place(rtCopy_);
  jump(copyPro_);
  place(copyBody_);
  const CellRef s = reg(), y = reg(), prev = reg(), n = reg(), nn = reg(),
                v = reg(), vv = reg(), e = reg(), prevE = reg(), nv = reg();
  const CellRef vloop = newLabel(), vdone = newLabel(), novalue = newLabel();
  const CellRef lloop = newLabel(), ldone = newLabel(), lfirst = newLabel();
  mov(S(s), S(a0_));
  alloc(y);
  mov(D(y), D(s));
  mov(S(prev), S(y));
  mov(S(n), S(rd(D(s), core_.dims().vars)));
  place(vloop);
  branchIfZero(n, vdone);
  alloc(nn);
  mov(D(nn), D(n));
  linkOp(D(prev), core_.dims().vars, D(nn));
  mov(S(v), S(rd(D(n), core_.dims().values)));
  branchIfZero(v, novalue);
  mov(S(a0_), S(v));
  callUser(rtCopy_);
  mov(S(vv), S(r0_));
  linkOp(D(nn), core_.dims().values, D(vv));
  place(novalue);
  mov(S(prev), S(nn));
  mov(S(n), S(rd(D(n), core_.dims().vars)));
  jump(vloop);
  place(vdone);
  mov(S(e), S(rd(D(s), core_.dims().grab)));
  branchIfZero(e, ldone);
  mov(S(a0_), S(e));
  callUser(rtCopy_);
  mov(S(nv), S(r0_));
  linkOp(D(y), core_.dims().grab, D(nv));
  mov(S(prevE), S(nv));
  place(lfirst);
  mov(S(e), S(rd(D(e), core_.dims().step)));
  place(lloop);
  branchIfZero(e, ldone);
  mov(S(a0_), S(e));
  callUser(rtCopy_);
  mov(S(nv), S(r0_));
  linkOp(D(prevE), core_.dims().step, D(nv));
  mov(S(prevE), S(nv));
  mov(S(e), S(rd(D(e), core_.dims().step)));
  jump(lloop);
  place(ldone);
  mov(S(r0_), S(y));
  emitFramePop();
  ret();
  place(copyPro_);
  emitFramePush(copyPlan_);
  jump(copyBody_);
  frame_ = nullptr;
}

void Gen::emitRtEqual() {
  tail_                  = std::nullopt;
  equalPlan_.cyclic      = true;
  frame_                 = &equalPlan_;
  const CellRef equalPro = newLabel(), equalBody = newLabel();
  place(*rtEqual_);
  jump(equalPro);
  place(equalBody);
  const CellRef x = reg(), y = reg(), eq = reg(), n = reg(), v1 = reg(),
                v2 = reg(), e1 = reg(), e2 = reg(), isMarker = reg();
  const CellRef xZero = newLabel(), bothZero = newLabel(), nope = newLabel(),
                headOk = newLabel(), vloop = newLabel(), vnext = newLabel(),
                vdone = newLabel(), v1Zero = newLabel(), vcheck = newLabel(),
                lloop = newLabel(), l1Zero = newLabel(), lstep = newLabel(),
                yep = newLabel();
  mov(S(x), S(a0_));
  mov(S(y), S(a1_));
  branchIfZero(x, xZero);
  branchIfZero(y, nope);
  emit(OpcodeKind::Eq, "heq", {D(x), D(y)}, {eq});
  branch(S(eq), headOk);
  jump(nope);
  place(xZero);
  branchIfZero(y, bothZero);
  jump(nope);
  place(bothZero);
  jump(yep);

  place(headOk);
  mov(S(n), S(rd(D(x), core_.dims().vars)));
  place(vloop);
  branchIfZero(n, vdone);
  emit(OpcodeKind::Eq, "mrk?", {D(n), S(cStr(std::string(kRecordMarker)))},
       {isMarker});
  branch(S(isMarker), vnext);
  mov(S(v1), S(rd(D(n), core_.dims().values)));
  mov(S(a0_), S(y));
  mov(S(a1_), D(n));
  callLabel(rtFind_);
  mov(S(v2), S(r0_));
  branchIfZero(v1, v1Zero);
  branchIfZero(v2, nope);
  mov(S(a0_), S(v1));
  mov(S(a1_), S(v2));
  callUser(*rtEqual_);
  branchIfZero(r0_, nope);
  jump(vnext);
  place(v1Zero);
  branchIfZero(v2, vnext);
  jump(nope);
  place(vcheck);
  place(vnext);
  mov(S(n), S(rd(D(n), core_.dims().vars)));
  jump(vloop);

  place(vdone);
  mov(S(e1), S(rd(D(x), core_.dims().grab)));
  mov(S(e2), S(rd(D(y), core_.dims().grab)));
  place(lloop);
  branchIfZero(e1, l1Zero);
  branchIfZero(e2, nope);
  mov(S(a0_), S(e1));
  mov(S(a1_), S(e2));
  callUser(*rtEqual_);
  branchIfZero(r0_, nope);
  place(lstep);
  mov(S(e1), S(rd(D(e1), core_.dims().step)));
  mov(S(e2), S(rd(D(e2), core_.dims().step)));
  jump(lloop);
  place(l1Zero);
  branchIfZero(e2, yep);
  jump(nope);

  place(yep);
  movC(r0_, one_);
  emitFramePop();
  ret();
  place(nope);
  movC(r0_, zero_);
  emitFramePop();
  ret();
  place(equalPro);
  emitFramePush(equalPlan_);
  jump(equalBody);
  frame_ = nullptr;
}

void Gen::emitRtRaiseHostEv() {
  beginInternal(*rtRaiseHostEv_);
  CellRef vc       = reg();
  const CellRef ev = reg(), self = reg(), sp = reg(), sc = reg(), tc = reg();
  mov(S(ev), S(a0_));
  selfName(self);
  alloc(sp);
  mov(D(sp), S(cStr(std::string(kListContent))));
  alloc(sc);
  mov(D(sc), S(self));
  callListPush(sp, sc);
  callSetField(ev, std::string(kEventImplicitSource), sp, vc);
  alloc(sp);
  mov(D(sp), S(cStr(std::string(kListContent))));
  callSetField(ev, std::string(kEventImplicitSpace), sp, vc);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp      = reg();
  CellRef timeP, nameP;
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  callFind(tp, std::string(kTurnTime), timeP, nameP);
  movC(tc, zero_);
  const CellRef noTime = newLabel();
  branchIfZero(timeP, noTime);
  mov(S(tc), D(timeP));
  place(noTime);
  callSetField(ev, std::string(kEventImplicitTime), std::nullopt, vc);
  mov(D(vc), S(tc));
  const FieldInfo &ord = fields_.at(std::string(kFieldOrdinal));
  emit(OpcodeKind::Add, "ord+", {S(ord.valueCell), S(one_)}, {ord.valueCell});
  callSetField(ev, std::string(kEventImplicitOrdinal), std::nullopt, vc);
  mov(D(vc), S(ord.valueCell));
  emitAppendTail(S(agentCell_), dims_.pending, dims_.pending, ev);
  ret();
}

void Gen::emitRtStrLen() {
  beginInternal(*rtStrLen_);
  const CellRef s = reg(), len = reg(), ch = reg(), eq = reg();
  const CellRef loop = newLabel(), done = newLabel();
  mov(S(s), S(a0_));
  movC(len, zero_);
  place(loop);
  emit(OpcodeKind::Value, "ch", {S(s), S(len), S(one_)}, {ch});
  emit(OpcodeKind::Eq, "end?", {S(ch), S(cStr(""))}, {eq});
  branch(S(eq), done);
  emit(OpcodeKind::Add, "len+", {S(len), S(one_)}, {len});
  jump(loop);
  place(done);
  mov(S(r0_), S(len));
  ret();
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

CellRef Gen::initialValueCell(const Ty &t) {
  switch (t.k) {
  case Ty::K::Int:
    return arena_.makeScalarCell(std::int64_t{0});
  case Ty::K::Float:
    return arena_.makeScalarCell(0.0);
  case Ty::K::Bool:
    return arena_.makeScalarCell(false);
  case Ty::K::Str:
    return arena_.makeCell("");
  case Ty::K::List:
  case Ty::K::Opt:
    return arena_.makeCell(std::string(kListContent));
  case Ty::K::Record: {
    const CellRef record = arena_.makeCell(t.name);
    const CellRef marker = arena_.makeCell(std::string(kRecordMarker));
    zigzag::expectWritten(
        arena_.link(record, core_.dims().vars, DimVector::POS, marker));
    return record;
  }
  case Ty::K::Unit:
    return arena_.makeCell(std::string(kUnitContent));
  }
  return arena_.makeCell(std::string(kUnitContent));
}

void Gen::addField(const std::string &fname, const Ty &ty) {
  addFieldTo(agentCell_, lastFieldName_, fields_, fname, ty);
}

CellRef Gen::loadFieldInto(const FieldInfo &f, CellRef out) {
  if (f.ty.composite()) {
    mov(S(out), S(rd(S(f.nameCell), core_.dims().values)));
  } else {
    mov(S(out), S(f.valueCell));
  }
  return out;
}

void Gen::storeField(const FieldInfo &f, CellRef valueReg) {
  if (f.ty.composite()) {
    linkOp(S(f.nameCell), core_.dims().values, D(valueReg));
  } else {
    mov(S(f.valueCell), S(valueReg));
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

Ty Gen::valueInto(const Expr &e, CellRef out) {
  Ty t = expr(e, out);
  if (t.composite() && isPlace(e)) {
    callCopy(out, out);
  }
  return t;
}

Ty Gen::literal(const ELiteral &l, CellRef out, SourceLocation loc) {
  switch (l.kind) {
  case ELiteral::Kind::Int:
    movC(out, cInt(l.intValue));
    return tyOf(Ty::K::Int);
  case ELiteral::Kind::Duration:
    movC(out, cInt(l.durationMs));
    return tyOf(Ty::K::Int);
  case ELiteral::Kind::Float:
    movC(out, cFloat(l.floatValue));
    return tyOf(Ty::K::Float);
  case ELiteral::Kind::Str:
    movC(out, cStr(l.text));
    return tyOf(Ty::K::Str);
  case ELiteral::Kind::Bool:
    movC(out, cBool(l.boolValue));
    return tyOf(Ty::K::Bool);
  case ELiteral::Kind::Unit:
    movC(out, unit_c_);
    return tyOf(Ty::K::Unit);
  case ELiteral::Kind::None: {
    alloc(out);
    mov(D(out), S(cStr(std::string(kListContent))));
    return tyOf(Ty::K::Opt);
  }
  }
  unsupported("literal", loc);
}

Ty Gen::name(const EName &n, CellRef out, SourceLocation loc) {
  if (const Local *l = findLocal(n.name)) {
    mov(S(out), S(l->reg));
    return l->ty.k == Ty::K::Opt ? unwrapOpt(out, l->ty, out, loc) : l->ty;
  }
  if (const FieldInfo *f = findField(n.name)) {
    loadFieldInto(*f, out);
    return f->ty.k == Ty::K::Opt ? unwrapOpt(out, f->ty, out, loc) : f->ty;
  }
  bail(CompileError::UnknownName, "unknown name " + n.name, loc);
}

Ty Gen::unwrapOpt(CellRef optReg, const Ty &opt, CellRef out,
                  SourceLocation loc) {
  if (opt.args.empty()) unsupported("a read of an untyped none", loc);
  const Ty &inner    = opt.args[0];
  const CellRef held = rd(D(optReg), core_.dims().grab);
  branchIfZero(held, failStep_);
  if (inner.composite()) {
    mov(S(out), S(held));
  } else {
    mov(S(out), D(held));
  }
  return inner;
}

void Gen::coerceInto(const Ty &target, const Ty &actual, CellRef v) {
  if (target.k != Ty::K::Opt || actual.k == Ty::K::Opt) return;
  const CellRef res = reg();
  alloc(res);
  mov(D(res), S(cStr(std::string(kListContent))));
  CellRef cell = v;
  if (!actual.composite()) {
    cell = reg();
    alloc(cell);
    mov(D(cell), S(v));
  }
  callListPush(res, cell);
  mov(S(v), S(res));
}

std::optional<Ty> Gen::staticTy(const Expr &e) {
  if (const auto *n = std::get_if<EName>(&e.node)) {
    if (const Local *l = findLocal(n->name)) return l->ty;
    if (const FieldInfo *f = findField(n->name)) return f->ty;
    return std::nullopt;
  }
  if (const auto *f = std::get_if<EField>(&e.node)) {
    auto base = staticTy(*f->base);
    if (base && base->k == Ty::K::Opt && !base->args.empty()) {
      base = base->args[0];
    }
    if (!base || base->k != Ty::K::Record) return std::nullopt;
    return fieldTy(base->name, f->name);
  }
  return std::nullopt;
}

Ty Gen::special(const ESpecial &s, CellRef out, SourceLocation loc) {
  switch (s.kind) {
  case ESpecial::Kind::Self:
    loadFieldInto(fields_.at(std::string(kFieldName)), out);
    return tyOf(Ty::K::Str);
  case ESpecial::Kind::Parent: {
    const FieldInfo &parent = fields_.at(std::string(kFieldParent));
    loadFieldInto(parent, out);
    return unwrapOpt(out, parent.ty, out, loc);
  }
  case ESpecial::Kind::Inner:
    movC(out, cStr(std::string(kSpaceInner)));
    return tyOf(Ty::K::Str);
  case ESpecial::Kind::Outer:
    movC(out, cStr(std::string(kSpaceOuter)));
    return tyOf(Ty::K::Str);
  case ESpecial::Kind::Occurrence:
    if (!ctx_.eventTy) {
      bail(CompileError::UnknownName, "occurrence outside a handler", loc);
    }
    mov(S(out), S(curEvent_));
    return *ctx_.eventTy;
  }
  unsupported("name", loc);
}

Ty Gen::expr(const Expr &e, CellRef out) {
  const SourceLocation loc = e.loc;
  return std::visit(
      [&](const auto &n) -> Ty {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ELiteral>) {
          return literal(n, out, loc);
        } else if constexpr (std::is_same_v<T, EName>) {
          return name(n, out, loc);
        } else if constexpr (std::is_same_v<T, ESpecial>) {
          return special(n, out, loc);
        } else if constexpr (std::is_same_v<T, EUnary>) {
          return unary(n, out, loc);
        } else if constexpr (std::is_same_v<T, EBinary>) {
          return binary(n, out, loc);
        } else if constexpr (std::is_same_v<T, ERange>) {
          return range(n, out);
        } else if constexpr (std::is_same_v<T, ECall>) {
          return call(n, out, loc);
        } else if constexpr (std::is_same_v<T, EIndex>) {
          return index(n, out);
        } else if constexpr (std::is_same_v<T, EField>) {
          return field(n, out, loc);
        } else if constexpr (std::is_same_v<T, EList>) {
          return list(n, out);
        } else if constexpr (std::is_same_v<T, ERecord>) {
          return record(n, out, loc);
        } else if constexpr (std::is_same_v<T, EBlock>) {
          return block(n, out);
        } else if constexpr (std::is_same_v<T, EIf>) {
          return ifExpr(n, out);
        } else if constexpr (std::is_same_v<T, EWhile>) {
          return whileExpr(n, out);
        } else if constexpr (std::is_same_v<T, EFor>) {
          return forExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EEmit>) {
          return emitExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, ESpawn>) {
          return spawnExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EReturn>) {
          if (n.value) {
            valueInto(*n.value, ctx_.retReg);
          } else {
            movC(ctx_.retReg, unit_c_);
          }
          emit(OpcodeKind::Cut, "cut", {S(ctx_.d0)});
          jump(ctx_.epi);
          return tyOf(Ty::K::Unit);
        } else if constexpr (std::is_same_v<T, EConsume>) {
          mov(S(fields_.at(std::string(kFieldConsumed)).valueCell), S(true_));
          movC(out, unit_c_);
          return tyOf(Ty::K::Unit);
        } else if constexpr (std::is_same_v<T, EFail>) {
          failNow();
          return tyOf(Ty::K::Unit);
        } else if constexpr (std::is_same_v<T, EBelieves>) {
          return believesExpr(n, out);
        } else if constexpr (std::is_same_v<T, EFindall>) {
          return findallExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EBeliefChange>) {
          return beliefChange(n.kind, n.fact, out);
        } else if constexpr (std::is_same_v<T, EInstall>) {
          return installByName(n.kind, n.name, n.args, out, loc);
        } else if constexpr (std::is_same_v<T, ETell>) {
          return tellExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EStop>) {
          return stopExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, ETerminate>) {
          return terminateExpr(n, out);
        } else if constexpr (std::is_same_v<T, EAdopt>) {
          return adoptExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EAchieve>) {
          return achieveExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EAsk>) {
          return askExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EAwait>) {
          return awaitExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EDrop>) {
          return dropExpr(n, out, loc);
        } else if constexpr (std::is_same_v<T, EYield>) {
          return yieldExpr(n, out, loc);
        } else {
          unsupported("this construct", loc);
        }
      },
      e.node);
}

Ty Gen::unary(const EUnary &u, CellRef out, SourceLocation loc) {
  switch (u.op) {
  case EUnary::Op::Neg: {
    const CellRef v = reg();
    const Ty t      = expr(*u.operand, v);
    emit(OpcodeKind::Neg, "neg", {S(v)}, {out});
    return t;
  }
  case EUnary::Op::Not: {
    // Succeeds with () when the operand fails, and fails when it succeeds.
    const CellRef ok = newLabel(), end = newLabel();
    const CellRef d = dRegAt(stmtDepth_);
    ++stmtDepth_;
    emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
    emit(OpcodeKind::Choice, "not", {S(opref(ok))});
    const CellRef scratch = reg();
    expr(*u.operand, scratch);
    emit(OpcodeKind::Cut, "cut", {S(d)});
    --stmtDepth_;
    failNow();
    place(ok);
    movC(out, unit_c_);
    place(end);
    return tyOf(Ty::K::Unit);
  }
  case EUnary::Op::Size: {
    const CellRef l = reg();
    const Ty t      = expr(*u.operand, l);
    if (t.k == Ty::K::Str) {
      callStrLen(l, out);
      return tyOf(Ty::K::Int);
    }
    if (t.k != Ty::K::List) unsupported("size of a non-list", loc);
    movC(out, zero_);
    listWalk(l, [&](CellRef) {
      emit(OpcodeKind::Add, "n+", {S(out), S(one_)}, {out});
    });
    return tyOf(Ty::K::Int);
  }
  case EUnary::Op::Generate: {
    const CellRef l = reg();
    const Ty t      = expr(*u.operand, l);
    if (t.k == Ty::K::Str) {
      const CellRef idx = reg(), ch = reg(), eq = reg();
      const CellRef loop = newLabel(), next = newLabel(), done = newLabel(),
                    after = newLabel();
      movC(idx, zero_);
      place(loop);
      emitCondTick();
      emit(OpcodeKind::Value, "ch", {S(l), S(idx), S(one_)}, {ch});
      emit(OpcodeKind::Eq, "end?", {S(ch), S(cStr(""))}, {eq});
      branch(S(eq), done);
      emit(OpcodeKind::Choice, "gen", {S(opref(next))});
      mov(S(out), S(ch));
      jump(after);
      place(next);
      emit(OpcodeKind::Add, "idx+", {S(idx), S(one_)}, {idx});
      jump(loop);
      place(done);
      failNow();
      place(after);
      return tyOf(Ty::K::Str);
    }
    if (t.k != Ty::K::List && t.k != Ty::K::Opt) {
      unsupported("generation (!e) on a non-list", loc);
    }
    const Ty et        = t.args.empty() ? tyOf(Ty::K::Unit) : t.args[0];
    const CellRef cur  = reg();
    const CellRef loop = newLabel(), next = newLabel(), done = newLabel(),
                  after = newLabel();
    mov(S(cur), S(rd(D(l), core_.dims().grab)));
    place(loop);
    emitCondTick();
    branchIfZero(cur, done);
    emit(OpcodeKind::Choice, "gen", {S(opref(next))});
    if (et.composite()) {
      callCopy(cur, out);
    } else {
      mov(S(out), D(cur));
    }
    jump(after);
    place(next);
    mov(S(cur), S(rd(D(cur), core_.dims().step)));
    jump(loop);
    place(done);
    failNow();
    place(after);
    return et;
  }
  }
  unsupported("unary operator", loc);
}

void Gen::listWalk(CellRef listReg, const std::function<void(CellRef)> &each) {
  const CellRef e    = reg();
  const CellRef loop = newLabel(), done = newLabel();
  mov(S(e), S(rd(D(listReg), core_.dims().grab)));
  place(loop);
  branchIfZero(e, done);
  each(e);
  mov(S(e), S(rd(D(e), core_.dims().step)));
  jump(loop);
  place(done);
}

Ty Gen::comparison(OpcodeKind k, const EBinary &b, CellRef out) {
  const CellRef l = reg(), r = reg(), t = reg();
  const CellRef ok = newLabel();
  const Ty lt      = expr(*b.left, l);
  const Ty rt      = expr(*b.right, r);
  if (lt.composite() || rt.composite()) {
    if (k != OpcodeKind::Eq && k != OpcodeKind::Neq) {
      unsupported("ordering comparison of records or lists", b.left->loc);
    }
    if (lt.k == Ty::K::Opt && rt.k != Ty::K::Opt) coerceInto(lt, rt, r);
    if (rt.k == Ty::K::Opt && lt.k != Ty::K::Opt) coerceInto(rt, lt, l);
    callEqual(l, r, t);
    if (k == OpcodeKind::Neq) {
      emit(OpcodeKind::Not, "neq", {S(t)}, {t});
    }
    branch(S(t), ok);
    failNow();
    place(ok);
    mov(S(out), S(r));
    return rt;
  }
  emit(k, "cmp", {S(l), S(r)}, {t});
  branch(S(t), ok);
  failNow();
  place(ok);
  mov(S(out), S(r));
  return rt;
}

Ty Gen::concat(const EBinary &b, CellRef out) {
  const CellRef l = reg(), r = reg();
  const Ty lt = expr(*b.left, l);
  const Ty rt = expr(*b.right, r);
  if (lt.k == Ty::K::Str && rt.k == Ty::K::Str) {
    emit(OpcodeKind::Add, "cat", {S(l), S(r)}, {out});
    return lt;
  }
  if (lt.k == Ty::K::List && rt.k == Ty::K::List) {
    const CellRef res = reg();
    alloc(res);
    mov(D(res), S(cStr(std::string(kListContent))));
    const Ty elemTy = lt.args.empty()
                          ? rt.args.empty() ? tyOf(Ty::K::Unit) : rt.args[0]
                          : lt.args[0];
    for (const CellRef src : {l, r}) {
      listWalk(src, [&](CellRef el) {
        const CellRef fresh = reg();
        if (elemTy.composite()) {
          callCopy(el, fresh);
        } else {
          alloc(fresh);
          mov(D(fresh), D(el));
        }
        callListPush(res, fresh);
      });
    }
    mov(S(out), S(res));
    Ty t = tyOf(Ty::K::List);
    t.args.push_back(elemTy);
    return t;
  }
  bail(CompileError::TypeMismatch, "++ needs two strings or two lists",
       b.left->loc);
}

Ty Gen::binary(const EBinary &b, CellRef out, SourceLocation loc) {
  using Op         = EBinary::Op;
  const auto arith = [&](OpcodeKind k) {
    const CellRef l = reg(), r = reg();
    const Ty lt = expr(*b.left, l);
    const Ty rt = expr(*b.right, r);
    emit(k, "arith", {S(l), S(r)}, {out});
    return lt.k == Ty::K::Float || rt.k == Ty::K::Float ? tyOf(Ty::K::Float)
                                                        : lt;
  };
  switch (b.op) {
  case Op::Mul:
    return arith(OpcodeKind::Mul);
  case Op::Div:
    return arith(OpcodeKind::Div);
  case Op::Mod:
    return arith(OpcodeKind::Mod);
  case Op::Add:
    return arith(OpcodeKind::Add);
  case Op::Sub:
    return arith(OpcodeKind::Sub);
  case Op::Concat:
    return concat(b, out);
  case Op::Lt:
    return comparison(OpcodeKind::Lt, b, out);
  case Op::Le:
    return comparison(OpcodeKind::Lte, b, out);
  case Op::Gt:
    return comparison(OpcodeKind::Gt, b, out);
  case Op::Ge:
    return comparison(OpcodeKind::Gte, b, out);
  case Op::Eq:
    return comparison(OpcodeKind::Eq, b, out);
  case Op::Ne:
    return comparison(OpcodeKind::Neq, b, out);
  case Op::And: {
    const CellRef scratch = reg();
    expr(*b.left, scratch);
    return expr(*b.right, out);
  }
  case Op::Alt: {
    const CellRef alt = newLabel(), end = newLabel();
    emit(OpcodeKind::Choice, "alt", {S(opref(alt))});
    const Ty t = expr(*b.left, out);
    jump(end);
    place(alt);
    expr(*b.right, out);
    place(end);
    return t;
  }
  case Op::Assign:
  case Op::RevAssign:
    return assign(b, out, loc);
  case Op::Scan: {
    const CellRef subj = reg(), savedSubj = reg(), savedPos = reg(),
                  savedLen = reg();
    const Ty lt            = expr(*b.left, subj);
    if (lt.k != Ty::K::Str) {
      bail(CompileError::TypeMismatch, "? requires a string subject", loc);
    }
    mov(S(savedSubj), S(scanSubjG_));
    mov(S(savedPos), S(scanPosG_));
    mov(S(savedLen), S(scanLenG_));
    const CellRef scanFail = newLabel(), scanDone = newLabel();
    emit(OpcodeKind::Choice, "scan", {S(opref(scanFail))});
    mov(S(scanSubjG_), S(subj));
    movC(scanPosG_, zero_);
    callStrLen(subj, scanLenG_);
    const Ty rt = expr(*b.right, out);
    mov(S(scanSubjG_), S(savedSubj));
    mov(S(scanPosG_), S(savedPos));
    mov(S(scanLenG_), S(savedLen));
    jump(scanDone);
    place(scanFail);
    mov(S(scanSubjG_), S(savedSubj));
    mov(S(scanPosG_), S(savedPos));
    mov(S(scanLenG_), S(savedLen));
    failNow();
    place(scanDone);
    return rt;
  }
  }
  unsupported("binary operator", loc);
}

void Gen::storeInto(const Expr &target, CellRef valueReg, const Ty &valueTy,
                    SourceLocation loc) {
  (void)valueTy;
  if (const auto *n = std::get_if<EName>(&target.node)) {
    if (const Local *l = findLocal(n->name)) {
      mov(S(l->reg), S(valueReg));
      return;
    }
    if (const FieldInfo *f = findField(n->name)) {
      if (n->name.starts_with('$')) {
        bail(CompileError::UnknownName, "reserved field " + n->name, loc);
      }
      storeField(*f, valueReg);
      return;
    }
    bail(CompileError::UnknownName, "unknown name " + n->name, loc);
  }
  if (const auto *f = std::get_if<EField>(&target.node)) {
    const CellRef base = reg();
    const Ty bt        = expr(*f->base, base);
    if (bt.k != Ty::K::Record) {
      bail(CompileError::TypeMismatch, "field of a non-record", loc);
    }
    const auto ft = fieldTy(bt.name, f->name);
    if (!ft) {
      bail(CompileError::UnknownName, "no field " + f->name + " in " + bt.name,
           loc);
    }
    CellRef vp, np;
    callFind(base, f->name, vp, np);
    branchIfZero(vp, failStep_);
    if (ft->composite()) {
      linkOp(D(np), core_.dims().values, D(valueReg));
    } else {
      mov(D(vp), S(valueReg));
    }
    return;
  }
  if (const auto *ix = std::get_if<EIndex>(&target.node)) {
    const CellRef base = reg(), idx = reg(), cnt = reg(), el = reg();
    const Ty bt = expr(*ix->base, base);
    expr(*ix->index, idx);
    if (bt.k != Ty::K::List || (!bt.args.empty() && bt.args[0].composite())) {
      unsupported("assignment to a composite list element", loc);
    }
    const CellRef loop = newLabel(), found = newLabel();
    movC(cnt, zero_);
    mov(S(el), S(rd(D(base), core_.dims().grab)));
    place(loop);
    branchIfZero(el, failStep_);
    const CellRef eq = reg();
    emit(OpcodeKind::Eq, "ix", {S(cnt), S(idx)}, {eq});
    branch(S(eq), found);
    mov(S(el), S(rd(D(el), core_.dims().step)));
    emit(OpcodeKind::Add, "n+", {S(cnt), S(one_)}, {cnt});
    jump(loop);
    place(found);
    mov(D(el), S(valueReg));
    return;
  }
  bail(CompileError::TypeMismatch, "not an assignable place", loc);
}

Ty Gen::assign(const EBinary &b, CellRef out, SourceLocation loc) {
  const CellRef v = reg();
  const Ty t      = valueInto(*b.right, v);
  if (const auto target = staticTy(*b.left)) coerceInto(*target, t, v);
  storeInto(*b.left, v, t, loc);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

void Gen::rangeTest(CellRef cur, CellRef hi, Opd by, CellRef done) {
  // Past the end: cur > hi for a positive step, cur < hi for a negative one.
  const CellRef neg = reg(), t = reg();
  const CellRef posTest = newLabel(), go = newLabel();
  emit(OpcodeKind::Lt, "by<0", {by, S(zero_)}, {neg});
  emit(OpcodeKind::Not, "by>=0", {S(neg)}, {neg});
  branch(S(neg), posTest);
  emit(OpcodeKind::Lt, "cur<hi", {S(cur), S(hi)}, {t});
  branch(S(t), done);
  jump(go);
  place(posTest);
  emit(OpcodeKind::Gt, "cur>hi", {S(cur), S(hi)}, {t});
  branch(S(t), done);
  place(go);
}

Ty Gen::range(const ERange &r, CellRef out) {
  const CellRef lo = reg(), hi = reg(), by = reg(), cur = reg();
  expr(*r.lo, lo);
  expr(*r.hi, hi);
  if (r.by) {
    expr(*r.by, by);
  } else {
    movC(by, one_);
  }
  mov(S(cur), S(lo));
  const CellRef loop = newLabel(), next = newLabel(), done = newLabel(),
                after = newLabel();
  place(loop);
  emitCondTick();
  rangeTest(cur, hi, S(by), done);
  emit(OpcodeKind::Choice, "to", {S(opref(next))});
  mov(S(out), S(cur));
  jump(after);
  place(next);
  emit(OpcodeKind::Add, "cur+", {S(cur), S(by)}, {cur});
  jump(loop);
  place(done);
  failNow();
  place(after);
  return tyOf(Ty::K::Int);
}

Ty Gen::call(const ECall &c, CellRef out, SourceLocation loc) {
  if (const auto cap = capacityCall(c, out, loc)) return *cap;
  if (const auto *fe = std::get_if<EField>(&c.callee->node)) {
    const auto *cap = std::get_if<EName>(&fe->base->node);
    if (cap != nullptr && capacities_.contains(cap->name)) {
      return capacityCall(cap->name, fe->name, c.args, out, loc);
    }
  }
  const auto *callee = std::get_if<EName>(&c.callee->node);
  if (callee == nullptr) unsupported("a call on a non-name", loc);
  if (callee->name == "text") {
    if (c.args.size() != 1)
      bail(CompileError::BadArity, "text takes one argument", loc);
    const CellRef v = reg();
    const Ty t      = expr(*c.args[0], v);
    if (t.k == Ty::K::Int || t.k == Ty::K::Float || t.k == Ty::K::Bool) {
      emit(OpcodeKind::Trim, "text", {S(v)}, {out});
    } else if (t.k == Ty::K::Str) {
      mov(S(out), S(v));
    } else {
      unsupported("text() of this type", loc);
    }
    return tyOf(Ty::K::Str);
  }
  // String scanning built-ins (§10).
  if (callee->name == "pos") {
    if (c.args.empty()) {
      mov(S(out), S(scanPosG_));
      return tyOf(Ty::K::Int);
    }
    if (c.args.size() == 1) {
      const CellRef target = reg(), eq = reg(), ok = newLabel();
      expr(*c.args[0], target);
      emit(OpcodeKind::Eq, "pos?", {S(scanPosG_), S(target)}, {eq});
      branch(S(eq), ok);
      failNow();
      place(ok);
      mov(S(out), S(scanPosG_));
      return tyOf(Ty::K::Int);
    }
    bail(CompileError::BadArity, "pos takes 0 or 1 argument", loc);
  }
  if (callee->name == "tab" || callee->name == "move") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, callee->name + " takes 1 argument", loc);
    }
    const CellRef arg = reg(), newPos = reg(), oldPos = reg(), lo = reg(),
                  hi = reg(), len = reg(), bad = reg();
    const CellRef ok1 = newLabel(), ok2 = newLabel(), fwd = newLabel(),
                  haveBounds = newLabel(), emptySub = newLabel(),
                  done = newLabel();
    expr(*c.args[0], arg);
    mov(S(oldPos), S(scanPosG_));
    if (callee->name == "move") {
      emit(OpcodeKind::Add, "mv+", {S(oldPos), S(arg)}, {newPos});
    } else {
      mov(S(newPos), S(arg));
    }
    emit(OpcodeKind::Lt, "p<0", {S(newPos), S(zero_)}, {bad});
    branchIfZero(bad, ok1);
    failNow();
    place(ok1);
    emit(OpcodeKind::Gt, "p>len", {S(newPos), S(scanLenG_)}, {bad});
    branchIfZero(bad, ok2);
    failNow();
    place(ok2);
    mov(S(scanPosG_), S(newPos));
    emit(OpcodeKind::Lte, "o<=n", {S(oldPos), S(newPos)}, {bad});
    branch(S(bad), fwd);
    mov(S(lo), S(newPos));
    mov(S(hi), S(oldPos));
    jump(haveBounds);
    place(fwd);
    mov(S(lo), S(oldPos));
    mov(S(hi), S(newPos));
    place(haveBounds);
    emit(OpcodeKind::Sub, "len", {S(hi), S(lo)}, {len});
    branchIfZero(len, emptySub);
    emit(OpcodeKind::Value, "sub", {S(scanSubjG_), S(lo), S(len)}, {out});
    jump(done);
    place(emptySub);
    movC(out, cStr(""));
    place(done);
    return tyOf(Ty::K::Str);
  }
  if (callee->name == "match") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "match takes 1 argument", loc);
    }
    const CellRef pref = reg(), plen = reg(), endPos = reg(), bad = reg(),
                  sub      = reg();
    const CellRef inBounds = newLabel(), emptyPref = newLabel(),
                  checkSub = newLabel(), matched = newLabel();
    expr(*c.args[0], pref);
    callStrLen(pref, plen);
    emit(OpcodeKind::Add, "end", {S(scanPosG_), S(plen)}, {endPos});
    emit(OpcodeKind::Gt, "end>len", {S(endPos), S(scanLenG_)}, {bad});
    branchIfZero(bad, inBounds);
    failNow();
    place(inBounds);
    branchIfZero(plen, emptyPref);
    emit(OpcodeKind::Value, "sub", {S(scanSubjG_), S(scanPosG_), S(plen)},
         {sub});
    jump(checkSub);
    place(emptyPref);
    movC(sub, cStr(""));
    place(checkSub);
    emit(OpcodeKind::Eq, "m?", {S(sub), S(pref)}, {bad});
    branch(S(bad), matched);
    failNow();
    place(matched);
    mov(S(out), S(endPos));
    return tyOf(Ty::K::Int);
  }
  if (callee->name == "upto") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "upto takes 1 argument", loc);
    }
    const CellRef cset = reg(), clen = reg(), i = reg(), j = reg(), ch = reg(),
                  cch = reg(), cmp = reg();
    const CellRef oloop = newLabel(), iloop = newLabel(), inext = newLabel(),
                  onext = newLabel(), found = newLabel(), done = newLabel(),
                  after = newLabel();
    expr(*c.args[0], cset);
    callStrLen(cset, clen);
    mov(S(i), S(scanPosG_));
    place(oloop);
    emitCondTick();
    emit(OpcodeKind::Gte, "i>=len", {S(i), S(scanLenG_)}, {cmp});
    branch(S(cmp), done);
    emit(OpcodeKind::Value, "ch", {S(scanSubjG_), S(i), S(one_)}, {ch});
    movC(j, zero_);
    place(iloop);
    emit(OpcodeKind::Gte, "j>=clen", {S(j), S(clen)}, {cmp});
    branch(S(cmp), onext);
    emit(OpcodeKind::Value, "cch", {S(cset), S(j), S(one_)}, {cch});
    emit(OpcodeKind::Eq, "hit?", {S(ch), S(cch)}, {cmp});
    branch(S(cmp), found);
    place(inext);
    emit(OpcodeKind::Add, "j+", {S(j), S(one_)}, {j});
    jump(iloop);
    place(found);
    emit(OpcodeKind::Choice, "upto", {S(opref(onext))});
    mov(S(out), S(i));
    jump(after);
    place(onext);
    emit(OpcodeKind::Add, "i+", {S(i), S(one_)}, {i});
    jump(oloop);
    place(done);
    failNow();
    place(after);
    return tyOf(Ty::K::Int);
  }
  if (callee->name == "find") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "find takes 1 argument", loc);
    }
    const CellRef sub = reg(), slen = reg(), i = reg(), endPos = reg(),
                  cand = reg(), cmp = reg();
    const CellRef loop = newLabel(), emptySub = newLabel(),
                  checkCand = newLabel(), hit = newLabel(), next = newLabel(),
                  done = newLabel(), after = newLabel();
    expr(*c.args[0], sub);
    callStrLen(sub, slen);
    mov(S(i), S(scanPosG_));
    place(loop);
    emitCondTick();
    emit(OpcodeKind::Add, "end", {S(i), S(slen)}, {endPos});
    emit(OpcodeKind::Gt, "end>len", {S(endPos), S(scanLenG_)}, {cmp});
    branch(S(cmp), done);
    branchIfZero(slen, emptySub);
    emit(OpcodeKind::Value, "cand", {S(scanSubjG_), S(i), S(slen)}, {cand});
    jump(checkCand);
    place(emptySub);
    movC(cand, cStr(""));
    place(checkCand);
    emit(OpcodeKind::Eq, "hit?", {S(cand), S(sub)}, {cmp});
    branch(S(cmp), hit);
    jump(next);
    place(hit);
    emit(OpcodeKind::Choice, "find", {S(opref(next))});
    mov(S(out), S(i));
    jump(after);
    place(next);
    emit(OpcodeKind::Add, "i+", {S(i), S(one_)}, {i});
    jump(loop);
    place(done);
    failNow();
    place(after);
    return tyOf(Ty::K::Int);
  }
  // `std:agent` built-in functions (§10).
  if (callee->name == "intentions") {
    if (!c.args.empty()) {
      bail(CompileError::BadArity, "intentions takes 0 arguments", loc);
    }
    const CellRef res = reg(), ic = reg();
    const CellRef loop = newLabel(), next = newLabel(), done = newLabel();
    alloc(res);
    mov(D(res), S(cStr(std::string(kListContent))));
    mov(S(ic), S(rd(S(agentCell_), dims_.intention)));
    place(loop);
    branchIfZero(ic, done);
    CellRef idVp, idNp;
    callFind(ic, std::string(kIntentionId), idVp, idNp);
    branchIfZero(idVp, next);
    const CellRef cell = reg();
    alloc(cell);
    mov(D(cell), D(idVp));
    callListPush(res, cell);
    place(next);
    mov(S(ic), S(rd(D(ic), dims_.intention)));
    jump(loop);
    place(done);
    mov(S(out), S(res));
    Ty t = tyOf(Ty::K::List);
    t.args.push_back(tyOf(Ty::K::Int));
    return t;
  }
  if (callee->name == "is_maintain") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "is_maintain takes 1 argument", loc);
    }
    const CellRef id = reg(), ic = reg(), eq = reg();
    const CellRef ok = newLabel(), bad = newLabel();
    expr(*c.args[0], id);
    mov(S(a0_), S(id));
    intentionRuntime("FindIntention");
    mov(S(ic), S(r0_));
    branchIfZero(ic, bad);
    CellRef kindVp, kindNp;
    callFind(ic, std::string(kIntentionKind), kindVp, kindNp);
    branchIfZero(kindVp, bad);
    emit(OpcodeKind::Eq, "maint?",
         {D(kindVp), S(cStr(std::string(kGoalMaintain)))}, {eq});
    branch(S(eq), ok);
    place(bad);
    failNow();
    place(ok);
    movC(out, unit_c_);
    return tyOf(Ty::K::Unit);
  }
  if (callee->name == "restorers") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "restorers takes 1 argument", loc);
    }
    const CellRef id = reg(), ic = reg(), sub = reg(), res = reg();
    const CellRef done = newLabel();
    expr(*c.args[0], id);
    alloc(res);
    mov(D(res), S(cStr(std::string(kListContent))));
    mov(S(a0_), S(id));
    intentionRuntime("FindIntention");
    mov(S(ic), S(r0_));
    branchIfZero(ic, done);
    mov(S(sub), S(rd(D(ic), dims_.subgoal)));
    branchIfZero(sub, done);
    CellRef subIdVp, subIdNp;
    callFind(sub, std::string(kIntentionId), subIdVp, subIdNp);
    branchIfZero(subIdVp, done);
    const CellRef cell = reg();
    alloc(cell);
    mov(D(cell), D(subIdVp));
    callListPush(res, cell);
    place(done);
    mov(S(out), S(res));
    Ty t = tyOf(Ty::K::List);
    t.args.push_back(tyOf(Ty::K::Int));
    return t;
  }
  if (callee->name == "children") {
    if (!c.args.empty()) {
      bail(CompileError::BadArity, "children takes 0 arguments", loc);
    }
    const CellRef res = reg(), ch = reg();
    const CellRef loop = newLabel(), done = newLabel();
    alloc(res);
    mov(D(res), S(cStr(std::string(kListContent))));
    mov(S(ch), S(rd(S(agentCell_), dims_.holon)));
    place(loop);
    branchIfZero(ch, done);
    const CellRef cell = reg();
    alloc(cell);
    mov(D(cell), D(ch));
    callListPush(res, cell);
    mov(S(ch), S(rd(D(ch), dims_.holon)));
    jump(loop);
    place(done);
    mov(S(out), S(res));
    Ty t = tyOf(Ty::K::List);
    t.args.push_back(tyOf(Ty::K::Str));
    return t;
  }
  if (callee->name == "members") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "members takes 1 argument", loc);
    }
    const CellRef spName = reg(), res = reg(), sp = reg(), eq = reg(),
                  mb    = reg();
    const CellRef sloop = newLabel(), sfound = newLabel(), mloop = newLabel(),
                  done = newLabel();
    expr(*c.args[0], spName);
    alloc(res);
    mov(D(res), S(cStr(std::string(kListContent))));
    mov(S(sp), S(rd(S(agentCell_), dims_.space)));
    place(sloop);
    branchIfZero(sp, done);
    emit(OpcodeKind::Eq, "sp?", {D(sp), S(spName)}, {eq});
    branch(S(eq), sfound);
    mov(S(sp), S(rd(D(sp), dims_.space)));
    jump(sloop);
    place(sfound);
    mov(S(mb), S(rd(D(sp), dims_.member)));
    place(mloop);
    branchIfZero(mb, done);
    const CellRef cell = reg();
    alloc(cell);
    mov(D(cell), D(mb));
    callListPush(res, cell);
    mov(S(mb), S(rd(D(mb), dims_.member)));
    jump(mloop);
    place(done);
    mov(S(out), S(res));
    Ty t = tyOf(Ty::K::List);
    t.args.push_back(tyOf(Ty::K::Str));
    return t;
  }
  if (callee->name == "dropped_field") {
    if (c.args.size() != 1) {
      bail(CompileError::BadArity, "dropped_field takes 1 argument", loc);
    }
    const CellRef fname = reg();
    expr(*c.args[0], fname);
    if (own_ != nullptr) {
      const CellRef qualified = reg();
      emit(OpcodeKind::Add, "dropped-owner",
           {S(cStr(own_->name + ".")), S(fname)}, {qualified});
      mov(S(fname), S(qualified));
    }
    const CellRef ag = reg();
    loadRef(ag, agentCell_);
    CellRef vp, np;
    callFind(ag, std::string(kFieldDroppedFields), vp, np);
    const CellRef noDropped = newLabel(), done = newLabel();
    branchIfZero(vp, noDropped);
    mov(S(a0_), S(vp));
    mov(S(a1_), S(fname));
    callLabel(rtFind_);
    branchIfZero(r0_, noDropped);
    // r0 names the saved value cell; the field's text is its content.
    mov(S(out), D(r0_));
    jump(done);
    place(noDropped);
    movC(out, cStr(""));
    place(done);
    return tyOf(Ty::K::Str);
  }
  MethodInfo *own = findMethod(callee->name);
  if (own == nullptr) {
    if (const auto cap = capacityOfMethod(callee->name)) {
      return capacityCall(*cap, callee->name, c.args, out, loc);
    }
    bail(CompileError::UnknownName, "unknown method " + callee->name, loc);
  }
  MethodInfo &mi = *own;
  if (c.args.size() != mi.paramTys.size()) {
    bail(CompileError::BadArity,
         callee->name + " takes " + std::to_string(mi.paramTys.size()) +
             " arguments",
         loc);
  }
  std::vector<CellRef> temps;
  for (const auto &a : c.args) {
    const CellRef t = reg();
    valueInto(*a, t);
    temps.push_back(t);
  }
  for (std::size_t i = 0; i < temps.size(); ++i) {
    mov(S(mi.argRegs[i]), S(temps[i]));
  }
  callUser(mi.entry);
  mov(S(out), S(mi.retReg));
  return mi.ret;
}

// ---------------------------------------------------------------------------
// Built-in capacities: Schedules, Clock, Logging, Accepts (design §10)
//
// Vortex cannot call C++, so `Schedules` and `Logging` stage outbox entries
// the adapter applies at the drain (§4.2 step 9: "timer changes"), and `Clock`
// reads the `time` field of `$turn` (§4.2 step 3). A Timer is the scalar id
// minted here from `$next_id`, so the call can return it before the drain.
// ---------------------------------------------------------------------------

std::optional<Ty> Gen::capacityCall(const ECall &c, CellRef out,
                                    SourceLocation loc) {
  const auto *callee = std::get_if<EField>(&c.callee->node);
  if (callee == nullptr) return std::nullopt;
  const auto *owner = std::get_if<EName>(&callee->base->node);
  if (owner == nullptr) return std::nullopt;
  const std::string &cap = owner->name;
  const std::string &fn  = callee->name;
  const auto arity       = [&](std::size_t n) {
    if (c.args.size() != n) {
      bail(CompileError::BadArity,
                 cap + "." + fn + " takes " + std::to_string(n) + " arguments", loc);
    }
  };
  CellRef vc;
  if (cap == "Clock" && fn == "now") {
    arity(0);
    const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
    const CellRef tp      = reg();
    CellRef timeP, nameP;
    mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
    callFind(tp, std::string(kTurnTime), timeP, nameP);
    branchIfZero(timeP, failStep_);
    mov(S(out), D(timeP));
    return tyOf(Ty::K::Int);
  }
  if (cap == "Schedules" && (fn == "after" || fn == "periodic")) {
    arity(2);
    const CellRef delay = reg(), ev = reg(), id = reg();
    valueInto(*c.args[0], delay);
    const Ty et = valueInto(*c.args[1], ev);
    if (et.k != Ty::K::Record) {
      bail(CompileError::TypeMismatch, cap + "." + fn + " needs an event", loc);
    }
    const FieldInfo &next = fields_.at(std::string(kFieldNextId));
    mov(S(id), S(next.valueCell));
    emit(OpcodeKind::Add, "id+", {S(next.valueCell), S(one_)},
         {next.valueCell});
    const CellRef entry = callOutbox(kKindTimer);
    callAppendField(entry, std::string(kOutboxTimerId), std::nullopt, vc);
    mov(D(vc), S(id));
    callAppendField(entry, std::string(kOutboxTimerDelay), std::nullopt, vc);
    mov(D(vc), S(delay));
    callAppendField(entry, std::string(kOutboxTimerPeriod), std::nullopt, vc);
    mov(D(vc), fn == "periodic" ? S(delay) : S(zero_));
    callAppendField(entry, std::string(kOutboxEvent), ev, vc);
    mov(S(out), S(id));
    return tyOf(Ty::K::Int);
  }
  if (cap == "Schedules" && fn == "cancel") {
    arity(1);
    const CellRef id = reg();
    valueInto(*c.args[0], id);
    const CellRef entry = callOutbox(kKindTimerCancel);
    callAppendField(entry, std::string(kOutboxTimerId), std::nullopt, vc);
    mov(D(vc), S(id));
    movC(out, unit_c_);
    return tyOf(Ty::K::Unit);
  }
  if (cap == "Logging" && (fn == "info" || fn == "debug")) {
    arity(1);
    const CellRef msg = reg();
    valueInto(*c.args[0], msg);
    const CellRef entry = callOutbox(kKindLog);
    callAppendField(entry, std::string(kOutboxLogLevel), std::nullopt, vc);
    mov(D(vc),
        S(cStr(std::string(fn == "info" ? kLogLevelInfo : kLogLevelDebug))));
    callAppendField(entry, std::string(kOutboxLogMessage), std::nullopt, vc);
    mov(D(vc), S(msg));
    movC(out, unit_c_);
    return tyOf(Ty::K::Unit);
  }
  return std::nullopt;
}

Ty Gen::index(const EIndex &i, CellRef out) {
  const CellRef base = reg();
  const Ty bt        = expr(*i.base, base);
  if (bt.k == Ty::K::Str) {
    if (const auto *r = std::get_if<ERange>(&i.index->node)) {
      const CellRef lo = reg(), hi = reg(), slen = reg(), len = reg(),
                    bad = reg();
      const CellRef ok1 = newLabel(), ok2 = newLabel(), ok3 = newLabel(),
                    emptySlice = newLabel(), done = newLabel();
      expr(*r->lo, lo);
      expr(*r->hi, hi);
      callStrLen(base, slen);
      emit(OpcodeKind::Lt, "lo<0", {S(lo), S(zero_)}, {bad});
      branchIfZero(bad, ok1);
      jump(failStep_);
      place(ok1);
      emit(OpcodeKind::Gt, "lo>hi", {S(lo), S(hi)}, {bad});
      branchIfZero(bad, ok2);
      jump(failStep_);
      place(ok2);
      emit(OpcodeKind::Gt, "hi>len", {S(hi), S(slen)}, {bad});
      branchIfZero(bad, ok3);
      jump(failStep_);
      place(ok3);
      emit(OpcodeKind::Sub, "len", {S(hi), S(lo)}, {len});
      branchIfZero(len, emptySlice);
      emit(OpcodeKind::Value, "slice", {S(base), S(lo), S(len)}, {out});
      jump(done);
      place(emptySlice);
      movC(out, cStr(""));
      place(done);
      return tyOf(Ty::K::Str);
    }
    const CellRef idx = reg(), bad = reg();
    const CellRef ok1 = newLabel(), ok2 = newLabel();
    expr(*i.index, idx);
    emit(OpcodeKind::Lt, "idx<0", {S(idx), S(zero_)}, {bad});
    branchIfZero(bad, ok1);
    jump(failStep_);
    place(ok1);
    emit(OpcodeKind::Value, "ch", {S(base), S(idx), S(one_)}, {out});
    emit(OpcodeKind::Eq, "oob?", {S(out), S(cStr(""))}, {bad});
    branchIfZero(bad, ok2);
    jump(failStep_);
    place(ok2);
    return tyOf(Ty::K::Str);
  }
  const CellRef idx = reg(), cnt = reg(), el = reg(), eq = reg();
  expr(*i.index, idx);
  if (bt.k != Ty::K::List) unsupported("indexing a non-list", i.base->loc);
  const Ty et        = bt.args.empty() ? tyOf(Ty::K::Unit) : bt.args[0];
  const CellRef loop = newLabel(), found = newLabel();
  movC(cnt, zero_);
  mov(S(el), S(rd(D(base), core_.dims().grab)));
  place(loop);
  branchIfZero(el, failStep_);
  emit(OpcodeKind::Eq, "ix", {S(cnt), S(idx)}, {eq});
  branch(S(eq), found);
  mov(S(el), S(rd(D(el), core_.dims().step)));
  emit(OpcodeKind::Add, "n+", {S(cnt), S(one_)}, {cnt});
  jump(loop);
  place(found);
  if (et.composite()) {
    mov(S(out), S(el));
  } else {
    mov(S(out), D(el));
  }
  return et;
}

Ty Gen::field(const EField &f, CellRef out, SourceLocation loc) {
  const CellRef base = reg();
  const Ty bt        = expr(*f.base, base);
  if (bt.k != Ty::K::Record) {
    bail(CompileError::TypeMismatch, "field access on a non-record", loc);
  }
  const auto ft = fieldTy(bt.name, f.name);
  if (!ft) {
    bail(CompileError::UnknownName, "no field " + f.name + " in " + bt.name,
         loc);
  }
  CellRef vp, np;
  callFind(base, f.name, vp, np);
  branchIfZero(vp, failStep_);
  if (ft->composite()) {
    mov(S(out), S(vp));
  } else {
    mov(S(out), D(vp));
  }
  return ft->k == Ty::K::Opt ? unwrapOpt(out, *ft, out, loc) : *ft;
}

Ty Gen::list(const EList &l, CellRef out) {
  const CellRef res = reg();
  alloc(res);
  mov(D(res), S(cStr(std::string(kListContent))));
  Ty t = tyOf(Ty::K::List);
  for (const auto &el : l.elems) {
    const CellRef v = reg();
    const Ty et     = valueInto(*el, v);
    if (t.args.empty()) t.args.push_back(et);
    CellRef cell = v;
    if (!et.composite()) {
      cell = reg();
      alloc(cell);
      mov(D(cell), S(v));
    }
    callListPush(res, cell);
  }
  mov(S(out), S(res));
  return t;
}

Ty Gen::record(const ERecord &r, CellRef out, SourceLocation loc) {
  if (!events_.contains(r.type) && !records_.contains(r.type)) {
    bail(CompileError::UnknownType, "unknown record or event " + r.type, loc);
  }
  const auto decl   = fieldsOf(r.type);
  const CellRef res = reg();
  alloc(res);
  mov(D(res), S(cStr(r.type)));
  markRecord(res);
  for (const auto &rf : r.fields) {
    const auto it = std::ranges::find_if(
        decl, [&](const auto &p) { return p.first == rf.name; });
    if (it == decl.end()) {
      bail(CompileError::UnknownName, "no field " + rf.name + " in " + r.type,
           loc);
    }
    const CellRef v = reg();
    const Ty vt     = valueInto(*rf.value, v);
    coerceInto(it->second, vt, v);
    CellRef vc;
    if (it->second.composite()) {
      callAppendField(res, rf.name, v, vc);
    } else {
      callAppendField(res, rf.name, std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  // A field that may be empty and was left out is `none` (§8.3).
  for (const auto &[fname, fty] : decl) {
    const bool given = std::ranges::any_of(
        r.fields, [&](const auto &rf) { return rf.name == fname; });
    const bool implicit =
        std::ranges::any_of(preludeImplicitFields(),
                            [&](const auto &f) { return f.name == fname; });
    if (given || implicit || fty.k != Ty::K::Opt) continue;
    const CellRef none = reg();
    alloc(none);
    mov(D(none), S(cStr(std::string(kListContent))));
    CellRef vc;
    callAppendField(res, fname, none, vc);
  }
  mov(S(out), S(res));
  Ty t   = tyOf(Ty::K::Record);
  t.name = r.type;
  return t;
}

static bool isCompoundStmt(const Expr &e) {
  return std::holds_alternative<EBlock>(e.node) ||
         std::holds_alternative<EIf>(e.node) ||
         std::holds_alternative<EWhile>(e.node) ||
         std::holds_alternative<EFor>(e.node);
}

Ty Gen::block(const EBlock &b, CellRef out) {
  pushScope();
  Ty last = tyOf(Ty::K::Unit);
  if (b.stmts.empty()) movC(out, unit_c_);
  for (std::size_t i = 0; i < b.stmts.size(); ++i) {
    const Stmt &s     = b.stmts[i];
    const bool isLast = i + 1 == b.stmts.size();
    if (s.isLet) {
      const CellRef tmp = reg();
      Ty t;
      stmtWrap([&] { t = valueInto(*s.value, tmp); });
      if (s.type) {
        const Ty declTy = fromType(*s.type, s.loc);
        coerceInto(declTy, t, tmp);
        t = declTy;
      }
      const CellRef r = declareLocal(s.name, t);
      mov(S(r), S(tmp));
      scopes_.back()[s.name] = Local{r, t};
      if (isLast) movC(out, unit_c_);
      last = tyOf(Ty::K::Unit);
    } else {
      const CellRef target = isLast ? out : reg();
      if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*s.value)) {
        last = expr(*s.value, target);
      } else {
        stmtWrap([&] { last = expr(*s.value, target); });
      }
    }
  }
  popScope();
  return last;
}

Ty Gen::ifExpr(const EIf &i, CellRef out) {
  const CellRef elseL = newLabel(), end = newLabel();
  const std::size_t curDepth = stmtDepth_;
  const CellRef d            = dRegAt(curDepth);
  ++stmtDepth_;
  emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
  if (ctx_.inPlan && curDepth == 0) {
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  }
  emit(OpcodeKind::Choice, "else", {S(opref(elseL))});
  const CellRef scratch = reg();
  expr(*i.cond, scratch);
  emit(OpcodeKind::Cut, "cut", {S(d)});
  --stmtDepth_;
  Ty t;
  if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*i.thenBranch)) {
    t = expr(*i.thenBranch, out);
  } else {
    stmtWrap([&] { t = expr(*i.thenBranch, out); });
  }
  jump(end);
  place(elseL);
  if (ctx_.inPlan && curDepth == 0) {
    emit(OpcodeKind::Cut, "cut", {S(d)});
  }
  if (i.elseBranch) {
    if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*i.elseBranch)) {
      expr(*i.elseBranch, out);
    } else {
      stmtWrap([&] { expr(*i.elseBranch, out); });
    }
  } else {
    movC(out, unit_c_);
    t = tyOf(Ty::K::Unit);
  }
  place(end);
  return t;
}

Ty Gen::whileExpr(const EWhile &w, CellRef out) {
  const CellRef loop = newLabel(), done = newLabel();
  place(loop);
  emitCondTick();
  const std::size_t curDepth = stmtDepth_;
  const CellRef d            = dRegAt(curDepth);
  ++stmtDepth_;
  emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
  if (ctx_.inPlan && curDepth == 0) {
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  }
  emit(OpcodeKind::Choice, "done", {S(opref(done))});
  const CellRef scratch = reg();
  expr(*w.cond, scratch);
  emit(OpcodeKind::Cut, "cut", {S(d)});
  --stmtDepth_;
  const CellRef bodyOut = reg();
  if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*w.body)) {
    expr(*w.body, bodyOut);
  } else {
    stmtWrap([&] { expr(*w.body, bodyOut); });
  }
  jump(loop);
  place(done);
  if (ctx_.inPlan && curDepth == 0) {
    emit(OpcodeKind::Cut, "cut", {S(d)});
  }
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::forExpr(const EFor &f, CellRef out, SourceLocation loc) {
  if (!f.iterable) return forBelieves(f, out);
  pushScope();
  const CellRef bodyOut = reg();
  const CellRef loop = newLabel(), done = newLabel();
  if (const auto *r = std::get_if<ERange>(&f.iterable->node)) {
    const CellRef x  = declareLocal(f.binder, tyOf(Ty::K::Int));
    const CellRef lo = reg(), hi = reg(), by = reg(), cur = reg();
    stmtWrap([&] {
      expr(*r->lo, lo);
      expr(*r->hi, hi);
      if (r->by) {
        expr(*r->by, by);
      } else {
        movC(by, one_);
      }
    });
    mov(S(cur), S(lo));
    scopes_.back()[f.binder] = Local{x, tyOf(Ty::K::Int)};
    place(loop);
    emitCondTick();
    rangeTest(cur, hi, S(by), done);
    mov(S(x), S(cur));
    if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*f.body)) {
      expr(*f.body, bodyOut);
    } else {
      stmtWrap([&] { expr(*f.body, bodyOut); });
    }
    emit(OpcodeKind::Add, "cur+", {S(cur), S(by)}, {cur});
    jump(loop);
    place(done);
  } else {
    const CellRef l = reg();
    Ty lt;
    stmtWrap([&] { lt = expr(*f.iterable, l); });
    if (lt.k != Ty::K::List) unsupported("iterating a non-list", loc);
    const Ty et              = lt.args.empty() ? tyOf(Ty::K::Unit) : lt.args[0];
    const CellRef x          = declareLocal(f.binder, et);
    scopes_.back()[f.binder] = Local{x, et};
    listWalk(l, [&](CellRef el) {
      emitCondTick();
      if (et.composite()) {
        // A loop variable is a copy, like every stored value (V11): its record
        // hangs on the frame, and the list's element is already owned.
        const CellRef own = reg();
        callCopy(el, own);
        assignRef(x, own, true);
      } else {
        mov(S(x), D(el));
      }
      if (ctx_.inPlan && stmtDepth_ == 0 && isCompoundStmt(*f.body)) {
        expr(*f.body, bodyOut);
      } else {
        stmtWrap([&] { expr(*f.body, bodyOut); });
      }
    });
  }
  movC(out, unit_c_);
  popScope();
  return tyOf(Ty::K::Unit);
}

// ---------------------------------------------------------------------------
// Goals, plans, intentions, and suspension points (§6.2–§6.6)
// ---------------------------------------------------------------------------

CellRef Gen::buildGoalArgs(const GoalInfo &gi, const std::vector<ExprPtr> &args,
                           SourceLocation loc) {
  if (args.size() != gi.params.size()) {
    bail(CompileError::BadArity,
         gi.name + " takes " + std::to_string(gi.params.size()) + " arguments",
         loc);
  }
  const CellRef rec = reg();
  alloc(rec);
  mov(D(rec), S(cStr(gi.name)));
  markRecord(rec);
  for (std::size_t i = 0; i < gi.params.size(); ++i) {
    const CellRef v = reg();
    const Ty vt     = valueInto(*args[i], v);
    coerceInto(gi.params[i].second, vt, v);
    CellRef vc;
    if (gi.params[i].second.composite()) {
      callAppendField(rec, gi.params[i].first, v, vc);
    } else {
      callAppendField(rec, gi.params[i].first, std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  return rec;
}

CellRef Gen::mintIntentionCell(const GoalInfo &gi, CellRef argsRec,
                               bool setReady) {
  const FieldInfo &next = fields_.at(std::string(kFieldNextId));
  const FieldInfo &turn = fields_.at(std::string(kFieldTurnCount));
  const CellRef ic = reg(), id = reg(), tried = reg();
  mov(S(id), S(next.valueCell));
  emit(OpcodeKind::Add, "id+", {S(next.valueCell), S(one_)}, {next.valueCell});
  alloc(ic);
  mov(D(ic), S(cStr(gi.name)));
  markRecord(ic);
  CellRef vc;
  callAppendField(ic, std::string(kIntentionId), std::nullopt, vc);
  mov(D(vc), S(id));
  callAppendField(ic, std::string(kIntentionGoal), std::nullopt, vc);
  mov(D(vc), S(cStr(gi.name)));
  std::string_view kindStr = kGoalAchieve;
  if (gi.kind == GoalDecl::Kind::Perform) kindStr = kGoalPerform;
  if (gi.kind == GoalDecl::Kind::Maintain) kindStr = kGoalMaintain;
  callAppendField(ic, std::string(kIntentionKind), std::nullopt, vc);
  mov(D(vc), S(cStr(std::string(kindStr))));
  callAppendField(ic, std::string(kIntentionArgs), argsRec, vc);
  callAppendField(ic, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(zero_));
  alloc(tried);
  mov(D(tried), S(cStr(std::string(kListContent))));
  callAppendField(ic, std::string(kIntentionTried), tried, vc);
  if (setReady) {
    callAppendField(ic, std::string(kIntentionReady), std::nullopt, vc);
    mov(D(vc), S(turn.valueCell));
  }
  return ic;
}

Ty Gen::adoptExpr(const EAdopt &a, CellRef out, SourceLocation loc) {
  const auto it = goals_.find(a.goal.name);
  if (it == goals_.end()) {
    bail(CompileError::UnknownName, "unknown goal " + a.goal.name, loc);
  }
  const GoalInfo &gi    = it->second;
  const CellRef argsRec = buildGoalArgs(gi, a.goal.args, loc);
  const bool ready      = gi.kind != GoalDecl::Kind::Maintain;
  const CellRef ic      = mintIntentionCell(gi, argsRec, ready);
  emitAppendTail(S(agentCell_), dims_.intention, dims_.intention, ic);
  CellRef idVp, idNp;
  callFind(ic, std::string(kIntentionId), idVp, idNp);
  mov(S(out), D(idVp));
  return tyOf(Ty::K::Int);
}

Ty Gen::dropExpr(const EDrop &d, CellRef out, SourceLocation loc) {
  (void)loc;
  const CellRef id = reg(), ic = reg();
  const CellRef done = newLabel();
  expr(*d.target, id);
  mov(S(a0_), S(id));
  intentionRuntime("FindIntention");
  mov(S(ic), S(r0_));
  branchIfZero(ic, done);
  mov(S(a0_), S(ic));
  movC(a1_, cStr(std::string(kGoalDropped)));
  movC(a2_, cStr(std::string(kCauseDropped)));
  intentionRuntime("EndIntention");
  place(done);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::yieldExpr(const EYield &y, CellRef out, SourceLocation loc) {
  (void)y;
  if (!ctx_.inPlan || ctx_.planInfo == nullptr) {
    bail(CompileError::Unsupported, "yield outside a plan", loc);
  }
  PlanInfo &pi              = *ctx_.planInfo;
  const std::size_t stepOrd = pi.stepEntries.size();
  const CellRef resumeLabel = newLabel();
  pi.stepEntries.push_back(resumeLabel);
  CellRef vc;
  callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
  const FieldInfo &turn = fields_.at(std::string(kFieldTurnCount));
  callSetField(curIntention_, std::string(kIntentionReady), std::nullopt, vc);
  mov(D(vc), S(turn.valueCell));
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);

  place(resumeLabel);
  emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
  emit(OpcodeKind::Choice, "pcontract",
       {S(opref(planLabel(ctx_.planContractFail)))});
  emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::achieveExpr(const EAchieve &a, CellRef out, SourceLocation loc) {
  if (!ctx_.inPlan || ctx_.planInfo == nullptr) {
    bail(CompileError::Unsupported, "achieve outside a plan", loc);
  }
  PlanInfo &pi              = *ctx_.planInfo;
  const std::size_t stepOrd = pi.stepEntries.size();
  const CellRef resumeLabel = newLabel();
  pi.stepEntries.push_back(resumeLabel);

  if (!a.by) {
    // Subgoal on `d.subgoal` (§6.6).
    const auto it = goals_.find(a.goal.name);
    if (it == goals_.end()) {
      bail(CompileError::UnknownName, "unknown goal " + a.goal.name, loc);
    }
    const GoalInfo &gi = it->second;
    if (gi.kind == GoalDecl::Kind::Maintain) {
      bail(CompileError::TypeMismatch, "cannot achieve a maintain goal", loc);
    }
    const CellRef argsRec = buildGoalArgs(gi, a.goal.args, loc);
    const CellRef subIc   = mintIntentionCell(gi, argsRec, true);
    linkOp(D(curIntention_), dims_.subgoal, D(subIc));
    CellRef subIdVp, subIdNp, vc;
    callFind(subIc, std::string(kIntentionId), subIdVp, subIdNp);
    const CellRef waitRec = reg();
    alloc(waitRec);
    mov(D(waitRec), S(cStr("Waiting")));
    markRecord(waitRec);
    callAppendField(waitRec, std::string(kWaitingKind), std::nullopt, vc);
    mov(D(vc), S(cStr(std::string(kWaitKindSubgoal))));
    callAppendField(waitRec, std::string(kWaitingKey), std::nullopt, vc);
    mov(D(vc), D(subIdVp));
    callSetField(curIntention_, std::string(kIntentionWaiting), waitRec, vc);
    callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
    mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
    callClearField(curIntention_, std::string(kIntentionReady));
    emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
    emitFramePop();
    callLabel(rtSettle_);
    jump(haltOp_);

    place(resumeLabel);
    emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
    emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
    CellRef wVp, wNp, mVp, mNp, okVp, okNp;
    callFind(curIntention_, std::string(kIntentionWaiting), wVp, wNp);
    callClearField(curIntention_, std::string(kIntentionWaiting));
    const CellRef subFailed = newLabel(), subDone = newLabel(),
                  subOk = newLabel();
    branchIfZero(wVp, subFailed);
    callFind(wVp, std::string(kWaitingMatch), mVp, mNp);
    branchIfZero(mVp, subFailed);
    callFind(mVp, std::string(kEffectOutcomeOk), okVp, okNp);
    branchIfZero(okVp, subFailed);
    branch(D(okVp), subOk);
    jump(subFailed);
    place(subOk);
    movC(out, unit_c_);
    jump(subDone);
    place(subFailed);
    if (a.orElse) {
      emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
      emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
      emit(OpcodeKind::Choice, "pcontract",
           {S(opref(planLabel(ctx_.planContractFail)))});
      emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
      emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
      expr(*a.orElse, out);
    } else {
      failNow();
    }
    place(subDone);
    return tyOf(Ty::K::Unit);
  }

  // Delegated `achieve G(args) by target within d [else e]` (§7.4).
  const auto it = goals_.find(a.goal.name);
  if (it == goals_.end()) {
    bail(CompileError::UnknownName, "unknown goal " + a.goal.name, loc);
  }
  const GoalInfo &gi  = it->second;
  const CellRef toReg = reg(), durReg = reg();
  expr(*a.by, toReg);
  const CellRef argsRec = buildGoalArgs(gi, a.goal.args, loc);
  expr(*a.within, durReg);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), nowReg = reg(), deadline = reg();
  CellRef timeP, nameP;
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  callFind(tp, std::string(kTurnTime), timeP, nameP);
  movC(nowReg, zero_);
  const CellRef noTime = newLabel();
  branchIfZero(timeP, noTime);
  mov(S(nowReg), D(timeP));
  place(noTime);
  emit(OpcodeKind::Add, "dl", {S(nowReg), S(durReg)}, {deadline});

  const FieldInfo &next = fields_.at(std::string(kFieldNextId));
  const CellRef reqId   = reg();
  mov(S(reqId), S(next.valueCell));
  emit(OpcodeKind::Add, "id+", {S(next.valueCell), S(one_)}, {next.valueCell});

  CellRef idVp, idNp, vc;
  callFind(curIntention_, std::string(kIntentionId), idVp, idNp);
  const CellRef entry = callOutbox(kKindAchieve);
  callAppendField(entry, std::string(kOutboxReqId), std::nullopt, vc);
  mov(D(vc), S(reqId));
  callAppendField(entry, std::string(kOutboxReqTo), std::nullopt, vc);
  mov(D(vc), S(toReg));
  callAppendField(entry, std::string(kOutboxReqName), std::nullopt, vc);
  mov(D(vc), S(cStr(gi.name)));
  callAppendField(entry, std::string(kOutboxReqArgs), argsRec, vc);
  callAppendField(entry, std::string(kOutboxReqDeadline), std::nullopt, vc);
  mov(D(vc), S(deadline));
  callAppendField(entry, std::string(kOutboxReqIntention), std::nullopt, vc);
  mov(D(vc), D(idVp));

  const CellRef waitRec = reg();
  alloc(waitRec);
  mov(D(waitRec), S(cStr("Waiting")));
  markRecord(waitRec);
  callAppendField(waitRec, std::string(kWaitingKind), std::nullopt, vc);
  mov(D(vc), S(cStr(std::string(kWaitKindDelegate))));
  callAppendField(waitRec, std::string(kWaitingKey), std::nullopt, vc);
  mov(D(vc), S(reqId));
  callAppendField(waitRec, std::string(kWaitingDeadline), std::nullopt, vc);
  mov(D(vc), S(deadline));
  callAppendField(waitRec, std::string(kWaitingTo), std::nullopt, vc);
  mov(D(vc), S(toReg));
  callSetField(curIntention_, std::string(kIntentionWaiting), waitRec, vc);
  callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
  callClearField(curIntention_, std::string(kIntentionReady));
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);

  place(resumeLabel);
  emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
  emit(OpcodeKind::Choice, "pcontract",
       {S(opref(planLabel(ctx_.planContractFail)))});
  emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
  CellRef wVp, wNp, mVp, mNp, okVp, okNp, toVp, toNp;
  callFind(curIntention_, std::string(kIntentionWaiting), wVp, wNp);
  const CellRef delFailed = newLabel(), checkMatch = newLabel(),
                delDone = newLabel();
  branchIfZero(wVp, delFailed);
  callFind(wVp, std::string(kWaitingTimedOut), toVp, toNp);
  branchIfZero(toVp, checkMatch);
  // Timed out: send $cancel to the delegate (§7.4).
  mov(S(a0_), S(wVp));
  intentionRuntime("CancelWait");
  callClearField(curIntention_, std::string(kIntentionWaiting));
  jump(delFailed);
  place(checkMatch);
  callClearField(curIntention_, std::string(kIntentionWaiting));
  callFind(wVp, std::string(kWaitingMatch), mVp, mNp);
  branchIfZero(mVp, delFailed);
  callFind(mVp, std::string(kEffectOutcomeOk), okVp, okNp);
  branchIfZero(okVp, delFailed);
  branch(D(okVp), delDone);
  place(delFailed);
  if (a.orElse) {
    emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
    emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
    emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
    expr(*a.orElse, out);
  } else {
    failNow();
  }
  place(delDone);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::askExpr(const EAsk &a, CellRef out, SourceLocation loc) {
  if (!ctx_.inPlan || ctx_.planInfo == nullptr) {
    bail(CompileError::Unsupported, "ask outside a plan", loc);
  }
  const auto *call = std::get_if<ECall>(&a.question->node);
  const auto *f =
      call != nullptr ? std::get_if<EField>(&call->callee->node) : nullptr;
  if (f == nullptr) {
    bail(CompileError::Unsupported, "ask expects agent.question(args)", loc);
  }
  const auto it = questions_.find(f->name);
  if (it == questions_.end()) {
    bail(CompileError::UnknownName, "unknown question " + f->name, loc);
  }
  const QuestionInfo &qi = it->second;
  if (call->args.size() != qi.params.size()) {
    bail(CompileError::BadArity,
         qi.name + " takes " + std::to_string(qi.params.size()) + " arguments",
         loc);
  }
  PlanInfo &pi              = *ctx_.planInfo;
  const std::size_t stepOrd = pi.stepEntries.size();
  const CellRef resumeLabel = newLabel();
  pi.stepEntries.push_back(resumeLabel);

  const CellRef toReg = reg(), argsRec = reg(), durReg = reg();
  expr(*f->base, toReg);
  alloc(argsRec);
  mov(D(argsRec), S(cStr("args")));
  markRecord(argsRec);
  for (std::size_t i = 0; i < qi.params.size(); ++i) {
    const CellRef v = reg();
    const Ty vt     = valueInto(*call->args[i], v);
    coerceInto(qi.params[i].second, vt, v);
    CellRef vc;
    if (qi.params[i].second.composite()) {
      callAppendField(argsRec, positionalName(i), v, vc);
    } else {
      callAppendField(argsRec, positionalName(i), std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  expr(*a.within, durReg);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), nowReg = reg(), deadline = reg();
  CellRef timeP, nameP;
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  callFind(tp, std::string(kTurnTime), timeP, nameP);
  movC(nowReg, zero_);
  const CellRef noTime = newLabel();
  branchIfZero(timeP, noTime);
  mov(S(nowReg), D(timeP));
  place(noTime);
  emit(OpcodeKind::Add, "dl", {S(nowReg), S(durReg)}, {deadline});

  const FieldInfo &next = fields_.at(std::string(kFieldNextId));
  const CellRef reqId   = reg();
  mov(S(reqId), S(next.valueCell));
  emit(OpcodeKind::Add, "id+", {S(next.valueCell), S(one_)}, {next.valueCell});

  CellRef idVp, idNp, vc;
  callFind(curIntention_, std::string(kIntentionId), idVp, idNp);
  const CellRef entry = callOutbox(kKindAsk);
  callAppendField(entry, std::string(kOutboxReqId), std::nullopt, vc);
  mov(D(vc), S(reqId));
  callAppendField(entry, std::string(kOutboxReqTo), std::nullopt, vc);
  mov(D(vc), S(toReg));
  callAppendField(entry, std::string(kOutboxReqName), std::nullopt, vc);
  mov(D(vc), S(cStr(qi.name)));
  callAppendField(entry, std::string(kOutboxReqArgs), argsRec, vc);
  callAppendField(entry, std::string(kOutboxReqDeadline), std::nullopt, vc);
  mov(D(vc), S(deadline));
  callAppendField(entry, std::string(kOutboxReqIntention), std::nullopt, vc);
  mov(D(vc), D(idVp));

  const CellRef waitRec = reg();
  alloc(waitRec);
  mov(D(waitRec), S(cStr("Waiting")));
  markRecord(waitRec);
  callAppendField(waitRec, std::string(kWaitingKind), std::nullopt, vc);
  mov(D(vc), S(cStr(std::string(kWaitKindAsk))));
  callAppendField(waitRec, std::string(kWaitingKey), std::nullopt, vc);
  mov(D(vc), S(reqId));
  callAppendField(waitRec, std::string(kWaitingDeadline), std::nullopt, vc);
  mov(D(vc), S(deadline));
  callSetField(curIntention_, std::string(kIntentionWaiting), waitRec, vc);
  callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
  callClearField(curIntention_, std::string(kIntentionReady));
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);

  place(resumeLabel);
  emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
  emit(OpcodeKind::Choice, "pcontract",
       {S(opref(planLabel(ctx_.planContractFail)))});
  emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
  CellRef wVp, wNp, mVp, mNp, okVp, okNp, valVp, valNp;
  callFind(curIntention_, std::string(kIntentionWaiting), wVp, wNp);
  callClearField(curIntention_, std::string(kIntentionWaiting));
  const CellRef askFailed = newLabel(), askOk = newLabel(),
                askDone = newLabel();
  branchIfZero(wVp, askFailed);
  callFind(wVp, std::string(kWaitingMatch), mVp, mNp);
  branchIfZero(mVp, askFailed);
  callFind(mVp, std::string(kEffectOutcomeOk), okVp, okNp);
  branchIfZero(okVp, askFailed);
  branch(D(okVp), askOk);
  jump(askFailed);
  place(askOk);
  callFind(mVp, std::string(kEffectOutcomeValue), valVp, valNp);
  if (qi.result.k == Ty::K::Unit) {
    movC(out, unit_c_);
  } else {
    branchIfZero(valVp, askFailed);
    if (qi.result.composite()) {
      const CellRef cp = reg();
      callCopy(valVp, cp);
      mov(S(out), S(cp));
    } else {
      mov(S(out), D(valVp));
    }
  }
  jump(askDone);
  place(askFailed);
  if (a.orElse) {
    emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
    emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
    emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
    expr(*a.orElse, out);
  } else {
    failNow();
  }
  place(askDone);
  return qi.result.k == Ty::K::Opt ? unwrapOpt(out, qi.result, out, loc)
                                   : qi.result;
}

Ty Gen::awaitExpr(const EAwait &a, CellRef out, SourceLocation loc) {
  if (!ctx_.inPlan || ctx_.planInfo == nullptr) {
    bail(CompileError::Unsupported, "await outside a plan", loc);
  }
  PlanInfo &pi              = *ctx_.planInfo;
  const std::size_t stepOrd = pi.stepEntries.size();
  const CellRef resumeLabel = newLabel();
  pi.stepEntries.push_back(resumeLabel);

  // Determine the result type of `await`.
  Ty resTy = tyOf(Ty::K::Unit);
  if (!a.eventType.empty()) {
    resTy      = tyOf(Ty::K::Record);
    resTy.name = a.eventType;
  }

  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), nowReg = reg(), durReg = reg(), deadline = reg();
  CellRef timeP, nameP;
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  callFind(tp, std::string(kTurnTime), timeP, nameP);
  movC(nowReg, zero_);
  const CellRef noTime = newLabel();
  branchIfZero(timeP, noTime);
  mov(S(nowReg), D(timeP));
  place(noTime);
  if (a.within) {
    expr(*a.within, durReg);
    emit(OpcodeKind::Add, "dl", {S(nowReg), S(durReg)}, {deadline});
  } else {
    movC(deadline, zero_);
  }

  // Build `waiting` record on `curIntention_`.
  const CellRef waitRec = reg();
  alloc(waitRec);
  mov(D(waitRec), S(cStr("Waiting")));
  markRecord(waitRec);
  CellRef vc;
  callAppendField(waitRec, std::string(kWaitingKind), std::nullopt, vc);
  if (!a.eventType.empty()) {
    mov(D(vc), S(cStr(std::string(kWaitKindAwaitEvent))));
    callAppendField(waitRec, std::string(kWaitingEventType), std::nullopt, vc);
    mov(D(vc), S(cStr(a.eventType)));
  } else if (a.term) {
    mov(D(vc), S(cStr(std::string(kWaitKindAwaitBelief))));
    callAppendField(waitRec, std::string(kWaitingPattern), std::nullopt, vc);
    mov(D(vc), S(cStr(a.term->name)));
  }
  if (a.within) {
    callAppendField(waitRec, std::string(kWaitingDeadline), std::nullopt, vc);
    mov(D(vc), S(deadline));
  }
  callAppendField(waitRec, std::string(kWaitingAwaitId), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));

  callSetField(curIntention_, std::string(kIntentionWaiting), waitRec, vc);
  callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
  callClearField(curIntention_, std::string(kIntentionReady));
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);

  place(resumeLabel);
  emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
  emit(OpcodeKind::Choice, "pcontract",
       {S(opref(planLabel(ctx_.planContractFail)))});
  emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
  CellRef wVp, wNp, mVp, mNp, toVp, toNp;
  callFind(curIntention_, std::string(kIntentionWaiting), wVp, wNp);
  callClearField(curIntention_, std::string(kIntentionWaiting));
  const CellRef awFailed = newLabel(), awCheck = newLabel(),
                awDone = newLabel();
  branchIfZero(wVp, awFailed);
  callFind(wVp, std::string(kWaitingTimedOut), toVp, toNp);
  branchIfZero(toVp, awCheck);
  jump(awFailed);
  place(awCheck);
  callFind(wVp, std::string(kWaitingMatch), mVp, mNp);
  branchIfZero(mVp, awFailed);
  if (!a.eventType.empty()) {
    const CellRef cp = reg();
    callCopy(mVp, cp);
    mov(S(out), S(cp));
  } else {
    movC(out, unit_c_);
  }
  jump(awDone);
  place(awFailed);
  if (a.orElse) {
    emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
    emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
    emit(OpcodeKind::Choice, "pcontract",
         {S(opref(planLabel(ctx_.planContractFail)))});
    emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
    emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
    expr(*a.orElse, out);
  } else {
    failNow();
  }
  place(awDone);
  return resTy;
}

Ty Gen::effectCall(const std::string &skillName, const MethodSig &sig,
                   const std::vector<ExprPtr> &args, CellRef out,
                   SourceLocation loc) {
  if (args.size() != sig.params.size()) {
    bail(CompileError::BadArity,
         sig.name + " takes " + std::to_string(sig.params.size()) +
             " arguments",
         loc);
  }
  const CellRef argsRec = reg();
  alloc(argsRec);
  mov(D(argsRec), S(cStr(sig.name)));
  markRecord(argsRec);
  for (std::size_t i = 0; i < sig.params.size(); ++i) {
    const CellRef v  = reg();
    const Ty paramTy = fromType(sig.params[i].type, sig.params[i].loc);
    const Ty vt      = valueInto(*args[i], v);
    coerceInto(paramTy, vt, v);
    CellRef vc;
    if (paramTy.composite()) {
      callAppendField(argsRec, sig.params[i].name, v, vc);
    } else {
      callAppendField(argsRec, sig.params[i].name, std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  const FieldInfo &next = fields_.at(std::string(kFieldNextId));
  const CellRef effId   = reg();
  mov(S(effId), S(next.valueCell));
  emit(OpcodeKind::Add, "id+", {S(next.valueCell), S(one_)}, {next.valueCell});

  const CellRef entry = callOutbox(kKindEffect);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxEffectSkill), std::nullopt, vc);
  mov(D(vc), S(cStr(skillName)));
  callAppendField(entry, std::string(kOutboxEffectMethod), std::nullopt, vc);
  mov(D(vc), S(cStr(sig.name)));
  callAppendField(entry, std::string(kOutboxEffectId), std::nullopt, vc);
  mov(D(vc), S(effId));
  callAppendField(entry, std::string(kOutboxEffectArgs), argsRec, vc);

  Ty retTy = sig.result ? fromType(*sig.result, loc) : tyOf(Ty::K::Unit);
  if (!ctx_.inPlan || ctx_.planInfo == nullptr) {
    // §6.6: in a handler, a unit-returning effect call is staged without an
    // intention or waiting record and returns immediately.
    movC(out, unit_c_);
    return retTy;
  }

  PlanInfo &pi              = *ctx_.planInfo;
  const std::size_t stepOrd = pi.stepEntries.size();
  const CellRef resumeLabel = newLabel();
  pi.stepEntries.push_back(resumeLabel);

  CellRef idVp, idNp;
  callFind(curIntention_, std::string(kIntentionId), idVp, idNp);
  callAppendField(entry, std::string(kOutboxEffectIntention), std::nullopt, vc);
  mov(D(vc), D(idVp));

  const CellRef waitRec = reg();
  alloc(waitRec);
  mov(D(waitRec), S(cStr("Waiting")));
  markRecord(waitRec);
  callAppendField(waitRec, std::string(kWaitingKind), std::nullopt, vc);
  mov(D(vc), S(cStr(std::string(kWaitKindEffect))));
  callAppendField(waitRec, std::string(kWaitingKey), std::nullopt, vc);
  mov(D(vc), S(effId));
  callSetField(curIntention_, std::string(kIntentionWaiting), waitRec, vc);
  callSetField(curIntention_, std::string(kIntentionStep), std::nullopt, vc);
  mov(D(vc), S(cInt(static_cast<std::int64_t>(stepOrd))));
  callClearField(curIntention_, std::string(kIntentionReady));
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);

  place(resumeLabel);
  emit(OpcodeKind::ChoiceDepth, "d", {}, {dRegAt(0)});
  emit(OpcodeKind::Choice, "pcontract",
       {S(opref(planLabel(ctx_.planContractFail)))});
  emit(OpcodeKind::ChoiceDepth, "pcd", {}, {planContractDG_});
  emit(OpcodeKind::Choice, "pfail", {S(opref(planLabel(ctx_.planFail)))});
  CellRef wVp, wNp, mVp, mNp, okVp, okNp, valVp, valNp;
  callFind(curIntention_, std::string(kIntentionWaiting), wVp, wNp);
  callClearField(curIntention_, std::string(kIntentionWaiting));
  const CellRef effFailed = newLabel(), effOk = newLabel(),
                effDone = newLabel();
  branchIfZero(wVp, effFailed);
  callFind(wVp, std::string(kWaitingMatch), mVp, mNp);
  branchIfZero(mVp, effFailed);
  callFind(mVp, std::string(kEffectOutcomeOk), okVp, okNp);
  branchIfZero(okVp, effFailed);
  branch(D(okVp), effOk);
  jump(effFailed);
  place(effOk);
  if (retTy.k == Ty::K::Unit) {
    movC(out, unit_c_);
  } else {
    callFind(mVp, std::string(kEffectOutcomeValue), valVp, valNp);
    branchIfZero(valVp, effFailed);
    if (retTy.composite()) {
      const CellRef cp = reg();
      callCopy(valVp, cp);
      mov(S(out), S(cp));
    } else {
      mov(S(out), D(valVp));
    }
  }
  jump(effDone);
  place(effFailed);
  failNow();
  place(effDone);
  return retTy.k == Ty::K::Opt ? unwrapOpt(out, retTy, out, loc) : retTy;
}

Ty Gen::emitExpr(const EEmit &e, CellRef out, SourceLocation loc) {
  const CellRef ev = reg();
  const Ty et      = valueInto(*e.event, ev);
  if (et.k != Ty::K::Record) {
    bail(CompileError::TypeMismatch, "emit needs an event", loc);
  }
  CellRef spaceV = reg();
  if (e.space) {
    expr(*e.space, spaceV);
  } else {
    movC(spaceV, cStr(std::string(kSpaceOuter)));
  }
  std::optional<CellRef> forList;
  if (e.recipients) {
    const CellRef list = reg();
    forList            = list;
    const Ty rt        = valueInto(*e.recipients, list);
    if (rt.k == Ty::K::Str) {
      const CellRef single = reg(), cell = reg();
      mov(S(single), S(list));
      alloc(list);
      mov(D(list), S(cStr(std::string(kListContent))));
      alloc(cell);
      mov(D(cell), S(single));
      callListPush(list, cell);
    } else if (rt.k != Ty::K::List) {
      bail(CompileError::TypeMismatch, "for needs a name or a list", loc);
    }
  }
  const CellRef entry = callOutbox(kKindEvent);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxEvent), ev, vc);
  callAppendField(entry, std::string(kOutboxSpace), std::nullopt, vc);
  mov(D(vc), S(spaceV));
  if (forList) {
    callAppendField(entry, std::string(kOutboxFor), *forList, vc);
  }
  const FieldInfo &ord = fields_.at(std::string(kFieldOrdinal));
  emit(OpcodeKind::Add, "ord+", {S(ord.valueCell), S(one_)}, {ord.valueCell});
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::spawnExpr(const ESpawn &s, CellRef out, SourceLocation loc) {
  const auto ag = agents_.find(s.agent);
  if (ag == agents_.end()) {
    bail(CompileError::UnknownAgent, "unknown agent " + s.agent, loc);
  }
  if (s.space) {
    const auto *sp = std::get_if<ESpecial>(&s.space->node);
    if (sp == nullptr || sp->kind != ESpecial::Kind::Inner) {
      unsupported("spawn into a space other than inner", loc);
    }
  }
  const auto &params = ag->second->params;
  if (params.size() != s.args.size()) {
    bail(CompileError::BadArity,
         s.agent + " takes " + std::to_string(params.size()) + " arguments",
         loc);
  }
  const CellRef args = reg();
  alloc(args);
  markRecord(args);
  for (std::size_t i = 0; i < params.size(); ++i) {
    const CellRef v = reg();
    const Ty t      = valueInto(*s.args[i], v);
    CellRef vc;
    if (t.composite()) {
      callAppendField(args, params[i].name, v, vc);
    } else {
      callAppendField(args, params[i].name, std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  const FieldInfo &cnt = fields_.at(std::string(kFieldSpawnCount));
  const CellRef nm = reg(), cs = reg(), selfName = reg();
  loadFieldInto(fields_.at(std::string(kFieldName)), selfName);
  emit(OpcodeKind::Add, "nm", {S(selfName), S(cStr("/" + s.agent + "#"))},
       {nm});
  emit(OpcodeKind::Trim, "cnt", {S(cnt.valueCell)}, {cs});
  emit(OpcodeKind::Add, "nm", {S(nm), S(cs)}, {nm});
  const CellRef entry = callOutbox(kKindSpawn);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxAgentType), std::nullopt, vc);
  mov(D(vc), S(cStr(s.agent)));
  callAppendField(entry, std::string(kOutboxChildName), std::nullopt, vc);
  mov(D(vc), S(nm));
  callAppendField(entry, std::string(kOutboxArgs), args, vc);
  emit(OpcodeKind::Add, "sc+", {S(cnt.valueCell), S(one_)}, {cnt.valueCell});
  mov(S(out), S(nm));
  return tyOf(Ty::K::Str);
}

// ---------------------------------------------------------------------------
// Beliefs (design §5.1, §6.1, §6.5, §9.1)
//
// A predicate is a cell on the agent's `d.belief` rank, its facts clause cells
// on `d.clause`. A fact's term is laid out as Vlog lays terms out: the
// predicate name as content, the arguments on `+d.grab` and `d.step`, so
// `#Unify` applies to it. A clause's sources are peer cells on `d.source`,
// whose content is the asserting agent's name.
//
// Deviation from the §9.1 text ("copy `t`, `#Choice`, `#Unify`"): a query is
// matched argument by argument. A value argument is compared with `#Eq`
// (scalars) or `#Unify` (lists), and a `?v` captures the ref of the clause's
// argument cell. A `?v` therefore never becomes a Vlog variable. `MakeVar`
// appends to the rank headed by the `d.vars` dimension cell, so a variable
// kept by a cut would stay on that rank for the life of the agent and make
// every later `MakeVar` walk it; a captured ref costs nothing and is undone
// by the same `release` that undoes the locals it is copied into.
// ---------------------------------------------------------------------------

void Gen::mintPredicates() {
  for (const TopDecl &d : unit_.decls) {
    const auto *pd = std::get_if<PredicateDecl>(&d);
    if (pd == nullptr) continue;
    PredInfo pi;
    bool leadingKeys = true;
    for (const PredParam &p : pd->params) {
      Ty t = fromType(p.type, pd->loc);
      if (t.k == Ty::K::Record || t.k == Ty::K::Unit) {
        unsupported("a record or () belief argument", pd->loc);
      }
      if (p.key && leadingKeys) {
        ++pi.nkeys;
      } else {
        leadingKeys = false;
      }
      pi.argTys.push_back(std::move(t));
    }
    pi.cell = arena_.makeCell(pd->name);
    stateCells_.insert(pi.cell);
    std::optional<ArbitrationPolicy> arbitration;
    if (pd->arbitration) {
      const bool continuous = std::ranges::any_of(
          std::views::iota(std::size_t{0}, pd->params.size()),
          [&](std::size_t i) {
            return !pd->params[i].key && pi.argTys[i].k == Ty::K::Float;
          });
      // The checker already accepted the clause; a refusal here means the
      // two disagree, which is a compiler defect, not a user error.
      const auto policy = checkArbitrationPolicy(*pd->arbitration, continuous);
      if (!policy) {
        bail(CompileError::Unsupported,
             "arbitration policy the checker accepted: " +
                 policy.error().front().message,
             pd->loc);
      }
      pi.arbitrated       = true;
      pi.clockSkewSeconds = static_cast<double>(policy->clockSkewMs) / 1000.0;
      arbitration.emplace(*policy);
    }
    const CellRef cell = pi.cell;
    if (!preds_.emplace(pd->name, std::move(pi)).second) {
      bail(CompileError::DuplicateName, "predicate " + pd->name, pd->loc);
    }
    if (arbitration) mintPolicyMaster(cell, *arbitration);
    zigzag::expectWritten(arena_.link(lastPred_.value_or(agentCell_),
                                      dims_.belief, DimVector::POS, cell));
    lastPred_ = cell;
  }
}

/// The predicate's policy master heads its `d.epistemic` rank: a record of
/// typed constants that later calibration records take clone views of
/// (P19 section 2.1). Durations are Int64 milliseconds; absent optional keys
/// have no field rather than a sentinel value.
void Gen::mintPolicyMaster(CellRef predicate, const ArbitrationPolicy &p) {
  const CellRef master = arena_.makeCell(std::string_view("EpistemicPolicy"));
  zigzag::expectWritten(
      arena_.link(predicate, dims_.epistemic, DimVector::POS, master));
  CellRef tail     = master;
  const auto field = [&](std::string_view name, CellRef value) {
    const CellRef label = arena_.makeCell(name);
    zigzag::expectWritten(
        arena_.link(tail, core_.dims().vars, DimVector::POS, label));
    zigzag::expectWritten(
        arena_.link(label, core_.dims().values, DimVector::POS, value));
    tail = label;
  };
  const auto real = [&](std::string_view name, std::optional<double> v) {
    if (v) field(name, arena_.makeScalarCell(*v));
  };
  const auto ms = [&](std::string_view name, std::optional<std::int64_t> v) {
    if (v) field(name, arena_.makeScalarCell(*v));
  };
  const auto text = [&](std::string_view name,
                        const std::optional<std::string> &v) {
    if (v) field(name, arena_.makeCell(std::string_view(*v)));
  };
  text("strategy", std::string(strategyName(p.strategy)));
  text("model", p.model);
  real("min_confidence", p.minConfidence);
  real("prior_alpha", p.priorAlpha);
  real("prior_beta", p.priorBeta);
  text("clock", std::string(clockName(p.clock)));
  ms("max_age", p.maxAgeMs);
  ms("clock_skew", p.clockSkewMs);
  ms("max_uncertainty", p.maxUncertaintyMs);
  ms("decay", p.decayMs);
  ms("alignment", p.alignmentMs);
  real("score_tolerance", p.scoreTolerance);
  field("allow_untested", arena_.makeScalarCell(p.allowUntested));
  field("continuous", arena_.makeScalarCell(p.continuous));
  real("cluster_tolerance", p.clusterTolerance);
  text("frame", p.frame);
  text("units", p.units);
  real("min_variance", p.minVariance);
  real("max_variance", p.maxVariance);
  real("max_cluster_diameter", p.maxClusterDiameter);
  real("max_slew", p.maxSlew);
  real("innovation", p.innovation);
  text("hash", policyHash(p));
}

const Gen::PredInfo &Gen::predOf(const Term &t) {
  const auto it = preds_.find(t.name);
  if (it == preds_.end()) {
    bail(CompileError::UnknownName, "unknown predicate " + t.name, t.loc);
  }
  if (it->second.argTys.size() != t.args.size()) {
    bail(CompileError::BadArity,
         t.name + " takes " + std::to_string(it->second.argTys.size()) +
             " arguments",
         t.loc);
  }
  return it->second;
}

void Gen::breakOp(Opd owner, DimRef dim) {
  emit(OpcodeKind::Break, "brk", {owner, S(cInt(dim)), S(one_)});
}

/// Removal links a cell's predecessor to its successor (§5.1); the owner is
/// the predecessor of a rank's first cell, so no case is special but the end.
void Gen::unlinkFromRank(CellRef xReg, DimRef dim) {
  const CellRef prev = rd(D(xReg), dim, true);
  const CellRef nxt  = rd(D(xReg), dim);
  const CellRef last = newLabel(), done = newLabel();
  branchIfZero(nxt, last);
  linkOp(D(prev), dim, D(nxt));
  jump(done);
  place(last);
  breakOp(D(prev), dim);
  place(done);
}

void Gen::requireTrue(CellRef cond, CellRef orElse) {
  const CellRef good = newLabel();
  branch(S(cond), good);
  jump(orElse);
  place(good);
}

void Gen::selfName(CellRef out) {
  loadFieldInto(fields_.at(std::string(kFieldName)), out);
}

/// a0 = "+" or "-", a1 = the fact's term, a2 = the predicate cell.
void Gen::emitRtBRaise() {
  beginInternal(rtBRaise_);
  const CellRef sign = reg(), term = reg(), pred = reg(), ev = reg(),
                nm = reg(), cp = reg();
  mov(S(sign), S(a0_));
  mov(S(term), S(a1_));
  mov(S(pred), S(a2_));
  alloc(ev);
  emit(OpcodeKind::Add, "evname", {S(sign), D(pred)}, {nm});
  mov(D(ev), S(nm));
  // The event owns a copy: the clause keeps its term on its own `+d.grab`, and
  // a cell has one `-d.grab` neighbour.
  callCopy(term, cp);
  linkOp(D(ev), core_.dims().grab, D(cp));
  emitAppendTail(S(agentCell_), dims_.pending, dims_.pending, ev);
  ret();
}

/// a0 = clause, a1 = name; r0 = 1 when the clause has that source.
void Gen::emitRtHasSrc() {
  beginInternal(rtHasSrc_);
  const CellRef cl = reg(), nm = reg(), s = reg(), eq = reg();
  const CellRef loop = newLabel(), yes = newLabel(), no = newLabel();
  mov(S(cl), S(a0_));
  mov(S(nm), S(a1_));
  mov(S(s), S(rd(D(cl), dims_.source)));
  place(loop);
  branchIfZero(s, no);
  emit(OpcodeKind::Eq, "src?", {D(s), S(nm)}, {eq});
  branch(S(eq), yes);
  mov(S(s), S(rd(D(s), dims_.source)));
  jump(loop);
  place(yes);
  movC(r0_, one_);
  ret();
  place(no);
  movC(r0_, zero_);
  ret();
}

/// a0 = predicate, a1 = clause, a2 = name: remove that source; when none is
/// left, unlink the clause and raise `-pred`. r0 = 1 when the clause went.
void Gen::emitRtBDrop() {
  beginInternal(rtBDrop_);
  const CellRef pred = reg(), cl = reg(), nm = reg(), s = reg(), eq = reg(),
                left = reg(), ct = reg();
  const CellRef loop = newLabel(), found = newLabel(), none = newLabel(),
                kept = newLabel();
  mov(S(pred), S(a0_));
  mov(S(cl), S(a1_));
  mov(S(nm), S(a2_));
  mov(S(s), S(rd(D(cl), dims_.source)));
  place(loop);
  branchIfZero(s, none);
  emit(OpcodeKind::Eq, "src?", {D(s), S(nm)}, {eq});
  branch(S(eq), found);
  mov(S(s), S(rd(D(s), dims_.source)));
  jump(loop);
  place(found);
  unlinkFromRank(s, dims_.source);
  mov(S(left), S(rd(D(cl), dims_.source)));
  branchIfZero(left, kept);
  movC(r0_, zero_);
  ret();
  place(kept);
  unlinkFromRank(cl, core_.dims().clause);
  mov(S(ct), S(rd(D(cl), core_.dims().grab)));
  movC(a0_, cStr(std::string(kBeliefRemoved)));
  mov(S(a1_), S(ct));
  mov(S(a2_), S(pred));
  callLabel(rtBRaise_);
  movC(r0_, one_);
  ret();
  place(none);
  movC(r0_, zero_);
  ret();
}

/// a0 = predicate, a1 = a fresh ground term (adopted), a2 = source name,
/// a3 = replace (keyed `believe`, `revise`), a4 = key count. r1 = the source's
/// support cell on the fact, for `rtObserve`.
///
/// With replace, every other fact with the same key arguments that this source
/// asserts loses the source first, so `-old` is raised before `+new`. Then the
/// fact is added, or only its source is when it is already present.
void Gen::emitRtBSet() {
  beginInternal(rtBSet_);
  const CellRef pred = reg(), term = reg(), src = reg(), repl = reg(),
                nk = reg(), c = reg(), nx = reg(), ct = reg(), t1 = reg(),
                t2 = reg(), i = reg(), u = reg(), cl = reg(), sc = reg(),
                keep = reg(), moved = reg(), old = reg(), hist = reg();
  const CellRef add = newLabel(), loop1 = newLabel(), next1 = newLabel(),
                keyOk = newLabel(), keySame = newLabel(), kloop = newLabel(),
                loop2 = newLabel(), exists = newLabel(), addSrc = newLabel(),
                create = newLabel();
  mov(S(pred), S(a0_));
  mov(S(term), S(a1_));
  mov(S(src), S(a2_));
  mov(S(repl), S(a3_));
  mov(S(nk), S(a4_));
  // A fresh global is not 1, so only rtObserve's explicit flag keeps history.
  emit(OpcodeKind::Eq, "keep?", {S(keepHistG_), S(one_)}, {keep});
  movC(moved, zero_);
  branchIfZero(repl, add);
  mov(S(c), S(rd(D(pred), core_.dims().clause)));
  place(loop1);
  branchIfZero(c, add);
  mov(S(nx), S(rd(D(c), core_.dims().clause)));
  mov(S(ct), S(rd(D(c), core_.dims().grab)));
  mov(S(t1), S(rd(D(term), core_.dims().grab)));
  mov(S(t2), S(rd(D(ct), core_.dims().grab)));
  movC(i, zero_);
  place(kloop);
  emit(OpcodeKind::Gte, "i>=nk", {S(i), S(nk)}, {u});
  branch(S(u), keySame);
  emit(OpcodeKind::Unify, "key", {D(t1), D(t2)}, {u});
  branch(S(u), keyOk);
  jump(next1);
  place(keyOk);
  mov(S(t1), S(rd(D(t1), core_.dims().step)));
  mov(S(t2), S(rd(D(t2), core_.dims().step)));
  emit(OpcodeKind::Add, "i+", {S(i), S(one_)}, {i});
  jump(kloop);
  place(keySame);
  emit(OpcodeKind::Unify, "same", {D(term), D(ct)}, {u});
  branch(S(u), next1);
  mov(S(a0_), S(c));
  mov(S(a1_), S(src));
  callLabel(rtHasSrc_);
  branchIfZero(r0_, next1);
  {
    // One owner at a time: detach this source's history from the support
    // about to be dropped, to re-attach it to the surviving support.
    const CellRef noKeep = newLabel(), scan = newLabel(), got = newLabel(),
                  same = reg();
    branchIfZero(keep, noKeep);
    mov(S(old), S(rd(D(c), dims_.source)));
    place(scan);
    branchIfZero(old, noKeep);
    emit(OpcodeKind::Eq, "src?", {D(old), S(src)}, {same});
    branch(S(same), got);
    mov(S(old), S(rd(D(old), dims_.source)));
    jump(scan);
    place(got);
    mov(S(hist), S(rd(D(old), dims_.epistemic)));
    branchIfZero(hist, noKeep);
    breakOp(D(old), dims_.epistemic);
    mov(S(moved), S(hist));
    place(noKeep);
  }
  mov(S(a0_), S(pred));
  mov(S(a1_), S(c));
  mov(S(a2_), S(src));
  callLabel(rtBDrop_);
  place(next1);
  mov(S(c), S(nx));
  jump(loop1);

  place(add);
  mov(S(c), S(rd(D(pred), core_.dims().clause)));
  place(loop2);
  branchIfZero(c, create);
  mov(S(ct), S(rd(D(c), core_.dims().grab)));
  emit(OpcodeKind::Unify, "dup", {D(term), D(ct)}, {u});
  branch(S(u), exists);
  mov(S(c), S(rd(D(c), core_.dims().clause)));
  jump(loop2);

  place(exists);
  mov(S(a0_), S(c));
  mov(S(a1_), S(src));
  callLabel(rtHasSrc_);
  branchIfZero(r0_, addSrc);
  {
    const CellRef scan = newLabel(), got = newLabel(), same = reg();
    mov(S(sc), S(rd(D(c), dims_.source)));
    place(scan);
    emit(OpcodeKind::Eq, "src?", {D(sc), S(src)}, {same});
    branch(S(same), got);
    mov(S(sc), S(rd(D(sc), dims_.source)));
    jump(scan);
    place(got);
  }
  finishSupport(sc, moved);
  movC(r0_, zero_);
  ret();
  place(addSrc);
  alloc(sc);
  mov(D(sc), S(src));
  emitAppendTail(D(c), dims_.source, dims_.source, sc);
  finishSupport(sc, moved);
  movC(r0_, zero_);
  ret();

  place(create);
  alloc(cl);
  mov(D(cl), D(pred));
  linkOp(D(cl), core_.dims().grab, D(term));
  alloc(sc);
  mov(D(sc), S(src));
  linkOp(D(cl), dims_.source, D(sc));
  emitAppendTail(D(pred), core_.dims().clause, core_.dims().clause, cl);
  finishSupport(sc, moved);
  movC(a0_, cStr(std::string(kBeliefAdded)));
  mov(S(a1_), S(term));
  mov(S(a2_), S(pred));
  callLabel(rtBRaise_);
  movC(r0_, one_);
  ret();
}

void Gen::finishSupport(CellRef sc, CellRef moved) {
  const CellRef none = newLabel();
  branchIfZero(moved, none);
  emitAppendTail(D(sc), dims_.epistemic, dims_.epistemic, moved);
  place(none);
  mov(S(r1_), S(sc));
}

void Gen::endObservation(std::string_view code) {
  mov(S(fields_.at(std::string(kFieldTurnResult)).valueCell),
      S(cStr(std::string(code))));
  jump(haltOp_);
}

/// `observeRefusalEntry` (P19 I3-1b): charge the refusals in `$turn.refusal`
/// to their principal's profile, apart from the gate's `reject_count`.
/// Content refusals of direct events may feed health (I3-2) and show the
/// device alive; every other refusal, and any of a relayed copy, is
/// diagnostic only, so a replay or a hostile relay can make a source look
/// neither faulty nor alive.
void Gen::emitObserveRefusal(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), note = reg(), who = reg(), content = reg(),
                other = reg(), code = reg(), contact = reg();
  const CellRef malformed = newLabel();
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  mov(S(a0_), S(tp));
  movC(a1_, cStr(std::string(kTurnRefusal)));
  callLabel(rtFind_);
  mov(S(note), S(r0_));
  branchIfZero(note, malformed);
  const auto field = [&](std::string_view name, CellRef out) {
    mov(S(a0_), S(note));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  for (const auto &[name, out] :
       {std::pair{kObsSource, who}, std::pair{kRefusalContent, content},
        std::pair{kRefusalOther, other}, std::pair{kRefusalCode, code}}) {
    field(name, out);
    branchIfZero(out, malformed);
  }
  field(kRefusalContact, contact);
  const CellRef prof = reg(), same = reg(), loop = newLabel(),
                found = newLabel(), absent = newLabel();
  mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
  place(loop);
  branchIfZero(prof, absent);
  emit(OpcodeKind::Eq, "who?", {D(prof), D(who)}, {same});
  branch(S(same), found);
  mov(S(prof), S(rd(D(prof), dims_.trust)));
  jump(loop);
  place(absent);
  endObservation(kTurnResultNoProfile);
  place(found);
  const CellRef sum = reg();
  const auto charge = [&](std::string_view counter, CellRef by) {
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(counter)));
    callLabel(rtFind_);
    const CellRef v = reg();
    mov(S(v), S(r0_));
    branchIfZero(v, malformed);
    emit(OpcodeKind::Add, "n+", {D(v), D(by)}, {sum});
    mov(D(v), S(sum));
  };
  charge(kProfileContentRefusals, content);
  charge(kProfileRefusedCount, other);
  putScalar(prof, kProfileLastRefusal, D(code));
  const CellRef unseen = newLabel();
  branchIfZero(contact, unseen);
  {
    // Charges coalesce, so a refusal received before a later commit can be
    // charged after it: contact only ever moves forward.
    const CellRef last = reg(), older = reg(), fresh = newLabel();
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(kProfileLastContact)));
    callLabel(rtFind_);
    mov(S(last), S(r0_));
    branchIfZero(last, fresh);
    emit(OpcodeKind::Lte, "older?", {D(contact), D(last)}, {older});
    branch(S(older), unseen);
    place(fresh);
    putScalar(prof, kProfileLastContact, D(contact));
  }
  place(unseen);
  endObservation(kTurnResultCommitted);
  place(malformed);
  endObservation(kTurnResultIngestionFault);
}

/// `sourceChangeEntry` (P19 I3-1c): advance a principal to the incarnation
/// a new binding carries, or retire it for the operator. The arena is the
/// authority on the replay floor, so a binding the host accepted may still
/// refuse `StaleIncarnation` here, and the adapter then revokes it.
///
/// An advance retires every record of a lower incarnation on every key, so a
/// replacement device never leaves its predecessor's readings current, and
/// clears the dedup window and the streams, whose sequences restart. The
/// latches (faulted, the counters) stay: a restart cannot shed them.
///
/// A retirement drops the principal's supports through `rtBDrop`, settling
/// `-pred`, excises its unpinned records with their clone occurrences, and
/// leaves the profile cell as a tombstone with the higher of the two
/// incarnations. Records are reached from the profile master's `d.clone`
/// rank, one occurrence per record: O(K * H) cells.
void Gen::emitSourceChange(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), change = reg(), who = reg(), inc = reg(),
                retire = reg(), t = reg(), maxSources = reg(), retained = reg();
  const CellRef malformed = newLabel();
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  const auto find = [&](CellRef record, std::string_view name, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  find(tp, kTurnSourceChange, change);
  branchIfZero(change, malformed);
  for (const auto &[name, out] :
       {std::pair{kObsSource, who}, std::pair{kSourceRetire, retire},
        std::pair{kIngestMaxSources, maxSources},
        std::pair{kSourceRetained, retained}}) {
    find(change, name, out);
    branchIfZero(out, malformed);
  }
  // Absent only for the retirement of a principal no binding names.
  find(change, kObsIncarnation, inc);
  // A fresh streams record and dedup list, for a new or revived profile.
  const auto freshWindows = [&](CellRef prof) {
    const CellRef streams = reg(), seen = reg();
    alloc(streams);
    mov(D(streams), S(cStr(std::string(kProfileStreams))));
    markRecord(streams);
    CellRef vc;
    callSetField(prof, std::string(kProfileStreams), streams, vc);
    alloc(seen);
    mov(D(seen), S(cStr(std::string(kListContent))));
    callSetField(prof, std::string(kProfileDedup), seen, vc);
  };
  const auto setField = [&](CellRef record, std::string_view name, Opd v) {
    putScalar(record, name, v);
  };
  // The negward head of @p from's run on @p dim.
  const auto head = [&](CellRef from, DimRef dim, CellRef out) {
    const CellRef p = reg(), loop = newLabel(), done = newLabel();
    mov(S(out), S(from));
    place(loop);
    mov(S(p), S(rd(D(out), dim, true)));
    branchIfZero(p, done);
    mov(S(out), S(p));
    jump(loop);
    place(done);
  };
  // A clone occurrence is the value of its record's `profile` field.
  const auto recordOf = [&](CellRef view, CellRef out) {
    const CellRef name = reg();
    mov(S(name), S(rd(D(view), core_.dims().values, true)));
    // An occurrence no record names is a broken rank, not a record.
    branchIfZero(name, malformed);
    head(name, core_.dims().vars, out);
  };

  const CellRef prof = reg(), same = reg(), loop = newLabel(),
                found = newLabel(), absent = newLabel();
  mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
  place(loop);
  branchIfZero(prof, absent);
  emit(OpcodeKind::Eq, "who?", {D(prof), D(who)}, {same});
  branch(S(same), found);
  mov(S(prof), S(rd(D(prof), dims_.trust)));
  jump(loop);
  place(absent);
  {
    // Nothing seen yet: an advance has nothing to do, and a retirement
    // leaves a bare tombstone so the floor still holds.
    const CellRef skip = newLabel();
    emit(OpcodeKind::Eq, "retire?", {D(retire), S(true_)}, {t});
    requireTrue(t, skip);
    branchIfZero(inc, skip);
    alloc(prof);
    mov(D(prof), D(who));
    markRecord(prof);
    setField(prof, kProfileIncarnation, D(inc));
    setField(prof, kProfileRetired, S(true_));
    emitAppendTail(S(agentCell_), dims_.trust, dims_.trust, prof);
    place(skip);
    endObservation(kTurnResultCommitted);
  }
  place(found);
  const CellRef profInc = reg(), tomb = reg(), lower = reg();
  find(prof, kProfileIncarnation, profInc);
  branchIfZero(profInc, malformed);
  {
    const CellRef v = reg(), known = newLabel();
    movC(tomb, false_);
    find(prof, kProfileRetired, v);
    branchIfZero(v, known);
    mov(S(tomb), D(v));
    place(known);
  }
  const CellRef doRetire = newLabel(), stale = newLabel();
  emit(OpcodeKind::Eq, "retire?", {D(retire), S(true_)}, {t});
  branch(S(t), doRetire);
  branchIfZero(inc, malformed);
  {
    // Advance, or revive a tombstone, only to a strictly higher incarnation;
    // a live profile already there needs nothing.
    const CellRef revive = newLabel(), advance = newLabel();
    emit(OpcodeKind::Lt, "higher?", {D(profInc), D(inc)}, {lower});
    branch(S(tomb), revive);
    branch(S(lower), advance);
    emit(OpcodeKind::Eq, "same?", {D(profInc), D(inc)}, {same});
    requireTrue(same, stale);
    endObservation(kTurnResultCommitted);
    place(revive);
    requireTrue(lower, stale);
    {
      // A revived principal takes a share of the budget again.
      const CellRef p = reg(), n = reg(), v = reg(), full = reg(),
                    walk = newLabel(), live = newLabel(), next = newLabel(),
                    counted = newLabel(), room = newLabel();
      movC(n, zero_);
      mov(S(p), S(rd(S(agentCell_), dims_.trust)));
      place(walk);
      branchIfZero(p, counted);
      find(p, kProfileRetired, v);
      branchIfZero(v, live);
      emit(OpcodeKind::Eq, "tomb?", {D(v), S(true_)}, {t});
      branch(S(t), next);
      place(live);
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
      place(next);
      mov(S(p), S(rd(D(p), dims_.trust)));
      jump(walk);
      place(counted);
      emit(OpcodeKind::Lt, "room?", {S(n), D(maxSources)}, {full});
      branch(S(full), room);
      endObservation(kTurnResultEpistemicBudget);
      place(room);
    }
    mov(D(profInc), D(inc));
    setField(prof, kProfileRetired, S(false_));
    freshWindows(prof);
    endObservation(kTurnResultCommitted);
    place(advance);
    mov(D(profInc), D(inc));
    const CellRef v = reg(), rec = reg(), ri = reg(), st = reg();
    const CellRef walk = newLabel(), next = newLabel(), done = newLabel();
    mov(S(v), S(rd(D(prof), core_.dims().clone)));
    place(walk);
    branchIfZero(v, done);
    recordOf(v, rec);
    find(rec, kObsIncarnation, ri);
    branchIfZero(ri, next);
    emit(OpcodeKind::Lt, "older?", {D(ri), D(inc)}, {t});
    requireTrue(t, next);
    find(rec, kObsStatus, st);
    branchIfZero(st, next);
    mov(D(st), S(cStr(std::string(kObsStatusRetired))));
    place(next);
    mov(S(v), S(rd(D(v), core_.dims().clone)));
    jump(walk);
    place(done);
    // The predecessor's supports now hold no current reading. A belief
    // means a current boot asserts it, so each is dropped through rtBDrop,
    // settling -pred; its records move to the profile's retired history,
    // where calibration still finds them, bounded below.
    {
      const CellRef sup = reg(), cl = reg(), pred = reg(), nm = reg(),
                    chain = reg();
      const CellRef walk2 = newLabel(), next2 = newLabel(), done2 = newLabel();
      mov(S(v), S(rd(D(prof), core_.dims().clone)));
      place(walk2);
      branchIfZero(v, done2);
      recordOf(v, rec);
      // Moved already (its head is the profile) or detached: nothing to do.
      head(rec, dims_.epistemic, sup);
      mov(S(cl), S(rd(D(sup), dims_.source, true)));
      branchIfZero(cl, next2);
      mov(S(chain), S(rd(D(sup), dims_.epistemic)));
      breakOp(D(sup), dims_.epistemic);
      emitAppendTail(D(prof), dims_.epistemic, dims_.epistemic, chain);
      head(sup, dims_.source, cl);
      head(cl, core_.dims().clause, pred);
      mov(S(nm), D(sup));
      mov(S(a0_), S(pred));
      mov(S(a1_), S(cl));
      mov(S(a2_), S(nm));
      callLabel(rtBDrop_);
      place(next2);
      mov(S(v), S(rd(D(v), core_.dims().clone)));
      jump(walk2);
      place(done2);
    }
    {
      // The retired history keeps the newest `retained` records; the oldest
      // unpinned go first, with their clone occurrences.
      const CellRef n = reg(), r = reg(), over = reg(), pin = reg(),
                    view  = reg();
      const CellRef tally = newLabel(), counted = newLabel(), trim = newLabel(),
                    scan = newLabel(), nextRec = newLabel(),
                    trimmed = newLabel(), noView = newLabel();
      movC(n, zero_);
      mov(S(r), S(rd(D(prof), dims_.epistemic)));
      place(tally);
      branchIfZero(r, counted);
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
      mov(S(r), S(rd(D(r), dims_.epistemic)));
      jump(tally);
      place(counted);
      place(trim);
      emit(OpcodeKind::Gt, "over?", {S(n), D(retained)}, {over});
      requireTrue(over, trimmed);
      mov(S(r), S(rd(D(prof), dims_.epistemic)));
      place(scan);
      branchIfZero(r, trimmed);
      find(r, kObsPinned, pin);
      branchIfZero(pin, nextRec);
      emit(OpcodeKind::Eq, "pinned?", {D(pin), S(true_)}, {t});
      branch(S(t), nextRec);
      find(r, kObsProfile, view);
      branchIfZero(view, noView);
      unlinkFromRank(view, core_.dims().clone);
      place(noView);
      unlinkFromRank(r, dims_.epistemic);
      emit(OpcodeKind::Sub, "n-", {S(n), S(one_)}, {n});
      jump(trim);
      place(nextRec);
      mov(S(r), S(rd(D(r), dims_.epistemic)));
      jump(scan);
      place(trimmed);
    }
    callLabel(rtSettle_);
    freshWindows(prof);
    endObservation(kTurnResultCommitted);
  }
  place(doRetire);
  {
    const CellRef v = reg(), nextV = reg(), rec = reg(), sup = reg(),
                  cl = reg(), pred = reg(), nm = reg(), st = reg(), pin = reg();
    const CellRef walk = newLabel(), detached = newLabel(), kept = newLabel(),
                  next = newLabel(), done = newLabel();
    mov(S(v), S(rd(D(prof), core_.dims().clone)));
    place(walk);
    branchIfZero(v, done);
    mov(S(nextV), S(rd(D(v), core_.dims().clone)));
    recordOf(v, rec);
    find(rec, kObsStatus, st);
    {
      const CellRef none = newLabel();
      branchIfZero(st, none);
      mov(D(st), S(cStr(std::string(kObsStatusRetired))));
      place(none);
    }
    // The record's support, dropped from its clause once; a support no
    // longer on a source rank is already detached.
    head(rec, dims_.epistemic, sup);
    mov(S(cl), S(rd(D(sup), dims_.source, true)));
    branchIfZero(cl, detached);
    head(sup, dims_.source, cl);
    head(cl, core_.dims().clause, pred);
    mov(S(nm), D(sup));
    mov(S(a0_), S(pred));
    mov(S(a1_), S(cl));
    mov(S(a2_), S(nm));
    callLabel(rtBDrop_);
    place(detached);
    // Pinned records stay until I3-4 can release the pin.
    find(rec, kObsPinned, pin);
    branchIfZero(pin, kept);
    emit(OpcodeKind::Eq, "pinned?", {D(pin), S(true_)}, {t});
    branch(S(t), next);
    place(kept);
    unlinkFromRank(v, core_.dims().clone);
    unlinkFromRank(rec, dims_.epistemic);
    place(next);
    mov(S(v), S(nextV));
    jump(walk);
    place(done);
    callLabel(rtSettle_);
    callClearField(prof, std::string(kProfileDedup));
    callClearField(prof, std::string(kProfileStreams));
    setField(prof, kProfileRetired, S(true_));
    const CellRef keep = newLabel();
    branchIfZero(inc, keep);
    emit(OpcodeKind::Lt, "higher?", {D(profInc), D(inc)}, {lower});
    requireTrue(lower, keep);
    mov(D(profInc), D(inc));
    place(keep);
    endObservation(kTurnResultCommitted);
  }
  place(stale);
  endObservation(kTurnResultStaleIncarnation);
  place(malformed);
  endObservation(kTurnResultIngestionFault);
}

/// `observeEntry` (P19 I3-1b): record the checked observation in
/// `$turn.observation` under its source's support and settle. Every exit
/// leaves a code in `$turn_result` and halts, so the adapter can undo a
/// refusal without losing the reason, which `#Fail` would.
///
/// Arbitrated facts have no other writer (the checker refuses program
/// changes and no tell test exists for them), so a support here is always
/// named by an authenticated identity token. A predicate's key arguments
/// decide which of its source's earlier facts a new one replaces; a predicate
/// with none keeps each tuple as its own proposition.
void Gen::emitObserve(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = global(), obs = global(), ty = global(), eq = global(),
                tuple = global(), who = global(), obsName = global();
  const CellRef malformed = newLabel(), invalid = newLabel();
  const CellRef ingest = global(), maxSources = global(),
                dedupWindow = global(), inc = global(), eventId = global(),
                digest = global(), seq = global(), received = global(),
                relayed = global(), historyLimit = global(),
                keysLimit = global(), acquiredAt = global(),
                acquiredUnc = global(), clockEpoch = global(),
                mapping = global(), view = global(), health = global();
  // Records are read in one walk in the order their writer used, checking
  // each name, rather than by a field search per read: the search was most
  // of an ingestion's cost.
  const auto readInOrder =
      [&](CellRef record, std::span<const std::string_view> names,
          const std::map<std::string_view, CellRef> &into, CellRef cursor) {
        mov(S(cursor), S(rd(D(record), core_.dims().vars)));
        branchIfZero(cursor, malformed);
        emit(OpcodeKind::Eq, "mrk?",
             {D(cursor), S(cStr(std::string(kRecordMarker)))}, {eq});
        requireTrue(eq, malformed);
        for (const auto name : names) {
          mov(S(cursor), S(rd(D(cursor), core_.dims().vars)));
          branchIfZero(cursor, malformed);
          emit(OpcodeKind::Eq, "field?",
               {D(cursor), S(cStr(std::string(name)))}, {eq});
          requireTrue(eq, malformed);
          if (const auto out = into.find(name); out != into.end()) {
            mov(S(out->second), S(rd(D(cursor), core_.dims().values)));
            branchIfZero(out->second, malformed);
          }
        }
      };
  // A stored record's fields by position (kObservationOrder), in one walk:
  // every record here was written by the executor in that order.
  const auto readAt = [&](CellRef record,
                          std::vector<std::pair<std::size_t, CellRef>> at) {
    std::ranges::sort(at);
    const CellRef f = reg();
    mov(S(f), S(rd(D(record), core_.dims().vars)));
    std::size_t here = 0;
    for (const auto &[position, out] : at) {
      // A record shorter than the order is malformed, never a read of 0.
      for (; here <= position; ++here) {
        mov(S(f), S(rd(D(f), core_.dims().vars)));
        branchIfZero(f, malformed);
      }
      mov(S(out), S(rd(D(f), core_.dims().values)));
    }
  };
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  mov(S(a0_), S(tp));
  movC(a1_, cStr(std::string(kTurnObservation)));
  callLabel(rtFind_);
  mov(S(obs), S(r0_));
  mov(S(obsName), S(r1_));
  branchIfZero(obs, malformed);
  const CellRef cursor = global();
  readInOrder(obs, kObservationOrder,
              {{kObsProfile, view},
               {kObsHealth, health},
               {kObsPredicate, ty},
               {kObsSource, who},
               {kObsTuple, tuple},
               {kObsIncarnation, inc},
               {kObsEventId, eventId},
               {kObsDigest, digest},
               {kObsSequence, seq},
               {kObsAcquiredAt, acquiredAt},
               {kObsAcquiredUncertainty, acquiredUnc},
               {kObsClockEpoch, clockEpoch},
               {kObsClockMappingVersion, mapping},
               {kObsReceivedSteady, received}},
              cursor);
  {
    // The optional relay follows the required fields.
    const CellRef loop = newLabel(), done = newLabel(), next = newLabel();
    movC(relayed, zero_);
    place(loop);
    mov(S(cursor), S(rd(D(cursor), core_.dims().vars)));
    branchIfZero(cursor, done);
    emit(OpcodeKind::Eq, "relay?",
         {D(cursor), S(cStr(std::string(kObsRelayedBy)))}, {eq});
    requireTrue(eq, next);
    mov(S(relayed), S(rd(D(cursor), core_.dims().values)));
    jump(done);
    place(next);
    jump(loop);
    place(done);
  }
  mov(S(a0_), S(tp));
  movC(a1_, cStr(std::string(kTurnIngest)));
  callLabel(rtFind_);
  mov(S(ingest), S(r0_));
  branchIfZero(ingest, malformed);
  static constexpr std::array<std::string_view, 4> kIngestOrder{
      kIngestMaxSources, kIngestDedup, kIngestHistory, kIngestKeysPerSource};
  readInOrder(ingest, kIngestOrder,
              {{kIngestMaxSources, maxSources},
               {kIngestDedup, dedupWindow},
               {kIngestHistory, historyLimit},
               {kIngestKeysPerSource, keysLimit}},
              cursor);

  // 1. The source's profile: found by its token on `$agent +d.trust`, or
  //    created within the principal budget.
  const CellRef prof = global(), count = global(), full = global();
  {
    const CellRef same = reg(), loop = newLabel(), found = newLabel(),
                  absent = newLabel();
    movC(count, zero_);
    mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
    place(loop);
    branchIfZero(prof, absent);
    emit(OpcodeKind::Eq, "who?", {D(prof), D(who)}, {same});
    branch(S(same), found);
    {
      // A tombstone keeps its replay floor but no share of the budget.
      const CellRef retired = reg(), live = newLabel(), next = newLabel();
      mov(S(a0_), S(prof));
      movC(a1_, cStr(std::string(kProfileRetired)));
      callLabel(rtFind_);
      mov(S(retired), S(r0_));
      branchIfZero(retired, live);
      emit(OpcodeKind::Eq, "tomb?", {D(retired), S(true_)}, {same});
      branch(S(same), next);
      place(live);
      emit(OpcodeKind::Add, "n+", {S(count), S(one_)}, {count});
      place(next);
    }
    mov(S(prof), S(rd(D(prof), dims_.trust)));
    jump(loop);
    place(absent);
    const CellRef admit = newLabel();
    emit(OpcodeKind::Lt, "room?", {S(count), D(maxSources)}, {full});
    branch(S(full), admit);
    endObservation(kTurnResultEpistemicBudget);
    place(admit);
    alloc(prof);
    mov(D(prof), D(who));
    markRecord(prof);
    const auto init = [&](std::string_view name, Opd value) {
      CellRef vc;
      callSetField(prof, std::string(name), std::nullopt, vc);
      mov(D(vc), value);
    };
    init(kProfileIncarnation, D(inc));
    init(kProfileFaulted, S(false_));
    for (const auto counter : {kProfileRejectionStreak, kProfileRejectCount,
                               kProfileContentRefusals, kProfileRefusedCount})
      init(counter, S(zero_));
    const CellRef streams = reg(), seen = reg();
    alloc(streams);
    mov(D(streams), S(cStr(std::string(kProfileStreams))));
    markRecord(streams);
    CellRef vc;
    callSetField(prof, std::string(kProfileStreams), streams, vc);
    alloc(seen);
    mov(D(seen), S(cStr(std::string(kListContent))));
    callSetField(prof, std::string(kProfileDedup), seen, vc);
    emitAppendTail(S(agentCell_), dims_.trust, dims_.trust, prof);
    place(found);
  }
  const auto profileField = [&](std::string_view name, CellRef out) {
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  // 2. The observation carries exactly the profile's incarnation. Ingress
  //    already requires the binding's, and an advance turn runs before any
  //    observation under a new binding, so this is defensive: only binding
  //    advances an incarnation, and a tombstone takes no observation.
  {
    const CellRef profInc = reg(), retired = reg(), same = reg(),
                  live = newLabel(), notLower = newLabel(), ok = newLabel();
    profileField(kProfileRetired, retired);
    branchIfZero(retired, live);
    emit(OpcodeKind::Eq, "tomb?", {D(retired), S(true_)}, {same});
    requireTrue(same, live);
    endObservation(kTurnResultStaleIncarnation);
    place(live);
    profileField(kProfileIncarnation, profInc);
    emit(OpcodeKind::Eq, "inc?", {D(profInc), D(inc)}, {same});
    branch(S(same), ok);
    emit(OpcodeKind::Lt, "lower?", {D(inc), D(profInc)}, {same});
    requireTrue(same, notLower);
    endObservation(kTurnResultStaleIncarnation);
    place(notLower);
    endObservation(kTurnResultUnboundIncarnation);
    place(ok);
  }
  // 3. Exact retries commit nothing; a reused ID with other bytes refuses.
  const CellRef seenList = global();
  profileField(kProfileDedup, seenList);
  {
    const CellRef e = reg(), v = reg(), same = reg(), loop = newLabel(),
                  next = newLabel(), done = newLabel(), dup = newLabel();
    mov(S(e), S(rd(D(seenList), core_.dims().grab)));
    place(loop);
    branchIfZero(e, done);
    // An entry is (marker, incarnation, event ID, digest), as minted below.
    const CellRef f = reg();
    mov(S(f), S(rd(D(e), core_.dims().vars)));
    const auto nextField = [&] {
      mov(S(f), S(rd(D(f), core_.dims().vars)));
      mov(S(v), S(rd(D(f), core_.dims().values)));
    };
    nextField();
    emit(OpcodeKind::Eq, "seen?", {D(v), D(inc)}, {same});
    requireTrue(same, next);
    nextField();
    emit(OpcodeKind::Eq, "seen?", {D(v), D(eventId)}, {same});
    requireTrue(same, next);
    nextField();
    emit(OpcodeKind::Eq, "digest?", {D(v), D(digest)}, {same});
    branch(S(same), dup);
    endObservation(kTurnResultConflictingEvent);
    place(dup);
    endObservation(kTurnResultDuplicate);
    place(next);
    mov(S(e), S(rd(D(e), core_.dims().step)));
    jump(loop);
    place(done);
  }
  // 4. Transport order, per (profile, predicate) stream.
  const CellRef streamsRec = global(), predName = global();
  profileField(kProfileStreams, streamsRec);
  mov(S(predName), D(ty));
  // A stream is (last sequence, clock epoch, mapping version); a regression
  // of the epoch, or of the mapping within one epoch, is a clock fault.
  static constexpr std::array<std::string_view, 3> kStreamOrder{
      kObsSequence, kObsClockEpoch, kObsClockMappingVersion};
  const CellRef stream = global(), lastSeq = global(), lastEpoch = global(),
                lastMapping = global();
  {
    const CellRef stale = reg(), fresh = newLabel(), sameEpoch = newLabel(),
                  ordered = newLabel();
    mov(S(a0_), S(streamsRec));
    mov(S(a1_), S(predName));
    callLabel(rtFind_);
    mov(S(stream), S(r0_));
    branchIfZero(stream, fresh);
    readInOrder(stream, kStreamOrder,
                {{kObsSequence, lastSeq},
                 {kObsClockEpoch, lastEpoch},
                 {kObsClockMappingVersion, lastMapping}},
                cursor);
    emit(OpcodeKind::Lte, "stale?", {D(seq), D(lastSeq)}, {stale});
    const CellRef advancing = newLabel();
    requireTrue(stale, advancing);
    endObservation(kTurnResultStaleSequence);
    place(advancing);
    emit(OpcodeKind::Lt, "epoch<?", {D(clockEpoch), D(lastEpoch)}, {stale});
    requireTrue(stale, sameEpoch);
    endObservation(kTurnResultObservationTimeMismatch);
    place(sameEpoch);
    emit(OpcodeKind::Eq, "epoch=?", {D(clockEpoch), D(lastEpoch)}, {stale});
    requireTrue(stale, ordered);
    emit(OpcodeKind::Lt, "map<?", {D(mapping), D(lastMapping)}, {stale});
    requireTrue(stale, ordered);
    endObservation(kTurnResultObservationTimeMismatch);
    place(ordered);
    place(fresh);
  }
  // Committed only: the profile's view in the record, the dedup entry, the
  // stream and the receipt times.
  const auto bookkeeping = [&](CellRef rec) {
    {
      // The record's profile field already holds the token; its value cell
      // becomes the clone occurrence, spliced posward after the master.
      (void)rec;
      const CellRef old = reg(), lone = newLabel();
      mov(S(old), S(rd(D(prof), core_.dims().clone)));
      breakOp(D(prof), core_.dims().clone);
      linkOp(D(prof), core_.dims().clone, D(view));
      branchIfZero(old, lone);
      linkOp(D(view), core_.dims().clone, D(old));
      place(lone);
    }
    {
      const CellRef entry = reg(), n = reg(), e = reg(), over = reg();
      const CellRef tally = newLabel(), counted = newLabel(), kept = newLabel();
      alloc(entry);
      mov(D(entry), S(cStr("Seen")));
      markRecord(entry);
      for (const auto &[name, value] :
           {std::pair{kObsIncarnation, inc}, std::pair{kObsEventId, eventId},
            std::pair{kObsDigest, digest}}) {
        CellRef vc;
        callSetField(entry, std::string(name), std::nullopt, vc);
        mov(D(vc), D(value));
      }
      callListPush(seenList, entry);
      movC(n, zero_);
      mov(S(e), S(rd(D(seenList), core_.dims().grab)));
      place(tally);
      branchIfZero(e, counted);
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
      mov(S(e), S(rd(D(e), core_.dims().step)));
      jump(tally);
      place(counted);
      emit(OpcodeKind::Gt, "over?", {S(n), D(dedupWindow)}, {over});
      requireTrue(over, kept);
      // The oldest entry is first; its successor heads the window.
      const CellRef first = reg(), second = reg(), last = newLabel();
      mov(S(first), S(rd(D(seenList), core_.dims().grab)));
      mov(S(second), S(rd(D(first), core_.dims().step)));
      breakOp(D(seenList), core_.dims().grab);
      branchIfZero(second, last);
      breakOp(D(first), core_.dims().step);
      linkOp(D(seenList), core_.dims().grab, D(second));
      place(last);
      place(kept);
    }
    {
      const CellRef first = newLabel(), written = newLabel();
      branchIfZero(stream, first);
      mov(D(lastSeq), D(seq));
      mov(D(lastEpoch), D(clockEpoch));
      mov(D(lastMapping), D(mapping));
      jump(written);
      place(first);
      const CellRef fresh = reg();
      alloc(fresh);
      mov(D(fresh), S(cStr(std::string(kProfileStreams))));
      markRecord(fresh);
      for (const auto &[fname, value] :
           {std::pair{kObsSequence, seq}, std::pair{kObsClockEpoch, clockEpoch},
            std::pair{kObsClockMappingVersion, mapping}}) {
        CellRef vc;
        callSetField(fresh, std::string(fname), std::nullopt, vc);
        mov(D(vc), D(value));
      }
      mov(S(a0_), S(streamsRec));
      mov(S(a1_), S(predName));
      mov(S(a2_), S(fresh));
      callLabel(routineLabel(rtSetField_, &Gen::emitRtSetField));
      place(written);
    }
    putScalar(prof, kProfileLastReceive, D(received));
    // Only a directly received event shows the device is alive.
    const CellRef direct = newLabel(), after = newLabel();
    branchIfZero(relayed, direct);
    jump(after);
    place(direct);
    putScalar(prof, kProfileLastContact, D(received));
    place(after);
  };
  for (auto &[name, info] : preds_) {
    if (!info.arbitrated) continue;
    const CellRef next = newLabel();
    emit(OpcodeKind::Eq, "pred?", {D(ty), S(cStr(name))}, {eq});
    requireTrue(eq, next);
    // The fact, rebuilt from the tuple's positional fields.
    const CellRef tr = reg();
    alloc(tr);
    mov(D(tr), S(cStr(name)));
    std::optional<CellRef> prev;
    for (std::size_t i = 0; i < info.argTys.size(); ++i) {
      mov(S(a0_), S(tuple));
      movC(a1_, cStr(std::to_string(i)));
      callLabel(rtFind_);
      const CellRef vp = reg(), cell = reg();
      mov(S(vp), S(r0_));
      branchIfZero(vp, invalid);
      if (info.argTys[i].composite()) {
        callCopy(vp, cell);
      } else {
        alloc(cell);
        mov(D(cell), D(vp));
      }
      if (!prev) {
        linkOp(D(tr), core_.dims().grab, D(cell));
      } else {
        linkOp(D(*prev), core_.dims().step, D(cell));
      }
      prev = cell;
    }
    const CellRef support = reg(), name_ = reg();
    mov(S(name_), D(who));
    movC(keepHistG_, one_);
    loadRef(a0_, info.cell);
    mov(S(a1_), S(tr));
    mov(S(a2_), S(name_));
    // Always replace: a keyless arbitrated predicate is one quantity per
    // source, so its new reading supersedes the last (one implicit key)
    // rather than piling up as further current propositions.
    movC(a3_, one_);
    movC(a4_, cInt(static_cast<std::int64_t>(info.nkeys)));
    callLabel(rtBSet_);
    movC(keepHistG_, zero_);
    mov(S(support), S(r1_));
    // The record is the checked observation itself, detached from `$turn`
    // so it outlives the turn, plus what ingestion decides. Copying it cost
    // a frame per cell and as many slots again.
    const CellRef rec = reg();
    breakOp(D(obsName), core_.dims().values);
    mov(S(rec), S(obs));
    // Status, verdict and pinned arrive at their initial values; health is
    // NOMINAL unless the profile's latch says otherwise.
    {
      const CellRef faulted = reg(), noted = newLabel();
      profileField(kProfileFaulted, faulted);
      branchIfZero(faulted, noted);
      emit(OpcodeKind::Eq, "faulted?", {D(faulted), S(true_)}, {eq});
      requireTrue(eq, noted);
      mov(D(health), S(cStr(std::string(kObsHealthFaulted))));
      place(noted);
    }
    bookkeeping(rec);
    // The source's previous current record on this key is superseded, unless
    // the new reading was acquired before it by more than both uncertainties
    // and the policy's skew in the same clock epoch: arrival order must never
    // let an older reading replace a newer one. Other keys never order each
    // other, so a multi-target sensor may report keys in any order.
    {
      const CellRef r = reg(), st = reg(), cur = reg(), v = reg(), same = reg(),
                    tol = reg(), floor = reg(), early = reg();
      const CellRef loop = newLabel(), skip = newLabel(), done = newLabel(),
                    retire = newLabel();
      mov(S(r), S(rd(D(support), dims_.epistemic)));
      place(loop);
      branchIfZero(r, done);
      const CellRef unc = reg(), when = reg();
      readAt(r, {{observationPosition(kObsStatus), st},
                 {observationPosition(kObsClockEpoch), v},
                 {observationPosition(kObsAcquiredUncertainty), unc},
                 {observationPosition(kObsAcquiredAt), when}});
      branchIfZero(st, skip);
      emit(OpcodeKind::Eq, "cur?",
           {D(st), S(cStr(std::string(kObsStatusCurrent)))}, {cur});
      requireTrue(cur, skip);
      branchIfZero(v, retire);
      {
        // Only a newer clock epoch resets the comparison; an older one
        // must never retire the reading it predates.
        const CellRef notOlder = newLabel();
        emit(OpcodeKind::Lt, "epoch<?", {D(clockEpoch), D(v)}, {same});
        requireTrue(same, notOlder);
        endObservation(kTurnResultObservationTimeMismatch);
        place(notOlder);
      }
      emit(OpcodeKind::Eq, "epoch?", {D(v), D(clockEpoch)}, {same});
      requireTrue(same, retire);
      emit(OpcodeKind::Add, "tol", {D(unc), D(acquiredUnc)}, {tol});
      emit(OpcodeKind::Add, "tol+skew",
           {S(tol), S(cFloat(info.clockSkewSeconds))}, {tol});
      emit(OpcodeKind::Sub, "floor", {D(when), S(tol)}, {floor});
      emit(OpcodeKind::Lt, "early?", {D(acquiredAt), S(floor)}, {early});
      requireTrue(early, retire);
      endObservation(kTurnResultObservationTimeMismatch);
      place(retire);
      mov(D(st), S(cStr(std::string(kObsStatusRetired))));
      place(skip);
      mov(S(r), S(rd(D(r), dims_.epistemic)));
      jump(loop);
      place(done);
    }
    emitAppendTail(D(support), dims_.epistemic, dims_.epistemic, rec);
    // One principal supports at most `keys_per_source` keys of a predicate.
    {
      const CellRef cl = reg(), sp = reg(), same = reg(), n = reg(),
                    over    = reg();
      const CellRef clauses = newLabel(), sources = newLabel(),
                    nextClause = newLabel(), nextSource = newLabel(),
                    counted = newLabel();
      movC(n, zero_);
      mov(S(cl), S(rd(S(info.cell), core_.dims().clause)));
      place(clauses);
      branchIfZero(cl, counted);
      mov(S(sp), S(rd(D(cl), dims_.source)));
      place(sources);
      branchIfZero(sp, nextClause);
      emit(OpcodeKind::Eq, "mine?", {D(sp), D(who)}, {same});
      requireTrue(same, nextSource);
      {
        // A key counts while it holds a current reading: a predecessor
        // boot's retired supports must not crowd out its successor's keys.
        const CellRef r = reg(), st = reg(), scan = newLabel(),
                      live = newLabel(), nextRec = newLabel();
        mov(S(r), S(rd(D(sp), dims_.epistemic)));
        place(scan);
        branchIfZero(r, nextSource);
        readAt(r, {{observationPosition(kObsStatus), st}});
        branchIfZero(st, nextRec);
        emit(OpcodeKind::Eq, "cur?",
             {D(st), S(cStr(std::string(kObsStatusCurrent)))}, {same});
        branch(S(same), live);
        place(nextRec);
        mov(S(r), S(rd(D(r), dims_.epistemic)));
        jump(scan);
        place(live);
      }
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
      place(nextSource);
      mov(S(sp), S(rd(D(sp), dims_.source)));
      jump(sources);
      place(nextClause);
      mov(S(cl), S(rd(D(cl), core_.dims().clause)));
      jump(clauses);
      place(counted);
      const CellRef within = newLabel();
      emit(OpcodeKind::Gt, "keys?", {S(n), D(keysLimit)}, {over});
      requireTrue(over, within);
      endObservation(kTurnResultEpistemicKeyLimit);
      place(within);
    }
    // History bound: excise the oldest unpinned retired records beyond it,
    // each with its clone occurrence; current and pinned records stay.
    {
      const CellRef n = reg(), r = reg(), v = reg(), ok = reg(), over = reg();
      const CellRef tally = newLabel(), counted = newLabel(), trim = newLabel(),
                    scan = newLabel(), nextRec = newLabel(),
                    trimmed = newLabel();
      movC(n, zero_);
      mov(S(r), S(rd(D(support), dims_.epistemic)));
      place(tally);
      branchIfZero(r, counted);
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
      mov(S(r), S(rd(D(r), dims_.epistemic)));
      jump(tally);
      place(counted);
      place(trim);
      emit(OpcodeKind::Gt, "over?", {S(n), D(historyLimit)}, {over});
      requireTrue(over, trimmed);
      mov(S(r), S(rd(D(support), dims_.epistemic)));
      place(scan);
      branchIfZero(r, trimmed);
      const CellRef pin = reg(), st = reg();
      readAt(r, {{observationPosition(kObsStatus), st},
                 {observationPosition(kObsPinned), pin},
                 {observationPosition(kObsProfile), v}});
      for (const auto &[have, want] :
           {std::pair{st, cStr(std::string(kObsStatusRetired))},
            std::pair{pin, false_}}) {
        branchIfZero(have, nextRec);
        emit(OpcodeKind::Eq, "is?", {D(have), S(want)}, {ok});
        requireTrue(ok, nextRec);
      }
      {
        const CellRef noView = newLabel();
        branchIfZero(v, noView);
        unlinkFromRank(v, core_.dims().clone);
        place(noView);
      }
      unlinkFromRank(r, dims_.epistemic);
      emit(OpcodeKind::Sub, "n-", {S(n), S(one_)}, {n});
      jump(trim);
      place(nextRec);
      mov(S(r), S(rd(D(r), dims_.epistemic)));
      jump(scan);
      place(trimmed);
    }
    callLabel(rtSettle_);
    endObservation(kTurnResultCommitted);
    place(next);
  }
  // A predicate this program does not arbitrate is a malformed request.
  place(malformed);
  endObservation(kTurnResultIngestionFault);
  place(invalid);
  endObservation(kTurnResultInvalidObservation);
}

/// The §4.2 settle loop: drain `d.pending` a round at a time. A round detaches
/// the whole queue first, so events a handler raises start the next round and
/// the walk is over cells nothing else touches. A round past
/// `beliefEventDepth_` is a hard failure: `#Fail` reaches the adapter's base
/// choice point, which lands on `failOp` and undoes the turn.
void Gen::emitSettle() {
  beginInternal(rtSettle_);
  const CellRef h = reg(), e = reg(), ty = reg(), over = reg(), eq = reg();
  const CellRef loop = newLabel(), inner = newLabel(), nextEv = newLabel(),
                done = newLabel(), tooDeep = newLabel(), fine = newLabel();
  movC(roundsG_, zero_);
  place(loop);
  mov(S(h), S(rd(S(agentCell_), dims_.pending)));
  branchIfZero(h, done);
  emit(OpcodeKind::Add, "round+", {S(roundsG_), S(one_)}, {roundsG_});
  emit(OpcodeKind::Gt, "deep?",
       {S(roundsG_), S(cInt(static_cast<std::int64_t>(beliefEventDepth_)))},
       {over});
  branch(S(over), tooDeep);
  jump(fine);
  place(tooDeep);
  failNow();
  place(fine);
  breakOp(S(agentCell_), dims_.pending);
  mov(S(e), S(h));
  place(inner);
  branchIfZero(e, loop);
  mov(S(curEvent_), S(e));
  mov(S(fields_.at(std::string(kFieldConsumed)).valueCell), S(false_));
  mov(S(ty), D(e));
  std::vector<std::pair<std::string, CellRef>> seqs;
  std::set<std::string> types;
  for (const HandlerInfo &hi : handlers_) {
    if (hi.beliefTerm != nullptr) types.insert(hi.eventType);
  }
  for (const std::string &t : types) {
    const CellRef seq = newLabel();
    emit(OpcodeKind::Eq, "ty?", {S(ty), S(cStr(t))}, {eq});
    branch(S(eq), seq);
    seqs.emplace_back(t, seq);
  }
  jump(nextEv);
  for (const auto &[t, seq] : seqs) {
    place(seq);
    for (const HandlerInfo *hp : mergedFor(t)) {
      emitHandlerRun(*hp, true, nextEv);
    }
    jump(nextEv);
  }
  place(nextEv);
  mov(S(e), S(rd(D(e), dims_.pending)));
  jump(inner);
  place(done);
  ret();
}

Gen::TermPlan Gen::planTerm(const Term &t, bool allowVars) {
  const PredInfo &p = predOf(t);
  TermPlan pl;
  pl.pred = &p;
  pl.name = t.name;
  for (std::size_t i = 0; i < t.args.size(); ++i) {
    const TermArg &a = t.args[i];
    const Ty &want   = p.argTys[i];
    ArgPlan ap;
    const auto asValue = [&](CellRef r, const Ty &got) {
      if ((got.k == Ty::K::List) != (want.k == Ty::K::List)) {
        bail(CompileError::TypeMismatch,
             "argument " + std::to_string(i + 1) + " of " + t.name, t.loc);
      }
      ap.k         = ArgPlan::K::Value;
      ap.reg       = r;
      ap.composite = want.composite();
      // A Float position holds a Float even when the program wrote `3`, so a
      // later `#Eq` or `#Unify` compares like with like.
      if (want.k == Ty::K::Float && got.k == Ty::K::Int) {
        const CellRef conv = reg();
        emit(OpcodeKind::Add, "tofloat", {S(r), S(cFloat(0.0))}, {conv});
        ap.reg = conv;
      }
    };
    if (a.logicVar) {
      if (!allowVars) unsupported("a ?variable outside a query", t.loc);
      const std::string &nm = *a.logicVar;
      const auto dup        = std::ranges::find_if(
          pl.vars, [&](const PlanVar &v) { return v.name == nm; });
      if (dup != pl.vars.end()) {
        ap.k     = ArgPlan::K::Dup;
        ap.first = static_cast<std::size_t>(dup - pl.vars.begin());
      } else if (const Local *l = findLocal(nm)) {
        if (l->ty.composite()) {
          const CellRef held = reg();
          mov(S(held), S(l->reg));
          asValue(held, l->ty);
        } else {
          asValue(l->reg, l->ty);
        }
      } else {
        PlanVar v;
        v.cap   = reg();
        v.name  = nm;
        v.ty    = want;
        v.local = declareLocal(nm, want);
        pl.vars.push_back(std::move(v));
        ap.k = ArgPlan::K::Var;
      }
    } else if (a.expr != nullptr) {
      if (const auto *n = std::get_if<EName>(&a.expr->node);
          n != nullptr && n->name == "_") {
        if (!allowVars) unsupported("a wildcard outside a query", t.loc);
        ap.k = ArgPlan::K::Wild;
      } else {
        const CellRef v = reg();
        const Ty got    = valueInto(*a.expr, v);
        asValue(v, got);
      }
    }
    pl.args.push_back(ap);
  }
  declarePlanVars(pl);
  return pl;
}

void Gen::declarePlanVars(const TermPlan &pl) {
  for (const PlanVar &v : pl.vars) {
    scopes_.back()[v.name] = Local{v.local, v.ty};
  }
}

/// A fresh term in a new register: nothing in it is shared with a register or
/// a clause, because each argument cell can sit on one `d.step` rank only.
CellRef Gen::buildTerm(const TermPlan &pl) {
  const CellRef tr = reg();
  alloc(tr);
  mov(D(tr), S(cStr(pl.name)));
  std::optional<CellRef> prev;
  for (const ArgPlan &ap : pl.args) {
    const CellRef cell = reg();
    if (ap.composite) {
      callCopy(ap.reg, cell);
    } else {
      alloc(cell);
      mov(D(cell), S(ap.reg));
    }
    if (!prev) {
      linkOp(D(tr), core_.dims().grab, D(cell));
    } else {
      linkOp(D(*prev), core_.dims().step, D(cell));
    }
    prev = cell;
  }
  return tr;
}

/// Jump to `noMatch` unless the clause term in `termReg` fits the plan. On a
/// match the `?v` captures hold the refs of the clause's argument cells.
void Gen::emitMatch(const TermPlan &pl, CellRef termReg, CellRef noMatch) {
  if (pl.args.empty()) return;
  const CellRef a = reg(), t = reg();
  mov(S(a), S(rd(D(termReg), core_.dims().grab)));
  std::size_t varIndex = 0;
  for (std::size_t i = 0; i < pl.args.size(); ++i) {
    const ArgPlan &ap = pl.args[i];
    switch (ap.k) {
    case ArgPlan::K::Wild:
      break;
    case ArgPlan::K::Value:
      if (ap.composite) {
        emit(OpcodeKind::Unify, "match", {D(ap.reg), D(a)}, {t});
      } else {
        emit(OpcodeKind::Eq, "match", {S(ap.reg), D(a)}, {t});
      }
      requireTrue(t, noMatch);
      break;
    case ArgPlan::K::Var:
      mov(S(pl.vars[varIndex++].cap), S(a));
      break;
    case ArgPlan::K::Dup:
      emit(OpcodeKind::Unify, "same?", {D(pl.vars[ap.first].cap), D(a)}, {t});
      requireTrue(t, noMatch);
      break;
    }
    if (i + 1 < pl.args.size()) mov(S(a), S(rd(D(a), core_.dims().step)));
  }
}

void Gen::bindVars(const TermPlan &pl) {
  for (const PlanVar &v : pl.vars) {
    if (v.ty.composite()) {
      const CellRef cp = reg();
      callCopy(v.cap, cp);
      assignRef(v.local, cp, true);
    } else {
      mov(S(v.local), D(v.cap));
    }
  }
}

/// §6.1: a copy of the clause list taken first, so a body that believes or
/// forgets does not change what the loop visits. Each element holds a clause
/// ref.
CellRef Gen::snapshotClauses(const PredInfo &p) {
  const CellRef snap = reg(), c = reg(), tail = reg();
  alloc(snap);
  mov(D(snap), S(cStr(std::string(kListContent))));
  movC(tail, zero_);
  mov(S(c), S(rd(S(p.cell), core_.dims().clause)));
  const CellRef loop = newLabel(), done = newLabel();
  place(loop);
  branchIfZero(c, done);
  const CellRef cell = reg();
  alloc(cell);
  mov(D(cell), S(c));
  pushTail(snap, tail, cell);
  mov(S(c), S(rd(D(c), core_.dims().clause)));
  jump(loop);
  place(done);
  return snap;
}

/// Append to a list through a tail register that starts at 0, so building n
/// elements is O(n) and not the O(n^2) of `rtListPush_` walking to the tail.
void Gen::pushTail(CellRef listReg, CellRef tailReg, CellRef cell) {
  const CellRef first = newLabel(), done = newLabel();
  branchIfZero(tailReg, first);
  linkOp(D(tailReg), core_.dims().step, D(cell));
  jump(done);
  place(first);
  linkOp(D(listReg), core_.dims().grab, D(cell));
  place(done);
  mov(S(tailReg), S(cell));
}

Ty Gen::believesExpr(const EBelieves &b, CellRef out) {
  const TermPlan pl = planTerm(b.term, true);
  std::optional<CellRef> fromReg;
  if (b.from) {
    fromReg = reg();
    expr(*b.from, *fromReg);
  }
  const CellRef c = reg(), ct = reg();
  const CellRef loop = newLabel(), noMatch = newLabel(), next = newLabel(),
                after = newLabel(), exhausted = newLabel();
  mov(S(c), S(rd(S(pl.pred->cell), core_.dims().clause)));
  place(loop);
  branchIfZero(c, exhausted);
  mov(S(ct), S(rd(D(c), core_.dims().grab)));
  emitMatch(pl, ct, noMatch);
  if (fromReg) {
    mov(S(a0_), S(c));
    mov(S(a1_), S(*fromReg));
    callLabel(rtHasSrc_);
    branchIfZero(r0_, noMatch);
  }
  // The alternative resumes with the next clause; the captures are copied into
  // the locals after the choice point, so backtracking restores them.
  emit(OpcodeKind::Choice, "bel", {S(opref(next))});
  bindVars(pl);
  jump(after);
  place(noMatch);
  place(next);
  mov(S(c), S(rd(D(c), core_.dims().clause)));
  jump(loop);
  place(exhausted);
  failNow();
  place(after);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::findallExpr(const EFindall &f, CellRef out, SourceLocation loc) {
  pushScope();
  const TermPlan pl = planTerm(f.goal, true);
  const CellRef res = reg(), tail = reg();
  alloc(res);
  mov(D(res), S(cStr(std::string(kListContent))));
  movC(tail, zero_);
  const CellRef snap = snapshotClauses(*pl.pred);
  Ty elemTy          = tyOf(Ty::K::Unit);
  listWalk(snap, [&](CellRef e) {
    const CellRef cl = reg(), ct = reg(), noMatch = newLabel();
    mov(S(cl), D(e));
    mov(S(ct), S(rd(D(cl), core_.dims().grab)));
    emitMatch(pl, ct, noMatch);
    bindVars(pl);
    const CellRef v = reg(), cell = reg();
    if (f.templ.logicVar) {
      const Local *l = findLocal(*f.templ.logicVar);
      if (l == nullptr) {
        bail(CompileError::UnknownName,
             "?" + *f.templ.logicVar + " does not occur in the goal", loc);
      }
      mov(S(v), S(l->reg));
      elemTy = l->ty;
    } else if (f.templ.expr != nullptr) {
      elemTy = valueInto(*f.templ.expr, v);
    } else {
      unsupported("findall without a template", loc);
    }
    if (elemTy.composite()) {
      callCopy(v, cell);
    } else {
      alloc(cell);
      mov(D(cell), S(v));
    }
    pushTail(res, tail, cell);
    place(noMatch);
  });
  popScope();
  mov(S(out), S(res));
  Ty t = tyOf(Ty::K::List);
  t.args.push_back(elemTy);
  return t;
}

Ty Gen::forBelieves(const EFor &f, CellRef out) {
  pushScope();
  const TermPlan pl     = planTerm(*f.believesTerm, true);
  const CellRef snap    = snapshotClauses(*pl.pred);
  const CellRef bodyOut = reg();
  listWalk(snap, [&](CellRef e) {
    const CellRef cl = reg(), ct = reg(), noMatch = newLabel();
    mov(S(cl), D(e));
    mov(S(ct), S(rd(D(cl), core_.dims().grab)));
    emitMatch(pl, ct, noMatch);
    bindVars(pl);
    stmtWrap([&] { expr(*f.body, bodyOut); });
    place(noMatch);
  });
  movC(out, unit_c_);
  popScope();
  return tyOf(Ty::K::Unit);
}

Ty Gen::beliefChange(EBeliefChange::Kind kind, const Term &t, CellRef out) {
  using Kind        = EBeliefChange::Kind;
  const PredInfo &p = predOf(t);
  // The checker refuses this with EpistemicIngressOnly; reaching rtBSet or
  // rtBDrop here would let a program forge an authenticated support.
  if (p.arbitrated) {
    bail(CompileError::Unsupported,
         "belief change on the arbitrated predicate " + t.name, t.loc);
  }
  const CellRef who = reg();
  selfName(who);
  if (kind == Kind::Forget) {
    pushScope();
    const TermPlan pl  = planTerm(t, true);
    const CellRef snap = snapshotClauses(p);
    listWalk(snap, [&](CellRef e) {
      const CellRef cl = reg(), ct = reg(), noMatch = newLabel();
      mov(S(cl), D(e));
      mov(S(ct), S(rd(D(cl), core_.dims().grab)));
      emitMatch(pl, ct, noMatch);
      loadRef(a0_, p.cell);
      mov(S(a1_), S(cl));
      mov(S(a2_), S(who));
      callLabel(rtBDrop_);
      place(noMatch);
    });
    popScope();
  } else {
    const TermPlan pl = planTerm(t, false);
    const CellRef tr  = buildTerm(pl);
    const bool repl   = kind == Kind::Revise || p.nkeys > 0;
    loadRef(a0_, p.cell);
    mov(S(a1_), S(tr));
    mov(S(a2_), S(who));
    movC(a3_, repl ? one_ : zero_);
    movC(a4_, cInt(static_cast<std::int64_t>(p.nkeys)));
    callLabel(rtBSet_);
  }
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

/// The routine a belief handler runs first, under the handler's choice point:
/// match the trigger against the fact the pending event carries and declare
/// the trigger's `?v` locals for the guard and body that follow.
void Gen::emitBeliefBind(HandlerInfo &h) {
  const CellRef entry = newLabel();
  h.bindEntry         = entry;
  beginHandlerCtx(h);
  place(entry);
  const CellRef noMatch = newLabel();
  const TermPlan pl     = planTerm(*h.beliefTerm, true);
  const CellRef ct      = reg();
  mov(S(ct), S(rd(D(curEvent_), core_.dims().grab)));
  emitMatch(pl, ct, noMatch);
  bindVars(pl);
  for (const PlanVar &v : pl.vars) {
    h.beliefLocals.emplace_back(v.name, Local{v.local, v.ty});
  }
  jump(ctx_.epi);
  place(noMatch);
  failNow();
  place(ctx_.epi);
  ret();
}

// ---------------------------------------------------------------------------
// tell, untell, stop, terminate (design §4.5, §7.1, §8.6)
//
// `tell`/`untell` stage one outbox entry carrying the fact and the recipients.
// The receiver half is compiled code reached from the dispatcher: the adapter
// mints the received envelope as an event whose type is `$tell <pred>` /
// `$untell <pred>` (runtime_abi.hpp), the dispatcher branches on it as it does
// on a user event type, and the routine below adds or removes the sender as a
// source through the `rtBSet_`/`rtBDrop_` that `believe` and `forget` use. A
// `+`/`-` handler therefore fires only on a change, and a keyed predicate
// replaces the sender's earlier fact. It is compiled code, not adapter C++,
// because beliefs are cells of the agent's own arena and only compiled code
// can raise the belief events the settle loop drains.
// ---------------------------------------------------------------------------

Ty Gen::tellExpr(const ETell &t, CellRef out, SourceLocation loc) {
  (void)predOf(t.fact);
  const CellRef to = reg();
  const Ty tt      = valueInto(*t.to, to);
  CellRef forList  = to;
  if (tt.k == Ty::K::Str) {
    const CellRef cell = reg();
    forList            = reg();
    alloc(forList);
    mov(D(forList), S(cStr(std::string(kListContent))));
    alloc(cell);
    mov(D(cell), S(to));
    callListPush(forList, cell);
  } else if (tt.k != Ty::K::List) {
    bail(CompileError::TypeMismatch, "tell needs an agent or a list", loc);
  }
  const TermPlan pl = planTerm(t.fact, false);
  const CellRef rec = reg();
  alloc(rec);
  mov(D(rec), S(cStr(t.fact.name)));
  markRecord(rec);
  for (std::size_t i = 0; i < pl.args.size(); ++i) {
    const ArgPlan &ap = pl.args[i];
    CellRef vc;
    if (ap.composite) {
      const CellRef cp = reg();
      callCopy(ap.reg, cp);
      callAppendField(rec, std::to_string(i), cp, vc);
    } else {
      callAppendField(rec, std::to_string(i), std::nullopt, vc);
      mov(D(vc), S(ap.reg));
    }
  }
  const CellRef entry = callOutbox(t.untell ? kKindUntell : kKindTell);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxFact), rec, vc);
  callAppendField(entry, std::string(kOutboxFor), forList, vc);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::stopExpr(const EStop &s, CellRef out, SourceLocation loc) {
  const CellRef who = reg();
  const Ty t        = valueInto(*s.target, who);
  if (t.k != Ty::K::Str) {
    bail(CompileError::TypeMismatch, "stop needs an agent", loc);
  }
  const CellRef entry = callOutbox(kKindStop);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxStopChild), std::nullopt, vc);
  mov(D(vc), S(who));
  callAppendField(entry, std::string(kOutboxStopKeep), std::nullopt, vc);
  mov(D(vc), S(cBool(s.keepChildren)));
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::terminateExpr(const ETerminate &t, CellRef out) {
  // The orderly stop is the adapter's, after this turn commits: a turn that
  // fails after `terminate` is undone, flag included.
  mov(S(fields_.at(std::string(kFieldTerminating)).valueCell), S(true_));
  mov(S(fields_.at(std::string(kFieldKeepChildren)).valueCell),
      S(cBool(t.keepChildren)));
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

/// One test per predicate and verb, emitted beside the dispatcher's own tests.
std::vector<Gen::TellSeq> Gen::emitTellTests(CellRef ty, CellRef eq) {
  std::vector<TellSeq> seqs;
  for (auto &[name, info] : preds_) {
    // A peer's tell is not authenticated ingress; with no test it is counted
    // unhandled like any undeclared event.
    if (info.arbitrated) continue;
    for (const bool untell : {false, true}) {
      TellSeq s{&name, &info, untell, newLabel()};
      const std::string type =
          std::string(untell ? kUntellEventPrefix : kTellEventPrefix) + name;
      emit(OpcodeKind::Eq, "tell?", {S(ty), S(cStr(type))}, {eq});
      branch(S(eq), s.label);
      seqs.push_back(s);
    }
  }
  return seqs;
}

void Gen::emitTellBodies(const std::vector<TellSeq> &seqs, CellRef end) {
  for (const TellSeq &s : seqs) {
    if (s.info->arbitrated) {
      bail(CompileError::Unsupported,
           "tell body for the arbitrated predicate " + *s.pred, {});
    }
    place(s.label);
    const PredInfo &p = *s.info;
    // The fact is rebuilt from the event's positional fields; a field the
    // sender left out makes the message malformed and it is ignored.
    const CellRef tr = reg();
    alloc(tr);
    mov(D(tr), S(cStr(*s.pred)));
    std::optional<CellRef> prev;
    for (std::size_t i = 0; i < p.argTys.size(); ++i) {
      mov(S(a0_), S(curEvent_));
      movC(a1_, cStr(std::to_string(i)));
      callLabel(rtFind_);
      const CellRef vp = reg(), cell = reg();
      mov(S(vp), S(r0_));
      branchIfZero(vp, end);
      if (p.argTys[i].composite()) {
        callCopy(vp, cell);
      } else {
        alloc(cell);
        mov(D(cell), D(vp));
      }
      if (!prev) {
        linkOp(D(tr), core_.dims().grab, D(cell));
      } else {
        linkOp(D(*prev), core_.dims().step, D(cell));
      }
      prev = cell;
    }
    const CellRef who = reg(), sp = reg();
    mov(S(a0_), S(curEvent_));
    movC(a1_, cStr(std::string(kEventImplicitSource)));
    callLabel(rtFind_);
    mov(S(sp), S(r0_));
    branchIfZero(sp, end);
    // `source` is an `Opt<Agent>`; a tell from no one is malformed.
    const CellRef named = rd(D(sp), core_.dims().grab);
    branchIfZero(named, end);
    mov(S(who), D(named));
    if (!s.untell) {
      loadRef(a0_, p.cell);
      mov(S(a1_), S(tr));
      mov(S(a2_), S(who));
      movC(a3_, p.nkeys > 0 ? one_ : zero_);
      movC(a4_, cInt(static_cast<std::int64_t>(p.nkeys)));
      callLabel(rtBSet_);
    } else {
      // Walk a snapshot: dropping the last source unlinks the clause.
      const CellRef snap = snapshotClauses(p);
      listWalk(snap, [&](CellRef e) {
        const CellRef cl = reg(), ct = reg(), same = reg();
        const CellRef skip = newLabel();
        mov(S(cl), D(e));
        mov(S(ct), S(rd(D(cl), core_.dims().grab)));
        emit(OpcodeKind::Unify, "same", {D(tr), D(ct)}, {same});
        requireTrue(same, skip);
        loadRef(a0_, p.cell);
        mov(S(a1_), S(cl));
        mov(S(a2_), S(who));
        callLabel(rtBDrop_);
        place(skip);
      });
    }
    jump(end);
  }
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

void Gen::collect() {
  for (const TopDecl &d : unit_.decls) {
    if (const auto *e = std::get_if<EventDecl>(&d)) {
      if (!events_.emplace(e->name, e).second) {
        bail(CompileError::DuplicateName, "event " + e->name, e->loc);
      }
    } else if (const auto *r = std::get_if<RecordDecl>(&d)) {
      if (!records_.emplace(r->name, r).second) {
        bail(CompileError::DuplicateName, "record " + r->name, r->loc);
      }
    } else if (const auto *a = std::get_if<AgentDecl>(&d)) {
      if (!agents_.emplace(a->name, a).second) {
        bail(CompileError::DuplicateName, "agent " + a->name, a->loc);
      }
    }
  }
  const auto it = agents_.find(agentType_);
  if (it == agents_.end()) {
    bail(CompileError::UnknownAgent, "no agent " + agentType_);
  }
  agent_ = it->second;
}

void Gen::mintAgentCell() {
  agentCell_ = arena_.makeCell(agentType_);
  stateCells_.insert(agentCell_);
  for (const Param &p : agent_->params) {
    if (fields_.contains(p.name)) {
      bail(CompileError::DuplicateName, p.name, p.loc);
    }
    addField(p.name, fromType(p.type, p.loc));
  }
  for (const Member &m : agent_->members) {
    if (const auto *v = std::get_if<VarDecl>(&m)) {
      if (fields_.contains(v->name)) {
        bail(CompileError::DuplicateName, v->name, v->loc);
      }
      addField(v->name, fromType(v->type, v->loc));
    }
  }
  const auto reserved = [&](std::string_view n, const Ty &t) {
    addField(std::string(n), t);
  };
  const Ty str = tyOf(Ty::K::Str), num = tyOf(Ty::K::Int),
           flag = tyOf(Ty::K::Bool);
  Ty bare       = tyOf(Ty::K::Unit);
  Ty lst        = tyOf(Ty::K::List);
  reserved(kFieldName, str);
  Ty maybeStr = tyOf(Ty::K::Opt);
  maybeStr.args.push_back(str);
  reserved(kFieldParent, maybeStr);
  reserved(kFieldOuter, str);
  reserved(kFieldInitialized, flag);
  reserved(kFieldPeers, bare);
  reserved(kFieldNextId, num);
  reserved(kFieldSpawnCount, num);
  reserved(kFieldTurnCount, num);
  reserved(kFieldTimers, lst);
  reserved(kFieldTurn, bare);
  reserved(kFieldTerminating, flag);
  reserved(kFieldCancelled, lst);
  reserved(kFieldStats, bare);
  reserved(kFieldOrdinal, num);
  reserved(kFieldConsumed, flag);
  // Only an agent that ingests observations reports typed turn results.
  if (std::ranges::any_of(unit_.decls, [](const TopDecl &d) {
        const auto *p = std::get_if<PredicateDecl>(&d);
        return p != nullptr && p->arbitration.has_value();
      }))
    reserved(kFieldTurnResult, str);
  reserved(kFieldKeepChildren, flag);
  // Unit-typed reserved fields are read dynamically through their name cell.
  fields_.at(std::string(kFieldTurn)).ty = tyOf(Ty::K::Record);
}

void Gen::prepareMethods() {
  prepareMethodSet(methods_, agent_->members, nullptr);
}

/// True when @p e reaches code the static call graph cannot see: a capacity
/// call (dispatched on whatever skill is installed then) or an install, whose
/// `on install` block runs arbitrary methods. Such a method may re-enter
/// itself, so it saves its registers like a recursive one.
static bool hasDynamicCall(const Expr &e,
                           const std::map<std::string, MethodInfo> &set) {
  if (std::holds_alternative<EInstall>(e.node)) return true;
  if (const auto *c = std::get_if<ECall>(&e.node)) {
    const auto *nm = std::get_if<EName>(&c->callee->node);
    if (nm == nullptr || (nm->name != "text" && !set.contains(nm->name))) {
      return true;
    }
  }
  bool found = false;
  forEachChild(e, [&](const Expr &child) {
    if (!found && hasDynamicCall(child, set)) found = true;
  });
  return found;
}

void Gen::prepareMethodSet(std::map<std::string, MethodInfo> &set,
                           const std::vector<Member> &members,
                           Container *owner) {
  std::map<std::string, std::set<std::string>> calls;
  for (const Member &m : members) {
    if (const auto *md = std::get_if<MethodDef>(&m)) {
      if (md->sig.isEffect) unsupported("effect methods", md->sig.loc);
      if (md->sig.name == "text") {
        bail(CompileError::DuplicateName, "text is built in", md->sig.loc);
      }
      MethodInfo mi;
      mi.def   = md;
      mi.owner = owner;
      if (set.contains(md->sig.name)) {
        bail(CompileError::DuplicateName, md->sig.name, md->sig.loc);
      }
      if (md->body) collectCallees(*md->body, calls[md->sig.name]);
      set.emplace(md->sig.name, std::move(mi));
    }
  }
  for (auto &[name, mi] : set) {
    // Reachability from the method back to itself.
    std::set<std::string> seen;
    std::vector<std::string> work(calls[name].begin(), calls[name].end());
    while (!work.empty()) {
      const std::string c = work.back();
      work.pop_back();
      if (c == name) {
        mi.cyclic = true;
        break;
      }
      if (!seen.insert(c).second || !set.contains(c)) continue;
      for (const auto &n2 : calls[c]) work.push_back(n2);
    }
    if (!mi.cyclic && mi.def->body && hasDynamicCall(*mi.def->body, set)) {
      mi.cyclic = true;
    }
    mi.plan.cyclic = mi.cyclic;
    for (const Param &p : mi.def->sig.params) {
      mi.paramTys.push_back(fromType(p.type, p.loc));
      mi.argRegs.push_back(global());
    }
    mi.ret = mi.def->sig.result ? fromType(*mi.def->sig.result, mi.def->sig.loc)
                                : tyOf(Ty::K::Unit);
    mi.entry     = newLabel();
    mi.bodyStart = newLabel();
    mi.pro       = newLabel();
    mi.epi       = newLabel();
    mi.retReg    = global();
  }
}

void Gen::emitMethod(MethodInfo &m) {
  own_   = m.owner;
  tail_  = std::nullopt;
  frame_ = &m.plan;
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  place(m.entry);
  jump(m.pro);
  place(m.bodyStart);
  for (std::size_t i = 0; i < m.paramTys.size(); ++i) {
    const Param &p  = m.def->sig.params[i];
    const CellRef h = declareLocal(p.name, m.paramTys[i]);
    m.paramHandles.push_back(h);
    scopes_.back()[p.name] = Local{h, m.paramTys[i]};
  }
  ctx_        = Ctx{};
  ctx_.retReg = m.retReg;
  ctx_.epi    = m.epi;
  ctx_.d0     = reg();
  emit(OpcodeKind::ChoiceDepth, "d0", {}, {ctx_.d0});
  if (m.def->body) {
    valueInto(*m.def->body, m.retReg);
  } else {
    movC(m.retReg, unit_c_);
  }
  popScope();
  place(m.epi);
  if (!m.plan.empty()) emitFramePop();
  ret();
  // The frame is built last, once the body has said how many slots it needs.
  place(m.pro);
  if (!m.plan.empty()) emitFramePush(m.plan);
  for (std::size_t i = 0; i < m.paramHandles.size(); ++i) {
    mov(S(m.paramHandles[i]), S(m.argRegs[i]));
  }
  jump(m.bodyStart);
  frame_ = nullptr;
  if (m.owner == nullptr) emitFrameBody(m);
}

/// The method's body as a unit a host can run against a frame it names. The
/// frame is adopted onto the stack top for the run and unstacked after it, so
/// the body's own epilogue is unchanged; between runs the frame is just a cell
/// holding the locals.
void Gen::emitFrameBody(MethodInfo &m) {
  // Built whole: every field is a real cell, none has an unset value.
  FrameBody fb{.frame  = mintHandle(),
               .make   = newLabel(),
               .enter  = newLabel(),
               .args   = m.argRegs,
               .result = m.retReg};
  const CellRef makeBody = newLabel(), enterBody = newLabel();

  tail_ = std::nullopt;
  place(fb.make);
  callLabel(makeBody);
  jump(haltOp_);
  tail_ = std::nullopt;
  place(makeBody);
  emitFramePush(m.plan);
  for (std::size_t i = 0; i < m.paramHandles.size(); ++i) {
    mov(S(m.paramHandles[i]), S(m.argRegs[i]));
  }
  emit(OpcodeKind::Deref, "fref", {S(frameH_)}, {refTmp_});
  pointOp(fb.frame, refTmp_);
  emitFramePop();
  ret();

  tail_ = std::nullopt;
  place(fb.enter);
  callLabel(enterBody);
  jump(haltOp_);
  tail_ = std::nullopt;
  place(enterBody);
  emit(OpcodeKind::Deref, "fref", {S(fb.frame)}, {refTmp_});
  emit(OpcodeKind::Link, "fadopt",
       {S(fb.frame), S(cInt(core_.dims().stack)), S(one_), S(frameH_)});
  pointOp(frameH_, refTmp_);
  frame_ = &m.plan;
  emitFrameBind(m.plan, m.plan.locals.size(), m.plan.pushTemps);
  frame_ = nullptr;
  jump(m.bodyStart);
  frameBodies_.emplace(m.def->sig.name, std::move(fb));
}

void Gen::beginHandlerCtx(HandlerInfo &h) {
  own_   = h.owner;
  tail_  = std::nullopt;
  frame_ = &h.plan;
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_           = Ctx{};
  ctx_.retReg    = scratchG_;
  ctx_.epi       = newLabel();
  ctx_.inHandler = true;
  Ty et          = tyOf(Ty::K::Record);
  et.name        = h.eventType;
  ctx_.eventTy   = et;
  if (h.binder) scopes_.back()[*h.binder] = Local{curEvent_, et};
  for (const auto &[n, l] : h.beliefLocals) scopes_.back()[n] = l;
}

void Gen::emitHandlerRoutines(HandlerInfo &h) {
  if (h.beliefTerm == nullptr && !events_.contains(h.eventType) &&
      findPreludeEvent(h.eventType) == nullptr) {
    bail(CompileError::UnknownType, "unknown event " + h.eventType, h.def->loc);
  }
  const auto routine = [&](CellRef entry, const std::function<void()> &body) {
    beginHandlerCtx(h);
    place(entry);
    ctx_.d0 = global();
    emit(OpcodeKind::ChoiceDepth, "d0", {}, {ctx_.d0});
    body();
    place(ctx_.epi);
    ret();
  };
  if (h.beliefTerm != nullptr) emitBeliefBind(h);
  if (h.def->guard) {
    const CellRef entry = newLabel();
    h.guardEntry        = entry;
    routine(entry, [&] {
      const CellRef g = global();
      expr(*h.def->guard, g);
      emit(OpcodeKind::Cut, "cut", {S(ctx_.d0)});
    });
  }
  h.bodyEntry = newLabel();
  routine(h.bodyEntry, [&] {
    if (h.def->body) {
      const CellRef o = global();
      expr(*h.def->body, o);
    }
  });
  emitHandlerFrame(h);
}

/// The routine that builds the frame bind, guard and body share. It is
/// emitted once the three have said how many slots they need.
void Gen::emitHandlerFrame(HandlerInfo &h) {
  frame_ = nullptr;
  if (h.plan.empty()) return;
  const CellRef entry = newLabel();
  h.pushEntry         = entry;
  tail_               = std::nullopt;
  place(entry);
  emitFramePush(h.plan);
  ret();
}

std::vector<const HandlerInfo *> Gen::mergedFor(const std::string &type) {
  std::set<std::string> types{type};
  for (auto &s : supertypesOf(type)) types.insert(s);
  std::vector<const HandlerInfo *> out;
  for (const HandlerInfo &h : handlers_) {
    if (types.contains(h.eventType)) out.push_back(&h);
  }
  std::ranges::stable_sort(out, [](const HandlerInfo *a, const HandlerInfo *b) {
    if (a->priority != b->priority) return a->priority > b->priority;
    return a->index < b->index;
  });
  return out;
}

/// The §9.2 idiom: a choice point of its own around guard and body, so a
/// failure anywhere undoes exactly this handler's writes.
void Gen::emitHandlerRun(const HandlerInfo &h, bool checkConsumed,
                         CellRef end) {
  const CellRef next = newLabel();
  const CellRef hd   = global();
  if (h.owner != nullptr) {
    // A behavior's handlers are in the merged rank always; it answers only
    // while it is on the agent's d.behavior rank.
    branchIfZero(rd(S(h.owner->cell), dims_.behavior, true), next);
  }
  emit(OpcodeKind::ChoiceDepth, "h", {}, {hd});
  emit(OpcodeKind::Choice, "next", {S(opref(next))});
  if (h.pushEntry) callLabel(*h.pushEntry);
  if (h.bindEntry) callLabel(*h.bindEntry);
  if (h.guardEntry) callLabel(*h.guardEntry);
  callLabel(h.bodyEntry);
  if (h.pushEntry) emitFramePop();
  emit(OpcodeKind::Cut, "cut", {S(hd)});
  if (checkConsumed) {
    branch(S(fields_.at(std::string(kFieldConsumed)).valueCell), end);
  }
  place(next);
}

CellRef Gen::emitFieldDefault(const VarDecl &field, Container *owner) {
  const CellRef entry = newLabel(), body = newLabel();
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entry);
  callLabel(body);
  jump(haltOp_);
  tail_ = std::nullopt;
  Framed fr;
  openFramed(fr, body);
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_          = Ctx{};
  ctx_.retReg   = scratchG_;
  ctx_.epi      = fr.epi;
  own_          = owner;
  const auto &f = owner ? owner->fields.at(field.name) : fields_.at(field.name);
  const CellRef val = global();
  stmtWrap([&] {
    const Ty vt = valueInto(*field.init, val);
    coerceInto(f.ty, vt, val);
    storeField(f, val);
  });
  closeFramed(fr);
  own_ = nullptr;
  return entry;
}

CellRef Gen::emitUpgradeHook(Container &owner) {
  const CellRef entry = newLabel();
  tail_               = std::nullopt;
  frame_              = nullptr;
  place(entry);
  // Adapter writes from_version in the normal Upgrade event shape.
  const auto &turn = fields_.at(std::string(kFieldTurn));
  mov(S(a0_), S(rd(S(turn.nameCell), core_.dims().values)));
  movC(a1_, cStr(std::string(kTurnEvent)));
  callLabel(rtFind_);
  mov(S(a0_), S(r0_));
  movC(a1_, cStr("from_version"));
  callLabel(rtFind_);
  mov(S(a0_), S(r0_));
  callLabel(owner.upgradeLabel);
  jump(haltOp_);
  return entry;
}

void Gen::emitInit(CellRef entryLabel) {
  const CellRef body = newLabel();
  tail_              = std::nullopt;
  frame_             = nullptr;
  place(entryLabel);
  callLabel(body);
  jump(haltOp_);
  // The body runs under a frame so a failure unwinds the stack with it.
  tail_ = std::nullopt;
  Framed fr;
  openFramed(fr, body);
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_        = Ctx{};
  ctx_.retReg = scratchG_;
  ctx_.epi    = fr.epi;
  own_        = nullptr;
  mov(S(curEvent_), S(zero_));
  for (const Member &m : agent_->members) {
    const auto *v = std::get_if<VarDecl>(&m);
    if (v == nullptr) continue;
    const FieldInfo &f = fields_.at(v->name);
    const CellRef val  = global();
    stmtWrap([&] {
      const Ty vt = valueInto(*v->init, val);
      coerceInto(f.ty, vt, val);
      storeField(f, val);
    });
  }
  // §8.5: believe, install and behavior members run in declaration order.
  for (const Member &m : agent_->members) {
    if (const auto *b = std::get_if<BeliefDecl>(&m)) {
      stmtWrap(
          [&] { beliefChange(EBeliefChange::Kind::Believe, b->fact, reg()); });
    } else if (const auto *i = std::get_if<InstallDecl>(&m)) {
      stmtWrap([&] {
        installByName(EInstall::Kind::Install, i->name, i->args, reg(), i->loc);
      });
    } else if (const auto *u = std::get_if<BehaviorUse>(&m)) {
      stmtWrap([&] {
        installByName(EInstall::Kind::Mount, u->name, u->args, reg(), u->loc);
      });
    }
  }
  const CellRef end = newLabel();
  for (const HandlerInfo *h : mergedFor(std::string(kEventInitialize))) {
    emitHandlerRun(*h, false, end);
  }
  place(end);
  callLabel(rtSettle_);
  closeFramed(fr);
}

void Gen::emitDispatch(CellRef entryLabel) {
  const CellRef body = newLabel();
  tail_              = std::nullopt;
  frame_             = nullptr;
  place(entryLabel);
  callLabel(body);
  jump(haltOp_);
  tail_ = std::nullopt;
  place(body);
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_        = 0;
  ctx_              = Ctx{};
  own_              = nullptr;
  const CellRef end = newLabel();
  mov(S(fields_.at(std::string(kFieldConsumed)).valueCell), S(false_));
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = global(), evp = global(), vp = global(), ty = global(),
                eq = global(), z = global();
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  mov(S(a0_), S(tp));
  movC(a1_, cStr(std::string(kEventFieldOfTurn)));
  callLabel(rtFind_);
  mov(S(vp), S(r0_));
  emit(OpcodeKind::Eq, "noev", {S(vp), S(zero_)}, {z});
  branch(S(z), end);
  mov(S(evp), S(vp));
  mov(S(curEvent_), S(evp));
  mov(S(ty), D(evp));
  std::vector<std::pair<std::string, CellRef>> seqs;
  std::vector<std::string> types;
  for (const auto &[n, d] : events_) types.push_back(n);
  // Initialize runs from emitInit, not from a delivery.
  for (const PreludeEvent &pe : preludeEvents()) {
    if (pe.name != kEventInitialize) types.emplace_back(pe.name);
  }
  for (const std::string &t : types) {
    if (mergedFor(t).empty()) continue;
    const CellRef seq = newLabel();
    emit(OpcodeKind::Eq, "ty?", {S(ty), S(cStr(t))}, {eq});
    branch(S(eq), seq);
    seqs.emplace_back(t, seq);
  }
  const auto tellSeqs = emitTellTests(ty, eq);
  jump(end);
  for (const auto &[t, seq] : seqs) {
    place(seq);
    for (const HandlerInfo *h : mergedFor(t)) emitHandlerRun(*h, true, end);
    jump(end);
  }
  emitTellBodies(tellSeqs, end);
  place(end);
  callLabel(rtSettle_);
  ret();
}

void Gen::buildProgramStructure() {
  const CellRef module = core_.getOrCreateModule("vagent:" + agentType_);
  std::set<std::string> typeNames;
  for (const HandlerInfo &h : handlers_) typeNames.insert(h.eventType);
  for (const std::string &t : typeNames) {
    const CellRef nameCell = arena_.makeCell(t);
    zigzag::expectWritten(
        arena_.link(zigzag::rankTail(arena_, module, core_.dims().vars),
                    core_.dims().vars, DimVector::POS, nameCell));
    CellRef prev = nameCell;
    for (const HandlerInfo *h : mergedFor(t)) {
      if (h->eventType != t) continue;
      const CellRef hc = arena_.makeCell(
          (h->owner != nullptr ? h->owner->name : agentType_) + ".on " + t);
      zigzag::expectWritten(
          arena_.link(prev, core_.dims().clause, DimVector::POS, hc));
      prev = hc;
      // By reference, not by link: an op reached along `-d.grab` would have
      // the handler cell on its output wing, and a cell with a `+d.spin`
      // neighbour on an output wing runs as a parameter pipeline.
      if (h->guardEntry) {
        zigzag::expectWritten(arena_.link(
            hc, core_.dims().grab, DimVector::POS,
            arena_.makeScalarCell(static_cast<std::int64_t>(*h->guardEntry))));
      }
      zigzag::expectWritten(arena_.link(
          hc, core_.dims().spin, DimVector::POS,
          arena_.makeScalarCell(static_cast<std::int64_t>(h->bodyEntry))));
      const CellRef pn = arena_.makeCell("priority");
      zigzag::expectWritten(
          arena_.link(hc, core_.dims().vars, DimVector::POS, pn));
      zigzag::expectWritten(arena_.link(pn, core_.dims().values, DimVector::POS,
                                        arena_.makeScalarCell(h->priority)));
    }
    CellRef pe = nameCell;
    for (const std::string &s : supertypesOf(t)) {
      const CellRef sc = arena_.makeCell(s);
      zigzag::expectWritten(arena_.link(pe, dims_.extends, DimVector::POS, sc));
      pe = sc;
    }
  }
}

// ---------------------------------------------------------------------------
// Capacities, skills and behaviors (design §5.3, §5.7, §8.5)
//
// Deviation from §5.3, which mints a skill frame and its instances when
// `install` runs: no opcode can address a cell minted at run time (see
// compiler.hpp), so the frame, its fields, one instance per capacity and the
// capacity-name cells are minted here, once per skill reachable from the
// agent, and `install` links them into the agent's `d.skill` rank. `uninstall`
// unlinks them. Rank membership is therefore the installed state, and a skill
// has at most one live installation, which `install`'s own rule (replace
// every skill that shares a capacity, itself included) already implies. A skill
// that implements no capacity has no instance to put on the rank, so its
// frame goes there instead; capacity walks skip a cell with no `+d.capacity`.
//
// A behavior is the same: one cell per reachable behavior, on `d.behavior`
// while mounted. Its handlers are in the dispatcher's merged ranks always and
// answer only while mounted. Deviation from §5.7: the tie-break "behaviors in
// mount order" is "behaviors in the order the agent first names them", which is
// the mount order for every behavior the agent mounts in its own body.
// ---------------------------------------------------------------------------

HandlerInfo Gen::makeHandlerInfo(const HandlerDef &h, std::size_t index,
                                 Container *owner) {
  HandlerInfo hi;
  hi.owner    = owner;
  hi.def      = &h;
  hi.priority = h.priority.value_or(0);
  hi.index    = index;
  if (const auto *bt = std::get_if<BeliefTrigger>(&h.trigger)) {
    (void)predOf(bt->fact);
    hi.eventType =
        std::string(bt->added ? kBeliefAdded : kBeliefRemoved) + bt->fact.name;
    hi.beliefTerm = &bt->fact;
  } else {
    const auto &trig = std::get<EventTrigger>(h.trigger);
    hi.eventType     = trig.eventType;
    hi.binder        = trig.binder;
  }
  return hi;
}

void Gen::addFieldTo(CellRef owner, std::optional<CellRef> &last,
                     std::unordered_map<std::string, FieldInfo> &into,
                     const std::string &fname, const Ty &ty) {
  FieldInfo f;
  f.ty        = ty;
  f.nameCell  = arena_.makeCell(fname);
  f.valueCell = initialValueCell(ty);
  stateCells_.insert(f.nameCell);
  stateCells_.insert(f.valueCell);
  zigzag::expectWritten(arena_.link(f.nameCell, core_.dims().values,
                                    DimVector::POS, f.valueCell));
  zigzag::expectWritten(arena_.link(last.value_or(owner), core_.dims().vars,
                                    DimVector::POS, f.nameCell));
  last        = f.nameCell;
  into[fname] = std::move(f);
}

/// A skill's or behavior's own field, then the agent's: the reserved `$`
/// fields (`consume`, `self`) belong to the agent whoever's code runs.
const FieldInfo *Gen::findField(const std::string &n) const {
  if (own_ != nullptr) {
    if (const auto f = own_->fields.find(n); f != own_->fields.end()) {
      return &f->second;
    }
  }
  const auto f = fields_.find(n);
  return f == fields_.end() ? nullptr : &f->second;
}

MethodInfo *Gen::findMethod(const std::string &n) {
  auto &set    = own_ != nullptr ? own_->methods : methods_;
  const auto m = set.find(n);
  return m == set.end() ? nullptr : &m->second;
}

std::vector<std::string>
Gen::capacityClosure(const std::vector<std::string> &names) const {
  std::vector<std::string> out;
  std::function<void(const std::string &)> add = [&](const std::string &n) {
    if (std::ranges::find(out, n) != out.end()) return;
    out.push_back(n);
    if (const auto it = capacities_.find(n); it != capacities_.end()) {
      for (const std::string &base : it->second->extends) add(base);
    }
  };
  for (const std::string &n : names) add(n);
  return out;
}

std::vector<const MethodSig *>
Gen::capacityMethods(const std::string &cap) const {
  std::vector<const MethodSig *> out;
  for (const std::string &c : capacityClosure({cap})) {
    if (const auto it = capacities_.find(c); it != capacities_.end()) {
      for (const MethodSig &m : it->second->methods) out.push_back(&m);
    }
  }
  return out;
}

std::optional<std::string>
Gen::capacityOfMethod(const std::string &method) const {
  const auto &uses = own_ != nullptr ? own_->uses : agentUses_;
  for (const std::string &cap : uses) {
    for (const MethodSig *m : capacityMethods(cap)) {
      if (m->name == method) return cap;
    }
  }
  return std::nullopt;
}

void Gen::reachExpr(const Expr &e, std::vector<std::string> &skillOrder,
                    std::vector<std::string> &behaviorOrder) {
  if (const auto *i = std::get_if<EInstall>(&e.node)) {
    auto &order = (i->kind == EInstall::Kind::Install ||
                   i->kind == EInstall::Kind::Uninstall)
                      ? skillOrder
                      : behaviorOrder;
    if (std::ranges::find(order, i->name) == order.end()) {
      order.push_back(i->name);
    }
  }
  forEachChild(e,
               [&](const Expr &c) { reachExpr(c, skillOrder, behaviorOrder); });
}

void Gen::reachFrom(const std::vector<Member> &members,
                    std::vector<std::string> &skillOrder,
                    std::vector<std::string> &behaviorOrder) {
  const auto note = [](std::vector<std::string> &order, const std::string &n) {
    if (std::ranges::find(order, n) == order.end()) order.push_back(n);
  };
  const auto walk = [&](const ExprPtr &p) {
    if (p) reachExpr(*p, skillOrder, behaviorOrder);
  };
  for (const Member &m : members) {
    if (const auto *i = std::get_if<InstallDecl>(&m)) {
      note(skillOrder, i->name);
    } else if (const auto *b = std::get_if<BehaviorUse>(&m)) {
      note(behaviorOrder, b->name);
    } else if (const auto *md = std::get_if<MethodDef>(&m)) {
      walk(md->body);
    } else if (const auto *h = std::get_if<HandlerDef>(&m)) {
      walk(h->guard);
      walk(h->body);
    } else if (const auto *l = std::get_if<LifecycleDef>(&m)) {
      walk(l->body);
    } else if (const auto *v = std::get_if<VarDecl>(&m)) {
      walk(v->init);
    }
  }
}

Container &Gen::makeContainer(Container::K k, const std::string &name,
                              const std::vector<Param> &params,
                              const std::vector<Member> &members,
                              SourceLocation loc) {
  Container c;
  c.k       = k;
  c.name    = name;
  c.params  = &params;
  c.members = &members;
  c.loc     = loc;
  c.cell    = arena_.makeCell(name);
  stateCells_.insert(c.cell);
  const auto add = [&](const std::string &fname, const Ty &ty,
                       SourceLocation at) {
    if (c.fields.contains(fname)) bail(CompileError::DuplicateName, fname, at);
    addFieldTo(c.cell, c.lastField, c.fields, fname, ty);
  };
  for (const Param &p : params) add(p.name, fromType(p.type, p.loc), p.loc);
  for (const Member &m : members) {
    if (const auto *v = std::get_if<VarDecl>(&m)) {
      add(v->name, fromType(v->type, v->loc), v->loc);
    } else if (const auto *u = std::get_if<UsesDecl>(&m)) {
      c.uses.insert(c.uses.end(), u->capacities.begin(), u->capacities.end());
    } else if (k == Container::K::Behavior &&
               (std::holds_alternative<GoalDecl>(m) ||
                std::holds_alternative<PlanDecl>(m) ||
                std::holds_alternative<AnswerDef>(m) ||
                std::holds_alternative<BeliefDecl>(m) ||
                std::holds_alternative<InstallDecl>(m) ||
                std::holds_alternative<BehaviorUse>(m))) {
      unsupported("this member of a behavior", loc);
    }
  }
  c.prepLabel      = newLabel();
  c.installLabel   = newLabel();
  c.uninstallLabel = newLabel();
  c.upgradeLabel   = newLabel();
  auto &table      = k == Container::K::Skill ? skills_ : behaviors_;
  return table.emplace(name, std::move(c)).first->second;
}

void Gen::prepareExtensions() {
  std::unordered_map<std::string, const SkillDecl *> skillDecls;
  std::unordered_map<std::string, const BehaviorDecl *> behaviorDecls;
  for (const TopDecl &d : unit_.decls) {
    if (const auto *c = std::get_if<CapacityDecl>(&d)) {
      capacities_.emplace(c->name, c);
    } else if (const auto *sk = std::get_if<SkillDecl>(&d)) {
      skillDecls.emplace(sk->name, sk);
    } else if (const auto *b = std::get_if<BehaviorDecl>(&d)) {
      behaviorDecls.emplace(b->name, b);
    }
  }
  std::vector<std::string> skillOrder, behaviorOrder;
  reachFrom(agent_->members, skillOrder, behaviorOrder);
  std::size_t si = 0, bi = 0;
  while (si < skillOrder.size() || bi < behaviorOrder.size()) {
    while (si < skillOrder.size()) {
      const std::string name = skillOrder[si++];
      const auto it          = skillDecls.find(name);
      if (it == skillDecls.end()) {
        bail(CompileError::UnknownName, "unknown skill " + name);
      }
      const SkillDecl &sd = *it->second;
      if (sd.native) {
        unsupported("native skill " + name + " (its methods are effects)",
                    sd.loc);
      }
      Container &c = makeContainer(Container::K::Skill, name, sd.params,
                                   sd.members, sd.loc);
      c.capacities = capacityClosure(sd.implements);
      CellRef prev = c.cell;
      for (const std::string &cap : c.capacities) {
        const CellRef inst = arena_.makeCell(name);
        stateCells_.insert(inst);
        zigzag::expectWritten(arena_.link(inst, dims_.capacity, DimVector::POS,
                                          arena_.makeCell(cap)));
        zigzag::expectWritten(
            arena_.link(prev, core_.dims().clone, DimVector::POS, inst));
        prev = inst;
        c.instances.push_back(inst);
      }
      c.member = c.instances.empty() ? c.cell : c.instances.front();
      prepareMethodSet(c.methods, sd.members, &c);
      reachFrom(sd.members, skillOrder, behaviorOrder);
    }
    while (bi < behaviorOrder.size()) {
      const std::string name = behaviorOrder[bi++];
      const auto it          = behaviorDecls.find(name);
      if (it == behaviorDecls.end()) {
        bail(CompileError::UnknownName, "unknown behavior " + name);
      }
      const BehaviorDecl &bd = *it->second;
      Container &c = makeContainer(Container::K::Behavior, name, bd.params,
                                   bd.members, bd.loc);
      c.member     = c.cell;
      prepareMethodSet(c.methods, bd.members, &c);
      reachFrom(bd.members, skillOrder, behaviorOrder);
    }
  }
  // Handlers last, in the order the behaviors were first named, so a behavior
  // that comes later loses a priority tie to one that comes earlier.
  for (const std::string &name : behaviorOrder) {
    Container &c = behaviors_.at(name);
    for (const Member &m : *c.members) {
      if (const auto *h = std::get_if<HandlerDef>(&m)) {
        handlers_.push_back(makeHandlerInfo(*h, handlers_.size(), &c));
      }
    }
  }
}

Ty Gen::capacityCall(const std::string &cap, const std::string &method,
                     const std::vector<ExprPtr> &args, CellRef out,
                     SourceLocation loc) {
  const MethodSig *sig = nullptr;
  for (const MethodSig *m : capacityMethods(cap)) {
    if (m->name == method) {
      sig = m;
      break;
    }
  }
  if (sig == nullptr) {
    bail(CompileError::UnknownName, cap + " has no method " + method, loc);
  }
  if (sig->isEffect) unsupported("effect methods", loc);
  if (args.size() != sig->params.size()) {
    bail(CompileError::BadArity,
         method + " takes " + std::to_string(sig->params.size()) + " arguments",
         loc);
  }
  std::vector<Container *> candidates;
  for (auto &[n, sk] : skills_) {
    if (std::ranges::find(sk.capacities, cap) != sk.capacities.end() &&
        sk.methods.contains(method)) {
      candidates.push_back(&sk);
    }
  }
  if (candidates.empty()) {
    bail(CompileError::UnknownName,
         "no skill reachable from the agent implements " + cap + "." + method,
         loc);
  }
  std::vector<CellRef> temps;
  for (const auto &a : args) {
    const CellRef t = reg();
    valueInto(*a, t);
    temps.push_back(t);
  }
  const CellRef frame = reg();
  movC(a0_, cStr(cap));
  callLabel(rtCapFind_);
  mov(S(frame), S(r0_));
  branchIfZero(frame, failStep_);
  std::vector<CellRef> labels;
  for (const Container *sk : candidates) {
    const CellRef label = newLabel(), eq = reg();
    emit(OpcodeKind::Eq, "skill?", {D(frame), S(cStr(sk->name))}, {eq});
    branch(S(eq), label);
    labels.push_back(label);
  }
  jump(failStep_);
  const CellRef end = newLabel();
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    place(labels[i]);
    MethodInfo &mi = candidates[i]->methods.at(method);
    if (mi.argRegs.size() != temps.size()) {
      bail(CompileError::BadArity,
           candidates[i]->name + "." + method + " differs from " + cap, loc);
    }
    for (std::size_t j = 0; j < temps.size(); ++j) {
      mov(S(mi.argRegs[j]), S(temps[j]));
    }
    callUser(mi.entry);
    mov(S(out), S(mi.retReg));
    jump(end);
  }
  place(end);
  return sig->result ? fromType(*sig->result, loc) : tyOf(Ty::K::Unit);
}

/// a0 = a capacity name; r0 = the frame of the first installed skill with an
/// instance for it (the clone master, §5.3), or 0.
void Gen::emitRtCapFind() {
  beginInternal(rtCapFind_);
  const CellRef inst = reg(), capName = reg(), eq = reg(), prev = reg(),
                frame = reg();
  const CellRef loop = newLabel(), next = newLabel(), found = newLabel(),
                climb = newLabel(), top = newLabel(), miss = newLabel();
  mov(S(inst), S(rd(S(agentCell_), dims_.skill)));
  place(loop);
  branchIfZero(inst, miss);
  mov(S(capName), S(rd(D(inst), dims_.capacity)));
  branchIfZero(capName, next);
  emit(OpcodeKind::Eq, "cap?", {D(capName), S(a0_)}, {eq});
  branch(S(eq), found);
  place(next);
  mov(S(inst), S(rd(D(inst), dims_.skill)));
  jump(loop);
  place(found);
  mov(S(frame), S(inst));
  place(climb);
  mov(S(prev), S(rd(D(frame), core_.dims().clone, true)));
  branchIfZero(prev, top);
  mov(S(frame), S(prev));
  jump(climb);
  place(top);
  mov(S(r0_), S(frame));
  ret();
  place(miss);
  movC(r0_, zero_);
  ret();
}

void Gen::beginRoutine(CellRef label, Container *owner, Framed *fr) {
  own_   = owner;
  tail_  = std::nullopt;
  frame_ = nullptr;
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_        = Ctx{};
  ctx_.retReg = scratchG_;
  if (fr != nullptr) {
    openFramed(*fr, label);
    ctx_.epi = fr->epi;
  } else {
    place(label);
  }
}

void Gen::emitContainerInits(Container &c) {
  for (const Member &m : *c.members) {
    const auto *v = std::get_if<VarDecl>(&m);
    if (v == nullptr) continue;
    const FieldInfo &f = c.fields.at(v->name);
    const CellRef val  = global();
    stmtWrap([&] {
      valueInto(*v->init, val);
      storeField(f, val);
    });
  }
}

void Gen::emitLifecycle(Container &c, bool uninstall) {
  for (const Member &m : *c.members) {
    const auto *l = std::get_if<LifecycleDef>(&m);
    if (l == nullptr || l->kind == LifecycleDef::Kind::Upgrade ||
        l->uninstall != uninstall || !l->body)
      continue;
    expr(*l->body, global());
  }
}

void Gen::emitUpgradeLifecycle(Container &c) {
  for (const Member &m : *c.members) {
    const auto *l = std::get_if<LifecycleDef>(&m);
    if (l == nullptr || l->kind != LifecycleDef::Kind::Upgrade || !l->body)
      continue;
    if (l->param) {
      const CellRef p           = declareLocal(*l->param, tyOf(Ty::K::Str));
      scopes_.back()[*l->param] = Local{p, tyOf(Ty::K::Str)};
      mov(S(p), S(a0_));
    }
    expr(*l->body, global());
  }
}

/// The refs of a skill's instances, in order. `#Deref` follows `d.clone` to
/// the master, so an instance is reached by walking the clone chain instead.
void Gen::forEachInstanceRef(Container &c,
                             const std::function<void(CellRef)> &fn) {
  if (c.instances.empty()) {
    const CellRef r = reg();
    loadRef(r, c.cell);
    fn(r);
    return;
  }
  std::optional<CellRef> prev;
  for (std::size_t i = 0; i < c.instances.size(); ++i) {
    const CellRef r = reg();
    mov(S(r), S(rd(prev ? D(*prev) : S(c.cell), core_.dims().clone)));
    fn(r);
    prev = r;
  }
}

void Gen::emitContainerRoutines() {
  for (auto &[name, c] : skills_) {
    // Uninstall whatever shares a capacity with this skill, itself included.
    beginRoutine(c.prepLabel, nullptr);
    for (auto &[tn, t] : skills_) {
      const bool shares =
          &t == &c || std::ranges::any_of(t.capacities, [&](const auto &x) {
            return std::ranges::find(c.capacities, x) != c.capacities.end();
          });
      if (shares) callLabel(t.uninstallLabel);
    }
    ret();

    Framed inst;
    beginRoutine(c.installLabel, &c, &inst);
    emitContainerInits(c);
    forEachInstanceRef(c, [&](CellRef r) {
      emitAppendTail(S(agentCell_), dims_.skill, dims_.skill, r);
    });
    emitLifecycle(c, false);
    closeFramed(inst);

    Framed uninst;
    beginRoutine(c.uninstallLabel, &c, &uninst);
    const CellRef gone = newLabel();
    branchIfZero(rd(S(c.member), dims_.skill, true), gone);
    emitLifecycle(c, true);
    forEachInstanceRef(c, [&](CellRef r) { unlinkFromRank(r, dims_.skill); });
    place(gone);
    closeFramed(uninst);

    Framed upg;
    beginRoutine(c.upgradeLabel, &c, &upg);
    emitUpgradeLifecycle(c);
    closeFramed(upg);
  }
  for (auto &[name, c] : behaviors_) {
    Framed inst;
    beginRoutine(c.installLabel, &c, &inst);
    emitContainerInits(c);
    const CellRef doLink = newLabel(), done = newLabel();
    branchIfZero(rd(S(c.cell), dims_.behavior, true), doLink);
    jump(done);
    place(doLink);
    const CellRef r = reg();
    loadRef(r, c.cell);
    emitAppendTail(S(agentCell_), dims_.behavior, dims_.behavior, r);
    place(done);
    closeFramed(inst);

    beginRoutine(c.uninstallLabel, &c);
    const CellRef off = newLabel();
    branchIfZero(rd(S(c.cell), dims_.behavior, true), off);
    const CellRef u = reg();
    loadRef(u, c.cell);
    unlinkFromRank(u, dims_.behavior);
    place(off);
    ret();

    beginRoutine(c.upgradeLabel, &c);
    ret();
  }
  own_ = nullptr;
}

Ty Gen::installByName(EInstall::Kind kind, const std::string &name,
                      const std::vector<ExprPtr> &args, CellRef out,
                      SourceLocation loc) {
  const bool skill =
      kind == EInstall::Kind::Install || kind == EInstall::Kind::Uninstall;
  auto &table   = skill ? skills_ : behaviors_;
  const auto it = table.find(name);
  if (it == table.end()) {
    bail(CompileError::UnknownName,
         std::string(skill ? "unknown skill " : "unknown behavior ") + name,
         loc);
  }
  Container &c = it->second;
  if (kind == EInstall::Kind::Install || kind == EInstall::Kind::Mount) {
    if (args.size() != c.params->size()) {
      bail(CompileError::BadArity,
           name + " takes " + std::to_string(c.params->size()) + " arguments",
           loc);
    }
    // Arguments first: they may read the very fields the replaced skill's
    // `on uninstall` block is about to change.
    std::vector<CellRef> temps;
    for (const auto &a : args) {
      const CellRef t = reg();
      valueInto(*a, t);
      temps.push_back(t);
    }
    if (skill) callUser(c.prepLabel);
    for (std::size_t i = 0; i < temps.size(); ++i) {
      storeField(c.fields.at((*c.params)[i].name), temps[i]);
    }
    callUser(c.installLabel);
  } else {
    callUser(c.uninstallLabel);
  }
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

CompiledAgent Gen::run() {
  dims_ = registerDimensions(arena_);
  collect();

  zero_     = cInt(0);
  one_      = cInt(1);
  negOne_   = cInt(-1);
  unit_c_   = arena_.makeCell(std::string(kUnitContent));
  true_     = arena_.makeScalarCell(true);
  false_    = arena_.makeScalarCell(false);
  a0_       = global();
  a1_       = global();
  a2_       = global();
  r0_       = global();
  r1_       = global();
  curEvent_ = global();
  refTmp_   = global();
  frameH_   = mintHandle();
  walkH_    = mintHandle();
  for (CellRef &h : pool_) h = mintHandle();
  for (CellRef &c : scratch_) c = global();
  scratchG_      = global();
  failStep_      = mkOp(OpcodeKind::Fail, "SFAIL");
  haltOp_        = mkOp(OpcodeKind::Halt, "HALT");
  failOp_        = mkOp(OpcodeKind::Halt, "FAIL");
  rtFind_        = newLabel();
  rtAppendField_ = newLabel();
  rtListPush_    = newLabel();
  rtOutbox_      = newLabel();
  rtCopy_        = newLabel();
  rtBRaise_      = newLabel();
  rtHasSrc_      = newLabel();
  rtBDrop_       = newLabel();
  rtBSet_        = newLabel();
  rtSettle_      = newLabel();
  rtCapFind_     = newLabel();
  a3_            = global();
  keepHistG_     = global();
  for (CellRef *zeroed : {&inCondG_, &condFuelG_, &condContractD_, &condD_,
                          &inPlanG_, &planContractDG_})
    *zeroed = arena_.makeScalarCell(std::int64_t{0});
  curIntention_ = global();
  scanSubjG_    = global();
  scanPosG_     = global();
  scanLenG_     = global();
  a4_           = global();
  roundsG_      = global();
  copyPro_      = newLabel();
  copyBody_     = newLabel();

  mintAgentCell();
  mintPredicates();

  // Handlers and the members the iteration does not support.
  for (const Member &m : agent_->members) {
    if (const auto *h = std::get_if<HandlerDef>(&m)) {
      handlers_.push_back(makeHandlerInfo(*h, handlers_.size(), nullptr));
    } else if (const auto *u = std::get_if<UsesDecl>(&m)) {
      agentUses_.insert(agentUses_.end(), u->capacities.begin(),
                        u->capacities.end());
    } else if (std::holds_alternative<AnswerDef>(m) ||
               std::holds_alternative<GoalDecl>(m) ||
               std::holds_alternative<PlanDecl>(m) ||
               std::holds_alternative<LifecycleDef>(m)) {
      unsupported("this member", agent_->loc);
    }
  }
  prepareMethods();
  prepareExtensions();

  emitRtFind();
  emitRtAppendField();
  emitRtListPush();
  emitRtOutbox();
  emitRtCopy();
  emitRtBRaise();
  emitRtHasSrc();
  emitRtBDrop();
  emitRtBSet();
  emitRtCapFind();
  for (auto &[n, mi] : methods_) emitMethod(mi);
  for (auto &[sn, sk] : skills_) {
    for (auto &[n, mi] : sk.methods) emitMethod(mi);
  }
  for (auto &[bn, bh] : behaviors_) {
    for (auto &[n, mi] : bh.methods) emitMethod(mi);
  }
  emitContainerRoutines();
  for (HandlerInfo &h : handlers_) emitHandlerRoutines(h);

  CompiledAgent out;
  out.agentCell     = agentCell_;
  out.initEntry     = newLabel();
  out.dispatchEntry = newLabel();
  out.haltOp        = haltOp_;
  out.failOp        = failOp_;
  emitSettle();
  emitInit(out.initEntry);
  emitDispatch(out.dispatchEntry);
  if (std::ranges::any_of(preds_,
                          [](const auto &p) { return p.second.arbitrated; })) {
    out.observeEntry = newLabel();
    emitObserve(*out.observeEntry);
    out.observeRefusalEntry = newLabel();
    emitObserveRefusal(*out.observeRefusalEntry);
    out.sourceChangeEntry = newLabel();
    emitSourceChange(*out.sourceChangeEntry);
  }
  for (const auto &member : agent_->members) {
    if (const auto *field = std::get_if<VarDecl>(&member))
      out.fieldDefaults.emplace(field->name, emitFieldDefault(*field, nullptr));
  }
  const auto metadata = [&](auto &containers, auto &output) {
    for (auto &[name, container] : containers) {
      CompiledContainer info;
      info.owner   = container.cell;
      info.members = container.instances.empty()
                         ? std::vector<CellRef>{container.cell}
                         : container.instances;
      for (const auto &member : *container.members) {
        if (const auto *field = std::get_if<VarDecl>(&member))
          info.fieldDefaults.emplace(field->name,
                                     emitFieldDefault(*field, &container));
      }
      info.upgradeEntry = emitUpgradeHook(container);
      output.emplace(name, std::move(info));
    }
  };
  metadata(skills_, out.skills);
  metadata(behaviors_, out.behaviors);
  while (!pendingRoutines_.empty()) {
    const Emitter emit = pendingRoutines_.back();
    pendingRoutines_.pop_back();
    (this->*emit)();
  }
  buildProgramStructure();
  return out;
}

} // namespace

Compiler::Compiler(zigzag::vortex::VortexCore &core) : core_(core), vm_(core) {}

std::optional<FrameBody> Compiler::frameBody(std::string_view name) const {
  const auto it = frameBodies_.find(name);
  if (it == frameBodies_.end()) return std::nullopt;
  return it->second;
}

Compiler *Compiler::setBeliefEventDepth(std::size_t rounds) noexcept {
  beliefEventDepth_ = rounds;
  return this;
}

Compiler *Compiler::setConditionFuel(std::size_t fuel) noexcept {
  conditionFuel_ = fuel;
  return this;
}

Compiler *Compiler::setMaintainBackoffMs(std::int64_t backoffMs) noexcept {
  maintainBackoffMs_ = backoffMs;
  return this;
}

std::expected<CompiledAgent, CompileFailure>
Compiler::compileAgent(const Unit &unit, std::string_view agentType) {
  try {
    Gen g(core_, vm_, unit, agentType, beliefEventDepth_, conditionFuel_,
          maintainBackoffMs_);
    auto out     = g.run();
    frameBodies_ = g.takeFrameBodies();
    return out;
  } catch (const Bail &b) {
    return std::unexpected(b.failure);
  }
}

} // namespace xanadu::vagent::runtime
