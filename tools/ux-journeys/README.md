# UX journey plans

These JSON envelopes curate representative, previously exercised UI and native-accessibility stages
from `build/ux-green-20261011`. They are reusable plans and evidence pointers, not a claim that
every journey is currently green or that the catalog alone covers every criterion. Read each
envelope's notes and `source_evidence` before running it.

The root UX journey runner consumes one envelope at a time. UI plans use ordinary visible inputs
(`--chord`, `--type`, pointer actions, captures); native plans use the native accessibility client
requests listed in `steps`. Run the profile's authoring/setup stages first, then dependent stages in
order. A `same profile` dependency means the exact same live data, config, cache, and store paths
across stages; do not copy a profile between save/resume stages. Each run needs a fresh evidence
output directory so captures and logs remain attributable.

The runner supplies `${repo}`, `${evidence}` and `${profile}`. Other `${variable}` placeholders used
in the plan require `--var NAME=VALUE`; unused metadata entries require no value. `variables`
documents the meaning and shows an example only; examples are not defaults or portable IDs. Resolve
store paths from the current run's manifest. Evidence outputs use `${evidence}` and repository
inputs use `${repo}`. Some plans retain fixture-specific coordinates, operation-number labels, or
geometry; the notes identify these limits.

Stages whose original plan file had been overwritten are marked reconstructed. Their launch
arguments and native requests were recovered from preserved execution metadata and partial event
logs; they are useful reproductions, but are not exact copies of the missing source plan. Accepted
source evidence remains at the original relative path. Missing or overwritten evidence is called out
in the envelope rather than inferred as a pass.

The native plans retain original fixed waits where the native helper performs an external Action or
assertion concurrently; an already-present accessibility label does not prove that action completed.
Other retained waits mark camera or scene settling, or keep the application alive for final
assertions and graceful exit. `--await-a11y` is available for exact UI readiness states when the
driver itself can verify completion; it does not replace camera comparisons or native-helper
synchronization.

```sh
python3 tools/ux-journey.py --list
python3 tools/ux-journey.py --check
make -j$(nproc) test/ux-journey-plans
python3 tools/ux-journey.py --journey J7 --stage author --run build/my-ux-run
```

Then inspect the saved author store and run `calculate-save` with `--var budget_store=PATH`. For
J8's `reopen-recompute`, pass `--profile-root build/my-ux-run/profiles/j7-author-profile` and
`--var budget_result=PATH` pointing to J7's saved output. The override keeps the same permascroll
binding; it does not copy a profile. Missing variables, reused stage evidence and captures outside
the evidence/profile directories fail before application launch.

A run records `driver-launch.json`, `resolved-plan.json`, driver logs and `stage-result.json`.
Native and PTY drivers also retain their own launch, events, snapshots or transcript, and result.
The runner selects `build/<program>` from the envelope; `--binary` can override it. `--timeout`
bounds the stage and terminates its process group, with time for helper cleanup.
