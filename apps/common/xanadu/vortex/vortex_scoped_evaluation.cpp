#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace zigzag::vortex {
namespace {
[[noreturn]] void deny(ScopedRefusal code, const char *reason) {
  throw ScopeError(code, reason);
}
void require(bool condition, ScopedRefusal code, const char *reason) {
  if (!condition) deny(code, reason);
}
bool pure(OpcodeKind kind) {
  switch (kind) {
  case OpcodeKind::Add:
  case OpcodeKind::Sub:
  case OpcodeKind::Mul:
  case OpcodeKind::Div:
  case OpcodeKind::DivMod:
  case OpcodeKind::Mod:
  case OpcodeKind::Neg:
  case OpcodeKind::Eq:
  case OpcodeKind::Neq:
  case OpcodeKind::Lt:
  case OpcodeKind::Lte:
  case OpcodeKind::Gt:
  case OpcodeKind::Gte:
  case OpcodeKind::And:
  case OpcodeKind::Or:
  case OpcodeKind::Not:
  case OpcodeKind::Abs:
  case OpcodeKind::Min:
  case OpcodeKind::Max:
  case OpcodeKind::Clamp:
  case OpcodeKind::Trim:
  case OpcodeKind::ToLower:
  case OpcodeKind::ToUpper:
  case OpcodeKind::Exp:
  case OpcodeKind::Ln:
  case OpcodeKind::Sqrt:
  case OpcodeKind::Jump:
  case OpcodeKind::Branch:
  case OpcodeKind::Call:
  case OpcodeKind::Return:
  case OpcodeKind::Nop:
  case OpcodeKind::Halt:
  case OpcodeKind::Choice:
  case OpcodeKind::Fail:
  case OpcodeKind::Cut:
  case OpcodeKind::ScopedEvaluate:
    return true;
  default:
    return false;
  }
}
std::optional<std::size_t> controlIndex(OpcodeKind kind) {
  switch (kind) {
  case OpcodeKind::Jump:
  case OpcodeKind::Call:
  case OpcodeKind::Choice:
    return 0;
  case OpcodeKind::Branch:
    return 1;
  default:
    return std::nullopt;
  }
}
ExecutionResult refusalResult(ScopedRefusal code, std::string message) {
  ExecutionResult r;
  r.success       = false;
  r.errorMessage  = std::move(message);
  r.scopedRefusal = code;
  return r;
}
} // namespace
void VortexVM::verifyScope(ScopedFrame &f, ScopeCodec &codec, CellRef entry,
                           CellRef frameRegister, CellRef args,
                           CellRef descriptor) {
  auto &a = core_.arena();
  f.graph.insert(f.descriptor.graph.begin(), f.descriptor.graph.end());
  require(entry != f.completion && f.graph.contains(entry) &&
              f.graph.contains(f.completion) &&
              getOpcodeKind(f.completion) == OpcodeKind::Nop,
          ScopedRefusal::InvalidGraph,
          "Scope entry/completion outside manifest");
  const auto protectedCells = codec.protectedCells();
  for (auto op : f.graph) {
    codec.charge();
    require(!protectedCells.contains(op), ScopedRefusal::InvalidGraph,
            "Scope data aliases executable graph");
  }
  std::unordered_set<CellRef> oldTargets;
  std::unordered_set<CellRef> registers;
  registers.insert(frameRegister);
  const auto frameTarget = codec.reference(frameRegister);
  oldTargets.insert(frameTarget);
  for (const auto &b : f.descriptor.bindings) {
    codec.charge();
    require(registers.insert(b.reg).second && !protectedCells.contains(b.reg) &&
                !f.graph.contains(b.reg) && b.reg != args &&
                b.reg != descriptor && b.reg != f.destination,
            ScopedRefusal::InvalidGraph,
            "Binding aliases protected scope data");
    const auto old = codec.reference(b.reg);
    require(oldTargets.insert(old).second && !protectedCells.contains(old) &&
                !f.graph.contains(old) && old != args && old != descriptor &&
                old != f.destination && old != frameRegister,
            ScopedRefusal::InvalidGraph,
            "Binding target aliases protected data");
    const bool arg    = b.target.starts_with("arg.");
    const bool result = b.target.starts_with("result.");
    if (arg || result) {
      const auto &schema = arg ? f.descriptor.args : f.descriptor.result;
      const auto name    = b.target.substr(arg ? 4 : 7);
      const auto &fields = schema.root().fields;
      auto field         = std::ranges::find_if(fields, [&](std::size_t index) {
        codec.charge();
        return schema.nodes[index].name == name;
      });
      require(field != fields.end() && schema.nodes[*field].type == b.type,
              ScopedRefusal::InvalidSchema,
              "Binding type does not match schema");
    } else {
      require(b.target.starts_with("temp.") || b.target == "outcome" ||
                  b.target == "error",
              ScopedRefusal::InvalidSchema, "Unknown frame binding target");
      if (b.target == "outcome" || b.target == "error")
        require(b.type == ScopeType::Str, ScopedRefusal::InvalidSchema,
                "Outcome/error bindings must be Str");
    }
    f.readable.insert(b.reg);
    if (!arg) f.writable.insert(b.reg);
  }
  for (auto target : oldTargets)
    require(!registers.contains(target), ScopedRefusal::InvalidGraph,
            "Private binding targets alias binding registers");
  require(
      !protectedCells.contains(frameTarget) && !f.graph.contains(frameTarget) &&
          !protectedCells.contains(frameRegister) &&
          !f.graph.contains(frameRegister) && !registers.contains(frameTarget),
      ScopedRefusal::InvalidGraph, "Frame handle aliases protected data");
  // Verify every listed instruction, including unreachable malicious nodes.
  bool completionEdge = false;
  std::unordered_set<CellRef> completionTargets{f.completion};
  for (auto op : f.descriptor.graph) {
    codec.charge();
    if (getOpcodeKind(op) == OpcodeKind::ScopedEvaluate) {
      auto refs = codec.wing(op, DimVector::POS);
      require(refs.size() == 5, ScopedRefusal::InvalidGraph,
              "Malformed nested scope operands");
      completionTargets.insert(refs[1]);
    }
  }
  for (auto op : f.descriptor.graph) {
    codec.charge();
    const auto kind = getOpcodeKind(op);
    require(kind && pure(*kind) && !isMemoized(op) &&
                !a.linked(op, core_.dims().cache, DimVector::POS) &&
                !a.linked(op, core_.dims().contract, DimVector::POS) &&
                !a.linked(op, core_.dims().contract, DimVector::NEG),
            ScopedRefusal::InvalidGraph, "Impure scoped opcode");
    if (completionTargets.contains(op)) {
      require(codec.wing(op, DimVector::POS).empty() &&
                  codec.wing(op, DimVector::NEG).empty(),
              ScopedRefusal::InvalidGraph, "Completion has operands");
      continue;
    }
    const auto in      = codec.wing(op, DimVector::POS);
    const auto out     = codec.wing(op, DimVector::NEG);
    const auto control = controlIndex(*kind);
    if (control) {
      require(*control < in.size() && f.graph.contains(in[*control]),
              ScopedRefusal::InvalidGraph, "Unresolved scoped control target");
      if (in[*control] == f.completion) {
        require(*kind == OpcodeKind::Jump, ScopedRefusal::InvalidGraph,
                "Completion requires explicit root Jump");
        completionEdge = true;
      }
    }
    if (*kind == OpcodeKind::ScopedEvaluate) {
      require(in.size() == 5 && out.size() == 1, ScopedRefusal::InvalidGraph,
              "Nested scope operand arity");
      // Runtime validates nested manifests independently, with the outer write
      // set imposing an additional publication boundary.
      continue;
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
      codec.charge();
      if (control && i == *control) continue;
      require(f.readable.contains(in[i]) && !core_.hasPipeline(in[i]),
              ScopedRefusal::InvalidGraph, "Unregistered scoped input");
    }
    for (auto target : out) {
      codec.charge();
      require(f.writable.contains(target) && !core_.hasPipeline(target),
              ScopedRefusal::InvalidGraph, "Forbidden scoped output");
    }
    if (*kind != OpcodeKind::Jump && *kind != OpcodeKind::Return &&
        *kind != OpcodeKind::Fail && *kind != OpcodeKind::Halt) {
      auto next = a.linked(op, core_.dims().spin, DimVector::POS);
      require(next && f.graph.contains(*next) && *next != f.completion,
              ScopedRefusal::InvalidGraph, "Scoped fallthrough escapes graph");
    }
  }
  require(completionEdge, ScopedRefusal::InvalidGraph,
          "Scope has no explicit completion edge");
}
ExecutionResult VortexVM::enterScope(CellRef cursor, CellRef opcode) {
  auto &a     = core_.arena();
  bool marked = false;
  if (!scopeLimits_)
    return refusalResult(ScopedRefusal::ResourceLimit,
                         "ScopedEvaluate requires a validated host grant");
  auto limits = *scopeLimits_;
  if (const auto work = a.remainingExecutionWork()) {
    limits.traversalWork = std::min(limits.traversalWork, *work);
    limits.instructions  = std::min(limits.instructions, *work);
    limits.fields        = std::min(limits.fields, *work);
  }
  if (const auto bytes = a.remainingScratchBytes())
    limits.bytes = std::min(limits.bytes, *bytes);
  limits.fields = std::min(limits.fields, a.remainingCellAllowance());
  std::shared_ptr<ScopedEvaluationBudget> budget;
  if (!scopedFrames_.empty()) {
    const auto &parent = scopedFrames_.back();
    limits.traversalWork =
        std::min(limits.traversalWork, parent.budget->workRemaining);
    limits.instructions = std::min(limits.instructions,
                                   parent.limits.instructions - parent.steps);
    limits.fields       = std::min(limits.fields, parent.limits.fields);
    limits.bytes        = std::min(limits.bytes, parent.budget->bytesRemaining);
    limits.depth        = std::min(limits.depth, parent.limits.depth);
    limits.scopes       = std::min(limits.scopes, parent.limits.scopes);
    budget              = parent.budget;
  }
  if (!limits.valid())
    return refusalResult(ScopedRefusal::ResourceLimit,
                         "Scope actor grant exhausted");
  try {
    if (!budget) budget = std::make_shared<ScopedEvaluationBudget>(limits);
    ScopeCodec codec(core_, limits, std::nullopt, budget);
    auto in  = codec.wing(opcode, DimVector::POS);
    auto out = codec.wing(opcode, DimVector::NEG);
    require(in.size() == 5 && out.size() == 1 &&
                scopedFrames_.size() < limits.scopes,
            ScopedRefusal::InvalidOperands,
            "Scope needs five inputs and one output");
    require(!isMemoized(opcode) &&
                !a.linked(opcode, core_.dims().contract, DimVector::POS) &&
                !a.linked(opcode, core_.dims().contract, DimVector::NEG),
            ScopedRefusal::InvalidGraph, "Scope invocation has effects");
    std::unordered_set<CellRef> roots{in[0], in[1], in[2],
                                      in[3], in[4], out[0]};
    require(roots.size() == in.size() + out.size() && !roots.contains(opcode) &&
                !roots.contains(cursor),
            ScopedRefusal::InvalidOperands, "Scoped invocation roots alias");
    for (auto data : {in[2], in[3], in[4], out[0]})
      require(!core_.hasPipeline(data), ScopedRefusal::InvalidGraph,
              "Scoped data operand has pipeline");
    require(a.valueKindOf(out[0]) == xanadu::ValueKind::None,
            ScopedRefusal::TypeMismatch, "Destination must be a record");
    ScopedFrame frame;
    frame.limits       = limits;
    frame.budget       = budget;
    frame.cursor       = cursor;
    frame.completion   = in[1];
    frame.destination  = out[0];
    frame.successor    = a.linked(opcode, core_.dims().spin, DimVector::POS);
    frame.stack        = a.linked(cursor, core_.dims().stack, DimVector::POS);
    frame.choiceFloor  = choiceStack_.size();
    frame.dewey        = currentDeweyPath_;
    frame.oracle       = oraclePath_;
    frame.oracleCursor = oracleCursor_;
    frame.descriptor   = codec.descriptor(in[3]);
    // Every caller-owned argument root and every PinnedRef needs a live pin.
    std::unordered_set<CellRef> pinned;
    std::unordered_set<CellRef> visited;
    auto pin =
        a.linked(core_.home(), core_.dims().pinningCursors, DimVector::POS);
    while (pin) {
      codec.charge();
      require(visited.insert(*pin).second, ScopedRefusal::InvalidOperands,
              "Cyclic pin directory");
      if (auto target = a.linked(*pin, core_.dims().cache, DimVector::POS))
        pinned.insert(*target);
      pin = a.linked(*pin, core_.dims().pinningCursors, DimVector::POS);
    }
    if (!scopedFrames_.empty()) {
      const auto &parent = scopedFrames_.back();
      for (const auto &[reg, target] : parent.bindingTargets) {
        codec.charge();
        (void)reg;
        pinned.insert(target);
      }
      for (const auto &[name, target] : parent.pins) {
        codec.charge();
        (void)name;
        pinned.insert(target);
      }
    }
    require(pinned.contains(in[2]), ScopedRefusal::InvalidOperands,
            "Scope arguments are not caller-pinned");
    const auto argsFields = codec.fields(in[2]);
    for (auto fieldIndex : frame.descriptor.args.root().fields) {
      codec.charge();
      const auto &field = frame.descriptor.args.nodes[fieldIndex];
      if (field.type != ScopeType::PinnedRef) continue;
      require(argsFields.contains(field.name), ScopedRefusal::TypeMismatch,
              "Missing pinned argument");
      auto target = codec.reference(argsFields.at(field.name));
      require(pinned.contains(target), ScopedRefusal::InvalidOperands,
              "Scope reference is not caller-pinned");
      frame.pins.emplace(field.name, target);
    }
    auto args        = codec.snapshot(in[2], frame.descriptor.args, frame.pins);
    auto destination = codec.fields(out[0]);
    require(destination.contains("outcome") && destination.size() <= 3,
            ScopedRefusal::TypeMismatch, "Invalid scope destination");
    for (const auto &[name, value] : destination) {
      (void)value;
      require(name == "outcome" || name == "payload" || name == "error",
              ScopedRefusal::TypeMismatch, "Unknown destination field");
    }
    ScopeSchema textSchema;
    textSchema.root().type  = ScopeType::Str;
    textSchema.root().limit = limits.bytes;
    (void)codec.snapshot(destination.at("outcome"), textSchema);
    if (destination.contains("error"))
      (void)codec.snapshot(destination.at("error"), textSchema);
    if (destination.contains("payload"))
      (void)codec.snapshot(destination.at("payload"), frame.descriptor.result,
                           frame.pins);
    ScopeCodec argsProtection(core_, limits, std::nullopt, budget);
    (void)argsProtection.snapshot(in[2], frame.descriptor.args, frame.pins);
    ScopeCodec destProtection(core_, limits, std::nullopt, budget);
    (void)destProtection.fields(out[0]);
    (void)destProtection.snapshot(destination.at("outcome"), textSchema);
    if (destination.contains("error"))
      (void)destProtection.snapshot(destination.at("error"), textSchema);
    if (destination.contains("payload"))
      (void)destProtection.snapshot(destination.at("payload"),
                                    frame.descriptor.result, frame.pins);
    for (const auto &active : scopedFrames_)
      for (const auto &assignment : active.assignments) {
        codec.charge();
        require(assignment.assigned ||
                    !argsProtection.protectedCells().contains(assignment.cell),
                ScopedRefusal::TypeMismatch,
                "Nested arguments read unassigned scoped data");
      }
    const auto relocationDimension =
        a.dimensionNamed(xanadu::cellRelocationDimension);
    for (auto cell : argsProtection.protectedCells()) {
      codec.charge();
      // Distinct argument/result handles legitimately share their canonical
      // relocation dimension; their mutable handle/marker cells stay disjoint.
      if (relocationDimension == cell) continue;
      require(!destProtection.protectedCells().contains(cell),
              ScopedRefusal::InvalidOperands,
              "Scoped destination aliases argument graph");
    }
    require(!argsProtection.protectedCells().contains(in[3]) &&
                !destProtection.protectedCells().contains(in[3]),
            ScopedRefusal::InvalidOperands,
            "Scoped schema aliases value graph");
    if (!scopedFrames_.empty()) {
      const auto &parent = scopedFrames_.back();
      for (auto op : frame.descriptor.graph) {
        codec.charge();
        require(parent.graph.contains(op), ScopedRefusal::InvalidGraph,
                "Nested scope graph escapes outer closed manifest");
      }
      bool owned = false;
      for (auto reg : parent.writable) {
        codec.charge();
        if (parent.bindingTargets.at(reg) == out[0] &&
            a.handleTarget(reg) == out[0])
          owned = true;
      }
      require(owned, ScopedRefusal::InvalidGraph,
              "Nested scope destination escapes parent frame");
    }
    verifyScope(frame, codec, in[0], in[4], in[2], in[3]);
    // All native allocations that could fail precede the arena mark where
    // possible. A failed entry owns exactly one well-nested mark.
    scopedFrames_.push_back(std::move(frame));
    auto &f            = scopedFrames_.back();
    f.mark             = a.mark();
    marked             = true;
    auto ownedArgs     = codec.materialize(args, f.descriptor.args);
    auto defaultResult = codec.defaults(f.descriptor.result, f.pins);
    auto result        = codec.materialize(defaultResult, f.descriptor.result);
    std::vector<CellRef> resultCells;
    const auto resultTree =
        codec.snapshot(result, f.descriptor.result, f.pins, &resultCells);
    std::vector<std::optional<std::size_t>> parents(resultTree.nodes.size());
    for (std::size_t i = 0; i < resultTree.nodes.size(); ++i)
      for (auto child : resultTree.nodes[i].children) {
        codec.charge();
        parents.at(child) = i;
      }
    for (std::size_t i = 0; i < resultTree.nodes.size(); ++i) {
      codec.charge();
      const auto &value  = resultTree.nodes[i];
      const auto &schema = f.descriptor.result.nodes.at(value.schema);
      const bool leaf =
          schema.type != ScopeType::Record || value.children.empty();
      const bool initialized = !leaf || schema.constant;
      f.assignmentIndex.emplace(resultCells.at(i), f.assignments.size());
      f.assignments.push_back(
          {resultCells.at(i), parents[i], initialized, leaf});
    }
    auto outcome = a.makeCell("Value");
    auto error   = a.makeCell("()");
    f.frame      = codec.record(
        "ScopedEvaluationFrame",
        {{"args", ownedArgs}, {"result", result}, {"outcome", outcome}});
    auto ownedArgFields = codec.fields(ownedArgs);
    auto resultFields   = codec.fields(result);
    for (const auto &binding : f.descriptor.bindings) {
      CellRef value{};
      if (binding.target.starts_with("arg."))
        value = ownedArgFields.at(binding.target.substr(4));
      else if (binding.target.starts_with("result."))
        value = resultFields.at(binding.target.substr(7));
      else if (binding.target == "outcome")
        value = outcome;
      else if (binding.target == "error")
        value = error;
      else
        value = a.makeCell("()");
      if (binding.target.starts_with("temp.") || binding.target == "error") {
        codec.charge();
        f.assignmentIndex.emplace(value, f.assignments.size());
        f.assignments.push_back({value, std::nullopt, false, false});
      }
      f.bindingTargets.emplace(binding.reg, value);
      f.bindingTypes.emplace(binding.reg, binding.type);
      require(
          bool(a.setValueBits(binding.reg, xanadu::ValueKind::OpHandle, value)),
          ScopedRefusal::InvalidOperands, "Cannot bind scope register");
    }
    require(bool(a.setValueBits(in[4], xanadu::ValueKind::OpHandle, f.frame)),
            ScopedRefusal::InvalidOperands, "Cannot bind evaluation frame");
    oraclePath_.clear();
    oracleCursor_ = 0;
    setCursorOpcode(cursor, in[0]);
    return {};
  } catch (const ScopeError &error) {
    if (marked) return finishScope(ScopedOutcome::Refused, error.code);
    return refusalResult(error.code, error.what());
  } catch (const std::exception &error) {
    if (marked)
      return finishScope(ScopedOutcome::Refused, ScopedRefusal::ResourceLimit);
    return refusalResult(ScopedRefusal::ResourceLimit, error.what());
  }
}
ExecutionResult VortexVM::stepScope(CellRef cursor) {
  try {
    auto &f = scopedFrames_.back();
    ScopeCodec codec(core_, f.limits, std::nullopt, f.budget);
    codec.charge();
    ++totalCycles_;
    for (const auto &active : scopedFrames_) {
      codec.charge();
      require(active.steps < active.limits.instructions,
              ScopedRefusal::ResourceLimit, "Scope instruction limit");
    }
    for (auto &active : scopedFrames_) ++active.steps;
    const auto op = getCursorOpcode(cursor);
    require(op && f.graph.contains(*op), ScopedRefusal::InvalidScopedCompletion,
            "Scoped PC escaped verified graph");
    const auto stack =
        core_.readLink(cursor, core_.dims().stack, DimVector::POS);
    if (*op == f.completion) {
      require(f.completionArmed && stack == f.stack,
              ScopedRefusal::InvalidScopedCompletion,
              "Scoped completion is not at callback root");
      return finishScope(ScopedOutcome::Value);
    }
    const auto kind = getOpcodeKind(*op);
    require(kind != OpcodeKind::Halt, ScopedRefusal::InvalidScopedCompletion,
            "Scoped callback reached Halt");
    require(kind && pure(*kind), ScopedRefusal::InvalidGraph,
            "Scoped opcode changed after verification");
    if (*kind == OpcodeKind::ScopedEvaluate) {
      auto result = enterScope(cursor, *op);
      if (!result.success)
        return refuseScopes(
            result.scopedRefusal.value_or(ScopedRefusal::CallbackFault));
      return result;
    }
    if (*kind == OpcodeKind::Return) {
      require(stack && stack != f.stack, ScopedRefusal::InvalidScopedCompletion,
              "Unmatched scoped Return");
      auto target = core_.arena().asInt64(*stack);
      require(target && *target > 0 &&
                  static_cast<std::uint64_t>(*target) <=
                      std::numeric_limits<CellRef>::max() &&
                  f.graph.contains(static_cast<CellRef>(*target)),
              ScopedRefusal::InvalidScopedCompletion,
              "Malformed scoped call frame");
    }
    const auto in      = codec.wing(*op, DimVector::POS);
    const auto out     = codec.wing(*op, DimVector::NEG);
    const auto control = controlIndex(*kind);
    std::vector<CellValue> inputs;
    for (std::size_t i = 0; i < in.size(); ++i) {
      codec.charge();
      if (control && i == *control) {
        require(f.graph.contains(in[i]), ScopedRefusal::InvalidGraph,
                "Scoped control target changed");
        if (in[i] == f.completion)
          require(*kind == OpcodeKind::Jump && stack == f.stack,
                  ScopedRefusal::InvalidScopedCompletion,
                  "Completion reached outside callback root");
        inputs.emplace_back(static_cast<std::int64_t>(in[i]));
      } else {
        require(f.readable.contains(in[i]), ScopedRefusal::InvalidGraph,
                "Scoped read escaped frame");
        auto target = codec.reference(in[i]);
        require(f.bindingTargets.at(in[i]) == target,
                ScopedRefusal::InvalidGraph,
                "Scoped input binding was redirected");
        if (const auto tracked = f.assignmentIndex.find(target);
            tracked != f.assignmentIndex.end())
          require(f.assignments[tracked->second].assigned,
                  ScopedRefusal::TypeMismatch,
                  "Scoped read precedes assignment");
        const auto type      = f.bindingTypes.at(in[i]);
        const auto valueKind = core_.arena().valueKindOf(target);
        require((type == ScopeType::Float &&
                 valueKind == xanadu::ValueKind::Double) ||
                    (type == ScopeType::Int &&
                     valueKind == xanadu::ValueKind::Int64) ||
                    (type == ScopeType::Bool &&
                     valueKind == xanadu::ValueKind::Bool) ||
                    ((type == ScopeType::Str || type == ScopeType::Unit) &&
                     valueKind == xanadu::ValueKind::None),
                ScopedRefusal::TypeMismatch,
                "Uninitialized or nonscalar scoped input");
        if (valueKind == xanadu::ValueKind::None) {
          auto text = codec.boundedText(target);
          for (std::size_t n = 0; n < text.size(); ++n) codec.charge();
          require(type != ScopeType::Unit || text == "()",
                  ScopedRefusal::TypeMismatch, "Invalid scoped Unit input");
          inputs.emplace_back(std::move(text));
        } else {
          auto value = core_.render(target);
          require(value.has_value(), ScopedRefusal::TypeMismatch,
                  "Unreadable scoped scalar");
          inputs.push_back(*value);
        }
      }
    }
    if (control)
      require(*control < inputs.size(), ScopedRefusal::InvalidGraph,
              "Malformed scoped control instruction");
    std::vector<CellRef> destinations;
    for (auto reg : out) {
      require(f.writable.contains(reg), ScopedRefusal::InvalidGraph,
              "Scoped write escaped frame");
      const auto target = codec.reference(reg);
      require(f.bindingTargets.at(reg) == target, ScopedRefusal::InvalidGraph,
              "Scoped output binding was redirected");
      require(core_.arena().cloneMaster(target, core_.dims().clone) == target,
              ScopedRefusal::InvalidGraph, "Scoped output clone escapes frame");
      destinations.push_back(target);
    }
    if (*kind == OpcodeKind::Fail && choiceStack_.size() == f.choiceFloor)
      return finishScope(ScopedOutcome::Empty);
    if (*kind == OpcodeKind::Cut) {
      std::size_t floor = f.choiceFloor;
      if (!inputs.empty()) {
        const auto *value = std::get_if<std::int64_t>(&inputs[0]);
        require(value && *value >= 0 &&
                    static_cast<std::uint64_t>(*value) >= floor &&
                    static_cast<std::uint64_t>(*value) <= choiceStack_.size(),
                ScopedRefusal::CutBelowFloor,
                "Scoped Cut crosses choice floor");
        floor = static_cast<std::size_t>(*value);
      }
      scopedCut_ = true;
      cut(floor);
      scopedCut_ = false;
      inputs.clear();
      // Body's default Cut(0) would violate the floor, so execute it here.
      for (std::size_t i = 0; i < destinations.size(); ++i) {
        require(f.bindingTypes.at(out[i]) == ScopeType::Bool,
                ScopedRefusal::TypeMismatch, "Scoped Cut output must be Bool");
        require(core_.writeValue(destinations[i], 0, std::nullopt, true)
                    .has_value(),
                ScopedRefusal::CallbackFault, "Scoped Cut output failed");
        assignScopedValue(f, destinations[i], codec);
      }
      setCursorOpcode(cursor,
                      core_.readLink(*op, core_.dims().spin, DimVector::POS));
      return {};
    }
    if (*kind == OpcodeKind::Call)
      require(inputs.size() == 1 &&
                  core_.readLink(*op, core_.dims().spin, DimVector::POS)
                      .has_value(),
              ScopedRefusal::InvalidScopedCompletion, "Malformed scoped Call");
    bool stringOutputCharged = false;
    // Preflight native string output before executeOpcodeBody allocates it.
    // Trim's exact length is unknown until it runs, so admit its input bound.
    if (*kind == OpcodeKind::Add && !inputs.empty() &&
        std::holds_alternative<std::string>(inputs.front()) &&
        (inputs.size() == 1 ||
         std::holds_alternative<std::string>(inputs[1]))) {
      codec.debitBytes(std::get<std::string>(inputs.front()).size());
      if (inputs.size() > 1)
        codec.debitBytes(std::get<std::string>(inputs[1]).size());
      stringOutputCharged = true;
    }
    if (*kind == OpcodeKind::Trim || *kind == OpcodeKind::ToLower ||
        *kind == OpcodeKind::ToUpper) {
      require(inputs.size() == 1 &&
                  std::holds_alternative<std::string>(inputs.front()),
              ScopedRefusal::TypeMismatch, "Scoped string opcode requires Str");
      codec.debitBytes(std::get<std::string>(inputs.front()).size());
      stringOutputCharged = true;
    }
    std::vector<CellValue> outputs;
    bool jumped = false;
    auto result =
        executeOpcodeBody(*kind, *op, inputs, outputs, cursor, jumped);
    if (!result.success)
      return finishScope(ScopedOutcome::Refused, ScopedRefusal::CallbackFault);
    require(outputs.size() >= destinations.size(), ScopedRefusal::TypeMismatch,
            "Scoped opcode did not produce declared outputs");
    for (const auto &output : outputs) {
      codec.charge();
      if (const auto *number = std::get_if<double>(&output))
        require(std::isfinite(*number), ScopedRefusal::CallbackFault,
                "Scoped numeric output is nonfinite");
    }
    for (std::size_t i = 0; i < destinations.size(); ++i) {
      codec.charge();
      const auto type = f.bindingTypes.at(out[i]);
      require((type == ScopeType::Float &&
               std::holds_alternative<double>(outputs[i])) ||
                  (type == ScopeType::Int &&
                   std::holds_alternative<std::int64_t>(outputs[i])) ||
                  (type == ScopeType::Bool &&
                   std::holds_alternative<bool>(outputs[i])) ||
                  (type == ScopeType::Str &&
                   std::holds_alternative<std::string>(outputs[i])) ||
                  (type == ScopeType::Unit &&
                   std::holds_alternative<std::string>(outputs[i]) &&
                   std::get<std::string>(outputs[i]) == "()"),
              ScopedRefusal::TypeMismatch,
              "Scoped output does not match binding type");
      if (const auto *text = std::get_if<std::string>(&outputs[i])) {
        if (!stringOutputCharged) codec.debitBytes(text->size());
        for (std::size_t n = 0; n < text->size(); ++n) codec.charge();
      }
      require(core_.writeValue(destinations[i], 0, std::nullopt, outputs[i])
                  .has_value(),
              ScopedRefusal::CallbackFault, "Scoped output write failed");
      assignScopedValue(f, destinations[i], codec);
    }
    if (*kind == OpcodeKind::Jump && control && in[*control] == f.completion)
      f.completionArmed = true;
    if (!jumped)
      setCursorOpcode(cursor,
                      core_.readLink(*op, core_.dims().spin, DimVector::POS));
    return {};
  } catch (const ScopeError &error) {
    scopedCut_ = false;
    return finishScope(ScopedOutcome::Refused, error.code);
  } catch (const std::exception &) {
    scopedCut_ = false;
    return finishScope(ScopedOutcome::Refused, ScopedRefusal::ResourceLimit);
  }
}
ExecutionResult VortexVM::finishScope(ScopedOutcome outcome,
                                      std::optional<ScopedRefusal> refusal) {
  auto &a    = core_.arena();
  auto &live = scopedFrames_.back();
  std::optional<ScopeValue> payload;
  if (outcome == ScopedOutcome::Value) {
    try {
      ScopeCodec codec(core_, live.limits, live.mark.scratchSize, live.budget);
      const auto fields = codec.fields(live.frame);
      const auto status = codec.boundedText(fields.at("outcome"));
      if (status == "Empty")
        outcome = ScopedOutcome::Empty;
      else if (status == "Refused") {
        outcome = ScopedOutcome::Refused;
        // An absent error remains absent; an explicit refusal must name a
        // versioned whitelist code through its declared private error binding.
        auto binding =
            std::ranges::find(live.descriptor.bindings, std::string("error"),
                              &ScopeBinding::target);
        require(binding != live.descriptor.bindings.end(),
                ScopedRefusal::TypeMismatch,
                "Refused outcome lacks error binding");
        const auto errorCell = live.bindingTargets.at(binding->reg);
        const auto tracked   = live.assignmentIndex.find(errorCell);
        require(tracked != live.assignmentIndex.end() &&
                    live.assignments[tracked->second].assigned,
                ScopedRefusal::TypeMismatch, "Refused error was not assigned");
        const auto code = codec.boundedText(errorCell);
        for (auto candidate :
             {ScopedRefusal::InvalidOperands, ScopedRefusal::InvalidSchema,
              ScopedRefusal::InvalidGraph,
              ScopedRefusal::InvalidScopedCompletion,
              ScopedRefusal::CutBelowFloor, ScopedRefusal::ResourceLimit,
              ScopedRefusal::TypeMismatch, ScopedRefusal::PublicationFailed,
              ScopedRefusal::CallbackFault, ScopedRefusal::Cancelled,
              ScopedRefusal::CursorOwnership}) {
          codec.charge();
          if (code == scopedRefusalName(candidate)) refusal = candidate;
        }
        require(refusal.has_value(), ScopedRefusal::TypeMismatch,
                "Unknown scoped refusal code");
      } else {
        require(status == "Value", ScopedRefusal::TypeMismatch,
                "Unknown scoped outcome");
        for (const auto &assignment : live.assignments) {
          codec.charge();
          require(!assignment.required || assignment.assigned,
                  ScopedRefusal::TypeMismatch,
                  "Value completion has unassigned result data");
        }
        payload = codec.snapshot(fields.at("result"), live.descriptor.result,
                                 live.pins);
      }
    } catch (const ScopeError &error) {
      outcome = ScopedOutcome::Refused;
      refusal = error.code;
    } catch (const std::exception &) {
      outcome = ScopedOutcome::Refused;
      refusal = ScopedRefusal::ResourceLimit;
    }
  }
  // Unwinding itself has no graph walk, allocation, callback, or fuel charge.
  while (choiceStack_.size() > live.choiceFloor) {
    a.discard(choiceStack_.back().mark);
    choiceStack_.pop_back();
  }
  auto saved = std::move(live);
  a.release(saved.mark);
  scopedFrames_.pop_back();
  currentDeweyPath_ = std::move(saved.dewey);
  oraclePath_       = std::move(saved.oracle);
  oracleCursor_     = saved.oracleCursor;
  setCursorOpcode(saved.cursor, saved.successor);
  const auto parentAssignmentTail =
      scopedFrames_.empty()
          ? std::nullopt
          : std::optional{scopedFrames_.back().assignmentTrail.size()};
  auto publication = a.mark();
  try {
    ScopeCodec codec(core_, saved.limits, std::nullopt, saved.budget);
    const auto status = outcome == ScopedOutcome::Value   ? "Value"
                        : outcome == ScopedOutcome::Empty ? "Empty"
                                                          : "Refused";
    std::vector<std::pair<std::string, CellRef>> fields;
    codec.charge();
    fields.emplace_back("outcome", a.makeCell(status));
    if (payload)
      fields.emplace_back("payload",
                          codec.materialize(*payload, saved.descriptor.result));
    if (outcome == ScopedOutcome::Refused) {
      require(refusal.has_value(), ScopedRefusal::CallbackFault,
              "Refused outcome has no typed error");
      codec.charge();
      // Serialized ABI error is a stable symbolic code; it is not a CellRef.
      fields.emplace_back("error", a.makeCell(scopedRefusalName(*refusal)));
    }
    codec.replaceRecord(saved.destination, fields);
    if (outcome != ScopedOutcome::Refused && !scopedFrames_.empty())
      assignScopedValue(scopedFrames_.back(), saved.destination, codec);
    a.discard(publication);
  } catch (const ScopeError &error) {
    a.release(publication);
    if (parentAssignmentTail) restoreScopedAssignments(*parentAssignmentTail);
    return refusalResult(error.code,
                         "Scoped result publication refused atomically");
  } catch (const std::exception &) {
    a.release(publication);
    if (parentAssignmentTail) restoreScopedAssignments(*parentAssignmentTail);
    const auto exhausted = a.remainingExecutionWork() == 0 ||
                           a.remainingScratchBytes() == 0 ||
                           a.remainingCellAllowance() == 0;
    return refusalResult(exhausted ? ScopedRefusal::ResourceLimit
                                   : ScopedRefusal::PublicationFailed,
                         "Scoped result publication failed atomically");
  }
  if (outcome == ScopedOutcome::Refused)
    return refusalResult(*refusal, "Scoped callback refused");
  return {};
}
void VortexVM::assignScopedValue(ScopedFrame &frame, CellRef cell,
                                 ScopeCodec &codec) {
  const auto target = frame.assignmentIndex.find(cell);
  if (target == frame.assignmentIndex.end()) return;
  for (std::size_t i = 0; i < frame.assignments.size(); ++i) {
    codec.charge();
    if (frame.assignments[i].assigned) continue;
    auto ancestor = std::optional{i};
    while (ancestor && *ancestor != target->second) {
      codec.charge();
      ancestor = frame.assignments[*ancestor].parent;
    }
    if (!ancestor) continue;
    frame.assignmentTrail.push_back(i);
    frame.assignments[i].assigned = true;
  }
}
void VortexVM::restoreScopedAssignments(std::size_t tail) noexcept {
  auto &frame = scopedFrames_.back();
  while (frame.assignmentTrail.size() > tail) {
    frame.assignments[frame.assignmentTrail.back()].assigned = false;
    frame.assignmentTrail.pop_back();
  }
}
ExecutionResult VortexVM::refuseScopes(ScopedRefusal code) {
  ExecutionResult result;
  while (!scopedFrames_.empty())
    result = finishScope(ScopedOutcome::Refused, code);
  return result;
}
} // namespace zigzag::vortex
