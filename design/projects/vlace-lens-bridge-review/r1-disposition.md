# R1 disposition — vlace-lens-bridge

All six critics NOT CONVERGED. Rulings (all ACCEPTED unless marked):

- C1 vacuous lensCommit: add `lensEdit :: Cell -> String -> IO (Result String Unit)` (routes through
  session->editContent, refuses ref-bound cells); `lensCommit :: Cell -> IO (Result String Int)`
  (dirtyCount snapshot -> commit -> Ok count / Err reason). Journey note: fs/git lenses are
  read-only today — commit only reachable over sql:/cfg: until FilesystemLens grows an unproject
  half; stated as dependency U-L9.
- C2 §5 example: rewritten — `case` over `find :: (a->Bool)->List a-> Option a` (listed as a prelude
  addition P4 with foldMap/concatMap), no `pure unit`, end on putStrLn.
- C3 meta: misroute: doc corrected — openLensUri has no meta: scheme; `IO.OpenLens` gains an
  explicit `meta:`/union-guard -> EvalError (P1 covers). union:/log: added to scheme list.
- C4 Proposal Register: added (P1-P5 below).
- C5 purity: D3 rewritten — Cell is a stable handle into mutable projected structure; pure reads
  observe the arena at forcing time; ordering vs mutating ops goes through IO bind order.
  childrenAll's dim-minting noted.
- C6 error channels: all effectful ops return Result/Option — openLens :: String -> IO (Result
  String Cell); Cell 0 sentinel deleted.
- C7 attr cells: session resolution via session-owns-cell (mint tracking), not keyFor alone; cellKey
  contract = projected node cells; ops walk negward to owning node before refusing.
- C8 refresh: every mutating op = gesture + session->refresh().
- C9 sessions\_ retention: D4 rewritten — sessions die with the Compiler/evaluation (arena
  lifetime), not cell reachability; OS resource cost priced; lensDrop deferred (U-L10).
- C10 mount returns delegate root: lensMount :: Cell -> IO (Result String Cell); lensNext :: Cell ->
  IO (Option Cell) (session-cell arg, Ok=materialized page); lensUnmount :: Cell -> IO (Result
  String Unit).
- C11 setLink bypass: new rule — arena verbs on session-owned cells are provenance-breaking and must
  be guarded or documented read-only; stated as D6 + P5 proposal.
- C12 cites/pricing: added throughout.

## Proposal Register (new)

- P1 compiler.cpp: installXanadu lens\* ctors + runIO IO.\* handlers + meta:/union guard +
  refOfWrapped inner-force fix. Justification: effect surface; cannot be expressed in vlace source.
- P2 types.cpp: builtin Schemes for the new signatures.
- P3 compiler.hpp: session-resolution helper + sessions\_ owns-cell scan.
- P4 prelude.hpp: find/foldMap additions (vlace-source, cheap).
- P5 projection_session.{hpp,cpp}: ownsCell(CellRef) over mint tracking
  - editContent routing + arena-verb ownership guard decision.

## Deferred

- U-L9 filesystem/git write-back (read-only lenses today)
- U-L10 lensDrop (sessions die with Compiler; revisit if Driver gains a persistent REPL over one
  Compiler)
- meta: in vlace (needs arena-with-base + MetaSource callback)
- keyCell inverse (cellKey is output-only; "round-trip" claim dropped)
- negward climb verb (D1's "back" relies on retained roots)
