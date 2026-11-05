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

  const auto test = emitDeclHead(decl, guardExpr, params,
                                 std::format("{}:guard", label(clauseName)),
                                 Type{"Bool"}, true);
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
  mintClause(host, clauseName, *test, *body, decl.priority);
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
