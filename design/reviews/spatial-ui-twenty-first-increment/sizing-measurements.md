# Publication install sizing measurements

These read-only measurements review the proposed exact pre-write size query and the copy overlap in
`build/validation-twenty-first/staging-policy-plan.md`. No product code, build target, benchmark, or
transport fixture was changed or run.

The sample scan covered all 17 authored trees under `tests/samples/xudu` that contain both
`ops.nodes` and `store.tables`. For each, the frozen `xudu-dump --section=header` read the actual
on-disk OpsSegmentHeader fields and validated signature, format 2, 65,536-byte header, 64-byte node
size, `firstOpIndex=1`, and `nodeCount == present`. All 17 headers passed. In every sample the
logical `ops.nodes` extent equals `headerBytes + nodeCount * nodeSize`; the maximum recorded count
is 127. This is independent validation of the format relationship, not a count inferred from file
length.

The actual native writer uses that exact extent: `SegmentedOpsSpool::writeSegmentFile()` writes the
header and then `opCount * sizeof(CompactOpNode)` bytes. `Store::save()` calls it, then derives the
deployment segment list and calls `writeStoreTables()`. That writer emits the 12-byte signature, a
4-byte format version, and encoded bencode data. Its output varies with the deployment descriptors;
these samples range from 92 to 1,062 logical bytes. A size query for a prepared Store can therefore
be exact if it uses the spool's owned operation extent and the same canonical table encoder and
inputs as the writer. A separate estimate based only on op count would miss table size changes.

| Authored sample                     | Ops in header | `ops.nodes` logical / allocated file B | `store.tables` logical / allocated file B | Retained metainfo B | Retained payload B | `published/` allocated by `du` B | Native + twice retained logical B |
| ----------------------------------- | ------------: | -------------------------------------: | ----------------------------------------: | ------------------: | -----------------: | -------------------------------: | --------------------------------: |
| `core_hypertext/unified_store`      |           126 |                         73,600 / 4,608 |                                  92 / 512 |                   0 |                  0 |                                0 |                            73,692 |
| `multimedia/04_audio_doc`           |            17 |                         66,624 / 4,608 |                               402 / 4,608 |                 143 |            352,844 |                          158,208 |                           773,000 |
| `multimedia/05_video_doc`           |            17 |                         66,624 / 4,608 |                               391 / 4,608 |                 122 |             21,778 |                           31,232 |                           110,815 |
| `multimedia/06_embedded_media_page` |            28 |                         67,328 / 4,608 |                             1,062 / 4,608 |                 418 |            374,850 |                          207,360 |                           818,926 |
| `multimedia/11_floating_image`      |             2 |                         65,664 / 4,608 |                               387 / 4,608 |                 120 |                228 |                           10,752 |                            66,747 |

Across the nine multimedia sample trees with a `published/` root, repeated publication hashes are
counted once per tree because each tree is a separate authored store. Their retained dependency
files total 1,126,634 logical bytes (1,428 metainfo and 1,125,206 payload), with 605,184 bytes
allocated to files and 629,248 bytes from directory-inclusive `du -B1` in this workspace. This
corpus sum is not one request's size: several trees refer to the same hash. Within a request, count
each unique hash once, matching `PublicationInbox`'s dependency set.

The last table column is only a known install-peak lower bound: actual sample native files plus one
cache copy and one retained copy of each metainfo and payload. It excludes the manifest carrier, its
metainfo, manifest retained in the final request root, the journal, directories, and filesystem
allocation overhead. A separate UI-authored signed manifest from the prior native run measured 1,871
logical bytes and 4,608 allocated file bytes; it is a distinct artifact, not part of the sample
corpus, and does not provide the manifest metainfo size. The interrupted-request fixture's 1,360
logical bytes remain a failure-only observation.

The `allocated file B` fields sum `st_blocks * 512` for regular files. Directory-inclusive `du`
values are recorded separately. These allocated values reflect this worktree's current filesystem,
including sparse/packing behavior; they are not portable disk reservations. The admission budget
should be defined over checked logical file lengths, not treated as a filesystem quota. Existing
whole-tree sample totals include `ops.export`; install sizing should count only files the install
writer actually creates. For example, `core_hypertext/unified_store` is 79,075 B as a complete
sample tree but its native `ops.nodes` plus `store.tables` are 73,692 B.

The copy overlap is concrete in `PublicationInbox::Impl::download()` and `installPublication()`:
dependency payloads and metainfo remain in the attempt cache while verified seeds are copied under
`store/published/`, and the native store is saved before the cache is removed. The aggregate
installation reservation therefore needs both copies at peak, not just the final retained root.
Metainfo belongs in both copies as well as payload bytes.

The bounded implementation review should use the prepared Store and same writer inputs: report the
spool's exact output extent and encode `store.tables` with the canonical encoder; report each unique
retained seed's payload and `metainfo.torrent` logical lengths. Refuse a pure size query while Store
metadata is pending instead of sealing it or replaying/copying solely to measure. Have the installer
reconcile predicted logical file sizes against actual files before it marks the request Ready.

These measurements do not validate pre-payload signed-carrier admission, normal network reception,
or a user budget default. The aggregate plan's zero-byte default remains necessary; sample sizes are
not grounds for choosing a production allowance. The machine-readable 17-row table is
`sizing-measurements.csv` and the complete row data plus separate manifest measurement is
`sizing-measurements.json`; reproduction commands are in `sizing-commands.txt`.
