# Independent source review: publication inbox persistence

I reviewed the increment-18 working-tree candidate based on `bce65b1f` after the latest
journal-consistency, retained-path, and staging-recovery fixes. This was a read-only source and test
review; I did not build or run the tests. The root-owned build and focused test result should be
recorded separately when it finishes.

The implementation now preserves the request ID, full pin or mutable pointer floor, phase,
verification evidence, and bounded error in an LMDB journal. It loads the journal before starting
the worker, turns interrupted work into an explicit retry state without creating a transport, and
persists submit, retry, cancel, pointer advancement, and manifest verification before announcing
those transitions. A verified mutable request resumes from its immutable verified pin; the
submitted-origin `exactPin` bit is not a retry-routing mode. Retries use separate attempt
directories, and completed retained stores are locally checked against the signed publication before
becoming Ready.

The source changes address the earlier findings: verified journal stages require the manifest hash
and signed title/sequence/version to agree with the full pin; Ready and Complete must agree;
retained roots are recursively checked for symlinks before use; and a recognized interrupted attempt
directory is preserved without being imported as an installed snapshot. The new tests cover a
verified stage with a missing manifest hash, an interrupted attempt containing a staged manifest, a
nested published-store symlink, interrupted mutable retry with a preserved lower bound and no
startup transport, and recovery of a matching installed orphan without transport.

I found no remaining blocker in the reviewed state-transition and recovery paths. The main remaining
validation gap is journal corruption coverage: current tests exercise unknown version, request-count
overflow, and a malformed verified-stage record, but do not yet provide the proposed mutation matrix
for truncated bencode, duplicate IDs, unsafe IDs, pin/lower-bound conflicts, and other malformed
fields, with typed refusal and byte-preservation assertions for each case. A process-kill test at
every write/rename boundary and injected journal-write failure coverage are also absent. These are
coverage gaps rather than observed failures in the inspected code.

The request and journal sizes are bounded, but failed attempt directories are retained across
retries and there is no explicit aggregate staging-disk budget or attempt-retention policy. Repeated
large failed attempts can therefore consume additional disk until managed externally. The design
intentionally preserves diagnostic staging data; a future cleanup policy should only remove a
directory after proving it is an incomplete attempt and should remain bounded and visible.

Finally, LMDB transaction commits serialize the journal and retain the previous valid transaction on
reported write failure, but the install rename and parent directory are not explicitly fsynced.
Power-loss durability around the attempt-to-installed-root rename remains an open gate, as
previously identified; the process-restart recovery tests should not be described as proving that
gate.

## Root validation follow-up

After this review, a test-only mutation matrix was added without changing the reviewed product
source. The final expanded owning publication/package suite passes 70/70 and verifies exact
journal-record and retained-file preservation for truncated data, duplicate and unsafe IDs,
pin/minimum and verified-identity conflicts, and invalid Ready/Complete pairs. This closes the codec
mutation-coverage gap above. The process-kill, write-failure, aggregate staging and power-loss
limits remain distinct from that passing check.
