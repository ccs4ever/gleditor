/**
 * @file compiler.cpp
 * @brief Lowers a vicon AST to inline Vortex opcode graphs
 *        (design/vicon-implementation.md).
 *
 * Emission contract: an expression's entry op is what a `Jump`/spin link or
 * `#Choice` altOp lands on; producing writes `produceOut_` and jumps to
 * `produceCont_`; exhaustion falls to a `#Fail`, which pops the next outer
 * choice point. The produce context travels through members because the
 * emit* signatures return only the fragment's entry op.
 *
 * Deviation note for iteration 2: the design's flag+drain bounded-eval and
 * `e \ n` kill-flag idioms assume a flag write survives `release()`. Under
 * ArenaManifold's trail, every write to a compile-time cell is recorded and
 * undone when the covering choice point pops, so flags cannot persist
 * through a resume. `emitBounded` therefore composes structurally (produce
 * flows straight into the continuation) and `limit` emits the documented
 * kill-flag graph but the flag cannot hold: `\n` currently yields all of
 * `e`, and stale `if`-else choice points can re-fire. The committed
 * std:goal module hits the same wall and resolves it host-side with a
 * second cursor and `cut()`; doing that inline needs a new opcode or a new
 * ChoicePoint field, both out of scope for I2.
 */
#include "common/xanadu/vicon/compiler.hpp"

#include <string>
#include <utility>
#include <variant>

#include "common/xanadu/vicon/parser.hpp"
#include "gleditor/logging.hpp"

namespace xanadu::vicon {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

std::unexpected<CompileFailure> fail(CompileError code, std::string detail,
                                     SourceLocation loc = {}) {
  return std::unexpected(
      CompileFailure{.code = code, .detail = std::move(detail), .loc = loc});
}

/// A parse refusal becomes the closest-named compile refusal.
CompileError mapParseError(ParseError code) {
  switch (code) {
  case ParseError::LexicalError:
    return CompileError::LexicalError;
  case ParseError::UnbalancedScan:
    return CompileError::UnbalancedScan;
  case ParseError::Unimplemented:
    return CompileError::UnsupportedConstruct;
  default:
    return CompileError::UnexpectedToken;
  }
}

} // namespace

Compiler::Compiler(zigzag::vortex::VortexCore &core)
    : core_(core), vm_(core), stdlib_(core, vm_) {}

// -- Emission helpers ---------------------------------------------------------

CellRef Compiler::mintOp(OpcodeKind kind, std::string_view label) {
  return vm_.mintOpcode(kind, label);
}

CellRef Compiler::mintScalar(std::int64_t v) {
  return core_.arena().makeScalarCell(v);
}

/// Jump/Branch/Choice targets travel as a scalar cell's *value* — the VM
/// renders the operand and takes the integer as the op to run (the opref
/// idiom std:goal uses everywhere).
CellRef Compiler::opref(CellRef target) {
  return core_.arena().makeScalarCell(static_cast<std::int64_t>(target));
}

void Compiler::spin(CellRef from, CellRef to) {
  zigzag::expectWritten(
      core_.arena().link(from, core_.dims().spin, zigzag::DimVector::POS, to));
}

CellRef Compiler::emitProduce(CellRef srcCell) {
  // Value out := src (whole-content write), then Jump to the op the current
  // continuation names. Landing on haltOp_ suspends; landing on another
  // fragment continues inline.
  CellRef prod = mintOp(OpcodeKind::Value, "VICON_PRODUCE");
  CellRef susp = mintOp(OpcodeKind::Jump, "VICON_SUSPEND");
  spin(prod, susp);
  core_.bindInput(prod, produceOut_);
  core_.bindInput(prod, zero_);
  core_.bindInput(prod, negOne_);
  core_.bindInput(prod, srcCell);
  core_.bindInput(susp, opref(produceCont_));
  return prod;
}

std::expected<CellRef, CompileFailure>
Compiler::emitInto(const Expr &e, CellRef out, CellRef cont) {
  const CellRef saveOut  = produceOut_;
  const CellRef saveCont = produceCont_;
  produceOut_            = out;
  produceCont_           = cont;
  auto res               = emitExpr(e);
  produceOut_            = saveOut;
  produceCont_           = saveCont;
  return res;
}

/// Bounded positions (operands of `to`, a `\n` count, call arguments,
/// non-final sequence elements) compose structurally: the operand's produce
/// writes `resultCell` and jumps to `contOp`, and its exhaustion propagates
/// to the next outer choice point. See the file-top deviation note.
std::expected<CellRef, CompileFailure>
Compiler::emitBounded(const Expr &e, CellRef resultCell, CellRef contOp) {
  return emitInto(e, resultCell, contOp);
}

CellRef Compiler::guardedAlt(CellRef alt) {
  // Every resume path inside a limited `e` checks each active kill flag
  // first: Branch kill -> KILL, else fall through to the real altOp.
  for (const auto &[flag, killOp] : killGuards_) {
    CellRef gate = mintOp(OpcodeKind::Branch, "VICON_KILLGATE");
    core_.bindInput(gate, flag);
    core_.bindInput(gate, opref(killOp));
    spin(gate, alt);
    alt = gate;
  }
  return alt;
}

std::expected<CellRef, CompileFailure>
Compiler::emitSequence(const std::vector<ExprPtr> &exprs) {
  if (exprs.empty()) {
    return mintOp(OpcodeKind::Fail, "VICON_SEQ_EMPTY");
  }
  // Emit right-to-left so each earlier expression's produce continuation is
  // the next expression's entry.
  auto next = emitInto(*exprs.back(), produceOut_, produceCont_);
  if (!next) {
    return next;
  }
  for (std::size_t i = exprs.size() - 1; i-- > 0;) {
    next = emitBounded(*exprs[i], scratchCell_, *next);
    if (!next) {
      return next;
    }
  }
  return next;
}

// -- Leaf fragments -----------------------------------------------------------

std::expected<CellRef, CompileFailure>
Compiler::emitLiteral(const ELiteral &l) {
  CellRef lit = noCell;
  switch (l.kind) {
  case ELiteral::Kind::Int:
    lit = mintScalar(l.intValue);
    break;
  case ELiteral::Kind::Real:
    lit = core_.arena().makeScalarCell(l.realValue);
    break;
  case ELiteral::Kind::Str:
  case ELiteral::Kind::Cset:
    // I2 has no cset representation; the text rides along as a string.
    lit = core_.arena().makeCell(l.text);
    break;
  }
  return emitProduce(lit);
}

std::expected<CellRef, CompileFailure> Compiler::emitExpr(const Expr &e) {
  return std::visit(
      [&](const auto &n) -> std::expected<CellRef, CompileFailure> {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, ELiteral>) {
          return emitLiteral(n);
        } else if constexpr (std::is_same_v<T, EIdent>) {
          // Resolve the newest binding of the name in the cursor's scope.
          CellRef tmp = core_.arena().makeCell();
          CellRef res = mintOp(OpcodeKind::Resolve, "VICON_IDENT");
          core_.bindInput(res, core_.arena().makeCell(n.name));
          core_.bindOutput(res, tmp);
          spin(res, emitProduce(tmp));
          return res;
        } else if constexpr (std::is_same_v<T, EKeyword>) {
          if (n.name == "null") {
            return emitProduce(nullCell_);
          }
          if (n.name == "fail") {
            return mintOp(OpcodeKind::Fail, "VICON_KEYFAIL");
          }
          return fail(CompileError::UnsupportedConstruct,
                      "&" + n.name + " is not supported in iteration 2", e.loc);
        } else if constexpr (std::is_same_v<T, EUnary>) {
          if (n.op != EUnary::Op::Neg) {
            // `!x` needs a rank walk (I3 scanning family); `not e` needs
            // genuine produce-vs-fail commit, which trailed cells cannot
            // express inline (see the deviation note).
            return fail(CompileError::UnsupportedConstruct,
                        "unary operator is not supported in iteration 2",
                        e.loc);
          }
          CellRef in  = core_.arena().makeCell();
          CellRef res = core_.arena().makeCell();
          CellRef neg = mintOp(OpcodeKind::Neg, "VICON_NEG");
          core_.bindInput(neg, in);
          core_.bindOutput(neg, res);
          spin(neg, emitProduce(res));
          return emitBounded(*n.operand, in, neg);
        } else if constexpr (std::is_same_v<T, EBinary>) {
          if (n.op == EBinary::Op::Scan) {
            return emitScan(n);
          }
          return emitBinary(n);
        } else if constexpr (std::is_same_v<T, EAlternation>) {
          return emitAlternation(n);
        } else if constexpr (std::is_same_v<T, ETo>) {
          return emitTo(n);
        } else if constexpr (std::is_same_v<T, ELimit>) {
          return emitLimit(n);
        } else if constexpr (std::is_same_v<T, ECall>) {
          return emitCall(n);
        } else if constexpr (std::is_same_v<T, ESequence>) {
          return emitSequence(n.exprs);
        } else if constexpr (std::is_same_v<T, EIf>) {
          return emitIf(n);
        } else if constexpr (std::is_same_v<T, EEvery>) {
          return emitEvery(n);
        } else if constexpr (std::is_same_v<T, EWhile>) {
          return emitWhile(n);
        } else if constexpr (std::is_same_v<T, EUntil>) {
          return emitUntil(n);
        } else if constexpr (std::is_same_v<T, EFail>) {
          return mintOp(OpcodeKind::Fail, "VICON_FAIL");
        } else if constexpr (std::is_same_v<T, EReturn>) {
          // Inside a procedure a return produces through the shared return
          // bridge; at top level it is just the expression.
          if (insideProc_) {
            if (nullptr == n.expr) {
              // Bare `return` yields &null to the caller.
              CellRef p = mintOp(OpcodeKind::Value, "VICON_RET_NULL");
              CellRef j = mintOp(OpcodeKind::Jump, "VICON_RET_J");
              core_.bindInput(p, procRes_);
              core_.bindInput(p, zero_);
              core_.bindInput(p, negOne_);
              core_.bindInput(p, nullCell_);
              core_.bindInput(j, opref(retResolve_));
              spin(p, j);
              return p;
            }
            return emitInto(*n.expr, procRes_, retResolve_);
          }
          return nullptr == n.expr ? emitProduce(nullCell_) : emitExpr(*n.expr);
        } else if constexpr (std::is_same_v<T, EBreak>) {
          return fail(loopStack_.empty() ? CompileError::BreakOutsideLoop
                                         : CompileError::UnsupportedConstruct,
                      "break is iteration 4", e.loc);
        } else if constexpr (std::is_same_v<T, ENext>) {
          return fail(loopStack_.empty() ? CompileError::NextOutsideLoop
                                         : CompileError::UnsupportedConstruct,
                      "next is iteration 4", e.loc);
        } else {
          // ESubscript, ESlice, ESuspend, ERepeat, ECase.
          return fail(CompileError::UnsupportedConstruct,
                      "construct is scheduled for a later iteration", e.loc);
        }
      },
      e.node);
}

// -- Operators ----------------------------------------------------------------

std::expected<CellRef, CompileFailure> Compiler::emitBinary(const EBinary &b) {
  const CellRef lC  = core_.arena().makeCell();
  const CellRef rC  = core_.arena().makeCell();
  const CellRef res = core_.arena().makeCell();

  auto arithEntry = [&](OpcodeKind kind) -> CellRef {
    CellRef op = mintOp(kind, "VICON_BIN");
    core_.bindInput(op, lC);
    core_.bindInput(op, rC);
    core_.bindOutput(op, res);
    spin(op, emitProduce(res));
    return op;
  };
  auto cmpEntry = [&](OpcodeKind kind) -> CellRef {
    // A comparison is a generator of its right operand: produce rC when the
    // test holds, else fall to #Fail (no booleans in the language).
    CellRef cmp   = mintOp(kind, "VICON_CMP");
    CellRef br    = mintOp(OpcodeKind::Branch, "VICON_CMP_BR");
    CellRef dead  = mintOp(OpcodeKind::Fail, "VICON_CMP_FAIL");
    CellRef t     = core_.arena().makeCell();
    CellRef prodE = emitProduce(rC);
    core_.bindInput(cmp, lC);
    core_.bindInput(cmp, rC);
    core_.bindOutput(cmp, t);
    core_.bindInput(br, t);
    core_.bindInput(br, opref(prodE));
    spin(cmp, br);
    spin(br, dead);
    spin(dead, prodE);
    return cmp;
  };

  std::optional<CellRef> opEntry;
  switch (b.op) {
  case EBinary::Op::Add:
  case EBinary::Op::Concat: // || concatenates strings; Add does that already
    opEntry = arithEntry(OpcodeKind::Add);
    break;
  case EBinary::Op::Sub:
    opEntry = arithEntry(OpcodeKind::Sub);
    break;
  case EBinary::Op::Mul:
    opEntry = arithEntry(OpcodeKind::Mul);
    break;
  case EBinary::Op::Div:
    opEntry = arithEntry(OpcodeKind::Div);
    break;
  case EBinary::Op::Mod:
    opEntry = arithEntry(OpcodeKind::Mod);
    break;
  case EBinary::Op::Lt:
  case EBinary::Op::StrLt:
    opEntry = cmpEntry(OpcodeKind::Lt);
    break;
  case EBinary::Op::Le:
  case EBinary::Op::StrLe:
    opEntry = cmpEntry(OpcodeKind::Lte);
    break;
  case EBinary::Op::Eq:
  case EBinary::Op::StrEq:
    opEntry = cmpEntry(OpcodeKind::Eq);
    break;
  case EBinary::Op::Ne:
  case EBinary::Op::StrNe:
    opEntry = cmpEntry(OpcodeKind::Neq);
    break;
  case EBinary::Op::Gt:
  case EBinary::Op::StrGt:
    opEntry = cmpEntry(OpcodeKind::Gt);
    break;
  case EBinary::Op::Ge:
  case EBinary::Op::StrGe:
    opEntry = cmpEntry(OpcodeKind::Gte);
    break;
  case EBinary::Op::Assign:
  case EBinary::Op::RevAssign: {
    // Both spellings emit a Bind: a fresh binding shadows the name on the
    // cursor's +d.vars rank. `:=` cannot skip the trail in this VM, so for
    // I2 both forms revert on backtrack like `<-` (spec calls that out).
    // The left side is the target *name* — it is never run as an
    // expression; only the rhs is evaluated and then bound.
    const auto *id = std::get_if<EIdent>(&b.left->node);
    if (nullptr == id) {
      return fail(CompileError::UnexpectedToken,
                  "assignment target must be an identifier", b.left->loc);
    }
    CellRef bind = mintOp(OpcodeKind::Bind, "VICON_ASSIGN");
    core_.bindInput(bind, core_.arena().makeCell(id->name));
    core_.bindInput(bind, res);
    spin(bind, emitProduce(res));
    return emitBounded(*b.right, res, bind);
  }
  case EBinary::Op::Pow:
    return fail(CompileError::UnsupportedConstruct,
                "^ has no opcode; use * for now", {});
  default:
    return fail(CompileError::UnsupportedConstruct,
                "binary operator is not supported in iteration 2", {});
  }

  // Left produces into lC and falls into the right operand; right produces
  // into rC and lands on the operator op. LIFO resumption varies the right
  // operand fastest, which is Icon's ordering.
  auto rEntry = emitBounded(*b.right, rC, *opEntry);
  if (!rEntry) {
    return rEntry;
  }
  return emitBounded(*b.left, lC, *rEntry);
}

// -- Generators ---------------------------------------------------------------

std::expected<CellRef, CompileFailure>
Compiler::emitAlternation(const EAlternation &alt) {
  // Choice ALT then fall through to e1; e1's exhaustion pops the frame and
  // lands on e2.
  auto e2 = emitInto(*alt.second, produceOut_, produceCont_);
  if (!e2) {
    return e2;
  }
  auto e1 = emitInto(*alt.first, produceOut_, produceCont_);
  if (!e1) {
    return e1;
  }
  CellRef entry = mintOp(OpcodeKind::Choice, "VICON_ALT");
  core_.bindInput(entry, opref(guardedAlt(*e2)));
  spin(entry, *e1);
  return entry;
}

std::expected<CellRef, CompileFailure> Compiler::emitTo(const ETo &to) {
  const CellRef loC = core_.arena().makeCell();
  const CellRef hiC = core_.arena().makeCell();
  const CellRef byC =
      nullptr == to.by ? one_ : CellRef{core_.arena().makeCell()};
  const CellRef cur = core_.arena().makeCell();
  const CellRef dir = core_.arena().makeCell();
  const CellRef t   = core_.arena().makeCell();
  const CellRef ok  = core_.arena().makeCell();

  // The retry idiom, with the bound test ahead of every produce so
  // `5 to 3` produces nothing. `init` runs before the first #Choice so a
  // resume's release() cannot revert it.
  CellRef init  = mintOp(OpcodeKind::Add, "VICON_TO_INIT");
  CellRef dirT  = mintOp(OpcodeKind::Gte, "VICON_TO_DIR");
  CellRef dirBr = mintOp(OpcodeKind::Branch, "VICON_TO_DIRBR");
  CellRef negT  = mintOp(OpcodeKind::Lt, "VICON_TO_NEG");
  CellRef join  = mintOp(OpcodeKind::Jump, "VICON_TO_JOIN");
  CellRef posT  = mintOp(OpcodeKind::Lt, "VICON_TO_POS");
  CellRef test  = mintOp(OpcodeKind::Not, "VICON_TO_TEST");
  CellRef okBr  = mintOp(OpcodeKind::Branch, "VICON_TO_OKBR");
  CellRef dead  = mintOp(OpcodeKind::Fail, "VICON_TO_FAIL");
  CellRef push  = mintOp(OpcodeKind::Choice, "VICON_TO_CHOICE");
  CellRef step  = mintOp(OpcodeKind::Add, "VICON_TO_STEP");
  CellRef jHead = mintOp(OpcodeKind::Jump, "VICON_TO_HEAD");
  CellRef prodE = emitProduce(cur);

  spin(init, dirT);
  spin(dirT, dirBr);
  spin(dirBr, negT);
  spin(negT, join);
  spin(join, posT);
  spin(posT, test);
  spin(test, okBr);
  spin(okBr, dead);
  spin(dead, push);
  spin(push, prodE);
  spin(step, jHead);

  core_.bindInput(init, loC);
  core_.bindInput(init, zero_);
  core_.bindOutput(init, cur);
  // dir := (step >= 0): the bound check differs per walking direction.
  core_.bindInput(dirT, byC);
  core_.bindInput(dirT, zero_);
  core_.bindOutput(dirT, dir);
  core_.bindInput(dirBr, dir);
  core_.bindInput(dirBr, opref(posT));
  // Descending: exhausted once cur < hi. Ascending: once hi < cur.
  core_.bindInput(negT, cur);
  core_.bindInput(negT, hiC);
  core_.bindOutput(negT, t);
  core_.bindInput(join, opref(test));
  core_.bindInput(posT, hiC);
  core_.bindInput(posT, cur);
  core_.bindOutput(posT, t);
  core_.bindInput(test, t);
  core_.bindOutput(test, ok);
  core_.bindInput(okBr, ok);
  core_.bindInput(okBr, opref(push));
  core_.bindInput(push, opref(guardedAlt(step)));
  core_.bindInput(step, cur);
  core_.bindInput(step, byC);
  core_.bindOutput(step, cur);
  core_.bindInput(jHead, opref(dirT));

  // Bounds evaluate in order lo, hi, by, each producing into its operand
  // cell and falling into the next stage.
  const CellRef head = init;
  auto byStage       = head;
  if (nullptr != to.by) {
    auto e = emitBounded(*to.by, byC, head);
    if (!e) {
      return e;
    }
    byStage = *e;
  }
  auto hi = emitBounded(*to.hi, hiC, byStage);
  if (!hi) {
    return hi;
  }
  return emitBounded(*to.lo, loC, *hi);
}

std::expected<CellRef, CompileFailure> Compiler::emitLimit(const ELimit &lim) {
  const CellRef nC   = core_.arena().makeCell();
  const CellRef cnt  = core_.arena().makeCell();
  const CellRef kill = core_.arena().makeCell();
  const CellRef eRes = core_.arena().makeCell();
  const CellRef t    = core_.arena().makeCell();

  CellRef entry   = mintOp(OpcodeKind::Choice, "VICON_LIMIT");
  CellRef initCnt = mintOp(OpcodeKind::Add, "VICON_LIMIT_INIT");
  CellRef initK   = mintOp(OpcodeKind::Value, "VICON_LIMIT_KILLCLR");
  CellRef gate    = mintOp(OpcodeKind::Sub, "VICON_LIMIT_GATE");
  CellRef cntT    = mintOp(OpcodeKind::Eq, "VICON_LIMIT_CNT");
  CellRef killBr  = mintOp(OpcodeKind::Branch, "VICON_LIMIT_KILLBR");
  CellRef delivJ  = mintOp(OpcodeKind::Jump, "VICON_LIMIT_DELIVJ");
  CellRef setKill = mintOp(OpcodeKind::Value, "VICON_LIMIT_KILLSET");
  CellRef killOp  = mintOp(OpcodeKind::Fail, "VICON_LIMIT_KILL");
  CellRef exit    = mintOp(OpcodeKind::Fail, "VICON_LIMIT_EXIT");

  // e's produce continuation is the gate; every resume path emitted inside
  // e is poisoned by the kill flag while this site is on the stack.
  killGuards_.push_back({kill, killOp});
  auto eEntry = emitInto(*lim.expr, eRes, gate);
  killGuards_.pop_back();
  if (!eEntry) {
    return eEntry;
  }

  CellRef delivE = emitProduce(eRes);

  spin(entry, initCnt);
  spin(initCnt, initK);
  spin(initK, *eEntry);
  spin(gate, cntT);
  spin(cntT, killBr);
  spin(killBr, delivJ);
  spin(delivJ, setKill);
  spin(setKill, delivE);

  core_.bindInput(entry, opref(guardedAlt(exit)));
  core_.bindInput(initCnt, nC);
  core_.bindInput(initCnt, zero_);
  core_.bindOutput(initCnt, cnt);
  core_.bindInput(initK, kill);
  core_.bindInput(initK, zero_);
  core_.bindInput(initK, negOne_);
  core_.bindInput(initK, zero_);
  core_.bindInput(gate, cnt);
  core_.bindInput(gate, one_);
  core_.bindOutput(gate, cnt);
  core_.bindInput(cntT, cnt);
  core_.bindInput(cntT, zero_);
  core_.bindOutput(cntT, t);
  core_.bindInput(killBr, t);
  core_.bindInput(killBr, opref(setKill));
  core_.bindInput(delivJ, opref(delivE));
  core_.bindInput(setKill, kill);
  core_.bindInput(setKill, zero_);
  core_.bindInput(setKill, negOne_);
  core_.bindInput(setKill, one_);

  return emitBounded(*lim.count, nC, entry);
}

std::expected<CellRef, CompileFailure> Compiler::emitScan(const EBinary &) {
  return fail(CompileError::UnsupportedConstruct,
              "string scanning ? is iteration 3", {});
}

// -- Control structures
// ---------------------------------------------------------

std::expected<CellRef, CompileFailure> Compiler::emitIf(const EIf &e) {
  // Flag-guarded else: `taken` is a compile-minted cell, so a write to it
  // is not trailed under any mark pushed after compile() — it survives
  // release() (unlike a runtime Bind cell). The catch frame's alt lands on
  // the guard: once the condition has produced even once, the flag is set
  // and the stale else frame dies instead of re-firing. `init` re-runs on
  // every activation, so a re-entered `if` (inside a loop) resets.
  const CellRef taken = core_.arena().makeCell();
  CellRef init        = mintOp(OpcodeKind::Value, "VICON_IF_INIT");
  CellRef dead        = mintOp(OpcodeKind::Fail, "VICON_IF_DEAD");
  CellRef guard       = mintOp(OpcodeKind::Branch, "VICON_IF_GUARD");
  CellRef entry       = mintOp(OpcodeKind::Choice, "VICON_IF");
  CellRef setT        = mintOp(OpcodeKind::Value, "VICON_IF_TAKEN");
  CellRef setJ        = mintOp(OpcodeKind::Jump, "VICON_IF_TAKENJ");

  auto elseE = e.elseBranch ? emitInto(*e.elseBranch, produceOut_, produceCont_)
                            : std::expected<CellRef, CompileFailure>{dead};
  if (!elseE) {
    return elseE;
  }
  auto thenE = emitInto(*e.thenBranch, produceOut_, produceCont_);
  if (!thenE) {
    return thenE;
  }
  auto cond = emitBounded(*e.cond, scratchCell_, setT);
  if (!cond) {
    return cond;
  }

  spin(init, entry);
  spin(entry, *cond);
  spin(guard, *elseE);
  spin(setT, setJ);

  core_.bindInput(init, taken);
  core_.bindInput(init, zero_);
  core_.bindInput(init, negOne_);
  core_.bindInput(init, zero_);
  core_.bindInput(entry, opref(guardedAlt(guard)));
  core_.bindInput(guard, taken);
  core_.bindInput(guard, opref(dead));
  core_.bindInput(setT, taken);
  core_.bindInput(setT, zero_);
  core_.bindInput(setT, negOne_);
  core_.bindInput(setT, one_);
  core_.bindInput(setJ, opref(*thenE));
  return init;
}

std::expected<CellRef, CompileFailure> Compiler::emitEvery(const EEvery &e) {
  // Choice DONE then drive the generator: each produce runs the body, whose
  // own produces land on #Fail and drain LIFO into the generator's frames.
  const CellRef drain = mintOp(OpcodeKind::Fail, "VICON_EVERY_DRAIN");
  auto body = nullptr == e.body ? std::expected<CellRef, CompileFailure>{drain}
                                : emitInto(*e.body, scratchCell_, drain);
  if (!body) {
    return body;
  }
  auto gen = emitInto(*e.gen, scratchCell_, *body);
  if (!gen) {
    return gen;
  }
  const CellRef done  = emitProduce(nullCell_);
  const CellRef entry = mintOp(OpcodeKind::Choice, "VICON_EVERY");
  core_.bindInput(entry, opref(guardedAlt(done)));
  spin(entry, *gen);
  return entry;
}

std::expected<CellRef, CompileFailure> Compiler::emitWhile(const EWhile &e) {
  // Choice DONE then evaluate the condition; each produce runs the body and
  // jumps back to re-enter the condition fresh. The body is consumed — its
  // produce is a side effect that loops, never a result of the `while`.
  const CellRef loop = mintOp(OpcodeKind::Jump, "VICON_WHILE_LOOP");
  auto body = nullptr == e.body ? std::expected<CellRef, CompileFailure>{loop}
                                : emitInto(*e.body, scratchCell_, loop);
  if (!body) {
    return body;
  }
  auto cond = emitBounded(*e.cond, scratchCell_, *body);
  if (!cond) {
    return cond;
  }
  const CellRef done  = emitProduce(nullCell_);
  const CellRef entry = mintOp(OpcodeKind::Choice, "VICON_WHILE");
  core_.bindInput(entry, opref(guardedAlt(done)));
  core_.bindInput(loop, opref(*cond));
  spin(entry, *cond);
  return entry;
}

std::expected<CellRef, CompileFailure> Compiler::emitUntil(const EUntil &e) {
  // `until e1 do e2`: loop condition is evaluated at each iteration.
  // When e1 fails, #Fail pops to body and loops back.
  // When e1 succeeds, it cuts the loop choice point and finishes with &null.
  CellRef loopHead    = mintOp(OpcodeKind::Choice, "VICON_UNTIL_HEAD");
  CellRef cutOp       = mintOp(OpcodeKind::Cut, "VICON_UNTIL_CUT");
  CellRef loopBack    = mintOp(OpcodeKind::Jump, "VICON_UNTIL_LOOP");
  const CellRef prodE = emitProduce(nullCell_);

  auto body = nullptr == e.body
                  ? std::expected<CellRef, CompileFailure>{loopBack}
                  : emitInto(*e.body, scratchCell_, loopBack);
  if (!body) {
    return body;
  }

  auto cond = emitBounded(*e.cond, scratchCell_, cutOp);
  if (!cond) {
    return cond;
  }

  spin(loopHead, *cond);
  spin(cutOp, prodE);
  spin(loopBack, loopHead);

  core_.bindInput(loopHead, opref(guardedAlt(*body)));
  core_.bindInput(loopBack, opref(loopHead));

  return loopHead;
}

// -- Calls
// ----------------------------------------------------------------------

std::expected<CellRef, CompileFailure> Compiler::emitCall(const ECall &c) {
  // The `write` builtin tees its argument: suspend the result to the driver
  // for printing, then forward it to the consumer on resume. At top level
  // (the produce target IS the halt op) the plain produce already prints.
  if (c.name == "write") {
    if (c.args.size() != 1) {
      return fail(CompileError::UnexpectedToken,
                  "write takes exactly one argument", {});
    }
    const CellRef in = core_.arena().makeCell();
    CellRef body     = noCell;
    if (produceCont_ == haltOp_) {
      body = emitProduce(in);
    } else {
      const CellRef tail = emitProduce(in);
      const CellRef tee  = mintOp(OpcodeKind::Choice, "VICON_WRITE");
      const CellRef copy = mintOp(OpcodeKind::Value, "VICON_WRITE_TEE");
      const CellRef susp = mintOp(OpcodeKind::Jump, "VICON_WRITE_SUSP");
      core_.bindInput(tee, opref(guardedAlt(tail)));
      core_.bindInput(copy, resultCell_);
      core_.bindInput(copy, zero_);
      core_.bindInput(copy, negOne_);
      core_.bindInput(copy, in);
      core_.bindInput(susp, opref(haltOp_));
      spin(tee, copy);
      spin(copy, susp);
      spin(susp, tail);
      body = tee;
    }
    return emitInto(*c.args.front(), in, body);
  }

  const auto target = procTargets_.find(c.name);
  if (target == procTargets_.end()) {
    return fail(CompileError::UnboundIdentifier,
                "no procedure or builtin named " + c.name, {});
  }
  const auto params = procParams_[c.name];
  const CellRef rC  = procResultCells_[c.name];
  // The call's produce continuation: copy the procedure's result cell into
  // the caller's produce sink and jump outward.
  const CellRef retCont = emitProduce(rC);

  // Tail of the call frame: bind each declared parameter, then the return
  // continuation, then jump through the indirection cell.
  CellRef retB = mintOp(OpcodeKind::Bind, "VICON_CALL_RET");
  core_.bindInput(retB, core_.arena().makeCell("__vicon_ret"));
  core_.bindInput(retB, mintScalar(static_cast<std::int64_t>(retCont)));
  CellRef jmp = mintOp(OpcodeKind::Jump, "VICON_CALL_GO");
  core_.bindInput(jmp, target->second); // value is patched to the entry op
  spin(retB, jmp);

  std::vector<CellRef> argCells;
  argCells.reserve(c.args.size());
  for (std::size_t i = 0; i < c.args.size(); ++i) {
    argCells.push_back(core_.arena().makeCell());
  }
  CellRef tail            = retB;
  const std::size_t bound = std::min(params.size(), c.args.size());
  for (std::size_t i = bound; i-- > 0;) {
    CellRef b = mintOp(OpcodeKind::Bind, "VICON_CALL_ARG");
    core_.bindInput(b, core_.arena().makeCell(params[i]));
    core_.bindInput(b, argCells[i]);
    spin(b, tail);
    tail = b;
  }

  // Evaluate arguments left to right; the last produce lands on the binds.
  auto chain = std::expected<CellRef, CompileFailure>{tail};
  for (std::size_t i = c.args.size(); i-- > 0;) {
    chain = emitBounded(*c.args[i], argCells[i], *chain);
    if (!chain) {
      return chain;
    }
  }
  return chain;
}

// -- Program assembly
// -----------------------------------------------------------

std::expected<CompiledProgram, CompileFailure>
Compiler::compile(const Program &program) {
  // Per-program plumbing.
  resultCell_  = core_.arena().makeCell();
  nullCell_    = core_.arena().makeCell();
  scratchCell_ = core_.arena().makeCell();
  zero_        = mintScalar(0);
  one_         = mintScalar(1);
  negOne_      = mintScalar(-1);
  haltOp_      = mintOp(OpcodeKind::Halt, "VICON_HALT");
  failOp_      = mintOp(OpcodeKind::Fail, "VICON_FAIL");
  // The procedure-return bridge: resolve the return continuation the caller
  // bound and jump to it indirectly (Jump renders its operand's value).
  retTmp_         = core_.arena().makeCell();
  retResolve_     = mintOp(OpcodeKind::Resolve, "VICON_RET");
  CellRef retJump = mintOp(OpcodeKind::Jump, "VICON_RET_GO");
  core_.bindInput(retResolve_, core_.arena().makeCell("__vicon_ret"));
  core_.bindOutput(retResolve_, retTmp_);
  core_.bindInput(retJump, retTmp_);
  spin(retResolve_, retJump);

  procedures_.clear();
  procTargets_.clear();
  procEntries_.clear();
  procParams_.clear();
  procResultCells_.clear();
  killGuards_.clear();
  insideProc_ = false;

  // Register signatures first so calls (forward or recursive) can jump
  // through the indirection cell before its value is patched.
  for (const auto &decl : program.procedures) {
    procedures_[decl.name]      = mintScalar(0);
    procTargets_[decl.name]     = procedures_[decl.name];
    procParams_[decl.name]      = decl.params;
    procResultCells_[decl.name] = core_.arena().makeCell();
    GLEDITOR_LOG_DEBUG("vicon.compile", "registered procedure {}", decl.name);
  }

  // Top-level statements produce the program's results.
  produceOut_  = resultCell_;
  produceCont_ = haltOp_;
  auto entry   = emitSequence(program.statements);
  if (!entry) {
    return std::unexpected(entry.error());
  }

  // Bodies produce into their result cell and bridge back to the caller's
  // continuation.
  for (const auto &decl : program.procedures) {
    insideProc_ = true;
    procRes_    = procResultCells_[decl.name];
    auto body   = emitInto(*decl.body, procRes_, retResolve_);
    insideProc_ = false;
    if (!body) {
      return std::unexpected(body.error());
    }
    // Patch the call-site indirection cell to the body's entry op now that
    // the body exists (forward references and recursion both resolve here).
    std::ignore = core_.value(procTargets_[decl.name], 0, -1,
                              CellValue{static_cast<std::int64_t>(*body)});
    procEntries_[decl.name] = *body;
  }

  CompiledProgram out;
  out.entry      = *entry;
  out.resultCell = resultCell_;
  out.haltOp     = haltOp_;
  out.failOp     = failOp_;
  for (const auto &decl : program.procedures) {
    out.procedures.emplace_back(decl.name, procEntries_[decl.name]);
  }
  GLEDITOR_LOG_DEBUG("vicon.compile", "compiled entry={} halt={}",
                     static_cast<long long>(out.entry),
                     static_cast<long long>(out.haltOp));
  return out;
}

std::expected<CompiledProgram, CompileFailure>
Compiler::compileSource(std::string_view source) {
  Parser parser(source);
  auto program = parser.parseProgram();
  if (!program) {
    return std::unexpected(
        CompileFailure{.code   = mapParseError(program.error().code),
                       .detail = program.error().detail,
                       .loc    = program.error().loc});
  }
  return compile(*program);
}

std::expected<CompiledProgram, CompileFailure>
Compiler::compileExpression(std::string_view source) {
  Parser parser(source);
  auto expr = parser.parseExpression();
  if (!expr) {
    return std::unexpected(
        CompileFailure{.code   = mapParseError(expr.error().code),
                       .detail = expr.error().detail,
                       .loc    = expr.error().loc});
  }
  Program program;
  program.statements.push_back(std::move(*expr));
  return compile(program);
}

} // namespace xanadu::vicon
