# Sixteenth increment native persistence review

The review covers the real OpenGL UI run under Xvfb, with each run isolated under
`build/validation-sixteenth/native/{data,config,cache,gpg}`. It uses two immutable application
phases. Frames 00–21 and 27–28 were captured with baseline `xuzz` SHA-256
`d791c7ac9b97ade8572aa67c2949735fd4c03ce75780c90829a05ed9cd4e6b5a`; frames 22–26 and 29–37 used
candidate `xuzz` SHA-256 `31fb47dbbc267d88298c2b0b44122ea4a6bf9118a1728270d4683cb0796531df`. The
linked library and dumper hashes are recorded in `baseline-record.txt` and the build hash files.

The first package attempt was misnamed. The script typed `curations:links` into a field already
containing `curations:links`, so carrier `28861784988338a27c410bb256e631a1a1f115af` is actually
`curations:linkscurations:links`. It is preserved as a negative control and is not part of the
controlled same-name comparison. The correctly named baseline carrier is
`04c7483b1e7274664e72104dca5a1eb97919cee8`; candidate #2 is
`722911c0021f2b2352c5efc76c2d683088d6cbac`. Both use package title `Spatial review`, name
`curations:links`, and the same curator. The subsequent third edition is
`f876ea31376a13935390edb1e0838e8ad2bca9f0`.

| Checkpoint | Native evidence                   | Persisted finding                                                                                                                                                                                                                                                                                                                                                                                 |
| ---------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 10–13      | `checkpoints/10…13`; frames 10–13 | The misnamed negative-control carrier was retained, enabled, set quiet, and selected.                                                                                                                                                                                                                                                                                                             |
| 14–16      | `checkpoints/14…16`; frames 14–16 | Correct-name baseline #1 was prepared at 14. Frames 15–16 inspected an unretained Ready candidate; neither frame shows an error or an enable refusal.                                                                                                                                                                                                                                             |
| 17–21      | `checkpoints/17…21`; frames 17–21 | Frame 17 explicitly selected #1 with left 2/2 and right 3/3 pins. The later exact-name candidate was prepared at 18–19. Frames 20–21 toggle normal then quiet while #1 remains enabled; they do not show a refusal.                                                                                                                                                                               |
| 22–26      | candidate phase; frames 22–26     | Frame 22 disables #1. Frame 23 compares #2 before retention; frame 24 retains #2 disabled. Frame 25 refreshes the comparison without switching the drawing, and frame 26 retains the old pinned Link context.                                                                                                                                                                                     |
| 27–28      | baseline phase; frames 27–28      | Replaying the baseline path reproduced the refusal. The UI named #1 as retained, disabled, quiet; the log says `Related publication unavailable: Verify the package before enabling it`.                                                                                                                                                                                                          |
| 29–32      | candidate phase; frames 29–32     | Frame 29 enables #1 on the candidate. Frame 30 shows #1 enabled/quiet and #2 disabled/normal. Frame 31 performs the actual explicit switch: #2 becomes enabled/normal and #1 disabled/quiet. Frame 32 retains the existing reader context.                                                                                                                                                        |
| 33–37      | candidate phase; frames 33–37     | After restart, the UI restored the existing left 2/2 and right 3/3 context. The preference ledger still has #2 enabled and #1 disabled. The comparison dialog explicitly identifies #2 as the retained baseline for the third edition. Frame 37 shows the real refusal message `Reader edition unavailable: Refresh comparison for the chosen edition before switching`; it did not switch to #3. |

Across checkpoints, the activity ledger still records one completed visit, current visit 1, payload
`visit3 0 doc db139f57d99fc5b2ce504a9e834eb489 0 0 0 0 0 0`, and no reference or annotation entries.
The activity store can append shutdown reading-place metadata, so its full operation dump is not
byte-identical across process exits; those metadata appends do not add a completed visit or change
the current visit. The decoder checked checkpoints 10–37 and passed the visit/reference/ annotation
assertions. The authored source store’s `ops.nodes` SHA-256 remains
`ac9f7eed1c1b6ee7b3cb17b33c52e1eeb4859c6eefb6b5b5a1419b381b0f0843`; `store.tables` remains
`fc15f548fddd5e8b4cb642ff53d8fc18ecdf00b02499fd08eecba1b0a2b919b4`. Every checkpoint has the same
source operation dump hash and the same two source-file hashes.

Checkpoint 33’s restored UI context is the pre-existing left 2/2 and right 3/3 context; its link
panel still reports a retained, disabled, quiet reader package. The separate persisted preference
ledger identifies the controlled pair: #2 is enabled, normal, and retained after restart, while #1
is disabled, quiet, and retained. Checkpoint 35’s actual comparison dialog names `Spatial review #2`
as the retained edition. The restored link context and the selected comparison baseline are distinct
observations; the UI’s disabled quiet context does not itself identify #2 as the active drawing.

The corrected decodes are in `decoded-checkpoints.json` and `decoded-preferences.json`. Their source
checkpoints, raw logs, exit codes, argv, and frames are kept alongside them. Frames 00–09 document
the initial UI run, including the misnamed package; none are discarded or relabeled as successful
exact-name evidence.

## Reviewed guard phase (38–43)

Frames 38–43 use the reviewed guard candidate binary, SHA-256
`98610d53d732c28f635deee03a9b6c49886a1e1a8e50f41f75ef0334498d1480`; the shared library and dumper
match the previous phase. The third edition’s actual carrier ID is
`f876ea31376a13935390edb1e0838e8ad2bca9f0`.

| Checkpoint | Native evidence                             | Persisted finding                                                                                                                                                                                                                                                                                        |
| ---------- | ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 38         | Frame 38; reviewed-visible-guard-switch log | Comparison reviewed candidate #3 against retained/enabled baseline #2. Endpoint signature was verified; curator enrollment remained unchecked.                                                                                                                                                           |
| 39         | Frame 39; same log                          | Changing the retained edition to #1 made the old comparison stale. The accessibility dump reports “Switch refused. Pair refreshed; review before switching.” The frame clips the status row below the seven visible fields, so this phase does not establish that the refusal text is visually readable. |
| 40         | Frame 40; same log                          | The accessibility dump reports the refreshed comparison returned to #2 and status “Review pair before switching.” The same status-row clipping limits visual confirmation.                                                                                                                               |
| 41         | Frame 41; same log                          | Explicitly switching the reviewed candidate succeeded: #3 enabled/normal, #2 disabled/normal.                                                                                                                                                                                                            |
| 42         | Frame 42; same log                          | The previously pinned Link context remained visible after switching, as the comparison dialog promised.                                                                                                                                                                                                  |
| 43         | Frame 43; reviewed-restart log              | After restart, #3 remained retained/enabled/normal and #2 retained/disabled/normal in the preference ledger. The prior Link context remained available.                                                                                                                                                  |

The activity visit ledger and source document invariants still pass for checkpoints 38–43: one
completed visit/current visit 1, no references or annotations, and identical source operation/file
hashes. Shutdown reading-place metadata may append to the activity store without adding a visit. The
decoded outputs cover checkpoints 10–43. The failed first launch attempts for this reviewed phase
are also retained with exit code 126 and the permission-denied log; the reruns completed with exit
code 0. The 98610d phase provides functional evidence for the refusal and successful switch, while
the clipped status presentation remains unresolved here. The binary hash is unchanged by the
execute-permission correction. A later visual-fix phase is pending.

## Final compact guard presentation phase (44–49)

Frames 44–49 use the accepted compact-guard build, SHA-256
`19792347b9f012782b71d4906fc61a35fd064e9c86dc5caee926e3bd11980310`; the library and dumper retain
the same hashes. The earlier 98610d guard phase is preserved as an intermediate result: its
accessibility state was correct, but its refusal row clipped in the frame.

| Checkpoint | Native evidence                             | Persisted finding                                                                                                                                                                                           |
| ---------- | ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 44         | Frame 44; accepted-visible-guard-switch log | Compared candidate #2, retained/disabled/normal, against baseline #3, retained/enabled/normal. The action and status rows fit in the panel.                                                                 |
| 45         | Frame 45; same log                          | Choosing baseline #1 invalidated the comparison. The UI displays “Switch refused. Pair refreshed; review before switching.” I inspected the 1440×1000 frame and confirmed the entire status row is visible. |
| 46         | Frame 46; same log                          | After refresh, candidate #2 and baseline #3 are shown with status “Review pair before switching.”                                                                                                           |
| 47         | Frame 47; same log                          | The explicit switch succeeds: #2 is enabled/normal and #3 is disabled/normal.                                                                                                                               |
| 48         | Frame 48; same log                          | The prior pinned Link context remains available after the switch.                                                                                                                                           |
| 49         | Frame 49; accepted-restart log              | Following restart, the persisted preference ledger still has #2 retained/enabled/normal and #3 retained/disabled/normal. The established left 2/2 and right 3/3 reader context remains available.           |

The actual third-edition carrier is `f876ea31376a13935390edb1e0838e8ad2bca9f0`. Across checkpoints
10–49, the activity visit ledger still decodes to one completed visit/current visit 1 with the
original payload and no references or annotations. Shutdown reading-place metadata may append to the
activity store, so full activity operation dumps can differ; the source operation digest and both
source store file hashes remain unchanged. `decoded-checkpoints.json` and `decoded-preferences.json`
contain the full 10–49 sequence. The compact phase resolves the visual clipping recorded for frames
38–43: the refusal text is now readable in the captured UI, and refresh followed by explicit review
allows the subsequent edition switch.
