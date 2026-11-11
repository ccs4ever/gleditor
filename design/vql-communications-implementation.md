# Communications adapters: implementation contract

**Status:** Stage 3 implementation design, 2026-10-22. This document turns
[VQL workflow extensions §4 and §5.4–5.6](vql-workflow-extensions.md) into an adapter boundary and a
controlled test plan. It is not a claim that mail, IRC, Mastodon, or ActivityPub support has
shipped.

## 1. Scope and boundaries

Stage 3 adds three real protocol families: mail reading and MIME interpretation, mail draft
construction and SMTP submission; IRC TCP/TLS reading, history where offered, and message sending;
and Mastodon REST reading/posting with explicit ActivityPub actor/object identity relationships.
Each adapter returns typed, owned results and uses the common evidence and effect journal. A generic
record writer alone does not satisfy this stage.

Initial support is client-facing. Mail access uses IMAP and submission uses SMTP submission; IRC
uses the client/server connection role; fediverse writes use the configured Mastodon-compatible
account REST API. ActivityPub server-to- server inbox delivery, signing, recipient expansion and
federation are a separate role and are explicitly unsupported by this stage. A Mastodon REST success
response is not a report of federation-wide delivery.

The dependencies remain in `apps/common/xanadu/evidence/`. No network or protocol code enters
`src/`, `include/gleditor/`, a Vortex worker's arena, or the UI. Workers consume request values and
host-issued capability leases, and return owned DTOs. Only the owning execution context promotes
those DTOs into store records.

## 2. Shared communications contract

Keep two separate typed boundaries, both backed by ordinary evidence records:

```cpp
struct CommunicationReadRequest {
  ConnectionRevision connection;
  ReadSelector selector;        // mailbox, channel, actor/status, or object URI
  ReadCursor cursor;            // typed protocol cursor, optional
  CapturePolicy limits;         // rows/bytes/pages/deadline and retention policy
  ReadCapability capability;    // host-bound; never serialized
};

struct CommunicationObservation {
  ConnectionWitness connection;
  CaptureState state;           // complete, partial, failed, cancelled, pending
  Coverage coverage;
  std::vector<CommunicationItem> items;
  std::vector<CommunicationGap> gaps;
  std::vector<OwnedArtifact> artifacts;
  std::optional<ReadCursor> next;
};

struct CommunicationIntent {
  ConnectionRevision connection;
  CommunicationAction action;   // mail-submit, irc-send, mastodon-post, ...
  TypedActionPayload payload;   // mail headers/body/parts, IRC target/text, etc.
  Preconditions preconditions;
  Audience audience;
  PayloadManifest retainedInputs;
  CapabilityRequirement requiredAuthority;
};

struct CommunicationReceipt {
  EffectOutcome outcome;
  std::vector<RecipientOutcome> recipients;
  std::vector<OwnedArtifact> protocolEvidence;
  std::string safeDiagnostic;
  bool mayHaveExecuted;
};
```

These are contract sketches, not required spelling. `ConnectionRevision`, `ReadSelector`,
`ReadCursor`, `CommunicationItem`, and action payloads must be tagged types or variants, not
unvalidated maps of strings. Protocol-specific fields stay in their typed branches. The common
envelope supplies operation, target, policy, limits, input evidence, correlation identity and result
state; it must not flatten protocol behavior into a universal `send(text)` API.

The read side follows `CaptureJob`: `start(owned request, host lease)`, `cancel()`, `busy()`,
`poll() -> optional<CommunicationObservation>`. Workers never receive `Store`, `EvidenceCase`,
`CellRef`, borrowed spans, widgets, or credential IDs that can be resolved without host mediation.
Results carry stable source identifiers and owned bytes/strings/typed values only. Limits bound
bytes, item counts, MIME depth/parts, page/history count and time. A completed prefix plus a failure
is `Partial` with an addressable gap, never a smaller `Complete` result.

Promotion is a separate owner-thread operation. It creates normal semantic records through
`EvidenceCase::appendRecord`, retains selected bytes through the existing capture/retention path,
connects records with references, and returns a map from adapter-local item keys to persistent
`RecordId`s. Every item with meaning—including a header, recipient occurrence, attachment, message,
IRC gap, connection field, action, failed part and receipt—must be addressable and annotatable.
Adapter-local keys and arena `CellRef`s are never persisted as cross-session identities.

Persist a connection definition and schema revision separately from its host credential binding. A
connection record may retain endpoint, account-scope label, protocol/version, declared capabilities,
safety policy and non-secret schema. Credential material, authentication headers, OAuth
refresh/access tokens, passwords and SASL payloads live only in a host credential broker. The broker
resolves an opaque binding only after user/session admission and returns a short-lived, zeroizing
lease to the worker. The saved binding is descriptive and conveys no authority after restart.

## 3. One admission and receipt path

Reuse `EffectRuntime`'s critical ordering for every external action:

1. Draft a typed, immutable action revision and persist its exact target, audience, payload
   manifest, source references and preconditions.
1. Validate the action strictly; do not silently drop invalid recipients, unsupported MIME parts,
   targets or visibility choices.
1. Obtain a session capability scoped to connection/account, operation, exact target/audience,
   limits, expiry and revocation generation.
1. Persist an `effect-attempt` admission and flush the case and author permascroll before opening a
   socket or issuing HTTP.
1. The adapter rechecks capability, expiry, revocation, target resolution, redirects and destination
   scope at the point of I/O. The credential broker releases only the credential required for this
   operation.
1. Return owned protocol evidence. The owner creates an annotatable receipt, per-recipient outcomes
   and output/evidence records, then durably saves.

Factor the common admit/receipt lifecycle rather than implement one journal per connector. The typed
communication request is one variant of the shared effect request; it preserves the same
intent→attempt→receipt semantics, recovery policy and cancellation rules as SQL/process effects.
Existing `EffectRuntime`'s local attempt scan is not a cross-process ownership claim; the stage
implementation must use the case's single-writer admission and keep the documented
no-resend-after-unknown rule. On restart, credentials must be re-authorized. Recovery may ingest
late receipts, reconcile through a declared source-specific status/read path, or leave outcome
unknown; it never resends just because a receipt is absent.

An authority record is evidence of a user decision, not a bearer token. The live capability object
is host-only and is not reconstructed from an ID on reopen. Redirects are denied by default. If a
connector follows one, it must re-authorize the resolved origin before forwarding any secret. TLS
peer and hostname checks are required whenever a credential is sent. Diagnostics and captured
headers are redacted before any append-only write; redaction after retention cannot remove a secret.

## 4. Mail: IMAP, MIME tree, draft and SMTP submission

### Read and retain

The IMAP connection witness includes normalized server authority, TLS peer identity, account scope,
mailbox name, protocol/capability edition and the selected mailbox's `UIDVALIDITY`. A message
continuity key is `server authority + account scope + mailbox binding + UIDVALIDITY + UID`; message
sequence numbers are only positions. If UIDVALIDITY changes, mint a new mailbox incarnation and
record a correspondence decision rather than reusing old message subjects. Message-ID, `References`,
`In-Reply-To`, sender address and subject are evidence used for threading, not globally unique
identity.

Fetch raw RFC 5322 message bytes and protocol attributes as an owned artifact. Parse the message
into an addressable semantic tree: envelope/header occurrences, decoded header values, sender and
recipient occurrences, date, Message-ID and threading assertions, MIME multipart nodes, leaf parts,
content type/charset/transfer encoding/disposition, attachments, flags and mailbox membership. A
message in two folders has two membership occurrences; it does not get two message identities.
Preserve the exact raw message when retention policy permits it. Keep raw bytes, transfer-decoded
bytes, charset decoded text and sanitized display text as distinct representations. Record available
mappings from decoded ranges back to raw ranges; never claim that HTML or character normalization
preserves byte positions without a mapping.

Connection defaults to read-only mailbox selection. Changing flags, moving, deleting and sending are
different intents. A failed body-part fetch leaves a partial message observation with a gap record;
it does not erase successfully fetched headers or report completeness.

### Draft and submit

A draft is a local immutable revision with structured From/To/Cc/Bcc recipient occurrences, subject,
`In-Reply-To`/`References`, body alternatives, MIME parts, attachment evidence references and exact
payload manifest. Revisions are separate: prior authorization does not transfer after a recipient,
attachment, body or audience change. MIME generation serializes the reviewed revision before
admission, and the retained manifest identifies the exact serialized bytes submitted (subject to the
privacy/retention choice).

Use the message submission service (normally SMTP submission) over TLS with hostname verification.
Model the SMTP transaction as ordered stages: EHLO and negotiated extensions, authentication
mechanism, MAIL FROM, each RCPT TO accept/reject response, DATA acceptance, and final server
response. Keep a recipient-specific outcome. A final successful response means the submission server
accepted responsibility under SMTP; it does not prove inbox placement or end-user delivery. If the
connection drops after message data is sent but before the final response, mark `outcome-unknown`;
do not retry automatically. An IMAP Sent-folder match can be reconciliation evidence only where that
server's behavior is known, not a universal idempotency guarantee.

### MIME implementation choice

Do not hand-roll MIME parsing, RFC 2047 header decoding, RFC 2231 parameter decoding, multipart
boundary handling or transfer decoding. Two viable pkg-config libraries are:

| Library  | pkg-config module | Useful scope                                                                                 | Recommendation                                                                                                                                         |
| -------- | ----------------- | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| GMime    | `gmime-3.0`       | C/C++ parser and builder for RFC message/MIME trees; raw byte preservation and serialization | Preferred MIME layer if the distribution's package is available; test raw fidelity and decoded-range limitations                                       |
| LibEtPan | `libetpan`        | C mail framework with IMAP, SMTP, POP, NNTP and RFC822/MIME parser/builder                   | Candidate single mail transport/parser dependency; first verify modern TLS, auth/OAuth mechanisms and required IMAP extensions in the selected release |

GMime is specifically a MIME parser/builder, not a network transport. LibEtPan advertises IMAP/SMTP
and MIME support, and its project documents the `pkg-config libetpan` invocation. Either choice is
substantially safer than a private parser. Decide transport and parser independently behind
`MailReadAdapter`, `MailSubmitAdapter` and `MimeCodec`; don't bind the evidence model to a library
object tree.

The implementation now declares both libraries as build, package and CI dependencies. GMime parses
and serializes messages. LibEtPan parses the IMAP dialogue and literal bodies over a bounded custom
stream; curl supplies asynchronous connection establishment and verified TLS. SMTP submission uses
curl with GMime's exact serialized bytes. The IMAP bootstrap uses an HTTP(S) connect-only URL with
the original mail host and port, so curl's IMAP greeting/authentication machinery cannot consume the
dialogue owned by LibEtPan. Workers return owned observations and protocol outcomes, never library
object trees or store references. Native mail acceptance remains subject to the controlled protocol
tests and interface journeys in the stage ledger.

## 5. IRC: live TCP/TLS session, history and send

The connection witness comprises network/server authority, TLS identity, configured account scope,
connection incarnation, nick claim and negotiated IRC/IRCv3 capability edition. Nicknames are
mutable presentation names, not subject identities. Persist channel membership, topic observations,
message events, replies, reactions/redactions when supported, sender/account evidence, server-time
tags and locally observed order as distinct semantic nodes.

Connection setup parses `CAP LS 302`, requests only understood capabilities, waits for ACK/NAK,
performs an explicitly configured SASL mechanism when granted, and sends `CAP END` before
registration completion. Never send SASL credentials without verified TLS unless the user explicitly
configured a trusted transport. Parse IRCv3 tags and BATCH state before extracting events. Bound
line and tag bytes, batch nesting, queued events and reconnect time.

Where advertised, use `CHATHISTORY` with BATCH, server message IDs and server-time for bounded
cursor-based catch-up. Store the server's capability advertisement, request interval, ordering rules
and end-of-history marker as coverage. History support is opportunistic: a server may omit old
messages, expire IDs or lack the extension. Reconnect then creates an explicit gap occurrence; it
never claims that the live stream or a successful `LATEST` request is complete. De-duplicate by
server-scoped message identity while preserving each delivery occurrence. Two authored identical
strings remain two messages.

Join, part, topic/mode changes, private messages, channel messages and moderation actions have
separate typed intents and authority. A send receipt can contain the local admission, server
error/numeric, and an `echo-message` echo if negotiated. Server acceptance or echo does not prove
every channel member received/read it. A socket break after write but before acknowledgment is
uncertain and must not trigger an automatic resend. IRC line splitting, multiline capability
negotiation and byte-count rules are protocol behavior, not UI string truncation.

## 6. Fediverse: Mastodon REST role and ActivityPub identities

The first adapter implements a configured Mastodon-compatible REST account, not universal
ActivityPub federation. Use the already-linked libcurl HTTP transport for bounded GET/POST
operations, TLS verification, pagination and redirect reauthorization. Keep the API base URL and
instance authority in the connection revision. Mastodon account and status numeric IDs are instance
local. Preserve them scoped by authority. Where present, also retain canonical actor and object URIs
as ActivityPub identity evidence; don't collapse two Mastodon IDs on different hosts. Record the
evidence mapping among REST account ID, account URL/actor URI, REST status ID and status URL/object
URI as a versioned correspondence claim.

Reading produces separate actor, account observation, object/status, activity, reply relation,
attachment, visibility/audience, and collection-membership records. A status body edit produces a
new observation/edition for the same asserted object subject. A `Create` or `Update` activity is not
the same subject as its object; a `Delete` activity and optional `Tombstone` are separate evidence,
while the local current view can point at a tombstone. Keep `Like`, `Announce`/boost, `Follow`,
reply and delivery as their own activity or occurrence records. Duplicated fetches keep
observation/delivery provenance.

Page through REST collection links only within the allowed origin. Each page stores request/response
evidence, page cursor, acquisition time, and partial or failed state. IDs are deduplication keys
only under their instance/actor scope; page traversal does not prove a coherent source snapshot.
Preserve visibility, content warning, attachment and recipient/audience fields as distinct
addressable records. Private scopes require the corresponding read capability.

Posting, replying, boosting, favouriting and deleting are distinct action variants. Bind the exact
visibility, reply target, account, content-warning, media/attachment evidence and recipient choices
to the intent revision. Honor the Mastodon REST `Idempotency-Key` semantics if supported by that
instance, but retain its limited server lifetime and scope; it is a mitigation for ambiguous
retries, not global exactly-once. On a response, retain the REST status/activity identities and
server response. Record `accepted` separately from remote federation delivery. Do not claim delivery
to every recipient.

ActivityPub client-to-server posting to an actor's outbox is a different protocol adapter and
authenticated role from Mastodon's REST API. Server-to- server inbox delivery additionally needs
actor ownership/signature verification, HTTP signature/key rotation, recipient and collection
expansion, delivery queues, replay protection and federation semantics. It is out of Stage 3. The
REST adapter may parse ActivityPub IDs/types/links as identity evidence and preserve
`Create`/`Update`/`Delete`/`Tombstone` relations exposed by the server, but it must not pretend to
implement server-to-server ActivityPub by POSTing arbitrary JSON.

## 7. Controlled protocol tests

All tests use loopback-only controlled servers or injected byte streams, local test certificates and
synthetic credentials from a test-only credential broker. They never require user accounts or
contact public services. Assert requests/transcripts and the resulting semantic records; test
failure states and durability, not just successful parsing.

### Mail fixtures and servers

- Raw MIME fixtures cover nested `multipart/mixed`, `alternative` and `related`; encoded display
  names/subjects; folded and duplicate headers; base64 and quoted-printable; RFC 2231 filenames;
  inline CID parts; empty parts; malformed boundaries; charset conversion; and oversized/deep trees.
- A scripted localhost IMAP server tests login/auth negotiation, TLS hostname failure, read-only
  selection, UIDVALIDITY+UID identity, message sequence renumbering, changed UIDVALIDITY, partial
  BODY fetch, flags and reconnect gaps. Assert per-message and mailbox coverage records.
- A scripted localhost SMTP submission server tests TLS, authenticated scope, recipient-specific
  RCPT rejection, DATA rejection, successful final response, disconnect before/after DATA and
  non-resend recovery. A connection drop after complete DATA but before final status must reopen as
  `outcome-unknown` and must not issue a second transaction.
- Draft tests pin serialized bytes, Bcc visibility, MIME alternatives, attachments, invalid address
  refusal and intent revision changes. No test sends to a real mailbox.

### IRC server

- A local TCP/TLS scripted server negotiates CAP versions and returns partial ACK/NAK, optional SASL
  mechanisms, tags, BATCH and CHATHISTORY. Test CHATHISTORY missing/expired cursor, duplicated and
  out-of-order batches, message ID reuse under a new network incarnation, and live/history overlap.
- Test channel and direct message reads, topic/membership changes, server-time and msgid
  preservation, missing history as a gap, rate-limit errors, line-byte boundary behavior, TLS
  verification and disconnect before/after write. Explicitly assert that a lost send acknowledgment
  remains unknown after reopen and is not replayed.
- SASL credentials come from a fake in-memory broker; assert they never appear in journal text,
  retained bytes, diagnostics or exported evidence.

### Mastodon-compatible HTTP server

- A loopback HTTP/TLS fixture serves account, status/context and paginated collections, returning
  changed pages, duplicate IDs, a failed later page, redirects to an ungranted origin,
  edits/deletions, attachments and differing visibility. Verify page-level completeness, scoped ID
  mapping, activity vs object subjects and redirect denial.
- POST fixtures inspect exact content, visibility, reply ID, media references, bearer-token scope
  and Idempotency-Key. Simulate acceptance with a lost response and show the reopened intent/attempt
  unresolved without resend; simulate duplicate-key retry within/outside the documented retention
  window and retain the limits of the guarantee.
- ActivityPub JSON fixtures include `Create`, `Update`, `Delete`, `Tombstone`, `Like`, `Announce`,
  actor aliases and collection recipients. Verify explicit actor/object/activity links and
  same-origin correspondence assertions. These are parse/identity tests only, not
  federation-delivery tests.

### Shared cross-adapter invariants

- Concurrent recovery admits one writer; if the admitted owner vanishes after remote acceptance, no
  lease expiry or missing receipt permits a second ordinary send.
- Revocation, expiration, account switch and target/redirect change between queue and worker start
  block I/O. Credential text never enters persisted intent/attempt/receipt records, output, logs or
  exports.
- Cancellation fences stale worker results from a new arena/case, but late results enter the
  original durable attempt inbox when authenticated. A late acceptance does not revive the
  continuation or dispatch downstream work.
- Notes on raw and decoded MIME spans, message/header/recipient relationships, IRC gaps, status
  visibility, activity, attempt, receipt, error and connection schema survive close/reopen. Every
  test has at least one annotation target below the top-level message/event/status.
- Host workers return only owned DTO values. Destruction of the source case or execution arena
  during a read cannot produce a use-after-free or an unbounded accumulated batch.

## 8. Build and delivery order

1. Land shared typed DTOs, credential/capability lease boundary and typed intent/receipt payloads
   without a live network adapter. Add serialization, round-trip and secret-redaction tests.
1. Add MIME codec and raw fixtures. Select GMime (`gmime-3.0`) or LibEtPan (`libetpan`) only after
   validating platform package availability, license, TLS/auth feature coverage and exact-byte
   behavior. Keep the MIME codec replaceable.
1. Add read-only mail, IRC and Mastodon adapters one by one with controlled server tests, bounded
   owned batches and durable local promotion.
1. Add typed draft builders and outbound adapters, routed through common EffectRuntime admission and
   receipt persistence. Test unknown outcomes and recovery before exposing UI controls.
1. Expose connection creation, read/coverage inspection, annotation, local draft review and
   separately authorized send through the xuzz interface. Run `ux-validation` for close/reopen
   offline and resume journeys. CLI or test-server reachability is groundwork, not the Stage 3
   interface pass.

Do not start broad VQL keywords as the first integration. Library functions can construct typed
requests and workflows can compose them once suspension, coverage, authority and retained lineage
are proven. Stage 3 is complete only when the §8 Stage 3 journeys are usable through xuzz and the
protocol, recovery, privacy and annotation tests above pass.

## 9. Protocol references

- [RFC 9051, IMAP4rev2](https://www.rfc-editor.org/rfc/rfc9051.html): mailbox UIDs and UIDVALIDITY
  define stable per-mailbox message identity; sequence numbers are mutable positions.
- [RFC 5322, Internet Message Format](https://www.rfc-editor.org/rfc/rfc5322.html),
  [RFC 2045, MIME Part One](https://www.rfc-editor.org/rfc/rfc2045.html), and
  [RFC 2046, MIME Part Two](https://www.rfc-editor.org/rfc/rfc2046.html): raw message syntax and
  MIME bodies/parts.
- [RFC 6409, Message Submission](https://www.rfc-editor.org/rfc/rfc6409.html) and
  [RFC 8314, TLS for Email Submission and Access](https://www.rfc-editor.org/rfc/rfc8314.html):
  submission role and TLS posture.
- [IRCv3 capability negotiation](https://ircv3.net/specs/core/capability-negotiation-3.2.html),
  [SASL 3.2](https://ircv3.net/specs/extensions/sasl-3.2), and
  [CHATHISTORY](https://ircv3.net/specs/extensions/chathistory.html): capability discovery,
  authentication negotiation and bounded history behavior.
- [Mastodon statuses API](https://docs.joinmastodon.org/methods/statuses/) and
  [OAuth scopes](https://docs.joinmastodon.org/client/authorized/): account REST posting, scoped
  authorization and the bounded Idempotency-Key behavior.
- [W3C ActivityPub](https://www.w3.org/TR/activitypub/): client-to-server outbox requests and
  server-to-server inbox delivery have distinct roles; activities and objects are separate
  identities.
- [GMime project](https://github.com/jstedfast/gmime) and
  [LibEtPan project documentation](https://etpan.org/libetpan.html): viable MIME parser/builder and
  mail-protocol implementation candidates.

## 10. Current engine-groundwork boundary

The communication retention layer accepts typed observations and stores source-shaped mail, IRC, and
ActivityPub records. Its durable case-derived indices now make mailbox UID, IRC message-ID and
ActivityStreams URI identities reusable across observations. Repeated identities with matching
payloads keep one canonical entity and append distinct observation or delivery records; conflicting
payloads are refused during preflight. When a later observation supplies an actor or object for an
earlier unresolved relation, an annotation-capable resolution record points to both the original
relation and the known entity. The identity checks and resolution index are reconstructed from
retained semantic records after reopen. Controlled tests should continue to cover repeated fetches,
conflicts, and late resolution across close/reopen before these adapters are treated as complete.

The native MIME parser retains the exact complete input message. GMime's object tree does not
provide original per-part source byte slices, so `decodedBytes` is transfer-decoded content and
`decodedText` is GMime's charset-converted text; neither is a substitute for the full raw message.
Outer, MIME-part, and embedded-message headers preserve occurrences and raw/decoded values. Charset
conversion availability is attached to each text part as an explicit status. A parser must not
present a regenerated part serialization as exact source bytes. The MIME library exposes the whole
original message but does not currently provide faithful source byte slices per part.
