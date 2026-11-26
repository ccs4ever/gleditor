#include <algorithm>
#include <array>
#include <string>

#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/io_match.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

namespace {

/// The functor of the request description a `read_bytes(offset, max)` call
/// builds. The host that stages a read decodes this term; the VM only carries
/// it as a cell.
constexpr const char *readBytesFunctor = "read_bytes";
/// The functors of the mutating descriptions: terms the host decodes, never
/// the VM.
constexpr const char *writeBytesFunctor  = "write_bytes";
constexpr const char *appendBytesFunctor = "append_bytes";
constexpr const char *syncFunctor        = "sync";

} // namespace

// A script I/O action lowers to host-facing opcodes only: no I/O schema is
// named here. The four shapes (ast.hpp, IoEffectStatement):
//
// Read -- effect { let $s := submit($cap, read_bytes(off, max)) [within N u];
//                  match $s { admitted($q) => await($q), refused($e) => $e } }
//
//   MakeTerm read_bytes(off, max[, N, unit]) -> spec
//   (write_bytes(off, src[, N, unit]), append_bytes(src[, N, unit]) and
//   sync([N, unit]) build their own term the same way; `src` is a text cell
//   or an outcome cell, which the host reads when it stages the request)
//   Submit   $cap, spec             -> sum, arm, payload
//   BranchFalseCell arm, ADMITTED   ; arm 0 is Admitted
//   CopyCellRefs payload            -> result        ; refused($e) => $e
//   jump END
//   ADMITTED: Await payload         -> result        ; admitted($q) =>
//   await($q) END
//
//   A detached read (`admitted($q) => $q`) copies the payload on both arms: the
//   request is the value and nothing waits.
//
// Await / Cancel -- effect { await($h) }, effect { cancel($h) }
//
//   RelationNext io_handle_arm | io_handle_cancel ($h) -> arm
//   BranchFalseCell arm, WAIT       ; arm 0: $h is a request to await
//   CopyCellRefs $h                 -> result        ; an outcome is the value
//   jump END
//   WAIT: Await $h                  -> result
//
// Capture -- effect { capture($r[, off, len]) } [as $c]
//
//   RelationNext io_capture_begin | io_capture_range ($r[, off, len]) -> h
//   LOOP: RelationNext io_capture_arm (h, h) -> arm, x
//   BranchFalseCell arm, WAIT        ; arm 0: x is the request to await
//   CopyCellRefs x                   -> result       ; arm 1: x is the value
//   jump END
//   WAIT: Await x                    -> step
//   RelationNext io_capture_step (h, step)
//   jump LOOP
//
// begin runs once (a resume continues at the Await); the host takes the next
// step of the capture each pass, so a capture is its append plus one or two
// durable steps, and a refusal or failure ends the loop with an outcome.
//
// Open -- effect { open("path") } [as $cap]
//
//   RelationNext io_open (path, mode) -> handle      ; the request or Rejected
//   (then the Await / Copy of $h above, over the handle)
//   RelationNext io_open_capability (outcome) -> found, capability
//   $cap := the capability when the open minted one, the outcome otherwise
//
// Read ... as $r (any submit, writes included) -- the outcome a name holds
// must outlive its request
//
//   RelationNext io_own_outcome (outcome) -> found, record
//   $r := the record when the host copied the outcome and retired the
//         request, the service's outcome itself when it kept it
//
// The record is also the statement's row, since the request's cells are gone.
//
// A name the compile has no binding for (vqueryc compiles with none) becomes a
// text cell naming it, which no Submit accepts as a capability, so such a
// program is inspectable and fails by value if it is ever run unbound.
std::optional<CellRef>
VQLCompiler::compileIoEffect(const IoEffectStatement &effect,
                             const std::size_t statement) {
  const auto bound = [&](const std::string &name) -> CellRef {
    return ioBoundCell(name);
  };
  const auto operand = [&](const IoOperand &value) -> CellRef {
    if (const auto *literal = std::get_if<std::int64_t>(&value))
      return emitConstant(*literal);
    return bound(std::get<std::string>(value));
  };
  const auto fresh = [&] { return core_.arena().makeCell(); };

  const auto relate = [&](const char *relation,
                          const std::vector<CellRef> &arguments) {
    return ioRelate(relation, arguments);
  };

  // `arm` is 0 for a request to await and nonzero for an outcome that already
  // is the value; both write @p result.
  const auto awaitOrKeep = [&](const CellRef arm, const CellRef handle,
                               const CellRef result) -> CellRef {
    const auto waiting = mintOpcode(OpcodeKind::Nop, "#IO_WAIT");
    const auto end     = mintOpcode(OpcodeKind::Nop, "#IO_END");
    const auto branch  = emitOp(OpcodeKind::BranchFalseCell, "#IO_MATCH");
    emitInput(branch, arm);
    emitInput(branch, waiting);
    const auto keep = emitOp(OpcodeKind::CopyCellRefs, "#IO_KEEP");
    emitInput(keep, handle);
    emitOutput(keep, result);
    const auto join = emitOp(OpcodeKind::BranchFalseCell, "#IO_JOIN");
    emitInputConstant(join, false);
    emitInput(join, end);
    appendOpcode(waiting);
    const auto await = emitOp(OpcodeKind::Await, "#IO_AWAIT");
    emitInput(await, handle);
    emitOutput(await, result);
    appendOpcode(end);
    return await;
  };

  const auto result = fresh();
  CellRef firstOpcode{};
  std::optional<CellRef> awaitOpcode;
  std::optional<CellRef> requestSlot;
  std::optional<CellRef> binding;

  using Kind = IoEffectStatement::Kind;
  switch (effect.kind) {
  case Kind::Read: {
    const auto capability = bound(effect.capabilityVar);

    const auto spec     = fresh();
    const auto makeTerm = emitOp(OpcodeKind::MakeTerm, "#IO_SPEC");
    const auto source   = [&] {
      return effect.source.isVariable ? bound(effect.source.text)
                                        : emitConstant(effect.source.text);
    };
    switch (effect.request) {
    case IoRequestKind::ReadBytes:
      emitInputConstant(makeTerm, std::string{readBytesFunctor});
      emitInput(makeTerm, operand(effect.offset));
      emitInput(makeTerm, operand(effect.maxBytes));
      break;
    case IoRequestKind::WriteBytes:
      emitInputConstant(makeTerm, std::string{writeBytesFunctor});
      emitInput(makeTerm, operand(effect.offset));
      emitInput(makeTerm, source());
      break;
    case IoRequestKind::AppendBytes:
      emitInputConstant(makeTerm, std::string{appendBytesFunctor});
      emitInput(makeTerm, source());
      break;
    case IoRequestKind::Sync:
      emitInputConstant(makeTerm, std::string{syncFunctor});
      break;
    }
    if (effect.within) {
      emitInput(makeTerm, operand(effect.within->amount));
      emitInputConstant(makeTerm, effect.within->unitMillis);
    }
    emitOutput(makeTerm, spec);

    const auto submission = fresh();
    const auto arm        = fresh();
    const auto payload    = fresh();
    const auto submit     = emitOp(OpcodeKind::Submit, "#IO_SUBMIT");
    emitInput(submit, capability);
    emitInput(submit, spec);
    emitOutput(submit, submission);
    emitOutput(submit, arm);
    emitOutput(submit, payload);
    firstOpcode = submit;
    if (effect.detached) {
      // Both arms are the value: the request, or the Rejected outcome.
      const auto keep = emitOp(OpcodeKind::CopyCellRefs, "#IO_STARTED");
      emitInput(keep, payload);
      emitOutput(keep, result);
    } else {
      awaitOpcode = awaitOrKeep(arm, payload, result);
      requestSlot = payload;
    }
    break;
  }
  case Kind::Await:
  case Kind::Cancel: {
    const auto handle = bound(effect.handleVar);
    const auto call = relate(effect.kind == Kind::Cancel ? handleCancelRelation
                                                         : handleArmRelation,
                             {handle});
    firstOpcode = call.opcode;
    awaitOpcode = awaitOrKeep(call.answers.front(), handle, result);
    requestSlot = handle;
    break;
  }
  case Kind::Capture: {
    // A capture is a loop over the three relations of its host (design §6.2):
    // begin once, then until the host says it holds the value, await the
    // request it names and let it take the next step. A resume continues at
    // the Await, so begin never runs twice.
    std::vector<CellRef> arguments{bound(effect.captureVar)};
    if (effect.captureRanged) {
      arguments.push_back(operand(effect.offset));
      arguments.push_back(operand(effect.maxBytes));
    }
    const auto begun = relate(effect.captureRanged ? captureRangeRelation
                                                   : captureBeginRelation,
                              arguments);
    firstOpcode        = begun.opcode;
    const auto capture = begun.answers.front();
    const auto loop    = mintOpcode(OpcodeKind::Nop, "#IO_CAPTURE_LOOP");
    const auto waiting = mintOpcode(OpcodeKind::Nop, "#IO_CAPTURE_WAIT");
    const auto end     = mintOpcode(OpcodeKind::Nop, "#IO_CAPTURE_END");
    appendOpcode(loop);
    // Two answers: arm 0 or 1, and the request to await or the value.
    const auto arm   = relate(captureArmRelation, {capture, capture});
    const auto armed = emitOp(OpcodeKind::BranchFalseCell, "#IO_CAPTURE_ARM");
    emitInput(armed, arm.answers[0]);
    emitInput(armed, waiting);
    const auto keep = emitOp(OpcodeKind::CopyCellRefs, "#IO_CAPTURE_VALUE");
    emitInput(keep, arm.answers[1]);
    emitOutput(keep, result);
    const auto leave = emitOp(OpcodeKind::BranchFalseCell, "#IO_CAPTURE_LEAVE");
    emitInputConstant(leave, false);
    emitInput(leave, end);
    appendOpcode(waiting);
    const auto stepOutcome = fresh();
    const auto await       = emitOp(OpcodeKind::Await, "#IO_CAPTURE_AWAIT");
    emitInput(await, arm.answers[1]);
    emitOutput(await, stepOutcome);
    relate(captureStepRelation, {capture, stepOutcome});
    const auto again = emitOp(OpcodeKind::BranchFalseCell, "#IO_CAPTURE_AGAIN");
    emitInputConstant(again, false);
    emitInput(again, loop);
    appendOpcode(end);
    awaitOpcode = await;
    requestSlot = arm.answers[1];
    break;
  }
  case Kind::Open: {
    const auto path = effect.openPath.isVariable
                          ? bound(effect.openPath.text)
                          : emitConstant(effect.openPath.text);
    const auto mode = emitConstant(static_cast<std::int64_t>(effect.openMode));
    const auto opened = relate(openRelation, {path, mode});
    const auto handle = opened.answers.front();
    firstOpcode       = opened.opcode;
    const auto arm    = relate(handleArmRelation, {handle});
    awaitOpcode       = awaitOrKeep(arm.answers.front(), handle, result);
    requestSlot       = handle;
    {
      // The capability relation runs for every open, bound or not: it is what
      // records the capability for the script-end close, so an unbound open
      // is closed too. `as $name` is then the capability the open minted, or
      // the outcome itself when it minted none (a later read through it fails
      // the script, naming the open and its tag: ScriptRunner::failedOpen).
      const auto capability = fresh();
      const auto keep       = emitOp(OpcodeKind::CopyCellRefs, "#IO_OPEN_KEEP");
      emitInput(keep, result);
      emitOutput(keep, capability);
      const auto minted = relate(openCapabilityRelation, {result});
      const auto skip   = mintOpcode(OpcodeKind::Nop, "#IO_OPEN_END");
      const auto branch =
          emitOp(OpcodeKind::BranchFalseCell, "#IO_OPEN_MINTED");
      emitInput(branch, minted.found);
      emitInput(branch, skip);
      const auto take = emitOp(OpcodeKind::CopyCellRefs, "#IO_OPEN_CAPABILITY");
      emitInput(take, minted.answers.front());
      emitOutput(take, capability);
      appendOpcode(skip);
      if (effect.resultVar) binding = capability;
    }
    break;
  }
  }

  std::optional<CellRef> owned;
  // A read names its outcome by a record and retires its request at once. An
  // `await($h) as $r` (or `cancel`) names a record too, but the handle stays
  // admitted and awaitable: a second `await($h)` must still find its request,
  // so the copy leaves it in place and the script's end retires it.
  const bool namesRead = effect.kind == Kind::Read && !effect.detached;
  const bool namesHandleOutcome =
      effect.kind == Kind::Await || effect.kind == Kind::Cancel;
  if ((namesRead || namesHandleOutcome) && effect.resultVar) {
    // The host copies the finished outcome by value; if it declines (over its
    // retention budget) the service's outcome stays the value, as before.
    const auto record = fresh();
    const auto keep   = emitOp(OpcodeKind::CopyCellRefs, "#IO_OWN_KEEP");
    emitInput(keep, result);
    emitOutput(keep, record);
    const auto copied =
        relate(namesRead ? ownOutcomeRelation : ownCopyRelation, {result});
    const auto skip   = mintOpcode(OpcodeKind::Nop, "#IO_OWN_END");
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#IO_OWNED");
    emitInput(branch, copied.found);
    emitInput(branch, skip);
    const auto take = emitOp(OpcodeKind::CopyCellRefs, "#IO_OWN_TAKE");
    emitInput(take, copied.answers.front());
    emitOutput(take, record);
    appendOpcode(skip);
    owned = record;
  }

  ioActions_.push_back({.submitOpcode = firstOpcode,
                        .awaitOpcode  = awaitOpcode,
                        .requestSlot  = requestSlot,
                        .statement    = statement});
  if (effect.resultVar) {
    letNames_.erase(*effect.resultVar);
    streamBindings_[*effect.resultVar] = {
        binding.value_or(owned.value_or(result))};
  }
  lastResultCells_ = {owned.value_or(result)};
  return owned.value_or(result);
}

VQLCompiler::IoRelated
VQLCompiler::ioRelate(const char *relation,
                      const std::vector<CellRef> &arguments) {
  const auto fresh = [&] { return core_.arena().makeCell(); };
  const auto call  = emitOp(OpcodeKind::RelationNext, "#IO_RELATION");
  emitInputConstant(call, std::string{relation});
  emitInputConstant(call, std::int64_t{0});
  emitInputConstant(call, std::int64_t{0});
  for (const auto argument : arguments) emitInput(call, argument);
  const auto found = fresh();
  emitOutput(call, found);
  emitOutput(call, fresh());
  emitOutput(call, fresh());
  std::vector<CellRef> answers;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    answers.push_back(fresh());
    emitOutput(call, answers.back());
  }
  return {call, found, std::move(answers)};
}

CellRef VQLCompiler::ioBoundCell(const std::string &name) {
  const auto found = streamBindings_.find(name);
  if (found == streamBindings_.end() ||
      (found->second.size() != 1 && !letNames_.contains(name)))
    return emitConstant(std::string{"$"} + name);
  if (!letNames_.contains(name)) return found->second.front();

  // A name this script's own `let` bound holds a stream of rows, not the one
  // cell a host binds a session variable to. Narrowing it here, and refusing
  // by a typed code when it is not exactly one, is what keeps an empty or
  // several-valued `let` from being written as nothing, or as its own name.
  const auto fresh  = [&] { return core_.arena().makeCell(); };
  const auto refuse = [&](const CellRef condition, const char *what) {
    const auto assertion = emitOp(OpcodeKind::Assert, "#IO_LET_ASSERT");
    emitInput(assertion, condition);
    emitInputConstant(assertion,
                      std::string{vortex::frontend_codes::vqlEffectArgument} +
                          ": $" + name + " " + what);
  };
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#IO_LET_BEGIN");
  for (const auto cell : found->second) emitInput(begin, cell);
  const auto iterator = fresh();
  emitOutput(begin, iterator);
  const auto next = emitOp(OpcodeKind::IteratorNext, "#IO_LET_NEXT");
  emitInput(next, iterator);
  const auto item    = fresh();
  const auto present = fresh();
  emitOutput(next, item);
  emitOutput(next, present);
  refuse(present, "holds no value, so an action cannot use it");
  const auto more = emitOp(OpcodeKind::IteratorNext, "#IO_LET_EXTRA");
  emitInput(more, iterator);
  const auto extra       = fresh();
  const auto extraExists = fresh();
  emitOutput(more, extra);
  emitOutput(more, extraExists);
  const auto none = emitOp(OpcodeKind::Not, "#IO_LET_NOT");
  emitInput(none, extraExists);
  const auto alone = fresh();
  emitOutput(none, alone);
  refuse(alone, "holds several values, and an action needs exactly one");
  return item;
}

std::optional<CellRef>
VQLCompiler::compileIoMatch(const IoMatchStatement &match,
                            const std::size_t statement) {
  // match $r { tag($a, $b) => item, ..., _ => item }
  //
  //   RelationNext io_match_tag ($r) -> ordinal      ; the host refuses a
  //                                                  ; value that is no outcome
  //   per arm:  Eq ordinal, TAG ; BranchFalseCell -> NEXT
  //             per binder: RelationNext <field relation> ($r) -> found, x
  //                         a stream that is empty unless found
  //             the arm's row item, as a one-item `return`, into the result
  //             jump END
  //   NEXT: ...
  //   END
  //
  // The dispatch is a branch on the ordinal the host answers, and every
  // binder is a relation the path functions already use, so the match names
  // no outcome schema and adds no opcode. An arm after the last tag a typed
  // exhaustive match names cannot run, and the parser has already refused a
  // match that could fall through.
  const auto subject      = ioBoundCell(match.subject);
  const auto resultStream = emitStreamClear();
  const auto dispatched   = ioRelate(matchTagRelation, {subject});
  ioActions_.push_back({.submitOpcode = dispatched.opcode,
                        .awaitOpcode  = std::nullopt,
                        .requestSlot  = std::nullopt,
                        .statement    = statement});
  const auto ordinal = dispatched.answers.front();
  const auto end     = mintOpcode(OpcodeKind::Nop, "#IO_MATCH_END");
  for (const auto &arm : match.arms) {
    const auto next = mintOpcode(OpcodeKind::Nop, "#IO_MATCH_NEXT");
    if (arm.tag) {
      const auto same = emitOp(OpcodeKind::Eq, "#IO_MATCH_TAG");
      emitInput(same, ordinal);
      emitInputConstant(same, static_cast<std::int64_t>(*arm.tag));
      const auto chosen = core_.arena().makeCell();
      emitOutput(same, chosen);
      const auto skip = emitOp(OpcodeKind::BranchFalseCell, "#IO_MATCH_SKIP");
      emitInput(skip, chosen);
      emitInput(skip, next);
    }
    {
      auto variables     = varBindings_;
      auto streams       = streamBindings_;
      auto functors      = functorBindings_;
      auto lets          = letNames_;
      const auto context = ambientContext_;
      struct Restore {
        VQLCompiler &compiler;
        decltype(variables) &variables;
        decltype(streams) &streams;
        decltype(functors) &functors;
        decltype(lets) &lets;
        const decltype(context) &context;
        ~Restore() {
          compiler.varBindings_     = std::move(variables);
          compiler.streamBindings_  = std::move(streams);
          compiler.functorBindings_ = std::move(functors);
          compiler.letNames_        = std::move(lets);
          compiler.ambientContext_  = context;
        }
      } const restore{*this, variables, streams, functors, lets, context};

      if (arm.tag) {
        const auto fields = ioMatchFieldsOf(*arm.tag);
        for (std::size_t i = 0; i < arm.binders.size(); ++i) {
          if (!arm.binders[i] || !fields[i]) continue;
          const char *relation = outcomePayloadRelation;
          switch (*fields[i]) {
          case IoMatchField::Payload:
            relation = outcomePayloadRelation;
            break;
          case IoMatchField::ErrorCode:
            relation = outcomeErrorRelation;
            break;
          case IoMatchField::Detail:
            relation = outcomeDetailRelation;
            break;
          case IoMatchField::Uncertain:
            relation = outcomeUncertainRelation;
            break;
          case IoMatchField::Cause:
            relation = outcomeCauseRelation;
            break;
          }
          const auto field  = ioRelate(relation, {subject});
          const auto stream = emitStreamClear();
          const auto absent = mintOpcode(OpcodeKind::Nop, "#IO_MATCH_ABSENT");
          const auto branch =
              emitOp(OpcodeKind::BranchFalseCell, "#IO_MATCH_FIELD");
          emitInput(branch, field.found);
          emitInput(branch, absent);
          const std::array answer{field.answers.front()};
          emitAppendStream(stream, answer);
          appendOpcode(absent);
          functorBindings_.erase(*arm.binders[i]);
          letNames_.erase(*arm.binders[i]);
          streamBindings_[*arm.binders[i]] = {stream};
          varBindings_[*arm.binders[i]]    = stream;
        }
      }
      // The arm is a one-item `return`: the same lowering of a path or a
      // parenthesised value as anywhere else, so nothing new can differ.
      const auto armRows = compileExecutionBlock(ExecutionBlock{
          .action = ActionClause{.clause = ReturnClause{.items = {arm.body}}}});
      if (armRows) {
        const std::array rows{*armRows};
        emitAppendStream(resultStream, rows);
      }
    }
    const auto leave = emitOp(OpcodeKind::BranchFalseCell, "#IO_MATCH_LEAVE");
    emitInputConstant(leave, false);
    emitInput(leave, end);
    appendOpcode(next);
  }
  // The parser has checked the arms against the tags an operation can end
  // with, but a table can lag an adapter (a read once lacked `Conflict`). A
  // match with no `_` therefore ends in a refusal a tag nobody named reaches,
  // never in an empty result that reads as "nothing to return".
  const bool hasWildcard =
      std::ranges::any_of(match.arms, [](const auto &arm) { return !arm.tag; });
  if (!hasWildcard) {
    const auto unmatched = emitOp(OpcodeKind::Assert, "#IO_MATCH_UNMATCHED");
    emitInputConstant(unmatched, false);
    emitInputConstant(
        unmatched,
        std::string{vortex::frontend_codes::vqlMatch} +
            ": no arm of this match names the outcome's tag, and it has no "
            "'_' arm");
    ioActions_.push_back({.submitOpcode = unmatched,
                          .awaitOpcode  = std::nullopt,
                          .requestSlot  = std::nullopt,
                          .statement    = statement});
  }
  appendOpcode(end);
  lastResultCells_ = {resultStream};
  return resultStream;
}

} // namespace xanadu::vql
