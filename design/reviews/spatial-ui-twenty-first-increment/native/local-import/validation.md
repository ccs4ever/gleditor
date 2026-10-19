# Increment 21 UI local signed-import validation

## Result and scope

The ordinary Open Document form's Custom path field opened the existing, locally retained, signed
`document.xanadoc`. The frozen increment-21 app logged
`xudu: read "document" by 4c6e92c9… seq 2, 79 bytes in 1 pieces, 1 links as a34 (complete store 9)`
and opened the restored text and link context. This path calls `Session::readPublication` and
`installPublication`, so it exercises the new exact native-size query and output-size reconciliation
in a real local import.

This was a **local signed import from retained files**, not an Inbox download or network test. No
transport was configured or mocked. It does not validate network preflight or whole-request budget
admission.

## Run and UI evidence

The run used the frozen increment-21 tuple recorded in `checkpoints/binary-tuple.sha256`, with a
separate copy of the increment-20 `data/` and `config/` profile and fresh `cache/`, `gpg/`, and
`runtime/` directories. It ran with a private Xvfb display, SDL X11, dummy audio, and software GL.
The interface route was Ctrl+O, choose `Custom path or file...`, Tab to the Custom path field, type
the full existing manifest path, and Enter. Exact argv, environment, exit, and logs are saved in
`argv.txt`, `environment.txt`, `exit`, and `logs/local-import.log`. Exit code was **0**.

`raw/01-open-chooser.ppm.gz`, `raw/02-path-entered.ppm.gz`, and `raw/03-imported-publication.ppm.gz`
preserve the run captures. Lossless PNGs are adjacent; exact decoded RGB equality and file hashes
are in `evidence/rgb-verified-frames.txt`. The internal UI dump shows the imported
`publication-20261019-153034` document selected, its source text, and its link context. The build
reports `accessibility: not in this build`; this was not an AT-SPI test.

## Installed result and unchanged source

The imported native store is `data/xudu/xanadocs/publication-20261019-153034/`. Its logical file
sizes are `ops.nodes` **67,840 B** and `store.tables` **697 B**. The install completed through the
frozen `installPublication()` path, whose increment-21 code queries these native sizes before save
and verifies the actual two file lengths before the staging root rename. The offline dumper read all
36 operations. `evidence/installed-publication.header.txt`,
`evidence/installed-publication.ops.txt`, and `evidence/installed-files.txt` record the result.

The installed `published/` closure contains three verified dependency trees totaling **61,232 B** of
logical regular-file data. Each tree's relative file set and SHA-256 values exactly match the
corresponding source seed tree; see `evidence/retained-seed-closure.txt`. The additional fourth seed
available in the source folder was not part of this publication's reviewed dependency closure and
was not copied. The 1,871-byte signed manifest was the input to this local import and is not a file
in the installed native root.

The signed source manifest, source store `ops.nodes` and `store.tables`, and package visibility file
retain their input hashes; see `checkpoints/source-inputs-before.sha256` and
`checkpoints/source-inputs-after.sha256`. The source store operation dump has no diff from its
pre-run rendering. The earlier failed Inbox request also remains exactly the same in the XPI1
record, including its full pin, failed phase, unchecked verification, and attempt 1; see
`checkpoints/after-inbox.json`.

Activity decoding reports one completed visit and current visit 1 before and after this route. The
activity operation dump records ordinary view/selection state changes, but no new completed visit.
The complete before/after dumps and decode are in `evidence/`.
