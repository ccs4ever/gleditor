# vagent design review record

The review of [`design/vagent-agent-language.md`](../../vagent-agent-language.md), from version 1.0
to 3.7, by five critics run as subagents: a technical writer (`writer`), an agent-oriented
programming expert (`aop`), a ZigZag purist (`zigzag`), a Vortex and Spool-Mesh expert (`vortex`),
and a language-design and ergonomics expert familiar with SARL (`ergo`).

Files are named `r<round>-<critic>.md`; `r<round>-disposition.md` is the orchestrator's account of
what was accepted, deferred or refused after that round, and is what the next round's critics read.
The briefs every critic received are `brief-common.md` (all rounds), `brief-follow-up-rounds.md`
(rounds 2 to 5) and `brief-focused-rounds.md` (rounds 6 to 9, BLOCKING and MAJOR only). Headings
longer than 100 columns were shortened for the Markdown gates; each keeps its full text on the line
below as "Full title".

| Round | Document version reviewed | Outcome                                                              |
| ----- | ------------------------- | -------------------------------------------------------------------- |
| 1     | 1.0                       | 21 BLOCKING; rewritten as 2.0                                        |
| 2     | 2.0                       | 3 BLOCKING; 3.0                                                      |
| 3     | 3.0                       | 1 BLOCKING; 3.1                                                      |
| 4     | 3.1                       | 1 BLOCKING; 3.2, core changes become a proposal register             |
| 5     | 3.2                       | 1 BLOCKING; 3.3                                                      |
| 6     | 3.3                       | MAJOR only; writer converged; 3.4, P1–P11 approved, P12–P14 proposed |
| 7     | 3.4                       | ZigZag, AOP, ergonomics converged; 3.5                               |
| 8     | 3.5                       | writer and Vortex converged; all five converged                      |
| 9     | 3.6 (P12–P14 approved)    | all five converged; minors applied as 3.7                            |
