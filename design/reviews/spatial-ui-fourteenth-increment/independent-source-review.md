# Independent source review: disabled package pins

Read-only source review at `337c04727ba167f226e2ccb6f451b1e0d1cc7b2d` with the current working diff
and later source updates. I read the repository instructions, Xuzz UI/navigation and UX validation
skills, the navigation workflow, the J24 package lifecycle contract, and the thirteenth-increment
lifecycle audit. I made no product or test changes. This is source evidence, not a native J24
journey or a build/test result.

## What the implementation now does

- `ReaderLinkPackages` separates enabled lookup (`package()`/`find()`) from payload lookup
  (`retainedPackage()`). `LinkContext::resolve()` uses the latter for the exact selected package, so
  disabling drawing can keep both endsets and their occurrence cursors resolvable.
- Package identity is immutable at this boundary: `ReaderLinkPackages::keyOf()` derives the
  `LinkKey.authority` from the carrier `InfoHash`, and `id` is the link ordinal. The key overload of
  `setEnabled()` resolves that authority back to its hash and requires the exact payload to remain
  retained. A same-title package with a different carrier hash has a different key; enabling the
  selected key cannot select a newer edition by title.
- The authority reverse map is now keyed by `DocumentId`, ordered over its fixed 16-byte value.
  Selected-pin checks (`carrier`, `enabled`, `containsAuthority`) use this map directly and do not
  format authority strings on the per-frame disabled query. `retain()` and preference loading still
  detect a derived-authority collision before accepting two hashes for one key.
- `LinkContext::packageVisibilityChanged()` no longer dismisses the selected package link.
  `selectedPackageDisabled()` gates endpoint `Enter` and `EnterAt`; the panel adds a disabled notice
  and a re-enable control, labels entry “Enter unavailable,” and clears selected-range styling. Its
  disabled Enter action has no focusability or accessibility actions, and the owning regression
  verifies `performAction(Click)` refuses it. `LinkBeams` suppresses selected-link emphasis and
  anchor focus while disabled. The test verifies selection/cursors, current visit, activity
  operations, and source-store operations survive disable, then verifies re-enable and explicit
  Dismiss.
- The expanded inspector reports the full carrier hash and, for each selected package member, the
  signed scroll and source byte range from the retained package, followed by the observed exact
  document/cell site when an occurrence is chosen. This keeps package source coordinates out of the
  native registry-reference path. The key and retained payload are checked before indexing the
  package link/member vectors.
- Queued beam accessibility commands capture `session.generation()` in `describe()` and are
  discarded by `drawFrame()` when the session generation changed. The owning UI regression queues a
  beam action, disables and re-enables the package before queue delivery, dismisses the selection,
  then delivers the queue and verifies selection remains dismissed with no new visit or source
  operations. This directly covers the stale accessibility event round trip. GPU picks also compare
  the current session generation to `builtFor` before resolving a strand.
- The owning regression checks no extra source operations and no activity-store operations for
  disable, refused Enter, re-enable, and Dismiss. It also checks a disabled selection can be
  restored in a second `LinkContext` in the same `Session` and retains its side/member/occurrence
  pins. The latest test source also starts a real pending page entry, disables the package, then
  builds/advances the page and re-enables; it asserts the pending entry was cancelled and no late
  visit or source operation is committed.

## Remaining gaps and limits

- **User-visible carrier identification and native appearance remain unproven.** J24 step 5 asks for
  hidden drawing with the inspector retained and explicit re-enable/dismiss choices; it does not
  require a spatial ghost marker. The owning test confirms the disabled notice and accessible
  Re-enable control and disabled Enter refusal, while source state retains the selected `LinkKey`
  and both endsets/cursors. The expanded inspector now includes the complete carrier hash. This
  source review has no captured native frame to assess its complete cursor presentation or visual
  quality. A quiet beam ghost should not be added as a requirement absent from the contract.
- **Exact edition behavior is established at the data-model boundary, with limited workflow proof.**
  Hash-derived authority means different immutable package carriers cannot alias through a mutable
  name, and the selected-key setter only re-enables that retained hash. `ReaderLinkPackagesTest`
  retains a same-title, newer sequence and asserts its key differs and it is not enabled. It does
  not prove the rendered inspector still identifies the old carrier during that update. Do not claim
  this user-visible variant from the model test alone.
- **Model restart is covered; full Session/UI restart is not.** `ReaderLinkPackagesTest` reloads the
  durable hash-keyed preferences, proves a missing payload is not trusted until the exact package is
  retained again, and proves a newer same-title carrier has a different key and is not enabled by
  retaining it. The changed UI test restores selection in a second `LinkContext` over the same
  `Session`; it does not close/reopen Xuzz or restore the disabled selection from
  `system://activity`. Startup reconstructs payloads from `LinkPackageExchange::statuses()` when
  preferences exist. Full restart evidence should verify the exact disabled key/cursors, missing
  payload status, and re-enable after restoration; it must never resolve by same-title package.
- **Missing payload retains a generic unavailable state, without inventing native references.** If a
  restored selection has no resolved occurrences, `appendInvestigation()` continues before the
  `LinkContext::reference()` fallback, so the package cannot reach the native
  `SourceRegistryMismatch` path. `LinkContext::apply()` reports that the saved package is disabled
  or unavailable and retains the exact activity choices; the carrier hash can still be identified
  from preferences. Without the verified payload, signed member spans cannot be displayed. A full
  restart journey should still check that the generic unavailable message is visible and that
  re-enable is refused until the exact package is verified again.
- **No separate Retain or Emphasis preference was added.** The durable package file remains an
  enabled boolean keyed by `InfoHash`; enabling still retains the package, and reader Emphasis has
  no independent state/control. The new API separates in-memory retained-payload access from enabled
  drawing, but it does not implement the independent reader inventory described by the prototype.

The source-level immutability checks support the narrow disabled-pin behavior, and the owning test
provides no-write/no-visit coverage, stale accessibility-action rejection, and cancellation of a
pending page entry before it can commit. Model preference reload and exact carrier separation have
existing unit coverage. The lead reports a successful native e8c local workflow with a genuine Ready
package, enable, exact member browsing, disable, retained inspector, blocked pending entry, and
unchanged visits/source stores; this review did not run that workflow. The latest build7 test run is
in progress, so no result is claimed here. Complete J24/J25, native visual assessment and full
Session/UI restart remain open, as does separate Retain/Enable/Emphasis acceptance. The new
Re-enable action uses the existing navigation command boundary, keymap action registry, accessible
panel callback, and default chord; it does not add a second command path.
