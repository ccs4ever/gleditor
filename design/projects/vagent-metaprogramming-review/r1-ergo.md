# Round 1 critique — Programming Language Design & Ergonomics

Scope: surface syntax, grammar ambiguity, static semantics, type/effect discipline, scope and
binding, failure surface, family consistency, developer ergonomics. Cross-checked against
`design/vagent-agent-language.md` (§8.1–§8.9 grammar and lexical rules), the vagent frontend
(`vagent/lexer.cpp`, `token.cpp`, `parser.cpp`, `checker.cpp`, `ast.hpp`, `compiler.hpp/.cpp`), and
the sibling docs for family conventions.

## What verified cleanly (no finding)

- **The lexer merge works as claimed.** `lexer.cpp:379-398` merges any `Identifier` immediately
  followed by `"` into one `PrefixedStringLiteral`; `isReservedWord` (`token.cpp:8-47`) marks only
  the §8.1 reserved list as `TokenKind::Keyword`, so contextual words — `cells` today, `code`
  tomorrow — are lexed as `Identifier` and merge. A *variable* named `code` creates no ambiguity:
  `code"…"` is always the literal, and `code "…"` (with a space) is two tokens that cannot form an
  expression anyway. The parser's hardcoded `!= "cells"` check (`parser.cpp:851`) must gain `code`,
  which §5.6's "the parser owns prefix validation" covers.
- **`static` and `splice` are not keyword collisions.** Neither is reserved in vagent (§8.1), and
  neither appears as syntax in vlace, vicon, or VQL. The contextual-keyword route matches the
  `succeed`/`holds`/`as` precedent, and `acceptWord` (`parser.cpp:141-146`) already matches by
  spelling.
- **`'splice' Postfix` and `'static' Unary` slot into `Primary` without grammatical ambiguity**;
  statement-position `splice` is just an expression statement, and `emit`'s `for`/`in`/`by`/`else`
  tails cannot be confused with it. `?` logic variables, `for`, `emit`, `believe` inside
  `code"…"` bodies are ordinary vagent grammar — no new ambiguity.
- **Hygiene by definition-site binding is sound for `splice`.** Because the operand must be a
  `let`-bound literal in scope or a `def` `code` parameter expanded at a call site, every captured
  binding is provably live at the expansion site (lexical nesting does the work) — see finding 6
  for why this deserves to be stated.

## Findings

### 1. MAJOR — §5.2: `def` parameters appear on both the allowed and refused `splice` lists

**Problem.** Paragraph one of the `splice` rules says `c` may name "a `code` parameter of a `def`
whose expansion is itself being checked"; three paragraphs later the same construct — "a `def`
parameter" — heads the list of Code values that are *not* statically known and are "a check error".
Both cannot hold. The distinction the text is reaching for (a `Code`-typed formal usable while its
owning `def` is being expanded per call site, versus a `Code` value received as data) is never
stated, and with it the entire staging model for `def`s with `Code` parameters is missing: are
such `def`s expanded at each call site (templates), are they barred from `effect`/`native`/capacity
membership and from being `graft_method` targets, and what budgets recursive expansion?

**Evidence.** `design/vagent-metaprogramming.md` §5.2 (lines 424-431). `MethodSig`/`ParamList`
permit `c: Code<Int>` with no marker (host grammar, `vagent-agent-language.md:1313,1392`).

**Fix.** Either (a) delete `code` parameters from the statically-known list — `splice` operands are
`let`-bound `code"…"` literals only — or (b) specify the template model in one paragraph: "`def`s
whose signature takes `Code<…>` are expanded at each call site rather than compiled once; they may
not be `effect`, `native`, capacity members, or `graft_method` targets; per-call expansion shares
the splice-depth budget." Then fix the refused list to read "a `Code` field, a `reflect`/`code_named`
result, or any `def` parameter when the `def` is not being expanded".

### 2. MAJOR — §5.2, §7.1, §6.3: composition re-verifies type `T` only; positional and purity
discipline is checked at the definition site and never at the splice site or graft point

**Problem.** `splice` "re-verifies only the splice-point fit: the position must accept `T`", and
§7.1 confirms position rules "apply inside `code"…"` — i.e., where the literal was written. But
vagent's legality rules are properties of the *position a body lands in*: `:=`/`return`/`consume`/
`terminate` need bounded positions, effect calls need plan suspension or bare bounded handler
positions, and conditions/`where`/guards must be pure. A fragment checked inside a handler body —
where `x := 3`, `terminate`, or a bounded effect call is legal — spliced into an operand
(`f(splice c)`), a method, or a `where` produces position-illegal merged code that nothing
re-walks. The runtime side has the same hole: `graft_where`'s signature check is "plan test: any
producing type" (§6.3) with no `pure` requirement, so an impure fragment can be installed as a
`where`/guard test, voiding §8.2's "conditions are pure" at a point the checker never sees.

**Evidence.** `design/vagent-metaprogramming.md` §5.2 (lines 424-436), §6.3 (lines 618-639), §7.1
(line 756). Position checks are context-driven in the checker: `:=` bounded at `checker.cpp:1234`,
`achieve`/`ask`/`await`/`yield` plan-suspension at `:1596-1714`, effect-call positions at
`:933-946`, `cells` mutating → `actionAllowed` at `:1416`. `validate` (§7.2) checks structure and
descriptor flags, not language positions.

**Fix.** (a) At `splice`, run the position/purity pass over the substituted AST in the site's
context (the machinery exists — `context.bounded`, `context.suspend`, `context.body`); or record a
position-requirement mask in the descriptor (`needs_bounded`, `has_effect_call`, `pure` exists) and
check it at expansion. (b) Per graft point, state the descriptor requirements: `graft_where` and
`graft_handler` tests must be `pure`; `graft_body`/`graft_plan` bodies must be `Code<()>` and may be
impure; `graft_method` must match the declared signature including `effect`-ness.

### 3. MAJOR — §5.5 / M5: `static` closedness is name-based and three holes follow

**Problem.** (a) The allowed free-name list includes "a `code` parameter" — but a fragment
parameter has no value until `splice`/`run` instantiation. `code"w: Int | w + static (w * 2)"`
cannot be folded "in the compile arena" at fragment-check time, and under `run` — where the graph
is emitted once, at compile — it can never fold; either it silently evaluates at run time
(violating §12's "run time never expands, parses or type-checks" and M5's determinism claim) or it
must be refused. (b) Closedness is defined over free *names*, so name-free arena reads slip
through: `cells"##/+d.x"` is `pureBody`-pure (non-mutating `Cells` returns pure,
`checker.cpp:857-861`) and has zero free names, so `static cells"##"` reads the *compile* arena and
"emits" a `Cell`/`[Cell]` literal naming cells that die with that arena — exactly the stale
session-handle failure §4.5 prices. `self`/`believes`/`findall`/`run` need to be excluded by the
same form-level rule, not left to name resolution luck. (c) "The result becomes a literal" never
bounds which types can be literals: `literal()` mints scalar `CellValue` cells, so record, list,
`Opt`, `Map`, `Cell`, `Code`, `Agent`, `Intention`, `Timer`, `Space` results have no emitted form.
And "a `static`-bound name" is referenced but no binding form is defined — is `let x := static e`
one, or is there a `static` declaration?

**Evidence.** `design/vagent-metaprogramming.md` §5.5 (lines 482-498), M5 (lines 172-180), §4.5
session-handle table (lines 359-372); `checker.cpp:827,857-861`; `ScalarCompilerLimits`
(`compiler.hpp:23-26`) where `staticFuel` is promised but unspecified as a binding form.

**Fix.** Rewrite the closedness rule as: every free name resolves to a literal or to a name bound
to a `static`/`let … := static …` (define the form); `code` parameters are not static-known —
refuse them inside `static` (or specify per-site deferred folding valid only under `splice`, with
such fragments refused by `run`/`graft`/`learn`); and forbid arena-reading *forms* outright —
`cells"…"`, `believes`, `findall`, the implicit names (`self`, `parent`, `inner`, `outer`,
`occurrence`), `run`, host/effect calls — alongside the purity gate. Restrict `static` result types
to the literal-expressible scalars and say so.

### 4. MAJOR — §5.4: `splice` argument binding is AST substitution (call-by-name); duplication and
elision of argument evaluation are unspecified and diverge from `run`

**Problem.** "The checker binds parameter names to the argument ASTs inside the fragment's checked
body" means each parameter *reference* re-evaluates the argument: `code"n: Int | n + n"` spliced as
`splice c(f())` inlines `f() + f()`; a parameter never referenced drops the argument's evaluation
entirely. `run(c, f())` evaluates `f()` once into the `#Call` frame. For an impure argument — a
`def` call that `believe`s, a `<-` write, a staged `emit` — `splice c(e)` and `run(c, e)` silently
differ in side-effect multiplicity and order, the classic macro trap, and §5.3's hygiene covers
name capture but not evaluation multiplicity or argument evaluation order.

**Evidence.** `design/vagent-metaprogramming.md` §5.4 (lines 469-478), §5.2 (line 440 "N sites mint
N graphs"); §9.1 (line 905) shows the intended inline-substitution reading.

**Fix.** Specify the calling convention. Recommended: bind each `splice` argument to a fresh `let`
local at the top of the expanded body — call-by-value, each argument evaluated once in argument
order — which matches `run`'s frame semantics and makes `splice`/`run` interchangeable on meaning,
differing only in cost. If call-by-name is intended, say so in §5.4 with a duplication warning and
restrict non-pure arguments to single-reference parameters.

### 5. MINOR — §5.1: `splice`/`static` callee names are shadowed, and the operand-vs-identifier
lookahead rule is unwritten

**Problem.** "They stay legal as identifiers elsewhere" is mostly true, but `splice(x)` always
parses as `splice` applied to `(x)` and `static(x)` as `static` applied to `(x)` — a `def` or
method named `splice`/`static` is uncallable as a bare call (`.splice()` still works, since any
`Word` follows `.`). Similarly, `splice` at end of line must fall back to `Identifier` (the §8.1
newline rules make this work), and `splice (x)`/`splice f(x)` need a stated rule: the operand is
whatever `Postfix` follows, so `splice f(x)` is splice-of-`f(x)`-the-value — a check error unless
`f` is a statically-known parameterized fragment, which also means `splice f(x)` can never mean
"splice the result of calling `f`". That last point deserves an explicit sentence because the
parenthesized form *looks* like an unquote call.

**Fix.** Add to §5.1: "`splice` and `static` are prefix forms wherever an operand can follow; a
user `def`/`method` of those names is unreachable as a bare call (the checker should reject the
declaration). `splice f(args)` instantiates a parameterized fragment named `f`; it never splices a
call result — splice of an expression value is always a check error; write `run(f(x))`."

### 6. MINOR — §5.3/§5.2: state the capture-liveness invariant that makes `splice` hygienic

**Problem.** The statically-known operand rule is doing invisible work: because a splicable
fragment is a `let`-bound literal in scope or a call-site argument, lexical nesting guarantees
every binding it captured is live at the expansion site — this is what keeps `splice` free of
§5.3's dead-frame caveat, which is presented only for `run`. An implementer who later relaxes the
operand rule (e.g., allows `splice f(x)` on a computed `Code`) silently reintroduces stale-capture
reads.

**Fix.** One sentence in §5.3: "the statically-known operand rule is also the capture-liveness
rule: every binding a `splice`d fragment captured is in scope at its expansion site by
construction; the stale-cell caveat of captured frame cells applies only to `run`."

### 7. MINOR — §5.1/§5.2: `code"…"` purity and a few result-type edges are unstated

**Problem.** (a) Whether a `code"…"` literal is pure — usable in `where`, guards, `var`
initializers, answers — is never said; it produces a value and has no run-time arena effect, so it
should be pure, but an implementer must guess. (b) `code"fail"` and other non-producing bodies have
no defined `T` — `CompiledStatement.resultType` is `std::optional` precisely for non-producing
statements (`compiler.hpp:28-34`), so `Code` needs an answer: refuse, or `Code<()>` with
`produced=false`. (c) Whether a spliced expression can be a place — `splice c := 3` targeting a
captured local — is unspecified.

**Fix.** State: `code"…"` and `static` are pure expressions; `splice`'s purity is the fragment's
`pure` flag. Non-producing bodies are refused (or typed `Code<()>` — pick and say). `splice` of a
bare captured place is legal as an assignment target iff the capture was a place — or refuse and
require explicit locals.

### 8. MINOR — §5.1, M2: nested `code"…"` and diagnostic locations inside string bodies

**Problem.** Nested literals work only through escaping — `code"x + splice code\"n | n+1\""` —
and a `cells"…"` inside a body costs `cells\"##/+d.x\"`, with `\\\"` for the level below. The doc
notes "escapes work as in `cells"…"`" and M2 prices recursive parse+check, but shows no nested
example, and the more important ergonomics gap is unaddressed: errors *inside* a `code"…"` body
need a location mapping back to the source (the `src:<module>:<line>:<col>` head label exists for
fragments, but parse/check diagnostics in string bodies are at inner offsets — without translation
the user gets "line 1, column 5" of a string). Also unremarked: the `#` label prefix must be
stripped before the inner lexer runs, since `#` is not a vagent token.

**Fix.** Add a nested-literal example and one sentence: "diagnostics inside `code"…"` bodies are
reported at the literal's position, qualified by the inner offset (e.g. `src:demo:12:4+3:10`); the
`#label` prefix is stripped textually before the inner lex, since `#` is not a token."

### 9. MINOR — §5.1/§6: naming overload and the expression/declaration boundary

**Problem.** (a) `splice` now names the unquote keyword while `Splice` is the Vortex opcode P15
mutates (§6.5, §10.2) and `spliceCellSpan`/`value()` splices are the store/vlace/vicon span
mechanism — three different `splice`s in the family, two of them on facing pages of this document.
(b) `code` names the literal prefix, the `Code<T>` constructor, the `d.code` registry, and the
"`code` parameter" concept. (c) `CodeBody ::= … Expr` means `splice` can never expand to a
declaration — no compile-time `def`/`plan`/`on` generation — which is coherent (declarations are
`graft_*`'s job) but worth one explicit sentence so readers do not hunt for decl-level macros.

**Fix.** A terminology note in §5.1 distinguishing `splice` (unquote) from `Splice` (opcode) and
span splices; consider `expand`/`inline` if a rename is cheap now. One line: "fragments are
expressions; declarations are produced at run time by `graft_*`, never by expansion."

## Summary

Four MAJOR items, all in the static-semantics layer rather than the grammar: the `def`-parameter
contradiction (F1), missing position/purity re-verification at composition boundaries (F2),
`static`'s under-specified closedness and result domain (F3), and unspecified argument-evaluation
semantics for `splice` (F4). The grammar additions themselves are clean — the lexer merge, the
contextual-keyword route, and the `cells"…"` precedent all verify against the code — and the
`run`/`graft` vs `splice` line is a genuinely good failure surface once F1 is fixed.

VERDICT: NOT CONVERGED
