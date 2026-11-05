/**
 * @file compile_contract.cpp
 * @brief Compiler backend for the shared VQL contract continuation driver.
 */
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contract_driver.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using CaptureBindings = std::unordered_map<std::string, std::vector<CellRef>>;

struct RestoreFlag {
  bool &target;
  bool saved;
  ~RestoreFlag() { target = saved; }
};

bool hasComputedDefault(const TypeExpr &type) {
  const auto valueIsComputed = [](const ValueExpr &value) {
    return !std::holds_alternative<ScalarLiteral>(value.kind) &&
           !std::holds_alternative<std::string>(value.kind);
  };
  const auto patternHasComputed = [&](const DestructurePattern &pattern) {
    return std::ranges::any_of(pattern.fields, [&](const auto &field) {
      return (field.defaultValue && valueIsComputed(*field.defaultValue)) ||
             (field.type && hasComputedDefault(*field.type));
    });
  };
  for (const auto &member : type.unionType.members) {
    if (member.refinement && patternHasComputed(member.refinement->pattern))
      return true;
    if (const auto *shape = std::get_if<ShapeType>(&member.kind)) {
      if (patternHasComputed(shape->pattern)) return true;
    } else if (const auto *generic = std::get_if<GenericType>(&member.kind)) {
      for (const auto &argument : generic->args)
        if (argument && hasComputedDefault(*argument)) return true;
    } else if (const auto *nested =
                   std::get_if<std::shared_ptr<TypeExpr>>(&member.kind)) {
      if (*nested && hasComputedDefault(**nested)) return true;
    }
  }
  return false;
}

void collectDirectDefaultAliases(const TypeExpr &type,
                                 std::vector<std::string> &names) {
  const auto visitPattern = [&](const DestructurePattern &pattern) {
    for (const auto &field : pattern.fields) {
      if (field.defaultValue) {
        if (const auto *name =
                std::get_if<std::string>(&field.defaultValue->kind))
          names.push_back(*name);
      }
      if (field.type) collectDirectDefaultAliases(*field.type, names);
    }
  };
  for (const auto &member : type.unionType.members) {
    if (member.refinement) visitPattern(member.refinement->pattern);
    if (const auto *shape = std::get_if<ShapeType>(&member.kind))
      visitPattern(shape->pattern);
    else if (const auto *generic = std::get_if<GenericType>(&member.kind)) {
      for (const auto &argument : generic->args)
        if (argument) collectDirectDefaultAliases(*argument, names);
    } else if (const auto *nested =
                   std::get_if<std::shared_ptr<TypeExpr>>(&member.kind)) {
      if (*nested) collectDirectDefaultAliases(**nested, names);
    }
  }
}

} // namespace

class VQLCompiler::ContractGraphBackend {
public:
  using Bindings     = CaptureBindings;
  using Continuation = gleditor::cpp26::function_ref<void()>;
  using Context      = std::optional<CellRef>;
  using Dimension    = CellRef; // A dimension name, resolved by the VM leaf.
  using Subject      = CellRef;
  using Subjects     = CellRef; // Runtime stream register.
  using MissingField = ContractMatcher::MissingField;

  explicit ContractGraphBackend(VQLCompiler &compiler) : compiler_(compiler) {}

  template <class Body>
  void scope(Bindings &bindings, const detail::ScopePolicy policy, Body &&body,
             Continuation success, Continuation failure) {
    const CellRef depth = compiler_.core_.arena().makeCell();
    const CellRef readDepth =
        compiler_.emitOp(OpcodeKind::ChoiceDepth, "#CONTRACT_CHOICE_DEPTH");
    compiler_.emitOutput(readDepth, depth);
    const CellRef failed  = compiler_.mintContractLabel("#CONTRACT_SCOPE_FAIL");
    const CellRef matched = compiler_.mintContractLabel("#CONTRACT_SCOPE_OK");
    const CellRef restored =
        compiler_.mintContractLabel("#CONTRACT_SCOPE_RESTORED");
    const CellRef scopeChoice =
        compiler_.emitOp(OpcodeKind::ScopedChoice, "#CONTRACT_SCOPE");
    compiler_.emitInput(scopeChoice, failed);

    if (policy == detail::ScopePolicy::CommitOnSuccess) {
      const CellRef keepScope = depthOffset(depth, 1);
      const auto onMatched    = [&] { goTo(matched); };
      const auto onRestored   = [&] { goTo(restored); };
      body(bindings, onMatched, onRestored);
      compiler_.appendOpcode(matched);
      const CellRef cut = compiler_.emitOp(OpcodeKind::Cut, "#CONTRACT_COMMIT");
      compiler_.emitInput(cut, depth);
      resume(success);
      compiler_.appendOpcode(restored);
      const CellRef cleanup =
          compiler_.emitOp(OpcodeKind::Cut, "#CONTRACT_FAILURE_CLEAN");
      compiler_.emitInput(cleanup, keepScope);
      const CellRef fail = compiler_.emitOp(OpcodeKind::Fail, "#CONTRACT_FAIL");
      (void)fail;
      compiler_.appendOpcode(failed);
      resume(failure);
      return;
    }

    const CellRef successRollback =
        compiler_.mintContractLabel("#CONTRACT_ROLLBACK_SUCCESS");
    const CellRef innerChoice =
        compiler_.emitOp(OpcodeKind::ScopedChoice, "#CONTRACT_ROLLBACK_SCOPE");
    compiler_.emitInput(innerChoice, successRollback);
    const CellRef keepBoth  = depthOffset(depth, 2);
    const CellRef keepOuter = depthOffset(depth, 1);
    const auto onMatched    = [&] { goTo(matched); };
    const auto onRestored   = [&] { goTo(restored); };
    body(bindings, onMatched, onRestored);
    compiler_.appendOpcode(matched);
    const CellRef cleanChoices =
        compiler_.emitOp(OpcodeKind::Cut, "#CONTRACT_ROLLBACK_CLEAN");
    compiler_.emitInput(cleanChoices, keepBoth);
    const CellRef undoSuccess =
        compiler_.emitOp(OpcodeKind::Fail, "#CONTRACT_ROLLBACK_OK");
    (void)undoSuccess;
    compiler_.appendOpcode(successRollback);
    const CellRef removeOuter =
        compiler_.emitOp(OpcodeKind::Cut, "#CONTRACT_ROLLBACK_COMMIT");
    compiler_.emitInput(removeOuter, depth);
    resume(success);
    compiler_.appendOpcode(restored);
    const CellRef removeInner =
        compiler_.emitOp(OpcodeKind::Cut, "#CONTRACT_ROLLBACK_NESTED");
    compiler_.emitInput(removeInner, keepOuter);
    const CellRef undoFailure =
        compiler_.emitOp(OpcodeKind::Fail, "#CONTRACT_ROLLBACK_FAIL");
    (void)undoFailure;
    compiler_.appendOpcode(failed);
    resume(failure);
  }

  template <class Body>
  void anyArm(std::size_t count, Bindings &bindings,
              const detail::ScopePolicy policy, Body &&body,
              Continuation success, Continuation failure) {
    if (count == 0) {
      resume(failure);
      return;
    }
    const CellRef matchedAll =
        compiler_.mintContractLabel("#CONTRACT_ARMS_MATCHED");
    const CellRef refusedAll =
        compiler_.mintContractLabel("#CONTRACT_ARMS_REFUSED");
    const CellRef done = compiler_.mintContractLabel("#CONTRACT_ARMS_DONE");
    const auto onMatchedAll = [&] { goTo(matchedAll); };
    for (std::size_t index = 0; index < count; ++index) {
      const CellRef next = compiler_.mintContractLabel("#CONTRACT_NEXT_ARM");
      const auto onNext  = [&] { goTo(next); };
      scope(
          bindings, policy,
          [&](Bindings &trial, Continuation matched, Continuation refused) {
            body(index, trial, matched, refused);
          },
          onMatchedAll, onNext);
      compiler_.appendOpcode(next);
    }
    goTo(refusedAll);
    compiler_.appendOpcode(matchedAll);
    resume(success);
    jump(done);
    compiler_.appendOpcode(refusedAll);
    resume(failure);
    compiler_.appendOpcode(done);
  }

  template <class Range, class Body>
  void allOf(const Range &range, Body &&body, Continuation success,
             Continuation failure) {
    for (const auto &item : range) {
      const CellRef next = compiler_.mintContractLabel("#CONTRACT_NEXT_FIELD");
      const auto onNext  = [&] { goTo(next); };
      body(item, onNext, failure);
      compiler_.appendOpcode(next);
    }
    resume(success);
  }

  template <class Body>
  void allOf(const Subjects &subjects, Body &&body, Continuation success,
             Continuation failure) {
    const CellRef iterator = compiler_.core_.arena().makeCell();
    const CellRef begin =
        compiler_.emitOp(OpcodeKind::StreamBegin, "#CONTRACT_ELEMS_BEGIN");
    compiler_.emitInput(begin, subjects);
    compiler_.emitOutput(begin, iterator);
    const CellRef loop = compiler_.mintContractLabel("#CONTRACT_ELEMS_LOOP");
    compiler_.appendOpcode(loop);
    const CellRef next =
        compiler_.emitOp(OpcodeKind::IteratorNext, "#CONTRACT_ELEMS_NEXT");
    compiler_.emitInput(next, iterator);
    const CellRef item    = compiler_.core_.arena().makeCell();
    const CellRef present = compiler_.core_.arena().makeCell();
    compiler_.emitOutput(next, item);
    compiler_.emitOutput(next, present);
    const CellRef done = compiler_.mintContractLabel("#CONTRACT_ELEMS_DONE");
    branchFalse(present, done);
    const auto repeat = [&] { goTo(loop); };
    body(item, repeat, failure);
    compiler_.appendOpcode(done);
    resume(success);
  }

  template <class Success>
  void validContext(Context context, Success &&success, Continuation failure) {
    if (!context) {
      resume(failure);
      return;
    }
    probeBasic(BasicType{.kind = BasicType::Kind::Cell}, *context,
               std::forward<Success>(success), failure);
  }

  template <class Success, class Failure>
  void empty(const Subjects &subjects, Success &&onEmpty,
             Failure &&onNonempty) {
    const CellRef present    = compiler_.emitCellPresence(subjects);
    const CellRef emptyLabel = compiler_.mintContractLabel("#CONTRACT_EMPTY");
    const CellRef done = compiler_.mintContractLabel("#CONTRACT_EMPTY_DONE");
    branchFalse(present, emptyLabel);
    onNonempty();
    jump(done);
    compiler_.appendOpcode(emptyLabel);
    onEmpty();
    compiler_.appendOpcode(done);
  }

  template <class Success>
  void single(const Subjects &subjects, Success &&onSingle,
              Continuation failure) {
    const CellRef iterator = compiler_.core_.arena().makeCell();
    const CellRef begin =
        compiler_.emitOp(OpcodeKind::StreamBegin, "#CONTRACT_SINGLE_BEGIN");
    compiler_.emitInput(begin, subjects);
    compiler_.emitOutput(begin, iterator);
    const CellRef first        = compiler_.core_.arena().makeCell();
    const CellRef firstPresent = compiler_.core_.arena().makeCell();
    const CellRef nextFirst =
        compiler_.emitOp(OpcodeKind::IteratorNext, "#CONTRACT_SINGLE_FIRST");
    compiler_.emitInput(nextFirst, iterator);
    compiler_.emitOutput(nextFirst, first);
    compiler_.emitOutput(nextFirst, firstPresent);
    const CellRef noFirst =
        compiler_.mintContractLabel("#CONTRACT_SINGLE_EMPTY");
    branchFalse(firstPresent, noFirst);

    const CellRef second        = compiler_.core_.arena().makeCell();
    const CellRef secondPresent = compiler_.core_.arena().makeCell();
    const CellRef nextSecond =
        compiler_.emitOp(OpcodeKind::IteratorNext, "#CONTRACT_SINGLE_SECOND");
    compiler_.emitInput(nextSecond, iterator);
    compiler_.emitOutput(nextSecond, second);
    compiler_.emitOutput(nextSecond, secondPresent);
    const CellRef one  = compiler_.mintContractLabel("#CONTRACT_SINGLE_ONE");
    const CellRef done = compiler_.mintContractLabel("#CONTRACT_SINGLE_DONE");
    branchFalse(secondPresent, one);
    resume(failure);
    compiler_.appendOpcode(one);
    onSingle(first);
    jump(done);
    compiler_.appendOpcode(noFirst);
    resume(failure);
    compiler_.appendOpcode(done);
  }

  template <class Success>
  void probeLink(Context context, Dimension dimension,
                 zigzag::DimVector direction, Success &&onFound,
                 Continuation onMissing) {
    if (!context) {
      resume(onMissing);
      return;
    }
    const CellRef candidate = compiler_.core_.arena().makeCell();
    const CellRef probe =
        compiler_.emitOp(OpcodeKind::VqlFieldProbe, "#CONTRACT_FIELD_PROBE");
    compiler_.emitInput(probe, *context);
    compiler_.emitInput(probe, dimension);
    compiler_.emitInputConstant(probe, direction == zigzag::DimVector::POS
                                           ? std::int64_t{1}
                                           : std::int64_t{-1});
    compiler_.emitOutput(probe, candidate);
    const CellRef present = compiler_.emitCellPresence(candidate);
    const CellRef absent =
        compiler_.mintContractLabel("#CONTRACT_FIELD_ABSENT");
    branchFalse(present, absent);
    onFound(candidate);
    compiler_.appendOpcode(absent);
    resume(onMissing);
  }

  template <class Values, class Failure>
  void evaluateDefault(const ValueExpr &expression, Context context,
                       const Bindings &defaultScope, Values &&onValues,
                       Failure &&onFailure) {
    // Captures are staged in separate stream ports. Lexical bindings do not
    // change until this entire walk returns; nested calls restore their scope.
    (void)defaultScope;

    const auto valuesFor = [&]() {
      if (std::holds_alternative<ScalarLiteral>(expression.kind)) {
        const auto raw = compiler_.compileValueExpr(
            expression, context.value_or(compiler_.core_.home()));
        if (!raw) return compiler_.emitStreamClear();
        const CellRef box =
            compiler_.emitOp(OpcodeKind::VqlBoxScalar, "#CONTRACT_DEFAULT_BOX");
        compiler_.emitInput(box, *raw);
        const CellRef boxed = compiler_.core_.arena().makeCell();
        compiler_.emitOutput(box, boxed);
        const CellRef stream = compiler_.emitStreamClear();
        compiler_.emitAppendStream(stream, std::span{&boxed, std::size_t{1}});
        return stream;
      }
      const auto values = compiler_.compileArgument(expression, context);
      return compiler_.normalizeFrameStream(values);
    };
    const bool alias     = std::holds_alternative<std::string>(expression.kind);
    const CellRef stream = valuesFor();
    if (!alias) {
      onValues(stream);
      return;
    }

    // Direct aliases historically resolve to zero or one cell. Keep that
    // runtime cardinality check when the lexical value is a dynamic stream.
    const CellRef iterator = compiler_.core_.arena().makeCell();
    const CellRef begin =
        compiler_.emitOp(OpcodeKind::StreamBegin, "#CONTRACT_ALIAS_BEGIN");
    compiler_.emitInput(begin, stream);
    compiler_.emitOutput(begin, iterator);
    const CellRef first        = compiler_.core_.arena().makeCell();
    const CellRef firstPresent = compiler_.core_.arena().makeCell();
    const CellRef firstNext =
        compiler_.emitOp(OpcodeKind::IteratorNext, "#CONTRACT_ALIAS_FIRST");
    compiler_.emitInput(firstNext, iterator);
    compiler_.emitOutput(firstNext, first);
    compiler_.emitOutput(firstNext, firstPresent);
    const CellRef empty = compiler_.mintContractLabel("#CONTRACT_ALIAS_EMPTY");
    const CellRef single =
        compiler_.mintContractLabel("#CONTRACT_ALIAS_SINGLE");
    const CellRef done    = compiler_.mintContractLabel("#CONTRACT_ALIAS_DONE");
    const CellRef allowed = compiler_.emitStreamClear();
    branchFalse(firstPresent, empty);
    const CellRef second        = compiler_.core_.arena().makeCell();
    const CellRef secondPresent = compiler_.core_.arena().makeCell();
    const CellRef secondNext =
        compiler_.emitOp(OpcodeKind::IteratorNext, "#CONTRACT_ALIAS_SECOND");
    compiler_.emitInput(secondNext, iterator);
    compiler_.emitOutput(secondNext, second);
    compiler_.emitOutput(secondNext, secondPresent);
    branchFalse(secondPresent, single);
    const CellRef assertion =
        compiler_.emitOp(OpcodeKind::Assert, "#CONTRACT_ALIAS_CARDINALITY");
    compiler_.emitInputConstant(assertion, false);
    compiler_.emitInputConstant(
        assertion,
        std::string{"contract default alias resolves to more than one cell"});
    jump(done);
    compiler_.appendOpcode(empty);
    jump(done);
    compiler_.appendOpcode(single);
    compiler_.emitAppendStream(allowed, std::span{&stream, std::size_t{1}});
    compiler_.appendOpcode(done);
    onValues(allowed);
    (void)onFailure;
  }

  Subjects emptySubjects() { return compiler_.emitStreamClear(); }

  template <class Success>
  void withSingleton(Subject subject, Success &&onSubjects) {
    const CellRef singleton = compiler_.emitStreamClear();
    compiler_.emitAppendStream(singleton, std::span{&subject, std::size_t{1}});
    onSubjects(singleton);
  }

  Subject subjectOf(Context context) const { return context.value(); }
  Context contextOf(Subject subject) const { return subject; }
  Dimension resolveDimension(std::string_view name) {
    return compiler_.emitConstant(CellValue(std::string{name}));
  }

  void capture(Bindings &bindings, std::string_view name,
               const Subjects &values) {
    const auto found = bindings.find(std::string{name});
    if (found == bindings.end() || found->second.empty()) return;
    const CellRef destination = found->second.front();
    const CellRef clear =
        compiler_.emitOp(OpcodeKind::StreamClear, "#CONTRACT_CAPTURE_CLEAR");
    compiler_.emitOutput(clear, destination);
    compiler_.emitAppendStream(destination, std::span{&values, std::size_t{1}});
  }

  template <class Success>
  void probeBasic(const BasicType &type, Subject subject, Success &&onMatch,
                  Continuation failure) {
    TypeExpr leaf{.unionType =
                      UnionType{.members = {PrimaryType{.kind = type}}}};
    const CellRef descriptor =
        compiler_.emitConstant(encodeTypeDescriptor(leaf));
    const CellRef match =
        compiler_.emitOp(OpcodeKind::MatchContract, "#CONTRACT_BASIC");
    compiler_.emitInput(match, descriptor);
    compiler_.emitInputConstant(match, std::int64_t{1});
    compiler_.emitInput(match, subject);
    const CellRef matched = compiler_.core_.arena().makeCell();
    compiler_.emitOutput(match, matched);
    const CellRef done = compiler_.mintContractLabel("#CONTRACT_BASIC_DONE");
    const CellRef refused =
        compiler_.mintContractLabel("#CONTRACT_BASIC_REFUSED");
    branchFalse(matched, refused);
    onMatch();
    jump(done);
    compiler_.appendOpcode(refused);
    resume(failure);
    compiler_.appendOpcode(done);
  }

  template <class Success>
  void validSubject(Subject subject, Success &&onValid, Continuation failure) {
    probeBasic(BasicType{.kind = BasicType::Kind::Cell}, subject,
               std::forward<Success>(onValid), failure);
  }

  void consumeWork() { compiler_.core_.arena().consumeWork(); }
  void validateType(const TypeExpr &type) {
    ContractMatcher::validateType(type);
  }

  void resume(Continuation continuation) { continuation(); }
  void goTo(CellRef target) { jump(target); }

private:
  CellRef depthOffset(CellRef depth, std::int64_t amount) {
    const CellRef add =
        compiler_.emitOp(OpcodeKind::Add, "#CONTRACT_DEPTH_OFFSET");
    compiler_.emitInput(add, depth);
    compiler_.emitInputConstant(add, amount);
    const CellRef result = compiler_.core_.arena().makeCell();
    compiler_.emitOutput(add, result);
    return result;
  }

  void jump(CellRef target) {
    const CellRef op =
        compiler_.emitOp(OpcodeKind::BranchFalseCell, "#CONTRACT_JUMP");
    compiler_.emitInputConstant(op, false);
    compiler_.emitInput(op, target);
  }

  void branchFalse(CellRef condition, CellRef target) {
    const CellRef branch =
        compiler_.emitOp(OpcodeKind::BranchFalseCell, "#CONTRACT_BRANCH");
    compiler_.emitInput(branch, condition);
    compiler_.emitInput(branch, target);
  }

  VQLCompiler &compiler_;
};

CellRef VQLCompiler::emitContractMatch(const TypeExpr &type,
                                       const std::span<const CellRef> subjects,
                                       const bool bindCaptures) {
  validateContractAliases(type);

  if (!hasComputedDefault(type)) {
    const auto defaults      = emitContractDefaultSlots(type);
    const CellRef descriptor = emitConstant(encodeTypeDescriptor(type));
    const CellRef match = emitOp(OpcodeKind::MatchContract, "#MATCH_CONTRACT");
    emitInput(match, descriptor);
    emitInputConstant(match, static_cast<std::int64_t>(subjects.size()));
    for (const CellRef subject : subjects) emitInput(match, subject);
    for (const CellRef value : defaults) emitInput(match, value);
    const CellRef matched = core_.arena().makeCell();
    emitOutput(match, matched);
    for (const auto &name : ContractMatcher::contractCaptureNames(type)) {
      const CellRef capture = core_.arena().makeCell();
      emitOutput(match, capture);
      if (!bindCaptures) continue;
      functorBindings_.erase(name);
      streamBindings_[name] = {capture};
      varBindings_[name]    = capture;
      if (name == ".")
        ambientContext_ = emitContextFallback(capture, ambientContext_);
    }
    return matched;
  }

  return emitContractGraphMatch(type, subjects, bindCaptures);
}

void VQLCompiler::validateContractAliases(const TypeExpr &type) {
  std::vector<std::string> aliases;
  collectDirectDefaultAliases(type, aliases);
  std::ranges::sort(aliases);
  aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
  for (const auto &name : aliases) {
    if (functorBindings_.contains(name))
      throw std::invalid_argument("contract default $" + name +
                                  " cannot capture a path functor");
    const auto stream = streamBindings_.find(name);
    if (stream == streamBindings_.end()) {
      const auto scalar = varBindings_.find(name);
      if (scalar == varBindings_.end() || !scalar->second)
        throw std::invalid_argument("contract default $" + name +
                                    " has no prior lexical binding");
      if (core_.hasPipeline(*scalar->second))
        throw std::invalid_argument(
            "contract default aliases cannot depend on parameter pipelines");
    } else {
      if (stream->second.size() > 1)
        throw std::invalid_argument("contract default $" + name +
                                    " must resolve to at most one cell");
      if (!stream->second.empty() && core_.hasPipeline(stream->second.front()))
        throw std::invalid_argument(
            "contract default aliases cannot depend on parameter pipelines");
    }
  }
}

CellRef
VQLCompiler::emitContractGraphMatch(const TypeExpr &type,
                                    const std::span<const CellRef> subjects,
                                    const bool bindCaptures) {
  validateContractAliases(type);
  const bool savedGraph   = compilingGraphContract_;
  compilingGraphContract_ = true;
  const RestoreFlag restore{compilingGraphContract_, savedGraph};

  const CaptureBindings defaultScope;

  CaptureBindings captures;
  if (bindCaptures) {
    for (const auto &name : ContractMatcher::contractCaptureNames(type)) {
      const CellRef stream = emitStreamClear();
      captures.emplace(name, std::vector<CellRef>{stream});
    }
  }
  const CellRef subjectStream = normalizeFrameStream(subjects);
  const CellRef matched       = core_.arena().makeCell();
  const CellRef succeeded     = mintContractLabel("#CONTRACT_MATCHED");
  const CellRef refused       = mintContractLabel("#CONTRACT_REFUSED");
  const CellRef done          = mintContractLabel("#CONTRACT_RESULT");
  ContractGraphBackend backend(*this);
  detail::ContractDriver<ContractGraphBackend> driver(backend);
  const auto onSucceeded = [&] { backend.goTo(succeeded); };
  const auto onRefused   = [&] { backend.goTo(refused); };
  driver.matchType(type, subjectStream, bindCaptures ? &captures : nullptr,
                   defaultScope, onSucceeded, onRefused);
  appendOpcode(succeeded);
  const CellRef setTrue =
      emitOp(OpcodeKind::CopyCellRefs, "#CONTRACT_RESULT_TRUE");
  emitInput(setTrue, emitConstant(CellValue(true)));
  emitOutput(setTrue, matched);
  backend.goTo(done);
  appendOpcode(refused);
  const CellRef setFalse =
      emitOp(OpcodeKind::CopyCellRefs, "#CONTRACT_RESULT_FALSE");
  emitInput(setFalse, emitConstant(CellValue(false)));
  emitOutput(setFalse, matched);
  appendOpcode(done);

  if (bindCaptures) {
    for (const auto &[name, slots] : captures) {
      if (slots.empty()) continue;
      const CellRef stream = slots.front();
      functorBindings_.erase(name);
      streamBindings_[name] = {stream};
      varBindings_[name]    = stream;
      if (name == ".") {
        const CellRef first = emitFirstCell(stream);
        ambientContext_     = emitContextFallback(first, ambientContext_);
      }
    }
  }
  return matched;
}

void VQLCompiler::emitRequiredContractMatch(
    const TypeExpr &type, const std::span<const CellRef> subjects,
    const std::string_view functionName) {
  if (!hasComputedDefault(type)) {
    const auto defaults      = emitContractDefaultSlots(type);
    const CellRef descriptor = emitConstant(encodeTypeDescriptor(type));
    const CellRef require = emitOp(OpcodeKind::RequireContract, "#RETURN_TYPE");
    emitInput(require, descriptor);
    emitInputConstant(require, std::string{functionName});
    emitInputConstant(require, static_cast<std::int64_t>(subjects.size()));
    for (const CellRef subject : subjects) emitInput(require, subject);
    for (const CellRef value : defaults) emitInput(require, value);
    return;
  }
  const bool savedGraph   = compilingGraphContract_;
  compilingGraphContract_ = true;
  const RestoreFlag restore{compilingGraphContract_, savedGraph};
  const auto hasAttachedPipeline = [&] {
    if (ambientContext_ && core_.hasPipeline(*ambientContext_)) return true;
    for (const auto &[_, cells] : streamBindings_)
      if (std::ranges::any_of(cells, [this](const CellRef cell) {
            return core_.hasPipeline(cell);
          }))
        return true;
    for (const auto &[_, cell] : varBindings_)
      if (cell && core_.hasPipeline(*cell)) return true;
    return false;
  };
  const CellRef matched = [&] {
    if (hasAttachedPipeline()) return emitContractMatch(type, subjects, false);
    return compileContractValidator(type, subjects);
  }();
  const CellRef failed = mintContractLabel("#RETURN_CONTRACT_FAILED");
  const CellRef done   = mintContractLabel("#RETURN_CONTRACT_DONE");
  const CellRef branch =
      emitOp(OpcodeKind::BranchFalseCell, "#RETURN_CONTRACT_BRANCH");
  emitInput(branch, matched);
  emitInput(branch, failed);
  const CellRef jump =
      emitOp(OpcodeKind::BranchFalseCell, "#RETURN_CONTRACT_OK");
  emitInputConstant(jump, false);
  emitInput(jump, done);
  appendOpcode(failed);
  TypeExpr any{.unionType = UnionType{
                   .members = {PrimaryType{
                       .kind = BasicType{.kind = BasicType::Kind::Any}}}}};
  const CellRef fallback =
      emitOp(OpcodeKind::RequireContract, "#RETURN_CONTRACT_REQUIRED");
  emitInput(fallback, emitConstant(encodeTypeDescriptor(any)));
  emitInputConstant(fallback, std::string{functionName});
  emitInputConstant(fallback, std::int64_t{0});
  appendOpcode(done);
}

} // namespace xanadu::vql
