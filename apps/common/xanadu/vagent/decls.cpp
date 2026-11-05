#include "common/xanadu/vagent/decls.hpp"

#include <format>
#include <map>
#include <string_view>
#include <utility>

#include "common/xanadu/vagent/ast_clone.hpp"
#include "common/xanadu/vagent/fragment.hpp"
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
  case DeclKind::Record:
  case DeclKind::Question:
  case DeclKind::Skill:
  case DeclKind::Capacity:
  case DeclKind::Behavior:
  case DeclKind::Belief:
  case DeclKind::Lifecycle:
  case DeclKind::Field:
    return DeclDisposition::Stub;
  case DeclKind::Uses:
  case DeclKind::Install:
  case DeclKind::BehaviorUse:
    return DeclDisposition::Refuse;
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
    const FragmentKey key{decl.loc, ordinal++};
    fragments.emplace(key,
                      FragmentRecord{.body = CodeBody{std::string{label},
                                                      params, cloneExpr(body)},
                                     .closed = true});
    infos.emplace(key, FragmentInfo{.result = std::move(result),
                                    .pure   = false,
                                    .closed = true,
                                    .arity  = params.size()});
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
      return true;
    default:
      return false;
    }
  }

  // The host verb a statement lowers to — the statement itself, or
  // the verb hoisted out of `x := <verb>` / `let x := <verb>`. The
  // two callers (the spine gate and the emitter) must agree on this
  // or a bound verb lowers as dead scalar code.
  static const Expr *hostVerbOf(const Expr &stmt) {
    if (isHostVerb(stmt.kind)) return &stmt;
    if (stmt.kind == ExprKind::Binary && stmt.text == ":=" &&
        stmt.children.size() == 2 &&
        stmt.children[0]->kind == ExprKind::Identifier &&
        isHostVerb(stmt.children[1]->kind))
      return stmt.children[1].get();
    if (stmt.kind == ExprKind::Let && stmt.children.size() == 1 &&
        isHostVerb(stmt.children.front()->kind))
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
    switch (expr.kind) {
    case ExprKind::Term: {
      const auto cell = arena.makeCell(expr.text);
      for (const auto &child : expr.children) {
        auto arg = marshalTerm(*child);
        if (!arg) return arg;
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, cell, dims().step), dims().step,
                       DimVector::POS, *arg));
      }
      return cell;
    }
    case ExprKind::String:
    case ExprKind::Integer:
    case ExprKind::Float:
    case ExprKind::Boolean:
      // Literals marshal as text cells so the host-side term walk's
      // textOf equality covers them — scalar cells would need a
      // value-typed compare the V1 walker doesn't carry.
      return arena.makeCell(expr.text);
    default:
      return std::unexpected(Diagnostic{
          DiagnosticCode::InvalidPosition,
          std::format("host-step term argument does not lower: kind {}",
                      static_cast<int>(expr.kind)),
          expr.loc});
    }
  }

  // marshalTerm's pattern form: LogicVariable arguments marshal as
  // '?'-prefixed text cells — MetaRuntime::termUnifies reads them as
  // wildcards. Used for `believes`-shaped goal conditions, which the
  // scalar emitter cannot lower and emitDeclHead would otherwise stub
  // to a vacuous `true`.
  std::expected<CellRef, Diagnostic> marshalPattern(const Expr &expr) {
    if (expr.kind == ExprKind::LogicVariable) {
      const auto text = expr.text.starts_with('?')
                            ? expr.text
                            : std::format("?{}", expr.text);
      return arena.makeCell(text);
    }
    // marshalTerm recurses on itself, so a Term carrying a variable
    // anywhere below the root would still refuse — the pattern walk
    // re-does the Term case with itself as the child step.
    if (expr.kind == ExprKind::Term) {
      const auto cell = arena.makeCell(expr.text);
      for (const auto &child : expr.children) {
        auto arg = marshalPattern(*child);
        if (!arg) return arg;
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, cell, dims().step), dims().step,
                       DimVector::POS, *arg));
      }
      return cell;
    }
    return marshalTerm(expr);
  }

  // The host-step cell: +d.verb names the MetaRuntime entry point, the
  // +d.vars record marshals the call's arguments under their parameter
  // names — for the belief verbs that is exactly `term`. Peer and
  // assertion turn are implicit, supplied by the executor at run time.
  std::expected<CellRef, Diagnostic> mintHostStep(const Expr &expr) {
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
    default:
      return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                        "statement is not a host verb",
                                        expr.loc});
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
    // within bound as the turn-clock scalar. `else` recovery still
    // refuses — the fallback is an evaluated expression.
    if (expr.kind == ExprKind::Ask) {
      if (expr.recovery)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "ask's else arm does not lower yet",
                                          expr.loc});
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

    // achieve shares adopt's goal-name marshal — the delegation the
    // runtime resolves on the parent intention. The grammar's only
    // deadline form is the remote one — `by <agent> within <dur>` —
    // which stages an outbox record and parks the parent on the
    // bound; else still refuses.
    if (expr.kind == ExprKind::Achieve) {
      if (expr.recovery)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "achieve's else arm does not lower yet", expr.loc});
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
      field("goal",
            arena.makeCell(expr.children.front()->children.front()->text));
      if (expr.recipients) field("peer", arena.makeCell(expr.recipients->text));
      if (expr.deadline)
        field("deadline", arena.makeScalarCell(static_cast<std::int64_t>(
                              std::stoll(expr.deadline->text))));
      return step;
    }

    // await marshals its own fields before the generic term path —
    // AwaitEvent carries no children at all (name in `text`, binding
    // in `binding`), and `within`'s deadline must be a literal so the
    // runtime can hold blocked_until as a plain scalar.
    if (expr.kind == ExprKind::AwaitEvent || expr.kind == ExprKind::AwaitTerm) {
      if (expr.guard || expr.recovery)
        return std::unexpected(
            Diagnostic{DiagnosticCode::InvalidPosition,
                       "await's guard/else arms do not lower yet", expr.loc});
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
      // adopt's operand is a Call — children[0] is the goal's
      // Identifier; the goal *name* marshals, resolution happens at
      // run time against the agent's own module.
      const auto &call = *expr.children.front();
      if (call.children.empty() ||
          call.children.front()->kind != ExprKind::Identifier)
        return std::unexpected(Diagnostic{DiagnosticCode::InvalidPosition,
                                          "adopt's operand is not a goal name",
                                          expr.loc});
      if (const auto rec = varsRecord(arena, core, step, "goal", true))
        zigzag::expectWritten(
            arena.link(*rec, dims().values, DimVector::POS,
                       arena.makeCell(call.children.front()->text)));
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
      const auto payload = arena.makeCell(*eventName);
      if (record) {
        // Field-carrying payload: values marshal as +d.step children
        // (the term-cell convention), each labeled +d.name with its
        // record field name — textOf(payload) still reads the event
        // name for drain and await matching.
        for (std::size_t i = 0; i < record->children.size(); ++i) {
          auto value = marshalTerm(*record->children[i]);
          if (!value) return std::unexpected(value.error());
          if (i < record->names.size())
            zigzag::expectWritten(arena.link(*value, dims().name,
                                             DimVector::POS,
                                             arena.makeCell(record->names[i])));
          zigzag::expectWritten(
              arena.link(zigzag::rankTail(arena, payload, dims().step),
                         dims().step, DimVector::POS, *value));
        }
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
        auto step = mintHostStep(*verbStmt);
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
    // complete even where the semantics are still deferred; member
    // decls (Field/Belief/Lifecycle) have no symbol shape to claim.
    switch (decl.kind) {
    case DeclKind::Record:
    case DeclKind::Question:
    case DeclKind::Skill:
    case DeclKind::Capacity:
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
    for (const auto &member : decl.members) lowerDecl(member, true);
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
        // patterns on a holdsBelief container's +d.step, the scalar
        // remainder lowers as holdsTest, and the monitor rung ANDs
        // the two. Variables a pattern captures bind nothing — a
        // remainder that names one refuses through the scalar
        // emitter's ordinary path. A non-self scope names a remote
        // bank the turn cannot reach — refused, not silently held.
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
          // expression.
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
          auto head = emitHead(decl, *remainder, paramsOf(decl, name),
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
    if (symbols.contains(name)) {
      refuse(decl, std::format("duplicate symbol '{}'", name));
      return;
    }
    if (!decl.body) {
      refuse(decl, std::format("'{}' declares no body", name));
      return;
    }
    const auto head =
        emitDeclHead(decl, *decl.body, decl.parameters, label(name),
                     decl.result.value_or(Type{"()"}), false);
    if (!head) {
      errors.push_back(head.error());
      return;
    }
    const auto sym = mintSymbol(name);
    zigzag::expectWritten(
        arena.link(sym, dims().values, DimVector::POS, *head));
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
    const auto host = symbols.contains(*decl.target) ? symbols[*decl.target]
                                                     : mintSymbol(*decl.target);
    if (insideAgent) mintHolder(host);
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
    const auto host = symbols.contains(name) ? symbols[name] : mintSymbol(name);
    if (insideAgent) mintHolder(host);
    const auto n = ++clauseCounts[host];
    const auto clause =
        n == 1 ? std::format("on:{}", name) : std::format("on:{}#{}", name, n);
    lowerClauseBody(decl, host, paramsOf(decl, name), clause);
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
