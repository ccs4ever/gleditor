# Controlled effects in an evidence case

Stage 2 adds controlled fake actions, guarded SQLite writes, argv process execution and pasted shell
history to the private evidence case. Open Xuzz's case panel with Ctrl+Alt+Shift+E, then select
Work. The same actions are available at the `vquery` prompt through `:evidence`.

## Draft, review, grant, dispatch

A draft is an immutable request. Review shows its retained inputs without executing them. A grant
authorizes that exact intent in the current host session, with an expiry. Dispatch is a separate
action. Changing the request requires a new draft and grant. Closing a case revokes its grants;
reopening a retained grant record does not restore authority.

The engine saves an attempt before starting external I/O. Acceptance and terminal receipts are
separate evidence: the delayed fake can accept an action before its final result is known. A crash
after admission or acceptance can leave an unknown outcome. Recovery records that uncertainty and
never resends automatically. An independent retry that might duplicate an earlier effect requires
explicit acknowledgement: type `DUPLICATE` in Xuzz, or use `acknowledge-duplication` at the prompt.

Pending work shows recent effects before imported history. Scroll to older records, or use the
Evidence filter to find an intent, attempt, acceptance, receipt or output. These are ordinary
records and can be annotated through the existing Notes screen.

## Terminal examples

Start with an open private case. Each command below follows `:evidence`:

```text
draft-fake "fake://review" 500
review INTENT_ID
grant INTENT_ID 300000
dispatch INTENT_ID GRANT_ID
poll
pending
```

The fake target is controlled by the adapter and contacts no external service. Append `unknown` to
the draft command to exercise an uncertain outcome. `cancel-work` requests cancellation; a late
acceptance or receipt can still arrive. Late results retain the original document identity. If that
case is unavailable or replaced, results enter a private receipt inbox for later reconciliation.

SQLite patches require an existing database, one bound write, one scalar guard and an exact typed
expected value. The adapter checks the guard and affected-row count in the same transaction and
rolls back a mismatch. For example:

```text
draft-sql "/absolute/billing.db" "UPDATE invoice SET amount=? WHERE id=?" "SELECT amount FROM invoice WHERE id=?" i:95 i:105 i:1042 --guard i:1042
```

Parameter prefixes are `n:` for NULL, `i:` for an integer, `r:` for a finite real, `t:` for text and
`b:` for hexadecimal blob bytes. This preserves integer, text, NULL and blob distinctions in the
guard. Review and grant the returned draft before dispatching it. A stale guard leaves the database
unchanged; an uncertain commit remains an unknown outcome.

Process arguments are separate values; the executable supplies `argv[0]`. No shell is inserted:

```text
draft-process "/usr/bin/printf" "/tmp" "%s" "literal $HOME; no shell"
```

The executable is pinned by a digest, the working directory is explicit, and stdout/stderr are
retained separately. The engine API also supplies explicit environment and stdin values, execution
deadlines, output limits and termination grace. Output truncation is partial; an unconfirmed process
or descendant outcome remains unknown. The current native process adapter supports POSIX hosts;
Windows execution is explicitly unsupported.

History import retains distinct occurrences, including duplicate commands. Pasting a command into
history does not grant or dispatch it. Unavailable timestamps, directories, exit statuses and
session identities remain explicitly unknown. The import is bounded by bytes and occurrence count.

## Persistence and scope

The evidence-case controller holds a lifetime writer lock for each private case. Saves stage and
flush snapshots, preserve existing scroll bindings, install monotonic tables before operations and
atomically replace the operations snapshot. Both admission and receipt storage failures are
surfaced. The native store format is unchanged.

These controls do not claim remote exactly-once execution. SQL, process and fake receipts have their
own meanings. Communications delivery and resumable workflow orchestration belong to later stages;
their implementation progress is tracked in [the stage ledger](vql-stage-progress.md).
