# Round 3 critique: Xanadu / ZigZag Purist (vagent-native-agent.md v0.3)

Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (decision-to-approval fan-in): RESOLVED. Decisions quote spans and do not link; revocation is
  one-to-one; every relation in §10.5 is a chain or a quotation.
- R2.2a (approval withdrawal): RESOLVED. The `d.approval` event rank with the tail as current state,
  and `vagent native withdraw`.
- R2.2b (ledger permascroll, load failure): RESOLVED. Private `authorities/permascroll/`, never
  published, typed refusal `LedgerPermascrollMissing`, fail-closed `approved`.

## MINOR

- §10.5 says decision purpose values quote "the approval's and the declaration's spans", but the
  declaration's text lives in the program's op log (the author's permascroll) while the ledger's
  text is meant to stay out of it. Say the purpose is spooled once into the ledger permascroll and
  decisions quote that copy, not the declaration.
- Say that each decision's attribute pairs are a chain along `d.vars` with `d.values` to each value,
  so the rank shape (a path, not a fan) is explicit. The same for an approval event.
- "At most one revocation per decision": say what happens on a second revocation event (ignored,
  logged), since `released` and `host_lost` can race.
- Add the "two decisions against one approval both still resolve their quoted spans" gate named in
  R2.1 if not already in §13.
