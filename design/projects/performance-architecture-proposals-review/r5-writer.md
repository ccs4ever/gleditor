# Round 5 critique: Technical Writer (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## BLOCKING

(None.)

## MAJOR

(None.)

## MINOR

- (§2.2, line 202) Dense `byRef` pricing omits the inclusive upper-bound slot. **Full title:**
  `byRef` capacity should account for `opCount + 1`. **Problem:** The proposed direct mapping uses
  `opIndex` values in `[1, opCount]`, so a vector indexed directly by `opIndex` needs `opCount + 1`
  entries; the price currently counts only `opCount`. **Evidence:** §2.2 lines 182–187 establishes
  the range, while lines 202–204 prices `4 bytes * opCount` and `ceil(opCount / 8)` bitmap bytes.
  **Fix:** Price the extra index-zero slot in both the values and occupancy storage, or explicitly
  define a zero-based offset and adjust lookup wording accordingly.

- (§2.2, line 186) Fallback threshold differs from its stated default. **Full title:** Use one
  explicit sparse-index threshold. **Problem:** The proposal sends indices above `2 * opCount` to
  overflow, while U-P2's default uses `2 * max(opCount, 4096)`. For small stores, those rules choose
  different representations, so the implementer cannot tell which threshold is intended.
  **Evidence:** §2.2 lines 186–187 and §5 line 351. **Fix:** State the same threshold and edge
  condition in both places, or clarify that the open question intentionally proposes a different
  threshold for synthetic tests.

VERDICT: CONVERGED
