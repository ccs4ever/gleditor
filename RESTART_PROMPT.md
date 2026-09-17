# Restart prompt: unified-store recovery

Resume work in `/data/git/gleditor/.worktrees/unified-store-recovery` on branch `recovery/unified-store`.

## User objective

1. Remove application YAML storage formats and readers/writers. Keep infrastructure YAML and defer system-store YAML while those stores are being rewritten.
2. Use one native `xanadu::Store` loading path for Xudu, ZigZag, VPL, VPLC, VQuery, and VQueryC. There is no storage-level distinction between xanadocs and ZigZag slices.
3. Preserve the local-main page-loading fix: background page building must be rate limited per frame.
4. Publish human-readable metadata as deterministic TSV, not YAML.
5. Merge the finished work into local `main` and push `main` to the remote.

## Current worktree

- Recovery commits already present:
  - `d1bbd14` — promote MultiStoreCoordinator into common xanadu core.
  - `6d6b8cb` — recover dirty multi-store refactor.
  - `91e49b9` — shared native-store loader and frontend integration.
- `src/doc.cpp` already contains the bounded `buildPendingPages()` fix from local main; do not redesign it.
- Shared loader files: `apps/common/xanadu/store_loader.{hpp,cpp}`.
- ZigZag main now loads native stores and no longer uses the application YAML slice loader.
- VQL frontends use `apps/common/xanadu/multi_store.*`, not a duplicate `vql/multi_store.*` implementation.
- The old refactor worktree at `/data/git/gleditor/.worktrees/multi-store-consolidation` must not be reset or cleaned.

## Unfinished work

The current uncommitted edits are the publication metadata migration:

- `Provenance::toYaml()` now emits deterministic tab-separated records (legacy method/member names remain for compatibility).
- `parseProvenance()` has a TSV parser with legacy YAML fallback.
- Published names changed to `AUTHORSHIP.tsv` and `AUTHORSHIP.tsv.asc`.
- `PublicationLedger::toYaml()/fromYaml()` are being converted to TSV; torrent name should be `PUBLICATION_LEDGER.tsv`.
- Finish compile validation and update publication/provenance tests and any remaining application references to `AUTHORSHIP.yaml` or `PUBLICATION_LEDGER.yaml`.
- Keep system-doc/config YAML untouched unless it is clearly part of published application metadata.

## Required next steps

1. Check `git status` and inspect the partial TSV edits.
2. Finish TSV serialization/parsing robustly: escape tabs, newlines, carriage returns, and backslashes; preserve all publication fields and topics; ensure deterministic output for signatures.
3. Build focused targets, preferably:
   - `make -j$(nproc) xudu_test`
   - relevant provenance/publication-ledger tests
4. Run tests with writable temporary/XDG directories; the sandbox may reject writes under the worktree.
5. Run `git diff --check`, review `rg -n "AUTHORSHIP\\.yaml|PUBLICATION_LEDGER\\.yaml|zzstructure_loader|home_slice\\.yaml" apps tests` and remove application-format references while preserving infrastructure/system YAML.
6. Commit the TSV migration.
7. Update local main from the remote, merge or cherry-pick the recovery commits and TSV commit into `/data/git/gleditor/main`, resolve conflicts without discarding local-main page-budget changes, run final tests, then push `main` to the configured remote with explicit user-approved network escalation.

## Important constraints

- Do not reset, clean, or delete the old refactor worktree.
- Use `apply_patch` for source edits.
- Infrastructure YAML (`.github`, Dependabot, formatter/linter configuration) is intentionally retained.
- The user specifically requested a restartable session handoff; update this file if the state changes materially.
