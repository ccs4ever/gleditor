# Round 2 Disposition

Document:
[`vagent-dynamic-source-weighting-and-epistemic-arbitration.md`](../vagent-dynamic-source-weighting-and-epistemic-arbitration.md),
revised from v1.1 to v1.2.

## Status

Round 2 is **incomplete**. Four of the ten critics (`writer`, `vortex`, `ergo`, `mathematician`)
failed with `RESOURCE_EXHAUSTED` (HTTP 429) before writing a report. A resume request to each hit
the same error. Their Round 1 findings were all dispositioned in
[`r1-disposition.md`](r1-disposition.md), but no Round 2 verdict exists for them. Do not read this
round as converged.

| critic           | verdict                                      |
| ---------------- | -------------------------------------------- |
| `zigzag`         | CONVERGED (0 BLOCKING, 0 MAJOR, 4 minor)     |
| `codebase`       | CONVERGED (0 BLOCKING, 0 MAJOR, 3 minor)     |
| `epistemologist` | CONVERGED (0 BLOCKING, 0 MAJOR, 3 minor)     |
| `ml-expert`      | CONVERGED (0 BLOCKING, 0 MAJOR, 3 minor)     |
| `architect`      | CONVERGED                                    |
| `hardware-fault` | NOT CONVERGED (0 BLOCKING, 4 MAJOR, 3 minor) |
| `writer`         | no report (quota)                            |
| `vortex`         | no report (quota)                            |
| `ergo`           | no report (quota)                            |
| `mathematician`  | no report (quota)                            |

## Hardware Fault findings

All four MAJOR findings and all three minors were accepted and applied in v1.2.

| finding                                         | disposition | where                                                           |
| ----------------------------------------------- | ----------- | --------------------------------------------------------------- |
| H2.1 health state machine unwired               | ACCEPTED    | §2.3 transition table; §5.1 flowchart; §5.3 steps 1 and 6       |
| H2.2 dead-reckoning drift disconnected          | ACCEPTED    | §2.3 `$sensor_kind`, `$r0`, `$q_drift`; §3.1; §5.3 step 3; §7.1 |
| H2.3 slew-rate gating missing                   | ACCEPTED    | §4.2 `max_slew`; §5.1 flowchart; §5.3 step 2                    |
| H2.4 no variance ingestion, posterior discarded | ACCEPTED    | §4.1 grammar; §4.2 `with variance:`; §5.3 steps 3 and 8         |
| M5 prior state for innovation gate undefined    | ACCEPTED    | §5.3 step 5: propagated prior, else median/MAD                  |
| M6 `cluster_tolerance` absent from policies     | ACCEPTED    | §4.2; required, `MissingClusterTolerance`                       |
| M7 no recovery directives                       | ACCEPTED    | §4.3 `reset source`, `isolate source`                           |

Rulings worth recording:

- `FAULTED` is latched. A stuck-at fault can pass a $\chi^2$ gate by chance, so release is an
  explicit `reset source`, which also restores the uninformative prior on `$alpha` and `$beta`.
- There is no default variance. A continuous clause with no `with variance:` and no profile `$r0` is
  refused at query time (`MissingVariance`), because an implicit $R_i = 1$ would reintroduce the
  H1.1 error. It is a runtime refusal because profiles are dynamic.
- `$status` and `$reject_count` are written only through the settle queue (§6.3), so a failed plan
  cannot undo a fault detection.
- Odometry is asserted as an integrated pose with a `DEAD_RECKONING` profile, never as absolute
  coordinates.

## Open items

- Rerun the four missing critics once quota allows, against v1.2.
- Rerun `hardware-fault` against v1.2 to confirm its four MAJORs are closed.
