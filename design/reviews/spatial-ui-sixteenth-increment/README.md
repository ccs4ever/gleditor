# Sixteenth spatial UI checkpoint

Based on `49b820774cd19ccca1978e6ccbe156682d857a46`, this increment adds explicit comparison and
switching of reader drawing between verified immutable editions from the same curator and package
name. Retention and emphasis remain independent for each carrier. The old selected Link, both
endsets, Left/Right cursors and frozen Origin remain pinned to their original authority. A switch
does not select the candidate Link or enter an endpoint.

## Implementation

The existing package Review offers Compare reader editions. The comparison shows the full baseline
and candidate hashes, package name, sequence, member counts and independent preferences. Baselines
are retained, verified carriers with the same curator and signed name, sorted by sequence. Titles
are not identity. Reviewing does not retain or enable the candidate. Retain candidate without
enabling is explicit; Switch drawing to candidate requires a retained candidate and an enabled
baseline. Keep current drawing closes without mutation. Changing the baseline selector refuses the
attempted switch and refreshes the selected pair with visible recovery guidance. The reader must
review that pair before a further switch.

`ReaderLinkPackages::switchEnabled()` validates both exact payloads, curator/name, distinct hashes,
different sequences, retention and the enabled baseline. One existing atomic preference save changes
only the two enabled bits. Failure preserves both durable and live choices; unrelated carriers and
both emphasis values survive. Explicit switching back to an older retained edition is supported. No
publication high-water state, authored operations, wire format or preference format changes. No new
view/camera or simulated acquisition service is introduced.

The old Review form could inspect a locally superseded carrier but refused its explicit Enable
because Session accepted only Ready/Published status. A shared phase-eligibility predicate now
permits local Superseded/NeedsVerification as well, followed by the existing cryptographic review
and carrier hash check. Received candidates in those phases remain ineligible. Failed status and
tampered signed content still refuse. Eligibility alone never establishes signature validity or
curator enrollment.

## Validation

Canonical GNU Make, repository format-check, lint and UI text policy pass. Final fresh headless UI
241/241, Xuzz 107/107 and owning reader-package 11/11 pass. Tests cover exact switch/revert,
unrelated choices, same-sequence conflicts, missing payload, atomic-save refusal and unchanged
preference bytes. The UI owning case uses a newer one-left/one-right package against the old
two-left/three-right Link: switching retains the old exact context without a source write or
completed visit. Signature tampering and Failed status refuse.

Unchanged library and library-test hashes permit reuse of 799/799 and mip/cache 53/53 evidence from
the previous checkpoint; these are not new library runs. Strict GL/GLES/Vulkan comparison passes
unchanged criteria: GL/GLES ordinary and overlay match exactly, Vulkan differs by 0.5104% / 0.5096%
under the existing 1% limit and channel tolerance 1. Configured one/four-thread frames match; the
small sample does not exercise split recording. Broad E2E and optimized performance are excluded.

## Actual native controls

The isolated OpenGL run authors and signs a local publication and independently prepares real
packages without network announcement. Public signing fingerprint:
`AA0D9FFDC8E23A594AFEE17153BB5133913BFC89`. Cryptographic endpoint verification is separate from
unchecked curator enrollment and referenced-publication availability.

| Carrier                                    | Signed package name              | Sequence | Role                                  |
| ------------------------------------------ | -------------------------------- | -------- | ------------------------------------- |
| `28861784988338a27c410bb256e631a1a1f115af` | `curations:linkscurations:links` | 1        | Same-title unrelated negative control |
| `04c7483b1e7274664e72104dca5a1eb97919cee8` | `curations:links`                | 1        | Old selected baseline                 |
| `722911c0021f2b2352c5efc76c2d683088d6cbac` | `curations:links`                | 2        | Explicit switch candidate             |

All use the title Spatial review. The first name was accidentally appended to the existing form
default; it is preserved as a different-name negative control, never treated as an edition baseline.
A third `curations:links` edition, `f876ea31376a13935390edb1e0838e8ad2bca9f0`, is prepared through
the same controls for the refresh guard.

Frames 00–21 and 27–28 use the frozen fifteenth app `d791c7ac…`; frames 22–26 and 29 onward use the
pre-review candidate `31fb47db…`. Frames 38–43 use `98610d53…`, whose added status exists in
accessibility but is clipped below the visible form. Final compact-form frames 44–49 use the
separately hashed accepted executable. Their source and binary provenance are separate. Historical
filenames are not acceptance claims:

- 00–04 and 07–09 author/sign/prepare/review the unrelated carrier. No 05/06 captures exist.
- 10–13 retain, enable, quiet and select that carrier. 14 actually prepares the correct-name #1;
  15–16 inspect that unretained candidate, with no baseline failure established.
- 17 explicitly selects correct-name #1 with Left 2/2, Right 3/3, active Right. 18–19 prepare #2.
- 20–21 actually toggle normal then quiet while #1 stays enabled. Their names overstate the step.
- 22 disables #1. 23 reviews #2 without retention; 24 retains it disabled. 25 is a refresh with both
  editions disabled, not a drawing switch. 26 shows the old disabled pinned context.
- 27 disables #1 on baseline; 28 actually reproduces the Review Enable refusal. 29 succeeds on
  candidate with the same exact signed old carrier.
- 30 shows #1 enabled/quiet and #2 disabled/normal. 31 explicitly switches to #2 enabled/normal and
  #1 disabled/quiet, with both retained. The unrelated carrier stays enabled/quiet throughout.
- 32 keeps the old selected Link and exact pins; 33 restores that same context after restart.
- 35 compares third edition against #2 and lists the two matching baselines; the unrelated name is
  excluded. 36 retains #3 without enabling it. 37 refuses switching after changing the baseline
  selector without refreshing. The enabled #2 and other preferences stay unchanged.
- 38–43 exercise a first recovery-form fix and #2→#3 switch/restart. Its recovery and final trust
  rows are clipped, so accessibility text alone does not establish visible recovery.
- Final 44–49 use a compact seven-row form, with recovery immediately below Action and concise
  verification text before the edition summary. They exercise visible refusal, refreshed pair,
  explicit #3→#2 drawing switch-back and restart of the old #1 pinned context.

All captured operation checkpoints preserve one completed visit/current 1, unchanged source
operation dumps and actual `ops.nodes`/`store.tables` hashes. Shutdown reading-place metadata
belongs to the private activity store and is not another completed visit. Lossless PNGs retain
source PPM and RGB hashes; full logs, argv, checkpoints and scripts are archived without profiles,
keys or binaries.

## Remaining gates and next task

Independent visual review confirms the bounded switch and old-context persistence. The pre-review
refusal was diagnostic-only and its summary truncated trust explanations. The final compact form
puts recovery below Action, shows curator key parts and package name, and states endpoint
verification, unchecked curator and deferred cited-byte checking before the edition summary through
the existing form seam. Native package visuals on GLES/Vulkan, native AT-SPI, curator enrollment,
invalid-signature and unavailable-referenced-content variants remain unverified. The native editions
have equal endpoint counts; changed endsets are covered by the owning UI test.

Continue the publication/pin-lifecycle audit with distinct native trust and referenced-content
availability outcomes and a fully visible comparison explanation. Do this before D2 history
enumeration. Full J24/J25, cell/history/media-position variants and complete J1–J32 acceptance
remain open, along with optimized performance and existing reading-layout stress limitations. See
the status document for the broader dependency order.
