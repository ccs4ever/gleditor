# I4-D1 gate latch: latency against false-latch risk (owner question, Q1 / ML N-1)

Evidence for the owner's decision; no design or source file edited. Branch
`design/vagent-epistemic-arbitration`, 2026-11-05.

## 1. The question and the short answer

Revision 2 of I4-D1 (design 7.2) latches FAULTED from the gate only when a fast pair (half-life 64,
f >= 0.70, N >= 48) and a slow pair (half-life 4,096, f >= 0.70, N >= 2,048) both hold. A source
rejected on every reading latches at event 2,515 from a clean start and at 6,812 from f_s = 0.05: 25
s and 68 s at 100 Hz, 251 s and 681 s at 10 Hz. Spec 5.1's streak of five latched at event 5.

Findings, with numbers below:

1. **Latency has no fusion-safety cost.** Rejected readings enter no fusion whether or not the
   source is latched. The latch adds only the lifetime exclusion of accepted readings.
1. **The new rule is not a slower streak; it covers fewer faults.** With rejection rate 0.5 or 0.3
   the design never latches (the fraction settles at the true rate, below 0.70). The streak latched
   those in about 40 and 400 events (section 4). This is the larger change and Q1 omits it.
1. **No tuning gets both.** A 4x faster slow window (1,024) loses the proof at rho = 0.99 and
   latches healthy sources 3,650 times per device-year at rho = 0.999. 2,515 events is about 12
   correlation times at rho = 0.99; that is the price of tolerating them.
1. **Recommendation: (a) plus (c1).** Keep the slow latch. Read the fast pair the rule already
   computes as a derived, non-latching exclusion of the source's accepted readings (68 events, 0.7 s
   at 100 Hz). Document the soft-fault gap. Price and amendments: sections 5 and 7.

## 2. Method and rules

- Stdlib Python 3.14, `python3 -I`, scripts in the scratchpad `latch/` directory under
  `/tmp/claude-1000/-data-git-gleditor-restore/4cca52a1-c339-40fd-96f4-c26bf1f777cd/scratchpad/`
  (`rules.py`, `sim_healthy.py`, `sim_faulty.py`, `bounds.py`, `bound_table.py`; output in `out/`).
  `bounds.py` re-implements design 7.4's weighted Chernoff bound in log space (numpy is absent) and
  reproduces 4.7e-86 per event (fast, p=0.10, m=1) and 1e-30 per year (slow, p=0.05, m=199).
- **Healthy**: Gaussian AR(1) residual at lag-1 correlation rho, two-sided 95% gate (hit = |x| >
  1.96, marginal 5% at every rho), as ML-1. Seeds 1001 to 1008, 2e7 events each: 1.6e8 per rho. All
  rules see one hit stream and restart their counters at each latch, as `ar.c` does. Per year =
  latches / events x 3.156e9 (100 Hz); divide by 10 for 10 Hz.
- **Faulty**: iid Bernoulli(r) rejections, r = 1, 0.8, 0.5, 0.3; clean start (N = 0) or steady
  (preloaded at f = 0.05); 200 trials per cell (1 for r = 1), seeds are strings of the cell, cap
  40,000 events. "never" = no latch in the cap; where f cannot reach theta that is the rule's
  property. Limit: a real soft fault rejects in runs, slower for fractions, faster for the streak.
  Zero latches in n events bounds the rate only by about 3/n.
- Rules, tuples (half-life, theta, min events): S5 spec 5.1 streak of 5; R1 fast (64, .70, 48)
  alone; **a** the design: fast and slow (4096, .70, 2048); b1 fast and slow (1024, .70, 512); b2
  fast and slow (512, .70, 256); b3 slow (4096, .40, 2048) alone; b4 slow (1024, .40, 512) alone; d
  b1 plus ML-1's run-length guard (mean rejection run x (1-f) \<= 3; iid gives 1); c1, c2, c3
  non-latching exclusion while a fast pair holds: (64, .70, 48), (16, .70, 16), (32, .50, 32).

## 3. False latch on healthy sources (per device-year at 100 Hz, simulated)

1.6e8 events per column. "0" means none seen: below about 59 per device-year at 95% (3/n), so read
it with the proved bound beside it.

| rule | rho=0 (iid) | 0.5   | 0.9    | 0.99   | 0.999  |
| ---- | ----------- | ----- | ------ | ------ | ------ |
| S5   | 789         | 2.0e5 | 1.05e7 | 2.4e7  | 2.9e7  |
| R1   | 0           | 0     | 0      | 1.97e5 | 1.5e6  |
| a    | 0           | 0     | 0      | 0      | 0      |
| b1   | 0           | 0     | 0      | 0      | 3,650  |
| b2   | 0           | 0     | 0      | 0      | 47,700 |
| b3   | 0           | 0     | 0      | 0      | 335    |
| b4   | 0           | 0     | 0      | 0      | 82,200 |
| d    | 0           | 0     | 0      | 0      | 59     |

S5 for iid residuals is 937 per year in theory (0.05^5 x 0.95 x 3.156e9); the 789 simulated agrees
within restart effects. S5 at rho = 0.99 latches once per 130 events: a healthy source is latched
for life in about a second. R1 reproduces ML-1 (first latch 1,595 events at 0.99, 68 at 0.999).

Proved bound, log10 of the per-device-year false-latch probability at 100 Hz, minimum over the
rule's pairs (`out/bounds.txt`; "none" means the bound exceeds 1 and proves nothing):

| pair (H, theta)      | p   | rho=0.9 | 0.99 | 0.995 | 0.999 |
| -------------------- | --- | ------- | ---- | ----- | ----- |
| slow 4096, 0.70 (a)  | .05 | -403    | -30  | -10   | none  |
| slow 4096, 0.70 (a)  | .10 | -278    | -18  | -4    | none  |
| slow 1024, 0.70 (b1) | .05 | -94     | 0    | none  | none  |
| slow 4096, 0.50      | .05 | -197    | -10  | 0     | none  |
| slow 4096, 0.40 (b3) | .05 | -126    | -3   | none  | none  |

Reading: only the design is proved to rho = 0.99 at p = 0.10 and 0.995 at p = 0.05; b1 and b2 only
to 0.9 (b2: -42 at 0.9, none above); R1 proves nothing from 0.9. At 0.999 nothing is provable and
the design's zero rests on simulation (its own 4e9 events agree with the 1.6e8 here).

## 4. Time to latch (or exclude) on faulty sources

Median events (95th percentile after a slash where it differs); clean = fresh profile, steady =
long-clean source at f = 0.05. Seconds: events/100 at 100 Hz, events/10 at 10 Hz.

| rule | r=1 clean | r=1 steady | r=0.8 clean | r=0.8 steady | r=0.5 clean / steady | r=0.3     |
| ---- | --------- | ---------- | ----------- | ------------ | -------------------- | --------- |
| S5   | 5         | 5          | 8 / 24      | 8 / 25       | 37 / 47              | 426 / 388 |
| R1   | 68        | 107        | 68          | 184 / 233    | never                | never     |
| a    | 2,515     | 6,812      | 2,515       | 11,878       | never                | never     |
| b1   | 629       | 1,703      | 629         | 2,960        | never                | never     |
| b2   | 315       | 852        | 315         | 1,475        | never                | never     |
| b3   | 2,515     | 2,716      | 2,515       | 3,714        | 2,515 / 8,907        | never     |
| b4   | 629       | 679        | 629         | 928          | 629 / 2,208          | never     |
| d    | 629       | 1,703      | 629         | 2,971        | never                | never     |

Design a in seconds (100 Hz / 10 Hz): r=1 clean 25 s / 251 s; r=1 steady 68 s / 681 s; r=0.8 steady
119 s / 1,188 s; r=0.5 and 0.3 never.

(b3 at r=0.5 "clean" is the cold-start edge: f is 0.5 > 0.40 the moment N reaches 2,048, so the
steady column, 8,907, is the realistic one.)

Non-latching exclusion (c rules), same conventions:

| rule | r=1 clean | r=1 steady | r=0.8 clean | r=0.8 steady | r=0.5 clean / steady | r=0.3        |
| ---- | --------- | ---------- | ----------- | ------------ | -------------------- | ------------ |
| c1   | 68        | 107        | 68          | 184 / 233    | never (199/200)      | never        |
| c2   | 27        | 27         | 27          | 44 / 71      | 994 / 1,247          | never        |
| c3   | 54        | 30         | 54          | 43 / 55      | 55 / 129             | mostly never |

The cost side of the c rules, on healthy sources (1.6e8 events per rho, `out/healthy.txt`):

| rule | rho=0 | 0.5 | 0.9            | 0.99 entries/hr, share of time | 0.999 entries/hr, share of time |
| ---- | ----- | --- | -------------- | ------------------------------ | ------------------------------- |
| c1   | 0     | 0   | 0              | 20, 0.23%                      | 52, 3.0%                        |
| c2   | 0     | 0   | 2.6/hr, 0.003% | 228, 1.9%                      | 128, 4.0%                       |
| c3   | 0     | 0   | 2.1/hr, 0.004% | 203, 2.5%                      | 104, 4.6%                       |

An exclusion that is wrong is repaired by the next accepted run of readings; a wrong latch is not.

## 5. Options

**(a) Accept as designed.** Hard fault: 25 s at 100 Hz (68 s steady), 251 s at 10 Hz (681 s). False
latch: none seen to rho = 0.999 in 1.6e8 events, proved to rho = 0.99 (p \<= 0.10) and 0.995 (p \<=
0.05). During the wait the source is SUSPECT from event 18, inflated x4, and every rejected reading
is excluded (7.2). The residual exposure is a source that passes the gate part of the time: its
accepted readings are used at 1/4 weight until the latch, or for ever if its rejection rate is below
about 0.7. Price: none beyond the documentation ML N-1 asks for (add 0.1 Hz: 7 h).

**(b) Retune H, theta, N.** Latency scales with the slow window (b1: 629 / 1,703 events; b2: 315 /
852). The cost: b1's bound is 1 at rho = 0.99, and it latches 3,650 healthy sources per device-year
at rho = 0.999 (b2: 47,700). Lowering theta to 0.40 (b3, b4) buys coverage down to r about 0.5, not
latency; b3 latches healthy sources 335 times a year at 0.999 and is proved only to rho = 0.9 at p =
0.05 (-3 at 0.99). Nothing moves r = 0.3: it is the SUSPECT threshold and near the gate's tolerance
(p \<= 0.15), so latching there is a policy, not a tuning.

**(c) Two tier: reversible exclusion plus the slow latch.** The I3-2 states do not already provide
one. `rtHealth` (i3-2 design 3.4) derives NOMINAL, DEGRADED and SUSPECT, all reversible, but their
licences are "displayed" and "inflated"; SUSPECT readings "are still stored and still calibrate".
Only FAULTED excludes (spec 5.1). `epistemic_abi_health.hpp` has no exclude state or kind
(`isolate`, `degrade`, `clear`, `reset` only). A huge SUSPECT inflation approximates one but also
silences the r about 0.3 flap zone (design 7.4.3) and rewrites every suspect's variance. The c rules
add the missing tier:

- **c1** reuses the fast pair the latch already computes, so no stored state is added. Exclusion in
  68 events; zero false entries to rho = 0.9; 20 per hour (0.23% of the time) at 0.99; 3% at 0.999.
  It does not see r = 0.5.
- **c3** sees r = 0.5 in 55 to 129 events but costs more on correlated healthy sources.
- **c2** is fastest (27 events) with the highest false-entry rate at 0.9 and above.

Safety gained over (a): the readings a failing source still passes through the gate in the 68 to
2,515 events before the latch, and (c3) those of a half-failing source, which never latches. Price:
a changed meaning of "excluded" in I4-3/I4-9 eligibility and `nEff`; a new reason code
(`gate_excluded`, displayed as SUSPECT, so no new state); I4-1b's version bump per entry; I4-8 must
measure the consensus effect of 20 entries/hr on a source at rho = 0.99.

**(d) Block or effective-sample-size counting.** Tested as rule d (ML-1's run-length guard). It cuts
b1's healthy latches at 0.999 from 3,650 to 59 per year, not to zero, and adds two Doubles per
profile; hard faults are unaffected (an open run is not in the mean). It does not beat (a): the slow
window is already an effective-sample-size device (design 7.4.2, m = (1+rho)/(1-rho)), and no rule
separates a persistent fault from a long excursion in fewer events than a correlation time.

**(e) Alternatives found.** A wall-clock floor on the latch (a 0.1 Hz sensor takes 7 h in events;
correlation is in time, so the guarantee changes; not simulated, a follow-up). Stating in the spec
that the gate latches only sources rejected nearly every time, and that partial rejection is SUSPECT
while it lasts, which is the honest reading of spec 5.2 ("not which sensor is wrong"). Operator
`isolate` for the middle band is already built.

## 6. Recommendation

1. **Keep the slow-pair latch exactly as designed (a).** Accept 2,515 / 6,812 events. Do not retune
   (b): each step toward lower latency forfeits the proved region or raises healthy latches by
   orders of magnitude, and the latency costs no fusion safety.
1. **Add c1 as a derived, non-latching exclusion.** The same fast-pair condition (64, 0.70, 48) that
   gates the latch also makes the source's accepted readings ineligible while it holds. A source
   that rejects almost everything leaves fusion at event 68, not 2,515, and returns by itself when
   it stops. Price: section 5 (c); about 20 entries per hour at rho = 0.99 on a healthy source, zero
   at rho \<= 0.9. Consider c3 only if half-rejecting sources must leave fusion.
1. **Tell the owner the soft-fault change in plain words.** The streak latched sources rejecting 30%
   to 70% of readings in 40 to 400 events (and healthy sources at rho >= 0.5 within seconds). The
   design latches none of them; they stay SUSPECT. A latch there is b3 (slow 4,096 at 0.40): r about
   0.5 after 8,907 events, 335 healthy latches a year at rho = 0.999, proof only to rho = 0.9 (p =
   0.05); not recommended.

## 7. What needs the owner (proposed amendments to spec 5.1)

- A12 as drafted (fast and slow fractions replace the streak; nominal-variance gate): already
  proposed. Ratify it knowing that partial-rate faults (r below about 0.7) no longer latch.
- New amendment (c1): while the fast gate fraction holds, the source's accepted readings are
  excluded from fusion, non-latching, reason `gate_excluded`, displayed SUSPECT. Spec 5.1 today
  excludes only "active fault status".
- Q1 restated: the 1e-6 per device-year figure plus the latency (2,515 / 6,812 events) and the loss
  of soft-fault latching.
- A retune (b rules) or a soft-fault latch (b3) would change A12; not recommended.
- Needs no ratification: the user-guide line "FAULTED from the gate is an event-count latch; a
  sensor below 1 Hz should be isolated by the health-timer or operator path", the 0.1 Hz figure, the
  wall-clock follow-up. If the owner declines c1, A12 stands and the cost is the (a) exposure.
