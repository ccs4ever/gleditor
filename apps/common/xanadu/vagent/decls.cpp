#include "common/xanadu/vagent/decls.hpp"

#include <bit>
#include <format>
#include <map>
#include <string_view>
#include <utility>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vagent/ast_clone.hpp"
#include "common/xanadu/vagent/fragment.hpp"
#include "common/xanadu/vagent/marshal.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace xanadu::vagent {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;

DeclDisposition declDisposition(DeclKind kind) noexcept {
  switch (kind) {
  case DeclKind::Agent:
  case DeclKind::Event:
  case DeclKind::Goal:
  case DeclKind::Predicate:
  case DeclKind::Plan:
  case DeclKind::Method:
  case DeclKind::Answer:
  case DeclKind::Handler:
    return DeclDisposition::Lower;
  case DeclKind::Field:
  case DeclKind::Record:
  case DeclKind::Belief:
  case DeclKind::Question:
  case DeclKind::Skill:
  case DeclKind::Capacity:
  case DeclKind::Behavior:
    return DeclDisposition::Lower;
  case DeclKind::Lifecycle:
    return DeclDisposition::Stub;
  case DeclKind::Uses:
  case DeclKind::Install:
  case DeclKind::BehaviorUse:
    return DeclDisposition::Lower;
  }
  return DeclDisposition::Refuse;
}

namespace {

// A decl body is not a code"…" literal, so its head goes through
// compileFragment on a synthesized context: a FragmentKey forged from
// the decl's loc plus a per-head ordinal, a FragmentRecord owning a
// cloneExpr of the body, and a FragmentInfo carrying the declared
// signature. That is the *same* table contract expand+check produce
// for literals, so the emitted head is indistinguishable from a
// literal's — descriptor, refs and recipe included — which is what
// lets run/graft/reify treat image-original bodies uniformly.
struct Lowerer {
  zigzag::vortex::VortexCore &core;
  zigzag::vortex::VortexVM &vm;
  Compiler compiler;
  zigzag::ArenaManifold &arena;
  const Program &program;
  LoweredImage image;
  std::vector<Diagnostic> errors;

  // The synthesized fragment context — held for the whole walk so
  // context.emitted dedupes and the records outlive every call.
  FragmentTable fragments;
  std::map<FragmentKey, FragmentMeta> metas;
  FragmentInfoMap infos;
  CompilerBindings bindings;
  FragmentContext context;
  std::uint64_t ordinal{0};

  // Name-keyed indexes gathered during the walk: module symbols (so a
  // plan/on host lookup is a find, not a rank scan) and the declared
  // parameter lists plans/handlers inherit their head signatures from.
  std::map<std::string, CellRef, std::less<>> symbols;
  std::map<std::string, std::vector<Parameter>, std::less<>> signatures;
  std::map<CellRef, std::size_t> clauseCounts;

  Lowerer(zigzag::vortex::VortexCore &core_, zigzag::vortex::VortexVM &vm_,
          ScalarCompilerLimits limits, const Program &program_)
      : core(core_), vm(vm_), compiler(core, vm, std::move(limits)),
        arena(core.arena()), program(program_) {
    context.fragments = &fragments;
    context.meta      = &metas;
    context.info      = &infos;
    context.bindings  = &bindings;
    context.module    = program.module;
  }

  [[nodiscard]] const auto &dims() const { return core.dims(); }

  void refuse(const Decl &decl, std::string_view what) {
    errors.push_back(Diagnostic{DiagnosticCode::InvalidDeclaration,
                                std::string{what}, decl.loc});
  }

  // A symbol cell on the module's +d.vars rank — the exportSymbol
  // shape minus the entry edge, which only callable heads carry.
  CellRef mintSymbol(std::string_view name) {
    const auto sym = arena.makeCell(std::string{name});
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, image.ownModule, dims().vars),
                   dims().vars, DimVector::POS, sym));
    symbols.emplace(name, sym);
    image.symbols.push_back(sym);
    return sym;
  }

  // head +d.entry → op, +d.name → label, +d.values → descriptor. The
  // label is the declared body-head name §3.5's resume identity
  // records — "decl:" prefixed so reify:/src: labels never collide.
  std::expected<CellRef, Diagnostic>
  emitHead(const Decl &decl, const Expr &body,
           const std::vector<Parameter> &params, std::string_view label,
           Type result) {
    Expr literal;
    literal.kind  = ExprKind::Unit;
    literal.loc   = decl.loc;
    literal.stamp = ScopeStamp{0, ordinal};
    // §5.8's owner convention: inside a skill or behavior's member
    // walk every emitted head declares a trailing `$owner` Cell
    // parameter — the dispatcher binds the runtime owner cell there,
    // and bare field names lower to #FIND/#SET hops on it instead of
    // compile-time cells. The slot is unconditional so dispatch stays
    // uniform when the owner declares no fields.
    std::vector<Parameter> effective = params;
    if (!memberPrefix_.empty())
      effective.push_back(Parameter{.name = "$owner",
                                    .type = Type{"Cell"}});
    const FragmentKey key{decl.loc, ordinal++};
    fragments.emplace(key,
                      FragmentRecord{.body = CodeBody{std::string{label},
                                                      effective,
                                                      cloneExpr(body)},
                                     .closed = true});
    infos.emplace(key, FragmentInfo{.result = std::move(result),
                                    .pure   = false,
                                    .closed = true,
                                    .arity  = effective.size(),
                                    .ownerFields = memberFields_});
    const auto object = compileFragment(literal, context, compiler);
    if (!object) return std::unexpected(object.error());
    image.heads.push_back(object->head);
    return object->head;
  }

  // A head whose real body the scalar emitter refused gets the full
  // apparatus anyway — over a placeholder literal — so run/graft see
  // the same shape and only the graph is a stub (M2 replaces it).
  std::expected<CellRef, Diagnostic>
  emitDeclHead(const Decl &decl, const Expr &body,
               const std::vector<Parameter> &params, std::string_view label,
               Type result, bool stubIsBoolean) {
    if (auto head = emitHead(decl, body, params, label, Type{result}))
      return head;
    Expr stub;
    stub.kind = stubIsBoolean ? ExprKind::Boolean : ExprKind::Unit;
    stub.text = stubIsBoolean ? "true" : "";
    stub.loc  = decl.loc;
    return emitHead(decl, stub, params, label, std::move(result));
  }

  // The clause record graftPlan/graftHandler mint: +d.name → label,
  // +d.grab → test head, +d.spin → body head, appended on the host
  // symbol's +d.clause rank.
  CellRef mintClause(CellRef host, std::string_view name, CellRef testHead,
                     CellRef bodyHead,
                     const std::optional<std::int64_t> &priority) {
    const auto clause = arena.makeCell();
    zigzag::expectWritten(arena.link(clause, dims().name, DimVector::POS,
                                     arena.makeCell(std::string{name})));
    zigzag::expectWritten(
        arena.link(clause, dims().grab, DimVector::POS, testHead));
    zigzag::expectWritten(
        arena.link(clause, dims().spin, DimVector::POS, bodyHead));
    if (priority) {
      if (const auto rec = varsRecord(arena, core, clause, "priority", true))
        zigzag::expectWritten(arena.link(
            *rec, dims().values, DimVector::POS,
            arena.makeScalarCell(static_cast<std::int64_t>(*priority))));
    }
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, host, dims().clause), dims().clause,
                   DimVector::POS, clause));
    image.clauses.push_back(clause);
    return clause;
  }

  // -- §3.8 host steps: statement-position verbs lower as spine cells ---------

  // The kinds that become host steps in M2 — believe/forget/revise.
  // Everything else statement-side stays on the single-fragment path
  // (Emit/Tell/… need marshal machinery M4 brings, so a Block mixing
  // them still refuses through the stub-head fallback).
  static bool isHostVerb(ExprKind kind) {
    switch (kind) {
    case ExprKind::Believe:
    case ExprKind::Forget:
    case ExprKind::Revise:
    case ExprKind::Adopt:
    case ExprKind::Install:
    case ExprKind::Uninstall:
    case ExprKind::Mount:
    case ExprKind::Unmount:
      return true;
    case ExprKind::Emit:
    case ExprKind::AwaitEvent:
    case ExprKind::AwaitTerm:
    case ExprKind::Achieve:
    case ExprKind::Tell:
    case ExprKind::Untell:
    case ExprKind::Stop:
    case ExprKind::Ask:
    case ExprKind::Drop:
    case ExprKind::Spawn:
    case ExprKind::Consume:
      return true;
    default:
      return false;
    }
  }

  // A `Capacity.method(args)` call in statement position lowers as a
  // host verb too — the `Ident.name` callee makes it syntactically
  // unambiguous, and the runtime resolves the installed instance the
  // capacity rides on. An unqualified `method(...)` keeps the scalar
  // path: nothing here can tell it from an ordinary call.
  static bool isCapacityCall(const Expr &expr) {
    return expr.kind == ExprKind::Call && !expr.children.empty() &&
           expr.children.front()->kind == ExprKind::Field &&
           !expr.children.front()->children.empty() &&
           expr.children.front()->children.front()->kind ==
               ExprKind::Identifier;
  }

  // The host verb a statement lowers to — the statement itself, or
  // the verb hoisted out of `x := <verb>` / `let x := <verb>`. The
  // two callers (the spine gate and the emitter) must agree on this
  // or a bound verb lowers as dead scalar code.
  static const Expr *hostVerbOf(const Expr &stmt) {
    if (isHostVerb(stmt.kind) || isCapacityCall(stmt)) return &stmt;
    const auto hoisted = [](const Expr &inner) {
      return isHostVerb(inner.kind) || isCapacityCall(inner);
    };
    if (stmt.kind == ExprKind::Binary && stmt.text == ":=" &&
        stmt.children.size() == 2 &&
        stmt.children[0]->kind == ExprKind::Identifier &&
        hoisted(*stmt.children[1]))
      return stmt.children[1].get();
    if (stmt.kind == ExprKind::Let && stmt.children.size() == 1 &&
        hoisted(*stmt.children.front()))
      return stmt.children.front().get();
    return nullptr;
  }

  // Any host-verb statement among the body's top level (a bare verb as
  // the whole body counts as a one-member spine).
  static bool bodyHasHostVerb(const Expr &body) {
    if (body.kind == ExprKind::Block) {
      for (const auto &child : body.children)
        if (hostVerbOf(*child)) return true;
      return false;
    }
    return hostVerbOf(body) != nullptr;
  }

  // A `+d.grab`-shaped term cell: functor name as content, args on
  // +d.step. V1 marshals ground terms only — LogicVariable patterns
  // (forget's `?x`) need unification, which is the fragment-position
  // clause-walk's job, not the host step's.
  std::expected<CellRef, Diagnostic> marshalTerm(const Expr &expr) {
    return marshalTermExpr(arena, core, expr);
  }

  // marshalTerm's pattern form: LogicVariable arguments marshal as
  // '?'-prefixed text cells — MetaRuntime::termUnifies reads them as
  // wildcards. Used for `believes`-shaped goal conditions, which the
  // scalar emitter cannot lower and emitDeclHead would otherwise stub
  // to a vacuous `true`.
  std::expected<CellRef, Diagnostic> marshalPattern(const Expr &expr) {
    return marshalPatternExpr(arena, core, expr);
  }

  // A goal-call argument: literals keep their type (marshalTerm's
  // text-cells convention would make "4" and 4 indistinguishable to a
  // bound param), a nested Call marshals functor + args like a Term —
  // the bound value is the term's ref. Anything else (identifiers,
  // operators) has no value the marshal can name and refuses loudly.
  std::expected<CellRef, Diagnostic> marshalArg(const Expr &expr) {
    switch (expr.kind) {
    case ExprKind::Integer:
      return arena.makeScalarCell(
          static_cast<std::int64_t>(std::stoll(expr.text)));
    case ExprKind::Float:
      return arena.makeScalarCell(std::stod(expr.text));
    case ExprKind::Boolean:
      return arena.makeScalarCell(expr.text == "true");
    case ExprKind::String:
      return arena.makeCell(expr.text);
    case ExprKind::Term:
      return marshalTerm(expr);
    case ExprKind::Call: {
      if (expr.children.empty() ||
          expr.children.front()->kind != ExprKind::Identifier)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "goal-call argument is not a named call", expr.loc});
      const auto cell = arena.makeCell(expr.children.front()->text);
      for (std::size_t i = 1; i < expr.children.size(); ++i) {
        auto arg = marshalArg(*expr.children[i]);
        if (!arg) return arg;
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, cell, dims().step),
                       dims().step, DimVector::POS, *arg));
      }
      return cell;
    }
    default:
      return std::unexpected(Diagnostic{
          DiagnosticCode::InvalidPosition,
          std::format("goal-call argument does not marshal: kind {}",
                      static_cast<int>(expr.kind)),
          expr.loc});
    }
  }

  // `verb G(a,…)` — the goal marshals as a term cell: functor name +
  // marshaled args on +d.step. textOf still reads the name; the args
  // ride the same cell so adopt and remote-achieve payloads carry the
  // same shape. The goal's declared signature bounds the arity here —
  // a mismatched call refuses before the runtime ever binds.
  std::expected<CellRef, Diagnostic> marshalGoalCall(const Expr &call) {
    if (call.kind != ExprKind::Call || call.children.empty() ||
        call.children.front()->kind != ExprKind::Identifier)
      return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                        "goal operand is not a named call",
                                        call.loc});
    const auto &name = call.children.front()->text;
    std::size_t arity = call.children.size() - 1;
    if (const auto it = signatures.find(std::string{name});
        it != signatures.end() && it->second.size() != arity)
      return std::unexpected(Diagnostic{
          DiagnosticCode::InvalidDeclaration,
          std::format("goal '{}' takes {} argument(s), call carries {}",
                      name, it->second.size(), arity),
          call.loc});
    const auto cell = arena.makeCell(name);
    for (std::size_t i = 1; i < call.children.size(); ++i) {
      auto arg = marshalArg(*call.children[i]);
      if (!arg) return std::unexpected(arg.error());
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, cell, dims().step),
                     dims().step, DimVector::POS, *arg));
    }
    return cell;
  }

  // The host-step cell: +d.verb names the MetaRuntime entry point, the
  // +d.vars record marshals the call's arguments under their parameter
  // names — for the belief verbs that is exactly `term`. Peer and
  // assertion turn are implicit, supplied by the executor at run time.
  // A suspension verb's `else` arm lowers to an ordinary head whose
  // extra param `cause` carries the expiry reason — the checker types
  // the recovery against the awaited value and names `cause: Str`
  // itself. The head is stored as a scalar ref: it is shared
  // program-image state a second run of the spine must not evict.
  std::expected<CellRef, Diagnostic>
  marshalElse(const Decl &decl, const std::vector<Parameter> &params,
              const Expr &recovery, std::string_view clauseName) {
    auto ps = params;
    ps.push_back(Parameter{.name = "cause",
                           .type = Type{"Str"},
                           .loc  = recovery.loc});
    return emitHead(decl, recovery, ps,
                    std::format("{}:else", label(clauseName)),
                    Type{"<any>"});
  }

  // A named call `F(a1, a2)` → a term cell named F whose +d.step
  // children are the marshaled args — the payload shape the
  // install/mount verbs and creation members all marshal to.
  std::expected<CellRef, Diagnostic>
  marshalCallPayload(const Expr &call) {
    if (call.children.empty() ||
        call.children.front()->kind != ExprKind::Identifier)
      return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                        "operand is not a named call",
                                        call.loc});
    auto payload = arena.makeCell(call.children.front()->text);
    for (std::size_t i = 1; i < call.children.size(); ++i) {
      auto arg = marshalArg(*call.children[i]);
      if (!arg) return std::unexpected(arg.error());
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, payload, dims().step),
                     dims().step, DimVector::POS, *arg));
    }
    return payload;
  }

  std::expected<CellRef, Diagnostic>
  mintHostStep(const Expr &expr, const Decl &decl,
               const std::vector<Parameter> &params,
               std::string_view clauseName) {
    // Capacity-qualified calls lower before the verb switch — Call is
    // not a host-verb kind, the qualifier is what gave it away.
    if (isCapacityCall(expr)) {
      const Expr &callee  = *expr.children.front();
      const auto fx       = effectSigs_.find(
          callee.children.front()->text + "." + callee.text);
      const auto step    = arena.makeCell();
      zigzag::expectWritten(arena.link(
          step, verbDim(), DimVector::POS,
          arena.makeCell(fx != effectSigs_.end() ? "effectcall"
                                                 : "capcall")));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      field("capacity", arena.makeCell(callee.children.front()->text));
      field("method", arena.makeCell(callee.text));
      // Args marshal on a term cell named by the method — the
      // executor's argValuesOf reads its +d.step children like every
      // other staged payload.
      const auto payload = arena.makeCell(callee.text);
      for (std::size_t i = 1; i < expr.children.size(); ++i) {
        auto arg = marshalArg(*expr.children[i]);
        if (!arg) return std::unexpected(arg.error());
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, payload, dims().step),
                       dims().step, DimVector::POS, *arg));
      }
      field("payload", payload);
      if (fx != effectSigs_.end()) {
        // V6: the signature's `within` is the park's deadline — the
        // binding's turn-clock is turns, like every other wait. `else`
        // lowers the same recovery head ask parks for its own ladder.
        field("deadline", arena.makeScalarCell(fx->second.turns));
        if (fx->second.preempts)
          field("preempts", arena.makeScalarCell(std::int64_t{1}));
        if (expr.recovery) {
          auto head =
              marshalElse(decl, params, *expr.recovery, clauseName);
          if (!head) return std::unexpected(head.error());
          field("else", arena.makeScalarCell(
                            static_cast<std::int64_t>(*head)));
        }
      }
      return step;
    }
    std::string_view verb;
    switch (expr.kind) {
    case ExprKind::Believe:
      verb = "believe";
      break;
    case ExprKind::Forget:
      verb = "forget";
      break;
    case ExprKind::Revise:
      verb = "revise";
      break;
    case ExprKind::Adopt:
      verb = "adopt";
      break;
    case ExprKind::Install:
      verb = "install";
      break;
    case ExprKind::Uninstall:
      verb = "uninstall";
      break;
    case ExprKind::Mount:
      verb = "mount";
      break;
    case ExprKind::Unmount:
      verb = "unmount";
      break;
    case ExprKind::Emit:
      verb = "emit";
      break;
    case ExprKind::AwaitEvent:
    case ExprKind::AwaitTerm:
      verb = "await";
      break;
    case ExprKind::Achieve:
      verb = "achieve";
      break;
    case ExprKind::Tell:
      verb = "tell";
      break;
    case ExprKind::Untell:
      verb = "untell";
      break;
    case ExprKind::Stop:
      verb = "stop";
      break;
    case ExprKind::Ask:
      verb = "ask";
      break;
    case ExprKind::Drop:
      verb = "drop";
      break;
    case ExprKind::Spawn:
      verb = "spawn";
      break;
    case ExprKind::Consume:
      verb = "consume";
      break;
    default:
      return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                        "statement is not a host verb",
                                        expr.loc});
    }
    // consume carries no operands — the bare verb cell is the whole
    // signal; dispatch reads it as "stop iterating this event's
    // clause rank" once the handler that ran it completes.
    if (expr.kind == ExprKind::Consume) {
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      return step;
    }
    // tell/untell stage cross-agent assertions through the outbox:
    // peer by name, fact as a marshaled term. The receiver-side
    // assert is the adapter's delivery work — the turn just stages
    // the record like emit does. stop's operand is the same kind of
    // Agent-typed bare name (self/parent or a bound agent var).
    if (expr.kind == ExprKind::Tell || expr.kind == ExprKind::Untell ||
        expr.kind == ExprKind::Stop) {
      const bool stops = expr.kind == ExprKind::Stop;
      if (expr.children.size() < (stops ? 1 : 2) ||
          expr.children[0]->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "peer operand is not a name",
                                          expr.loc});
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      field("peer", arena.makeCell(expr.children[0]->text));
      if (!stops) {
        auto term = marshalTerm(*expr.children[1]);
        if (!term) return std::unexpected(term.error());
        field("payload", *term);
      }
      return step;
    }

    // ask peer.q(args) within <dur> — suspension staged like the
    // other cross-agent verbs: the question rides the payload as a
    // marshaled term (functor + arg children), the peer by name, the
    // within bound as the turn-clock scalar. The `else` recovery
    // lowers to a `{cause: Str}`-param'd head the executor evaluates
    // on expiry — the fallback is an evaluated expression.
    if (expr.kind == ExprKind::Ask) {
      CellRef elseHead = noCell;
      if (expr.recovery) {
        auto head = marshalElse(decl, params, *expr.recovery, clauseName);
        if (!head) return std::unexpected(head.error());
        elseHead = *head;
      }
      if (!expr.deadline || expr.deadline->kind != ExprKind::Duration)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "ask's within deadline must be a duration", expr.loc});
      const auto &call   = *expr.children.front();
      const auto &callee = *call.children.front();
      if (callee.children.empty() ||
          callee.children.front()->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "ask's peer is not a name",
                                          expr.loc});
      auto payload = arena.makeCell(callee.text);
      for (std::size_t i = 1; i < call.children.size(); ++i) {
        auto arg = marshalTerm(*call.children[i]);
        if (!arg) return std::unexpected(arg.error());
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, payload, dims().step),
                       dims().step, DimVector::POS, *arg));
      }
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      field("peer", arena.makeCell(callee.children.front()->text));
      field("payload", payload);
      field("deadline", arena.makeScalarCell(static_cast<std::int64_t>(
                            std::stoll(expr.deadline->text))));
      if (elseHead != noCell)
        field("else", arena.makeScalarCell(
                          static_cast<std::int64_t>(elseHead)));
      return step;
    }

    // drop's operand is the Intention handle a bound adopt wrote —
    // the vars record's name marshals; the runtime resolves it back
    // to the record's cell. Expressions refusing to be plain names
    // (drop <call>) don't lower.
    if (expr.kind == ExprKind::Drop) {
      if (expr.children.empty() ||
          expr.children.front()->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "drop's operand is not a handle name",
                                          expr.loc});
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      if (const auto rec = varsRecord(arena, core, step, "handle", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS,
                       arena.makeCell(expr.children.front()->text)));
      return step;
    }

    // spawn Name() [in <space>] — the child's declared name and the
    // scope name marshal; the result binding rides the hoist's
    // `result` field.
    if (expr.kind == ExprKind::Spawn) {
      const auto &call = *expr.children.front();
      if (call.children.empty() ||
          call.children.front()->kind != ExprKind::Identifier)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "spawn's operand is not an agent name", expr.loc});
      if (expr.scope && expr.scope->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "spawn's in-scope is not a name",
                                          expr.loc});
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      field("name", arena.makeCell(call.children.front()->text));
      if (expr.scope) field("scope", arena.makeCell(expr.scope->text));
      return step;
    }

    // install S(args) / mount B(args) — the call marshals as a
    // payload term exactly like achieve's: functor is the skill or
    // behavior name, +d.step children carry the arguments the runtime
    // binds to the declared parameters when it seeds the frame or
    // instance.
    if (expr.kind == ExprKind::Install || expr.kind == ExprKind::Mount) {
      const auto payload = marshalCallPayload(*expr.children.front());
      if (!payload) return std::unexpected(payload.error());
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      if (const auto rec = varsRecord(arena, core, step, "payload", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS, *payload));
      return step;
    }

    // uninstall S / unmount B — the bare name is the whole operand;
    // the runtime finds the frame or instance by it.
    if (expr.kind == ExprKind::Uninstall || expr.kind == ExprKind::Unmount) {
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      if (const auto rec = varsRecord(arena, core, step, "name", true))
        zigzag::expectWritten(arena.link(*rec, dims().values, DimVector::POS,
                                         arena.makeCell(expr.text)));
      return step;
    }

    // achieve shares adopt's goal-name marshal — the delegation the
    // runtime resolves on the parent intention. The grammar's only
    // deadline form is the remote one — `by <agent> within <dur>` —
    // which stages an outbox record and parks the parent on the
    // bound; `else` rides the same expiry-evaluation path as ask's.
    if (expr.kind == ExprKind::Achieve) {
      CellRef elseHead = noCell;
      if (expr.recovery) {
        auto head = marshalElse(decl, params, *expr.recovery, clauseName);
        if (!head) return std::unexpected(head.error());
        elseHead = *head;
      }
      if (expr.recipients && expr.recipients->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "achieve's by-target is not a name",
                                          expr.loc});
      if (expr.deadline && expr.deadline->kind != ExprKind::Duration)
        return std::unexpected(Diagnostic{
            DiagnosticCode::InvalidPosition,
            "achieve's within deadline must be a duration", expr.loc});
      if (expr.children.empty() || expr.children.front()->children.empty() ||
          expr.children.front()->children.front()->kind != ExprKind::Identifier)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "achieve's operand is not a goal name", expr.loc});
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      const auto goalTerm = marshalGoalCall(*expr.children.front());
      if (!goalTerm) return std::unexpected(goalTerm.error());
      field("goal", *goalTerm);
      if (expr.recipients) field("peer", arena.makeCell(expr.recipients->text));
      if (expr.deadline)
        field("deadline", arena.makeScalarCell(static_cast<std::int64_t>(
                              std::stoll(expr.deadline->text))));
      if (elseHead != noCell)
        field("else", arena.makeScalarCell(
                          static_cast<std::int64_t>(elseHead)));
      return step;
    }

    // await marshals its own fields before the generic term path —
    // AwaitEvent carries no children at all (name in `text`, binding
    // in `binding`), and `within`'s deadline must be a literal so the
    // runtime can hold blocked_until as a plain scalar.
    if (expr.kind == ExprKind::AwaitEvent || expr.kind == ExprKind::AwaitTerm) {
      // The `[guard]` is a wake filter: it mints a Bool head whose
      // params are the clause's plus the `as` binding (a Cell ref-box
      // the runtime fills with the delivered payload term — `r.job`
      // reaches it through #FIND like an `on` alias). A guard-false
      // percept leaves the intention parked for the next match.
      CellRef guardHead = noCell;
      if (expr.guard) {
        auto ps = params;
        if (expr.binding)
          ps.push_back(Parameter{.name = *expr.binding,
                                 .type = Type{"Cell"},
                                 .loc  = expr.loc});
        auto head = emitHead(decl, *expr.guard, ps,
                             std::format("{}:await:guard",
                                         label(clauseName)),
                             Type{"Bool"});
        if (!head) return std::unexpected(head.error());
        guardHead = *head;
      }
      CellRef elseHead = noCell;
      if (expr.recovery) {
        auto head = marshalElse(decl, params, *expr.recovery, clauseName);
        if (!head) return std::unexpected(head.error());
        elseHead = *head;
      }
      // The turn protocol's only clock is turn-indexed — `within`'s
      // Duration marshals as its magnitude and expires in *turns*.
      // Wall-clock deadlines are the adapter's Timer capacity
      // ({Duration, Event} → Timer), not a field the intention record
      // can compare; the approximation is honest because it is the
      // only clock this layer owns.
      if (!expr.deadline || expr.deadline->kind != ExprKind::Duration)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "await's within deadline must be a duration", expr.loc});
      const auto step = arena.makeCell();
      zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                       arena.makeCell(std::string{verb})));
      const auto field = [&](std::string_view name, CellRef value) {
        if (const auto rec = varsRecord(arena, core, step, name, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, value));
      };
      if (expr.kind == ExprKind::AwaitEvent) {
        field("event", arena.makeCell(expr.text));
        if (expr.binding) field("binding", arena.makeCell(*expr.binding));
      } else {
        auto term = marshalTerm(*expr.children.front());
        if (!term) return std::unexpected(term.error());
        field("event", *term);
      }
      field("deadline", arena.makeScalarCell(static_cast<std::int64_t>(
                            std::stoll(expr.deadline->text))));
      if (guardHead != noCell)
        field("guard", arena.makeScalarCell(
                           static_cast<std::int64_t>(guardHead)));
      if (elseHead != noCell)
        field("else", arena.makeScalarCell(
                          static_cast<std::int64_t>(elseHead)));
      return step;
    }

    if (expr.children.empty())
      return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                        std::format("{} carries no term", verb),
                                        expr.loc});

    const auto step = arena.makeCell();
    zigzag::expectWritten(arena.link(step, verbDim(), DimVector::POS,
                                     arena.makeCell(std::string{verb})));

    if (expr.kind == ExprKind::Adopt) {
      // adopt's operand is a Call — the goal marshals as a term cell:
      // textOf reads the name, +d.step children carry the bound args
      // for the runtime's positional binding.
      const auto goalTerm = marshalGoalCall(*expr.children.front());
      if (!goalTerm) return std::unexpected(goalTerm.error());
      if (const auto rec = varsRecord(arena, core, step, "goal", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS, *goalTerm));
      return step;
    }

    if (expr.kind == ExprKind::Emit) {
      // emit's operand is an event value — Identifier or Call naming a
      // declared event; the name marshals as payload and the §3.7
      // record's kind/target/id fill at run time. `in` names the
      // delivery scope and `for` the recipients — separate fields, so
      // `emit E{} in s for t` carries both; compound/computed target
      // expressions still refuse.
      const auto nameOf = [](const Expr &e) -> const std::string * {
        return e.kind == ExprKind::Identifier ? &e.text : nullptr;
      };
      const std::string *scope  = nullptr;
      const std::string *target = nullptr;
      if (expr.scope && !(scope = nameOf(*expr.scope)))
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "emit's in-scope is not a name",
                                          expr.loc});
      if (expr.recipients && !(target = nameOf(*expr.recipients)))
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "emit's for-target is not a name",
                                          expr.loc});
      const auto &operand          = *expr.children.front();
      const std::string *eventName = nullptr;
      const Expr *record           = nullptr;
      if (operand.kind == ExprKind::Identifier)
        eventName = &operand.text;
      else if (operand.kind == ExprKind::Record) {
        eventName = &operand.text;
        if (!operand.children.empty()) record = &operand;
      } else if (operand.kind == ExprKind::Call && !operand.children.empty() &&
                 operand.children.front()->kind == ExprKind::Identifier &&
                 operand.children.size() == 1)
        eventName = &operand.children.front()->text;
      if (!eventName)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "emit's operand is not an event "
                                          "value",
                                          expr.loc});
      // Field-carrying payload: values marshal as +d.step children
      // (the term-cell convention), each labeled +d.name with its
      // record field name — textOf(payload) still reads the event
      // name for drain and await matching.
      CellRef payload = noCell;
      if (record) {
        auto marshaled = marshalTerm(*record);
        if (!marshaled) return std::unexpected(marshaled.error());
        payload = *marshaled;
      } else {
        payload = arena.makeCell(*eventName);
      }
      if (const auto rec = varsRecord(arena, core, step, "payload", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS, payload));
      if (scope)
        if (const auto rec = varsRecord(arena, core, step, "scope", true))
          zigzag::expectWritten(arena.link(*rec, dims().values, DimVector::POS,
                                           arena.makeCell(*scope)));
      if (target)
        if (const auto rec = varsRecord(arena, core, step, "target", true))
          zigzag::expectWritten(arena.link(*rec, dims().values, DimVector::POS,
                                           arena.makeCell(*target)));
      return step;
    }

    auto term = marshalTerm(*expr.children.front());
    if (!term) return term;
    if (const auto rec = varsRecord(arena, core, step, "term", true))
      zigzag::expectWritten(
          arena.link(*rec, dims().values, DimVector::POS, *term));
    return step;
  }

  DimRef verbDim() {
    if (verbDim_ == zigzag::noCell)
      verbDim_ = core.findOrMintDimension("d.verb");
    return verbDim_;
  }
  DimRef verbDim_{zigzag::noCell};

  // The owner prefix method/answer members qualify under — "S" while
  // a `skill S` or `behavior B` lowers its members, empty elsewhere.
  // behaviorDesc_ is the descriptor record member clauses and `uses`
  // file against while a behavior lowers; creationSteps_ collects the
  // install/mount host steps an agent's creation members marshal to,
  // in declaration order.
  std::string memberPrefix_;
  CellRef behaviorDesc_{noCell};
  CellRef usesTarget_{noCell};
  std::vector<CellRef> creationSteps_;
  // The declaring owner's field set while a skill/behavior lowers its
  // members — emitHead stamps it onto each member head's info so the
  // emitter resolves the names on the runtime owner. Empty everywhere
  // else.
  OwnerFields memberFields_;
  // Collect the member walk's field set before the first member head
  // emits — init heads take it too (`var b := a + 1` reads a).
  OwnerFields memberFieldsOf(const Decl &owner) {
    OwnerFields fields;
    for (const auto &member : owner.members)
      if (member.kind == DeclKind::Field && member.result)
        fields.emplace(member.name, *member.result);
    return fields;
  }
  // V6's effect surface: a capacity's `effect` methods keyed
  // "Cap.method" — the signature's `within` is the call's deadline
  // (the checker refuses an effect without one) and `preempts` rides
  // the staged entry so the binding's scheduling rule is durable.
  struct EffectSig {
    bool preempts{false};
    std::int64_t turns{0};
  };
  std::map<std::string, EffectSig, std::less<>> effectSigs_;
  DimRef capacityDim() {
    if (capacityDim_ == zigzag::noCell)
      capacityDim_ = core.findOrMintDimension("d.capacity");
    return capacityDim_;
  }
  DimRef capacityDim_{zigzag::noCell};
  DimRef usesDim() {
    if (usesDim_ == zigzag::noCell)
      usesDim_ = core.findOrMintDimension("d.uses");
    return usesDim_;
  }
  DimRef usesDim_{zigzag::noCell};

  // Split the body at host-verb statements: each run of ordinary
  // statements lowers to one fragment segment, each verb to a host
  // step, and the sequence lands on clause +d.step — the spine the
  // executor walks (§3.5's resumable-graph shape; the $step resume
  // index itself is M4's intention machinery). `+d.spin` keeps a
  // descriptor-bearing head on top for signatureOf.
  void emitSpine(const Decl &decl, CellRef clause,
                 const std::vector<Parameter> &params,
                 std::string_view clauseName) {
    std::vector<const Expr *> statements;
    if (decl.body->kind == ExprKind::Block)
      for (const auto &child : decl.body->children)
        statements.push_back(child.get());
    else
      statements.push_back(decl.body.get());

    std::size_t member    = 0;
    const auto linkMember = [&](CellRef cell) {
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, clause, dims().step), dims().step,
                     DimVector::POS, cell));
      ++member;
    };

    std::vector<const Expr *> run;
    const auto flushRun = [&]() {
      if (run.empty()) return;
      Expr segment;
      segment.kind = ExprKind::Block;
      segment.loc  = decl.loc;
      for (const auto *stmt : run) segment.children.push_back(cloneExpr(*stmt));
      run.clear();
      auto head =
          emitDeclHead(decl, segment, params,
                       std::format("{}:seg{}", label(clauseName), member),
                       Type{"()"}, false);
      if (head) {
        linkMember(*head);
      } else {
        errors.push_back(head.error());
      }
    };

    for (const auto *stmt : statements) {
      // Expression-position verbs hoist: `x := adopt G()` and
      // `let x := ask ...` mint the verb's own host step plus a
      // `result` field naming the bound var — the value lands on the
      // agent's vars record at run time, durable like any other
      // agent state.
      const Expr *verbStmt = hostVerbOf(*stmt);
      std::string bound;
      if (verbStmt != stmt) {
        bound = stmt->kind == ExprKind::Binary ? stmt->children[0]->text
                                               : stmt->text;
      }
      if (verbStmt) {
        flushRun();
        auto step = mintHostStep(*verbStmt, decl, params, clauseName);
        if (step && !bound.empty())
          if (const auto rec = varsRecord(arena, core, *step, "result", true))
            zigzag::expectWritten(arena.link(
                *rec, dims().values, DimVector::POS, arena.makeCell(bound)));
        if (step) {
          linkMember(*step);
          continue;
        }
        // A host verb whose args don't marshal (field-carrying emits,
        // variable-bearing terms) degrades to a stub segment — M0's
        // refused-body semantics at member granularity: the spine and
        // its named head land, only the graph is empty.
        Expr stub;
        stub.kind = ExprKind::Unit;
        stub.loc  = stmt->loc;
        auto head =
            emitDeclHead(decl, stub, params,
                         std::format("{}:seg{}", label(clauseName), member),
                         Type{"()"}, false);
        if (head) {
          linkMember(*head);
        } else {
          errors.push_back(step.error());
        }
      } else {
        run.push_back(stmt);
      }
    }
    flushRun();
  }

  std::vector<Parameter> paramsOf(const Decl &decl, std::string_view host) {
    if (const auto it = signatures.find(std::string{host});
        it != signatures.end())
      return it->second;
    // planBindings carry names only — untyped Parameter entries keep
    // the arity right until M2's statement lowering needs more.
    std::vector<Parameter> params;
    if (decl.planBindings)
      for (const auto &name : *decl.planBindings)
        params.push_back(Parameter{.name = name});
    return params;
  }

  std::string label(std::string_view name) const {
    return std::format("decl:{}:{}", program.module.value_or(""), name);
  }

  // +d.holder marks a symbol's owning agent — but as *content*, not a
  // live edge: one link per cell per dimension is the pairing the
  // arena maintains, so N symbols pointing +d.holder at one agent
  // would evict each other off the agent's single -d.holder slot. The
  // edge lands on a fresh scalar cell whose int64 is the agent ref;
  // dispatch reads linked(sym, d.holder) → asInt64 → CellRef, which
  // keeps ownership symbol-side and single-edge-safe.
  void mintHolder(CellRef sym) {
    if (arena.linked(sym, holderDim(), Posward)) return;
    zigzag::expectWritten(arena.link(
        sym, holderDim(), DimVector::POS,
        arena.makeScalarCell(static_cast<std::int64_t>(image.agentCell))));
  }
  DimRef holderDim() {
    if (holderDim_ == zigzag::noCell)
      holderDim_ = core.findOrMintDimension("d.holder");
    return holderDim_;
  }
  DimRef holderDim_{zigzag::noCell};

  void lowerDecl(const Decl &decl, bool insideAgent);
  void lowerClauseBody(const Decl &decl, CellRef host,
                       const std::vector<Parameter> &params,
                       std::string_view clauseName);
  void lowerFieldSlot(CellRef descriptor, const Decl &member,
                      const std::vector<Parameter> &ownerParams,
                      std::string_view ownerName);
  CellRef clauseTableChild(CellRef descriptor, std::string_view table,
                           std::string_view key);
};

// One clause = a guard head (+d.grab) plus a body head (+d.spin). A
// decl with no guard lowers `where true` so the clause shape is
// uniform for clauseFormals/graft checks.
void Lowerer::lowerClauseBody(const Decl &decl, CellRef host,
                              const std::vector<Parameter> &params,
                              std::string_view clauseName) {
  Expr trueGuard;
  trueGuard.kind        = ExprKind::Boolean;
  trueGuard.text        = "true";
  trueGuard.loc         = decl.loc;
  const Expr &guardExpr = decl.guard ? *decl.guard : trueGuard;

  // A Boolean test position must never take emitDeclHead's stub —
  // an unlowerable `where` is a refusal, not a vacuous `true` the
  // selector would silently pass.
  const auto test = emitHead(decl, guardExpr, params,
                             std::format("{}:guard", label(clauseName)),
                             Type{"Bool"});
  if (!test) {
    errors.push_back(test.error());
    return;
  }
  const auto body = emitDeclHead(
      decl, decl.body ? *decl.body : trueGuard, params,
      std::format("{}:body", label(clauseName)), Type{"()"}, false);
  if (!body) {
    errors.push_back(body.error());
    return;
  }
  const auto clause = mintClause(host, clauseName, *test, *body, decl.priority);
  // A body with statement-position verbs gets the segment spine
  // alongside its spin head — the executor prefers +d.step when the
  // rank is non-empty (§3.8).
  if (decl.body && bodyHasHostVerb(*decl.body))
    emitSpine(decl, clause, params, clauseName);
}

// A skill/behavior `var` member: a name cell on the descriptor's
// "fields" term, +d.values → an init head taking the owner's
// parameters — install/mount runs each against the call's arguments
// to seed the frame's or instance's vars records.
void Lowerer::lowerFieldSlot(
    CellRef descriptor, const Decl &member,
    const std::vector<Parameter> &ownerParams,
    std::string_view ownerName) {
  CellRef list = noCell;
  if (const auto r =
          varsRecord(arena, core, descriptor, "fields", true)) {
    if (const auto v = arena.linked(*r, dims().values, DimVector::POS))
      list = *v;
    else {
      list = arena.makeCell("fields");
      zigzag::expectWritten(
          arena.link(*r, dims().values, DimVector::POS, list));
    }
  }
  const auto slot = arena.makeCell(member.name);
  zigzag::expectWritten(arena.link(zigzag::rankTail(arena, list, dims().step),
                                 dims().step, DimVector::POS, slot));
  if (member.initializer) {
    auto head =
        emitDeclHead(member, *member.initializer, ownerParams,
                     std::format("{}:field:{}", label(ownerName), member.name),
                     member.result.value_or(Type{"<any>"}), false);
    if (!head) {
      errors.push_back(head.error());
      return;
    }
    zigzag::expectWritten(
        arena.link(slot, dims().values, DimVector::POS, *head));
  }
}

// A behavior's per-event (handlers) or per-goal (plans) clause table:
// the descriptor's `table` vars record holds a term whose +d.step
// children are keyed cells, each heading the +d.clause rank dispatch
// merges for that trigger.
CellRef Lowerer::clauseTableChild(CellRef descriptor,
                                  std::string_view table,
                                  std::string_view key) {
  CellRef term = noCell;
  if (const auto r = varsRecord(arena, core, descriptor, table, true)) {
    if (const auto v = arena.linked(*r, dims().values, DimVector::POS))
      term = *v;
    else {
      term = arena.makeCell(table);
      zigzag::expectWritten(
          arena.link(*r, dims().values, DimVector::POS, term));
    }
  }
  for (const CellRef child :
       zigzag::rankAfter(arena, term, dims().step))
    if (arena.textOf(child) == key) return child;
  const auto child = arena.makeCell(key);
  zigzag::expectWritten(arena.link(zigzag::rankTail(arena, term, dims().step),
                                 dims().step, DimVector::POS, child));
  return child;
}

void Lowerer::lowerDecl(const Decl &decl, bool insideAgent) {
  const auto name = decl.name;
  switch (declDisposition(decl.kind)) {
  case DeclDisposition::Refuse:
    refuse(decl,
           std::format("declaration kind cannot be lowered in M0: '{}'", name));
    return;
  case DeclDisposition::Stub:
    image.stubbed.push_back(name);
    // Named decls mint their symbol — the module vocabulary is
    // complete even where the semantics are still deferred; the
    // Lifecycle member decl has no symbol shape to claim.
    switch (decl.kind) {
    case DeclKind::Behavior:
      if (!symbols.contains(name)) mintSymbol(name);
      signatures.emplace(name, decl.parameters);
      break;
    default:
      break;
    }
    for (const auto &member : decl.members) lowerDecl(member, insideAgent);
    return;
  case DeclDisposition::Lower:
    break;
  }

  switch (decl.kind) {
  case DeclKind::Agent: {
    if (image.agentCell != noCell) {
      refuse(decl, "a program lowers at most one agent declaration");
      return;
    }
    image.agentCell = arena.makeCell();
    zigzag::expectWritten(arena.link(image.agentCell, dims().name,
                                     DimVector::POS, arena.makeCell(name)));
    // The purpose-minted directory cell: agents are reachable via
    // cells"##/+d.dirs/+d.agents" without hanging the bidirectionally
    // promoted agent off home (review r1's dead-cursor accretion).
    image.directory   = arena.makeCell();
    const auto dirs   = core.findOrMintDimension("d.dirs");
    const auto agents = core.findOrMintDimension("d.agents");
    // Mint the holder dim up front so it files even for an
    // agent whose members declare no owned symbols.
    (void)holderDim();
    zigzag::expectWritten(arena.link(zigzag::rankTail(arena, core.home(), dirs),
                                     dirs, DimVector::POS, image.directory));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, image.directory, agents), agents,
                   DimVector::POS, image.agentCell));
    // $-reserved seeds — the names the turn runtime binds
    // ($turn/$step/$stats) are claimed in the image so user minting
    // collides at reservedName instead of at runtime.
    for (const auto seed : {"$turn", "$step", "$stats"}) {
      if (const auto rec = varsRecord(arena, core, image.agentCell, seed, true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS,
                       arena.makeScalarCell(std::int64_t{0})));
    }
    // Var records mint before any body emits — a member fragment
    // binds `var` names through the bindings table at compile time,
    // whatever order the declarations list them in.
    for (const auto &member : decl.members)
      if (member.kind == DeclKind::Field) lowerDecl(member, true);
    for (const auto &member : decl.members)
      if (member.kind != DeclKind::Field) lowerDecl(member, true);
    // §8.5's creation order materializes as a spine: the `install`/
    // `behavior` members' host steps sit on a $create clause the
    // executor runs inside the $initialized turn, before `on
    // Initialize` dispatches.
    if (!creationSteps_.empty()) {
      const auto clause = arena.makeCell("$create");
      for (const CellRef step : creationSteps_)
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, clause, dims().step),
                       dims().step, DimVector::POS, step));
      if (const auto rec =
              varsRecord(arena, core, image.agentCell, "$create", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS, clause));
      creationSteps_.clear();
    }
    return;
  }
  case DeclKind::Record: {
    // A record is a field schema, not a body — the symbol and the
    // signature are its whole image work (the checker types literals
    // against it; marshalTermExpr lays the values out). Nothing is
    // deferred left to stub.
    if (symbols.contains(name)) {
      refuse(decl, std::format("duplicate symbol '{}'", name));
      return;
    }
    (void)mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    return;
  }
  case DeclKind::Belief: {
    // `believe p(args)` as an agent member is a creation-order fact —
    // the spec asserts it before `on Initialize`, so it mints into the
    // bank while the image assembles. meta.believe hangs the clause
    // and its {peer, turn} source exactly as the runtime verb does;
    // peer=self and turn 0 mark "asserted at creation".
    if (image.agentCell == noCell) {
      refuse(decl, "initial beliefs lower inside an agent");
      return;
    }
    auto term = marshalTerm(*decl.initializer);
    if (!term) {
      refuse(decl, term.error().detail);
      return;
    }
    MetaRuntime meta(core, vm, MetaBindSet{.agentCell = image.agentCell});
    if (auto clause = meta.believe(name, *term, image.agentCell, 0);
        !clause)
      refuse(decl, clause.error());
    return;
  }
  case DeclKind::Event:
  case DeclKind::Goal:
  case DeclKind::Predicate: {
    if (symbols.contains(name)) {
      refuse(decl, std::format("duplicate symbol '{}'", name));
      return;
    }
    const auto sym = mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    if (insideAgent && decl.kind != DeclKind::Predicate) mintHolder(sym);
    // `await ready(x)` on a predicate is the belief form — it tests
    // the bank rather than the journal. The grammar can't tell it
    // from an event term, so the marker rides the symbol.
    if (decl.kind == DeclKind::Predicate)
      if (const auto rec = varsRecord(arena, core, sym, "symKind", true))
        zigzag::expectWritten(arena.link(*rec, dims().values, DimVector::POS,
                                         arena.makeCell("predicate")));
    // The goal's clauses lower to named Bool heads keyed off its vars
    // record — dropTest is §3.5's reconsideration boundary, succeedTest
    // the achieve-goal completion check, holdsTest the maintain
    // restorer's condition. goalKind rides alongside so the retire
    // path knows which check applies.
    if (decl.kind == DeclKind::Goal) {
      const auto kindCode = decl.goalKind == GoalKind::Achieve ? std::int64_t{1}
                            : decl.goalKind == GoalKind::Maintain
                                ? std::int64_t{2}
                                : std::int64_t{0};
      if (const auto rec = varsRecord(arena, core, sym, "goalKind", true))
        zigzag::expectWritten(arena.link(*rec, dims().values, DimVector::POS,
                                         arena.makeScalarCell(kindCode)));
      for (const auto &gc : decl.goalClauses) {
        if (!gc.condition) continue;
        const char *field = nullptr;
        if (gc.kind == GoalClause::Kind::Drop)
          field = "dropTest";
        else if (gc.kind == GoalClause::Kind::Succeed)
          field = "succeedTest";
        else if (gc.kind == GoalClause::Kind::Holds)
          field = "holdsTest";
        if (!field) continue;
        // `holds believes p(...) [from self]` is a bank query, not a
        // scalar condition — emitHead cannot lower it and the stub
        // fallback would mint a vacuous `true`. Top-level `&`
        // conjuncts split: belief conjuncts marshal as unification
        // patterns on a holdsBelief container's +d.grab, the scalar
        // remainder lowers as holdsTest, and the monitor rung ANDs
        // the two under one substitution env. A `?name` the patterns
        // capture becomes a holdsTest parameter, so the remainder
        // reads the fact's bound value; a remainder name nothing
        // captures refuses through the scalar emitter's ordinary
        // path. A non-self scope names a remote bank the turn cannot
        // reach — refused, not silently held.
        const Expr *condition = gc.condition.get();
        if (gc.kind == GoalClause::Kind::Holds) {
          std::vector<const Expr *> beliefs;
          std::vector<const Expr *> scalars;
          const std::function<void(const Expr &)> peel =
              [&](const Expr &e) {
                if (e.kind == ExprKind::Believes) {
                  beliefs.push_back(&e);
                  return;
                }
                if (e.kind == ExprKind::Binary && e.text == "&") {
                  for (const auto &c : e.children)
                    if (c) peel(*c);
                  return;
                }
                scalars.push_back(&e);
              };
          peel(*condition);
          if (!beliefs.empty()) {
            const auto container = arena.makeCell("holdsBeliefs");
            for (const Expr *bexpr : beliefs) {
              const auto &query = *bexpr;
              if (query.scope) {
                const bool local =
                    query.scope->kind == ExprKind::Identifier &&
                    (query.scope->text == "self" ||
                     query.scope->text == "0");
                if (!local) {
                  errors.push_back(Diagnostic{
                      DiagnosticCode::InvalidDeclaration,
                      "holds believes ... from <peer> queries a remote "
                      "bank the turn cannot evaluate",
                      query.scope->loc});
                  return;
                }
              }
              if (query.children.empty()) {
                errors.push_back(
                    Diagnostic{DiagnosticCode::InvalidDeclaration,
                               "holds believes carries no term", query.loc});
                return;
              }
              auto pattern = marshalPattern(*query.children.front());
              if (!pattern) {
                errors.push_back(pattern.error());
                return;
              }
              // +d.grab is the membership dim — the patterns'
              // +d.step args share its chain, so +d.step itself
              // cannot separate sibling patterns.
              zigzag::expectWritten(arena.link(
                  zigzag::rankTail(arena, container, dims().grab),
                  dims().grab, DimVector::POS, *pattern));
            }
            if (const auto rec =
                    varsRecord(arena, core, sym, "holdsBelief", true))
              zigzag::expectWritten(arena.link(*rec, dims().values,
                                               DimVector::POS, container));
          }
          if (scalars.empty()) continue;
          // A leftover scalar half is an ordinary holdsTest — the
          // peeled conjuncts re-join under `&` so emitHead sees one
          // expression. Names a `?var` captures ride as extra params
          // after the goal's own: the monitor rung's unification env
          // fills them positionally, so the scalar half sees the
          // fact's bound value, not just the belief's existence.
          auto hparams = paramsOf(decl, name);
          const auto declared = [&](std::string_view want) {
            for (const auto &p : hparams)
              if (p.name == want) return true;
            return false;
          };
          // A `?name` argument's fragment type is the predicate's
          // declared type at that position — the same `expected` the
          // checker's term() binds the name at — looked up per
          // argument index as the pattern tree walks. A capture whose
          // type cannot be derived refuses the clause rather than
          // minting an untyped param the scalar lowerer rejects.
          const std::function<bool(const Expr &)> collect =
              [&](const Expr &term) -> bool {
            const auto sig = signatures.find(term.text);
            for (std::size_t i = 0; i < term.children.size(); ++i) {
              const auto &arg = *term.children[i];
              if (arg.kind == ExprKind::LogicVariable) {
                if (arg.text == "_") continue;
                if (declared(arg.text)) continue;
                Type type{};
                if (sig != signatures.end() &&
                    i < sig->second.size())
                  type = sig->second[i].type;
                if (type.name.empty()) {
                  errors.push_back(Diagnostic{
                      DiagnosticCode::InvalidDeclaration,
                      std::format(
                          "captured name '{}' has no declared predicate "
                          "argument type at {} position {}",
                          arg.text, term.text, i),
                      arg.loc});
                  return false;
                }
                hparams.push_back(Parameter{.name = std::string{arg.text},
                                            .type = std::move(type),
                                            .loc  = arg.loc});
                continue;
              }
              if ((arg.kind == ExprKind::Term ||
                   arg.kind == ExprKind::Record) &&
                  !collect(arg))
                return false;
            }
            return true;
          };
          for (const Expr *bexpr : beliefs)
            for (const auto &c : bexpr->children)
              if (c && (c->kind == ExprKind::Term ||
                        c->kind == ExprKind::Record) &&
                  !collect(*c))
                return;
          const Expr *remainder = scalars.front();
          Expr rebuilt;
          if (scalars.size() > 1) {
            rebuilt.kind = ExprKind::Binary;
            rebuilt.text = "&";
            rebuilt.loc  = condition->loc;
            for (const Expr *s : scalars)
              rebuilt.children.push_back(cloneExpr(*s));
            remainder = &rebuilt;
          }
          auto head = emitHead(decl, *remainder, hparams,
                               std::format("{}:holdsTest", label(name)),
                               Type{"Bool"});
          if (!head) {
            errors.push_back(head.error());
            return;
          }
          if (const auto rec = varsRecord(arena, core, sym, "holdsTest", true))
            zigzag::expectWritten(arena.link(*rec, dims().values,
                                             DimVector::POS, *head));
          continue;
        }
        // drop/succeed conditions are pure scalar tests — emitHead
        // direct so an unlowerable condition refuses instead of
        // stubbing `true`.
        auto head = emitHead(decl, *condition, paramsOf(decl, name),
                             std::format("{}:{}", label(name), field),
                             Type{"Bool"});
        if (!head) {
          errors.push_back(head.error());
          return;
        }
        if (const auto rec = varsRecord(arena, core, sym, field, true))
          zigzag::expectWritten(
              arena.link(*rec, dims().values, DimVector::POS, *head));
      }
    }
    for (const auto &member : decl.members) lowerDecl(member, insideAgent);
    return;
  }
  case DeclKind::Method:
  case DeclKind::Answer: {
    if (!decl.body) {
      refuse(decl, std::format("'{}' declares no body", name));
      return;
    }
    // A method inside `skill S` or `behavior B` claims the qualified
    // `S.m`/`B.m` symbol — §5.3's dispatch resolves skill methods by
    // that name; an answer (never a skill member) keeps its flat name.
    const std::string symName =
        decl.kind == DeclKind::Method && !memberPrefix_.empty()
            ? memberPrefix_ + "." + name
            : std::string{name};
    // An answer legitimately shares its symbol — the matching
    // `question` decl minted it — while a `def` that lands on an
    // existing name is the collision the old check caught.
    if (decl.kind == DeclKind::Method && symbols.contains(symName)) {
      refuse(decl, std::format("duplicate symbol '{}'", symName));
      return;
    }
    const auto head =
        emitDeclHead(decl, *decl.body, decl.parameters, label(symName),
                     decl.result.value_or(Type{"()"}), false);
    if (!head) {
      errors.push_back(head.error());
      return;
    }
    const auto sym =
        symbols.contains(symName) ? symbols[symName] : mintSymbol(symName);
    zigzag::expectWritten(
        arena.link(sym, dims().values, DimVector::POS, *head));
    return;
  }
  case DeclKind::Question: {
    // A question's whole image is the askable symbol plus the
    // signature `ask` checks against — the runtime never runs it.
    if (!symbols.contains(name)) mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    return;
  }
  case DeclKind::Capacity: {
    // Signatures only — §5.3's instance capacity-name matching reads
    // the name off the skill's descriptor, not the capacity's image,
    // and its bodiless method signatures are the checker's surface.
    if (!symbols.contains(name)) mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    // `effect` methods carry the wait's deadline on the signature —
    // the call site lowers to `effectcall` and the park rides these
    // turns, so they are recorded here keyed "Cap.method".
    for (const auto &member : decl.members)
      if (member.kind == DeclKind::Method && member.effect) {
        EffectSig sig{.preempts = member.preempts};
        if (member.deadline &&
            member.deadline->kind == ExprKind::Duration)
          sig.turns = std::stoll(member.deadline->text);
        effectSigs_.emplace(name + "." + member.name, sig);
      }
    return;
  }
  case DeclKind::Uses: {
    // §5.3: the +d.uses rank records one cell per imported capacity —
    // on the agent for its own `uses`, on the descriptor while a
    // skill/behavior lowers. The checker did the resolution; the rank
    // is durable evidence of what the owner claimed to import.
    const auto owner = usesTarget_ != noCell ? usesTarget_ : image.agentCell;
    if (owner == noCell) {
      refuse(decl, "'uses' lowers inside an agent only");
      return;
    }
    for (const auto &used : decl.names)
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, owner, usesDim()), usesDim(),
                     DimVector::POS, arena.makeCell(used)));
    return;
  }
  case DeclKind::Install:
  case DeclKind::BehaviorUse: {
    // §8.5's creation order: `install S(args)`/`behavior B(args)`
    // members marshal to the same host steps the action forms do and
    // collect on the agent's creation spine — the executor runs it
    // on the first turn, before `on Initialize`. `believe` members
    // still land at image time, so a `believe` declared after one of
    // these runs marginally earlier than strict declaration order.
    if (image.agentCell == noCell || !decl.initializer) {
      refuse(decl, "creation members lower inside an agent");
      return;
    }
    const auto payload = marshalCallPayload(*decl.initializer);
    if (!payload) {
      errors.push_back(payload.error());
      return;
    }
    const auto step = arena.makeCell();
    zigzag::expectWritten(
        arena.link(step, verbDim(), DimVector::POS,
                   arena.makeCell(decl.kind == DeclKind::Install ? "install"
                                                                : "mount")));
    if (const auto rec = varsRecord(arena, core, step, "payload", true))
      zigzag::expectWritten(
          arena.link(*rec, dims().values, DimVector::POS, *payload));
    creationSteps_.push_back(step);
    return;
  }
  case DeclKind::Skill: {
    if (symbols.contains(name)) {
      refuse(decl, std::format("duplicate symbol '{}'", name));
      return;
    }
    const auto sym = mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    // The descriptor `install` resolves at run time, riding the
    // symbol's +d.values like a method head does: install params for
    // positional binding, the closed capacity set (§5.3 counts
    // capacities those extend as instances too) on +d.capacity, field
    // initializers as heads taking the skill's own parameters, the
    // native marker, and the on install/on uninstall bodies.
    const auto rec = arena.makeCell(name);
    zigzag::expectWritten(arena.link(sym, dims().values, DimVector::POS, rec));
    if (!decl.parameters.empty()) {
      const auto list = arena.makeCell("params");
      for (const auto &p : decl.parameters)
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, list, dims().step), dims().step,
                       DimVector::POS, arena.makeCell(p.name)));
      if (const auto r = varsRecord(arena, core, rec, "params", true))
        zigzag::expectWritten(
            arena.link(*r, dims().values, DimVector::POS, list));
    }
    std::set<std::string> caps(decl.names.begin(), decl.names.end());
    std::vector<std::string> work(decl.names.begin(), decl.names.end());
    while (!work.empty()) {
      const auto next = work.back();
      work.pop_back();
      for (const auto &d : program.declarations)
        if (d.kind == DeclKind::Capacity && d.name == next)
          for (const auto &base : d.names)
            if (caps.insert(base).second) work.push_back(base);
    }
    for (const auto &cap : caps)
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, rec, capacityDim()), capacityDim(),
                     DimVector::POS, arena.makeCell(cap)));
    if (decl.native)
      if (const auto r = varsRecord(arena, core, rec, "native", true))
        zigzag::expectWritten(
            arena.link(*r, dims().values, DimVector::POS,
                       arena.makeScalarCell(std::int64_t{1})));
    const std::string prevPrefix = memberPrefix_;
    const CellRef prevUses       = usesTarget_;
    const OwnerFields prevFields = memberFields_;
    memberPrefix_                = name;
    usesTarget_                  = rec;
    memberFields_                = memberFieldsOf(decl);
    for (const auto &member : decl.members) {
      if (member.kind == DeclKind::Field) {
        lowerFieldSlot(rec, member, decl.parameters, name);
        continue;
      }
      if (member.kind == DeclKind::Lifecycle) {
        if (!member.body) continue;
        // Same clause shape a handler carries — +d.grab/+d.spin heads
        // plus the +d.step spine when the body holds host verbs — so
        // install/uninstall replay it through the executor's ordinary
        // clause dispatch.
        Expr trueGuard;
        trueGuard.kind  = ExprKind::Boolean;
        trueGuard.text  = "true";
        trueGuard.loc   = member.loc;
        const auto test = emitHead(
            member, trueGuard, decl.parameters,
            std::format("{}:{}:guard", label(name), member.name), Type{"Bool"});
        if (!test) {
          errors.push_back(test.error());
          continue;
        }
        const auto body =
            emitDeclHead(member, *member.body, decl.parameters,
                         std::format("{}:{}:body", label(name), member.name),
                         Type{"()"}, false);
        if (!body) {
          errors.push_back(body.error());
          continue;
        }
        const auto clause = arena.makeCell();
        zigzag::expectWritten(arena.link(clause, dims().name, DimVector::POS,
                                         arena.makeCell(member.name)));
        zigzag::expectWritten(
            arena.link(clause, dims().grab, DimVector::POS, *test));
        zigzag::expectWritten(
            arena.link(clause, dims().spin, DimVector::POS, *body));
        if (bodyHasHostVerb(*member.body))
          emitSpine(member, clause, decl.parameters, member.name);
        if (const auto r = varsRecord(arena, core, rec, member.name, true))
          zigzag::expectWritten(
              arena.link(*r, dims().values, DimVector::POS, clause));
        continue;
      }
      lowerDecl(member, insideAgent);
    }
    memberPrefix_ = prevPrefix;
    usesTarget_   = prevUses;
    memberFields_ = prevFields;
    return;
  }
  case DeclKind::Behavior: {
    // §5.7: a behavior's program contributes its own ranks — its
    // handler clauses sit on a per-event table off the descriptor,
    // its plan clauses on a per-goal one, and dispatch merges them in
    // mount order rather than filing onto the shared symbols. The
    // descriptor mirrors the skill record's shape: params, fields as
    // init heads, qualified `B.m` methods.
    if (symbols.contains(name)) {
      refuse(decl, std::format("duplicate symbol '{}'", name));
      return;
    }
    const auto sym = mintSymbol(name);
    signatures.emplace(name, decl.parameters);
    const auto rec = arena.makeCell(name);
    zigzag::expectWritten(
        arena.link(sym, dims().values, DimVector::POS, rec));
    if (!decl.parameters.empty()) {
      const auto list = arena.makeCell("params");
      for (const auto &p : decl.parameters)
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, list, dims().step),
                       dims().step, DimVector::POS, arena.makeCell(p.name)));
      if (const auto r = varsRecord(arena, core, rec, "params", true))
        zigzag::expectWritten(
            arena.link(*r, dims().values, DimVector::POS, list));
    }
    const std::string prevPrefix = memberPrefix_;
    const CellRef prevDesc       = behaviorDesc_;
    const CellRef prevUses       = usesTarget_;
    const OwnerFields prevFields = memberFields_;
    memberPrefix_                = name;
    behaviorDesc_                = rec;
    usesTarget_                  = rec;
    memberFields_                = memberFieldsOf(decl);
    for (const auto &member : decl.members) {
      if (member.kind == DeclKind::Field) {
        lowerFieldSlot(rec, member, decl.parameters, name);
        continue;
      }
      if (member.kind == DeclKind::Goal) {
        // Behavior-owned goal semantics — where the goal cell lives
        // and whose bank selects it — is outside M0's merge.
        image.stubbed.push_back(
            std::format("{}:{}", name, member.name));
        continue;
      }
      lowerDecl(member, insideAgent);
    }
    memberPrefix_ = prevPrefix;
    behaviorDesc_ = prevDesc;
    usesTarget_   = prevUses;
    memberFields_ = prevFields;
    return;
  }
  case DeclKind::Plan: {
    if (!decl.target) {
      refuse(decl, std::format("plan '{}' names no goal", name));
      return;
    }
    if (!decl.body) {
      refuse(decl, std::format("plan '{}' declares no body", name));
      return;
    }
    // A plan for an undeclared goal declares the goal implicitly —
    // clauseHost needs a symbol to hang the clause on either way.
    auto host = symbols.contains(*decl.target) ? symbols[*decl.target]
                                               : mintSymbol(*decl.target);
    // A behavior's plan files on its descriptor's per-goal table —
    // §5.7's mount-order merge — not the shared goal rank.
    if (behaviorDesc_ != noCell)
      host = clauseTableChild(behaviorDesc_, "plans", *decl.target);
    else if (insideAgent)
      mintHolder(host);
    lowerClauseBody(decl, host, paramsOf(decl, *decl.target), name);
    return;
  }
  case DeclKind::Handler: {
    if (decl.trigger && decl.trigger->kind != ExprKind::Identifier) {
      // +/-belief and term triggers have no symbol host at M0.
      image.stubbed.push_back(name);
      return;
    }
    if (!decl.body) {
      refuse(decl, std::format("handler 'on {}' declares no body", name));
      return;
    }
    // 'on Tick' declares Tick when no event decl did.
    auto host = symbols.contains(name) ? symbols[name] : mintSymbol(name);
    // The behavior's handler files on its descriptor's per-event
    // table — dispatch merges instance ranks at run time.
    if (behaviorDesc_ != noCell)
      host = clauseTableChild(behaviorDesc_, "handlers", name);
    else if (insideAgent)
      mintHolder(host);
    const auto n = ++clauseCounts[host];
    const auto clause =
        n == 1 ? std::format("on:{}", name) : std::format("on:{}#{}", name, n);
    auto params = paramsOf(decl, name);
    // The event term itself binds for field access: the checker scopes
    // `occurrence` (always) and the `as` alias under the event's type —
    // at descriptor level both are Cell ref-boxes dispatch fills with
    // int64(payload-term). The Cell type-text is eventArgs's cue to
    // bind the term rather than a missing payload field. A declared
    // field already carrying the name wins — the field param binds it.
    const auto named = [&](std::string_view want) {
      for (const auto &p : params)
        if (p.name == want) return true;
      return false;
    };
    if (!named("occurrence"))
      params.push_back(
          Parameter{.name = "occurrence", .type = Type{"Cell"}});
    if (decl.binding && !named(*decl.binding))
      params.push_back(Parameter{.name = *decl.binding,
                                 .type = Type{"Cell"}});
    lowerClauseBody(decl, host, params, clause);
    return;
  }
  case DeclKind::Field: {
    if (!insideAgent) {
      // Module-scope vars are a different bank — nothing mints for a
      // member-less field outside the agent.
      image.stubbed.push_back(name);
      return;
    }
    // The agent's state slot: a vars record whose +d.values child is
    // the value-holder the bindings table names state-side — a member
    // fragment's Value op reads and writes the holder's content at
    // run time, so `x :=` lands in the durable record, never a
    // compile-time snapshot. A non-literal initializer can't mint a
    // value; the holder stands empty (reads fault honestly) and the
    // name files under stubbed so the image's degraded surface stays
    // truthful.
    const auto rec = varsRecord(arena, core, image.agentCell, name, true);
    if (!rec) {
      refuse(decl, std::format("var '{}' record could not mint", name));
      return;
    }
    const auto holder = arena.makeCell();
    zigzag::expectWritten(
        arena.link(*rec, dims().values, DimVector::POS, holder));
    // The "var.slot" name marks this as a declared slot rather than a
    // repointable value cell: the `x := verb` hoists write through a
    // marked holder instead of swapping the record's +d.values edge —
    // a repoint would orphan the cell the fragment bindings resolved.
    zigzag::expectWritten(arena.link(holder, dims().name, DimVector::POS,
                                     arena.makeCell("var.slot")));
    if (decl.initializer) {
      const auto &init = *decl.initializer;
      switch (init.kind) {
      case ExprKind::Integer:
        core.writeValue(holder, 0, std::nullopt,
                        CellValue{std::stoll(init.text)});
        break;
      case ExprKind::Float:
        core.writeValue(holder, 0, std::nullopt,
                        CellValue{std::stod(init.text)});
        break;
      case ExprKind::Boolean:
        core.writeValue(holder, 0, std::nullopt,
                        CellValue{init.text == "true"});
        break;
      case ExprKind::String:
        core.writeValue(holder, 0, std::nullopt, CellValue{init.text});
        break;
      case ExprKind::None:
        break;
      default:
        image.stubbed.push_back(name);
        break;
      }
    }
    bindings.emplace(
        name,
        CompilerBinding{holder, decl.result.value_or(Type{"<any>"}),
                        /*stateSide=*/true});
    return;
  }
  default:
    refuse(decl, std::format("declaration kind unlowered: '{}'", name));
    return;
  }
}

} // namespace

std::expected<LoweredImage, std::vector<Diagnostic>>
lowerProgram(const Program &program, zigzag::vortex::VortexCore &core,
             zigzag::vortex::VortexVM &vm, ScalarCompilerLimits limits) {
  Lowerer lowerer{core, vm, std::move(limits), program};
  lowerer.image.programRoot = core.home();
  lowerer.image.ownModule = core.getOrCreateModule(program.module.value_or(""));
  for (const auto &decl : program.declarations) lowerer.lowerDecl(decl, false);
  if (!lowerer.errors.empty())
    return std::unexpected(std::move(lowerer.errors));
  if (const auto image = finalizeProgramImage(lowerer.compiler))
    lowerer.image.imageRecord = *image;
  lowerer.image.imageExtent = static_cast<CellRef>(core.arena().cellCount());
  return std::move(lowerer.image);
}

} // namespace xanadu::vagent
