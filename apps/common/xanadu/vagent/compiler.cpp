/**
 * @file compiler.cpp
 * @brief vagent compiler, iteration 2 (see compiler.hpp for the machine it
 *        targets and why).
 *
 * Deviations from the design, each retired by a later iteration or by an
 * approved core change:
 *  - The dispatcher is emitted as opcodes per event type here, not written in
 *    a `std:agent` prelude (§10). The program-side ranks of §5.7 are built
 *    anyway so iteration 3 can walk them.
 *  - Locals and arguments are registers of the routine, not cells on a frame
 *    (§5.8): no opcode addresses a cell minted at run time except through a
 *    slot poke, and `#Call`'s frame is not reachable that way.
 *  - Scalar agent fields are read and written through their static value
 *    cell; record and list fields are looked up through their name cell on
 *    every access, so they may be replaced by relinking.
 */
#include "common/xanadu/vagent/compiler.hpp"

#include <algorithm>
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

#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
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
    List
  } k{K::Unit};
  std::string name;     ///< Record: the declared type
  std::vector<Ty> args; ///< List: the element type
  [[nodiscard]] bool composite() const {
    return k == K::Record || k == K::List;
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
constexpr std::string_view kMergeLabelPrefix = "r";

/// An operand: a static cell, or the cell whose ref is held in register `ptr`.
struct Opd {
  CellRef cell{noCell};
  CellRef ptr{noCell};
};
Opd S(CellRef c) { return Opd{c, noCell}; }
Opd D(CellRef p) { return Opd{noCell, p}; }

struct Local {
  CellRef reg{noCell};
  Ty ty;
};

struct FieldInfo {
  CellRef nameCell{noCell};
  CellRef valueCell{noCell};
  Ty ty;
};

struct Routine {
  std::vector<CellRef> regs;
  int id{0};
};

struct Container;

struct MethodInfo {
  /// The skill whose method this is; null for the agent's own.
  Container *owner{nullptr};
  const MethodDef *def{nullptr};
  Routine rt;
  CellRef entry{noCell};
  CellRef bodyStart{noCell};
  CellRef pro{noCell};
  CellRef epi{noCell};
  CellRef retReg{noCell};
  std::vector<CellRef> paramRegs;
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
  CellRef guardEntry{noCell};
  CellRef bodyEntry{noCell};
  /// Belief triggers (`on +t` / `on -t`): the trigger term, the routine that
  /// matches it against the pending event's fact, and the locals its `?`
  /// variables declare for the guard and body.
  const Term *beliefTerm{nullptr};
  CellRef bindEntry{noCell};
  std::vector<std::pair<std::string, Local>> beliefLocals;
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
  CellRef cell{noCell}; ///< skill frame / behavior instance, holds the fields
  CellRef lastField{noCell};
  std::unordered_map<std::string, FieldInfo> fields;
  std::map<std::string, MethodInfo> methods;
  std::vector<std::string> uses;
  /// Skill: every capacity it implements, extended ones included.
  std::vector<std::string> capacities;
  std::vector<CellRef> instances; ///< one per capacity, on d.skill when live
  /// The cell whose rank membership means "installed": the first instance, or
  /// the frame itself for a skill that implements no capacity.
  CellRef member{noCell};
  CellRef prepLabel{noCell};      ///< uninstalls what shares a capacity
  CellRef installLabel{noCell};   ///< install / mount body
  CellRef uninstallLabel{noCell}; ///< uninstall / unmount body
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
        } else if constexpr (std::is_same_v<T, ETell>) {
          opt(n.to);
          for (const auto &a : n.fact.args) opt(a.expr);
        } else if constexpr (std::is_same_v<T, EStop>) {
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
      std::size_t beliefEventDepth)
      : core_(core), vm_(vm), arena_(core.arena()), unit_(unit),
        agentType_(agentType), beliefEventDepth_(beliefEventDepth) {}

  CompiledAgent run();

private:
  // -- primitives ------------------------------------------------------------
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  zigzag::ArenaManifold &arena_;
  const Unit &unit_;
  std::string agentType_;
  AgentDims dims_{};

  CellRef tail_{noCell};
  std::vector<CellRef> *curRegs_{nullptr};
  int nextRoutineId_{0};

  std::map<std::int64_t, CellRef> intConsts_;
  std::map<std::string, CellRef> strConsts_;
  std::map<CellRef, CellRef> oprefs_;
  CellRef zero_{noCell}, one_{noCell}, negOne_{noCell}, unit_c_{noCell},
      true_{noCell}, false_{noCell};
  CellRef a0_{noCell}, a1_{noCell}, a2_{noCell}, r0_{noCell}, r1_{noCell};
  CellRef curEvent_{noCell}, depth_{noCell}, depthStr_{noCell},
      nameTmp_{noCell}, scratchG_{noCell};
  CellRef failStep_{noCell};
  CellRef haltOp_{noCell}, failOp_{noCell};
  CellRef rtFind_{noCell}, rtAppendField_{noCell}, rtListPush_{noCell},
      rtOutbox_{noCell}, rtCopy_{noCell};
  Routine copyRoutine_;
  CellRef copyPro_{noCell}, copyEpi_{noCell}, copyBody_{noCell};

  CellRef agentCell_{noCell};
  std::unordered_map<std::string, FieldInfo> fields_;
  CellRef lastFieldName_{noCell};

  std::unordered_map<std::string, const EventDecl *> events_;
  std::unordered_map<std::string, const RecordDecl *> records_;
  std::unordered_map<std::string, const AgentDecl *> agents_;
  const AgentDecl *agent_{nullptr};

  std::map<std::string, MethodInfo> methods_;
  std::vector<HandlerInfo> handlers_;

  // -- skills, behaviors and capacities (§5.3, §5.7); see the section below
  std::map<std::string, Container> skills_;
  std::map<std::string, Container> behaviors_;
  std::unordered_map<std::string, const CapacityDecl *> capacities_;
  std::vector<std::string> agentUses_;
  /// The skill or behavior whose code is being emitted; null for the agent's.
  Container *own_{nullptr};
  CellRef rtCapFind_{noCell};

  // Per-routine emission state.
  std::vector<std::unordered_map<std::string, Local>> scopes_;
  std::vector<CellRef> dRegs_;
  std::size_t stmtDepth_{0};
  struct Ctx {
    CellRef retReg{noCell};
    CellRef d0{noCell};
    CellRef epi{noCell};
    std::optional<Ty> eventTy;
    std::optional<std::string> binder;
    bool inHandler{false};
  } ctx_;

  // -- cell and op construction
  // ------------------------------------------------
  CellRef mkOp(OpcodeKind k, std::string_view label = {}) {
    return vm_.mintOpcode(k, label);
  }
  CellRef cInt(std::int64_t v) {
    auto [it, fresh] = intConsts_.try_emplace(v, noCell);
    if (fresh) it->second = arena_.makeScalarCell(v);
    return it->second;
  }
  CellRef cStr(const std::string &s) {
    auto [it, fresh] = strConsts_.try_emplace(s, noCell);
    if (fresh) it->second = arena_.makeCell(s);
    return it->second;
  }
  CellRef cFloat(double v) { return arena_.makeScalarCell(v); }
  CellRef cBool(bool v) { return v ? true_ : false_; }
  CellRef opref(CellRef target) {
    auto [it, fresh] = oprefs_.try_emplace(target, noCell);
    if (fresh) {
      it->second = arena_.makeScalarCell(static_cast<std::int64_t>(target));
    }
    return it->second;
  }
  CellRef reg() {
    const CellRef c = arena_.makeCell();
    if (curRegs_ != nullptr) curRegs_->push_back(c);
    return c;
  }
  CellRef global() { return arena_.makeCell(); }

  void spin(CellRef from, CellRef to) {
    zigzag::expectWritten(
        arena_.link(from, core_.dims().spin, DimVector::POS, to));
  }
  void append(CellRef op) {
    if (tail_ != noCell) spin(tail_, op);
    tail_ = op;
  }
  CellRef newLabel() { return mkOp(OpcodeKind::Nop, "L"); }
  void place(CellRef label) { append(label); }
  void kill() { tail_ = noCell; }

  struct Poke {
    CellRef slot;
    CellRef ptr;
  };

  CellRef addSlot(CellRef op, const Opd &o, bool input, std::vector<Poke> *pk) {
    const CellRef target = o.ptr == noCell ? o.cell : unit_c_;
    const CellRef slot =
        arena_.makeScalarCell(static_cast<std::int64_t>(target));
    const DimVector side = input ? DimVector::POS : DimVector::NEG;
    const CellRef first  = arena_.linked(op, core_.dims().grab, side);
    if (first == noCell) {
      zigzag::expectWritten(arena_.link(op, core_.dims().grab, side, slot));
    } else {
      zigzag::expectWritten(
          arena_.link(zigzag::rankTail(arena_, first, core_.dims().step),
                      core_.dims().step, DimVector::POS, slot));
    }
    if (o.ptr != noCell && pk != nullptr) pk->push_back({slot, o.ptr});
    return slot;
  }

  /// A poke writes the ref held in `ptr` into `slot`, so the operation that
  /// owns the slot reads that cell: the only way an opcode reaches a cell
  /// minted at run time.
  void flushPokes(const std::vector<Poke> &pk) {
    for (const Poke &p : pk) {
      const CellRef op = mkOp(OpcodeKind::Value, "poke");
      core_.bindInput(op, p.slot);
      core_.bindInput(op, zero_);
      core_.bindInput(op, negOne_);
      core_.bindInput(op, p.ptr);
      append(op);
    }
  }

  CellRef emit(OpcodeKind k, std::string_view label,
               std::initializer_list<Opd> ins,
               std::initializer_list<CellRef> outs = {}) {
    const CellRef op = mkOp(k, label);
    std::vector<Poke> pk;
    for (const Opd &o : ins) addSlot(op, o, true, &pk);
    for (const CellRef o : outs) core_.bindOutput(op, o);
    flushPokes(pk);
    append(op);
    return op;
  }

  void mov(Opd dst, Opd src) {
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
    const CellRef p  = reg();
    const CellRef lk = mkOp(OpcodeKind::Link, "rd");
    std::vector<Poke> pk;
    addSlot(lk, owner, true, &pk);
    addSlot(lk, S(cInt(dim)), true, &pk);
    addSlot(lk, S(cInt(negward ? kNeg : kPos)), true, &pk);
    flushPokes(pk);
    zigzag::expectWritten(
        arena_.link(p, core_.dims().spin, DimVector::POS, lk));
    emit(OpcodeKind::Nop, "rd", {S(p)});
    return p;
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

  // -- save/restore of a recursive routine's registers ----------------------
  std::string nameFor(const Routine &r, std::size_t i) {
    return std::string(kMergeLabelPrefix) + std::to_string(r.id) + "." +
           std::to_string(i) + "#";
  }
  void saveRegs(const Routine &r) {
    emit(OpcodeKind::Add, "d+", {S(depth_), S(one_)}, {depth_});
    emit(OpcodeKind::Trim, "ds", {S(depth_)}, {depthStr_});
    for (std::size_t i = 0; i < r.regs.size(); ++i) {
      emit(OpcodeKind::Add, "nm", {S(cStr(nameFor(r, i))), S(depthStr_)},
           {nameTmp_});
      emit(OpcodeKind::Bind, "save", {S(nameTmp_), S(r.regs[i])});
    }
  }
  void restoreRegs(const Routine &r) {
    emit(OpcodeKind::Trim, "ds", {S(depth_)}, {depthStr_});
    for (std::size_t i = 0; i < r.regs.size(); ++i) {
      emit(OpcodeKind::Add, "nm", {S(cStr(nameFor(r, i))), S(depthStr_)},
           {nameTmp_});
      emit(OpcodeKind::Resolve, "rest", {S(nameTmp_)}, {r.regs[i]});
    }
    emit(OpcodeKind::Sub, "d-", {S(depth_), S(one_)}, {depth_});
  }

  // -- shared runtime routines ------------------------------------------------
  void beginInternal(CellRef label) {
    tail_    = noCell;
    curRegs_ = nullptr;
    place(label);
  }
  void ret() {
    emit(OpcodeKind::Return, "return", {});
    kill();
  }
  void emitRtFind();
  void emitRtAppendField();
  void emitRtListPush();
  void emitRtOutbox();
  void emitRtCopy();
  void callFind(CellRef owner, const std::string &name, CellRef &vp,
                CellRef &np);
  /// Heads a freshly allocated record's `+d.vars` rank with `kRecordMarker`.
  void markRecord(CellRef record);
  void callAppendField(CellRef owner, const std::string &name,
                       std::optional<CellRef> existing, CellRef &vc);
  void callListPush(CellRef list, CellRef elem);
  CellRef callOutbox(std::string_view kind);
  void callCopy(CellRef src, CellRef dst);

  // -- types -----------------------------------------------------------------
  Ty fromType(const TypeExpr &t, SourceLocation loc);
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
    const CellRef d = dRegAt(stmtDepth_);
    ++stmtDepth_;
    emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
    emit(OpcodeKind::Choice, "sfail", {S(opref(failStep_))});
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

  // -- declarations
  // ------------------------------------------------------------
  void collect();
  void mintAgentCell();
  void prepareMethods();
  void emitMethod(MethodInfo &m);
  void emitHandlerRoutines(HandlerInfo &h);
  void emitHandlerRun(const HandlerInfo &h, bool checkConsumed, CellRef end);
  void emitInit(CellRef entryLabel);
  void emitDispatch(CellRef entryLabel);
  void buildProgramStructure();
  std::vector<const HandlerInfo *> mergedFor(const std::string &type);
  void beginHandlerCtx(const HandlerInfo &h);

  // -- beliefs (§6.1, §6.5)
  // ----------------------------------------------------
  /// A declared predicate: its cell on the agent's `d.belief` rank and the
  /// argument types the checker already verified.
  struct PredInfo {
    CellRef cell{noCell};
    std::vector<Ty> argTys;
    std::size_t nkeys{0};
  };
  /// How one argument of a term participates in a match or a build.
  struct ArgPlan {
    enum class K : std::uint8_t { Value, Var, Dup, Wild } k{K::Wild};
    CellRef reg{noCell};   ///< Value: the evaluated argument
    std::size_t first{0};  ///< Dup: index of the variable it repeats
    bool composite{false}; ///< Value: a list, compared and copied as a term
  };
  struct PlanVar {
    CellRef cap{noCell};   ///< holds the ref of the clause argument it matched
    CellRef local{noCell}; ///< the local the variable declares
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
  CellRef lastPred_{noCell};
  CellRef a3_{noCell}, a4_{noCell}, roundsG_{noCell};
  CellRef rtBRaise_{noCell}, rtHasSrc_{noCell}, rtBDrop_{noCell},
      rtBSet_{noCell}, rtSettle_{noCell};

  void mintPredicates();
  const PredInfo &predOf(const Term &t);
  void breakOp(Opd owner, DimRef dim);
  void unlinkFromRank(CellRef xReg, DimRef dim);
  void emitRtBRaise();
  void emitRtHasSrc();
  void emitRtBDrop();
  void emitRtBSet();
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
  void addFieldTo(CellRef owner, CellRef &last,
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
  void emitLifecycle(Container &c, bool uninstall);
  void emitContainerInits(Container &c);
  void beginRoutine(CellRef label, Container *owner);
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
    CellRef label{noCell};
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
  if (n == "Opt") unsupported("Opt<T>", loc);
  if (events_.contains(n) || records_.contains(n)) {
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
    out.emplace_back(std::string(kEventImplicitSource), tyOf(Ty::K::Str));
    out.emplace_back(std::string(kEventImplicitSpace), tyOf(Ty::K::Str));
    out.emplace_back(std::string(kEventImplicitTime), tyOf(Ty::K::Int));
    out.emplace_back(std::string(kEventImplicitOrdinal), tyOf(Ty::K::Int));
    return out;
  }
  if (type == kEventInitialize || type == kEventDestroy) {
    out.emplace_back(std::string(kEventImplicitSource), tyOf(Ty::K::Str));
    out.emplace_back(std::string(kEventImplicitSpace), tyOf(Ty::K::Str));
    out.emplace_back(std::string(kEventImplicitTime), tyOf(Ty::K::Int));
    out.emplace_back(std::string(kEventImplicitOrdinal), tyOf(Ty::K::Int));
  }
  return out;
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
  // Recursive through list elements and field values, so its registers are
  // saved per activation like a cyclic method's.
  tail_    = noCell;
  curRegs_ = &copyRoutine_.regs;
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
  callLabel(rtCopy_);
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
  callLabel(rtCopy_);
  mov(S(nv), S(r0_));
  linkOp(D(y), core_.dims().grab, D(nv));
  mov(S(prevE), S(nv));
  place(lfirst);
  mov(S(e), S(rd(D(e), core_.dims().step)));
  place(lloop);
  branchIfZero(e, ldone);
  mov(S(a0_), S(e));
  callLabel(rtCopy_);
  mov(S(nv), S(r0_));
  linkOp(D(prevE), core_.dims().step, D(nv));
  mov(S(prevE), S(nv));
  mov(S(e), S(rd(D(e), core_.dims().step)));
  jump(lloop);
  place(ldone);
  mov(S(r0_), S(y));
  jump(copyEpi_);
  curRegs_ = nullptr;
  place(copyPro_);
  saveRegs(copyRoutine_);
  jump(copyBody_);
  place(copyEpi_);
  restoreRegs(copyRoutine_);
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
    movC(out, cInt(l.durationNs));
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
  case ELiteral::Kind::None:
    unsupported("none", loc);
  }
  unsupported("literal", loc);
}

Ty Gen::name(const EName &n, CellRef out, SourceLocation loc) {
  if (const Local *l = findLocal(n.name)) {
    mov(S(out), S(l->reg));
    return l->ty;
  }
  if (const FieldInfo *f = findField(n.name)) {
    loadFieldInto(*f, out);
    return f->ty;
  }
  bail(CompileError::UnknownName, "unknown name " + n.name, loc);
}

Ty Gen::special(const ESpecial &s, CellRef out, SourceLocation loc) {
  switch (s.kind) {
  case ESpecial::Kind::Self:
    loadFieldInto(fields_.at(std::string(kFieldName)), out);
    return tyOf(Ty::K::Str);
  case ESpecial::Kind::Parent:
    loadFieldInto(fields_.at(std::string(kFieldParent)), out);
    return tyOf(Ty::K::Str);
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
    if (t.k != Ty::K::List) unsupported("size of a non-list", loc);
    movC(out, zero_);
    listWalk(l, [&](CellRef) {
      emit(OpcodeKind::Add, "n+", {S(out), S(one_)}, {out});
    });
    return tyOf(Ty::K::Int);
  }
  case EUnary::Op::Generate:
    unsupported("generation (!e)", loc);
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
    unsupported("comparison of records or lists", b.left->loc);
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
  case Op::Scan:
    unsupported("string scanning", loc);
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
    if (t.k == Ty::K::Int) {
      emit(OpcodeKind::Trim, "text", {S(v)}, {out});
    } else if (t.k == Ty::K::Str) {
      mov(S(out), S(v));
    } else {
      unsupported("text() of this type", loc);
    }
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
  if (c.args.size() != mi.paramRegs.size()) {
    bail(CompileError::BadArity,
         callee->name + " takes " + std::to_string(mi.paramRegs.size()) +
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
  callLabel(mi.entry);
  mov(S(out), S(mi.retReg));
  return mi.ret;
}

Ty Gen::index(const EIndex &i, CellRef out) {
  const CellRef base = reg(), idx = reg(), cnt = reg(), el = reg(), eq = reg();
  const Ty bt = expr(*i.base, base);
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
  return *ft;
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
    valueInto(*rf.value, v);
    CellRef vc;
    if (it->second.composite()) {
      callAppendField(res, rf.name, v, vc);
    } else {
      callAppendField(res, rf.name, std::nullopt, vc);
      mov(D(vc), S(v));
    }
  }
  mov(S(out), S(res));
  Ty t   = tyOf(Ty::K::Record);
  t.name = r.type;
  return t;
}

Ty Gen::block(const EBlock &b, CellRef out) {
  pushScope();
  Ty last = tyOf(Ty::K::Unit);
  if (b.stmts.empty()) movC(out, unit_c_);
  for (std::size_t i = 0; i < b.stmts.size(); ++i) {
    const Stmt &s     = b.stmts[i];
    const bool isLast = i + 1 == b.stmts.size();
    if (s.isLet) {
      const CellRef r = reg();
      Ty t;
      stmtWrap([&] { t = valueInto(*s.value, r); });
      if (s.type) t = fromType(*s.type, s.loc);
      scopes_.back()[s.name] = Local{r, t};
      if (isLast) movC(out, unit_c_);
      last = tyOf(Ty::K::Unit);
    } else {
      const CellRef target = isLast ? out : reg();
      stmtWrap([&] { last = expr(*s.value, target); });
    }
  }
  popScope();
  return last;
}

Ty Gen::ifExpr(const EIf &i, CellRef out) {
  const CellRef elseL = newLabel(), end = newLabel();
  const CellRef d = dRegAt(stmtDepth_);
  ++stmtDepth_;
  emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
  emit(OpcodeKind::Choice, "else", {S(opref(elseL))});
  const CellRef scratch = reg();
  expr(*i.cond, scratch);
  emit(OpcodeKind::Cut, "cut", {S(d)});
  --stmtDepth_;
  Ty t;
  stmtWrap([&] { t = expr(*i.thenBranch, out); });
  jump(end);
  place(elseL);
  if (i.elseBranch) {
    stmtWrap([&] { expr(*i.elseBranch, out); });
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
  const CellRef d = dRegAt(stmtDepth_);
  ++stmtDepth_;
  emit(OpcodeKind::ChoiceDepth, "d", {}, {d});
  emit(OpcodeKind::Choice, "done", {S(opref(done))});
  const CellRef scratch = reg();
  expr(*w.cond, scratch);
  emit(OpcodeKind::Cut, "cut", {S(d)});
  --stmtDepth_;
  const CellRef bodyOut = reg();
  stmtWrap([&] { expr(*w.body, bodyOut); });
  jump(loop);
  place(done);
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

Ty Gen::forExpr(const EFor &f, CellRef out, SourceLocation loc) {
  if (!f.iterable) return forBelieves(f, out);
  pushScope();
  const CellRef x       = reg();
  const CellRef bodyOut = reg();
  const CellRef loop = newLabel(), done = newLabel();
  if (const auto *r = std::get_if<ERange>(&f.iterable->node)) {
    const CellRef lo = reg(), hi = reg(), by = reg(), cur = reg();
    bounded([&] {
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
    rangeTest(cur, hi, S(by), done);
    mov(S(x), S(cur));
    stmtWrap([&] { expr(*f.body, bodyOut); });
    emit(OpcodeKind::Add, "cur+", {S(cur), S(by)}, {cur});
    jump(loop);
    place(done);
  } else {
    const CellRef l = reg();
    Ty lt;
    bounded([&] { lt = expr(*f.iterable, l); });
    if (lt.k != Ty::K::List) unsupported("iterating a non-list", loc);
    const Ty et              = lt.args.empty() ? tyOf(Ty::K::Unit) : lt.args[0];
    scopes_.back()[f.binder] = Local{x, et};
    listWalk(l, [&](CellRef el) {
      if (et.composite()) {
        mov(S(x), S(el));
      } else {
        mov(S(x), D(el));
      }
      stmtWrap([&] { expr(*f.body, bodyOut); });
    });
  }
  movC(out, unit_c_);
  popScope();
  return tyOf(Ty::K::Unit);
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
  CellRef forList = noCell;
  if (e.recipients) {
    forList     = reg();
    const Ty rt = valueInto(*e.recipients, forList);
    if (rt.k == Ty::K::Str) {
      const CellRef single = reg(), cell = reg();
      mov(S(single), S(forList));
      alloc(forList);
      mov(D(forList), S(cStr(std::string(kListContent))));
      alloc(cell);
      mov(D(cell), S(single));
      callListPush(forList, cell);
    } else if (rt.k != Ty::K::List) {
      bail(CompileError::TypeMismatch, "for needs a name or a list", loc);
    }
  }
  const CellRef entry = callOutbox(kKindEvent);
  CellRef vc;
  callAppendField(entry, std::string(kOutboxEvent), ev, vc);
  callAppendField(entry, std::string(kOutboxSpace), std::nullopt, vc);
  mov(D(vc), S(spaceV));
  if (forList != noCell) {
    callAppendField(entry, std::string(kOutboxFor), forList, vc);
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
    pi.cell            = arena_.makeCell(pd->name);
    const CellRef cell = pi.cell;
    if (!preds_.emplace(pd->name, std::move(pi)).second) {
      bail(CompileError::DuplicateName, "predicate " + pd->name, pd->loc);
    }
    zigzag::expectWritten(
        arena_.link(lastPred_ == noCell ? agentCell_ : lastPred_, dims_.belief,
                    DimVector::POS, cell));
    lastPred_ = cell;
  }
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
/// a3 = replace (keyed `believe`, `revise`), a4 = key count.
///
/// With replace, every other fact with the same key arguments that this source
/// asserts loses the source first, so `-old` is raised before `+new`. Then the
/// fact is added, or only its source is when it is already present.
void Gen::emitRtBSet() {
  beginInternal(rtBSet_);
  const CellRef pred = reg(), term = reg(), src = reg(), repl = reg(),
                nk = reg(), c = reg(), nx = reg(), ct = reg(), t1 = reg(),
                t2 = reg(), i = reg(), u = reg(), cl = reg(), sc = reg();
  const CellRef add = newLabel(), loop1 = newLabel(), next1 = newLabel(),
                keyOk = newLabel(), keySame = newLabel(), kloop = newLabel(),
                loop2 = newLabel(), exists = newLabel(), addSrc = newLabel(),
                create = newLabel();
  mov(S(pred), S(a0_));
  mov(S(term), S(a1_));
  mov(S(src), S(a2_));
  mov(S(repl), S(a3_));
  mov(S(nk), S(a4_));
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
  movC(r0_, zero_);
  ret();
  place(addSrc);
  alloc(sc);
  mov(D(sc), S(src));
  emitAppendTail(D(c), dims_.source, dims_.source, sc);
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
  movC(a0_, cStr(std::string(kBeliefAdded)));
  mov(S(a1_), S(term));
  mov(S(a2_), S(pred));
  callLabel(rtBRaise_);
  movC(r0_, one_);
  ret();
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
        asValue(l->reg, l->ty);
      } else {
        PlanVar v;
        v.cap   = reg();
        v.local = reg();
        v.name  = nm;
        v.ty    = want;
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
  CellRef prev = noCell;
  for (const ArgPlan &ap : pl.args) {
    const CellRef cell = reg();
    if (ap.composite) {
      callCopy(ap.reg, cell);
    } else {
      alloc(cell);
      mov(D(cell), S(ap.reg));
    }
    if (prev == noCell) {
      linkOp(D(tr), core_.dims().grab, D(cell));
    } else {
      linkOp(D(prev), core_.dims().step, D(cell));
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
      callCopy(v.cap, v.local);
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
  CellRef fromReg   = noCell;
  if (b.from) {
    fromReg = reg();
    expr(*b.from, fromReg);
  }
  const CellRef c = reg(), ct = reg();
  const CellRef loop = newLabel(), noMatch = newLabel(), next = newLabel(),
                after = newLabel(), exhausted = newLabel();
  mov(S(c), S(rd(S(pl.pred->cell), core_.dims().clause)));
  place(loop);
  branchIfZero(c, exhausted);
  mov(S(ct), S(rd(D(c), core_.dims().grab)));
  emitMatch(pl, ct, noMatch);
  if (fromReg != noCell) {
    mov(S(a0_), S(c));
    mov(S(a1_), S(fromReg));
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
      movC(a0_, opref(p.cell));
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
    movC(a0_, opref(p.cell));
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
  h.bindEntry = newLabel();
  beginHandlerCtx(h);
  place(h.bindEntry);
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
    place(s.label);
    const PredInfo &p = *s.info;
    // The fact is rebuilt from the event's positional fields; a field the
    // sender left out makes the message malformed and it is ignored.
    const CellRef tr = reg();
    alloc(tr);
    mov(D(tr), S(cStr(*s.pred)));
    CellRef prev = noCell;
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
      if (prev == noCell) {
        linkOp(D(tr), core_.dims().grab, D(cell));
      } else {
        linkOp(D(prev), core_.dims().step, D(cell));
      }
      prev = cell;
    }
    const CellRef who = reg(), sp = reg();
    mov(S(a0_), S(curEvent_));
    movC(a1_, cStr(std::string(kEventImplicitSource)));
    callLabel(rtFind_);
    mov(S(sp), S(r0_));
    branchIfZero(sp, end);
    mov(S(who), D(sp));
    if (!s.untell) {
      movC(a0_, opref(p.cell));
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
        movC(a0_, opref(p.cell));
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
  reserved(kFieldParent, str);
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
    mi.rt.id = nextRoutineId_++;
    curRegs_ = &mi.rt.regs;
    for (const Param &p : mi.def->sig.params) {
      mi.paramRegs.push_back(reg());
      mi.paramTys.push_back(fromType(p.type, p.loc));
    }
    curRegs_ = nullptr;
    for (std::size_t i = 0; i < mi.paramRegs.size(); ++i) {
      mi.argRegs.push_back(mi.cyclic ? global() : mi.paramRegs[i]);
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
  own_     = m.owner;
  tail_    = noCell;
  curRegs_ = &m.rt.regs;
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  place(m.entry);
  if (m.cyclic) jump(m.pro);
  place(m.bodyStart);
  for (std::size_t i = 0; i < m.paramRegs.size(); ++i) {
    scopes_.back()[m.def->sig.params[i].name] =
        Local{m.paramRegs[i], m.paramTys[i]};
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
  curRegs_ = nullptr;
  place(m.epi);
  if (m.cyclic) restoreRegs(m.rt);
  ret();
  if (m.cyclic) {
    place(m.pro);
    saveRegs(m.rt);
    for (std::size_t i = 0; i < m.paramRegs.size(); ++i) {
      mov(S(m.paramRegs[i]), S(m.argRegs[i]));
    }
    jump(m.bodyStart);
  }
}

void Gen::beginHandlerCtx(const HandlerInfo &h) {
  own_     = h.owner;
  tail_    = noCell;
  curRegs_ = nullptr;
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
      h.eventType != kEventInitialize && h.eventType != kEventDestroy) {
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
    h.guardEntry = newLabel();
    routine(h.guardEntry, [&] {
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
  if (h.bindEntry != noCell) callLabel(h.bindEntry);
  if (h.guardEntry != noCell) callLabel(h.guardEntry);
  callLabel(h.bodyEntry);
  emit(OpcodeKind::Cut, "cut", {S(hd)});
  if (checkConsumed) {
    branch(S(fields_.at(std::string(kFieldConsumed)).valueCell), end);
  }
  place(next);
}

void Gen::emitInit(CellRef entryLabel) {
  const CellRef body = newLabel();
  tail_              = noCell;
  curRegs_           = nullptr;
  place(entryLabel);
  callLabel(body);
  jump(haltOp_);
  // The body runs under a frame so a failure unwinds the stack with it.
  tail_ = noCell;
  place(body);
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_        = Ctx{};
  ctx_.retReg = scratchG_;
  own_        = nullptr;
  mov(S(curEvent_), S(zero_));
  for (const Member &m : agent_->members) {
    const auto *v = std::get_if<VarDecl>(&m);
    if (v == nullptr) continue;
    const FieldInfo &f = fields_.at(v->name);
    const CellRef val  = global();
    stmtWrap([&] {
      valueInto(*v->init, val);
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
  ret();
}

void Gen::emitDispatch(CellRef entryLabel) {
  const CellRef body = newLabel();
  tail_              = noCell;
  curRegs_           = nullptr;
  place(entryLabel);
  callLabel(body);
  jump(haltOp_);
  tail_ = noCell;
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
  types.emplace_back(kEventDestroy);
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
      if (h->guardEntry != noCell) {
        zigzag::expectWritten(arena_.link(
            hc, core_.dims().grab, DimVector::POS,
            arena_.makeScalarCell(static_cast<std::int64_t>(h->guardEntry))));
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

void Gen::addFieldTo(CellRef owner, CellRef &last,
                     std::unordered_map<std::string, FieldInfo> &into,
                     const std::string &fname, const Ty &ty) {
  FieldInfo f;
  f.ty        = ty;
  f.nameCell  = arena_.makeCell(fname);
  f.valueCell = initialValueCell(ty);
  zigzag::expectWritten(arena_.link(f.nameCell, core_.dims().values,
                                    DimVector::POS, f.valueCell));
  zigzag::expectWritten(arena_.link(last == noCell ? owner : last,
                                    core_.dims().vars, DimVector::POS,
                                    f.nameCell));
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
  c.k            = k;
  c.name         = name;
  c.params       = &params;
  c.members      = &members;
  c.loc          = loc;
  c.cell         = arena_.makeCell(name);
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
    callLabel(mi.entry);
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

void Gen::beginRoutine(CellRef label, Container *owner) {
  own_     = owner;
  tail_    = noCell;
  curRegs_ = nullptr;
  scopes_.clear();
  dRegs_.clear();
  stmtDepth_ = 0;
  pushScope();
  ctx_        = Ctx{};
  ctx_.retReg = scratchG_;
  place(label);
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
    if (l == nullptr || l->uninstall != uninstall || !l->body) continue;
    expr(*l->body, global());
  }
}

void Gen::emitContainerRoutines() {
  const auto cellsOf = [&](Container &c) {
    return c.instances.empty() ? std::vector<CellRef>{c.cell} : c.instances;
  };
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

    beginRoutine(c.installLabel, &c);
    emitContainerInits(c);
    for (const CellRef cell : cellsOf(c)) {
      const CellRef r = reg();
      movC(r, opref(cell));
      emitAppendTail(S(agentCell_), dims_.skill, dims_.skill, r);
    }
    emitLifecycle(c, false);
    ret();

    beginRoutine(c.uninstallLabel, &c);
    const CellRef gone = newLabel();
    branchIfZero(rd(S(c.member), dims_.skill, true), gone);
    emitLifecycle(c, true);
    for (const CellRef cell : cellsOf(c)) {
      const CellRef r = reg();
      movC(r, opref(cell));
      unlinkFromRank(r, dims_.skill);
    }
    place(gone);
    ret();
  }
  for (auto &[name, c] : behaviors_) {
    beginRoutine(c.installLabel, &c);
    emitContainerInits(c);
    const CellRef doLink = newLabel(), done = newLabel();
    branchIfZero(rd(S(c.cell), dims_.behavior, true), doLink);
    jump(done);
    place(doLink);
    const CellRef r = reg();
    movC(r, opref(c.cell));
    emitAppendTail(S(agentCell_), dims_.behavior, dims_.behavior, r);
    place(done);
    ret();

    beginRoutine(c.uninstallLabel, &c);
    const CellRef off = newLabel();
    branchIfZero(rd(S(c.cell), dims_.behavior, true), off);
    const CellRef u = reg();
    movC(u, opref(c.cell));
    unlinkFromRank(u, dims_.behavior);
    place(off);
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
    if (skill) callLabel(c.prepLabel);
    for (std::size_t i = 0; i < temps.size(); ++i) {
      storeField(c.fields.at((*c.params)[i].name), temps[i]);
    }
    callLabel(c.installLabel);
  } else {
    callLabel(c.uninstallLabel);
  }
  movC(out, unit_c_);
  return tyOf(Ty::K::Unit);
}

CompiledAgent Gen::run() {
  dims_ = registerDimensions(arena_);
  collect();

  zero_           = cInt(0);
  one_            = cInt(1);
  negOne_         = cInt(-1);
  unit_c_         = arena_.makeCell(std::string(kUnitContent));
  true_           = arena_.makeScalarCell(true);
  false_          = arena_.makeScalarCell(false);
  a0_             = global();
  a1_             = global();
  a2_             = global();
  r0_             = global();
  r1_             = global();
  curEvent_       = global();
  depth_          = arena_.makeScalarCell(std::int64_t{0});
  depthStr_       = global();
  nameTmp_        = global();
  scratchG_       = global();
  failStep_       = mkOp(OpcodeKind::Fail, "SFAIL");
  haltOp_         = mkOp(OpcodeKind::Halt, "HALT");
  failOp_         = mkOp(OpcodeKind::Halt, "FAIL");
  rtFind_         = newLabel();
  rtAppendField_  = newLabel();
  rtListPush_     = newLabel();
  rtOutbox_       = newLabel();
  rtCopy_         = newLabel();
  rtBRaise_       = newLabel();
  rtHasSrc_       = newLabel();
  rtBDrop_        = newLabel();
  rtBSet_         = newLabel();
  rtSettle_       = newLabel();
  rtCapFind_      = newLabel();
  a3_             = global();
  a4_             = global();
  roundsG_        = global();
  copyPro_        = newLabel();
  copyEpi_        = newLabel();
  copyBody_       = newLabel();
  copyRoutine_.id = nextRoutineId_++;

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
  buildProgramStructure();
  return out;
}

} // namespace

Compiler::Compiler(zigzag::vortex::VortexCore &core) : core_(core), vm_(core) {}

Compiler *Compiler::setBeliefEventDepth(std::size_t rounds) noexcept {
  beliefEventDepth_ = rounds;
  return this;
}

std::expected<CompiledAgent, CompileFailure>
Compiler::compileAgent(const Unit &unit, std::string_view agentType) {
  try {
    Gen g(core_, vm_, unit, agentType, beliefEventDepth_);
    return g.run();
  } catch (const Bail &b) {
    return std::unexpected(b.failure);
  }
}

} // namespace xanadu::vagent
