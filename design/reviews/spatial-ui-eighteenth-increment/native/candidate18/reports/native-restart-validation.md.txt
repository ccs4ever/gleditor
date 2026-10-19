# Eighteenth-increment native restart validation

The frozen candidate preserved one UI-created pinned download request across process restarts and an
explicit Retry. The request returned to the same genuine no-connection failure, with its request ID
and full immutable pin unchanged. This run establishes local request durability only; no remote
content availability, signature rejection, or interrupted-transfer behavior was tested.

The tested executable was `build/validation-eighteenth/frozen/xuzz`, SHA-256
`9dc89cd770437c5bf93da0645ff2f8101d9ee179a2890fd5e15afafda9153c91`; `libgleditor.so.0` was
`ea4d46e75ce858119626f470045c3cc555956ad2bd4ca9425f3aeffe3e831ce9`; `xudu-dump` was
`7031847f950135234935ae67d06c3e59da6a945bd95a5202f14591064f00947c`. The isolated profile began with
only `data/` and `config/` copied from the sixteenth-increment UI-authored workspace, plus empty
cache and GNUPG directories. All app invocations exited 0 under Xvfb, X11, software OpenGL, and a
private D-Bus session.

The UI-created request ID was `319507aabff5973c66527f4a3398ef35750615642c3cd4ce25b751a9cbf1af28`.
Its exact pin was publisher `4c6e92c9b3e4abbd75ca25d8fc7cfaa3dc9d45ef2b8fa700e79f3cfca81abf79`,
salt/name `document`, carrier hash `f8ff688c5680ab88c27e4297decb5e7640b86a31`, sequence `2`, version
`a34`, and title `document`. Strict decoding of the post-exit default-hex LMDB dumps validated the
`XPI1` envelope and exact 21-field record.

The first UI process created and failed the request with
`No publication connection is configured. Content availability has not been checked.` The
verification state was `Unchecked`. On the first restart, five ordinary Right inputs selected the
full-pin row in Ctrl+O. Return opened the choice list rather than submitting it, so this launch is
retained as a menu-selection attempt and not counted as an opened status form. The next launch used
the same selection followed by Tab and Return; the restored request opened in `Failed` with the same
pin and no Refresh or Retry action. Its post-exit inbox record still had attempt `0`.

The explicit Retry launch began from the confirmed `Failed` form. Right selected Retry and Return
submitted it. The internal accessibility snapshots recorded `Queued`, then the scheduled Return
refresh showed the real no-connection failure again. The post-exit inbox record still had the
original request ID and pin, now at attempt `1`, phase `Failed`, verification `Unchecked`. A
subsequent restart showed the same full-pin Ctrl+O row and reopened the same failure form without
automatic requeue; the record remained at attempt `1`. Two later read-only UI launches were used
only to inspect the expanded download list.

Seven app invocations are retained: initial create/fail; a restart whose Return opened the Document
choice list but did not submit; a restart that opened the restored failure form with Tab then
Return; explicit Retry and refresh; restart after Retry; and two read-only visual-list inspections.
The non-submitting choice-list attempt is preserved separately in the argv, log, exit, and frame
evidence. The successful ordinary sequence is Ctrl+O, five Rights, Tab, Return.

The request index was decoded after graceful exits for the initial failure, restored failure,
post-Retry failure, and final post-Retry restart. Those checkpoints show one request throughout;
attempt changed only from `0` to `1` on the explicit Retry. The source operation dump, source
`ops.nodes` and `store.tables` hashes, and package-visibility bytes match the initial checkpoint.
Activity decoding reports one completed visit and current visit `1` both before and after; no
byte-identity claim is made for activity metadata.

At 1440×1000, the selected Ctrl+O download row visibly fits its complete title, version, sequence,
40-character carrier hash, and Failed phase after ordinary Down navigation scrolls it into view.
Opening the choice list initially shows only the first five options; the failed row is lower in the
list. The failed-status panel fits its error text and all fields. Its publisher control visibly
shows the first half of the key; the internal accessibility snapshot lists both key halves, and the
persisted record contains the complete key. Internal `--dump-a11y` snapshots are included in the
preserved logs; this run makes no native AT-SPI claim because the frozen build reports accessibility
is unavailable.

Original PPM captures are archived as deterministic mtime-zero gzip streams with paired PNGs. Every
PPM/PNG pair was checked at zero RGB pixel difference and has per-frame PPM, archive, PNG, and RGB
hashes. The candidate archive includes argv, environment, binary hashes, actual exit codes, logs,
scripts, source/package/activity checkpoints, and decoded request-index dumps. It excludes profile
data, cache, GNUPG home, private keys, LMDB database files, and executables.

The native interrupted-transfer variant remains unrun because no real publication transport was
configured. The no-connection outcome does not establish remote unavailability.
