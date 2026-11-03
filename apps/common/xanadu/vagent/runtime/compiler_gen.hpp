/**
 * @file compiler_gen.hpp
 * @brief Private declaration of the vagent code generator (`codegen::Gen`) and
 *        the helper types it is built from.
 *
 * Only the compiler translation units include this. The declaration lives in a
 * header so that the emitters of later areas can sit in files of their own and
 * take a `Gen &`; the members defined out of class are in compiler.cpp. The
 * namespace is named because a header cannot hold an anonymous one.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_COMPILER_GEN_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_COMPILER_GEN_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/vagent/runtime/ast.hpp"
#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/prelude.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime::codegen {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Bail {
  CompileFailure failure;
};

[[noreturn]] inline void bail(CompileError code, std::string detail,
                              SourceLocation loc = {}) {
  throw Bail{CompileFailure{code, std::move(detail), loc}};
}

[[noreturn]] inline void unsupported(std::string_view what,
                                     SourceLocation loc) {
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

inline Ty tyOf(Ty::K k) {
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
inline Opd S(CellRef c) { return Opd{c, false, false}; }
inline Opd D(CellRef p) { return Opd{p, true, false}; }
inline Opd Hd(CellRef c) { return Opd{c, false, true}; }

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
          opt(n.believesFrom);
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
          opt(n.from);
        } else if constexpr (std::is_same_v<T, EBeliefChange>) {
          for (const auto &a : n.fact.args) opt(a.expr);
          opt(n.from);
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

inline void collectCallees(const Expr &e, std::set<std::string> &out) {
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
  CellRef scanSubjG_{}, scanPosG_{}, scanLenG_{}, scanCursorG_{};

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
    emit(OpcodeKind::Not, "!z", {S(r)}, {t});
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
                SourceLocation loc, const std::string &capacityName = {});
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
  void emitProfileFor(CellRef who, CellRef inc, CellRef maxSources,
                      CellRef prof);
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
  Ty beliefChange(EBeliefChange::Kind kind, const Term &t,
                  const ExprPtr &fromExpr, CellRef out);
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

} // namespace xanadu::vagent::runtime::codegen

#endif // COMMON_XANADU_VAGENT_RUNTIME_COMPILER_GEN_HPP
