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

} // namespace

// effect { let $s := submit($cap, read_bytes(off, max));
//          match $s { admitted($q) => await($q), refused($e) => $e } }
//
//   MakeTerm read_bytes(off, max)   -> spec
//   Submit   $cap, spec             -> sum, arm, payload
//   BranchFalseCell arm, ADMITTED   ; arm 0 is Admitted
//   CopyCellRefs payload            -> result        ; refused($e) => $e
//   jump END
//   ADMITTED: Await payload         -> result        ; admitted($q) =>
//   await($q) END
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

  const auto capability = bound(effect.capabilityVar);
  const auto offset     = operand(effect.offset);
  const auto length     = operand(effect.maxBytes);

  const auto spec     = core_.arena().makeCell();
  const auto makeTerm = emitOp(OpcodeKind::MakeTerm, "#IO_SPEC");
  emitInputConstant(makeTerm, std::string{readBytesFunctor});
  emitInput(makeTerm, offset);
  emitInput(makeTerm, length);
  emitOutput(makeTerm, spec);

  const auto submission = core_.arena().makeCell();
  const auto arm        = core_.arena().makeCell();
  const auto payload    = core_.arena().makeCell();
  const auto submit     = emitOp(OpcodeKind::Submit, "#IO_SUBMIT");
  emitInput(submit, capability);
  emitInput(submit, spec);
  emitOutput(submit, submission);
  emitOutput(submit, arm);
  emitOutput(submit, payload);

  const auto result   = core_.arena().makeCell();
  const auto admitted = mintOpcode(OpcodeKind::Nop, "#IO_ADMITTED");
  const auto end      = mintOpcode(OpcodeKind::Nop, "#IO_END");
  const auto match    = emitOp(OpcodeKind::BranchFalseCell, "#IO_MATCH");
  emitInput(match, arm);
  emitInput(match, admitted);
  const auto refused = emitOp(OpcodeKind::CopyCellRefs, "#IO_REFUSED");
  emitInput(refused, payload);
  emitOutput(refused, result);
  const auto join = emitOp(OpcodeKind::BranchFalseCell, "#IO_JOIN");
  emitInputConstant(join, false);
  emitInput(join, end);
  appendOpcode(admitted);
  const auto await = emitOp(OpcodeKind::Await, "#IO_AWAIT");
  emitInput(await, payload);
  emitOutput(await, result);
  appendOpcode(end);

  ioActions_.push_back(
      {.submitOpcode = submit, .awaitOpcode = await, .statement = statement});
  lastResultCells_ = {result};
  return result;
}

} // namespace xanadu::vql
