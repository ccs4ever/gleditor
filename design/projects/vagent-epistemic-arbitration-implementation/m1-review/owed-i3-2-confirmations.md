# M1: owed I3-2 round-3 confirmations

Under [decisions.md](../decisions.md) section 0.1, M1's reviewers check the open round-3 MAJOR
findings of the ML, epistemology, hardware and security personas against the shipped code. The
resolutions are in [i3-2-design.md](../i3-2-design.md) section 12; the rulings R3-n are in
decisions.md section 1. MINOR findings of those reports are mapped in section 12 too and are checked
only where a reviewer meets them in the code.

| Finding | Report                               | Ruling | Claim to confirm in code                                                                  | Checked by |
| ------- | ------------------------------------ | ------ | ----------------------------------------------------------------------------------------- | ---------- |
| ml M1   | `i3-2-review/review-ml-r3.md`        | R3-12  | a note applies refusals and credits as an evenly interleaved mixture, tested to `n`=1,000 | ML         |
| ml M2   | `i3-2-review/review-ml-r3.md`        | R3-9   | `R` and `N` rescale together, before the update                                           | ML         |
| epi M3  | `i3-2-review/review-epistemology-r3` | R3-9   | as ml M2; the fuzz invariant carries it                                                   | ML         |
| epi M1  | `i3-2-review/review-epistemology-r3` | R3-8   | A4 reworded; the wedge test exists                                                        | ML         |
| epi M2  | `i3-2-review/review-epistemology-r3` | R3-1   | health reserve charged to live profiles; tombstone actions draw on exempt headroom        | SEC        |
| sec M3  | `i3-2-review/review-security-r3.md`  | R3-1   | as epi M2; `EpistemicBudget` past the headroom; no `2 x max_sources` cap                  | SEC        |
| sec M1  | `i3-2-review/review-security-r3.md`  | R3-5   | a replayed signed event cannot move `R`/`N` past the high-water mark and refusal ring     | SEC        |
| hw M1   | `i3-2-review/review-hardware-r3.md`  | R3-5   | refusals never raise the mark; the estimator cannot freeze                                | DIST       |
| sec M2  | `i3-2-review/review-security-r3.md`  | R3-10  | audit sink: no lock held, non-blocking, serialized install, at most once, nothing secret  | SEC        |
| hw M2   | `i3-2-review/review-hardware-r3.md`  | R3-13  | host latch negative cache is never an authority; one test per transition                  | DIST       |

ML also carries the I3-2c2 named items from `remaining-plan.md` (I3-2c2 gate): the reset epoch and
`retired_by`, which later expire labels.

Each reviewer adds a "Confirmations" section to their report with one line per row they own:
CONFIRMED, or a finding with `file:line`.
