#include "common/xanadu/vagent/expand.hpp"

#include "common/xanadu/vagent/ast_clone.hpp"
#include "common/xanadu/vagent/cell_path.hpp"
#include "common/xanadu/vagent/code_path.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xanadu::vagent {
namespace {

// The inner-offset reporting context: locs of the enclosing code"…"
// literals from the outermost in, each a position in its parent's text —
// the outermost in real source coordinates. A diagnostic raised inside
// the innermost literal's text wraps through innerDiagnostic once per
// level, innermost first, producing the src:m:l:c+o:c chain of §5.1. An
// empty chain is program-level reporting: the diagnostic keeps its own
// loc.
struct LiteralChain {
  std::vector<SourceLocation> literals;
};

// A binding identity for expand's own tables — ScopeStamp flattened to a
// comparable pair (ordinal-less stamps are scope-only and never key a
// binding here).
struct BindingId {
  std::uint64_t scope{};
  std::uint64_t ordinal{};
};
bool operator<(const BindingId &left, const BindingId &right) {
  return std::tie(left.scope, left.ordinal) <
         std::tie(right.scope, right.ordinal);
}
BindingId bindingOf(const ScopeStamp &stamp) {
  return {stamp.scope, stamp.ordinal.value_or(0)};
}

// A Code<…>-parameter actual at a template or splice expansion: the
// statically-known fragment (as the Code literal node that denotes it)
// an enclosing Code parameter stands for. Keyed by the parameter's
// binding identity so `splice p` and `splice cap$N` operands resolve
// through the same channel.
using Actuals = std::map<BindingId, const Expr *>;

// The §5.4 substitution context for one fragment-body instantiation:
// the declared parameters, each Code parameter's actual literal node,
// and each value parameter's synthesized `let p<k>` name and fresh
// binding stamp. `outer` chains the enclosing substitution so a nested
// literal's `splice` of an outer body's parameter resolves transitively.
struct Substitution {
  const std::vector<Parameter> *params{};
  // The expand-minted scope id parameter references carry — the record's
  // canonical param identity, shared by every clone of the body.
  std::uint64_t paramScope{~0ull};
  std::map<std::size_t, std::string> letNames;
  std::map<std::size_t, ScopeStamp> letStamps;
  std::map<std::size_t, const Expr *> actuals;
  std::shared_ptr<const Substitution> outer;
};

// The walking context for the merged tree, one body-processing walk, or
// one substitution instantiation. `params`/`paramScope` name the
// innermost literal body's parameter set; `subst` is present while a
// body is being instantiated at a site; `actuals` carries enclosing
// template Code-parameter actuals; `enclosingParams` is the enclosing
// literal bodies' parameter lists, innermost first — a name in one is an
// outer fragment parameter, resolvable only through the subst chain.
// `watermark` is the current body's scope watermark: binding stamps with
// scope above it are body-internal.
struct Site {
  const Decl *owner{};
  const std::vector<Parameter> *params{};
  std::uint64_t paramScope{~0ull};
  std::shared_ptr<const Substitution> subst;
  const Actuals *actuals{};
  std::vector<const std::vector<Parameter> *> enclosingParams;
  std::uint64_t watermark{~0ull};
  LiteralChain chain;
  std::size_t depth{};
  Expr *currentBlock{};
  bool insideBody{false};
};

// A call to a method whose template-ness was undecidable when walked —
// its own declaration had not finished expanding. The site snapshot is
// what judging the Code actuals needs when the call is expanded later.
struct DeferredCall {
  const Expr *call{};
  const Decl *owner{};
  std::shared_ptr<const Substitution> subst;
  Actuals actuals;
  std::vector<const std::vector<Parameter> *> enclosingParams;
  LiteralChain chain;
  std::size_t depth{};
  bool insideBody{false};
};

class Expander {
public:
  Expander(Program &program, LiteralSeeds &seeds, ExpandLimits limits)
      : program_(program), seeds_(seeds), limits_(limits) {}

  std::expected<Expansion, std::vector<Diagnostic>> run() {
    indexDecls(program_.declarations, nullptr);
    for (auto &decl : program_.declarations) walkDecl(decl, nullptr);
    processDeferred();
    sweepDeadLets();
    removeTemplates();
    residualScan();
    if (!diagnostics_.empty())
      return std::unexpected(std::move(diagnostics_));
    return Expansion{std::move(fragments_), std::move(metas_),
                     std::move(captures_), std::move(synthesized_)};
  }

private:
  void report(Diagnostic diagnostic, const LiteralChain &chain) {
    for (auto it = chain.literals.rbegin(); it != chain.literals.rend();
         ++it)
      diagnostic = innerDiagnostic(std::move(diagnostic), *it);
    diagnostics_.push_back(std::move(diagnostic));
  }

  // ---- declaration indexing --------------------------------------------------

  // Registers every declaration, its owner, its method members (template
  // call lookup) and the env scope id its bindings mint under, when the
  // id is recoverable from a directly env-walked expression's stamp.
  void indexDecls(std::vector<Decl> &decls, const Decl *owner) {
    for (auto &decl : decls) {
      owners_[&decl] = owner;
      allDecls_.push_back(&decl);
      for (auto &member : decl.members)
        if (member.kind == DeclKind::Method)
          methods_[&decl].emplace(member.name, &member);
      if (auto scope = envScopeOf(decl)) declScope_[*scope] = &decl;
      indexDecls(decl.members, &decl);
    }
  }

  // The env scope a declaration's bindings mint into — the stamp scope of
  // any expression resolve walked directly in the declaration env: body,
  // field initializer, guard, trigger or deadline (contracts and goal
  // clauses walk in env copies, so their nodes' own scopes differ while
  // their binding stamps still carry the env id).
  static std::optional<std::uint64_t> envScopeOf(const Decl &decl) {
    for (const ExprPtr *slot :
         {&decl.body, &decl.initializer, &decl.guard, &decl.trigger,
          &decl.deadline})
      if (*slot && (*slot)->stamp) return (*slot)->stamp->scope;
    return std::nullopt;
  }

  const Decl *methodOf(const Decl *owner, std::string_view name) const {
    const auto found = methods_.find(owner);
    if (found == methods_.end()) return nullptr;
    const auto method = found->second.find(name);
    return method == found->second.end() ? nullptr : method->second;
  }

  // The declaration an env scope belongs to — for scopes unrecoverable
  // from direct exprs (a def whose only splice sits in a contract walks
  // in an env copy), a stamped identifier bound to one of its parameters
  // supplies the id.
  const Decl *declOfScope(std::uint64_t scope, const Expr &bindingUse) {
    if (const auto found = declScope_.find(scope);
        found != declScope_.end())
      return found->second;
    if (!bindingUse.stamp || !bindingUse.stamp->ordinal) return nullptr;
    const auto ordinal = *bindingUse.stamp->ordinal;
    const auto name    = sourceName(bindingUse);
    for (const Decl *decl : allDecls_) {
      if (ordinal < decl->parameters.size() &&
          decl->parameters[ordinal].name == name) {
        declScope_[scope] = decl;
        return decl;
      }
    }
    return nullptr;
  }

  // ---- name helpers -----------------------------------------------------------

  // The source-level name a reference spells, chased through the alias
  // chain: a cap$N alias resolves one hop toward its captured name, and a
  // capture of an outer literal's alias keeps hopping — bounded the way
  // the checker's displayName is.
  std::string_view sourceName(const Expr &expression) const {
    std::string_view name = expression.text;
    for (std::size_t hops = 0; hops < 16 && name.starts_with("cap$");
         ++hops) {
      const auto found = captures_.find(std::string(name));
      if (found == captures_.end()) break;
      name = found->second.captured;
    }
    return name;
  }

  static bool codeType(const Type &type) { return type.name == "Code"; }

  // ---- the expression walk ----------------------------------------------------

  void walkDecl(Decl &decl, const Decl *owner) {
    if (auto scope = envScopeOf(decl)) declScope_[*scope] = &decl;
    for (auto &member : decl.members) walkDecl(member, &decl);
    // A def that splices a direct Code<…> parameter anywhere in its own
    // walked expressions is a template (M13); splices inside nested
    // literal bodies mark it from the operand-resolution path instead.
    detectTemplate(decl);
    Site site;
    site.owner = owner;
    for (ExprPtr *slot :
         {&decl.initializer, &decl.body, &decl.guard, &decl.trigger,
          &decl.deadline})
      if (*slot) {
        parentSlot_[slot->get()] = slot;
        walkExpr(*slot, site);
      }
    for (auto &contract : decl.contracts)
      if (contract.condition) {
        parentSlot_[contract.condition.get()] = &contract.condition;
        walkExpr(contract.condition, site);
      }
    for (auto &clause : decl.goalClauses)
      if (clause.condition) {
        parentSlot_[clause.condition.get()] = &clause.condition;
        walkExpr(clause.condition, site);
      }
    finalized_.insert(&decl);
  }

  // Detects `splice <ident>` in a decl's walked expressions where ident
  // binds a direct Code parameter of that decl — marks it a template.
  // Only methods expand; a splice of a non-def parameter is the operand
  // resolution's named refusal, not a mark.
  void detectTemplate(const Decl &decl) {
    if (decl.kind != DeclKind::Method || decl.parameters.empty()) return;
    const auto scan = [&](this auto &&scan, const Expr &node) -> void {
      if (node.kind == ExprKind::Splice && !node.children.empty()) {
        const Expr &operand = *node.children.front();
        if (operand.kind == ExprKind::Identifier && operand.stamp &&
            operand.stamp->ordinal &&
            declScope_.contains(operand.stamp->scope) &&
            declScope_.at(operand.stamp->scope) == &decl &&
            *operand.stamp->ordinal < decl.parameters.size() &&
            decl.parameters[*operand.stamp->ordinal].name ==
                operand.text &&
            codeType(decl.parameters[*operand.stamp->ordinal].type))
          markTemplate(&decl, {});
      }
      for (const auto &child : node.children)
        if (child) scan(*child);
      for (const ExprPtr *meta :
           {&node.scope, &node.recipients, &node.deadline, &node.guard,
            &node.recovery})
        if (*meta) scan(**meta);
    };
    for (const ExprPtr *slot :
         {&decl.initializer, &decl.body, &decl.guard, &decl.trigger,
          &decl.deadline})
      if (*slot) scan(**slot);
    for (const auto &contract : decl.contracts)
      if (contract.condition) scan(*contract.condition);
    for (const auto &clause : decl.goalClauses)
      if (clause.condition) scan(*clause.condition);
  }

  void walkExpr(ExprPtr &slot, Site &site) {
    Expr &node = *slot;
    switch (node.kind) {
    case ExprKind::Code:
      literal(node, site);
      return;
    case ExprKind::Splice:
      splice(slot, site);
      return;
    case ExprKind::Static:
      // The phase-2 boundary (§5.5): a `static` inside a fragment body is
      // refused uniformly — the operand's closedness is the checker's
      // judgment where the expanded body lands. Program-level nodes pass
      // through for that arm; their children still expand.
      if (site.insideBody) {
        report(Diagnostic{DiagnosticCode::Unsupported,
                          "'static' is refused inside a code literal",
                          node.loc},
               site.chain);
        return;
      }
      for (auto &child : node.children) {
        if (!child) continue;
        walkExpr(child, site);
        parentSlot_[child.get()] = &child;
      }
      return;
    case ExprKind::Call:
      callOrTemplate(slot, site);
      return;
    case ExprKind::Identifier:
      if (site.subst || site.actuals) repointIdentifier(slot, site);
      return;
    case ExprKind::Binary:
      if ((node.text == ":=" || node.text == "<-") &&
          node.children.size() == 2) {
        // A splice refused as a place is not walked — its operand
        // judgment would add a second, redundant refusal.
        if (!placeCheck(*node.children.front(), site))
          walkExpr(node.children.front(), site);
        parentSlot_[node.children.front().get()] = &node.children.front();
        walkExpr(node.children.back(), site);
        parentSlot_[node.children.back().get()] = &node.children.back();
        return;
      }
      break;
    case ExprKind::Let:
      for (auto &child : node.children) {
        if (!child) continue;
        walkExpr(child, site);
        parentSlot_[child.get()] = &child;
      }
      registerLet(node, site);
      return;
    case ExprKind::Block: {
      Expr *outer    = site.currentBlock;
      site.currentBlock = &node;
      for (auto &child : node.children) {
        if (!child) continue;
        walkExpr(child, site);
        parentSlot_[child.get()] = &child;
      }
      site.currentBlock = outer;
      return;
    }
    default:
      break;
    }
    for (auto &child : node.children) {
      if (!child) continue;
      walkExpr(child, site);
      parentSlot_[child.get()] = &child;
    }
    for (ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                          &node.guard, &node.recovery})
      if (*meta) {
        walkExpr(*meta, site);
        parentSlot_[meta->get()] = meta;
      }
  }

  // §5.2: a splice is never a place — `splice c := 3` refuses whether the
  // splice is the place itself or a field/index base below it.
  bool placeCheck(const Expr &place, const Site &site) {
    const Expr *node = &place;
    while (node && (node->kind == ExprKind::Field ||
                    node->kind == ExprKind::Index) &&
           !node->children.empty())
      node = node->children.front().get();
    if (node && node->kind == ExprKind::Splice) {
      report(Diagnostic{DiagnosticCode::InvalidPosition,
                        "'splice' is never a place — assignment targets "
                        "must be locals, fields or indexes",
                        node->loc},
             site.chain);
      spliceReported_.insert(node);
      return true;
    }
    return false;
  }

  void registerLet(Expr &node, Site &site) {
    if (!node.stamp) return;
    lets_[{node.stamp->scope, node.text}] = &node;
    letBlock_[&node]                 = site.currentBlock;
  }

  // ---- cap$N captures ----------------------------------------------------------

  // The outside-reference pass over one literal body, run on resolve's
  // stamps after resolveFragment and before splice substitution. Every
  // free name stamped to a binding minted before the body's resolve —
  // an enclosing *value* binding — is renamed to a cap$N alias holding
  // the captured binding's stamp; cells"…" $-anchors rewrite through the
  // same map; record.env gets each {alias → captured spelling} hop for
  // the checker's env seeding.
  void capturePass(Expr &body, const LiteralSeed &seed,
                   const FragmentKey &key, FragmentRecord &record,
                   FragmentMeta &meta, Site &site) {
    std::optional<std::uint64_t> ownerScope;
    if (const auto self = seed.bindings.find("self");
        self != seed.bindings.end())
      ownerScope = self->second.scope;
    const auto &params = record.body.parameters;
    // Mint or reuse the alias for one captured binding. `spelling` is the
    // reference's own text — one hop, since an outer body's capture may
    // itself be an alias.
    const auto captureFor = [&](std::string_view spelling,
                                const ScopeStamp &binding,
                                SourceLocation loc) -> const CaptureRecord & {
      const BindingId id = bindingOf(binding);
      if (const auto existing = capByBinding_.find(id);
          existing != capByBinding_.end()) {
        if (std::ranges::find(meta.captures, existing->second->alias) ==
            meta.captures.end())
          meta.captures.push_back(existing->second->alias);
        return *existing->second;
      }
      const auto number  = capCounter_++;
      CaptureRecord entry;
      entry.alias        = "cap$" + std::to_string(number);
      entry.anchorAlias  = "cap_" + std::to_string(number);
      entry.captured     = std::string(spelling);
      entry.binding      = binding;
      entry.stable =
          ownerScope && binding.scope == *ownerScope;
      entry.literal = key.loc;
      entry.loc     = loc;
      auto [it, inserted] =
          captures_.emplace(entry.alias, std::move(entry));
      (void)inserted;
      meta.captures.push_back(it->first);
      capByBinding_[id] = &it->second;
      return it->second;
    };
    const auto paramIndex =
        [&](const Expr &node) -> std::optional<std::size_t> {
      // A body-internal binding (let/for/cause) shadows any same-named
      // parameter — its stamp carries the internal scope's id.
      if (node.stamp && node.stamp->ordinal &&
          node.stamp->scope > meta.scopeWatermark)
        return std::nullopt;
      for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].name == node.text) return i;
      return std::nullopt;
    };
    const auto outside = [&](const Expr &node) -> bool {
      if (!node.stamp || !node.stamp->ordinal) {
        // No binding stamp: an enclosing body's parameter by name is
        // frame-bound, so the literal cannot be closed.
        for (const auto *enclosing : site.enclosingParams)
          for (const auto &parameter : *enclosing)
            if (parameter.name == node.text) {
              meta.outerParamRefs = true;
              return false;
            }
        return false;
      }
      // freshId() pre-increments, so the body's own env mints at
      // watermark + 1: scope ids above the watermark are body-internal,
      // anything at or below it was minted before the literal's
      // resolve — an outside binding.
      return node.stamp->scope <= meta.scopeWatermark;
    };
    const auto visit = [&](this auto &&visit, Expr &node) -> void {
      switch (node.kind) {
      case ExprKind::Code:
        return; // nested bodies capture under their own watermark
      case ExprKind::Identifier:
      case ExprKind::LogicVariable: {
        if (node.text.starts_with("cap$")) return;
        if (const auto index = paramIndex(node)) {
          node.stamp = ScopeStamp{meta.paramScope, *index};
          return;
        }
        if (outside(node)) {
          const std::string spelling = node.text;
          const auto &capture =
              captureFor(spelling, *node.stamp, node.loc);
          record.env[capture.alias] = spelling;
          node.text                 = capture.alias;
        }
        return;
      }
      case ExprKind::Cells: {
        const auto path = parseCellsPath(node.text, node.loc);
        if (!path ||
            path->anchor.kind != CellsPath::Anchor::Kind::Variable ||
            !node.stamp || !node.stamp->ordinal ||
            node.stamp->scope > meta.scopeWatermark)
          return; // no $-anchor, unbound anchor, or a body-internal one
        // The anchor names an outside binding — the node's stamp is the
        // anchor binding's identity (resolve.cpp:505). Frame-bound
        // anchors are refused outright; only the stable class follows a
        // ref-box.
        const auto &capture =
            captureFor(path->anchor.variable, *node.stamp, node.loc);
        if (!capture.stable) {
          report(Diagnostic{
                     DiagnosticCode::InvalidPosition,
                     "cells\"…\" $-anchor '$" + path->anchor.variable +
                         "' captures a frame-bound binding — anchors are "
                         "supported for stable captures (fields, member "
                         "vars and implicit names) only",
                     node.loc},
                 site.chain);
          return;
        }
        record.env[capture.alias] = path->anchor.variable;
        if (node.text.starts_with("$" + path->anchor.variable))
          node.text = "$" + capture.anchorAlias +
                      node.text.substr(1 + path->anchor.variable.size());
        return;
      }
      default:
        break;
      }
      for (auto &child : node.children)
        if (child) visit(*child);
      for (ExprPtr *meta2 :
           {&node.scope, &node.recipients, &node.deadline, &node.guard,
            &node.recovery})
        if (*meta2) visit(**meta2);
    };
    visit(body);
  }

  // §5.6's classification, recomputed after expansion consumed whatever
  // it consumed: a surviving literal is closed when every outside
  // reference the body still makes is state-side (an owner-scope
  // binding) — name-resolved member and global refs never capture in
  // the first place, so the still-referenced captures are the whole
  // judgment. A splice that consumed a capture leaves no reference
  // behind, which is what substitution is for.
  void reclassify(Expr &root, FragmentRecord &record,
                  const FragmentMeta &meta) {
    bool closed = true;
    const auto visit = [&](this auto &&visit, const Expr &node) -> void {
      if (node.kind == ExprKind::Code) return;
      if ((node.kind == ExprKind::Identifier ||
           node.kind == ExprKind::LogicVariable) &&
          node.text.starts_with("cap$")) {
        if (const auto found = captures_.find(node.text);
            found != captures_.end() && !found->second.stable)
          closed = false;
      }
      if (node.kind == ExprKind::Cells &&
          node.text.starts_with("$cap_")) {
        // Anchor aliases mint under the stable gate — the refusal
        // already fired for frame-bound ones.
      }
      for (const auto &child : node.children)
        if (child) visit(*child);
      for (const ExprPtr *meta :
           {&node.scope, &node.recipients, &node.deadline, &node.guard,
            &node.recovery})
        if (*meta) visit(**meta);
    };
    visit(root);
    record.closed = closed && !meta.outerParamRefs;
  }

  // ---- literals ----------------------------------------------------------------

  void literal(Expr &literalExpr, Site &site) {
    const FragmentKey key{literalExpr.loc,
                          literalExpr.stamp ? literalExpr.stamp->ordinal
                                            : std::nullopt};
    if (fragments_.contains(key)) return; // processed or a cloneExpr copy
    if (site.depth >= limits_.check.maxExpansionDepth) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "code\"…\" nesting exceeds "
                        "CheckLimits::maxExpansionDepth",
                        literalExpr.loc},
             site.chain);
      return;
    }
    auto parsed =
        parseCodePath(literalExpr.text, literalExpr.loc, limits_.parse);
    auto &record = fragments_[key];
    auto &meta   = metas_[key];
    if (parsed) {
      record.body = std::move(*parsed);
    } else {
      report(std::move(parsed.error()), site.chain);
    }
    if (!record.body.body) return;
    for (const auto &parameter : record.body.parameters)
      if (codeType(parameter.type)) meta.spliceOnly = true;
    if (!record.body.parameters.empty())
      meta.paramScope = ++seeds_.nextScope;
    LiteralChain chain = site.chain;
    chain.literals.push_back(literalExpr.loc);
    const auto seed = seeds_.byLiteral.find(key);
    if (seed == seeds_.byLiteral.end()) {
      // A literal resolve never env-walked — the record still parses,
      // but without stamps there is no capture or substitution truth.
      record.closed = false;
      return;
    }
    meta.scopeWatermark = seeds_.nextScope;
    auto resolved =
        resolveFragment(program_, *record.body.body, seed->second, seeds_,
                        limits_.resolve);
    if (!resolved)
      for (auto &diagnostic : resolved.error())
        report(std::move(diagnostic), chain);
    Site body;
    body.owner      = seed->second.owner;
    body.params     = &record.body.parameters;
    body.paramScope = meta.paramScope;
    body.subst      = site.subst;
    body.actuals    = site.actuals;
    body.enclosingParams = site.enclosingParams;
    if (site.params)
      body.enclosingParams.insert(body.enclosingParams.begin(),
                                  site.params);
    body.watermark  = meta.scopeWatermark;
    body.chain      = chain;
    body.depth      = site.depth + 1;
    body.insideBody = true;
    capturePass(*record.body.body, seed->second, key, record, meta, body);
    parentSlot_[record.body.body.get()] = &record.body.body;
    walkExpr(record.body.body, body);
    reclassify(*record.body.body, record, meta);
  }

  // ---- splice substitution ------------------------------------------------------

  struct Judged {
    enum class Kind : std::uint8_t { Fragment, Pending, Refused } kind;
    const Expr *literal{};
    const Expr *consumed{}; // a let binding the substitution consumes
  };

  // §5.2's statically-known judgment — transitive and post-substitution.
  // A literal, a substituted block ending in one, a let-bound chain to
  // one, a Code parameter of the body being instantiated, or an
  // enclosing template's Code parameter. `role` names the operand
  // position in refusals — a splice's operand, a splice's Code
  // argument, or a template call's Code argument.
  Judged judgeOperand(Expr &operand, Site &site,
                      const std::string &role) {
    if (operand.kind == ExprKind::Code) {
      literal(operand, site);
      return {Judged::Kind::Fragment, &operand, nullptr};
    }
    if (operand.kind == ExprKind::Block) {
      std::set<const Expr *> visiting;
      if (const auto *lit = judgeInit(operand, site, visiting))
        return {Judged::Kind::Fragment, lit, nullptr};
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        role + " is not a statically-known fragment",
                        operand.loc},
             site.chain);
      return {Judged::Kind::Refused, nullptr, nullptr};
    }
    if (operand.kind != ExprKind::Identifier) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        role + " must name a fragment — a code\"…\" "
                               "literal or a let-bound name",
                        operand.loc},
             site.chain);
      return {Judged::Kind::Refused, nullptr, nullptr};
    }
    const std::string_view name = sourceName(operand);
    // The innermost body's own parameters carry (paramScope, index).
    if (operand.stamp && operand.stamp->ordinal && site.params &&
        operand.stamp->scope == site.paramScope &&
        *operand.stamp->ordinal < site.params->size()) {
      const auto index   = *operand.stamp->ordinal;
      const auto &param  = (*site.params)[index];
      if (site.subst && site.subst->paramScope == site.paramScope) {
        if (const auto actual = site.subst->actuals.find(index);
            actual != site.subst->actuals.end())
          return {Judged::Kind::Fragment, actual->second, nullptr};
        report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                          role + " '" + param.name +
                              "' is a value parameter, not a fragment",
                          operand.loc},
               site.chain);
        return {Judged::Kind::Refused, nullptr, nullptr};
      }
      if (codeType(param.type))
        return {Judged::Kind::Pending, nullptr, nullptr};
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        role + " '" + param.name +
                            "' is a value parameter, not a fragment",
                        operand.loc},
             site.chain);
      return {Judged::Kind::Refused, nullptr, nullptr};
    }
    if (operand.stamp && operand.stamp->ordinal) {
      const BindingId id = bindingOf(*operand.stamp);
      // An enclosing template expansion's Code parameter actual.
      if (site.actuals) {
        if (const auto found = site.actuals->find(id);
            found != site.actuals->end())
          return {Judged::Kind::Fragment, found->second, nullptr};
      }
      // A direct Code parameter of a declaration — splicing it anywhere
      // makes that declaration a template (M13).
      if (const Decl *decl =
              declOfScope(operand.stamp->scope, operand)) {
        const auto &params = decl->parameters;
        if (*operand.stamp->ordinal < params.size() &&
            params[*operand.stamp->ordinal].name == name &&
            codeType(params[*operand.stamp->ordinal].type)) {
          if (decl->kind == DeclKind::Method) {
            markTemplate(decl, site.chain);
            return {Judged::Kind::Pending, nullptr, nullptr};
          }
          report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                            role + " '" + std::string(name) +
                                "' is a Code parameter of a non-def "
                                "declaration — only def Code parameters "
                                "expand",
                            operand.loc},
                 site.chain);
          return {Judged::Kind::Refused, nullptr, nullptr};
        }
      }
      // A let-bound name in the operand's binding scope.
      if (const auto found =
              lets_.find({operand.stamp->scope, std::string(name)});
          found != lets_.end()) {
        std::set<const Expr *> visiting;
        if (const auto *lit = judgeLet(*found->second, site, visiting))
          return {Judged::Kind::Fragment, lit, found->second};
      }
      // A binding that is none of the above — a field, a var, a
      // non-Code parameter, a loop or recovery binding — is never
      // statically known.
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        role + " '" + std::string(name) +
                            "' is not a statically-known fragment — a "
                            "var-bound name, a field, a call result or "
                            "an opaque Code parameter never is",
                        operand.loc},
             site.chain);
      return {Judged::Kind::Refused, nullptr, nullptr};
    }
    // Scope-only stamped or unstamped operand: an enclosing body's
    // parameter by name, or unbound.
    for (const auto *enclosing : site.enclosingParams)
      for (std::size_t i = 0; i < enclosing->size(); ++i) {
        if ((*enclosing)[i].name != name) continue;
        if (site.subst) {
          for (auto link = site.subst; link; link = link->outer) {
            const auto &params = *link->params;
            for (std::size_t j = 0; j < params.size(); ++j) {
              if (params[j].name != name) continue;
              if (const auto actual = link->actuals.find(j);
                  actual != link->actuals.end())
                return {Judged::Kind::Fragment, actual->second, nullptr};
            }
          }
        }
        return {Judged::Kind::Pending, nullptr, nullptr};
      }
    report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                      role + " '" + std::string(name) +
                          "' is not a statically-known fragment — it "
                          "names no binding here",
                      operand.loc},
           site.chain);
    return {Judged::Kind::Refused, nullptr, nullptr};
  }

  // The transitive tail: a let initializer that is itself statically
  // known — a literal, an identifier naming a statically-known binding,
  // or a substituted block ending in one.
  const Expr *judgeLet(const Expr &let, Site &site,
                       std::set<const Expr *> &visiting) {
    if (!visiting.insert(&let).second || let.children.empty())
      return nullptr;
    return judgeInit(*let.children.front(), site, visiting);
  }

  const Expr *judgeInit(const Expr &init, Site &site,
                        std::set<const Expr *> &visiting) {
    switch (init.kind) {
    case ExprKind::Code:
      return &init;
    case ExprKind::Identifier: {
      if (!init.stamp || !init.stamp->ordinal) return nullptr;
      const BindingId id = bindingOf(*init.stamp);
      if (site.actuals)
        if (const auto found = site.actuals->find(id);
            found != site.actuals->end())
          return found->second;
      if (const auto found =
              lets_.find({init.stamp->scope,
                          std::string(sourceName(init))});
          found != lets_.end())
        return judgeLet(*found->second, site, visiting);
      return nullptr;
    }
    case ExprKind::Block:
      // A substituted initializer — synthesized head lets end in the
      // value the site computed.
      if (!init.children.empty())
        return judgeInit(*init.children.back(), site, visiting);
      return nullptr;
    default:
      return nullptr;
    }
  }

  void splice(ExprPtr &slot, Site &site) {
    Expr &node = *slot;
    if (++splices_ > limits_.check.maxSplices) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "expansion exceeds CheckLimits::maxSplices",
                        node.loc},
             site.chain);
      spliceReported_.insert(&node);
      return;
    }
    if (site.depth + 1 >= limits_.check.maxExpansionDepth) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "splice expansion exceeds "
                        "CheckLimits::maxExpansionDepth",
                        node.loc},
             site.chain);
      spliceReported_.insert(&node);
      return;
    }
    if (node.children.empty() || !node.children.front()) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "'splice' requires an operand", node.loc},
             site.chain);
      spliceReported_.insert(&node);
      return;
    }
    // Arguments evaluate in the site's context, inside-out: a nested
    // splice substitutes first (§5.2). A `splice` operand substitutes
    // itself before the operand judgment — the post-substitution form
    // is what is known.
    for (std::size_t i = 1; i < node.children.size(); ++i) {
      walkExpr(node.children[i], site);
      parentSlot_[node.children[i].get()] = &node.children[i];
    }
    if (node.children.front() &&
        node.children.front()->kind == ExprKind::Splice) {
      walkExpr(node.children.front(), site);
      parentSlot_[node.children.front().get()] = &node.children.front();
    }
    Expr &operand = *node.children.front();
    const Judged judged =
        judgeOperand(operand, site, "'splice' operand");
    if (judged.kind != Judged::Kind::Fragment) {
      spliceReported_.insert(&node);
      return; // pending or refused
    }
    substitute(slot, *judged.literal, judged.consumed, site);
  }

  // cloneExpr of the fragment body, the synthesized `let p<k>` argument
  // bindings §5.4 requires at its head, the parameter references
  // re-pointed, and the body's own nested splices expanded where it
  // lands.
  void substitute(ExprPtr &slot, const Expr &literalExpr,
                  const Expr *consumed, Site &site) {
    Expr &node = *slot;
    const FragmentKey key{literalExpr.loc,
                          literalExpr.stamp ? literalExpr.stamp->ordinal
                                            : std::nullopt};
    const auto found = fragments_.find(key);
    if (found == fragments_.end() || !found->second.body.body) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "splice operand has no parsed fragment body",
                        node.loc},
             site.chain);
      spliceReported_.insert(&node);
      return;
    }
    const FragmentRecord &record  = found->second;
    const FragmentMeta &meta      = metas_.at(key);
    const std::size_t arity       = record.body.parameters.size();
    if (node.children.size() - 1 != arity) {
      report(Diagnostic{DiagnosticCode::TypeMismatch,
                        "splice passes " +
                            std::to_string(node.children.size() - 1) +
                            " argument(s) to a fragment declaring " +
                            std::to_string(arity) + " parameter(s)",
                        node.loc},
             site.chain);
      spliceReported_.insert(&node);
      return;
    }
    auto subst        = std::make_shared<Substitution>();
    subst->params     = &record.body.parameters;
    subst->paramScope = meta.paramScope;
    subst->outer      = site.subst;
    bool pending      = false;
    bool failed       = false;
    for (std::size_t i = 0; i < arity; ++i) {
      const auto &parameter = record.body.parameters[i];
      ExprPtr &arg          = node.children[i + 1];
      if (!codeType(parameter.type)) continue;
      // A Code actual must be statically known — the same transitive
      // judgment as the splice operand itself. A pending actual makes
      // the whole splice pending: it resolves where the enclosing
      // template is instantiated.
      const Judged actual =
          judgeOperand(*arg, site,
                       "splice argument for Code parameter '" +
                           parameter.name + "'");
      if (actual.kind == Judged::Kind::Pending) {
        pending = true;
        continue;
      }
      if (actual.kind != Judged::Kind::Fragment) {
        failed = true;
        continue;
      }
      subst->actuals[i] = actual.literal;
    }
    // Pending resolves per-site inside a clone (or vanishes with the
    // template that owns it); failed already reported — neither should
    // see the residual scan's generic refusal.
    if (pending || failed) {
      spliceReported_.insert(&node);
      return;
    }
    const auto synthScope = ++seeds_.nextScope;
    std::size_t synthOrd  = 0;
    std::vector<ExprPtr> lets;
    for (std::size_t i = 0; i < arity; ++i) {
      const auto &parameter = record.body.parameters[i];
      ExprPtr &arg          = node.children[i + 1];
      if (codeType(parameter.type)) continue;
      // Call-by-value: one fresh `let p<k> := <arg>` per value argument,
      // evaluated once in argument order at the expanded body's head.
      const std::string synthName = "p" + std::to_string(synthCounter_++);
      const ScopeStamp synthStamp{synthScope, synthOrd++};
      auto let        = std::make_unique<Expr>();
      let->kind       = ExprKind::Let;
      let->text       = synthName;
      let->loc        = node.loc;
      let->annotation = parameter.type;
      let->stamp      = ScopeStamp{synthScope, std::nullopt};
      Expr *argNode   = arg.get();
      let->children.push_back(std::move(arg));
      parentSlot_[argNode] = &let->children.back();
      subst->letNames[i]   = synthName;
      subst->letStamps[i]  = synthStamp;
      synthesized_.push_back(
          SynthRecord{synthStamp, synthName, parameter.name, node.loc});
      lets.push_back(std::move(let));
    }
    auto cloned = cloneExpr(*record.body.body);
    remapScopes(*cloned, meta.scopeWatermark, meta.paramScope);
    Site sub;
    sub.owner           = site.owner;
    sub.params          = &record.body.parameters;
    sub.paramScope      = meta.paramScope;
    sub.subst           = subst;
    sub.actuals         = site.actuals;
    sub.enclosingParams = site.enclosingParams;
    if (site.params)
      sub.enclosingParams.insert(sub.enclosingParams.begin(),
                                 site.params);
    sub.enclosingParams.insert(sub.enclosingParams.begin(), sub.params);
    sub.watermark  = meta.scopeWatermark;
    sub.chain      = site.chain;
    sub.depth      = site.depth + 1;
    sub.insideBody = true;
    walkExpr(cloned, sub);
    if (!site.insideBody) renameCaptures(*cloned);
    if (consumed) consumedLets_.insert(const_cast<Expr *>(consumed));
    if (lets.empty()) {
      slot = std::move(cloned);
    } else {
      auto block   = std::make_unique<Expr>();
      block->kind  = ExprKind::Block;
      block->loc   = node.loc;
      block->stamp = node.stamp;
      for (auto &let : lets)
        block->children.push_back(std::move(let));
      block->children.push_back(std::move(cloned));
      for (auto &child : block->children)
        parentSlot_[child.get()] = &child;
      slot = std::move(block);
    }
  }

  // A clone landing at a program position leaves the record's env
  // records behind — cap$N resolves only through those records, so an
  // inlined body's aliases rewrite back to their captured names. The
  // binding stamp survives verbatim, which is the channel §5.3 names
  // for the checker's stamp-honoring rule; a clone landing inside a
  // body keeps the alias, where the enclosing record's env resolves it
  // by name.
  void renameCaptures(Expr &node) {
    const auto visit = [&](this auto &&visit, Expr &node2) -> void {
      if (node2.kind == ExprKind::Code) return;
      if ((node2.kind == ExprKind::Identifier ||
           node2.kind == ExprKind::LogicVariable) &&
          node2.text.starts_with("cap$"))
        node2.text = std::string(sourceName(node2));
      if (node2.kind == ExprKind::Cells) {
        const auto path = parseCellsPath(node2.text, node2.loc);
        if (path &&
            path->anchor.kind == CellsPath::Anchor::Kind::Variable &&
            path->anchor.variable.starts_with("cap_")) {
          const std::string alias =
              "cap$" + path->anchor.variable.substr(4);
          if (const auto found = captures_.find(alias);
              found != captures_.end() &&
              node2.text.starts_with("$" + path->anchor.variable))
            node2.text = "$" + found->second.captured +
                         node2.text.substr(1 +
                                           path->anchor.variable.size());
        }
        return;
      }
      for (auto &child : node2.children)
        if (child) visit(*child);
      for (const ExprPtr *meta :
           {&node2.scope, &node2.recipients, &node2.deadline,
            &node2.guard, &node2.recovery})
        if (*meta) visit(**meta);
    };
    visit(node);
  }

  // Per-clone binding-identity freshening: a substituted body carries
  // resolve stamps minted inside the record, and N sites mint N graphs —
  // each internal scope gets a fresh id so two sites' bindings never
  // share an identity. Capture stamps (scope ≤ watermark) keep the
  // outside binding, the canonical paramScope keeps its identity, and
  // fragment ordinals on Code nodes are untouched — (loc, ordinal)
  // resolves every clone of one literal to the original's record.
  void remapScopes(Expr &node, std::uint64_t watermark,
                   std::uint64_t exempt) {
    std::map<std::uint64_t, std::uint64_t> remap;
    const auto visit = [&](this auto &&visit, Expr &node2) -> void {
      if (node2.stamp && node2.stamp->scope > watermark &&
          node2.stamp->scope != exempt) {
        auto &fresh = remap[node2.stamp->scope];
        if (!fresh) fresh = ++seeds_.nextScope;
        node2.stamp->scope = fresh;
      }
      for (auto &child : node2.children)
        if (child) visit(*child);
      for (ExprPtr *meta :
           {&node2.scope, &node2.recipients, &node2.deadline, &node2.guard,
            &node2.recovery})
        if (*meta) visit(**meta);
    };
    visit(node);
  }

  // Identifier uses inside an instantiated body: an enclosing template
  // Code-parameter actual replaces the node wholesale; a value
  // parameter's reference re-points at its synthesized binding.
  void repointIdentifier(ExprPtr &slot, Site &site) {
    Expr &node = *slot;
    if (!node.stamp || !node.stamp->ordinal) return;
    const BindingId id = bindingOf(*node.stamp);
    if (site.actuals) {
      if (const auto found = site.actuals->find(id);
          found != site.actuals->end()) {
        slot = cloneExpr(*found->second);
        return;
      }
    }
    if (site.subst && node.stamp->scope == site.subst->paramScope &&
        site.subst->params &&
        *node.stamp->ordinal < site.subst->params->size()) {
      const auto index = *node.stamp->ordinal;
      if (const auto let = site.subst->letStamps.find(index);
          let != site.subst->letStamps.end()) {
        node.text  = site.subst->letNames.at(index);
        node.stamp = let->second;
        return;
      }
      if (const auto actual = site.subst->actuals.find(index);
          actual != site.subst->actuals.end())
        slot = cloneExpr(*actual->second);
    }
  }

  // ---- template expansion (M13) -------------------------------------------------

  // The template bars from spec :456-470, reported once per marked decl:
  // no effect/native/capacity membership and no control flow in template
  // bodies — an inlined return would exit the caller.
  void markTemplate(const Decl *decl, const LiteralChain &chain) {
    if (!templates_.insert(decl).second) return;
    if (decl->native)
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "a 'native' method cannot be a template — "
                        "templates exist only at expansion time",
                        decl->loc},
             chain);
    if (decl->effect)
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "an 'effect' method cannot be a template — "
                        "templates exist only at expansion time",
                        decl->loc},
             chain);
    if (const auto owner = owners_.at(decl);
        owner && owner->kind == DeclKind::Capacity)
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "a capacity member cannot be a template — "
                        "templates exist only at expansion time",
                        decl->loc},
             chain);
    const auto scan = [&](this auto &&scan, const Expr &node) -> void {
      std::string_view barred;
      switch (node.kind) {
      case ExprKind::Return:    barred = "return"; break;
      case ExprKind::Yield:     barred = "yield"; break;
      case ExprKind::Consume:   barred = "consume"; break;
      case ExprKind::Terminate: barred = "terminate"; break;
      default: break;
      }
      if (!barred.empty()) {
        report(Diagnostic{
                   DiagnosticCode::InvalidPosition,
                   "'" + std::string(barred) +
                       "' is refused in a template body — inlined, it "
                       "would exit the caller",
                   node.loc},
               chain);
        return;
      }
      for (const auto &child : node.children)
        if (child) scan(*child);
      for (const ExprPtr *meta :
           {&node.scope, &node.recipients, &node.deadline, &node.guard,
            &node.recovery})
        if (*meta) scan(**meta);
    };
    for (const ExprPtr *slot :
         {&decl->initializer, &decl->body, &decl->guard, &decl->trigger,
          &decl->deadline})
      if (*slot) scan(**slot);
    for (const auto &contract : decl->contracts)
      if (contract.condition) scan(*contract.condition);
    for (const auto &clause : decl->goalClauses)
      if (clause.condition) scan(*clause.condition);
  }

  void callOrTemplate(ExprPtr &slot, Site &site) {
    Expr &node = *slot;
    if (!node.children.empty()) {
      const Expr &callee = *node.children.front();
      std::string_view name;
      if (callee.kind == ExprKind::Identifier) {
        name = callee.text;
      } else if (callee.kind == ExprKind::Field &&
                 !callee.children.empty() &&
                 callee.children.front()->kind == ExprKind::Identifier) {
        name = callee.text; // self.t(…) resolves on the same owner
      }
      if (!name.empty()) {
        if (const Decl *def = methodOf(site.owner, name)) {
          if (templates_.contains(def)) {
            templateCall(slot, site, def);
            return;
          }
          if (!finalized_.contains(def)) {
            // The callee's own declaration has not finished expanding —
            // template-ness may arrive with a nested literal's splice,
            // so the call defers to the end of the pass when every
            // decl's status is final.
            DeferredCall deferred;
            deferred.call            = &node;
            deferred.owner           = site.owner;
            deferred.subst           = site.subst;
            if (site.actuals) deferred.actuals = *site.actuals;
            deferred.enclosingParams = site.enclosingParams;
            deferred.chain           = site.chain;
            deferred.depth           = site.depth;
            deferred.insideBody      = site.insideBody;
            deferred_.push_back(std::move(deferred));
          }
        }
      }
    }
    for (auto &child : node.children) {
      if (!child) continue;
      walkExpr(child, site);
      parentSlot_[child.get()] = &child;
    }
    for (ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                          &node.guard, &node.recovery})
      if (*meta) {
        walkExpr(*meta, site);
        parentSlot_[meta->get()] = meta;
      }
  }

  // Inline the template's body at the call site: every Code parameter
  // binds a statically-known actual (an opaque one is the named call-site
  // refusal), every value parameter a fresh `let p<k>`, and the result
  // expands in the site's context.
  void templateCall(ExprPtr &slot, Site &site, const Decl *def) {
    Expr &node = *slot;
    if (std::ranges::find(expanding_, def) != expanding_.end()) {
      report(Diagnostic{
                 DiagnosticCode::InvalidDeclaration,
                 "recursive-template: expansion of '" + def->name +
                     "' cycles back to itself — template cycles are "
                     "refused, directly or transitively",
                 node.loc},
             site.chain);
      return;
    }
    if (++splices_ > limits_.check.maxSplices) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "expansion exceeds CheckLimits::maxSplices",
                        node.loc},
             site.chain);
      return;
    }
    if (site.depth + 1 >= limits_.check.maxExpansionDepth) {
      report(Diagnostic{DiagnosticCode::InvalidDeclaration,
                        "template expansion exceeds "
                        "CheckLimits::maxExpansionDepth",
                        node.loc},
             site.chain);
      return;
    }
    if (!def->body) return; // a body-less decl never splices a parameter
    const auto s_d = envScopeOf(*def);
    if (node.children.size() - 1 != def->parameters.size()) {
      report(Diagnostic{DiagnosticCode::TypeMismatch,
                        "call passes " +
                            std::to_string(node.children.size() - 1) +
                            " argument(s) to '" + def->name +
                            "' declaring " +
                            std::to_string(def->parameters.size()) +
                            " parameter(s)",
                        node.loc},
             site.chain);
      return;
    }
    auto subst        = std::make_shared<Substitution>();
    subst->params     = &def->parameters;
    subst->paramScope = s_d.value_or(~0ull);
    subst->outer      = site.subst;
    bool pending      = false;
    bool failed       = false;
    for (std::size_t i = 0; i < def->parameters.size(); ++i) {
      const auto &parameter = def->parameters[i];
      ExprPtr &arg          = node.children[i + 1];
      if (!codeType(parameter.type)) continue;
      const Judged actual =
          judgeOperand(*arg, site,
                       "Code argument '" + parameter.name + "' to '" +
                           def->name + "'");
      if (actual.kind == Judged::Kind::Pending) {
        pending = true;
        continue;
      }
      if (actual.kind != Judged::Kind::Fragment) {
        failed = true;
        continue;
      }
      subst->actuals[i] = actual.literal;
    }
    if (pending || failed) return;
    auto actuals = std::make_shared<Actuals>();
    const auto synthScope = ++seeds_.nextScope;
    std::size_t synthOrd  = 0;
    std::vector<ExprPtr> lets;
    for (std::size_t i = 0; i < def->parameters.size(); ++i) {
      const auto &parameter = def->parameters[i];
      ExprPtr &arg          = node.children[i + 1];
      if (codeType(parameter.type)) {
        if (s_d) (*actuals)[BindingId{*s_d, i}] = subst->actuals.at(i);
        continue;
      }
      const std::string synthName = "p" + std::to_string(synthCounter_++);
      const ScopeStamp synthStamp{synthScope, synthOrd++};
      auto let        = std::make_unique<Expr>();
      let->kind       = ExprKind::Let;
      let->text       = synthName;
      let->loc        = node.loc;
      let->annotation = parameter.type;
      let->stamp      = ScopeStamp{synthScope, std::nullopt};
      Expr *argNode   = arg.get();
      let->children.push_back(std::move(arg));
      parentSlot_[argNode] = &let->children.back();
      subst->letNames[i]   = synthName;
      subst->letStamps[i]  = synthStamp;
      synthesized_.push_back(
          SynthRecord{synthStamp, synthName, parameter.name, node.loc});
      lets.push_back(std::move(let));
    }
    auto cloned = cloneExpr(*def->body);
    // Parameter references re-point at the actuals and the synthesized
    // bindings — stamped (s_d, ordinal) so substitution never
    // re-resolves; cap$N aliases of the parameters inside nested literal
    // bodies resolve through the same channel.
    if (s_d) {
      const auto repoint = [&](this auto &&repoint, ExprPtr &slotRef) {
        Expr &node2 = *slotRef;
        if (node2.kind == ExprKind::Identifier && node2.stamp &&
            node2.stamp->ordinal && node2.stamp->scope == *s_d &&
            *node2.stamp->ordinal < def->parameters.size() &&
            def->parameters[*node2.stamp->ordinal].name ==
                sourceName(node2)) {
          const auto index = *node2.stamp->ordinal;
          if (const auto actual = subst->actuals.find(index);
              actual != subst->actuals.end()) {
            slotRef = cloneExpr(*actual->second);
            return;
          }
          if (const auto let = subst->letStamps.find(index);
              let != subst->letStamps.end()) {
            node2.text  = subst->letNames.at(index);
            node2.stamp = let->second;
            return;
          }
        }
        for (auto &child : node2.children) {
          if (!child) continue;
          repoint(child);
          parentSlot_[child.get()] = &child;
        }
        for (ExprPtr *meta : {&node2.scope, &node2.recipients,
                              &node2.deadline, &node2.guard,
                              &node2.recovery})
          if (*meta) {
            repoint(*meta);
            parentSlot_[meta->get()] = meta;
          }
      };
      repoint(cloned);
      // Every scope minted inside the def's env is per-site — internal
      // bindings get fresh ids; owner-scope and program-scope bindings
      // keep theirs.
      std::map<std::uint64_t, std::uint64_t> remap;
      const auto freshen = [&](this auto &&freshen, Expr &node2) -> void {
        if (node2.stamp && node2.stamp->scope >= *s_d) {
          auto &fresh = remap[node2.stamp->scope];
          if (!fresh) fresh = ++seeds_.nextScope;
          node2.stamp->scope = fresh;
        }
        for (auto &child : node2.children)
          if (child) freshen(*child);
        for (ExprPtr *meta : {&node2.scope, &node2.recipients,
                              &node2.deadline, &node2.guard,
                              &node2.recovery})
          if (*meta) freshen(**meta);
      };
      freshen(*cloned);
    }
    Site sub;
    sub.owner           = site.owner;
    sub.subst           = subst;
    static const Actuals emptyActuals;
    sub.actuals         = s_d ? &*actuals : &emptyActuals;
    sub.enclosingParams = site.enclosingParams;
    sub.chain           = site.chain;
    sub.depth           = site.depth + 1;
    sub.insideBody      = site.insideBody;
    expanding_.push_back(def);
    walkExpr(cloned, sub);
    expanding_.pop_back();
    if (!site.insideBody) renameCaptures(*cloned);
    if (lets.empty()) {
      slot = std::move(cloned);
    } else {
      auto block   = std::make_unique<Expr>();
      block->kind  = ExprKind::Block;
      block->loc   = node.loc;
      block->stamp = node.stamp;
      for (auto &let : lets)
        block->children.push_back(std::move(let));
      block->children.push_back(std::move(cloned));
      for (auto &child : block->children)
        parentSlot_[child.get()] = &child;
      slot = std::move(block);
    }
  }

  void processDeferred() {
    for (std::size_t i = 0; i < deferred_.size(); ++i) {
      // A local copy keeps the actuals map alive for the whole
      // expansion whatever the queue does meanwhile.
      DeferredCall deferred = deferred_[i];
      const auto slot = parentSlot_.find(deferred.call);
      if (slot == parentSlot_.end() || !*slot->second) continue;
      const Expr &node = *deferred.call;
      if (node.children.empty()) continue;
      const Expr &callee = *node.children.front();
      std::string_view name;
      if (callee.kind == ExprKind::Identifier) name = callee.text;
      else if (callee.kind == ExprKind::Field &&
               !callee.children.empty() &&
               callee.children.front()->kind == ExprKind::Identifier)
        name = callee.text;
      if (name.empty()) continue;
      const Decl *def = methodOf(deferred.owner, name);
      if (!def || !templates_.contains(def)) continue;
      Site site;
      site.owner           = deferred.owner;
      site.subst           = deferred.subst;
      static const Actuals emptyActuals;
      site.actuals = deferred.actuals.empty() ? &emptyActuals
                                             : &deferred.actuals;
      site.enclosingParams = deferred.enclosingParams;
      site.chain           = deferred.chain;
      site.depth           = deferred.depth;
      site.insideBody      = deferred.insideBody;
      templateCall(*slot->second, site, def);
    }
    deferred_.clear();
  }

  // ---- dead-let erasure and residual checks ---------------------------------------

  // A binding has references iff an Identifier or LogicVariable still
  // carries its (scope, source-name) pair — cap$N aliases resolve to
  // their captured names so renamed uses count too.
  void collectRefs(const Expr &node,
                   std::set<std::pair<std::uint64_t, std::string>> &refs) {
    if ((node.kind == ExprKind::Identifier ||
         node.kind == ExprKind::LogicVariable) &&
        node.stamp && node.stamp->ordinal)
      refs.emplace(node.stamp->scope, std::string(sourceName(node)));
    for (const auto &child : node.children)
      if (child) collectRefs(*child, refs);
    for (const ExprPtr *meta : {&node.scope, &node.recipients,
                                &node.deadline, &node.guard,
                                &node.recovery})
      if (*meta) collectRefs(**meta, refs);
  }

  // §5.4: a let binding left dead by full substitution is erased — and
  // the erasure cascades through let-to-let chains. Candidates are lets
  // consumed by a splice or bound to a statically-known-shaped
  // initializer (a literal or identifier — pure, so erasure is safe); a
  // let whose initializer might have effects is never a candidate.
  void sweepDeadLets() {
    for (;;) {
      std::set<std::pair<std::uint64_t, std::string>> refs;
      const auto collectDecl = [&](this auto &&collectDecl,
                                   const Decl &decl) -> void {
        for (const ExprPtr *slot :
             {&decl.initializer, &decl.body, &decl.guard, &decl.trigger,
              &decl.deadline})
          if (*slot) collectRefs(**slot, refs);
        for (const auto &contract : decl.contracts)
          if (contract.condition) collectRefs(*contract.condition, refs);
        for (const auto &clause : decl.goalClauses)
          if (clause.condition) collectRefs(*clause.condition, refs);
        for (const auto &member : decl.members) collectDecl(member);
      };
      for (const auto &decl : program_.declarations) collectDecl(decl);
      for (const auto &[key, record] : fragments_)
        if (record.body.body) collectRefs(*record.body.body, refs);
      std::vector<std::pair<std::pair<std::uint64_t, std::string>, Expr *>>
          dead;
      for (const auto &[key, let] : lets_) {
        if (let->children.empty()) continue;
        const auto block = letBlock_.find(let);
        if (block == letBlock_.end() || !block->second) continue;
        // A let of a splice-only literal is an evaluated-position
        // refusal, not a residue — the residual scan names it, so it is
        // never an erasure candidate.
        if (let->children.front()->kind == ExprKind::Code) {
          const Expr &init = *let->children.front();
          const FragmentKey initKey{
              init.loc,
              init.stamp ? init.stamp->ordinal : std::nullopt};
          const auto meta = metas_.find(initKey);
          if (meta != metas_.end() && meta->second.spliceOnly) continue;
        }
        const bool candidate =
            consumedLets_.contains(let) ||
            let->children.front()->kind == ExprKind::Code ||
            let->children.front()->kind == ExprKind::Identifier;
        if (!candidate || !let->stamp) continue;
        if (!refs.contains({let->stamp->scope, let->text}))
          dead.push_back({key, let});
      }
      if (dead.empty()) return;
      for (const auto &[key, let] : dead) {
        auto *block = letBlock_[let];
        if (!block) continue;
        std::erase_if(block->children,
                      [&](const ExprPtr &child) {
                        return child.get() == let;
                      });
        lets_.erase(key);
        letBlock_.erase(let);
      }
    }
  }

  // Templates exist only at expansion time — the decl leaves the merged
  // program once every call site is expanded. Removal by index keeps
  // sibling Decls and every nested Expr address stable (a vector move
  // steals the members buffer, so member-of-member addresses survive).
  void removeTemplates() {
    const auto sweep = [&](this auto &&sweep, Decl &decl) -> void {
      std::vector<std::size_t> doomed;
      for (std::size_t i = 0; i < decl.members.size(); ++i)
        if (templates_.contains(&decl.members[i])) doomed.push_back(i);
      for (auto it = doomed.rbegin(); it != doomed.rend(); ++it)
        decl.members.erase(decl.members.begin() +
                           static_cast<long>(*it));
      for (auto &member : decl.members) sweep(member);
    };
    for (auto &decl : program_.declarations) sweep(decl);
  }

  void residualScan() {
    std::set<const Expr *> seen;
    const auto scanDecl = [&](this auto &&scanDecl,
                              const Decl &decl) -> void {
      for (const ExprPtr *slot :
           {&decl.initializer, &decl.body, &decl.guard, &decl.trigger,
            &decl.deadline})
        if (*slot) residualExpr(**slot, {}, seen);
      for (const auto &contract : decl.contracts)
        if (contract.condition)
          residualExpr(*contract.condition, {}, seen);
      for (const auto &clause : decl.goalClauses)
        if (clause.condition)
          residualExpr(*clause.condition, {}, seen);
      for (const auto &member : decl.members) scanDecl(member);
    };
    for (const auto &decl : program_.declarations) scanDecl(decl);
  }

  // `run`/`reify` under `std:meta` are the evaluated positions that DO
  // receive a fragment (§6.1): the operand literal reaches checking —
  // where the named Code-parameter refusal, arity and element-type
  // arms live — so the splice-only gate does not fire on it.
  bool metaFormOperand(std::string_view name,
                       const std::optional<std::string> &qualifier) const {
    if (name != "run" && name != "reify") return false;
    for (const auto &imported : program_.imports) {
      if (imported.path != "std:meta") continue;
      const bool selected =
          imported.symbols.empty() ||
          std::ranges::find(imported.symbols, name) !=
              imported.symbols.end();
      const bool qualified =
          !qualifier || *qualifier == imported.alias.value_or("std:meta");
      if (selected && qualified) return true;
    }
    return false;
  }

  // Whether @p child is the operand of @p parent — the first argument
  // of a `run`/`reify` special-form call.
  bool runOperandOf(const Expr &parent, const Expr &child) const {
    if (parent.kind != ExprKind::Call || parent.children.size() < 2 ||
        parent.children[1].get() != &child || !parent.children.front())
      return false;
    const Expr &callee = *parent.children.front();
    if (callee.kind == ExprKind::Identifier)
      return metaFormOperand(callee.text, std::nullopt);
    if (callee.kind == ExprKind::Field && !callee.children.empty() &&
        callee.children.front()->kind == ExprKind::Identifier)
      return metaFormOperand(callee.text,
                             callee.children.front()->text);
    return false;
  }

  void residualExpr(const Expr &node, const LiteralChain &chain,
                    std::set<const Expr *> &seen,
                    const bool runOperand = false) {
    if (!seen.insert(&node).second) return;
    switch (node.kind) {
    case ExprKind::Splice:
      // A splice already refused or left pending reported at its own
      // judgment — the residual arm covers only splices that produced
      // no diagnostic of their own.
      if (spliceReported_.contains(&node)) return;
      report(Diagnostic{
                 DiagnosticCode::InvalidDeclaration,
                 "a 'splice' that reaches checking is a compile error — "
                 "its operand is not a statically-known fragment",
                 node.loc},
             chain);
      return;
    case ExprKind::Code: {
      const FragmentKey key{node.loc,
                            node.stamp ? node.stamp->ordinal
                                       : std::nullopt};
      const auto meta = metas_.find(key);
      if (meta != metas_.end()) {
        if (meta->second.spliceOnly && !runOperand)
          report(Diagnostic{
                     DiagnosticCode::InvalidPosition,
                     "a Code-parameterized literal is splice-only — it "
                     "may appear only as a splice or template actual, "
                     "never in an evaluated position",
                     node.loc},
                 chain);
        for (const auto &alias : meta->second.captures) {
          const auto &capture = captures_.at(alias);
          const auto decl = declScope_.find(capture.binding.scope);
          if (decl != declScope_.end() && templates_.contains(decl->second))
            report(Diagnostic{
                       DiagnosticCode::InvalidDeclaration,
                       "capture '" + capture.captured +
                           "' binds a template parameter — it exists "
                           "only at expansion time",
                       node.loc},
                   chain);
        }
      }
      const auto record = fragments_.find(key);
      if (record != fragments_.end() && record->second.body.body) {
        LiteralChain inner = chain;
        inner.literals.push_back(node.loc);
        residualExpr(*record->second.body.body, inner, seen);
      }
      return;
    }
    default:
      break;
    }
    for (const auto &child : node.children)
      if (child)
        residualExpr(*child, chain, seen, runOperandOf(node, *child));
    for (const ExprPtr *meta : {&node.scope, &node.recipients,
                                &node.deadline, &node.guard,
                                &node.recovery})
      if (*meta) residualExpr(**meta, chain, seen);
  }

  Program &program_;
  LiteralSeeds &seeds_;
  ExpandLimits limits_;
  FragmentTable fragments_;
  std::map<FragmentKey, FragmentMeta> metas_;
  std::map<std::string, CaptureRecord, std::less<>> captures_;
  std::vector<SynthRecord> synthesized_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t splices_{0};
  std::size_t capCounter_{0};
  std::size_t synthCounter_{0};
  // (binding scope, name) → the let node minted there.
  std::map<std::pair<std::uint64_t, std::string>, Expr *> lets_;
  std::unordered_map<const Expr *, Expr *> letBlock_;
  std::set<Expr *> consumedLets_;
  std::unordered_map<const Expr *, ExprPtr *> parentSlot_;
  std::map<std::uint64_t, const Decl *> declScope_;
  std::vector<const Decl *> allDecls_;
  // Splices that produced a diagnostic at their own judgment — the
  // residual scan must not re-report them.
  std::set<const Expr *> spliceReported_;
  // §5.3's single compile-wide counter: one binding → one cap$N alias,
  // reused across every literal and substituted body so a cap$0 inside a
  // clone can never cross-resolve an unrelated cap$0.
  std::map<BindingId, const CaptureRecord *> capByBinding_;
  std::map<const Decl *, const Decl *> owners_;
  std::map<const Decl *, std::map<std::string, const Decl *, std::less<>>>
      methods_;
  std::set<const Decl *> templates_;
  std::set<const Decl *> finalized_;
  std::vector<const Decl *> expanding_;
  std::vector<DeferredCall> deferred_;
};

} // namespace

std::expected<Expansion, std::vector<Diagnostic>>
expand(Program &program, LiteralSeeds &seeds, ExpandLimits limits) {
  return Expander(program, seeds, limits).run();
}

} // namespace xanadu::vagent
