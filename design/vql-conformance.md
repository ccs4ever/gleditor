# VQL engine conformance and executable examples

This is the implemented execution contract for VQL 16.1. The
[language specification](vql-query-language.md) also retains explicitly proposed syntax and runtime
designs. Parser acceptance alone promises neither direct execution nor compiler support.

## Engine matrix

| Feature                                                             | Parser                                | Direct evaluator                                           | Vortex compiler                                          | Persistence/service integration            |
| ------------------------------------------------------------------- | ------------------------------------- | ---------------------------------------------------------- | -------------------------------------------------------- | ------------------------------------------ |
| Home/valid cell anchor                                              | Accepted                              | Selects cell identity                                      | Supported anchor subset                                  | Arena only                                 |
| Context anchor                                                      | Accepted                              | Uses explicit current context                              | Refused without equivalent context binding               | Arena only                                 |
| Literal cell creation and creation chains                           | Accepted                              | Supported                                                  | Dimension-attached forward literals and supported yields | Arena only                                 |
| Whole-rank traversal/filtering                                      | Accepted                              | Supported                                                  | Refused before emission                                  | Reads attached arena/store views           |
| Variables, FLWOR, lambdas and definitions                           | Accepted                              | Supported existing subset                                  | Refused                                                  | No automatic persistent edits              |
| Structural parameter/return contracts                               | Accepted                              | Parameter matching, return errors                          | Refused; no unchecked erasure                            | No host-dispatch guarantee                 |
| Bound point-free functors                                           | Accepted                              | Shared step-vector representation                          | Refused                                                  | Ephemeral                                  |
| Shape predicates and empty masks                                    | Accepted                              | Supported neighborhood subset                              | Refused                                                  | Arena only                                 |
| Bare shape application                                              | Accepted for diagnostics              | Refused; select project or apply_shape                     | Refused                                                  | No implicit writeback                      |
| Explicit project/apply_shape                                        | Ordinary positional calls             | Snapshot masks / checked explicit neighborhood edit        | Refused                                                  | No source commit                           |
| Stream comparisons and explicit-domain quantifiers                  | Accepted                              | Cartesian existential comparison; two-argument quantifiers | Refused                                                  | No remote coverage claim                   |
| Cardinality and same_cell                                           | Ordinary positional calls             | Supported                                                  | Refused                                                  | Identity within bound arena                |
| Clone join                                                          | Accepted                              | Leftmost authority; collision checks                       | Refused                                                  | Arena topology                             |
| current and attached named stores                                   | Accepted                              | Local selection                                            | Refused except bare home                                 | No implicit source loading                 |
| Native find over attached stores                                    | Accepted                              | Existing content search                                    | Refused                                                  | Known attached content only                |
| Distributed generators/anchors                                      | Accepted design syntax                | Unsupported operations refuse                              | Refused                                                  | No silent home substitution                |
| Version pins and historical folds                                   | Accepted design syntax                | Refused                                                    | Refused                                                  | Unbuilt VQL integration                    |
| watch/delta                                                         | Accepted design syntax                | Refused; no simulated live updates                         | Refused                                                  | Unbuilt VQL subscription                   |
| Named/scoped perspectives                                           | Accepted                              | Transient arena overlay and canonical fallback prototype   | Refused                                                  | No durable private proxy or access control |
| Pivots, virtual dimensions and dynamic lenses                       | Accepted design syntax                | Refused                                                    | Refused                                                  | Unbuilt execution                          |
| Named arguments, scalar infix arithmetic, conditional scalar values | Outside current accepted value subset | Use supported positional functions                         | Refused                                                  | Design examples only                       |

Compiler refusal is a correctness boundary. Its former single-Link path lowering did not implement
the direct evaluator's whole-rank semantics, and several accepted script constructs were ignored.
The compiler now validates the full query before emitting cells/opcodes. It must not execute only
the first statement or erase a typed definition's unchecked contracts.

The compiler supports one verified path or a binding-free single path return, without predicates,
version selectors, shape/clone operations, general function calls or ordinary traversal. The
creation-only path subset supports dimension-attached literals with default forward placement and
the tested yield modes. Bare creation anchors refuse before emission: their preallocated identity
otherwise bypasses the per-New reuse check. Unsupported features report a diagnostic recommending
direct execution; that recommendation does not imply the direct evaluator implements an unbuilt
service.

Compiled creation is a single-use executable over an open posward rank. Its New instruction opts
into an explicit append mode, resolving the current tail at execution rather than overwriting an
existing neighbor. Ordinary three-operand Vortex New retains its immediate-neighbor behavior.
Starting another cursor on still-linked creation outputs or appending to a cycle fails before that
instruction changes links. Compile a fresh executable for a new creation run. Manually detaching
outputs does not turn them into fresh identities: reusable allocation frames and general
negative/placed creation are not claimed.

## File-backed example corpus

The files below are consumed directly by
[the conformance tests](../tests/xuzz/vql_conformance_test.cpp). The tests create isolated arenas,
run both engines where supported, inspect ordered results, and require unsupported compilation to
leave the arena and VM cursor set unchanged.

| File                                                                              | Executed assertion                                            |
| --------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| [home.vql](../tests/samples/vql/conformance/home.vql)                             | Both engines preserve home identity                           |
| [create_rank.vql](../tests/samples/vql/conformance/create_rank.vql)               | Both create Alpha, Beta in order                              |
| [create_last.vql](../tests/samples/vql/conformance/create_last.vql)               | Last yield selects Beta                                       |
| [create_fields.vql](../tests/samples/vql/conformance/create_fields.vql)           | Creation chain returns Title                                  |
| [create_fields_last.vql](../tests/samples/vql/conformance/create_fields_last.vql) | Multi-input last yield has global cardinality one             |
| [create_fields_both.vql](../tests/samples/vql/conformance/create_fields_both.vql) | Both yield preserves input/output ordering                    |
| [rank_read.vql](../tests/samples/vql/conformance/rank_read.vql)                   | Direct reads both precreated members; compiler refuses        |
| [filter_rank.vql](../tests/samples/vql/conformance/filter_rank.vql)               | Direct filters Alpha; compiler refuses                        |
| [empty_shape.vql](../tests/samples/vql/conformance/empty_shape.vql)               | Empty literal parses/direct executes; compiler refuses script |

The accepted-syntax/rejected-compilation corpus also includes
[functors](../tests/samples/vql/conformance/functor.vql),
[multiple statements](../tests/samples/vql/conformance/multiple_statements.vql),
[shape application](../tests/samples/vql/conformance/shape_projection.vql),
[versions](../tests/samples/vql/conformance/version.vql),
[generators](../tests/samples/vql/conformance/generator.vql),
[conditional actions](../tests/samples/vql/conformance/conditional.vql),
[watches](../tests/samples/vql/conformance/watch.vql) and
[definitions](../tests/samples/vql/conformance/definition.vql). These files are rejection fixtures,
not claims of working external services.

Full-input validation rejects an unmatched closing brace followed by ignored text. The corpus also
explicitly rejects unsupported named arguments and scalar infix arithmetic.

## Semantic regression coverage

[Semantic repair tests](../tests/xuzz/vql_semantics_repair_test.cpp) establish:

- Unsupported source, historical and watch operations refuse instead of substituting home/current
  state or manufacturing delta metadata.
- Stream comparisons examine all operand pairs; empty operands compare false. Equality and
  inequality can both hold over a mixed stream.
- Quantifiers receive explicit domain and predicate. Empty-domain universal and none are true.
  One-argument built-in forms refuse instead of recovering a lost predicate from syntax.
  User-defined routines retain precedence over built-ins.
- Cardinality errors and identity tests distinguish duplicate content from the same cell.
- Projection remaps selected cyclic topology, preserves referenced content spans, and leaves
  original reciprocal links untouched. Only masks are supported for projection.
- Explicit shape application edits reciprocal links; incompatible batch endpoint claims refuse
  before edits. Nested shape values refuse instead of becoming fabricated Boolean targets. Bare
  braces refuse ambiguity.
- Clone joins preserve leftmost authority and reject occupied-rank collisions.
- UDF/lambda lexical bindings restore after exceptions; explicit arguments use caller scope and
  defaults can see earlier callee parameters. Surplus arguments refuse before evaluation.
- Empty downstream path, functor and macro steps invoke no UDF/lambda and do not reground through a
  root selector; an empty final statement clears prior results.
- Unknown nested scalar calls and unsupported builtin arities refuse instead of fabricating empty
  text or discarding arguments. Invalid shape-assignment calls preserve the subject topology.

Bare `f()` and explicit `./f()` currently share the parser's initial Context-anchor call form; they
are not distinct absent-context operations. Missing untyped arguments, duplicate parameter names and
required/default declaration ordering retain the prior matching policy. The repaired excess-arity
check is not full strict callable validation.

Scalar comparisons first attempt to convert both operands to `double`. Integer and Boolean values
convert directly, and a string converts when `strtod` consumes its entire nonempty text. When both
convert, equality is true when the absolute difference is below `1e-9`; inequality is true at or
above that boundary. Ordering uses ordinary `double` comparisons. If either operand does not
convert, the evaluator compares strings: original string operands keep their text, while numeric and
Boolean operands use `std::to_string` on their converted `double`. Empty strings do not convert.
Large integers therefore lose precision at the `double` conversion boundary, and non-finite strings
such as `nan`/`inf` follow the platform `strtod` result and IEEE comparison behavior. These are the
current compatibility contracts, including their limits; this repair does not establish SQL NULL
semantics, exact decimal arithmetic, a full gradual type/effect pass or cross-store durable subject
identity. Those belong to the workflow proposal's later contracts.

## Run

Build with the repository's compiler and standard probes:

```sh
make -j$(nproc) xuzz_test vquery vqueryc
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
  XDG_CACHE_HOME=$PWD/build/xdg/cache \
  ./build/xuzz_test --gtest_filter='VQL*'
```

The command runs headlessly with isolated content/config/cache paths. A passing corpus is scoped to
its tested features; it does not certify proposed distributed workflows, source writeback or every
design-grammar production. Performance measurements are separate in the
[spike report](vql-performance-spike.md).
