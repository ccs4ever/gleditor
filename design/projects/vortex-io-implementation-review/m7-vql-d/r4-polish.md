# Round 4 polish (M7-VQL-d gate)

All three critics CONVERGED in round 4 at `2e9e4178`. The MINORs, each fixed:

| finding    | resolution                                                                                                                                                                                                             |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| zigzag m1  | the `VqlIoKnownLeakTest` ceilings are now named constants at about 1.25 times the measured figures (333, 144, 58, 334 and 207 against 266, 115, 46, 267 and 165); a comment says d3 turns them into equality with zero |
| vortex m1  | the `keepIfNamed` count comparison (r3 vortex m2) is named in roadmap 6.1c item 1, so d3 replaces it with a cell comparison                                                                                            |
| vortex m2  | design note 8c now says the leak mechanism is pre-existing but the `join` figures are new (d3's baseline, not pre-slice numbers)                                                                                       |
| zigzag (b) | the join-and-write "one text cell per run" figure is in the 8c table                                                                                                                                                   |

Also recorded: the gate as passed in the implementation plan's M7-VQL-d notes, the design note's
status line and change history, and the roadmap's 6.1 status and change history.
