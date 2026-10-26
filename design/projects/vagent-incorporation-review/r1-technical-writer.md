# r1 technical writer

## Verdict: NOT CONVERGED

The frozen `r1-input.txt` matches the current v3.8 specification. The R4 resolutions are otherwise
incorporated faithfully: §1/root and §18 give the same first-release boundaries and deferred work;
the companion recommendation note is identified as historical input; P1–P14 remain stated as
approved, with no deferred core proposal inheriting that approval. Version 3.8 is consistent between
the header and change history. I found one actionable status-table drafting error that should be
fixed before this incorporation passes review.

### MAJOR

1. **§1’s newly added status-table rows are split into malformed logical rows, misplacing the U-A9
   and test-harness status.** At `design/vagent-agent-language.md:96–97`, the busy-agent reclamation
   mechanism ends with “distinct from” and the next row begins “checkpoint/migration,” so the
   intended distinction and deferred status do not form one table entry. At `:99–101`, the
   StoreWriter row is followed by a row whose concept is “contract and R8 interpretation, U-A9” but
   whose state cell says “Test harness”; the next row puts “mock native effects (§11.3)” under the
   mechanism column with an empty state. As rendered, the root status table does not accurately say
   that StoreWriter and its R8 interpretation are deferred, or that the deterministic test harness
   is specified while its implementation is owed. This conflicts with V9 and §16 (`:211–218`,
   `:2168–2172`), and with §11.3/§18.10, which say the harness is specified but unbuilt
   (`:1604–1621`, `:2441–2447`). **Correction:** restore one complete three-cell row for U-A1
   reclamation, with checkpoint/migration clearly distinguished in the mechanism cell; give the U-A9
   StoreWriter contract/R8 interpretation an explicit deferred/unimplemented state; and make the
   test harness and mock native effects separate, correctly aligned concepts with their stated
   implementation status. Keep the repaired table consistent with §18’s authoritative initial
   boundaries.

## Incorporation audit

The v3.8 header and introduction preserve “specification only” and state that incorporation adds
initial boundaries without approving deferred changes
(`design/vagent-agent-language.md:5–10, 27–30`). The P1–P14 approval register is unchanged and
continues to say the approvals apply to those fourteen proposals (`:1511–1517, 1574–1578`). §18
explicitly makes its boundaries authoritative and labels the companion note historical input
(`:2202–2211`). Its Now/Later status is also reflected in the introduction, §3 V9, §4.4, §§6.4–6.6,
§8.11, §11.1/§11.3, §14–§17 and the 3.8 history. I found no other obsolete open-question framing,
approval leakage, version mismatch, broken section cross-reference, or contradiction between §1 and
§18.

The incorporated Contract-Net example carries forward R4’s bounded scope and deadline correction. It
restricts the example to one node/owner and a shared nondecreasing clock with no migration
(`:1960–1966`), requires `now < result_until <= expires_at` at both creation and participant
acceptance, and refuses an elapsed deadline without adopting a work goal (`:1979–1990`). The gate
exercises elapsed acceptance (`:2030–2042`). The example and §18.13 both preserve the missing-source
and Correlation prerequisites rather than claiming a built protocol (`:2040–2042, 2500–2504`).

## All-fourteen disposition

“Correct” means the recommendation’s initial boundary and deferral are represented in v3.8; it does
not imply approval of any additional extension.

| U-A | Incorporation status                                        | Evidence in `design/vagent-agent-language.md`                                                                                                          |
| --- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | Correct; same-pid replacement deferred                      | §4.4 records unbounded busy-agent memory and the deferred mechanism; §18.1 retains checkpoint/resume initially (`:430–432, 2243–2255`).                |
| 2   | Correct; single-owner initial boundary                      | §1/§4.1 and §18.2 (`:87, 299, 2257–2270`).                                                                                                             |
| 3   | Correct; inline limit retained, handle deferred             | §5.5 and §18.3 specify the limit and future handle/source/checkpoint prerequisites (`:572–577, 2272–2310`).                                            |
| 4   | Correct; hosted relay retained                              | §5.4 and §15 retain relay/failure cost; §18.4 defers hostless spaces (`:529–539, 2120, 2312–2324`).                                                    |
| 5   | Correct; single-minded default retained                     | §6.4 states drop-first precedence and defers modifiers; §18.5 matches (`:772–777, 2326–2341`).                                                         |
| 6   | Correct; colon syntax target, migration separate            | §8.11 and §18.6 scope the name convention and migration (`:1382–1388, 2343–2355`).                                                                     |
| 7   | Correct; restricted suspension retained                     | §6.6 states positions and rewrites; §18.7 defers continuation semantics (`:876–886, 2357–2370`).                                                       |
| 8   | Correct; single-turn guarantee, replay deferred             | §14 separates captured replay from crash recovery; §18.8 scopes the optional trace (`:2108–2111, 2372–2400`).                                          |
| 9   | Correct; StoreWriter unavailable initially                  | V9, §16 and §18.9 defer the contract and keep R8 unchanged (`:211–218, 2168–2172, 2402–2439`). The §1 table misrenders this status as described above. |
| 10  | Correct; deterministic host-side harness specified, unbuilt | §11.3 and §18.10 (`:1604–1621, 2441–2447`). The §1 table misplaces its status as described above.                                                      |
| 11  | Correct; FIFO and P4 arbitration baseline, urgency deferred | §§6.5/11.1 state mailbox limits and required baseline arbitration; §18.11 defers urgency (`:802–846, 1566–1572, 2449–2472`).                           |
| 12  | Correct; all selectors fixed initially                      | §§6.3/6.5 and §17 remain fixed; §18.12 describes only a future plan selector (`:732–737, 800–805, 2184, 2474–2486`).                                   |
| 13  | Correct; library direction in §12.5, syntax deferred        | §12.5 is scoped as a proposed library contract; §18.13 preserves source/Correlation prerequisites and maturity claim (`:1958–2042, 2488–2504`).        |
| 14  | Correct; private-program baseline retained                  | §4.4/§15 preserve the per-agent copy; §18.14 defers sharing pending measurement (`:421–424, 2136, 2506–2523`).                                         |

## Required correction

Repair and re-read the §1 table entries at `design/vagent-agent-language.md:96–103`. No other
incorporation change is required by this review. The gates and implementation details remain
proposals; no tests were run as part of this document review.
