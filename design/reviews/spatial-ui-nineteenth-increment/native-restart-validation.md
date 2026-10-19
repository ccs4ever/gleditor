# Nineteenth-increment native restart validation

The frozen nineteenth-increment app reopened the eighteenth-increment UI-authored download request
without changing its pin or automatically requeuing it. The request remained `Failed` with
`Unchecked` verification and attempt `1`; the app exited normally. This bounded smoke test does not
exercise Retry, a successful installation, or remote availability.

The run used `build/validation-nineteenth/frozen/xuzz` (SHA-256
`82d45197ab5936cc83120d4918fe631d9e0e691490898146141d3716516b8fd9`), `libgleditor.so.0`
(`ea4d46e75ce858119626f470045c3cc555956ad2bd4ca9425f3aeffe3e831ce9`), and `xudu-dump`
(`7031847f950135234935ae67d06c3e59da6a945bd95a5202f14591064f00947c`). It ran under private Xvfb/X11
at 1600×1200, software OpenGL, private D-Bus, and isolated XDG data/config/cache plus an empty GNUPG
home. The fresh run directory copied only `data/` and `config/` from the eighteenth candidate
profile to continue its previously UI-authored request. The profile itself is not archived.

The actual UI sequence was Ctrl+O, open the existing download entry, inspect the failed form,
Escape, and Ctrl+Q. No Retry action was selected or submitted. The internal accessibility dump in
the compressed process log lists the full-pin download entry and the failed form. The form displays
`Not checked`, the carrier hash, edition `document / a34 #2`, publication name `document`, and
publisher key fields. At 1440×1000, all status rows and the no-connection explanation fit in the
captured failed-form frame.

After graceful exit, read-only `mdb_dump` output was strictly decoded as one XPI1 record with
exactly 21 fields. It retained request ID
`319507aabff5973c66527f4a3398ef35750615642c3cd4ce25b751a9cbf1af28` and the full pin: publisher
`4c6e92c9b3e4abbd75ca25d8fc7cfaa3dc9d45ef2b8fa700e79f3cfca81abf79`, salt `document`, sequence `2`,
version `a34`, title `document`, carrier hash `f8ff688c5680ab88c27e4297decb5e7640b86a31`. The final
phase is `Failed`, verification `Unchecked`, attempt `1`, with error “No publication connection is
configured. Content availability has not been checked.”

The source document operation dump and hashes for its `ops.nodes`, `store.tables`, and package
visibility file match the eighteenth-increment pre-run checkpoint. Activity decoding reports one
completed visit and current visit `1` both before and after. Activity operation dumps are not byte
identical: shutdown appended activity metadata, so no activity-store byte-identity claim is made.

Both lossless PPM captures are retained with deterministic gzip streams and PNG viewing copies.
Their RGB hashes match exactly. The archive at `native/restart-smoke/` includes the actual argv,
selected isolated environment, app logs and internal accessibility dump, actual exit, binary hash
tuple, strict LMDB text dump and decode, source/activity checkpoints, and frame hashes. It excludes
profile roots, private keys, LMDB database files, and binaries.

The genuine no-connection error says availability has not been checked. This run does not establish
remote content unavailability, signature verification outcome, or successful-install durability.
