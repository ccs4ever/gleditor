# Increment 21 native restart compatibility inspection

## Result

The frozen increment-21 app opened the existing failed publication request from an isolated copy of
the increment-20 profile, displayed the persisted request, and exited normally. This is a
restart/state compatibility check only. It does not exercise a successful installation, the new
native-size query through the UI, transport preflight, or a budget admission path.

The copied profile retained the exact pinned request: publication name `document`, version `a34`,
sequence `2`, carrier `f8ff688c5680ab88c27e4297decb5e7640b86a31`, publisher
`4c6e92c9b3e4abbd75ca25d8fc7cfaa3dc9d45ef2b8fa700e79f3cfca81abf79`, salt `document`, phase `Failed`,
verification `Unchecked`, and attempt `1`. The persisted error remained “No publication connection
is configured. Content availability has not been checked.” The form showed `Refresh progress`
selected, with `Retry download`, `Discard partial data`, and `Close` available. I closed the form
and app without selecting Retry or Discard and without opening the publication.

## Run and evidence

The run used frozen app `build/validation-twenty-first/frozen/xuzz`, library `libgleditor.so.0`, and
`xudu-dump`. Their SHA-256 tuple is recorded in `evidence/frozen-binary-tuple.sha256`; the app hash
matches the requested frozen hash. The run copied only the prior run's `data/` and `config/` into
`native-compat/`, and used fresh `cache/`, `gpg/`, and `runtime/` directories. It ran under a
private Xvfb display with SDL X11, dummy audio, and software GL.

The ordinary keyboard route was Ctrl+O, move to the existing failed `[Download]` entry in the
Document chooser, open it, inspect the status form, Escape, and Ctrl+Q. The exact argv, environment,
exit code, and full log are `argv.txt`, `environment.txt`, `exit`, and `logs/restart.log`. Exit code
was **0**. The internal UI accessibility dump named the Action, Verification, Requested carrier,
Requested edition, Publication name, and Requested publisher controls. The build log also reports
`accessibility: not in this build`, so this is not a native AT-SPI/accesskit validation.

The raw 1440×1000 frames are in `raw/*.ppm.gz`; lossless PNG copies are in the same directory.
`evidence/rgb-verified-frames.txt` records hashes and confirms exact decoded RGB equality for both
frames.

## State checks after close

- The post-exit LMDB dump decoded as XPI1 with exactly one request and exactly 21 request fields.
  Its normalized dump SHA-256 matches the copied pre-run dump. The complete pin, phase, error,
  verification value, and attempt count match the pre-run snapshot; no Retry or Discard transition
  occurred. See `checkpoints/before-inbox.json`, `checkpoints/after-inbox.json`, and
  `checkpoints/inbox-mdb-dump.txt`.
- The source publication's `ops.nodes`, `store.tables`, and package-visibility hashes match the
  copied baseline. The source publication operation rendering has an empty diff. Hashes are in
  `checkpoints/source-package.sha256`; operation diff is `evidence/source-ops.diff`.
- Activity decoding reports one completed visit and current visit 1 both before and after. No visit
  was completed by this inspection. The activity operation dump did gain ordinary selection and
  view-state operations while using and closing the app; the decoded visit count/current marker
  remained unchanged. Full dumps and decoder output are in `evidence/`.
- The copied profile's LMDB file bytes and active permascroll file bytes changed during ordinary
  open/close, so their raw-file hashes are not represented as immutable. The inbox journal's
  normalized contents and request record remained identical, and source/package operations above
  remained unchanged.

This smoke is evidence that the failed request, full pin, and attempt survive an ordinary restart
and inspection. It is not evidence of a successful local import or install-size reconciliation.
