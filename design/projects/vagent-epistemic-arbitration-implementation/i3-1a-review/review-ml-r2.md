# ML gate review, round 2: I3-1a

Re-read epistemic_policy.{hpp,cpp} and the new tests in the working tree. Suites not rerun by me.

Disposition of round 1:

- F1 (score_tolerance default): resolved. `double scoreTolerance{kDefaultScoreTolerance}` (1e-9,
  rationale comment), always hashed; explicit-default == omitted is tested. `allow_untested` added
  as resolved bool, consensus+true refused. Deferring epsilon_w/epsilon_floor/features/health with a
  tag-bump rule is accepted (they are I4-consumed constants; I4 must put them in the policy or hash
  tag when it lands - noted for the I4 gate).
- F2 (hash test): resolved. HashChangesWithEveryFieldAndOptionalPresence mutates all key-driven
  fields, strategy, drops each optional, checks Int vs Float and +/-0.0.
- F3 (domain tests): resolved. EveryDomainBoundaryAcceptsOrRefuses covers boundaries for every
  numeric/duration/text key, 2^53 edge, flag type, consensus rule.
- F4 alignment >= 0: fixed. F5 2^53 widening: fixed in `matches`. F7 -0.0: normalised in encoding.
  F8 duration cap 1e12 ms applied to max_age/skew/decay/alignment. F10 prior cap 1e12: fixed. F9
  docs: accepted.

Remaining MINOR (non-gating):

1. Float duration literals still round to whole ms in the lexer (1.5ms -> 2ms) silently; harmless
   for policy but worth a lexer note.
1. `model` and `continuous` are not in the mutation table (model has one admitted value; continuous
   follows the signature) - acceptable.
1. min_confidence and clock_skew mutation cases are covered; the lowered master record is still
   compared for only a subset of fields (hash is the full guard).
1. alignment/decay remain accepted on discrete predicates; harmless until I4 defines use.

Tally: BLOCKING 0, MAJOR 0, MINOR 4.

VERDICT: PASS
