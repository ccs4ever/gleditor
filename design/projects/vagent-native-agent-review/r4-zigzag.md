# Round 4 critique: Xanadu / ZigZag Purist (vagent-native-agent.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Status of previous findings and v0.4 audit

- **R3 minor findings:** Noted in `r3-disposition.md` for final polish. §10.5 bounds ledger
  retention to store segment growth (NQ5) and spools purpose text once per program into the private
  `authorities/permascroll/`.
- **N13 `d.subgoal` negward walk:** Sound. In `agent_executor.cpp:101-103` and
  `runtime_abi.hpp:267`, an intention's subgoal chain is strictly sequential (at most one active
  subgoal at a time along `+d.subgoal`). Walking `-d.subgoal` traverses a 1D chain of degree ≤ 2 to
  the root intention with zero fan-in or link eviction.
- **`hold_terminal`:** Sound. The grant scope transition is purely runtime broker state in its
  ephemeral in-memory table (§10.1, §10.5, §12.5). It creates no persistent links, no new cell
  dimensions, and no manifold mutations.
- **§6.4 C++ builder:** Sound. `vagent_native.hpp` is a header-only wrapper around `vn_api`
  producing byte-identical op logs to the C ABI (verified against `curses_hello_c.c` in §13.2). The
  op log replays into a validated single-parent AST `Unit` tree (§6.3) lowered by `compileAgent`. No
  C++ state leaks into the manifold.
- **`none_if` boundary translation:** Sound. Maps C sentinels (`-1`) to `Opt<T>` at the FFI boundary
  (§11.2), enforcing House Rule 3 ("No sentinels in APIs") before values enter the agent arena.
- **§10.5 Authority ledger:** Sound. Topological purity is preserved across all relations: 1D event
  ranks on `d.approval`, a chronological chain on `d.decision`, span quotation avoiding link fan-in,
  1-to-1 `d.revocation` links, and store isolation in `authorities/permascroll/`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- §10.5: In lines 752–754, clarify that decision purpose values quote the copy spooled into
  `authorities/permascroll/` rather than the declaration's span in the author's published
  permascroll, matching lines 737–738 and 769.
- §10.5: In line 755 ("At most one per decision, linked from it along `d.revocation`"), explicitly
  specify that a racing second revocation event (e.g. `released` racing with `host_lost`) is
  dropped/ignored so `d.revocation` never encounters link eviction.
- §13.4: Add an explicit test gate checking that multiple decisions quoting the same approval span
  resolve their quoted spans independently without link collisions.

VERDICT: CONVERGED
