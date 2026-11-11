# Workflow-user critique, round 1

The six source areas, field/span annotation, explicit identity continuity, shell-history
distinction, private credentials, offline evidence and unknown-outcome recovery are materially
covered. Engine conformance correctly refuses unavailable service semantics; these findings concern
the proposed contracts rather than a claim of tested integration. Three missing contracts remain:

1. **Late completion must not erase action evidence (high).** §4.1 says “reject late completions
   after cancellation or generation change,” whereas §4.2 requires durable receipts and later
   reconciliation of unknown outcomes. If I cancel a mail or shell attempt, close its view, and the
   remote acceptance arrives afterward, dropping that result leaves me unable to distinguish
   “cancelled before execution” from “actually sent.” Fix: fence stale arena/UI mutation separately
   from receipt ingestion. An authenticated completion for a durable attempt must enter that
   attempt's evidence/reconciliation inbox even when its original execution generation has ended.
   Record late-arrival/cancellation causality; do not resume a cancelled continuation or dispatch
   downstream work automatically. Acceptance: cancel, destroy the session, deliver success, reopen;
   the old attempt shows the late receipt, with no new send or stale arena write.

1. **A completed pagination walk needs a scan-consistency contract (high).** §5.2 makes each page
   annotatable and failed pages incomplete; §4.3 disclaims universal atomic snapshots. Neither says
   whether a resumed page-three fetch can be combined with yesterday's first two pages and then
   labelled complete. In a changing ticket collection this can omit or duplicate resources even when
   every request succeeds, producing a wrong billing conclusion. Fix: define a page-set/scan record
   with request/filter edition, pagination lineage, source snapshot/validator when available,
   acquisition interval, overlap/dedup policy and stability guarantee. “All pages fetched” and
   “complete coherent collection” must be separate claims. A stale continuation either starts a new
   scan edition or produces an explicitly mixed-time result; earlier annotations stay pinned.
   Acceptance: insert/delete/reorder records between page requests and restart; inspect the exact
   coverage limitation instead of a false complete count.

1. **Concurrent offline draft edits need an explicit dispatch conflict boundary (medium).** §4.2
   binds dispatch to an exact intent revision and §4.3 supports branch/resume, but only source rows
   get a concrete base/proposal/current conflict workflow (§6.1). If a laptop changes a mail's
   audience while an offline desktop changes its body, automatic combination can create a message
   nobody reviewed as a whole. Fix: specify local draft/annotation/workflow-definition merges
   independently of external-source reconciliation. Concurrent intent revisions remain separate;
   merging creates another reviewable revision and invalidates prior dispatch authorization unless
   its explicit scope covers that exact result. Notes retain authorship and alternatives rather than
   last-writer erasure. Acceptance: edit body and recipients on disconnected devices, merge, and
   verify no queued attempt dispatches a synthesized revision under the old approval.

These additions fit the existing subject/observation/intent model and ordinary library/schema
contracts. No additional connector syntax or account access is needed.
