# vagent v3.8 incorporation review: language ergonomics

## Verdict: one MINOR finding

The frozen incorporation input matches `design/vagent-agent-language.md` v3.8 byte-for-byte. The
incorporated decisions otherwise track the final R4 recommendations and the prior R3/R4 review:
single-minded commitment and drop-first precedence are explicit; suspension limits have authoring
rewrites; StoreWriter is unavailable initially; the protocol example is clearly bounded to one
node/owner/shared clock and includes expiry, cancellation, active-effect handling, and terminal
races. Deferred mechanisms are labeled as unimplemented prerequisites rather than first-release
features. The initial language remains usable within the stated transport, payload, and scheduling
boundaries.

### 1. MINOR — distinguish fixed language selection from still-owed P4 source arbitration

§6.5 says “The selection functions are fixed” and then lists mailbox/host event selection and
oldest-ready intention selection (`design/vagent-agent-language.md:800–805`). But §11.1 says the
adapter/P4 design must still specify how mailbox work and local wakes share the pump, including
host-delivery versus ready-intention order and deadline service (`:1566–1572`); §18.11 likewise says
P4 still needs a complete contract (`:2456–2462`). These statements are compatible if §6.5 fixes
ordering within each source while P4 owns arbitration between sources, but that distinction is not
stated. Because timeouts, effects, and host completions depend on those wake turns, also make clear
that due deadlines receive starvation-free service under sustained mailbox traffic; the current “at
most `ready_per_message`” bound alone is an upper bound on wakes, not a service guarantee.

**Correction:** qualify §6.5 as fixing the language-level FIFO and ready-order rules, while naming
P4 arbitration between message, host, timer, and intention work as an owed baseline contract.
Require the P4 policy to give due deadlines and ordinary local work a finite service bound under a
continuously nonempty mailbox, without claiming hard real-time latency.

## All 14 incorporated decisions

| U-A                       | Disposition             | Incorporation review                                                                                                                                                 |
| ------------------------- | ----------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Busy-agent reclamation | **Accept**              | Same-pid replacement is deferred; checkpoint/resume and unbounded busy-agent memory costs remain explicit (`:2213–2255`, `:2133–2135`).                              |
| 2. Cross-owner spaces     | **Accept**              | Single-owner admission is the initial boundary; cross-owner authority is deferred (`:2257–2270`).                                                                    |
| 3. Large payloads         | **Accept**              | Inline limits remain; handles, resolver, and lease lifecycle are deferred with stable-source requirements (`:2272–2310`).                                            |
| 4. Hostless swarm         | **Accept**              | Hosted relay remains the initial design; discovery is not treated as authority (`:2312–2324`).                                                                       |
| 5. Commitment strategies  | **Accept**              | Single-minded commitment is initial behavior; drop wins when drop and success coincide (`:770–777`, `:2326–2342`).                                                   |
| 6. Module paths           | **Accept**              | Colon-separated logical names remain vagent’s syntax; family migration is separate (`:1381–1390`, §18.6).                                                            |
| 7. Suspension anywhere    | **Accept**              | Suspension positions stay restricted; diagnostics give statement extraction and belief-loop rewrites (`:876–885`).                                                   |
| 8. Recorded streams       | **Accept**              | Single-turn determinism is separated from deferred recording, replay, and crash recovery (`:2372–2400`).                                                             |
| 9. Agents and R8          | **Accept**              | StoreWriter is unavailable initially; authority, commit/retry, and R8 interpretation remain deferred (`:2168–2172`, `:2402–2439`).                                   |
| 10. Tests                 | **Accept**              | Deterministic host-side harness and distinct integration coverage are specified (`:1604–1622`).                                                                      |
| 11. Fast lane             | **Accept with finding** | FIFO and no priority modifier are clear; clarify §6.5’s fixed policies versus owed P4 arbitration and bounded wake service (`:841–846`, `:1566–1572`, `:2449–2472`). |
| 12. Selection functions   | **Accept**              | Fixed selectors remain initial behavior; pure plan ranking is a constrained future extension (§18.12; :2474–2486).                                                   |
| 13. Protocol declarations | **Accept**              | Same-owner library example is explicitly illustrative and deferred; dedicated syntax, full source, and correlation binding remain prerequisites (`:1958–2042`).      |
| 14. Shared program        | **Accept**              | Private program remains the baseline; shared immutable base is a measured future optimization (§18.14; :2506–2523).                                                  |

The new §12.5 protocol example aligns with the R4 resolution, including the `result_until` check at
creation and participant acceptance (`:1973–1980`) and a test for elapsed execution deadlines
(`:2030–2042`). Its shared-clock/no-migration constraint avoids implying distributed clock
semantics. No StoreWriter operation leaks into the initial language (`:211–218`, `:2402–2439`).
