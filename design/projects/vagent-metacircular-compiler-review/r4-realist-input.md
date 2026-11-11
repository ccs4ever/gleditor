# R4 non-critic input — Systems Realist (deep-reasoning dialectic)
Feeding the disposition; not one of the six critics.

F1 HIGH: register ALWAYS defers in-executor — executor takes base Choice
mark + m0 before user code (executor.cpp:256-257) and every fragment body
adds its wrapper Choice mark → outstandingMarks()>=3 for the whole turn;
==0 only post-commit (executor.cpp:3024-3029). Consequences: compile→invoke
same-turn is impossible (T+1 callable); drain sites must be ENUMERATED host
points (post-commit, post-runFragment), never an arena-level on-0 hook
(evictOldestMark can hit 0 mid-execution; vortex_stdlib solver transient
marks); drain-time failure needs a fault:<label> record (genesis.cpp:219-228
precedent); two-state return (accepted-pending); starvation impossible
in-executor but non-executor drivers each need a named drain site.

F2 HIGH: fuel honesty — meta.cpp has ZERO consumeWork calls; every verb-side
walk is unbilled; traversalBound = whole arena not a budget. Pin consumeWork
per visited cell; max_register_ops per call; pending-queue cap; drain
charged to next turn (post-commit is consumeWork-free).

F3 HIGH: bindProgramOpcodes is PER-SPARK (fresh arena+VM each, or_parallel
.cpp:345-358) — carrier re-walk per spark is O(Σreach)·sparks; memoize one
enumeration per incarnation in the closure; ephemeral registered ops are
invisible to workers (workerArena overlays persistent base) — spark export
checks registration coverage or state or_parallel reaches durable-registered
graphs only.

F4 MED-HIGH: (b½) placement is NOT free — installedCopies are minted during
(d) (genesis.cpp:257-261,580); a pre-(d) sweep misses them for a whole
session. Pin: run carrier sweep after (d) (fold into (e)'s installedCopies
loop) or per-copy register inside replayPatch; union seen-set across carrier
heads (fan-in → O(total reachable) not O(heads×avg)).

F5 MED-HIGH: bootstrap join pair is expensive — each writer/linker call is a
full runFragment (~15-25 cells, 40-80 cycles overhead); ~200-400 cycles +
~100 cells per emitted op → 100-op body ≈ 20-40k cycles, most of a 100k turn.
Pin a BATCHED join surface (one host step consuming a rank of link/write
tuples). AST residue is session-persistent until promote — compact() never
reclaims cells; grant maxCells is the only bound today — say so.
Enumeration walk uses in-memory seen-set, not validate's per-op tag mints.

F6 MED: carrier grows monotonically; faulted head re-walks+re-faults every
incarnation — pin fault-eviction or unregister note; state O(total registered
ops) per incarnation complexity.

F7 MED: fuse enumeration+validate into ONE traversal; cache programCells per
incarnation; O(K) dup-label scans → max_registered_heads; re-register skips
walk on carrier-listed head.

F8 LOW: drain non-reentrant (queue to next drain point); register inside
genesis (b½)/(d) applies immediately (marks==0) — a third code path worth
naming; non-executor marks==0 contexts make register synchronous.
