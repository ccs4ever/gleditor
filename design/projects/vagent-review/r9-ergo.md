# vagent review, round 9 - ergonomics and surface-language critic (focused round)

Document: design/vagent-agent-language.md (v3.6). Read: my R7 critique, the R8 disposition, and the
git diff v3.4..v3.6. The surface-language changes since my last review are small: the `for` note and
the C-style `else` sentence in section 8.9, `ChildStopping{child, cause}` in section 9, and the
fatal `Initialize` failure in section 4. The remainder is P12-P14 bookkeeping, which does not touch
the grammar, the typing rules or the examples.

Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Previous BLOCKING / MAJOR findings

- R6 #1 (types of `:=`, `<-`, `for`, `while`, `if` arms): RESOLVED (unchanged, confirmed R7).
- R6 #2 (entry point `module/Agent(literals)`): RESOLVED (unchanged, confirmed R7).
- No R7 BLOCKING or MAJOR existed.

## BLOCKING / MAJOR

None. Checks made on the new text:

- "A `for` loop begins only at the start of an expression, so the two never meet" (8.9): after the
  operands of `emit` the parser is mid-postfix-expression, so `for` there can only be the recipient
  modifier; `emit E & for x in xs do ...` has `for` after `&`, which is an expression start. No
  conflict, and the R7 lookahead concern is withdrawn.
- "`else` binds to the nearest unmatched `if`, `achieve`, `ask` or `await`, as in C": closes my R7
  dangling-else minor. The braced-body rule for `achieve`/`ask`/`await` keeps it enforceable.
- Fatal init failure: the program author sees it as a `ChildStopping` with a cause on the parent, so
  the behaviour is observable from the language. No grammar or example is affected.

## MINOR

- The cause value `"init"` is written as a string literal in section 4, while other causes (`fuel`,
  `cycles`, `depth`, `remote_fault`) are listed as names; section 9 should state once that `cause`
  is a `Str` drawn from a closed set that includes `init`, so a handler can match on it.
- R6/R7 leftovers (a timer example, `return` in plans, newlines outside a `Block`) remain
  unaddressed; none blocks an implementer.

VERDICT: CONVERGED
