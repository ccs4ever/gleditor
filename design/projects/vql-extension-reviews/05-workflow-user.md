# VQL as the connective tissue of my work

I want to return to a decision six months later and walk from its exact evidence through the query,
command, conversation, and external action that produced it. A database browser beside a mail reader
is useful; a durable, annotatable chain between them would change how I work.

This review treats §4.12 and the v16 additions as the intended VQL baseline despite the stale 15.1
banner. Everything below is proposed semantics. Examples use existing VQL expression/function forms;
all workflow functions and schemas are NEW standard-library proposals, not implemented syntax or
verified protocol guarantees. No accounts or services were contacted.

## The missing shared model

The generic lens gives me traversal and source edits; ZZ9P gives me transport; `watch` gives me
hypertime observation. The lens implementation plan already proposes private command Transactions
with `d.target`, `d.action`, and `d.reply` (A6/T2). Build on that. My missing abstraction is the
work that survives the projection session and relates heterogeneous services:

- **Connection:** an annotatable definition with endpoint, account identity, adapter/schema version,
  capabilities, default capture policy, and an opaque credential binding. Connection revisions
  remain inspectable. The secret itself stays in a credential broker outside publishable primedia.
- **Subject:** a durable local proxy for an external row, message, request template, command,
  person, or channel. Its identity is namespaced by service and account, with explicit alias/rekey
  evidence. A local cell does not make an unstable remote identifier stable.
- **Observation:** an immutable captured state with source revision, acquisition time, observation
  time, request, completeness, and retained bytes. A current view points to the latest observation;
  notes on yesterday's field remain on yesterday's observation. Refresh must not silently retarget
  their spans. Do not use a mutable clone master to stand for immutable evidence.
- **Action and attempt:** an intention with target, parameters, authority, expected revision, input
  observation, and separate attempts. `d.reply` resolves to a receipt describing what was accepted,
  observed, rejected, or remains unknown. Acceptance and completion are separate facts.
- **Subscription and workflow:** durable definitions, executions, checkpoints, gaps, cancellation,
  and errors. Each execution pins its definition revision and evidence inputs.

Use ordinary cells for every definition, argument, result field, output chunk, status transition,
error, receipt, and checkpoint. A container reaches its first child on `d.contains`; siblings chain
on `d.seq`. Relations such as `d.subject`, `d.observation`, `d.attempt`, `d.receipt`, `d.cause`, and
`d.checkpoint` attach relation occurrences, with proxies where necessary to preserve one neighbor
per direction. Many items must never become many competing direct pointers. Reconcile the
implementation plan's `d.child`/`d.sibling` vocabulary with the canonical lens names before teaching
users either.

Any such cell or retained span can have private or shared notes in a separate annotation store.
Persisting a note on a live projection first creates/resolves a durable proxy; it cannot persist an
arena reference. Unsupported, uncaptured, absent, null, redacted, failed, and genuinely empty are
separate inspectable states.

## Journey 1: explain a billing anomaly without losing the evidence

I connect a read-only billing database and a REST ticket service. SQL results give me invoice rows,
column-schema cells, typed values, primary-key identity, and query/parameter cells. I join invoice
1042 to ticket T77, quote the disputed amount and a ticket-response span, then annotate why their
currencies disagree. A mail draft to the account owner includes selected evidence, not my private
investigation notes.

```vql
// NEW library functions; existing VQL-shaped pseudocode.
let $rows := db_query($billing, $sql_template, $invoice_parameters)
let $ticket := http_request($support, $ticket_request)
let $case := case_create("Invoice 1042 mismatch", list($rows, $ticket))
let $draft := mail_draft($mail, $case, $selected_evidence)
return $case, $draft
```

The SQL observation records isolation/completeness, timezone and exact numeric representation; NULL
must survive distinctly from zero and empty text. REST pagination records each request, continuation
and response boundary. A failed page yields an incomplete result with its error, not a convincing
smaller total. Both remain useful offline. A later database update creates another observation, not
a rewritten earlier quotation. Any proposed correction is a separate conditional transaction with a
reviewed patch and affected-row receipt. A changed row produces a conflict cell linking base,
proposed and current values; I can annotate the disagreement before choosing.

## Journey 2: turn shell history into an accountable incident investigation

An IRC report points to a failing deployment. I search imported shell history for the command used
last week, link its invocation to the deployment ticket, change one argument in a draft, then
execute that new action. The run's stdout explains a REST health response, and I quote the relevant
stderr span into a follow-up mail draft.

```vql
// NEW functions; imported history is evidence, never an execution request.
let $history := history_import($terminal, $selected_history_file)
let $prior := history_find($history, "deploy")
let $action := process_draft($prior, $argument_patch)
let $run := action_submit($action, $run_authority)
return $run/d.stdout, $run/d.stderr, $run/d.receipt
```

History entries retain source file/session and capture quality. An old history file may lack working
directory, status, timestamps or environment; display those as unknown. Never invent a successful
run from a saved command string. New invocations capture executable, argument vector, working
directory, selected environment provenance, start/end, exit/signal and ordered stream chunks. A
shell script is an explicit execution mode, so pasted metacharacters cannot silently change an argv
action into shell interpretation.

History is incomplete by nature; dedup must preserve repeated identical commands. Streaming output
has capture limits, truncation markers and resumable viewing. Terminal progress rewrites remain raw
byte/chunk evidence plus a separately derived presentation. On cancellation I see what stopped, what
children remain unknown, and which output was retained. Reopening the incident never reruns it.

## Journey 3: carry a technical conversation between mail, IRC and the fediverse

A mail thread proposes a release; an IRC participant identifies a regression; an ActivityPub-facing
account has already announced availability. I make a case linking all three, annotate exact
statements and versions, and derive a recipient-specific release correction draft from the confirmed
facts.

```vql
// NEW library functions; drafting has no delivery effect.
let $conversation := case_create("Release correction", $selected_messages)
let $mail := mail_reply_draft($mail_account, $thread, $approved_excerpt)
let $irc := irc_message_draft($irc_account, $channel, $short_correction)
let $post := social_post_draft($social_account, $audience, $public_correction)
return $conversation, $mail, $irc, $post
```

Source message, body alternatives, attachment, sender assertion, recipient set, reply relationship,
and observed delivery state are separately annotatable. Identical display names do not merge people;
I assert identity links with evidence. IRC disconnects create explicit gap intervals when the
adapter cannot recover history. Social edits and deletions append observation/tombstone events; they
do not make an archived quotation mysteriously current.

Sending each draft is a later action against its exact revision and audience. Per-recipient receipts
can differ, and local acceptance must not masquerade as universal delivery. A social post is an
account-scoped application action through a declared adapter, not a claim that all ActivityPub
servers share one REST interface. If offline, drafts remain editable and queued actions show whether
they require revalidation. Cancellation cannot promise to retract a message already accepted
elsewhere.

## Journey 4: resume a recurring investigation, not merely an event callback

Every morning I compare database overdue invoices, ticket REST status, and relevant mail/IRC
messages. I want a durable “accounts needing attention” slice with reasons, not an automatic
notification storm. A workflow edition holds the query definitions, connection revisions, schedule,
capture policy, deduplication rule and decision notes. Each run contains inputs, per-step state and
its checkpoint.

```vql
// NEW functions; watch remains the existing observation mechanism.
let $flow := workflow_define($definition, $private_workspace)
let $subscription := subscribe_source($tickets, $event_filter, $capture_policy)
watch $subscription/d.events as $event {
  weave workflow_step($flow, $event)
}
```

The source checkpoint advances only with the durable local event/work record it acknowledges.
Replayed events resolve to existing source-event identities; genuine repeats remain distinct. A
deleted remote cursor or expired history becomes a recorded gap and resynchronization run, not
“zero-loss” fiction. Local hypertime cannot reconstruct events never captured from a third-party
service. Queue backlog, rate limits, authentication expiry and schema drift each have status/error
cells with recovery actions.

On resume I see “DB complete, REST page 3 retryable, mail draft unsent.” Earlier successful external
steps are not repeated because a later step failed. There is no implied atomic transaction spanning
SQL, REST, shell and messaging: durable attempts and explicit compensating actions make partial
completion understandable. Unknown outcomes require reconciliation before retry, with deduplication
only where the adapter can support it.

## Boundaries, choices and first delivery

Private work must stay private through quotation, linkage and publication. Shared notes need an
explicit audience and a reviewed export closure including source spans, identifiers, raw requests,
URLs, attachments and receipts. Hiding a dimension is not deleting its prior operations or
preventing other paths from exposing it. A clean publication artifact must exclude disallowed bytes
and history. Credential values, authorization headers and secret environment values must be excluded
before append-only capture; redacting a later display is too late. Even opaque broker bindings and
endpoint names can reveal infrastructure and should be export-controlled. Capturing a service's
bytes locally does not establish that service user's Xanadu authorship or permission to redistribute
them.

The key usability trap is making every cell edit look equally consequential. Editing a note,
changing a request draft, refreshing evidence and submitting an external action need distinct
visible states. An empty query result must show whether it means no matches, unavailable source,
unknown dimension, filtered authority or incomplete capture. A single “retry” button must explain
which attempt resumes and whether duplication is possible. A single “undo” cannot rewind the
external world with hypertime.

My first vertical slice would connect a read-only SQLite query to one REST GET in a private case,
retain their request/result observations, annotate one field and one span, close, reopen offline,
then refresh into a visible version conflict. Add one dry-run action and a simulated ambiguous
receipt so the action model is tested before delivery adapters. Acceptance is entirely through the
interface: create connections without source files, inspect completeness, annotate, resume, compare,
and export only a selected safe excerpt. Broaden next to shell-history import, controlled process
execution, then mail, IRC and social drafting/delivery.

The ambitious destination is a personal work graph where the evidence behind a decision, the action
it justified, its uncertain consequences and my private reasoning remain navigable together. That
requires durable semantics and a legible interface, beyond projecting more services as ranks.

## Integrated-draft follow-up (baseline 7617d3f0)

The synthesis covers all six requested source areas and the substantial annotation, failed-page,
unknown-outcome, offline/restart and export cases. No additional connector category is necessary.
Three workflow holes remain worth making explicit:

1. **A person's pending-work inbox and selective recovery.** The records exist, but specify an
   ordinary saved slice presenting draft/blocked/unknown/conflicted work across runs and accounts.
   From it I should inspect the exact precondition, replace one input, skip a step with a reason,
   reconcile an ambiguous attempt, or branch and resume only the unfinished dependency subgraph.
   Preserve earlier run evidence; edits must not silently modify an already dispatched intent. This
   makes recovery a practical operation rather than an architectural promise.

1. **Scheduled-run admission and unattended scope changes.** Section 9 names background authority,
   but overlapping morning runs, missed schedules, and a schema/filter change that expands the
   affected population need declared policies. Coalesce, queue, skip or allow overlap; record each
   decision. A later definition/account revision must not silently broaden an existing send or
   shell-execution authorization. Add a dry-run difference showing changed recipients, rows, targets
   and expected work before enabling the new edition.

1. **Commands' file/artifact dependencies.** Stdout/stderr and output artifacts are listed, but a
   command can read mutable files and overwrite an artifact consumed by another step. Represent
   selected input-file observations, executable/script editions and produced artifact observations
   as first-class dependency occurrences. Distinguish “replay retained results,” “rerun with current
   filesystem,” and “rerun with retained inputs”; record unverifiable dependencies as unknown. This
   matters for bridging shell work to database imports, email attachments and REST uploads. A graph
   of command strings and stream output alone cannot explain the resulting dataset.

These are proposed model/UX refinements, not claims from a build or live interface audit. The
recommended SQLite + REST evidence slice remains the right first delivery.
