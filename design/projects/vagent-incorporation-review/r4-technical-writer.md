# vagent incorporation review R4

## Verdict: CONVERGED

The frozen `r4-input.txt` matches the current v3.8 target byte for byte. I found no actionable
incorporation findings. The prior §1 table and P4 arbitration/determinism findings are closed; the
R3 editorial correction is present. All fourteen R4 decisions are represented as initial boundaries
or clearly identified deferred work. The three new keyword examples are separate from the existing
examples, and their explanations match the language rules and prerequisites stated elsewhere in the
specification.

## Review notes

- **Status and traceability:** The header still identifies the document as specification only and
  lists P1–P14 as the previously approved changes (lines 5–10). The introduction explicitly limits
  what the R4 incorporation means (lines 26–30). The rebuilt §1 table has complete, correctly
  aligned rows: same-pid replacement is deferred; inline limits remain while large handles are
  deferred; StoreWriter is unavailable initially and its contract/R8 interpretation are deferred;
  the host-side harness is specified while implementation is owed; replay and shared programs are
  deferred (lines 95–101). Those entries agree with §18 and do not suggest implementation or
  additional approval.
- **Prior scheduler and determinism findings:** §5.6 distinguishes restored ready/deadline state
  from external delivery schedule (lines 607–611). §6.5 describes order within each source and
  leaves cross-source selection to the owed P4 policy (lines 800–808). §11.1 now requires a
  deterministic, versioned, finite service guarantee for already-admitted local work under sustained
  mailbox load; it explicitly says a wake quota alone is insufficient (lines 1568–1581). §18.11
  keeps that scheduler contract in the baseline while deferring urgency and changed priority (lines
  3046–3071).
- **Examples:** §12.6 states that each fenced unit is independent and explains harness input, staged
  outbox observation, receiver turns, and specification-only status (lines 2053–2059). The 23 units
  proceed from simple expressions and assignment to search/backtracking, loops, method contracts,
  scanning, rollback, beliefs, goals, waits, messaging, effects, composition, scheduling,
  cancellation and termination (lines 2061–2635). The three newly added units are locally scoped and
  supported: the loop example's list/range and generator distinction matches §8.2's bounded `for`
  and `!e` rules (lines 2148–2169; §8.2 lines 1065–1079); `return`, `requires`, and `ensures` match
  the method/contract rules and error behavior (lines 2171–2196; lines 903–905 and §8); the scan
  uses the documented subject/position syntax and explicitly identifies `std:goal` scanning
  functions as specified but unbuilt prerequisites (lines 2198–2219; §1 lines 9 and 63–68). The
  earlier R3 stray list marker is corrected: the `yield` explanation is continuous at lines
  2605–2612. The awaited-event plan is named `wait_event`, avoiding collision with reserved `event`,
  at lines 2380–2382.
- **Contract-Net and final status:** §12.5 retains the R4 same-node/shared-clock/no-migration scope
  and checks `now < result_until <= expires_at` when creating and accepting work, refusing an
  elapsed result deadline without adopting the goal (lines 1967–2051). §18.13 preserves missing full
  source and Correlation binding as adoption prerequisites (lines 3087–3103). Section 18 is
  authoritative, the companion note is historical input, and deferred work neither claims
  implementation nor inherits P1–P14 approval (lines 2801–2808, 3128–3136). Version 3.8, the
  contents anchor, and the section title agree (lines 5, 53, 2799, 3126).

No tests or builds were run; this was a read-only specification review.

## All-fourteen disposition

“Correct” means the initial boundary and deferral are incorporated; it does not imply that a
deferred extension is implemented or approved.

| U-A | Disposition                                                                             | Incorporation evidence                                            |
| --- | --------------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| 1   | Correct; same-pid reclamation deferred                                                  | §1 line 96; §4.4 lines 428–430; §18.1 lines 2810–2852             |
| 2   | Correct; single-owner admission remains                                                 | §1 line 87; §4.1; §18.2 lines 2854–2867                           |
| 3   | Correct; inline limit remains, handle/resolver deferred                                 | §1 line 97; §5.5; §18.3 lines 2869–2907                           |
| 4   | Correct; hosted relay remains                                                           | §1 line 95; §5.4; §18.4 lines 2909–2921                           |
| 5   | Correct; single-minded commitment remains default                                       | §6.4 lines 770–796; §18.5 lines 2923–2938                         |
| 6   | Correct; colon-separated convention scoped to logical names                             | §8.11 lines 1384–1390; §18.6 lines 2940–2952                      |
| 7   | Correct; restricted suspension remains                                                  | §6.6 lines 853 onward; §18.7 lines 2954–2967                      |
| 8   | Correct; single-turn determinism is distinct from deferred captured replay/recovery     | §14 lines 2692 onward; §18.8 lines 2969–2997                      |
| 9   | Correct; StoreWriter unavailable initially and R8 unchanged pending a separate decision | §1 line 98; V9; §16; §18.9 lines 2999–3036                        |
| 10  | Correct; deterministic host-side harness specified, implementation owed                 | §1 line 99; §11.3; §18.10 lines 3038–3044                         |
| 11  | Correct; FIFO and finite-service P4 baseline required; urgency deferred                 | §6.5 lines 800–848; §11.1 lines 1568–1581; §18.11 lines 3046–3071 |
| 12  | Correct; initial selectors fixed, plan ordering only considered later                   | §6.3; §17; §18.12 lines 3073–3085                                 |
| 13  | Correct; bounded Contract-Net library direction, protocol syntax deferred               | §12.5 lines 1967–2051; §18.13 lines 3087–3103                     |
| 14  | Correct; private program copy remains baseline                                          | §1 line 101; §4.4 lines 419–422; §18.14 lines 3105–3122           |

## Findings

None.
