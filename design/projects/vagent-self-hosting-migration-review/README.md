# VAgent Self-Hosting Migration Design Review

- **Target Document:**
  [`design/vagent-self-hosting-migration.md`](../../vagent-self-hosting-migration.md)
- **Status:** **CONVERGED** (All 6 reviewers returned `VERDICT: CONVERGED`)
- **Review Directory:** `design/projects/vagent-self-hosting-migration-review/`

______________________________________________________________________

## Convergence Summary Table

| Reviewer Role                              | Round 1 Tally                       | Round 1 Verdict     | Round 2 Tally                     | Round 2 Verdict     |
| :----------------------------------------- | :---------------------------------- | :------------------ | :-------------------------------- | :------------------ |
| **Technical Writer (`writer`)**            | 3 BLOCKING, 5 MAJOR, 4 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 4 MINOR      | **CONVERGED**       |
| **Vortex & Spool-Mesh Expert (`vortex`)**  | 4 BLOCKING, 3 MAJOR, 2 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 2 MINOR      | **CONVERGED**       |
| **Codebase Expert (`codebase`)**           | 3 BLOCKING, 5 MAJOR, 3 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 0 MINOR      | **CONVERGED**       |
| **System Architect (`architect`)**         | 3 BLOCKING, 3 MAJOR, 2 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 0 MINOR      | **CONVERGED**       |
| **PL Design & Ergonomics Expert (`ergo`)** | 4 BLOCKING, 4 MAJOR, 2 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 2 MINOR      | **CONVERGED**       |
| **Xanadu & ZigZag Purist (`zigzag`)**      | 3 BLOCKING, 4 MAJOR, 4 MINOR        | NOT CONVERGED       | 0 BLOCKING, 0 MAJOR, 2 MINOR      | **CONVERGED**       |
| **Total**                                  | **20 BLOCKING, 24 MAJOR, 17 MINOR** | **0 / 6 CONVERGED** | **0 BLOCKING, 0 MAJOR, 10 MINOR** | **6 / 6 CONVERGED** |

______________________________________________________________________

## Numbered Proposals for User Approval (`P18`–`P25`)

The review established eight numbered proposals formally tracked in Section 6 of
[`design/vagent-self-hosting-migration.md`](../../vagent-self-hosting-migration.md):

1. **`P18` (Scope Restoration on `#Return` / `#RunResume` in `VortexVM`)**:
   - *Target:* `apps/common/xanadu/vortex/vortex_vm.cpp`
   - *Effect:* Restore caller `varsHead` on frame exit to eliminate scope leaks and variable
     shadowing in recursive descent parsing.
1. **`P19` (Scanning Cut in `compiler.cpp`)**:
   - *Target:* `apps/common/xanadu/vagent/compiler.cpp`
   - *Effect:* Emit `OpcodeKind::Cut` upon successful completion of `s ? e` to protect subsequent
     AST/opcode allocations from backtracking rollbacks.
1. **`P20` (R1 AST Declaration Manifold Support)**:
   - *Target:* `apps/common/xanadu/vagent/r1_ast.{hpp,cpp}`, `marshal.cpp`
   - *Effect:* Extend R1 AST with declaration kind tags and marshaling for `Program`, `Import`,
     `AgentDecl`, etc.
1. **`P21` (Compilation Scratch Arena Lifecycle)**:
   - *Target:* `apps/common/xanadu/vagent/host_compile.cpp`, `inlang_parser.cpp`
   - *Effect:* Use an isolated scratch `ArenaManifold` during compilation, releasing it via RAII to
     prevent permanent cell leaks in live agent manifolds.
1. **`P22` (Dynamic Record Expressions)**:
   - *Target:* `apps/common/xanadu/vagent/compiler.cpp`
   - *Effect:* Enable dynamic member evaluation in record literals for runtime `Token` construction
     in `stdlib/lexer.vagent`.
1. **`P23` (C++ Compiler Runtime Shims)**:
   - *Target:* `apps/common/xanadu/vagent/inlang_*.{hpp,cpp}`
   - *Effect:* Replace C++ frontend bodies with thin C++ wrapper shims calling in-language `.vagent`
     compiled fragments.
1. **`P24` (Retire C++ Compiler Frontend in `Makefile`)**:
   - *Target:* `Makefile`
   - *Effect:* Remove ~14,842 lines of deprecated C++ compiler code upon quine fixed-point
     verification.
1. **`P25` (Eliminate `noCell` Sentinels Across Engine APIs)**:
   - *Target:* `apps/common/xanadu/vagent/inlang_checker.hpp`, `inlang_emitter.hpp`, `executor.hpp`
   - *Effect:* Convert all residual `CellRef = 0` sentinels to `std::optional<zigzag::CellRef>`.
