# Private evidence cases in Xuzz and vquery

An evidence case keeps read-only SQLite and HTTP GET observations, coverage, and annotations in a
private local store. Refresh adds a new immutable observation; it does not edit the source or move
older notes. The GUI journey and the `vquery` command interface use the same `EvidenceController`.

## Work through the Xuzz panel

Open the panel with **Ctrl+Alt+Shift+E**, the default `evidence-case-toggle` key binding. The chord
is configurable in the system keymap. In **Case**, enter a local case directory and title, then
choose **New case**. **Open case** resumes a saved case; opening it does not contact any source. Use
**Save case** to save without closing, or **Close case** to save and close.

In **Sources**, add a connection definition:

- For SQLite, enter a source name, database path, and a read-only `SELECT` query. A stable key
  column is optional and must be named explicitly.
- For REST, enter a source name and an HTTP(S) URL. This stage supports read-only GET capture;
  redirects and authenticated endpoints are unsupported.

Adding a connection does not acquire data. Select it, use **Refresh**, and then use **Check capture
status** while acquisition is running. I/O happens off the UI thread; completion and retention are
handled when the UI polls. Repeat the status check until it reports completion or a coverage
outcome. **Cancel capture** requests cancellation. The selected source's persisted budget is shown
before refresh. Defaults are 1,000 rows, 64 columns, 16 REST pages, 1 MiB of response bytes, and 5
seconds. Refresh retains the raw response and reported coverage in the private case. Partial and
failed pages remain visible as incomplete evidence.

In **Evidence**, inspect captured observations, requests, coverage, schema fields, rows, and values.
Filter the list by kind, label, record ID, parent context, or record text to find a specific item.
Choose **Show metadata** to reveal and select metadata such as coverage, errors, and schema details;
metadata rows retain their parent record context. To annotate a field, select its observed value or
response body, switch to **Notes**, enter note text, and choose **Annotate selected field**.

For an exact quotation, select the retained value or response body, enter the text in **Text to
quote exactly**, and choose **Locate exact text**. A unique match fills the UTF-8 byte offset and
length. Repeated matches are reported as ambiguous and do not select a range. Choose **Preview exact
span** to see escaped selected bytes, surrounding retained context, the target record ID, and the
half-open byte interval before committing with **Annotate exact span**. Locate and preview are
read-only. The annotation is pinned to those bytes in that observation; refresh will not silently
retarget it.

Use the observation IDs shown in **Evidence** to compare an earlier and later capture in **Compare
and export**. The comparison is local to the retained case; it does not claim continuity where the
source has not established it. For SQLite, naming a key column is an explicit continuity assertion.
Matching also requires a capture-time witness for a regular database file: its canonical path,
device number and inode must be available and unchanged across the capture. The witness scopes a
local continuity claim; it is not proof against key reuse, inode reuse, or replacement by a
semantically different database. If the witness is unavailable or differs, comparison treats rows as
positionally corresponding with identity uncertain. SQLite schema editions are recorded from the
result columns' names and declared types; a schema change makes field continuity uncertain. Without
a declared unique key, row identity is observation-local; duplicate or uncertain keys are not proof
that a row is the same entity.

Enter a record ID and a destination directory to **Prepare selective export**. The export is written
locally. It is not published or sent automatically; review the selected closure before sharing it.

## Public account observations

The **Communications** screen reads a public Mastodon-compatible account collection. Enter a
connection revision name, the HTTPS instance origin, its origin-local account ID and the known
ActivityPub actor URI. A profile page URL alone is not evidence of an actor identity. Choose **Read
public statuses**, then **Check capture status**; **Cancel read** requests cancellation.

Repeating the same named request retains another observation. A name cannot silently change its
endpoint or account; use a new revision name for a different request. Opening a case reads retained
evidence offline. It does not refresh the source. Closing a case cancels its read and fences a late
read result from a subsequently opened case.

Inspect `fediverse-observation`, `fediverse-response-page`, interpreted object/actor records and
`fediverse-read-gap` records in **Evidence**. Page bytes and interpreted content remain distinct.
Annotate any record through **Notes**. If a response echoes authentication material, the original
entity is withheld before retention; the notice and any quotation of it are explicitly marked as a
derivative. A successful local REST response is not proof of federation or recipient delivery.

The fediverse interface currently exposes public reading. Authenticated fediverse draft/send
interfaces remain part of the unfinished work in the [stage ledger](vql-stage-progress.md).

At the `vquery` prompt, the corresponding explicit acquisition command is:

```text
:evidence fediverse-read "social" "https://social.example" "123" "https://social.example/users/alice"
:evidence poll
:evidence list
```

## Mail observations and explicit submission

In **Communications**, use **Mail connection** and **Mail TLS trust** to save an immutable IMAPS
connection. Bind a separate, expiring read credential in **Mail read credential**, then explicitly
read and poll. Raw messages, MIME parts, headers, flags and coverage remain separate annotatable
records. Reopening a case needs no mail service; a new read needs a fresh credential.

Use **SMTP connection** to save a submission profile and its TLS trust. **Mail draft** separates
**Envelope**, **Content**, and **Review**. Enter each recipient on its own line; duplicate
occurrences are preserved. Bcc addresses belong to the SMTP envelope and are omitted from message
headers. Scroll the review list to inspect all recipients and body lines before creating an intent.

An intent sends nothing. Bind a credential for that exact intent in **Submit credential**, then
review, grant and explicitly dispatch through **Work**. Read credentials cannot authorize
submission. An accepted receipt means local submission acceptance, not recipient delivery. Unknown
outcomes require review; reopening never resends them.

The terminal prompts keep credentials out of command text and history:

```text
:evidence mail-credential "mail connection name" 300000
:evidence mail-submit-credential "intent ID" 300000
```

These prompts require an interactive terminal, mask input, and cancel without binding on Escape,
Ctrl+C or end of input. They refuse piped credential input.

## IRC sessions

Choose **IRC connection and session** in **Communications**. Configure **Service**, **Identity**,
**TLS**, **Protocol bounds** and **Pending reads**, then save the immutable connection. Account and
network scope labels are declared claims, not proof of account ownership. Certificate and hostname
verification are required. Optional **SASL credential** binding remains in host memory; it releases
the secret only after verified TLS and negotiated SASL support.

In **Session and reads**, connect explicitly, join a target, then request a live or history read and
**Poll and retain**. A bounded read is not a claim of complete history. Pending snapshots replay
until retention is saved durably; only then does their ticket advance the local cursor. Cancel and
poll unfinished work before closing a session or case. Closing revokes its credential; reopening
never reconnects or rejoins.

**Message draft** creates one exact local message intent. Review, grant and dispatch remain separate
actions. One IRC message cannot contain embedded CR or LF. A matching server echo is evidence of
server acknowledgement, not delivery to every channel participant. Read messages, membership
receipts, source gaps and effect receipts can all be annotated in **Evidence** and **Notes**.

For hidden terminal credential entry:

```text
:evidence irc-credential "connection name" 300000
```

## Local joins and finite windows

Use **Compare and export** to join two saved SQLite observations or choose **Window observations**.
These operations use retained evidence and work while the source database is unavailable. Joins
preserve duplicate occurrences and declare their key, null and cardinality policies. Windows declare
their time basis, half-open bounds, watermark and lateness policy. Missing timestamps, late data,
coverage gaps and computation cutoffs remain inspectable rather than becoming empty successful
results.

Inputs and limits occupy separate pages. Run explicitly, then poll to retain the result. Annotate a
query run, member, exclusion or witness through **Evidence** and **Notes**. Selected native exports
retain the selected derivation and its typed witnesses; inspect the closure before sharing. The
export still needs its matching permascroll and is not a standalone published package.

## Use the shared controller from vquery

Start interactive `vquery`, then enter `:evidence help` for command syntax. For example:

```text
:evidence new "/tmp/review-case" "Review"
:evidence sqlite "orders" "/data/orders.db" "SELECT id, memo FROM orders" "id"
:evidence refresh "orders"
:evidence poll
:evidence list
:evidence inspect 123
:evidence note 123 "Check this value"
:evidence locate 123 "café"
:evidence preview 123 8 5
:evidence span 123 8 5 "The exact UTF-8 bytes were checked"
:evidence save
:evidence close
```

Replace `123` and the byte range with IDs and values from the retained case. `locate` accepts a
unique exact substring; `preview` validates a nonempty byte range against retained bytes. A span
note should follow a successful preview of the same target and range. `poll` is an explicit
nonblocking check for acquisition completion.

The evidence case is an ordinary store that VQL can traverse. Load it in the same REPL with
`:store case /tmp/review-case`, then follow the record head through its explicit sequence dimension:

```text
##case/d.evidence.records/d.evidence.record-sequence/d.evidence.label
##case/d.evidence.records/d.evidence.record-sequence/d.evidence.kind
##case/d.evidence.records/d.evidence.record-sequence/d.evidence.attributes/d.evidence.attribute-sequence/d.evidence.attribute-key
count(##case/d.evidence.records/d.evidence.record-sequence)
```

The first three queries return record labels, kinds, and attribute names; the count query reports
the number of records. `##case` resolves the label supplied to `:store`. The explicit
`d.evidence.record-sequence` traversal is required to enumerate the whole record rank; querying
`##case/d.evidence.records` alone does not enumerate it. The syntax follows the named-store anchor
(`##NAME`) and whole-rank traversal defined in the [VQL specification](vql-query-language.md); load
a case first or the anchor resolves to no store.

## Stage 1 validation

Stage 1 is complete. The [acceptance record](../output/vql-stage1-20261022/acceptance.md) links the
actual GUI frames and accessibility dumps, full terminal PTY transcript, saved operations, selected
export and test results. Both source types were exercised, including a failed later REST page. Field
and exact UTF-8 span notes survived offline reopen; refresh comparisons preserved old/new values and
the selected export excluded unrelated source definitions. Read-only response inspection escapes
non-ASCII and control bytes; the retained underlying bytes remain exact.

The [iteration record](vql-stage-progress.md) separates stage 0 and stage 1 and records verification
limits. Later stages in the [workflow extensions plan](vql-workflow-extensions.md), including
external writes, authentication and publication packaging, remain proposals.
