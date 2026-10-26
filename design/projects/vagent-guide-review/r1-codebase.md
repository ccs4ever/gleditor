# R1 codebase review

**Verdict: NOT CONVERGED.** The guide accurately explains that `build/vagent` only checks standalone
source units, and it warns readers that checked syntax does not prove runtime support. The concrete
checker workflow matches the executable: `--help`, `--version`, `check FILE...`, and `check -` are
supported; diagnostics go to stderr, success prints `FILE: checked`, and rejected input produces a
nonzero exit. I ran the existing CLI headlessly over `tests/samples/vagent/*.vagent`; all 23 files
were accepted. No build was run.

## Actionable findings

### 1. The implementation-status table omits the partial compiler kernel

**Where:** `docs/vagent-user-guide.md`, “Implementation status,” rows “Syntax, types and static
restrictions,” “Running an agent,” and “REPL, event delivery and observing turns.”

**Evidence:** `design/vagent-implementation.md` records a scalar compiler kernel with 39 integration
tests that lowers and executes a limited set of scalar expressions as Vortex graphs. It is an
internal API, does not compile complete agent programs, and is not exposed by the CLI. The guide's
statement that compiler work is planned with the actor adapter misses this current milestone and can
leave readers with the impression that no Vagent-to-Vortex compilation work exists.

**Correction:** Add a row or note stating that the internal scalar lowering kernel is verified by 39
tests, while complete language lowering, module linking, and an executable agent runner remain open.
Keep `vagent run` labeled as future and avoid presenting that kernel as a usable source compiler.

### 2. The Map row points to a package API that does not exist as an implemented facility

**Where:** `docs/vagent-user-guide.md`, “Values, fields and locals,” the `Map<K, V>` row.

**Evidence:** The frontend checker recognizes the type spelling, but this iteration has no linked
`std:agent` package operations or runtime package API to consult. The implementation progress record
also says the linked prelude is still open. The guide's surrounding caveat distinguishes checking
from execution, but “consult the implemented package API” specifically implies that such an API is
available.

**Correction:** Say that `Map<K, V>` is part of the specified type surface and that map operations
depend on the future linked prelude/runtime; point readers to the design contract until then.

## Checks against the codebase

- The Make targets named in the guide exist. `make ... vagent vagent_frontend_test` is a valid build
  command; the progress record also gives the separate `test/vagent-frontend` target.
- `build/vagent check FILE...` checks each file independently and continues across rejected files;
  stdin is accepted as `-`. The sample-corpus command in the progress record matches actual CLI
  behavior.
- The guide does not invent a native-binding registration function, correctly leaves native skills
  dependent on host integration, and explicitly says the `vagent.*` settings are design defaults
  rather than live knobs.
- The disclosed gaps for cross-file loading, module alias/selector checks, adopting-agent template
  scope, actor execution, and the REPL agree with the current progress record.
