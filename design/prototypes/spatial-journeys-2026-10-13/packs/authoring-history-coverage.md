# State pack coverage · J27–J32

This pack turns the previous concept-review gaps into scripted, named transitions. Every action
replaces the visible result panels and records its effect domain. Fixtures are local simulated data;
the pack does not claim production behavior.

## Prior finding mapping

| Prior finding | Concrete state/action coverage                                                                                                                                                                                                                                                     |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R3-01         | J27-s5-a2..a3; J28-s4-a1..a5; J28-s5-a1..a3; variants J28-unknown-confirmed-uncommitted, J28-status-created-existing, J28-status-unavailable, J28-discard-confirmed-uncommitted; J31-s5-a1..a3; variants J31-recovery-confirmed-uncommitted, J31-recovery-status-unavailable.      |
| R3-02         | J27-s1-a1..a4; J27-s2-a1..a5; J27-s3-a1..a3; J27-s4-a1..a2; J27-s5-a1..a3; variant J27-discard-draft.                                                                                                                                                                              |
| R3-03         | J28-s1-a1..a2; J28-s3-a1..a5; J28-s6-a1..a2; variant J28-discard-confirmed-uncommitted.                                                                                                                                                                                            |
| R3-04         | J29-s1-a1..a2; J29-s2-a1..a4; J29-s3-a1..a3; J29-s4-a1..a3; J29-s5-a1..a3; variants J29-unsupported-word-seek, J29-unavailable-segment.                                                                                                                                            |
| R3-05         | J30-s1-a1..a2; variant J30-ten-paths; J30-s2-a1..a3; J30-s3-a1..a6; J30-s4-a1..a2; J30-s7-a1..a5.                                                                                                                                                                                  |
| R3-06         | J30-s5-a1..a3; J30-s6-a1..a3.                                                                                                                                                                                                                                                      |
| R3-07         | J31-s1-a1..a2; J31-s2-a1..a2; J31-s3-a1..a2; J31-s4-a1..a5; J31-s5-a1..a3; J31-s6-a1..a2; variants J31-unavailable-source, J31-choose-new-base, J31-invalid-author-destination, J31-recovery-unknown-created, J31-recovery-confirmed-uncommitted, J31-recovery-status-unavailable. |
| R3-08         | J32-s1-a1..a2; J32-s2-a1..a3; J32-s3-a1..a4; J32-s4-a1..a3; J32-s5-a1..a3; J32-s6-a1..a6; variants J32-repair-session and J32-cancel-request.                                                                                                                                      |
| R3-09         | J31-s6-a2 explicitly labels the badge as 3 difference categories for A V27 versus B V40, separate from recovery into B V31.                                                                                                                                                        |

## Journey step coverage

| Journey | Step | New action IDs                                                                                                                                             | Variants                           |
| ------- | ---: | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| J27     |    1 | J27-s1-a1, J27-s1-a2, J27-s1-a3, J27-s1-a4                                                                                                                 | —                                  |
| J27     |    2 | J27-s2-a1, J27-s2-a2, J27-s2-a3, J27-s2-a4, J27-s2-a5                                                                                                      | —                                  |
| J27     |    3 | J27-s3-a1, J27-s3-a2, J27-s3-a3                                                                                                                            | —                                  |
| J27     |    4 | J27-s4-a1, J27-s4-a2                                                                                                                                       | —                                  |
| J27     |    5 | J27-s5-a1, J27-s5-a2, J27-s5-a3                                                                                                                            | —                                  |
| J27     |    5 | J27-discard-draft-a1                                                                                                                                       | J27-discard-draft                  |
| J28     |    1 | J28-s1-a1, J28-s1-a2                                                                                                                                       | —                                  |
| J28     |    2 | J28-s2-a1, J28-s2-a2, J28-s2-a3                                                                                                                            | —                                  |
| J28     |    3 | J28-s3-a1, J28-s3-a2, J28-s3-a3, J28-s3-a4, J28-s3-a5                                                                                                      | —                                  |
| J28     |    4 | J28-s4-a1, J28-s4-a2, J28-s4-a3, J28-s4-a4, J28-s4-a5                                                                                                      | —                                  |
| J28     |    5 | J28-s5-a1, J28-s5-a2, J28-s5-a3                                                                                                                            | —                                  |
| J28     |    6 | J28-s6-a1, J28-s6-a2                                                                                                                                       | —                                  |
| J28     |    2 | J28-destination-failure-a1, J28-destination-failure-a2                                                                                                     | J28-destination-failure            |
| J28     |    5 | J28-unknown-confirmed-uncommitted-a1, J28-unknown-confirmed-uncommitted-a2, J28-unknown-confirmed-uncommitted-a3                                           | J28-unknown-confirmed-uncommitted  |
| J28     |    5 | J28-status-created-existing-a1, J28-status-created-existing-a2                                                                                             | J28-status-created-existing        |
| J28     |    5 | J28-status-unavailable-a1, J28-status-unavailable-a2                                                                                                       | J28-status-unavailable             |
| J28     |    6 | J28-discard-confirmed-uncommitted-a1, J28-discard-confirmed-uncommitted-a2                                                                                 | J28-discard-confirmed-uncommitted  |
| J29     |    1 | J29-s1-a1, J29-s1-a2                                                                                                                                       | —                                  |
| J29     |    2 | J29-s2-a1, J29-s2-a2, J29-s2-a3, J29-s2-a4                                                                                                                 | —                                  |
| J29     |    3 | J29-s3-a1, J29-s3-a2, J29-s3-a3                                                                                                                            | —                                  |
| J29     |    4 | J29-s4-a1, J29-s4-a2, J29-s4-a3                                                                                                                            | —                                  |
| J29     |    5 | J29-s5-a1, J29-s5-a2, J29-s5-a3                                                                                                                            | —                                  |
| J29     |    4 | J29-unsupported-word-seek-a1, J29-unsupported-word-seek-a2                                                                                                 | J29-unsupported-word-seek          |
| J29     |    4 | J29-unavailable-segment-a1, J29-unavailable-segment-a2                                                                                                     | J29-unavailable-segment            |
| J30     |    1 | J30-s1-a1, J30-s1-a2                                                                                                                                       | —                                  |
| J30     |    2 | J30-s2-a1, J30-s2-a2, J30-s2-a3                                                                                                                            | —                                  |
| J30     |    3 | J30-s3-a1, J30-s3-a2, J30-s3-a3, J30-s3-a4, J30-s3-a5, J30-s3-a6                                                                                           | —                                  |
| J30     |    4 | J30-s4-a1, J30-s4-a2                                                                                                                                       | —                                  |
| J30     |    5 | J30-s5-a1, J30-s5-a2, J30-s5-a3                                                                                                                            | —                                  |
| J30     |    6 | J30-s6-a1, J30-s6-a2, J30-s6-a3                                                                                                                            | —                                  |
| J30     |    7 | J30-s7-a1, J30-s7-a2, J30-s7-a3, J30-s7-a4, J30-s7-a5                                                                                                      | —                                  |
| J30     |    1 | J30-ten-paths-a1, J30-ten-paths-a2                                                                                                                         | J30-ten-paths                      |
| J31     |    1 | J31-s1-a1, J31-s1-a2                                                                                                                                       | —                                  |
| J31     |    2 | J31-s2-a1, J31-s2-a2                                                                                                                                       | —                                  |
| J31     |    3 | J31-s3-a1, J31-s3-a2                                                                                                                                       | —                                  |
| J31     |    4 | J31-s4-a1, J31-s4-a2, J31-s4-a3, J31-s4-a4, J31-s4-a5                                                                                                      | —                                  |
| J31     |    5 | J31-s5-a1, J31-s5-a2, J31-s5-a3                                                                                                                            | —                                  |
| J31     |    6 | J31-s6-a1, J31-s6-a2                                                                                                                                       | —                                  |
| J31     |    4 | J31-unavailable-source-a1, J31-unavailable-source-a2                                                                                                       | J31-unavailable-source             |
| J31     |    4 | J31-choose-new-base-a1, J31-choose-new-base-a2, J31-choose-new-base-a3                                                                                     | J31-choose-new-base                |
| J31     |    5 | J31-recovery-unknown-created-a1, J31-recovery-unknown-created-a2                                                                                           | J31-recovery-unknown-created       |
| J31     |    5 | J31-invalid-author-destination-a1, J31-invalid-author-destination-a2, J31-invalid-author-destination-a3, J31-invalid-author-destination-a4                 | J31-invalid-author-destination     |
| J31     |    5 | J31-recovery-confirmed-uncommitted-a1, J31-recovery-confirmed-uncommitted-a2, J31-recovery-confirmed-uncommitted-a3, J31-recovery-confirmed-uncommitted-a4 | J31-recovery-confirmed-uncommitted |
| J31     |    5 | J31-recovery-status-unavailable-a1, J31-recovery-status-unavailable-a2                                                                                     | J31-recovery-status-unavailable    |
| J32     |    1 | J32-s1-a1, J32-s1-a2                                                                                                                                       | —                                  |
| J32     |    2 | J32-s2-a1, J32-s2-a2, J32-s2-a3                                                                                                                            | —                                  |
| J32     |    3 | J32-s3-a1, J32-s3-a2, J32-s3-a3, J32-s3-a4                                                                                                                 | —                                  |
| J32     |    4 | J32-s4-a1, J32-s4-a2, J32-s4-a3                                                                                                                            | —                                  |
| J32     |    5 | J32-s5-a1, J32-s5-a2, J32-s5-a3                                                                                                                            | —                                  |
| J32     |    6 | J32-s6-a1, J32-s6-a2, J32-s6-a3, J32-s6-a4, J32-s6-a5, J32-s6-a6                                                                                           | —                                  |
| J32     |    5 | J32-repair-session-a1, J32-repair-session-a2, J32-repair-session-a3                                                                                        | J32-repair-session                 |
| J32     |    5 | J32-cancel-request-a1, J32-cancel-request-a2                                                                                                               | J32-cancel-request                 |

## Identity and effect invariants represented

- Source operations are fixture baselines; reader navigation, inspection, filters, views, cue
  alignment, and restore do not increment them.
- Explicit Enter creates one reader visit. Restore/Resume and inspection do not.
- Reader collections/activity, private staging, unpublished destination staging, and published
  destination receipts have separate effect domains.
- All authored commit paths bind a stable attempt token to ordered payload and pinned
  destination/base. Unknown outcomes block edit, retry and discard until status resolves.
- J30 preserves 160 unique ops as 100 trunk + 3 × 20 branch ops, with forks D001/D050/D100; the
  ten-path fixture remains separate.
- J31 pins source A V27 and receiving base B V31 independently from retained B V40 and advanced head
  B V41.
- J32 restores saved identities before cache/media resolution and does not auto-refresh or enable
  candidate packages.
