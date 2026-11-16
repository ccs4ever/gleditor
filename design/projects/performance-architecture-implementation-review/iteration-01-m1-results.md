# M1 measured release results

All four Clang modes are validated on source content
`0b39e41fd7453534fe899f42a8c5036bae8147d887b56f1e4dda2c9e24b735b5`. The combined release passed the
complete test suite and headless backend comparisons. Each mode passed 16 held-out workload blocks
with seven measured samples, plus 128 separately validated user-space perf counter samples. All 512
counter samples had both events counted at full runtime.

The [raw sample evidence](iteration-01-m1-evidence.jsonl) records timing and hardware samples
separately, source/build identities, text sizes and observed stage costs. These are serial blocks,
not simultaneously paired measurements. Process counters include startup/setup; probe timings cover
their stated internal scenario scope. Rendering stops when settled with no fixed frame count. No
universal speedup or controlled cold-build speed claim is made.

The independent [expert review](iteration-01-m1-four-mode-cpp-review.md) found M1 ready for M2 with
these limitations. Its [follow-up](iteration-01-m1-four-mode-cpp-review-r2.md) verifies the archive
cleanup. PGO-only render and CLI regressions remain in the table; inconclusive intervals are not
passes. M2 needs fresh source-specific training. M2-M6 remain implementation work.

## Four-mode comparison

Status: **complete**. All four archives are present; findings remain per-workload and
uncertainty-qualified.

Ratios are treatment/control median wall time; \<1 is lower observed time. Intervals resample the
two serial blocks independently (10,000 bootstrap draws; 95% percentile). These are not paired runs.

| Mode        | Workload                        |              Control median s (range) |            Treatment median s (range) |  Ratio |     95% interval |
| ----------- | ------------------------------- | ------------------------------------: | ------------------------------------: | -----: | ---------------: |
| thinlto     | editor_layout_validation        |       0.0813466 (0.0624252–0.0818927) |       0.0681181 (0.0613708–0.0718525) | 0.8374 | [0.7678, 1.0198] |
| thinlto     | editor_render_validation        |          0.708655 (0.695462–0.715298) |          0.713735 (0.709757–0.736336) | 1.0072 | [0.9987, 1.0294] |
| thinlto     | probe_arena_links_validation    |    7.5745e-05 (5.3923e-05–8.4732e-05) |    4.5196e-05 (4.3193e-05–8.6986e-05) | 0.5967 | [0.5510, 1.0266] |
| thinlto     | probe_fold_branch_validation    |    0.00327004 (0.00294387–0.00382949) |    0.00239269 (0.00226728–0.00311524) | 0.7317 | [0.6485, 0.8289] |
| thinlto     | probe_fold_structure_validation |    0.00323595 (0.00269686–0.00464101) |    0.00239938 (0.00223989–0.00306751) | 0.7415 | [0.6535, 0.8735] |
| thinlto     | probe_fold_text_validation      |     0.0024687 (0.00217929–0.00870146) |     0.0027523 (0.00212024–0.00325519) | 1.1149 | [0.7959, 1.3580] |
| thinlto     | probe_operands_validation       | 0.000496778 (0.000447293–0.000599494) | 0.000331383 (0.000308399–0.000421605) | 0.6671 | [0.5775, 0.7409] |
| thinlto     | probe_vm_snapshots_validation   |  0.000363514 (0.00035655–0.000658457) |   0.000526725 (0.000348024–0.0009328) | 1.4490 | [0.8448, 1.4832] |
| thinlto     | store_projection_validation     |        0.0699021 (0.062845–0.0816301) |       0.0749907 (0.0700536–0.0802573) | 1.0728 | [0.9600, 1.1481] |
| thinlto     | vagent_validation               |       0.0440289 (0.0431613–0.0478965) |       0.0486613 (0.0423302–0.0579929) | 1.1052 | [1.0203, 1.2981] |
| thinlto     | vicon_validation                |        0.0511381 (0.043005–0.0571733) |       0.0536925 (0.0469351–0.0554827) | 1.0500 | [0.8964, 1.0757] |
| thinlto     | vlace_validation                |       0.0546385 (0.0492567–0.0655505) |       0.0568882 (0.0503759–0.0598386) | 1.0412 | [0.9890, 1.0881] |
| thinlto     | vpl_file_validation             |       0.0397023 (0.0382617–0.0477331) |       0.0416495 (0.0362295–0.0504307) | 1.0490 | [0.9457, 1.2312] |
| thinlto     | vprolog_file_validation         |        0.0366342 (0.0342411–0.040732) |       0.0390323 (0.0314819–0.0441327) | 1.0655 | [0.8607, 1.1398] |
| thinlto     | vprolog_validation              |        0.0514482 (0.0459589–0.055065) |       0.0538525 (0.0522538–0.0569838) | 1.0467 | [0.9934, 1.1598] |
| thinlto     | vql_destructure_validation      |        0.0855531 (0.0817817–0.090096) |       0.0858638 (0.0780465–0.0924578) | 1.0036 | [0.8902, 1.1006] |
| thinlto-pgo | editor_layout_validation        |       0.0813466 (0.0624252–0.0818927) |       0.0728705 (0.0702244–0.0739334) | 0.8958 | [0.8877, 1.0860] |
| thinlto-pgo | editor_render_validation        |          0.708655 (0.695462–0.715298) |          0.711649 (0.707906–0.729338) | 1.0042 | [0.9968, 1.0249] |
| thinlto-pgo | probe_arena_links_validation    |    7.5745e-05 (5.3923e-05–8.4732e-05) |     4.4575e-05 (3.681e-05–6.8962e-05) | 0.5885 | [0.4713, 0.9583] |
| thinlto-pgo | probe_fold_branch_validation    |    0.00327004 (0.00294387–0.00382949) |     0.00272223 (0.00217496–0.0035866) | 0.8325 | [0.6996, 0.9381] |
| thinlto-pgo | probe_fold_structure_validation |    0.00323595 (0.00269686–0.00464101) |     0.0022642 (0.00200556–0.00293385) | 0.6997 | [0.5938, 0.7846] |
| thinlto-pgo | probe_fold_text_validation      |     0.0024687 (0.00217929–0.00870146) |    0.00222259 (0.00201631–0.00324962) | 0.9003 | [0.6610, 1.4515] |
| thinlto-pgo | probe_operands_validation       | 0.000496778 (0.000447293–0.000599494) | 0.000271808 (0.000250849–0.000359807) | 0.5471 | [0.4560, 0.6733] |
| thinlto-pgo | probe_vm_snapshots_validation   |  0.000363514 (0.00035655–0.000658457) |  0.000309531 (0.000289573–0.00042978) | 0.8515 | [0.5153, 1.1730] |
| thinlto-pgo | store_projection_validation     |        0.0699021 (0.062845–0.0816301) |       0.0714573 (0.0698306–0.0768602) | 1.0222 | [0.9344, 1.1042] |
| thinlto-pgo | vagent_validation               |       0.0440289 (0.0431613–0.0478965) |       0.0449715 (0.0421051–0.0488317) | 1.0214 | [0.9712, 1.0934] |
| thinlto-pgo | vicon_validation                |        0.0511381 (0.043005–0.0571733) |       0.0492699 (0.0461443–0.0557766) | 0.9635 | [0.8546, 1.0907] |
| thinlto-pgo | vlace_validation                |       0.0546385 (0.0492567–0.0655505) |       0.0516531 (0.0466121–0.0559272) | 0.9454 | [0.8914, 1.0182] |
| thinlto-pgo | vpl_file_validation             |       0.0397023 (0.0382617–0.0477331) |        0.0381426 (0.033958–0.0410175) | 0.9607 | [0.9092, 1.0235] |
| thinlto-pgo | vprolog_file_validation         |        0.0366342 (0.0342411–0.040732) |        0.0353607 (0.030304–0.0389598) | 0.9652 | [0.8386, 1.0327] |
| thinlto-pgo | vprolog_validation              |        0.0514482 (0.0459589–0.055065) |        0.0497179 (0.044149–0.0573534) | 0.9664 | [0.8904, 1.0818] |
| thinlto-pgo | vql_destructure_validation      |        0.0855531 (0.0817817–0.090096) |       0.0803536 (0.0751949–0.0830192) | 0.9392 | [0.8714, 0.9988] |
| pgo         | editor_layout_validation        |       0.0813466 (0.0624252–0.0818927) |       0.0738287 (0.0682882–0.0828983) | 0.9076 | [0.8900, 1.1327] |
| pgo         | editor_render_validation        |          0.708655 (0.695462–0.715298) |            0.720477 (0.71731–0.74115) | 1.0167 | [1.0120, 1.0483] |
| pgo         | probe_arena_links_validation    |    7.5745e-05 (5.3923e-05–8.4732e-05) |    4.9013e-05 (3.8834e-05–6.5906e-05) | 0.6471 | [0.5663, 1.0088] |
| pgo         | probe_fold_branch_validation    |    0.00327004 (0.00294387–0.00382949) |     0.0033193 (0.00257709–0.00507549) | 1.0151 | [0.8873, 1.3122] |
| pgo         | probe_fold_structure_validation |    0.00323595 (0.00269686–0.00464101) |    0.00319624 (0.00289284–0.00361643) | 0.9877 | [0.8422, 1.1350] |
| pgo         | probe_fold_text_validation      |     0.0024687 (0.00217929–0.00870146) |    0.00321108 (0.00228791–0.00374535) | 1.3007 | [0.9268, 1.5555] |
| pgo         | probe_operands_validation       | 0.000496778 (0.000447293–0.000599494) | 0.000322817 (0.000307828–0.000550692) | 0.6498 | [0.5664, 1.0027] |
| pgo         | probe_vm_snapshots_validation   |  0.000363514 (0.00035655–0.000658457) | 0.000532958 (0.000391808–0.000557024) | 1.4661 | [0.8521, 1.4909] |
| pgo         | store_projection_validation     |        0.0699021 (0.062845–0.0816301) |       0.0733291 (0.0705507–0.0801433) | 1.0490 | [0.9353, 1.1342] |
| pgo         | vagent_validation               |       0.0440289 (0.0431613–0.0478965) |       0.0431002 (0.0384015–0.0480371) | 0.9789 | [0.8999, 1.0765] |
| pgo         | vicon_validation                |        0.0511381 (0.043005–0.0571733) |       0.0491215 (0.0463656–0.0531421) | 0.9606 | [0.8525, 1.0392] |
| pgo         | vlace_validation                |       0.0546385 (0.0492567–0.0655505) |        0.0515168 (0.045886–0.0632533) | 0.9429 | [0.8569, 1.0161] |
| pgo         | vpl_file_validation             |       0.0397023 (0.0382617–0.0477331) |       0.0415578 (0.0386863–0.0458601) | 1.0467 | [1.0100, 1.1096] |
| pgo         | vprolog_file_validation         |        0.0366342 (0.0342411–0.040732) |       0.0405077 (0.0316746–0.0416581) | 1.1057 | [1.0126, 1.1453] |
| pgo         | vprolog_validation              |        0.0514482 (0.0459589–0.055065) |         0.051334 (0.0458889–0.052387) | 0.9978 | [0.9442, 1.0934] |
| pgo         | vql_destructure_validation      |        0.0855531 (0.0817817–0.090096) |          0.0864684 (0.079245–0.10034) | 1.0107 | [0.9439, 1.1470] |

Counters are whole-process user-space counts including setup and subprocesses; probe timings cover
the internal scenario. They must not be read as kernel instruction counts. `editor_render` uses
`--profile` until settled rather than a fixed frame count, so instruction-count changes may reflect
different rendered work. Binary `.text` totals are reported per artifact; aliases are not summed. No
clean build-cost claim is made because stage timings and warm ccache do not isolate cold build cost.
