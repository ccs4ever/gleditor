# Twentieth-increment native zero-byte discard validation

The native Xuzz flow ran the explicit Discard partial data action on the existing UI-authored
publication request when no partial-attempt files were present; the form reported zero logical bytes
removed and kept the request. After a graceful restart, the same full-pinned request remained
available with its original `Failed` status, unchecked verification, attempt count, and
no-connection error. No retry was selected or submitted.

The run continued the exact-pinned request originally submitted through the eighteenth-increment UI
journey and reopened in the nineteenth-increment restart smoke. It copied only that run's `data/`
and `config/` into a fresh twentieth-increment profile; cache and GNUPGHOME were newly created. The
frozen app at `build/validation-twentieth/frozen/xuzz` has SHA-256
`4b6e1a1386fa842f1715d1e93d9bf9582af5e62336e612cfa01a4fa15080a71d`; its library hash is
`ea4d46e75ce858119626f470045c3cc555956ad2bd4ca9425f3aeffe3e831ce9`, and the store dumper hash is
`7031847f950135234935ae67d06c3e59da6a945bd95a5202f14591064f00947c`. Both app processes ran under
private Xvfb/X11 at 1600×1200, software OpenGL, private D-Bus, isolated XDG directories, and an
empty GNUPGHOME. Both exited 0.

The first process opened the saved download request. Before submission, the Action field was set to
`Discard partial data`; the captured UI tree listed that selected value and the other available
actions. After Enter and Refresh, the form said “Discarded 0 logical file bytes of partial data;
request and completed copies kept.” The settled 1440×1000 frame shows the three-line explanation,
all six fields, and keyboard footer. The earlier Pending frame is retained as an intermediate
capture; it has a partial redraw with lower fields absent and is not used as a layout acceptance
check. A pixel check confirms the initial capital `D` is present in the cleanup result. The
transient cleanup notice did not reappear after restart.

After the second process exited, read-only `mdb_dump` output decoded as one XPI1 record with exactly
21 fields. The request ID remains
`319507aabff5973c66527f4a3398ef35750615642c3cd4ce25b751a9cbf1af28`; its exact pin remains publisher
`4c6e92c9b3e4abbd75ca25d8fc7cfaa3dc9d45ef2b8fa700e79f3cfca81abf79`, salt `document`, sequence `2`,
version `a34`, title `document`, carrier hash `f8ff688c5680ab88c27e4297decb5e7640b86a31`. Phase is
still `Failed`, verification is `Unchecked`, attempt is `1`, and the error remains “No publication
connection is configured. Content availability has not been checked.” The inbox directory contains
only LMDB files; no `.attempt-*` file is present. The displayed cleanup result and filesystem
inventory both report zero partial bytes.

The source document's `ops.nodes`, `store.tables`, package-visibility, and published-manifest hashes
match the nineteenth-increment checkpoint. The hashes for all saved link-package files also match,
and its permascroll-backed operation dump is identical. Activity decoding reports one completed
visit and current visit `1` before and after. Activity op dumps gain shutdown metadata, so they are
not claimed byte-identical. The archive includes all seven original PPM frames as deterministic gzip
streams, PNG views, per-frame PPM/PNG/RGB hashes, both exact argv/environment captures, full logs
and exits, frozen binary hashes, strict LMDB dump/decode, source/activity checkpoints, and exact
helper scripts as text. It excludes profiles, key material, LMDB database files, and binaries.

This validates the native zero-byte discard route and restart state. The captured failed form is for
this zero-staged-byte request; this journey does not visually validate a nonzero partial-data
cleanup state. It does not claim cleanup of nonzero staged data, a successful installation, remote
availability, or a signature-verification result; the separate engine validation covers nonzero
cleanup.

Evidence: [native archive](native/zero-discard/), [provenance](native/zero-discard/provenance.json),
[visual frame check](native/zero-discard/visual-check-05.txt).
