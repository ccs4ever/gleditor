#include <algorithm>
#include <cstdint>
#include <expected>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/expand.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/resolve.hpp"

namespace {

using namespace xanadu::vagent;

std::string describe(const std::vector<Diagnostic> &diagnostics) {
  std::string out;
  for (const auto &diagnostic : diagnostics) {
    if (!out.empty()) out += '\n';
    out += formatDiagnostic(diagnostic);
  }
  return out;
}

// parse → resolve → expand: the three pipeline stages check() sees, in
// test-visible form. On an expand refusal the diagnostics are what is
// asserted.
struct Expanded {
  Program program;
  LiteralSeeds seeds;
  Expansion expansion;
};

std::expected<Expanded, std::vector<Diagnostic>>
runExpand(std::string_view source, ExpandLimits limits = {}) {
  Parser parser(source, limits.parse);
  auto program = parser.parseProgram();
  if (!program) return std::unexpected(std::vector{program.error()});
  LiteralSeeds seeds;
  auto resolved = resolve(*program, &seeds, limits.resolve);
  if (!resolved) return std::unexpected(resolved.error());
  auto expansion = expand(*program, seeds, limits);
  if (!expansion) return std::unexpected(expansion.error());
  return Expanded{std::move(*program), std::move(seeds), std::move(*expansion)};
}

std::vector<Diagnostic> diagnostics(std::string_view source,
                                    ExpandLimits limits = {}) {
  auto expanded = runExpand(source, limits);
  if (expanded) return {};
  return expanded.error();
}

void rejects(std::string_view source, DiagnosticCode code,
             ExpandLimits limits = {}) {
  const auto errors = diagnostics(source, limits);
  EXPECT_TRUE(std::ranges::any_of(
      errors, [code](const Diagnostic &error) { return error.code == code; }))
      << "expected diagnostic code " << static_cast<int>(code) << " in:\n"
      << describe(errors);
}

const Expr *findKind(const Expr &node, ExprKind kind) {
  if (node.kind == kind) return &node;
  for (const auto &child : node.children)
    if (child)
      if (const auto *hit = findKind(*child, kind)) return hit;
  for (const ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                              &node.guard, &node.recovery})
    if (*meta)
      if (const auto *hit = findKind(**meta, kind)) return hit;
  return nullptr;
}

const Expr *findKind(const Decl &decl, ExprKind kind) {
  for (const ExprPtr *slot : {&decl.initializer, &decl.body, &decl.guard,
                              &decl.trigger, &decl.deadline})
    if (*slot)
      if (const auto *hit = findKind(**slot, kind)) return hit;
  for (const auto &contract : decl.contracts)
    if (contract.condition)
      if (const auto *hit = findKind(*contract.condition, kind)) return hit;
  for (const auto &clause : decl.goalClauses)
    if (clause.condition)
      if (const auto *hit = findKind(*clause.condition, kind)) return hit;
  for (const auto &member : decl.members)
    if (const auto *hit = findKind(member, kind)) return hit;
  return nullptr;
}

const Expr *findKind(const Program &program, ExprKind kind) {
  for (const auto &decl : program.declarations)
    if (const auto *hit = findKind(decl, kind)) return hit;
  return nullptr;
}

// Every Identifier/LogicVariable/Let spelling in a tree — what 'cap$N'
// and 'p<k>' presence checks read.
void collectNames(const Expr &node, std::set<std::string> &names) {
  if (node.kind == ExprKind::Identifier ||
      node.kind == ExprKind::LogicVariable || node.kind == ExprKind::Let)
    names.insert(node.text);
  for (const auto &child : node.children)
    if (child) collectNames(*child, names);
  for (const ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                              &node.guard, &node.recovery})
    if (*meta) collectNames(**meta, names);
}

void collectNames(const Decl &decl, std::set<std::string> &names) {
  for (const ExprPtr *slot : {&decl.initializer, &decl.body, &decl.guard,
                              &decl.trigger, &decl.deadline})
    if (*slot) collectNames(**slot, names);
  for (const auto &contract : decl.contracts)
    if (contract.condition) collectNames(*contract.condition, names);
  for (const auto &clause : decl.goalClauses)
    if (clause.condition) collectNames(*clause.condition, names);
  for (const auto &member : decl.members) collectNames(member, names);
}

std::set<std::string> names(const Program &program) {
  std::set<std::string> out;
  for (const auto &decl : program.declarations) collectNames(decl, out);
  return out;
}

const FragmentRecord *onlyRecord(const FragmentTable &table) {
  if (table.size() != 1) return nullptr;
  return &table.begin()->second;
}

// The decl carrying a 'let <name>' anywhere in its walked expressions —
// how the tests locate the handler/method a splice expanded in without
// depending on member order.
const Decl *declContainingLet(const Program &program, std::string_view name) {
  const Decl *found = nullptr;
  const auto scan   = [&](this auto &&scan, const Decl &decl) -> void {
    if (found) return;
    const auto visit = [&](this auto &&visit, const Expr &node) -> void {
      if (node.kind == ExprKind::Let && node.text == name) found = &decl;
      for (const auto &child : node.children)
        if (child) visit(*child);
      for (const ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                                  &node.guard, &node.recovery})
        if (*meta) visit(**meta);
    };
    for (const ExprPtr *slot : {&decl.initializer, &decl.body, &decl.guard,
                                &decl.trigger, &decl.deadline})
      if (*slot) visit(**slot);
    for (const auto &member : decl.members) scan(member);
  };
  for (const auto &decl : program.declarations) scan(decl);
  return found;
}

// ---- §9.1: the worked example ------------------------------------------

TEST(VagentExpandTest, ParameterizedFragmentSplicesCallByValue) {
  auto expanded = runExpand(R"VA(
    event Tick
    agent Counter {
      var count: Int := 0
      on Tick {
        let inc := code"n: Int | n + 1"
        let next := splice inc(count)
        count := next
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  // §9.1's shape: { let p0 := count; p0 + 1 } — the argument evaluated
  // once at the head, the parameter references re-pointed.
  const auto programNames = names(expanded->program);
  EXPECT_FALSE(programNames.contains("inc")); // §5.4 residue erasure
  EXPECT_TRUE(programNames.contains("p0"));
  EXPECT_TRUE(programNames.contains("next"));
  // The substituted block sits as `next`'s initializer.
  const Decl *handler = declContainingLet(expanded->program, "next");
  ASSERT_NE(handler, nullptr);
  const Expr *block = nullptr;
  const auto scan   = [&](this auto &&scan, const Expr &node) -> void {
    if (node.kind == ExprKind::Let && node.text == "next" &&
        !node.children.empty() &&
        node.children.front()->kind == ExprKind::Block)
      block = node.children.front().get();
    for (const auto &child : node.children)
      if (child) scan(*child);
  };
  for (const ExprPtr *slot : {&handler->body, &handler->initializer})
    if (*slot) scan(**slot);
  ASSERT_NE(block, nullptr);
  ASSERT_GE(block->children.size(), 2U);
  const Expr &head = *block->children.front();
  ASSERT_EQ(head.kind, ExprKind::Let);
  EXPECT_EQ(head.text, "p0");
  ASSERT_FALSE(head.children.empty());
  EXPECT_EQ(head.children.front()->text, "count");
  // The synthesized binding record names the source parameter.
  ASSERT_FALSE(expanded->expansion.synthesized.empty());
  EXPECT_EQ(expanded->expansion.synthesized.front().parameter, "n");
}

TEST(VagentExpandTest, SpliceOfALiteralSubstitutesDirectly) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int { splice code"n: Int | n + 1"(count) }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  EXPECT_EQ(findKind(expanded->program, ExprKind::Splice), nullptr);
  const auto *synth = findKind(expanded->program, ExprKind::Let);
  ASSERT_NE(synth, nullptr);
  EXPECT_EQ(synth->text, "p0");
}

// ---- §5.3: cap$N capture hygiene ---------------------------------------

TEST(VagentExpandTest, CapturesRenameToCapAliasesKeepingBindingStamps) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int {
        let x := 7
        let c := code"x + count + x"
        0
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto *record = onlyRecord(expanded->expansion.fragments);
  ASSERT_NE(record, nullptr);
  // One alias per captured binding, reused on the repeated reference.
  EXPECT_EQ(record->env.size(), 2U);
  const auto &captures = expanded->expansion.captures;
  ASSERT_EQ(captures.size(), 2U);
  const CaptureRecord &first  = captures.at("cap$0");
  const CaptureRecord &second = captures.at("cap$1");
  EXPECT_EQ(first.captured, "x");
  EXPECT_FALSE(first.stable); // a frame-bound let clears `closed`
  EXPECT_EQ(second.captured, "count");
  EXPECT_TRUE(second.stable); // an owner-scope field is state-side
  // The body's references carry the captured bindings' own stamps.
  std::set<std::string> spellings;
  collectNames(*record->body.body, spellings);
  EXPECT_TRUE(spellings.contains(first.alias));
  EXPECT_TRUE(spellings.contains(second.alias));
  EXPECT_FALSE(spellings.contains("x"));
  EXPECT_FALSE(spellings.contains("count"));
  const auto *renamed = findKind(*record->body.body, ExprKind::Identifier);
  ASSERT_NE(renamed, nullptr);
  ASSERT_TRUE(renamed->stamp.has_value());
  EXPECT_TRUE(renamed->stamp->ordinal.has_value());
  // The frame-bound capture makes the literal not closed.
  EXPECT_FALSE(record->closed);
}

TEST(VagentExpandTest, StableOnlyCaptureKeepsClosed) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int { let c := code"count + 1"; 0 }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto *record = onlyRecord(expanded->expansion.fragments);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->closed);
  ASSERT_EQ(record->env.size(), 1U);
  EXPECT_EQ(record->env.begin()->first, "cap$0");
  EXPECT_EQ(record->env.begin()->second, "count");
}

TEST(VagentExpandTest, NestedLiteralCapturesTheEnclosingScope) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int {
        let x := 3
        let c := code"code\"x + 1\""
        0
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  ASSERT_EQ(expanded->expansion.fragments.size(), 2U);
  // The inner literal captured the enclosing let — a non-stable
  // (frame-bound) capture, so the inner record is not closed while the
  // outer one (a bare nested literal) stays closed.
  const FragmentRecord *inner = nullptr;
  for (const auto &[key, record] : expanded->expansion.fragments)
    if (record.body.body &&
        findKind(*record.body.body, ExprKind::Binary) != nullptr)
      inner = &record;
  ASSERT_NE(inner, nullptr);
  EXPECT_FALSE(inner->closed);
  ASSERT_EQ(inner->env.size(), 1U);
  EXPECT_EQ(inner->env.begin()->second, "x");
}

// ---- cells"…" $-anchor rewriting ----------------------------------------

TEST(VagentExpandTest, CellAnchorsRewriteToTheAnchorSpelling) {
  auto expanded = runExpand(R"VA(
    agent A {
      var state: Cell := cells"##"
      def f(): Int { let c := code"cells\"$state\""; 0 }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto *record = onlyRecord(expanded->expansion.fragments);
  ASSERT_NE(record, nullptr);
  const auto *cells = findKind(*record->body.body, ExprKind::Cells);
  ASSERT_NE(cells, nullptr);
  // vql's $var charset carries no '$' — identifiers read cap$N, anchors
  // read cap_N; the env record names both through the one alias.
  EXPECT_EQ(cells->text, "$cap_0");
  ASSERT_EQ(record->env.size(), 1U);
  EXPECT_EQ(record->env.begin()->first, "cap$0");
  EXPECT_EQ(record->env.begin()->second, "state");
  const auto &capture = expanded->expansion.captures.at("cap$0");
  EXPECT_EQ(capture.anchorAlias, "cap_0");
  EXPECT_TRUE(capture.stable);
}

TEST(VagentExpandTest, FragmentMetaCarriesSpliceOnlyAndParamScope) {
  auto expanded = runExpand(R"VA(
    agent A {
      def f(): Int {
        splice code"p: Code<Int>, n: Int | splice p + n"(code"1", 2)
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  // The parameterized actual is splice-only (a Code parameter makes the
  // literal compositional only) and its declared parameters carry the
  // canonical (paramScope, index) stamps splice judges resolve through.
  const FragmentMeta *spliceOnly = nullptr;
  const FragmentRecord *owned    = nullptr;
  for (const auto &[key, meta] : expanded->expansion.fragmentMeta)
    if (meta.spliceOnly) {
      spliceOnly = &meta;
      owned      = &expanded->expansion.fragments.at(key);
    }
  ASSERT_NE(spliceOnly, nullptr);
  EXPECT_NE(spliceOnly->paramScope, ~0ull);
  ASSERT_NE(owned->body.body, nullptr);
  const auto *splice = findKind(*owned->body.body, ExprKind::Splice);
  ASSERT_NE(splice, nullptr); // resolves per-site inside the clone
  ASSERT_FALSE(splice->children.empty());
  const Expr &operand = *splice->children.front();
  ASSERT_TRUE(operand.stamp.has_value());
  EXPECT_EQ(operand.stamp->scope, spliceOnly->paramScope);
  ASSERT_TRUE(operand.stamp->ordinal.has_value());
  EXPECT_EQ(*operand.stamp->ordinal, 0U);
}

TEST(VagentExpandTest, FrameBoundAnchorsAreRefused) {
  rejects(R"VA(agent A {
             def f(target: Cell): Int {
               let c := code"cells\"$target\""
               0
             }
           })VA",
          DiagnosticCode::InvalidPosition);
}

// ---- splice substitution -------------------------------------------------

TEST(VagentExpandTest, CallByValueEvaluatesEachArgumentOnceInOrder) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int {
        splice code"x: Int, y: Int | x * y"(count, 3)
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  ASSERT_EQ(expanded->expansion.synthesized.size(), 2U);
  EXPECT_EQ(expanded->expansion.synthesized[0].parameter, "x");
  EXPECT_EQ(expanded->expansion.synthesized[1].parameter, "y");
  const auto programNames = names(expanded->program);
  EXPECT_TRUE(programNames.contains("p0"));
  EXPECT_TRUE(programNames.contains("p1"));
}

TEST(VagentExpandTest, SpliceInsideABodySubstitutesThroughTheCapture) {
  auto expanded = runExpand(R"VA(
    agent A {
      def f(): Int {
        let inner := code"n: Int | n + 1"
        let outer := code"splice inner(2)"
        splice outer
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  EXPECT_EQ(findKind(expanded->program, ExprKind::Splice), nullptr);
  // The merged statement carries the fully substituted shape —
  // { let p0 := 2; p0 + 1 } — and no record body keeps a `splice`.
  for (const auto &[key, record] : expanded->expansion.fragments)
    if (record.body.body)
      EXPECT_EQ(findKind(*record.body.body, ExprKind::Splice), nullptr)
          << "record at " << key.loc.line << ":" << key.loc.column;
}

TEST(VagentExpandTest, SpliceOfASpliceOnlyActualComposes) {
  auto expanded = runExpand(R"VA(
    agent A {
      def f(): Int {
        splice code"p: Code<Int> | splice p + 1"(code"2")
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto *binary = findKind(expanded->program, ExprKind::Binary);
  ASSERT_NE(binary, nullptr);
  EXPECT_EQ(binary->text, "+");
}

TEST(VagentExpandTest, ResidualSpliceIsANamedRefusal) {
  rejects(R"VA(agent A {
             var count: Int := 0
             def f(): Int { splice count }
           })VA",
          DiagnosticCode::InvalidDeclaration);
  rejects(R"VA(agent A {
             def f(): Int { splice whatever }
           })VA",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentExpandTest, SpliceArityMismatchIsATypeError) {
  rejects(R"VA(agent A {
             def f(): Int { splice code"x: Int | x"(1, 2) }
           })VA",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentExpandTest, SpliceIsNeverAPlace) {
  rejects(R"VA(agent A {
             def f(): Int { splice code"1" := 3 }
           })VA",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentExpandTest, SpliceOnlyLiteralRefusedInEvaluatedPositions) {
  rejects(R"VA(agent A {
             def f(): Int { let c := code"p: Code<Int> | 0"; 0 }
           })VA",
          DiagnosticCode::InvalidPosition);
}

// ---- dead-let erasure ------------------------------------------------------

TEST(VagentExpandTest, ConsumedLetChainsEraseTransitively) {
  auto expanded = runExpand(R"VA(
    agent A {
      def f(): Int {
        let f1 := code"1"
        let f2 := f1
        splice f2
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto programNames = names(expanded->program);
  EXPECT_FALSE(programNames.contains("f1"));
  EXPECT_FALSE(programNames.contains("f2"));
}

// ---- templates (M13)
// ---------------------------------------------------------

TEST(VagentExpandTest, CodeSplicingDefExpandsAtCallSites) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def wrap(c: Code<Int>): Int { splice c }
      def f(): Int { wrap(code"count + 1") }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto programNames = names(expanded->program);
  // The template def leaves the merged program entirely.
  EXPECT_EQ(findKind(expanded->program, ExprKind::Call), nullptr);
  const auto *binary = findKind(expanded->program, ExprKind::Binary);
  ASSERT_NE(binary, nullptr);
  EXPECT_EQ(binary->text, "+");
  // The inlined body names its captures back at program level — the
  // alias resolves only inside record envs.
  EXPECT_FALSE(programNames.contains("cap$0"));
  EXPECT_TRUE(programNames.contains("count"));
}

TEST(VagentExpandTest, TemplateCallsBeforeTheDefinitionExpandDeferred) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int { wrap(code"count + 1") }
      def wrap(c: Code<Int>): Int { splice c }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  EXPECT_EQ(findKind(expanded->program, ExprKind::Call), nullptr);
}

TEST(VagentExpandTest, OpaqueCodeActualsRefuseAtTheCallSite) {
  rejects(R"VA(agent A {
             var count: Int := 0
             def wrap(c: Code<Int>): Int { splice c }
             def f(): Int { wrap(count) }
           })VA",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentExpandTest, RecursiveTemplateCyclesAreRefused) {
  // A cycle only enters expanding_ once every Code actual along it is
  // statically known — opaque-actual calls stay pending instead — so
  // the refusal needs literal arguments on both legs.
  rejects(R"VA(agent A {
             def a(c: Code<Int>): Int { splice c; a(code"1") }
           })VA",
          DiagnosticCode::InvalidDeclaration);
  rejects(R"VA(agent A {
             def a(c: Code<Int>): Int { splice c; b(code"1") }
             def b(c: Code<Int>): Int { splice c; a(code"1") }
           })VA",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentExpandTest, ControlFlowBarsRefuseTemplates) {
  rejects(R"VA(agent A {
             def wrap(c: Code<Int>): Int { splice c; return 1 }
           })VA",
          DiagnosticCode::InvalidPosition);
  // The declaration bars: an effect method exists at run time — a
  // template exists only at expansion time. (Capacity methods are
  // signature-only, so they can never splice in the first place.)
  rejects(R"VA(agent A {
             effect def wrap(c: Code<Int>): Int { splice c }
           })VA",
          DiagnosticCode::InvalidDeclaration);
}

// ---- static
// --------------------------------------------------------------------

TEST(VagentExpandTest, StaticPassesThroughAtProgramLevel) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int { static (60 * 1000) + count }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  EXPECT_NE(findKind(expanded->program, ExprKind::Static), nullptr);
}

TEST(VagentExpandTest, StaticInsideABodyIsANamedInnerRefusal) {
  const auto errors = diagnostics(R"VA(
    agent A {
      def f(): Int { let c := code"static (1 + 1)"; 0 }
    }
  )VA");
  ASSERT_FALSE(errors.empty());
  const auto &error = errors.front();
  EXPECT_EQ(error.code, DiagnosticCode::Unsupported);
  // §5.1's inner-offset convention: the diagnostic locates inside the
  // literal's text.
  EXPECT_FALSE(error.inner.empty());
}

// ---- expansion limits
// --------------------------------------------------------------

TEST(VagentExpandTest, SpliceBudgetIsEnforced) {
  ExpandLimits limits;
  limits.check.maxSplices = 1;
  rejects(R"VA(agent A {
             def f(): Int { { splice code"1"; splice code"2" } }
           })VA",
          DiagnosticCode::InvalidDeclaration, limits);
}

TEST(VagentExpandTest, LiteralNestingDepthIsEnforced) {
  ExpandLimits limits;
  limits.check.maxExpansionDepth = 1;
  rejects(R"VA(agent A {
             def f(): Int { let c := code"code\"1\""; 0 }
           })VA",
          DiagnosticCode::InvalidDeclaration, limits);
}

TEST(VagentExpandTest, SubstitutionDepthIsEnforced) {
  ExpandLimits limits;
  limits.check.maxExpansionDepth = 2;
  rejects(R"VA(agent A {
             def f(): Int {
               let inner := code"1"
               let outer := code"splice inner"
               splice outer
             }
           })VA",
          DiagnosticCode::InvalidDeclaration, limits);
}

// ---- scope/binding identity
// ----------------------------------------------------------

TEST(VagentExpandTest, TwoSitesMintDistinctInternalScopes) {
  auto expanded = runExpand(R"VA(
    agent A {
      def f(): Int {
        let inc := code"n: Int | { let m := n; m + 1 }"
        { splice inc(1); splice inc(2) }
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  // Each site's cloned `let m` carries a fresh scope id — the two
  // sites' locals never share a binding identity.
  std::set<std::uint64_t> letScopes;
  const auto visit = [&](this auto &&visit, const Expr &node) -> void {
    if (node.kind == ExprKind::Let && node.text == "m" && node.stamp)
      letScopes.insert(node.stamp->scope);
    for (const auto &child : node.children)
      if (child) visit(*child);
    for (const ExprPtr *meta : {&node.scope, &node.recipients, &node.deadline,
                                &node.guard, &node.recovery})
      if (*meta) visit(**meta);
  };
  const auto visitDecl = [&](this auto &&visitDecl, const Decl &decl) -> void {
    for (const ExprPtr *slot : {&decl.initializer, &decl.body, &decl.guard,
                                &decl.trigger, &decl.deadline})
      if (*slot) visit(**slot);
    for (const auto &member : decl.members) visitDecl(member);
  };
  for (const auto &decl : expanded->program.declarations) visitDecl(decl);
  EXPECT_EQ(letScopes.size(), 2U);
}

// ---- checker plumbing
// ----------------------------------------------------------------

TEST(VagentExpandTest, ExpandedProgramChecksClean) {
  auto expanded = runExpand(R"VA(
    event Tick
    agent Counter {
      var count: Int := 0
      on Tick {
        let inc := code"n: Int | n + 1"
        let next := splice inc(count)
        count := next
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto checked =
      check(expanded->program, expanded->expansion.fragments, {});
  EXPECT_TRUE(checked.has_value())
      << (checked ? "" : describe(checked.error()));
}

TEST(VagentExpandTest, RecordBodiesKeepAliasesAndCheckClean) {
  auto expanded = runExpand(R"VA(
    agent A {
      var count: Int := 0
      def f(): Int {
        let x := 7
        let c := code"x + count"
        0
      }
    }
  )VA");
  ASSERT_TRUE(expanded.has_value()) << describe(expanded.error());
  const auto checked =
      check(expanded->program, expanded->expansion.fragments, {});
  EXPECT_TRUE(checked.has_value())
      << (checked ? "" : describe(checked.error()));
}

} // namespace
