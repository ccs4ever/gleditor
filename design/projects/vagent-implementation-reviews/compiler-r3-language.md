# Compiler R3 language semantics review

**Scope:** audit of the `ResultUse::{Value, Discard}` compiler addition against
`design/vagent-agent-language.md` §§8.2–8.3, 6.7 and 5.8. Read-only; no build or test command run.

## Result: CONVERGED

The addition keeps result use separate from position boundedness, as the design requires. The
default API mode retains expression values. In discard mode, successful results become `()` while
`Bottom` remains failure. In a block, only nonfinal children inherit discard; the final child
inherits the caller's mode. Conditional arms inherit discard when the conditional's result is
discarded. Conditions and expression operands retain value use, and a `let` initializer is
preflighted in value mode.

The emitter implements discard as a value normalization after successful execution: it evaluates the
expression into a temporary, then writes Unit to the requested output. Thus its choices and writes
still run under the same bounded choice point. It does not make an otherwise speculative `:=` legal,
and it does not prevent a bounded discarded statement from using `:=`. Failure still takes the
failure continuation and is rolled back by the enclosing mark.

Coverage read from the tests includes discarded unlike conditional arms, nonfinal versus final block
results, preserved failure/arm selection, condition purity under discard, and a discarded block
containing a local initializer and bounded assignment. No concrete mismatch was found in this
addition.
