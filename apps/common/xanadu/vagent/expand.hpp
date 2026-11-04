#ifndef COMMON_XANADU_VAGENT_EXPAND_HPP
#define COMMON_XANADU_VAGENT_EXPAND_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/resolve.hpp"

namespace xanadu::vagent {

struct ExpandLimits {
  ParseLimits parse{};
  ResolveLimits resolve{};
  // The §5.6 per-compile counters (maxExpansionDepth/maxSplices) live in
  // CheckLimits so the stage and its consumer share one budget type.
  CheckLimits check{};
};

// §5.3's capture env record — one per cap$N alias minted in a compile,
// shared across all literals and all substituted bodies so two captures
// of one binding within one literal share the alias while captures in
// different bodies never collide. The alias renames the body's outside
// references in place (the Identifier keeps the captured binding's own
// ScopeStamp — that stamp, not this record, is the resolution channel);
// `anchorAlias` is the cells"…" $-anchor spelling of the same alias —
// vql's `$var` charset is [alnum_] and cannot carry a '$', so anchors
// are rewritten to `cap_N` where identifiers read `cap$N`. `stable`
// marks the state-side class (a field, member var, agent parameter or
// implicit name — a binding minted in the owner scope): the class that
// counts toward FragmentRecord::closed and the only class a cells"…"
// $-anchor may name. `captured` is the one-hop spelling the renamed
// reference carried (it may itself be an outer body's alias — the
// checker chases the chain), `literal` the introducing literal's own
// loc.
struct CaptureRecord {
  std::string alias;
  std::string anchorAlias;
  std::string captured;
  ScopeStamp binding{};
  bool stable{false};
  SourceLocation literal{};
  SourceLocation loc{};
};

// Per-literal expand metadata — the half of a fragment-table record that
// is expand-internal rather than checker-facing (checker.hpp's
// FragmentRecord carries the body, `closed` and the §5.3 `env` records
// the checker seeds aliases from). `scopeWatermark` is the shared-id
// watermark taken immediately before the body's resolveFragment call:
// binding stamps with scope above it were minted inside this body, so
// splice substitution can re-stamp a clone's internals without touching
// capture stamps. `paramScope` is the canonical scope id expand mints
// for the body's declared parameters — references to them carry
// (paramScope, index), which is what lets `splice p` and `x := p` inside
// the body resolve at instantiation sites without re-resolving names.
// `spliceOnly` is §5.4's Code-parameterized class — the literal may
// appear only as a splice or template actual, never in an evaluated
// position, and emits no head and no descriptor.
struct FragmentMeta {
  std::uint64_t scopeWatermark{0};
  std::uint64_t paramScope{~0ull};
  bool spliceOnly{false};
  // The body references an enclosing fragment body's parameter by name —
  // a frame-bound outside reference the alias map cannot express (the
  // outer param carries no binding stamp), which still clears `closed`.
  bool outerParamRefs{false};
  // Aliases minted by this literal, in mint order — the per-fragment
  // outside-reference list closed classification and, later, learn's
  // re-verification read.
  std::vector<std::string> captures;
};

// One synthesized call-by-value binding at a substitution head (§5.4's
// `{ let p0 := count; p0 + 1 }` shape — a `let p<k> := <arg>` evaluated
// once, in argument order). `stamp` is the fresh binding identity the
// re-pointed parameter references carry; `parameter` is the source
// parameter name they spelled.
struct SynthRecord {
  ScopeStamp stamp{};
  std::string name;
  std::string parameter;
  SourceLocation loc{};
};

// What the expand stage leaves for check() and the lowering pass: the
// merged Program is transformed in place (splices substituted, template
// calls inlined, dead lets erased, template defs removed); `fragments`
// is the §5.6 fragment table keyed by literal loc+ordinal — a record's
// body is re-cloned per emission because kernel tables key on
// `const Expr *`. The side tables are expand's bookkeeping surface:
// `captures` is the {cap$N → captured-name} env-record set §5.3 inserts
// at each literal's position, `synthesized` the p<k> bindings minted at
// substitution heads, and `fragmentMeta` the per-literal internals.
struct Expansion {
  FragmentTable fragments;
  std::map<FragmentKey, FragmentMeta> fragmentMeta;
  std::map<std::string, CaptureRecord, std::less<>> captures;
  std::vector<SynthRecord> synthesized;
};

// §5.6 stage 3 — the Program → Program composition pass between
// resolve-only stamping and check(). Per code"…" literal: parseCodePath
// parses and narrows the body, the literal's LiteralSeed resumes
// binding-identity resolution inside it via resolveFragment, and a
// record lands in the fragment table under the literal's FragmentKey —
// nested literals through the same recursion. Then, over resolve's
// stamps: outside value bindings referenced inside a body are renamed
// to fresh cap$N aliases (capture records minted per §5.3 — record.env
// gets {alias → captured spelling}, cells"…" $-anchors rewrite to the
// anchor spelling), each `splice` whose operand is statically known
// substitutes a fresh cloneExpr of the fragment body with call-by-value
// `let p<k>` argument bindings at its head, and a def that splices a
// direct Code<…> parameter is expanded per call site as a template
// (M13). `static` passes through at program level for the checker's
// §5.5 arm; inside a code"…" body it stays a named refusal. A residual
// `splice` — an operand that is not a statically-known fragment — is a
// named DiagnosticCode refusal.
[[nodiscard]] std::expected<Expansion, std::vector<Diagnostic>>
expand(Program &program, LiteralSeeds &seeds, ExpandLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_EXPAND_HPP
