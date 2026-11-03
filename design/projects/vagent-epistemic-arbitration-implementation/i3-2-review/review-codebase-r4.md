# I3-2 design review, round 4: compiler and codebase expert (persona codebase)

Scope: `i3-2-design.md` revision 4 (`d710e7a3`), `decisions.md` section 1 (R3-1 to R3-13) and my
round-3 report `review-codebase-r3.md`, against P19 v1.5 sections 2.1, 4, 5.1 and 8.2. Code at HEAD
`d710e7a3` (T0 `4a912c69`, E1 `0dc6bbad`). Confirmation round: I re-read the changed text of 3.1,
3.2, 4, 5.4 and 6.6 against the code, and spot-checked citations. Section 4 is read as design text
only. Time limit applied; nothing was run (read-only, no builds).

## Earlier findings

| Round-3 finding                      | Status   | Why                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------------------ | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 `counted` breaks the relay rule   | Resolved | 3.2 "`firstCharge` keeps its meaning (R3-3)": the lifetime counters and `last_contact` are charged as at HEAD (`agent_executor.cpp:809-824`), `Logged` gets no field, and the 3.8 test keeps `ARelayedRefusalDoesNotHideTheDirectCopy`. The estimator is gated by the host mark instead (R3-5), which is a different mechanism and does not touch `Logged`.                                                                                                                                                               |
| M2 a note cannot own two charges     | Resolved | 3.2 "Order": `PostBox::Item::charge`, `attachCharge` under the ingress lock, the open charge one per (actor, principal). The lock order ingress then post box is the one `observe` already follows (`posts_->push` inside the ingress lock, `agent_executor.cpp:3763`). The drain, `abandon`, `dropHostEvents` and reset sites are in the 5.4 table, and the refuse/accept/refuse test is in 3.8. A note already taken by `drainPosted` leaves its charge open and the drain takes it from the map, so no charge is lost. |
| M3 tombstones and the health reserve | Resolved | 6.6 charges the reserve to live profiles only (R3-1), gives the tombstone figure (7 cells bare, 3,510 fill the ceiling; my `tomb.cpp` number of 2,800 for 400) and the exempt headroom for tombstone actions.                                                                                                                                                                                                                                                                                                             |
| M4 `validate()` capacity rules       | Resolved | 6.6 and 4.1 drop them (R3-2); the four tree configurations (`:649`, `:893`, `:1046`, `vagent_settings_test.cpp:143-175`) keep working, the constructor throw sites are `:988` and `:1024` as cited. The capacity table states that 128 sources are not reachable once active, which is honest.                                                                                                                                                                                                                            |
| m1 to m11                            | Resolved | Section 12's codebase table maps each; I checked m1 (`putBack` silent, `agent_executor.cpp:659-663`), m3 (test lines `:276`, `:281-284`, `:302` exist as cited), m5 (append after `arenaSlots`), m6 (clamp below `ephemeralBit`, no `validate()` rule) and m7 (second `admit` for exempt turns). m11 stays an item for the lead (plan X-7), recorded in 3.6.                                                                                                                                                              |

## What is sound

- The mark design (3.2) is bounded in host memory. A stream is keyed by (actor, principal,
  incarnation, predicate), and `observe` refuses an unbound (pid, principal, predicate) or a
  non-current incarnation before any mark is touched (`agent_executor.cpp:3586-3628`), so the number
  of marks is at most bindings times one incarnation, and the predicate length is capped at 256
  bytes (`:3577`). Each `counted` ring holds at most `healthReplayRing` Int64s.
- Citations I re-read at HEAD resolve, including the 5.4 table's `abandon` sites (`:3877`, `:3885`,
  `:3894`, `:3900`, `:4008`) and the insertion (`:3927`): the `PostBox` methods, `drainPosted` and
  its `abandon` sites, `dropHostEvents`, `settle`, `orderlyStop`, `epistemicCellsWithin`, the
  checkpoint `HostDeliveryPending` line, `kIngestOrder` (`compiler.cpp:4079`), `rtSettle_`
  (`:4500`).
- Section 4's settle loop is expressible as designed. Every `compiler.cpp` entry ends in `rtSettle_`
  today (the `callLabel(rtSettle_)` sites at `:2427` to `:5633`), so no entry leaves `d.pending`
  linked between wakes and the new `SettlePending` checkpoint refusal cannot regress an existing
  checkpoint.
- The reset floor structure (5.4) is a separate in-flight multiset and shares nothing with the
  estimator mark; the lock order is stated and followed.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- **m1. 4.1 says the discard "cannot fail" and also describes a fallback for a refused `link`.**
  Keep one statement: the fallback (unlink the whole queue plus a WARN) is the documented path, and
  a test should force it (a `link` refusal on a stale ref), because 4.3's discard test does not list
  that branch.
- **m2. Section 4.3's price counts `1 + n` turns but 4.1 says each settle turn increments
  `$turn_count`.** State whether the per-wake budget of 5.3 sees the `$turn_count` increments (R3-7
  says every turn counts), so 2b's gate "a 16-event retire completes in one wake" is not
  contradicted by the control cap.

## Questions for the owner

None new. Sections 14's open questions stand.

Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Verdict by slice

- **2a: PASS** (R3-3 and R3-4 resolved; the mark is bounded).
- **2b: PASS** (section 4 as design text; the discard fallback test is m2).
- **2c1: PASS** (ordering and `abandon` sites listed).
- **2c2: PASS** (R3-1 and R3-2 implemented as ruled; the tombstone figure is stated).

Verdict: PASS
