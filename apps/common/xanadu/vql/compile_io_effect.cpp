#include <string>

#include "common/xanadu/vql/compiler.hpp"

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
    if (const auto found = streamBindings_.find(name);
        found != streamBindings_.end() && found->second.size() == 1)
      return found->second.front();
    return emitConstant(std::string{"$"} + name);
  };
  const auto operand = [&](const IoOperand &value) -> CellRef {
    if (const auto *literal = std::get_if<std::int64_t>(&value))
      return emitConstant(*literal);
    return bound(std::get<std::string>(value));
  };
  const auto fresh = [&] { return core_.arena().makeCell(); };

  // One RelationNext call: the single answer per argument is the host's value.
  struct Related {
    CellRef opcode;
    CellRef found;
    std::vector<CellRef> answers;
  };
  const auto relate = [&](const char *relation,
                          const std::vector<CellRef> &arguments) -> Related {
    const auto call = emitOp(OpcodeKind::RelationNext, "#IO_RELATION");
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
  if (effect.kind == Kind::Read && !effect.detached && effect.resultVar) {
    // The host copies the finished outcome by value and retires the request
    // at once; if it declines (over its retention budget) the service's
    // outcome stays the value, as before.
    const auto record = fresh();
    const auto keep   = emitOp(OpcodeKind::CopyCellRefs, "#IO_OWN_KEEP");
    emitInput(keep, result);
    emitOutput(keep, record);
    const auto copied = relate(ownOutcomeRelation, {result});
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
  if (effect.resultVar)
    streamBindings_[*effect.resultVar] = {
        binding.value_or(owned.value_or(result))};
  lastResultCells_ = {owned.value_or(result)};
  return owned.value_or(result);
}

} // namespace xanadu::vql
