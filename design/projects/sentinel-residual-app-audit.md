# Residual application sentinel audit

This is a follow-up map for optional-absence work after the first API migration. It records
candidates and their domain boundaries; it does not authorize changing packed records or wire
layouts.

## Link endpoints and focus

| Candidate                                                                                             | Meaning and safe API direction                                                                                                                                                                                                                                                                                                                     | Consumer scope and checks                                                                                                                                                                                                                                          |
| ----------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `UniversalLinkEnd::cell()` in `apps/common/xanadu/universal_link_endpoint.hpp`                        | A document endpoint has no ZigZag cell. Return `std::optional<CellRef>` based on the existing `kind` discriminator. Keep the 16-byte `UniversalLinkEnd` fields and compact transclusion records unchanged; this is an accessor migration, not a representation change.                                                                             | `apps/common/xanadu/link_layout.cpp`, `apps/common/ui/xanadoc/beams.cpp`, and endpoint/spanfilade/universal-transclusion tests. Preserve document-versus-cell behavior and test a cell reference of zero only as invalid input if the endpoint factory rejects it. |
| `LinkContext::CellFocusQuery` and `ReadingStamp::cell` in `apps/common/ui/xanadoc/link_context.hpp`   | The callback uses `noCell` when the ZigZag pane has no focus. Make its result and the stamp field optional together. The `present()` query in `link_context.cpp` should receive only an engaged cell.                                                                                                                                              | `link_context.cpp`, callback wiring in `apps/xuzz/xuzz_app.cpp`, equality/revision tests and any UI code comparing reading stamps. Cover absent focus, a present focus, and movement between the two.                                                              |
| `SatelloidNeighbor::cell`, `CellSatelloid::cellRef`, and `SatelloidOverlay::Presentation::Slot::cell` | Neighbor and active satelloid records identify cells and should stay required. The slot's `noCell` default is only pre-draw initialization; `drawn` already records its lifecycle. Keep the sentinel local unless slot lifecycle is redesigned as a unit.                                                                                          | No migration advised in the current pass. If revisited, check slot creation, selection, and retirement together rather than treating the default alone as absence.                                                                                                 |
| `FlyingTetherAnchor::cellRef` in `apps/common/ui/xanadoc/tenuous_tether.hpp`                          | The field defaults to `noCell`, is assigned by `beams.cpp`, and has no reads in the tether implementation. `targetKind` plus `targetId` already carry the tether identity. Prefer removing this redundant field and its assignment, subject to source-compatibility expectations for this public aggregate; do not add a second optional identity. | `apps/common/ui/xanadoc/beams.cpp`, `tenuous_tether.hpp`, and aggregate construction sites. Check document and cell tether creation/removal.                                                                                                                       |

The `link` members on `UniversalLinkedPair` and `UniversalHalfLink` are required link identities in
emitted results, not optional endpoints. The `LinkTargetKind` tag is the external endpoint
discriminator and remains the authority for whether a target is a document or a cell.

## Pouch and reading-place state

`DropZoneConfig::cell`, `DropZone::cell()`, `DropZoneSpec::cell`, and the `addPouchZone` output are
now optional. A zone has no cell until its setting cell is found or minted. `SettingEntry::nameCell`
remains required for an entry that was read from the settings rank. Add/retain a regression for
default zones, successful minting, and refusal when a zone cell cannot be created.

`ReadingPlace::zigzagFocus` is now optional. `CellRef` is `uint32_t` and `noCell == 0`; the
visualizer initializes focus to zero and treats it as unfocused. Therefore zero is invalid, not a
valid persisted focus. The reader maps legacy non-positive scalar values to absence, the writer
refuses an engaged non-positive focus, and xuzz capture maps `noCell` to `nullopt`. Keep a test that
zero is refused and absence round-trips; a positive cell focus must round-trip as present.

## Swarm listen ports

`SwarmContentSource::Options::listenInterfaces` is a configuration string. Its default `0.0.0.0:0`
intentionally requests an OS-selected ephemeral port; the zero there is a valid request and must
remain accepted. The public `SwarmContentSource::listenPort()` getter reports the actual bound port.
Zero from that getter means the listening session is not ready yet, because a successfully bound TCP
listener cannot report port zero. A future getter can return `std::optional<uint16_t>` for readiness
without changing the request syntax.

`PublicationOutbox::listenPort()` currently forwards an atomic initialized to zero and the abstract
`PublicationTransport::listenPort()` default also returns zero. Those zero values mean no transport
port is available, not a request for an ephemeral port. If converting these getters, update the
virtual boundary, forwarding transport, outbox atomic, and tests together. Keep the configured swarm
interface separate from the bound-port result. Verify that polling before worker readiness is absent
and that readiness yields the non-zero actual port; existing swarm tests already wait for a non-zero
port before connecting.

## ZigZag focus still using zero

The remaining broad UI absence migration is `ZigzagVisualizer` focus. Here `CellRef` is `uint32_t`
and `noCell` is zero; the focus field uses zero to mean that a slice has no selected cell. This is
distinct from `FocusTarget::birthOp` zero, which deliberately names the legacy root Xanadoc and
should stay a raw required value when its kind is Xanadoc.

The focused API and state scope is:

- `ZigzagPresentationSurface::focusCell()` query and setter in
  `apps/common/xanadu/zigzag/presentation_surface.hpp`, with the implementation in
  `apps/common/ui/slice/zigzag_visualizer.hpp`. Both directions need typed absence; xuzz's callback
  wiring and `BridgeCoordinator` must avoid activating a cell when the query is absent.
- `ZigzagVisualizer::focusCellId()`, the private `accursed_cell_focus_`, and
  `reloadStoreVersion(..., newFocus)` in `zigzag_visualizer.hpp/.cpp`. Delete/navigation currently
  set zero when no successor exists (`nextFocus.value_or(0)`); preserve that state as nullopt and
  update guards, render-state indexing, command-bar actions, and inspection paths to act only on a
  present focus.
- xuzz consumers in `apps/xuzz/xuzz_app.cpp`, `LinkContext` and `BridgeCoordinator`, plus test
  surface implementations in `tests/xuzz/xuzz_vortex_test.cpp` and visualizer/presentation tests
  under `tests/zigzag/`. `ReadingPlace::zigzagFocus` and the LinkContext callback and stamp have
  already moved to optional values; wire their focus query directly instead of converting back to
  zero.

This is a wider migration than the endpoint accessors: the internal focus field participates in
rendering, navigation, deletion, Vortex actions, and many tests. It should land as one complete
API/caller change with regressions for an empty slice, deleting the last focused cell, restoring a
present focus, and an absent focus remaining absent through a reader stamp. Avoid changing required
results such as `createCell()` IDs or a successfully promoted cell.
