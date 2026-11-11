# VQL delivery iterations

## Iteration 1: stage 0 — complete

Completed 2026-10-22 against `7617d3f0` plus the working-tree repairs recorded in
[engine conformance](vql-conformance.md) and the
[four-round critique ledger](projects/vql-extension-reviews/critique-rounds.md).

The stage provides the per-engine matrix, executable common-subset corpus, explicit unsupported
states, detached projection versus explicit editing, and bounded identity/cardinality/value
contracts. The final acceptance audit found and fixed one remaining empty-stream path in return
field weaving. Boundary tests and the conformance document now specify the existing numeric-text,
double precision, tolerance, nonfinite and string-fallback comparison policy. Exact decimal/SQL NULL
and checked effects remain later contracts, not hidden stage-0 guarantees.

Verification: `make -j$(nproc) xuzz_test vquery` succeeded; all **266 tests from 28 suites** passed
with `VQL*:VQuery*:Vortex*:*Lens*`, including all **148 VQL tests**, headlessly with isolated XDG
directories. `git diff --check` passed. The existing performance spike and ABI evidence remain
scoped measurements. Repository-wide formatting/lint failures in untouched UI files are recorded in
the critique ledger rather than reported as passing.

No stage-1 implementation was included in this acceptance boundary. Changes remain uncommitted.

## Iteration 2: stage 1 — complete

Completed 2026-10-22 after the stage-0 acceptance boundary, with Luna implementation and independent
review agents. The engine captures read-only SQLite queries and bounded REST GET pages off the UI
thread. Private cases retain normal Structure operations and immutable primedia, typed values,
requests, schema, coverage and errors. Field, metadata and exact byte-span notes survive closing and
offline reopen. Refresh adds immutable observations; comparisons retain old/new value identities and
report incomplete or uncertain correspondence. Selected local exports contain their owned closure
and pinned notes without copying unrelated case history or connection definitions.

Xuzz exposes the journey through Ctrl+Alt+Shift+E and named, keyboard-focusable controls. `vquery`
exposes the same controller at its interactive prompt; VQL traverses the saved case as an ordinary
named store. See the [user guide](vql-evidence-case.md) and the
[acceptance record](../output/vql-stage1-20261022/acceptance.md).

Verification: the standard `make -j$(nproc)` build succeeded. Complete headless suites passed:
**1,939 engine tests** (17 network tests skipped; one disabled), **797 library tests**, and **245
UI/visualizer tests**. This includes 29 evidence engine tests and three panel interaction tests. The
real GUI journey covered both sources, an HTTP 503 later page, field/span annotation, offline reopen
with sources unavailable, SQL 95→105 comparison, REST refresh and selected local export. The
terminal journey retained its full PTY transcript and queried the saved case through VQL. Final
independent rendered review found no visual blocker.

Changed C++ and Markdown formatting, Markdown lint, Ruby syntax and whitespace checks passed.
Repository-wide format/lint gates still fail on pre-existing UI formatting and font-policy findings;
see the saved logs. Cross-platform package builds and rootless swarm tests were not run. No on-disk
engine format changed, and no publication or external write was performed. Changes remain
uncommitted.

## Iteration 3: stage 2 — complete

Completed 2026-10-22 with Luna implementation/review agents and final integration by the owner after
those agents reached their usage limit. See [controlled effects](vql-controlled-effects.md) and the
[acceptance record](../output/vql-stage2/acceptance.md).

The case retains immutable intent, expiring host grant, durable admission, acceptance and terminal
receipt records. Native guarded SQLite patches check exact typed preconditions and affected rows in
one transaction. Argv process execution has explicit streams/environment/stdin, deadlines, output
caps, process-group termination and host descriptor isolation. History import retains bounded
distinct occurrences without executing them. Opening, inspecting and recovering work never resends
it. Unknown outcomes require an independently acknowledged retry.

Case saves stage and flush snapshots, verify monotonic scroll bindings, then atomically replace the
operations snapshot. Lifetime writer locks exclude competing evidence-case owners. Late receipts are
fenced by document identity and retained in private inboxes when their original case is unavailable;
reopening reconciles them without dispatch. Tests include actual SIGKILL after durable acceptance,
staged-save faults, stale guards, cancellation, bounded process floods/held pipes, replacement cases
and file-backed inbox reconciliation. Two independently executed `vquery` processes verify ownership
refusal and release through their prompts.

The real Xuzz journey covers fake and process actions, duplicate history, uncertainty, acknowledged
retry, cancellation and prior-session grant refusal. Final 1280×900 and 1600×1200 frames verify
persistent field captions, complete retry cautions and recent outcomes visible immediately. The PTY
journey verifies SQLite 95→105, stale refusal, exact process output, history non-execution, explicit
retry and reopen without resend. Controlled native cases and their matching primedia are retained.

Verification: full `make -j$(nproc)` succeeded with the local GMime dependency prefix. The final
isolated headless engine run passed **1,991 tests**, with **17 network skips** and **one disabled
test**; all **248 UI/visualizer tests** passed. This includes **31 effects tests**, the evidence
storage/controller regressions and **13 communications/MIME groundwork tests**. Scoped C++ and
Markdown formatting/lint, workflow YAML lint, shell formatting, Ruby syntax and whitespace checks
passed. An earlier drag calibration failure passed in isolation and in the final complete rerun.
Repository-wide pre-existing formatting/lint findings, cross-platform package builds and rootless
swarm tests retain their previously recorded limits. No native engine format changed, no external
account was contacted, and changes remain uncommitted.

## Iteration 4: stage 3 — in progress

The remaining active objective is **all stages**. Communications and the full work environment are
not complete.

Compiled groundwork now includes bounded native GMime parsing and normal annotatable mail, IRC and
fediverse observations. Exact raw messages, MIME/header occurrences, decoded bytes/text and charset
availability stay distinct. Case-derived UID, message-ID and ActivityStreams indices survive reopen;
repeated observations retain their occurrences, conflicts refuse before writes, and later
objects/actors resolve earlier references with appended relations. Composite identity keys preserve
component boundaries. All 13 dedicated groundwork tests passed in the final engine run.

GMime 3 is a declared build/package/CI dependency. This environment uses a temporary local prefix
rather than modifying system packages. The preserved fediverse draft has since been hardened and
integrated into the engine. Native controlled tests cover pagination, later-page failures, redirect
refusal, bounded headers/body/time, exact public audience admission, credential revocation and
credential echoes withheld before returning evidence. REST account profile URLs do not become actor
identities; local REST acceptance does not imply federation delivery. Restricted recipients remain
unsupported.

Native IRC groundwork now negotiates capabilities, verifies TLS before SASL, retains history/live
gaps, requires confirmed channel membership, binds sends to a cryptographic connection incarnation
and distinguishes server echo from uncertain writes. Both communication effects reuse durable
admission, terminal receipts, original-case inbox routing and recovery without resend. Typed
credentials remain host-only. An asynchronous public fediverse read returns owned values for
owner-thread promotion; combined retention budgets refuse predictable overflows before appending.
Withheld response text remains annotatable with explicit derivative quotation provenance.

The coordinated build-6 focused run passed **41 communication/annotation tests** and **six panel
interaction tests**. These include native IRC and fediverse dispatch through the shared runtime,
late redacted receipts, restart without resend, combined bounds and connection-generation fencing. A
real headless Xuzz journey at 1280×900 and 1600×1200 verified named public-read controls and
retained failure/coverage records against a closed localhost endpoint. It found and fixed an
unregistered relation in the controller; successful remote reading and authenticated sending have
not passed a complete graphical journey. Subsequent review is addressing native connection timeout
coverage; these results are groundwork, not Stage 3 acceptance.

The full headless build-8 engine run passed **2,033 tests**, with **17 network skips** and **one
disabled test**; all **248 UI/visualizer tests** passed. The follow-up actor-correspondence fix
passed a full build and **350 VQL/Vortex/evidence regressions**. The
[verified groundwork record](../output/vql-stage3/groundwork.md) links logs and actual GUI/PTY
inputs. Configured actor identity correspondence is explicitly marked; withheld responses cannot
supply a parsed actor identity.

Automatic approval review initially held the IMAP/SMTP patch. On **2026-10-23**, the user answered
**“yes, implement code”** to the pending code-and-controlled-local-tests authorization. Native mail
implementation has resumed using the existing curl transport and GMime, without access to real
accounts or real sending.

Parallel Stage 4 groundwork now includes a pure bounded finite occurrence join library, with
explicit key/null/cardinality policies, duplicate preservation, source coverage independent of
computation truncation, and unknown left misses against incomplete right input. Strict at-most-one
requires complete right coverage. All **ten kernel contract tests** passed and independent review
quiesced. Retained-row lineage integration is being built and tested; no complete Stage 4 interface
or workflow acceptance is claimed.

Still required for the complete objective:

- Stage 3: native mail IMAP/SMTP, IRC and fediverse reading/drafting/scoped delivery through the
  shared durable runtime and real interfaces; credential leases, source-shaped gaps and
  audience-specific receipts; controlled native protocol acceptance tests.
- Stage 4: joins, windows, subscriptions, branching workflow runs, reusable Vortex packages and
  selective sharing, with bounded workloads, lineage, checkpoint/resume and safe export closure.

### Parallel implementation checkpoint — 2026-10-27

Main was fast-forwarded to `f0c55b9e`, with the existing VQL work preserved and adapted to its
optional-value APIs. The merge checkpoint passed 298 focused regressions. The subsequent stable team
build passed; workers and the owner serialize builds so compiler inputs cannot change while being
read.

Native IMAP and SMTP are now implemented. LibEtPan handles IMAP framing over a verified curl TLS
transport; GMime constructs exact drafts. Read and submit credentials have separate host-only,
expiring scopes. SMTP uses the shared durable effect journal, preserves duplicate envelope
recipients, excludes Bcc from message headers, and distinguishes local acceptance from delivery.
Controlled localhost graphical journeys verified authenticated mail capture, offline annotation,
explicit submission and reopen without authority or resend. The terminal credential journey also
verified hidden input, cancellation and pipe refusal. These are controlled fixtures, not
real-account acceptance. A later multiline compose journey exposed Return-key routing that joined
lines; its fix and new regression are undergoing validation. See
[mail read evidence](../output/vql-stage3/mail-read-positive-ui/run.log) and
[SMTP evidence](../output/vql-stage3/smtp-positive-ui/server.json).

IRC now has an asynchronous owned-result read job, scoped credential broker and bounded pending read
tickets. Acknowledgement advances a cursor only after owner-side retention. Controlled transport
tests cover cancellation, replay and target isolation. Credential-echo withholding and the
controller/interface consumer are still being integrated; this is not complete IRC acceptance.

Joins and finite windows now retain query runs, exact input occurrences, typed key/time witnesses,
coverage, exclusions and computation cutoffs. Joins have a complete local graphical journey,
including cardinality refusal, bounded output and selected native export. The corrected window
interface was exercised against retained SQLite evidence with the database unavailable: it retained
two duplicate members, a strict late-data rejection, and a separate output cutoff. A note on the
window survived closing and reopening. See
[window results](../output/vql-stage4/windows-ui/exclude-verified-11/run.log),
[strict rejection](../output/vql-stage4/windows-ui/reject-verified-11/run.log),
[cutoff](../output/vql-stage4/windows-ui/cutoff-verified-11/run.log), and
[offline note](../output/vql-stage4/windows-ui/annotation-reopen-11/reopen-corrected.log).

Stages 3 and 4 remain in progress. Authenticated IRC and fediverse interface journeys,
subscriptions, branching workflow execution and checkpoint/resume, reusable packages and complete
selective sharing remain required. A workflow journal backend is being implemented separately from
the already verified join/window paths. Changes remain uncommitted; no native store format changed.
