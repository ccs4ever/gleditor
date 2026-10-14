# Fifth increment: bounded multipage link-entry evidence

These two native OpenGL runs used the pinned `xuzz` binary SHA-256
`52dadabe6cfee1e5994a5e2e297ea35b6e9e6eef150f7f68012e9ff3a949cdf7` at 800×600. Each run authored an
80-line, 7,439-byte source through the UI, selected `[0,7439)`, created a distinct 60-byte target,
linked the selections, and captured the link preview. The full escaped command is in each run's
`argv.txt`; the complete session output, source snapshots, activity dump, and PNG preview are
retained beside it. Raw PPM frames and profile stores remain outside Git.

## Forced page break

`80-line-forced-break/source-before-0.txt` records operation 3 as `kind=pagebreak at=3719`; the
input sequence used the default `Ctrl+Return` chord between lines 40 and 41. This establishes that
the selected source range crosses an actual authored page break. The captured preview's
accessibility tree nevertheless reports both source and target as page 1/1. The run logged a real
click on the visible Enter button at `(327,463)` and tagged it to document 0/page 6/cluster 15. It
ended before an entry frame or a post-entry accessibility state was recorded, so this run does not
establish whether entry reached the source document.

Source snapshots are unchanged across the run:

- Source document 0, before and after:
  `281be2bdb2ea4cc831d2a24d69574860f20e47e49aac82fad146b494e9e1e96c`.
- Source document 1, before and after:
  `90ae27b411491dd9c1473e56cee7db9146198951d0f2dfc6dc4ef60c1462d1f3`.

The activity dump contains records 1–75 and no new completed visit. This was not a complete journey:
continuation scrolling, Origin, Back, and fold were not reached.

## Ordinary 80-line source

`80-line-normal` used the same 7,439-byte source and selected range without an explicit page-break
chord. Its preview also reports page 1/1. The run logged a real Enter-button click; the 60-second
accessibility wait timed out without finding `Doc 2=selected`. Before/after hashes match: document 0
`5031de24986621a29a06e47efea57e5098338c18b9f729cc9a9874d76835fc3c`; document 1
`6dd94955578cc69580a50c3ce1618e89470895e6e82789cbe5090302b0fb3b39`. Activity-after likewise contains
no new completed visit.

## Conclusion and limits

The fifth multipage journey did not pass: neither bounded run produced evidence of an entered
document, so no cross-page continuation or return behavior was validated. These observations
identify a preview/reporting and/or bounded-entry follow-up candidate, but do not prove a deadlock
or isolate a product defect. Only OpenGL at 800×600 was sampled. The separate 500-line attempt
exceeded its bound and is not used as evidence here.

`SHA256SUMS` records hashes for the packaged files in each run directory. `run-metadata.txt` records
the binary hash, selected byte/line counts, target size, backend, and viewport.
