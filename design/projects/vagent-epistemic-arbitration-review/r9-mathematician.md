# Round 9 critique: Mathematician & Markdown Math Specialist (v1.5)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R6.1 (Dependence parameter domain): RESOLVED — `rho` remains finite and constrained to `[0,1]`
  before scoring (§3.1, lines 305–313).
- R6.2 (Oversized single-linkage component): RESOLVED — the R7/R8 singleton rule remains present in
  §5.3; no R9 change alters it.
- R8 math review: RESOLVED / no open findings — the R9 changes preserve the reviewed score, replay,
  and scalar-fusion contracts.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Focused mathematical checks

- Source coalescing does not increase vote multiplicity. One common clause may have multiple support
  cells, but each support owns its own observation history; only one current observation per
  authenticated source/incarnation and predicate key contributes. A new same-tuple event retires
  that source's prior observation, exact retry is idempotent, and another source creates its own
  observation (§2.1, lines 94–125). Thus the distinct-source premise of the tempered pool remains
  intact (§3.1, lines 278–294).
- Moving removed support records into the keyed calibration record retains historical targets
  without turning history into extra votes. Corrections to a retired target can update its effective
  training label but cannot restore it as the current reading or displace a later sequence (§2.1,
  lines 119–125). This is consistent with the unchanged Beta update and deterministic correction
  replay: rebuild Beta from active labels; replay SGD in first-accept order with the correction
  substituted at its predecessor's position (§6.3). A label remains attached to its full
  observation/calibration key, so retirement does not imply migration into a replacement epoch.
- The translator's compatibility key includes predicate signature and key positions,
  domain/unit/frame, feature schema, policy/model/calibration epochs, and source incarnation; full
  typed values decide equality and hashes only accelerate it (§2.2, lines 204–208). Translation
  copies current/retired status and active labels, reconstructs owned edges through a ref map,
  resolves tokens last, and expires incompatible labels/completions while retaining replay floors
  (§2.2, lines 210–232). These lifecycle changes preserve the score's per-observation learner
  selection and correction-replay assumptions.
- The new clock fields add a finite nonnegative uncertainty bound and require an immutable mapping
  snapshot/version. Queries may not substitute a newer mapping; reboot/restart must re-establish
  comparability or expire observations (§3.2, lines 337–344). The translator preserves mapped time,
  uncertainty and mapping identity; heartbeat deadlines are rebased with elapsed downtime and
  uncertainty or state becomes stale (§2.2, lines 234–242). The unchanged warrant-decay rule still
  uses acquisition time, while fusion separately requires a shared target state time and upstream
  propagation of full variance (§5.2, lines 588–606). No new operation treats clock uncertainty as
  measurement variance or assumes the scalar covariance formula handles time uncertainty.
- The scalar contract remains `R = r I_k` with squared-unit variance; consensus still refuses
  unknown dependence and requires aligned, conditionally independent observations (§2.1, lines
  146–149; §5.3). No ownership or translation clause introduces cross-covariance or combines
  distinct observations from one source as independent measurements.

VERDICT: CONVERGED
