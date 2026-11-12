// Q1: path functions sort_into, find, contains, count.
//
// count(path) is lowered with the counting loop emitClamp already uses: a
// zeroed Int64 cell, one in-place Add per member of the argument stream, boxed
// as a scalar.
//
// sort_into is a stable insertion sort performed on the live rank. Each
// candidate after the first is compared with the sorted prefix behind it,
// walking back with Link reads; the walk is bounded by a counter of prefix
// cells, so no identity comparison of cells is needed. The relink is Link /
// Break inside one mutation group, so a budget or key failure rolls the rank
// back instead of leaving it half sorted.
//
// contains is a substring search written with Value slices: the needle's
// length is counted by slicing it one byte at a time, then the haystack is
// sliced at every offset until a slice equals the needle or comes back empty.
// Quadratic, but it needs no new opcode.
//
// find refuses: it reads store documents and answers quote cells, and no
// opcode does either (see .logs/w1/q1b-text-opcode-proposal.md). Zero-argument
// count() as a path step needs the whole incoming walk, so compileSteps hands
// it to compilePathFunctionWalk ahead of the per-cell emitMap.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::vortex::OpcodeKind;

namespace {

constexpr std::int64_t towardPositive = 1;
constexpr std::int64_t towardNegative = -1;

[[noreturn]] void refusePathFunction(
    const FunctionInvocation &fn, const std::string &reason,
    const QueryErrorCategory category = QueryErrorCategory::Unsupported) {
  throw QueryRefusal(category, "VQL path function '" + fn.name + "' " + reason);
}

// Arity is checked before any argument is compiled, so a wrong call refuses
// without evaluating its arguments. It stays Unsupported: the standing tests
// (UnsupportedBuiltinArityRefusesBeforeArgumentsRun, VQLQueryErrorTest) pin
// arity refusals as runtime errors of that category. Named arguments the
// function does not have are a caller's mistake, so they are Malformed.
void requireArity(const FunctionInvocation &fn, const std::size_t expected,
                  const std::string_view shape) {
  if (fn.args.size() != expected)
    refusePathFunction(fn, "takes " + std::string(shape));
}

void requirePositional(const FunctionInvocation &fn) {
  if (std::ranges::any_of(
          fn.argNames, [](const std::string &name) { return !name.empty(); }))
    refusePathFunction(fn, "does not take named arguments",
                       QueryErrorCategory::Malformed);
}

// find reads store documents and answers quote cells over their spans; neither
// is something a graph can do today.
void refuseFind(const FunctionInvocation &fn) {
  requireArity(fn, 1, "one needle");
  refusePathFunction(fn, "is not compiled to Vortex yet: searching document "
                         "prose needs a substring-search opcode and a quote "
                         "constructor, and neither exists");
}

} // namespace

std::vector<CellRef> VQLCompiler::compilePathFunctionStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (fn.name == "count" && fn.args.empty())
    refusePathFunction(fn, "without an argument counts the whole walk, and "
                           "step lowering runs once per cell; not compiled "
                           "yet");
  if (fn.name == "find") refuseFind(fn);
  // A top-level call parses as a step, so the argument form is evaluated per
  // current cell, as the string builtin and user calls are.
  return compilePathFunctionCall(fn, hasContext ? std::optional{current}
                                                : std::nullopt);
}

std::vector<CellRef>
VQLCompiler::compilePathFunctionWalk(const FunctionInvocation &fn,
                                     const std::span<const CellRef> stream) {
  if (fn.name != "count" || !fn.args.empty())
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "unknown compiled function: " + fn.name);
  // The counting loop of count(path) over the incoming walk. The counter is
  // minted by an Add so an empty walk leaves a present zero, which boxes to a
  // present 0 rather than an absent result.
  const auto fresh = [&] { return core_.arena().makeCell(); };
  const auto add   = [&](CellRef left, CellRef right, CellRef destination) {
    const auto opcode = emitOp(OpcodeKind::Add, "#COUNT_WALK_ADD");
    emitInput(opcode, left);
    emitInput(opcode, right);
    emitOutput(opcode, destination);
  };
  const auto zero  = emitConstant(std::int64_t{0});
  const auto one   = emitConstant(std::int64_t{1});
  const auto count = fresh();
  add(zero, zero, count);
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#COUNT_WALK_BEGIN");
  for (const auto cell : stream) emitInput(begin, cell);
  const auto iterator = fresh();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef) { add(count, one, count); });
  const auto box = emitOp(OpcodeKind::VqlBoxScalar, "#COUNT_WALK_BOX");
  emitInput(box, count);
  const auto boxed = fresh();
  emitOutput(box, boxed);
  return {boxed};
}

std::vector<CellRef>
VQLCompiler::compilePathFunctionCall(const FunctionInvocation &fn,
                                     const std::optional<CellRef> context) {
  if (fn.name == "find") refuseFind(fn);
  if (fn.name != "count" && fn.name != "sort_into" && fn.name != "contains")
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "unknown compiled function: " + fn.name);

  // Validate every contract before the first argument is compiled.
  // The only path shape the lowered sort can order: one plain +dimension step
  // from an anchor that names one cell. Anything else may walk several ranks
  // or a non-contiguous section, which the insertion sort cannot detect at run
  // time (the oracle refuses those), so it refuses here, before any effect.
  std::optional<DimRef> sortDimension;
  if (fn.name == "count") {
    requireArity(fn, 1, "exactly one path argument");
    requirePositional(fn);
  } else if (fn.name == "contains") {
    requireArity(fn, 2, "two arguments");
    requirePositional(fn);
  } else {
    requireArity(fn, 2, "a rank path and a by: key");
    if (fn.argNames.size() != 2 || !fn.argNames[0].empty() ||
        fn.argNames[1] != "by")
      refusePathFunction(fn, "takes a positional rank path and a named by: key",
                         QueryErrorCategory::Malformed);
    const auto *target =
        std::get_if<std::shared_ptr<PathExpression>>(&fn.args.front().kind);
    constexpr auto shapeReason =
        "orders one rank section: its first argument must be a path from a "
        "single-cell anchor with exactly one plain +dimension step";
    if (!target || !*target) refusePathFunction(fn, shapeReason);
    const auto &path = **target;
    const auto *dimension =
        path.steps.size() == 1
            ? std::get_if<SignedDimensionStep>(&path.steps.front().selector)
            : nullptr;
    const auto &only = path.steps.empty() ? PathStep{} : path.steps.front();
    // A variable names one cell when it is bound as a scalar or as a stream
    // of exactly one; a longer stream would walk several ranks.
    const auto streamBound = streamBindings_.find(path.anchor.name);
    const auto scalarBound = varBindings_.find(path.anchor.name);
    const bool singleCellVariable =
        streamBound != streamBindings_.end()
            ? streamBound->second.size() == 1
            : scalarBound != varBindings_.end() && scalarBound->second;
    const bool plainAnchor =
        !path.isFunctor && path.anchorPredicates.empty() &&
        !path.anchorProjection && !path.shapeProjection && !path.cloneTail &&
        !path.anchor.versionPin && !path.anchor.derefMaster &&
        (path.anchor.kind == AnchorKind::Home ||
         path.anchor.kind == AnchorKind::NamedStore ||
         path.anchor.kind == AnchorKind::Root ||
         path.anchor.kind == AnchorKind::Cursor ||
         path.anchor.kind == AnchorKind::NamedCursor ||
         path.anchor.kind == AnchorKind::LiteralCellId ||
         path.anchor.kind == AnchorKind::Context ||
         (path.anchor.kind == AnchorKind::Variable && singleCellVariable));
    if (!dimension || !plainAnchor || dimension->isBidir ||
        dimension->direction != zigzag::DimVector::POS ||
        dimension->placement != Placement::Default ||
        !dimension->creates.empty() || dimension->dimName.starts_with('$') ||
        only.derefMaster || !only.predicates.empty() ||
        !only.shapePredicates.empty() || only.projectionAfter ||
        only.rangeClamp || only.versionPin || only.capturedFunctor ||
        only.yieldMode != YieldMode::Default)
      refusePathFunction(fn, shapeReason);
    const auto *lambda =
        std::get_if<std::shared_ptr<PathLambda>>(&fn.args[1].kind);
    if (lambda && !*lambda)
      throw std::invalid_argument("VQL path function 'sort_into' by: key is a "
                                  "null path lambda");
    sortDimension = resolveDimension(dimension->dimName);
  }

  const auto fresh      = [&] { return core_.arena().makeCell(); };
  const auto arithmetic = [&](OpcodeKind kind, CellRef left, CellRef right,
                              std::optional<CellRef> destination =
                                  std::nullopt) {
    const auto opcode = emitOp(kind, "#PATHFN_ARITHMETIC");
    emitInput(opcode, left);
    emitInput(opcode, right);
    const auto output = destination ? *destination : fresh();
    emitOutput(opcode, output);
    return output;
  };
  const auto negate = [&](CellRef value) {
    const auto opcode = emitOp(OpcodeKind::Not, "#PATHFN_NOT");
    emitInput(opcode, value);
    const auto output = fresh();
    emitOutput(opcode, output);
    return output;
  };
  const auto branchFalse = [&](CellRef condition, CellRef target) {
    const auto opcode = emitOp(OpcodeKind::BranchFalseCell, "#PATHFN_BRANCH");
    emitInput(opcode, condition);
    emitInput(opcode, target);
  };
  const auto jump = [&](CellRef target) {
    const auto opcode = emitOp(OpcodeKind::BranchFalseCell, "#PATHFN_JUMP");
    emitInputConstant(opcode, false);
    emitInput(opcode, target);
  };
  const auto label = [&](std::string_view name) {
    return mintOpcode(OpcodeKind::Nop, name);
  };
  const auto copyRef = [&](CellRef source, CellRef destination) {
    const auto opcode = emitOp(OpcodeKind::CopyCellRefs, "#PATHFN_COPY");
    emitInput(opcode, source);
    emitOutput(opcode, destination);
  };
  const auto clearRef = [&](CellRef destination) {
    const auto opcode = emitOp(OpcodeKind::ClearCellRefs, "#PATHFN_CLEAR");
    emitOutput(opcode, destination);
  };
  const auto readNeighbour = [&](CellRef cell, DimRef dimension,
                                 std::int64_t sign) {
    const auto opcode = emitOp(OpcodeKind::Link, "#PATHFN_NEIGHBOUR");
    emitInput(opcode, cell);
    emitInputConstant(opcode, static_cast<std::int64_t>(dimension));
    emitInputConstant(opcode, sign);
    const auto output = fresh();
    emitOutput(opcode, output);
    return output;
  };
  const auto writeNeighbour = [&](CellRef cell, DimRef dimension,
                                  std::int64_t sign, CellRef target) {
    const auto opcode = emitOp(OpcodeKind::Link, "#PATHFN_RELINK");
    emitInput(opcode, cell);
    emitInputConstant(opcode, static_cast<std::int64_t>(dimension));
    emitInputConstant(opcode, sign);
    emitInput(opcode, target);
  };
  const auto assertThat = [&](CellRef condition, std::string_view message) {
    const auto opcode = emitOp(OpcodeKind::Assert, "#PATHFN_ASSERT");
    emitInput(opcode, condition);
    emitInputConstant(opcode, std::string{message});
  };
  const auto slice = [&](CellRef text, CellRef offset, CellRef length) {
    const auto opcode = emitOp(OpcodeKind::Value, "#PATHFN_SLICE");
    emitInput(opcode, text);
    emitInput(opcode, offset);
    emitInput(opcode, length);
    const auto output = fresh();
    emitOutput(opcode, output);
    return output;
  };
  // The one cell a stream holds; asserts there is exactly one.
  const auto exactlyOne = [&](std::span<const CellRef> cells,
                              std::string_view message) {
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#PATHFN_ONE_BEGIN");
    for (const auto cell : cells) emitInput(begin, cell);
    const auto iterator = fresh();
    emitOutput(begin, iterator);
    const auto next = emitOp(OpcodeKind::IteratorNext, "#PATHFN_ONE_NEXT");
    emitInput(next, iterator);
    const auto item    = fresh();
    const auto present = fresh();
    emitOutput(next, item);
    emitOutput(next, present);
    assertThat(present, message);
    const auto more = emitOp(OpcodeKind::IteratorNext, "#PATHFN_ONE_EXTRA");
    emitInput(more, iterator);
    const auto extra       = fresh();
    const auto extraExists = fresh();
    emitOutput(more, extra);
    emitOutput(more, extraExists);
    assertThat(negate(extraExists), message);
    return item;
  };
  const auto firstOf = [&](std::span<const CellRef> cells) {
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#PATHFN_FIRST_BEGIN");
    for (const auto cell : cells) emitInput(begin, cell);
    const auto iterator = fresh();
    emitOutput(begin, iterator);
    const auto next = emitOp(OpcodeKind::IteratorNext, "#PATHFN_FIRST");
    emitInput(next, iterator);
    const auto item    = fresh();
    const auto present = fresh();
    emitOutput(next, item);
    emitOutput(next, present);
    return item;
  };
  const auto box = [&](CellRef scalar) {
    const auto opcode = emitOp(OpcodeKind::VqlBoxScalar, "#PATHFN_BOX");
    emitInput(opcode, scalar);
    const auto boxed = fresh();
    emitOutput(opcode, boxed);
    return std::vector<CellRef>{boxed};
  };

  if (fn.name == "count") {
    const auto inputs = compileArgument(fn.args.front(), context);

    // Counters are reset on every run, so one compiled call can be invoked
    // repeatedly; an empty argument leaves the zero in place.
    const auto zero  = emitConstant(std::int64_t{0});
    const auto one   = emitConstant(std::int64_t{1});
    const auto count = arithmetic(OpcodeKind::Add, zero, zero);
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#COUNT_BEGIN");
    for (const auto input : inputs) emitInput(begin, input);
    const auto iterator = fresh();
    emitOutput(begin, iterator);
    emitIteratorLoop(iterator, [&](CellRef) {
      arithmetic(OpcodeKind::Add, count, one, count);
    });
    return box(count);
  }

  if (fn.name == "contains") {
    const auto haystackCells = compileArgument(fn.args[0], context);
    const auto needleCells   = compileArgument(fn.args[1], context);
    const auto zero          = emitConstant(std::int64_t{0});
    const auto one           = emitConstant(std::int64_t{1});
    const auto emptyText     = emitConstant(std::string{});
    const auto found         = arithmetic(OpcodeKind::Eq, zero, one);

    // Either side absent answers false, as the direct evaluator did.
    const auto haystackPresent = emitStreamPresence(haystackCells);
    const auto needlePresent   = emitStreamPresence(needleCells);
    const auto done            = label("#CONTAINS_END");
    branchFalse(arithmetic(OpcodeKind::And, haystackPresent, needlePresent),
                done);
    const auto haystackCell = firstOf(haystackCells);
    const auto needleCell   = firstOf(needleCells);
    // Slices read a cell's spooled text, which a scalar minted at run time
    // (a count, a sum) does not have. VqlString states any scalar as its
    // canonical text first, so both sides are searched as the text the direct
    // evaluator compared.
    const auto asText = [&](CellRef scalar) {
      const auto opcode = emitOp(OpcodeKind::VqlString, "#CONTAINS_TEXT");
      emitInput(opcode, scalar);
      const auto text = fresh();
      emitOutput(opcode, text);
      return text;
    };
    const auto haystack = asText(haystackCell);
    const auto needle   = asText(needleCell);

    // Count the needle's bytes one slice at a time; the first empty slice is
    // its end.
    const auto length    = arithmetic(OpcodeKind::Add, zero, zero);
    const auto lengthTop = emitOp(OpcodeKind::Nop, "#CONTAINS_LENGTH");
    const auto lengthEnd = label("#CONTAINS_LENGTH_END");
    branchFalse(negate(arithmetic(OpcodeKind::Eq, slice(needle, length, one),
                                  emptyText)),
                lengthEnd);
    arithmetic(OpcodeKind::Add, length, one, length);
    jump(lengthTop);
    appendOpcode(lengthEnd);

    // An empty needle is in every haystack.
    const auto searchEnd = label("#CONTAINS_SEARCH_END");
    const auto nonEmpty  = label("#CONTAINS_NONEMPTY");
    branchFalse(arithmetic(OpcodeKind::Eq, length, zero), nonEmpty);
    arithmetic(OpcodeKind::Eq, zero, zero, found);
    jump(searchEnd);
    appendOpcode(nonEmpty);

    const auto offset = arithmetic(OpcodeKind::Add, zero, zero);
    const auto top    = emitOp(OpcodeKind::Nop, "#CONTAINS_SCAN");
    const auto window = slice(haystack, offset, length);
    // An empty window means the offset is past the end.
    branchFalse(negate(arithmetic(OpcodeKind::Eq, window, emptyText)),
                searchEnd);
    const auto advance = label("#CONTAINS_ADVANCE");
    branchFalse(arithmetic(OpcodeKind::Eq, window, needle), advance);
    arithmetic(OpcodeKind::Eq, zero, zero, found);
    jump(searchEnd);
    appendOpcode(advance);
    arithmetic(OpcodeKind::Add, offset, one, offset);
    jump(top);
    appendOpcode(searchEnd);
    appendOpcode(done);
    return box(found);
  }

  // sort_into
  const auto dimension  = *sortDimension;
  const auto candidates = compileArgument(fn.args.front(), context);
  const auto zero       = emitConstant(std::int64_t{0});
  const auto one        = emitConstant(std::int64_t{1});

  // The key of one candidate, computed with that candidate as context: the
  // lambda's result, or the path evaluated from it.
  const auto keyOf = [&](CellRef candidate) {
    std::vector<CellRef> cells;
    if (const auto *lambda =
            std::get_if<std::shared_ptr<PathLambda>>(&fn.args[1].kind)) {
      DefDeclaration inlineLambda{
          .name   = "#vql-inline-sort-key-" + std::to_string(allOps_.size()),
          .params = (*lambda)->params,
          .body   = (*lambda)->body,
      };
      cells = compileCall(inlineLambda, {}, candidate, true, false);
    } else {
      cells = compileArgument(fn.args[1], candidate);
    }
    return exactlyOne(cells, "sort_into key must return exactly one scalar");
  };

  // Gt orders text byte-wise but converts everything else through double, so a
  // mixed or non-finite key would sort silently wrong. Every key is therefore
  // classified (text, integer, decimal) with the contract matcher, which reads
  // a cell's typed scalar the way the comparison opcodes do, and refused
  // unless it is of the first key's class. Integer against decimal is refused
  // too: the comparison goes through double and is inexact beyond 2^53. The
  // failure messages name the kind that was refused; "mixed" names the pair
  // by class because one Assert carries one static text.
  struct KeyClass {
    CellRef text;
    CellRef integer;
    CellRef decimal;
  };
  const auto matchesKind = [&](BasicType::Kind kind, CellRef subject) {
    const TypeExpr type{.unionType = {.members = {PrimaryType{
                                          .kind = BasicType{.kind = kind}}}}};
    return emitContractMatch(type, std::span{&subject, std::size_t{1}}, false);
  };
  const auto classify = [&](CellRef key) {
    const auto text    = matchesKind(BasicType::Kind::String, key);
    const auto integer = matchesKind(BasicType::Kind::Int, key);
    const auto decimal = matchesKind(BasicType::Kind::Float, key);
    const auto boolean = matchesKind(BasicType::Kind::Bool, key);
    const auto numeric = negate(text);
    const auto isInt   = arithmetic(OpcodeKind::And, numeric, integer);
    const auto isDec   = arithmetic(OpcodeKind::And,
                                    arithmetic(OpcodeKind::And, numeric, decimal),
                                    negate(integer));
    assertThat(negate(arithmetic(OpcodeKind::And, numeric, boolean)),
               "VQL path function 'sort_into' cannot order Bool keys");
    assertThat(arithmetic(OpcodeKind::Or, text,
                          arithmetic(OpcodeKind::Or, isInt, isDec)),
               "VQL path function 'sort_into' orders text, integer and "
               "decimal keys only");
    assertThat(
        arithmetic(OpcodeKind::Or, text, arithmetic(OpcodeKind::Eq, key, key)),
        "VQL path function 'sort_into' cannot order NaN keys");
    return KeyClass{text, isInt, isDec};
  };
  const auto firstClass = KeyClass{fresh(), fresh(), fresh()};

  const auto first    = fresh();
  const auto before   = fresh();
  const auto count    = arithmetic(OpcodeKind::Add, zero, zero);
  const auto steps    = fresh();
  const auto walk     = fresh();
  const auto follower = fresh();
  clearRef(first);
  clearRef(before);

  // The walk never repeats a candidate, but on a ring it stops at the anchor,
  // so the section's predecessor is also its successor. The oracle refused
  // that; so does this, before anything moves. Cell identity is read as the
  // number ProbeLink answers for a neighbour.
  const auto last    = fresh();
  const auto visited = arithmetic(OpcodeKind::Add, zero, zero);
  clearRef(last);
  const auto survey = emitOp(OpcodeKind::StreamBegin, "#SORT_SURVEY");
  for (const auto candidate : candidates) emitInput(survey, candidate);
  const auto surveyIterator = fresh();
  emitOutput(survey, surveyIterator);
  emitIteratorLoop(surveyIterator, [&](CellRef candidate) {
    const auto notFirst = label("#SORT_SURVEY_NEXT");
    branchFalse(arithmetic(OpcodeKind::Eq, visited, zero), notFirst);
    copyRef(candidate, first);
    copyRef(readNeighbour(candidate, dimension, towardNegative), before);
    appendOpcode(notFirst);
    copyRef(candidate, last);
    arithmetic(OpcodeKind::Add, visited, one, visited);
  });
  const auto unguarded = label("#SORT_NOT_A_RING");
  branchFalse(arithmetic(OpcodeKind::Gt, visited, zero), unguarded);
  branchFalse(emitCellPresence(before), unguarded);
  const auto identityBeside = [&](CellRef anchor, std::int64_t sign) {
    const auto opcode = emitOp(OpcodeKind::ProbeLink, "#SORT_IDENTITY");
    emitInput(opcode, anchor);
    emitInputConstant(opcode, static_cast<std::int64_t>(dimension));
    emitInputConstant(opcode, sign);
    const auto identity = fresh();
    emitOutput(opcode, identity);
    return identity;
  };
  const auto predecessor = identityBeside(first, towardNegative);
  const auto successor   = identityBeside(last, towardPositive);
  assertThat(negate(arithmetic(OpcodeKind::Eq, predecessor, successor)),
             "VQL path function 'sort_into' cannot reorder a cyclic rank");
  appendOpcode(unguarded);

  // One group around the whole sort: a failing key or an exhausted budget
  // rolls the rank back instead of leaving it half sorted.
  const auto group = beginMutationGroup("#SORT_GROUP");
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#SORT_BEGIN");
  for (const auto candidate : candidates) emitInput(begin, candidate);
  const auto iterator = fresh();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef candidate) {
    const auto notFirst = label("#SORT_NOT_FIRST");
    const auto end      = label("#SORT_ITEM_END");
    branchFalse(arithmetic(OpcodeKind::Eq, count, zero), notFirst);
    jump(end);
    appendOpcode(notFirst);

    // The sorted prefix is the `count` cells behind the candidate. Walk back
    // over those whose key is strictly greater; strictness keeps equal keys
    // in their original order. `follower` trails `walk` by one cell, so the
    // insertion point is between them.
    // The first candidate's key is read when the second arrives, so a
    // one-cell section evaluates no key, as the oracle did.
    const auto notSecond = label("#SORT_NOT_SECOND");
    branchFalse(arithmetic(OpcodeKind::Eq, count, one), notSecond);
    const auto firstKey = classify(keyOf(first));
    arithmetic(OpcodeKind::Or, firstKey.text, firstKey.text, firstClass.text);
    arithmetic(OpcodeKind::Or, firstKey.integer, firstKey.integer,
               firstClass.integer);
    arithmetic(OpcodeKind::Or, firstKey.decimal, firstKey.decimal,
               firstClass.decimal);
    appendOpcode(notSecond);
    const auto candidateKey   = keyOf(candidate);
    const auto candidateClass = classify(candidateKey);
    for (const auto [mine, reference] :
         {std::pair{candidateClass.text, firstClass.text},
          std::pair{candidateClass.integer, firstClass.integer},
          std::pair{candidateClass.decimal, firstClass.decimal}})
      assertThat(arithmetic(OpcodeKind::Eq, mine, reference),
                 "VQL path function 'sort_into' cannot mix text, integer and "
                 "decimal keys");
    const auto previous = readNeighbour(candidate, dimension, towardNegative);
    copyRef(previous, walk);
    copyRef(candidate, follower);
    arithmetic(OpcodeKind::Add, zero, zero, steps);
    const auto scan     = emitOp(OpcodeKind::Nop, "#SORT_SCAN");
    const auto scanDone = label("#SORT_SCAN_END");
    branchFalse(arithmetic(OpcodeKind::Lt, steps, count), scanDone);
    const auto walkKey = keyOf(walk);
    branchFalse(arithmetic(OpcodeKind::Gt, walkKey, candidateKey), scanDone);
    copyRef(walk, follower);
    copyRef(readNeighbour(walk, dimension, towardNegative), walk);
    arithmetic(OpcodeKind::Add, steps, one, steps);
    jump(scan);
    appendOpcode(scanDone);

    branchFalse(arithmetic(OpcodeKind::Gt, steps, zero), end);
    // Detach the candidate: close the gap behind it.
    const auto successor = readNeighbour(candidate, dimension, towardPositive);
    const auto noSuccessor = label("#SORT_LAST");
    const auto detached    = label("#SORT_DETACHED");
    branchFalse(emitCellPresence(successor), noSuccessor);
    writeNeighbour(previous, dimension, towardPositive, successor);
    jump(detached);
    appendOpcode(noSuccessor);
    const auto cut = emitOp(OpcodeKind::Break, "#SORT_CUT");
    emitInput(cut, previous);
    emitInputConstant(cut, static_cast<std::int64_t>(dimension));
    emitInputConstant(cut, towardPositive);
    appendOpcode(detached);
    // Insert it before `follower`, then after `walk` unless the walk ran off
    // the start of the section.
    writeNeighbour(candidate, dimension, towardPositive, follower);
    const auto atStart = label("#SORT_AT_START");
    branchFalse(emitCellPresence(walk), atStart);
    writeNeighbour(walk, dimension, towardPositive, candidate);
    appendOpcode(atStart);

    appendOpcode(end);
    arithmetic(OpcodeKind::Add, count, one, count);
  });
  endMutationGroup(group, "#SORT_GROUP_END");

  // The result is the sorted section in rank order: from the cell after the
  // section's predecessor, or from the rank's head when it had none.
  const auto result = emitStreamClear();
  const auto empty  = label("#SORT_EMPTY");
  branchFalse(arithmetic(OpcodeKind::Gt, count, zero), empty);
  const auto head      = fresh();
  const auto noBefore  = label("#SORT_NO_BEFORE");
  const auto headFound = label("#SORT_HEAD");
  branchFalse(emitCellPresence(before), noBefore);
  copyRef(readNeighbour(before, dimension, towardPositive), head);
  jump(headFound);
  appendOpcode(noBefore);
  copyRef(emitExtreme(first, dimension, zigzag::DimVector::NEG), head);
  appendOpcode(headFound);
  const auto seed = emitOp(OpcodeKind::StreamAppend, "#SORT_YIELD");
  emitInput(seed, head);
  emitOutput(seed, result);
  const auto taken    = arithmetic(OpcodeKind::Add, zero, one);
  const auto walkRank = emitOp(OpcodeKind::RankBegin, "#SORT_RANK");
  emitInput(walkRank, head);
  emitInputConstant(walkRank, static_cast<std::int64_t>(dimension));
  emitInputConstant(walkRank, towardPositive);
  const auto rankIterator = fresh();
  emitOutput(walkRank, rankIterator);
  emitIteratorLoop(rankIterator, [&](CellRef item) {
    const auto skip = label("#SORT_BEYOND");
    branchFalse(arithmetic(OpcodeKind::Lt, taken, count), skip);
    const auto append = emitOp(OpcodeKind::StreamAppend, "#SORT_YIELD");
    emitInput(append, item);
    emitOutput(append, result);
    arithmetic(OpcodeKind::Add, taken, one, taken);
    appendOpcode(skip);
  });
  appendOpcode(empty);
  return {result};
}

std::optional<CellRef>
VQLCompiler::compilePathFunctionValue(const FunctionInvocation &fn,
                                      const CellRef ctxCell) {
  // count and contains answer exactly one scalar; sort_into answers a stream,
  // whose first cell is the value, as every other stream-valued call here.
  const auto cells = compilePathFunctionCall(fn, ctxCell);
  if (cells.empty()) return std::nullopt;
  return emitFirstCell(cells.front());
}

} // namespace xanadu::vql
