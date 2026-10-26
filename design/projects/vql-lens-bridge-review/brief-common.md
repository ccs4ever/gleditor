# Review brief — design/vql-lens-bridge.md (v1)

Subject: a design spec for exposing projection lenses as first-class relations/generators in VQL —
`lens("uri")` opens, `cellPath` traverses, `lens*` generators mutate. Sources of truth:

- `design/vql-lens-bridge.md` — the doc under review.
- `design/vlace-lens-bridge.md` — the vlace design (v7.4) — the model: roots-as-values,
  ops-take-cells, ownsCell resolution, dirty-gate + edit/commit rules, session_ops seam.
- `apps/common/xanadu/vql/vql_engine.{hpp,cpp}` — resolveAnchor / StoreGeneratorCall / setVariable /
  env\_.
- `apps/common/xanadu/vql/{lexer,parser,token}.{hpp,cpp}` — KwLens, generator-call parsing.
- `apps/vquery/main.cpp` — $lens binding, LensRepl on coordinator.core, --lens raster, meta:
  MetaSource.
- `apps/common/xanadu/zigzag/session_ops.hpp` — the P7 shared gesture helpers the generators call.
- `apps/common/xanadu/zigzag/projection_session.{hpp,cpp}` —
  ownsCell/revertKey/commit/canApply/restateContent.
- `apps/common/xanadu/zigzag/lens_uri.cpp` — the P6 scheme guard.
- `apps/common/xanadu/zigzag/router_lens.{hpp,cpp}` — windowedFor/ nextStubAt.
- House style: Proposal Register; every change is a named file+change row; no sentinels; mechanism
  with exact file:line cites.

Each critic verifies claims against source, finds defects the doc doesn't cover, and renders
VERDICT: CONVERGED | NOT CONVERGED.
