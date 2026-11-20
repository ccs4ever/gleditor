/**
 * @file compiler.cpp
 * @brief Implementation of VQL Compiler translating AST to Vortex bytecode.
 */
#include "common/xanadu/vql/compiler.hpp"

#include "common/xanadu/vortex/assembly_transaction.hpp"
#include "common/xanadu/vql/invocation.hpp"
#include <gleditor/logging.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "common/xanadu/vql/closure_lambda_syntax.hpp"
#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/lexer.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vql {

namespace {

/// Throw sites inside the compiler are not annotated with a category (there
/// are about 260 of them), so a refusal that carries none is classified from
/// its message. Anything unrecognised is Unsupported: every unannotated site
/// refuses the whole query, and none of them is a caller error worth keeping
/// std::invalid_argument for.
QueryErrorCategory classifyCompilerFailure(const std::exception &error) {
  if (const auto *refusal = dynamic_cast<const QueryRefusal *>(&error))
    return refusal->category();
  if (dynamic_cast<const ParseError *>(&error))
    return QueryErrorCategory::Parse;
  if (dynamic_cast<const TypeContractError *>(&error))
    return QueryErrorCategory::TypeContract;
  const std::string_view message = error.what();
  constexpr std::array unknownMarkers{
      std::string_view{"unknown compiled function"},
      std::string_view{"unknown VQL function"},
  };
  for (const auto marker : unknownMarkers)
    if (message.find(marker) != std::string_view::npos)
      return QueryErrorCategory::Unknown;
  // "is not compiled to Vortex yet", "require(s) Vortex graph lowering",
  // "does not have Vortex graph lowering yet" and "require ... lowering" all
  // fall through to the default.
  return QueryErrorCategory::Unsupported;
}

/// Names the syntactic form of an operand for a refusal message, so the user
/// sees which construct could not be lowered.
std::string valueFormName(const ValueExpr &expr) {
  const auto &kind = expr.kind;
  if (std::holds_alternative<std::shared_ptr<PathExpression>>(kind))
    return "path";
  if (std::holds_alternative<ScalarLiteral>(kind)) return "literal";
  if (std::holds_alternative<std::string>(kind)) return "variable";
  if (std::holds_alternative<std::shared_ptr<FunctionInvocation>>(kind))
    return "function result";
  if (std::holds_alternative<std::shared_ptr<RuntimeClosureInvocation>>(kind))
    return "runtime closure call";
  if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(kind))
    return "shape";
  if (std::holds_alternative<std::shared_ptr<VersionExpr>>(kind))
    return "version expression";
  if (std::holds_alternative<std::shared_ptr<PathLambda>>(kind))
    return "closure";
  if (std::holds_alternative<std::shared_ptr<BooleanExpr>>(kind))
    return "boolean expression";
  return "expression";
}

} // namespace

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

void VQLCompiler::refuseSemanticBuiltin(const std::string_view name) {
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL semantic builtin '" + std::string(name) +
                         "' is not compiled to Vortex yet");
}

bool VQLCompiler::isSemanticShapeBuiltin(const std::string_view name) {
  return name == "project" || name == "apply_shape";
}

bool VQLCompiler::isSemanticQuantifierBuiltin(const std::string_view name) {
  return name == "any" || name == "all" || name == "none" || name == "one" ||
         name == "optional_one" || name == "first" || name == "same_cell";
}

bool VQLCompiler::isPathFunction(const std::string_view name) {
  return name == "sort_into" || name == "find" || name == "contains" ||
         name == "count";
}

bool VQLCompiler::isLensFunction(const std::string_view name) {
  return name == "lens" || name == "docs" || name == "files";
}

bool VQLCompiler::isKnownUnloweredFunction(const std::string_view name) {
  // The function matrix of design/vql-query-language.md section 2, plus delta.
  return isSemanticShapeBuiltin(name) || isSemanticQuantifierBuiltin(name) ||
         isPathFunction(name) || isLensFunction(name) || name == "delta" ||
         name == "current" || name == "local" || name == "slices" ||
         name == "topic" || name == "btpk" || name == "btih" ||
         name == "citing" || name == "ledger" || name == "system";
}

std::string_view
VQLCompiler::opcodeMnemonic(zigzag::vortex::OpcodeKind kind) noexcept {
  switch (kind) {
  case OpcodeKind::Add:
    return "ADD";
  case OpcodeKind::Sub:
    return "SUB";
  case OpcodeKind::Mul:
    return "MUL";
  case OpcodeKind::Div:
    return "DIV";
  case OpcodeKind::DivMod:
    return "DIVMOD";
  case OpcodeKind::Mod:
    return "MOD";
  case OpcodeKind::Neg:
    return "NEG";
  case OpcodeKind::Eq:
    return "EQ";
  case OpcodeKind::Neq:
    return "NEQ";
  case OpcodeKind::Lt:
    return "LT";
  case OpcodeKind::Lte:
    return "LTE";
  case OpcodeKind::Gt:
    return "GT";
  case OpcodeKind::Gte:
    return "GTE";
  case OpcodeKind::And:
    return "AND";
  case OpcodeKind::Or:
    return "OR";
  case OpcodeKind::Not:
    return "NOT";
  case OpcodeKind::Link:
    return "LINK";
  case OpcodeKind::Linked:
    return "LINKED";
  case OpcodeKind::Break:
    return "BREAK";
  case OpcodeKind::New:
    return "NEW";
  case OpcodeKind::Value:
    return "VALUE";
  case OpcodeKind::Splice:
    return "SPLICE";
  case OpcodeKind::Clone:
    return "CLONE";
  case OpcodeKind::Jump:
    return "JUMP";
  case OpcodeKind::Branch:
    return "BRANCH";
  case OpcodeKind::Call:
    return "CALL";
  case OpcodeKind::Return:
    return "RETURN";
  case OpcodeKind::Halt:
    return "HALT";
  case OpcodeKind::MatchContract:
    return "MATCH_CONTRACT";
  case OpcodeKind::RequireContract:
    return "REQUIRE_CONTRACT";
  case OpcodeKind::CopyCellRefs:
    return "COPY_CELL_REFS";
  case OpcodeKind::ClearCellRefs:
    return "CLEAR_CELL_REFS";
  case OpcodeKind::BranchFalseCell:
    return "BRANCH_FALSE_CELL";
  case OpcodeKind::StreamClear:
    return "STREAM_CLEAR";
  case OpcodeKind::StreamAppend:
    return "STREAM_APPEND";
  case OpcodeKind::StreamBegin:
    return "STREAM_BEGIN";
  case OpcodeKind::StreamReverseBegin:
    return "STREAM_REVERSE_BEGIN";
  case OpcodeKind::VqlCompare:
    return "VQL_COMPARE";
  case OpcodeKind::ResumeInit:
    return "RESUME_INIT";
  case OpcodeKind::ResumeSet:
    return "RESUME_SET";
  case OpcodeKind::ProbeLink:
    return "PROBE_LINK";
  case OpcodeKind::InvokeFrame:
    return "INVOKE_FRAME";
  case OpcodeKind::ReturnFrame:
    return "RETURN_FRAME";
  case OpcodeKind::VqlString:
    return "VQL_STRING";
  case OpcodeKind::VqlAdd:
    return "VQL_ADD";
  case OpcodeKind::VqlBoxScalar:
    return "VQL_BOX_SCALAR";
  case OpcodeKind::VqlMaskLinks:
    return "VQL_MASK_LINKS";
  case OpcodeKind::CreateClosure:
    return "CREATE_CLOSURE";
  case OpcodeKind::ApplyClosure:
    return "APPLY_CLOSURE";
  case OpcodeKind::CheckClosureCall:
    return "CHECK_CLOSURE_CALL";
  case OpcodeKind::ScopedChoice:
    return "SCOPED_CHOICE";
  case OpcodeKind::VqlFieldProbe:
    return "VQL_FIELD_PROBE";
  case OpcodeKind::PushGuard:
    return "PUSH_GUARD";
  case OpcodeKind::PopGuard:
    return "POP_GUARD";
  case OpcodeKind::CheckGuards:
    return "CHECK_GUARDS";
  case OpcodeKind::OpenGuardScope:
    return "OPEN_GUARD_SCOPE";
  case OpcodeKind::CloseGuardScope:
    return "CLOSE_GUARD_SCOPE";
  case OpcodeKind::BeginMutationGroup:
    return "BEGIN_MUTATION_GROUP";
  case OpcodeKind::EndMutationGroup:
    return "END_MUTATION_GROUP";
  case OpcodeKind::RankBegin:
    return "RANK_BEGIN";
  case OpcodeKind::IteratorNext:
    return "ITERATOR_NEXT";
  case OpcodeKind::Nop:
    return "NOP";
  case OpcodeKind::Bind:
    return "BIND";
  case OpcodeKind::RestoreBindings:
    return "RESTORE_BINDINGS";
  case OpcodeKind::Resolve:
    return "RESOLVE";
  case OpcodeKind::Assert:
    return "ASSERT";
  case OpcodeKind::Abs:
    return "ABS";
  case OpcodeKind::Min:
    return "MIN";
  case OpcodeKind::Max:
    return "MAX";
  case OpcodeKind::Clamp:
    return "CLAMP";
  case OpcodeKind::Trim:
    return "TRIM";
  case OpcodeKind::ToLower:
    return "TOLOWER";
  case OpcodeKind::ToUpper:
    return "TOUPPER";
  case OpcodeKind::Unify:
    return "UNIFY";
  case OpcodeKind::IsVar:
    return "ISVAR";
  case OpcodeKind::MakeVar:
    return "MAKEVAR";
  case OpcodeKind::MakeTerm:
    return "MAKETERM";
  case OpcodeKind::Deref:
    return "DEREF";
  case OpcodeKind::Choice:
    return "CHOICE";
  case OpcodeKind::Fail:
    return "FAIL";
  case OpcodeKind::Cut:
    return "CUT";
  case OpcodeKind::ChoiceDepth:
    return "CHOICEDEPTH";
  case OpcodeKind::Find:
    return "FIND";
  case OpcodeKind::FieldSet:
    return "SET";
  case OpcodeKind::RunCall:
    return "RUNCALL";
  case OpcodeKind::RunResume:
    return "RUNRESUME";
  case OpcodeKind::RelationNext:
    return "RELATIONNEXT";
  case OpcodeKind::DimByName:
    return "DIMBYNAME";
  case OpcodeKind::Hypertime:
    return "HYPERTIME";
  case OpcodeKind::Pow:
    return "POW";
  case OpcodeKind::SameCell:
    return "SAMECELL";
  case OpcodeKind::PinHistorical:
    return "PINHISTORICAL";
  case OpcodeKind::ProjectTopology:
    return "PROJECTTOPOLOGY";
  case OpcodeKind::VqlMaskProbe:
    return "VQLMASKPROBE";
  case OpcodeKind::CellOfInt:
    return "CELLOFINT";
  case OpcodeKind::IntOfCell:
    return "INTOFCELL";
  case OpcodeKind::EpochOfCell:
    return "EPOCHOFCELL";
  default:
    return "UNKNOWN";
  }
}

CellRef VQLCompiler::openGuardScope(const std::string_view label) {
  const CellRef opcode = emitOp(OpcodeKind::OpenGuardScope, label);
  const CellRef token  = core_.arena().makeCell();
  emitOutput(opcode, token);
  frameMutableSlots_.insert(token);
  return token;
}

void VQLCompiler::closeGuardScope(const CellRef token,
                                  const std::string_view label) {
  const CellRef opcode = emitOp(OpcodeKind::CloseGuardScope, label);
  emitInput(opcode, token);
}

void VQLCompiler::pushContractGuard(const PreparedContractValidator &prepared,
                                    const std::string_view diagnostic,
                                    const std::string_view label) {
  const auto limits = vm_.guardLimits();
  if (diagnostic.empty() || diagnostic.size() > limits.maxDiagnosticBytes)
    throw std::invalid_argument(
        "guard diagnostic exceeds the configured text budget");
  const CellRef opcode = emitOp(OpcodeKind::PushGuard, label);
  emitInput(opcode, prepared.descriptor);
  emitInputConstant(opcode, std::string{diagnostic});
  for (const CellRef stream : prepared.frozenStreams) emitInput(opcode, stream);
  const CellRef token = core_.arena().makeCell();
  emitOutput(opcode, token);
  frameMutableSlots_.insert(token);
}

CellRef VQLCompiler::beginMutationGroup(const std::string_view label) {
  const CellRef opcode = emitOp(OpcodeKind::BeginMutationGroup, label);
  const CellRef token  = core_.arena().makeCell();
  emitOutput(opcode, token);
  frameMutableSlots_.insert(token);
  return token;
}

void VQLCompiler::endMutationGroup(const CellRef token,
                                   const std::string_view label) {
  const CellRef opcode = emitOp(OpcodeKind::EndMutationGroup, label);
  emitInput(opcode, token);
}

VQLCompiler::VQLCompiler(zigzag::vortex::VortexCore &core,
                         zigzag::vortex::VortexVM &vm)
    : core_(core), vm_(vm) {}

VQLCompiler::VQLCompiler(MultiStoreCoordinator &coordinator,
                         zigzag::vortex::VortexVM &vm)
    : core_(coordinator.core()), vm_(vm), coordinator_(coordinator) {
  vm_.setContractCoordinator(coordinator);
}

void VQLCompiler::chargeContractEmission() {
  if (!compilingGraphContract_) return;
  if (remainingContractEmissions_ == 0)
    throw std::invalid_argument(
        "compiled contract graph emission budget exhausted");
  --remainingContractEmissions_;
}

CellRef VQLCompiler::mintOpcode(OpcodeKind kind, std::string_view label) {
  chargeContractEmission();
  if (remainingGeneratedOpcodes_ == 0)
    throw std::invalid_argument(
        "compiled VQL opcode emission budget exhausted");
  --remainingGeneratedOpcodes_;
  return vm_.mintOpcode(kind, label);
}

CellRef VQLCompiler::mintContractLabel(std::string_view label) {
  return mintOpcode(OpcodeKind::Nop, label);
}

CellRef VQLCompiler::emitOp(OpcodeKind kind, std::string_view label) {
  CellRef op = mintOpcode(kind, label);
  if (!entryOp_) {
    entryOp_ = op;
  }
  if (currentOp_) {
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, op));
  }
  currentOp_ = op;
  allOps_.push_back(op);
  return op;
}

void VQLCompiler::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
}

void VQLCompiler::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
  if (compilingFrameDefinition_) frameMutableSlots_.insert(target);
}

CellRef VQLCompiler::emitConstant(const CellValue &val,
                                  std::optional<CellRef> designatedCell) {
  CellRef c = designatedCell ? *designatedCell : core_.arena().makeCell();
  core_.writeValue(c, 0, std::nullopt, val);
  constantCells_.insert(c);
  return c;
}

CellRef VQLCompiler::emitInputConstant(CellRef op, const CellValue &val) {
  CellRef c = emitConstant(val);
  emitInput(op, c);
  return c;
}

CompilationResult VQLCompiler::compile(const QueryExpression &query,
                                       const CompilationOptions &options) {
  try {
    return compileOnce(query, options, false);
  } catch (const ViewDimensionsNeededEarlier &) {
    // A step was compiled without its guard before the first view was made, so
    // it would have walked that view's handles as plain cells. Compile again
    // with the view dimensions named first; the failed attempt's cells are the
    // program's and go with it.
    GLEDITOR_LOG_DEBUG("vql.compile",
                       "recompiling with the view dimensions named first");
    return compileOnce(query, options, true);
  }
}

CompilationResult VQLCompiler::compileOnce(const QueryExpression &query,
                                           const CompilationOptions &options,
                                           const bool nameViewDimensions) {
  zigzag::vortex::AssemblyTransaction transaction(vm_);
  remainingContractEmissions_ = options.maxContractEmissions;
  maxNamedFrameVariants_      = options.limits.maxNamedFrameVariants;
  remainingClosedFunctorTemplateWork_ =
      options.limits.maxClosedFunctorTemplateWork;
  remainingClosureCaptureWork_     = options.limits.maxClosureCaptureWork;
  nextFunctorVariantOrdinal_       = 0;
  nextClosureLambdaId_             = 0;
  remainingGeneratedOpcodes_       = options.limits.maxGeneratedOpcodes;
  remainingFrameDescriptorRecords_ = options.limits.maxFrameDescriptorRecords;
  nextContractValidatorId_         = 0;
  compilingGraphContract_          = false;
  shapeLimits_                     = options.limits.shape;
  remainingShapeProjectionRules_   = shapeLimits_.maxRules;
  remainingShapePredicateRules_    = shapeLimits_.maxRules;
  remainingShapePredicateValidationNodes_ = shapeLimits_.maxRules;
  shapeTargetConstants_.clear();
  expandedPredicateShapes_.clear();
  entryOp_.reset();
  currentOp_.reset();
  allOps_.clear();
  varBindings_.clear();
  versionVars_.clear();
  constantCells_.clear();
  sessionCells_.clear();
  guardSteps_        = false;
  viewGuardsSkipped_ = false;
  viewDimensions_.reset();
  viewRoutines_.reset();
  if (nameViewDimensions) viewDimensions_ = ensureViewDimensions(core_);
  for (const auto &[name, cells] : options.initialBindings) {
    constantCells_.insert(cells.begin(), cells.end());
    sessionCells_.insert(cells.begin(), cells.end());
  }
  mounts_ = options.mounts;
  mountBindings_.clear();
  streamBindings_ = options.initialBindings;
  for (const auto &[name, cells] : streamBindings_)
    varBindings_[name] =
        cells.empty() ? std::nullopt : std::optional{cells.front()};
  definitions_.clear();
  frameDefinitions_.clear();
  reusableFrameDefinitions_.clear();
  frameFreeCaptureNames_.clear();
  readOnlyDefinitions_.clear();
  frameFunctorFormalIndices_.clear();
  frameFunctorApplicationScopes_.clear();
  frameRequiredDefaultCaptureBindings_.clear();
  frameConditionalRequiredDefaultCaptureBindings_.clear();
  frameOmittedRestRequiredDefaultCaptureBindings_.clear();
  pendingFrameDefinitions_.clear();
  frameMutableSlots_.clear();
  compilingFrameDefinition_ = false;
  compilingFrameDefinitionName_.clear();
  activeCalls_.clear();
  contextInputs_  = options.contextCells;
  ambientContext_ = std::nullopt;
  functorBindings_.clear();
  functorBindings_ = options.initialFunctors;
  shapeHandles_    = options.initialShapes;
  shapesExpanding_.clear();
  shapeCallsActive_.clear();
  lastResultCells_.clear();
  dynamicStreams_.clear();

  try {
    const auto resultCell = compileQuery(query);
    compilePendingFrameDefinitions();
    if (lastResultCells_.empty() && resultCell) {
      lastResultCells_.push_back(*resultCell);
    }
    std::optional<CellRef> terminal;
    std::optional<CellRef> cursor;
    if (options.targetLibrary) {
      CellRef retOp = emitOp(OpcodeKind::Return, "#RETURN");
      for (CellRef r : lastResultCells_) {
        emitInput(retOp, r);
      }
      terminal = retOp;
      linkAsLibrary(entryOp_.value(), options.moduleName, options.symbolName);
    } else {
      CellRef haltOp = emitOp(OpcodeKind::Halt, "#HALT");
      for (CellRef r : lastResultCells_) {
        emitInput(haltOp, r);
      }
      terminal = haltOp;
      cursor   = linkAsExecutable(entryOp_.value());
    }

    std::string dis = disassemble(entryOp_);
    for (const auto &[key, state] : frameDefinitions_) {
      if (!state.entry) continue;
      std::string label = key.name;
      if (key.restGroupCount)
        label += "<rest=" + std::to_string(*key.restGroupCount) + ">";
      if (state.functorVariantOrdinal)
        label +=
            "<functor=" + std::to_string(*state.functorVariantOrdinal) + ">";
      dis += "\nFRAME " + label + ":\n" + disassemble(state.entry);
    }
    CompilationResult result{
        .success          = true,
        .entryOpcode      = entryOp_,
        .errorMessage     = "",
        .generatedOpcodes = allOps_,
        .disassembly      = std::move(dis),
        .terminalOpcode   = terminal,
        .cursor           = cursor,
        .resultSlots      = lastResultCells_,
        .shapes           = shapeHandles_,
    };
    for (const auto &[name, slots] : streamBindings_) {
      if (name == ".") continue;
      if (const auto initial = options.initialBindings.find(name);
          initial != options.initialBindings.end() && initial->second == slots)
        continue;
      result.sessionBindings.push_back(CompilationResult::SessionBinding{
          .name  = name,
          .kind  = CompilationResult::SessionBinding::Kind::Stream,
          .slots = slots,
      });
    }
    for (const auto &[name, steps] : functorBindings_) {
      result.sessionBindings.push_back(CompilationResult::SessionBinding{
          .name    = name,
          .kind    = CompilationResult::SessionBinding::Kind::Functor,
          .functor = steps,
      });
    }
    result.sessionBindings.insert(result.sessionBindings.end(),
                                  mountBindings_.begin(), mountBindings_.end());
    transaction.commit();
    return result;
  } catch (const std::invalid_argument &refused) {
    return failCompilation(refused);
  } catch (const QueryRefusal &refused) {
    return failCompilation(refused);
  }
}

std::vector<CompilationResult::SessionBinding>
VQLCompiler::declareMounts(const QueryExpression &query,
                           const CompilationOptions &options) {
  mounts_ = options.mounts;
  mountBindings_.clear();
  compileMountDeclarations(query);
  return std::exchange(mountBindings_, {});
}

CompilationResult VQLCompiler::failCompilation(const std::exception &refused) {
  entryOp_.reset();
  currentOp_.reset();
  allOps_.clear();
  lastResultCells_.clear();
  frameDefinitions_.clear();
  pendingFrameDefinitions_.clear();
  return CompilationResult{
      .success          = false,
      .entryOpcode      = std::nullopt,
      .errorMessage     = refused.what(),
      .generatedOpcodes = {},
      .disassembly      = "",
      .category         = classifyCompilerFailure(refused),
  };
}

DimRef VQLCompiler::resolveDimension(std::string_view name) {
  const auto &dims = core_.dims();
  if (name == "d.stores") return dims.stores;
  if (name == "d.name") return dims.name;
  if (name == "d.role") return dims.role;
  if (name == "d.clone") return dims.clone;
  if (name == "d.dims") return dims.dims;
  if (name == "d.grab") return dims.grab;
  if (name == "d.step") return dims.step;
  if (name == "d.spin") return dims.spin;
  if (name == "d.stack") return dims.stack;
  if (name == "d.contract") return dims.contract;
  if (name == "d.cursors") return dims.cursors;
  if (name == "d.cache") return dims.cache;
  if (name == "d.vars") return dims.vars;
  if (name == "d.values") return dims.values;
  if (name == "d.pinning-cursors") return dims.pinningCursors;
  if (name == "d.stdlib") return dims.stdlib;
  if (name == "d.clause") return dims.clause;

  return core_.findOrMintDimension(name);
}

CompilationResult VQLCompiler::compile(std::string_view queryString,
                                       const CompilationOptions &options) {
  try {
    Parser parser(queryString);
    QueryExpression ast = parser.parseQuery();
    return compile(ast, options);
  } catch (const ParseError &err) {
    return CompilationResult{
        .success          = false,
        .entryOpcode      = std::nullopt,
        .errorMessage     = std::string("Parse error: ") + err.what(),
        .generatedOpcodes = {},
        .disassembly      = "",
        .category         = QueryErrorCategory::Parse,
    };
  }
}

std::optional<CellRef> VQLCompiler::compileQuery(const QueryExpression &query) {
  const auto frameEligibility = analyzeFrameEligibility(query);
  if (!frameEligibility)
    throw QueryRefusal(QueryErrorCategory::Budget,
                       frameEligibility.error().message);
  reusableFrameDefinitions_      = frameEligibility->reusableDefinitions;
  readOnlyDefinitions_           = frameEligibility->readOnlyDefinitions;
  frameFreeCaptureNames_         = frameEligibility->freeCaptureNames;
  frameFunctorFormalIndices_     = frameEligibility->functorFormalIndices;
  frameFunctorApplicationScopes_ = frameEligibility->functorApplicationScopes;
  frameRequiredDefaultCaptureBindings_ =
      frameEligibility->requiredDefaultCaptureBindings;
  frameConditionalRequiredDefaultCaptureBindings_ =
      frameEligibility->conditionalRequiredDefaultCaptureBindings;
  frameOmittedRestRequiredDefaultCaptureBindings_ =
      frameEligibility->omittedRestRequiredDefaultCaptureBindings;
  try {
    for (const auto &definition : query.defs)
      validateFunctionSignature(definition);
  } catch (const TypeContractError &error) {
    throw QueryRefusal(QueryErrorCategory::TypeContract, error.what());
  }
  for (const auto &definition : query.defs) {
    if (definition.name == "#vql-current-timestamp")
      throw std::invalid_argument(
          "reserved VQL intrinsic name: #vql-current-timestamp");
    if (!definitions_.emplace(definition.name, definition).second)
      throw QueryRefusal(QueryErrorCategory::Malformed,
                         "duplicate function definition: " + definition.name);
  }
  if (!query.mounts.empty() || !query.namespaceDeclarations.empty())
    compileMountDeclarations(query);
  if (!query.body.statements.empty()) {
    std::optional<CellRef> result;
    for (const auto &statement : query.body.statements) {
      lastResultCells_.clear();
      if (const auto *path = std::get_if<PathExpression>(&statement)) {
        result = compilePathExpression(*path, core_.home());
      } else if (const auto *block = std::get_if<ExecutionBlock>(&statement)) {
        result = compileExecutionBlock(*block);
      } else if (const auto *weave = std::get_if<WeaveStatement>(&statement)) {
        result = compileExecutionBlock(
            ExecutionBlock{.action = ActionClause{.clause = weave->effect}});
      } else {
        result = compileWatchOrPerspectiveStatement(statement);
      }
    }
    return result;
  }
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return compilePathExpression(std::get<PathExpression>(query.expr),
                                 core_.home());
  }
  return compileExecutionBlock(std::get<ExecutionBlock>(query.expr));
}

namespace {

/// Whether a context-anchored path reads from its context cell. A first step
/// that names a variable alias (`/$p/...`) or a root selector supplies its own
/// start, so only a bare `.` and a path opening on a plain dimension need one.
bool walksFromContext(const PathExpression &path) {
  if (path.steps.empty()) return true;
  const auto *dimension =
      std::get_if<SignedDimensionStep>(&path.steps.front().selector);
  return dimension != nullptr && !dimension->dimName.starts_with('$');
}

} // namespace

std::optional<CellRef>
VQLCompiler::compilePathExpression(const PathExpression &path,
                                   CellRef ctxCell) {
  if (path.isInvocationRoot) {
    const auto *runtimeCall =
        path.steps.empty()
            ? nullptr
            : std::get_if<std::shared_ptr<RuntimeClosureInvocation>>(
                  &path.steps.front().selector);
    const bool validSelector =
        !path.steps.empty() && (std::holds_alternative<FunctionInvocation>(
                                    path.steps.front().selector) ||
                                (runtimeCall && *runtimeCall));
    if (path.anchor.kind != AnchorKind::Context || path.isFunctor ||
        path.anchor.derefMaster || path.anchor.versionPin ||
        !path.anchorPredicates.empty() || path.anchorProjection ||
        !validSelector || path.steps.front().capturedFunctor)
      throw QueryRefusal(QueryErrorCategory::Malformed,
                         "invalid initial invocation marker on path");
  }
  if (path.anchor.versionPin) return compileVersionPinnedPath(path, ctxCell);
  // Steps of this path may meet a view handle when they start from something
  // the compiler did not make from a plain anchor: a variable (a handle bound
  // by an earlier query), a context, or a cell a projection answers.
  struct GuardScope {
    bool &flag;
    bool saved;
    ~GuardScope() { flag = saved; }
  } const guardScope{guardSteps_, guardSteps_};
  guardSteps_ = path.anchor.kind == AnchorKind::Variable ||
                path.anchor.kind == AnchorKind::Context ||
                path.anchor.kind == AnchorKind::LiteralCellId ||
                static_cast<bool>(path.anchorProjection);
  const ShapeUse shapeUse = takeShapeUse();
  std::vector<CellRef> stream;
  if (path.anchor.kind == AnchorKind::Variable &&
      streamBindings_.contains(path.anchor.name)) {
    stream = streamBindings_.at(path.anchor.name);
  } else {
    if (path.anchor.kind == AnchorKind::Context) {
      if (ambientContext_)
        stream.push_back(*ambientContext_);
      else if (activeCalls_.empty() && !contextInputs_.empty())
        stream = contextInputs_;
      else if (path.isInvocationRoot)
        stream.push_back(ctxCell);
      else if (activeCalls_.empty() && walksFromContext(path))
        // R-CTX: a context-relative path with nothing bound would otherwise
        // compile into a walk from no cell and answer empty, which reads as
        // "no matches" instead of "no context". A body compiled for a call
        // takes its context from that call's arguments, so it is exempt.
        throw QueryRefusal(
            QueryErrorCategory::Unsupported,
            "VQL context-relative path has no bound context: '.' and "
            "steps without an anchor need a path, a weave or a call to supply "
            "one; anchor the path at ## or a variable");
    } else if (const auto start = compileAnchor(path.anchor)) {
      stream.push_back(*start);
    }
  }
  if (path.anchor.kind == AnchorKind::Variable &&
      !(shapeUse.consumes && namesBareVariable(path)))
    refuseShapeCells(stream, shapeUse.consumes
                                 ? shapeUse.use
                                 : "a path anchor, a source or a step");
  // A predicate filters into fresh program cells, so a handle in front of it
  // would reach the steps as a plain cell with the view dropped.
  if (!path.anchorPredicates.empty()) {
    refuseViewHandles(stream, "a predicate on the path's anchor");
    rejectViewHandlesAtRunTime(
        stream, viewHandleRefusal("a predicate on the path's anchor"));
  }
  for (const auto &predicate : path.anchorPredicates) {
    stream = emitMap(stream, [&](const CellRef candidate) {
      const CellRef result = core_.arena().makeCell();
      const auto clear     = emitOp(OpcodeKind::ClearCellRefs, "#ANCHOR_CLEAR");
      emitOutput(clear, result);
      auto condition = compileCandidatePredicate(predicate, candidate);
      if (!condition) condition = emitConstant(false);
      const auto end    = mintOpcode(OpcodeKind::Nop, "#ANCHOR_FILTER_END");
      const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#ANCHOR_FILTER");
      emitInput(branch, *condition);
      emitInput(branch, end);
      const auto copy = emitOp(OpcodeKind::CopyCellRefs, "#ANCHOR_KEEP");
      emitInput(copy, candidate);
      emitOutput(copy, result);
      appendOpcode(end);
      return std::vector<CellRef>{result};
    });
  }
  if (path.anchor.derefMaster) {
    refuseViewHandles(stream, "a master dereference of the path's anchor");
    rejectViewHandlesAtRunTime(
        stream, viewHandleRefusal("a master dereference of the path's anchor"));
  }
  if (path.anchor.derefMaster) {
    stream = emitMap(stream, [&](CellRef candidate) {
      return std::vector<CellRef>{emitDereference(candidate)};
    });
  }
  if (path.anchorProjection)
    stream = applyShapeProjection(stream, *path.anchorProjection, true);
  stream =
      compileSteps(path.steps, std::move(stream),
                   path.anchor.kind != AnchorKind::Context ||
                       ambientContext_.has_value() || !contextInputs_.empty(),
                   path.isInvocationRoot);

  if (path.shapeProjection) {
    stream = applyShapeProjection(stream, *path.shapeProjection, true);
  }

  if (path.cloneTail && !stream.empty()) {
    stream = emitMap(stream, [&](CellRef master) {
      const CellRef group     = beginMutationGroup("#CLONE_TAIL_GROUP");
      const auto cloneDim     = core_.dims().clone;
      const auto refuseUnless = [&](const CellRef condition,
                                    const std::string_view message) {
        const auto require = emitOp(OpcodeKind::Assert, "#CLONE_REFUSE");
        emitInput(require, condition);
        emitInputConstant(require, std::string{message});
      };
      // Reads the clone neighbour of @p cell and requires there be none.
      const auto requireUnlinked = [&](const CellRef cell, const DimVector side,
                                       const std::string_view message) {
        const auto read = emitOp(OpcodeKind::Link, "#CLONE_PROBE");
        emitInput(read, cell);
        emitInputConstant(read, static_cast<std::int64_t>(cloneDim));
        emitInputConstant(read, static_cast<std::int64_t>(toSign(side)));
        const auto neighbor = core_.arena().makeCell();
        emitOutput(read, neighbor);
        const auto linked = emitCellPresence(neighbor);
        const auto free   = core_.arena().makeCell();
        const auto negate = emitOp(OpcodeKind::Not, "#CLONE_FREE");
        emitInput(negate, linked);
        emitOutput(negate, free);
        refuseUnless(free, message);
      };

      // The leftmost cell is the authority: a join that starts mid-rank would
      // give the rank two masters.
      requireUnlinked(master, DimVector::NEG,
                      "VQL clone join refused: the left operand is already a "
                      "clone of another cell, so it is not the master");

      // Each operand joins the end of the rank, so `a><b><c` is the chain
      // a, b, c and every cell reads a's content.
      const CellRef tail = emitExtreme(master, cloneDim, DimVector::POS);
      for (const auto &op : path.cloneTail->operands) {
        std::optional<CellRef> opCell;
        if (op.path) {
          opCell = compilePathExpression(*op.path, core_.home());
          if (opCell) {
            const auto same = core_.arena().makeCell();
            const auto test = emitOp(OpcodeKind::SameCell, "#CLONE_SELF");
            emitInput(test, master);
            emitInput(test, *opCell);
            emitOutput(test, same);
            const auto distinct = core_.arena().makeCell();
            const auto negate   = emitOp(OpcodeKind::Not, "#CLONE_DISTINCT");
            emitInput(negate, same);
            emitOutput(negate, distinct);
            refuseUnless(distinct, "VQL clone join refused: a cell cannot be "
                                   "joined to its own clone rank");
            for (const auto side : {DimVector::NEG, DimVector::POS}) {
              requireUnlinked(*opCell, side,
                              "VQL clone join refused: the right operand "
                              "already belongs to a clone rank");
            }
          }
        } else if (op.bareCreate) {
          CellRef newOp = emitOp(OpcodeKind::New, "#CLONE_TAIL_NEW");
          emitInput(newOp, tail);
          emitInputConstant(newOp, static_cast<std::int64_t>(cloneDim));
          emitInputConstant(newOp, static_cast<std::int64_t>(1));
          opCell = core_.arena().makeCell();
          emitOutput(newOp, *opCell);
          if (op.bareCreate->kind == CreateValue::Kind::Literal) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
            emitInput(valOp, *opCell);
            emitInputConstant(valOp, static_cast<std::int64_t>(0));
            emitInputConstant(valOp, static_cast<std::int64_t>(-1));
            emitInputConstant(valOp, op.bareCreate->literal);
          } else if (op.bareCreate->kind == CreateValue::Kind::Expression &&
                     op.bareCreate->expr) {
            const auto valData =
                compileValueExpr(*op.bareCreate->expr, core_.home());
            if (valData) {
              CellRef valOp = emitOp(OpcodeKind::Value, "#CLONE_TAIL_INIT");
              emitInput(valOp, *opCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              emitInput(valOp, *valData);
            }
          }
        }

        if (opCell) {
          CellRef linkOp = emitOp(OpcodeKind::Link, "#CLONE_JOIN");
          emitInput(linkOp, tail);
          emitInputConstant(linkOp, static_cast<std::int64_t>(cloneDim));
          emitInputConstant(linkOp, static_cast<std::int64_t>(1));
          emitInput(linkOp, *opCell);
          const auto advance = emitOp(OpcodeKind::CopyCellRefs, "#CLONE_TAIL");
          emitInput(advance, *opCell);
          emitOutput(advance, tail);
        }
      }
      endMutationGroup(group, "#CLONE_TAIL_GROUP_END");
      return std::vector<CellRef>{master};
    });
  }

  lastResultCells_ = stream;
  if (stream.empty()) return std::nullopt;
  return stream.front();
}

std::vector<CellRef>
VQLCompiler::applyShapeProjection(std::span<const CellRef> owners,
                                  const ShapeLiteral &written,
                                  const bool allowViews) {
  // Variables, shape-returning calls and nested shapes are expanded first, so
  // validation and lowering see only the rules the static path knows.
  const ShapeLiteral shape = expandShape(written);
  if (allowViews && isDynamicViewShape(shape))
    return lowerDynamicView(shape, owners);
  refuseViewHandles(owners, "a shape projection");
  rejectViewHandlesAtRunTime(owners, viewHandleRefusal("a shape projection"));
  validateShapeProjection(shape);
  return emitMap(owners, [&](const CellRef owner) {
    compileShapeProjection(shape, owner);
    return std::vector<CellRef>{owner};
  });
}

std::optional<CellRef> VQLCompiler::compileAnchor(const AnchorNode &anchor) {
  CellRef res{};
  switch (anchor.kind) {
  case AnchorKind::Home:
  case AnchorKind::Root:
    if (coordinator_) {
      const auto home = coordinator_->homeAnchor();
      if (!home) return std::nullopt;
      res = *home;
    } else {
      res = core_.home();
    }
    break;
  case AnchorKind::NamedStore: {
    if (!coordinator_)
      throw std::invalid_argument("named-store anchors require a coordinator");
    const auto home = coordinator_->resolveNamedStore(anchor.name);
    if (!home) return std::nullopt;
    res = *home;
    break;
  }
  case AnchorKind::Cursor:
  case AnchorKind::NamedCursor:
  case AnchorKind::AllCursors:
  case AnchorKind::AllNamedCursors:
    throw std::invalid_argument("cursor anchors require VM selector lowering");
  case AnchorKind::Variable: {
    if (functorBindings_.contains(anchor.name)) {
      throw std::invalid_argument(
          "a point-free functor must be applied to a path before it can be "
          "returned as a cell");
    }
    auto it = varBindings_.find(anchor.name);
    if (it != varBindings_.end()) {
      if (!it->second) return std::nullopt;
      res = *it->second;
    } else {
      return std::nullopt;
    }
    break;
  }
  case AnchorKind::LiteralCellId:
    if (anchor.cellId == 0 ||
        anchor.cellId > std::numeric_limits<CellRef>::max()) {
      throw std::invalid_argument("literal cell anchor is invalid");
    }
    res = static_cast<CellRef>(anchor.cellId);
    if (!core_.arena().contains(res)) return std::nullopt;
    break;
  case AnchorKind::Context:
    res = core_.home();
    break;
  case AnchorKind::Create: {
    res = core_.arena().makeCell();
    if (anchor.createValue.has_value()) {
      const auto &cv      = *anchor.createValue;
      const CellRef group = beginMutationGroup("#ANCHOR_CREATE_GROUP");
      std::optional<CellRef> initialValue;
      if (cv.kind == CreateValue::Kind::Expression && cv.expr)
        initialValue = compileValueExpr(*cv.expr, core_.home());
      if (cv.kind == CreateValue::Kind::Literal) {
        CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
        emitInput(valOp, res);
        emitInputConstant(valOp, static_cast<std::int64_t>(0));
        emitInputConstant(valOp, static_cast<std::int64_t>(-1));
        emitInputConstant(valOp, cv.literal);
      } else if (cv.kind == CreateValue::Kind::Expression && cv.expr) {
        if (initialValue) {
          CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
          emitInput(valOp, res);
          emitInputConstant(valOp, static_cast<std::int64_t>(0));
          emitInputConstant(valOp, static_cast<std::int64_t>(-1));
          emitInput(valOp, *initialValue);
        }
      }
      endMutationGroup(group, "#ANCHOR_CREATE_GROUP_END");
    }
    break;
  }
  case AnchorKind::StoreGenerator:
  case AnchorKind::StoreAnchor:
  case AnchorKind::Microversion:
    return compileHistoricalAnchor(anchor);
  }

  return res;
}

std::optional<CellRef> VQLCompiler::compilePathStep(const PathStep &step,
                                                    CellRef inStreamCell) {
  PathExpression dummy;
  dummy.anchor.kind = AnchorKind::Context;
  dummy.steps.push_back(step);
  return compilePathExpression(dummy, inStreamCell);
}

std::optional<CellRef>
VQLCompiler::compileCandidatePredicate(const BooleanExpr &expr,
                                       const CellRef candidate) {
  struct RestoreContext {
    std::optional<CellRef> &target;
    std::optional<CellRef> previous;
    ~RestoreContext() { target = previous; }
  } restore{ambientContext_, ambientContext_};
  // Scalar evaluation may use home as a placeholder. Only an actual
  // predicate candidate establishes semantic context for nested calls.
  ambientContext_ = candidate;
  return compileBooleanExpr(expr, candidate);
}

std::optional<CellRef> VQLCompiler::compileBooleanExpr(const BooleanExpr &expr,
                                                       CellRef ctxCell) {
  const auto result     = core_.arena().makeCell();
  const auto falseValue = emitConstant(false);
  const auto trueValue  = emitConstant(true);
  const auto initialize = emitOp(OpcodeKind::CopyCellRefs, "#BOOL_FALSE");
  emitInput(initialize, falseValue);
  emitOutput(initialize, result);
  const auto end    = mintOpcode(OpcodeKind::Nop, "#BOOL_END");
  const auto append = [&](CellRef opcode) {
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, opcode));
    currentOp_ = opcode;
    allOps_.push_back(opcode);
  };
  // False factors skip the remainder of their conjunction; a true term skips
  // all later alternatives, including effects and return-contract checks.
  for (const auto &term : expr.terms) {
    const auto nextTerm = mintOpcode(OpcodeKind::Nop, "#BOOL_NEXT_TERM");
    for (const auto &factor : term.factors) {
      std::optional<CellRef> factCell;
      if (std::holds_alternative<ComparisonExpr>(factor.test)) {
        factCell =
            compileComparison(std::get<ComparisonExpr>(factor.test), ctxCell);
      } else if (std::holds_alternative<PredicateTest>(factor.test)) {
        const auto &pt = std::get<PredicateTest>(factor.test);
        factCell       = std::visit(
            [this, ctxCell](const auto &t) -> std::optional<CellRef> {
              using T = std::decay_t<decltype(t)>;
              if constexpr (std::is_same_v<T,
                                                 std::shared_ptr<PathExpression>>) {
                if (t) {
                  const auto cells =
                      compileArgument(ValueExpr{.kind = t}, ctxCell);
                  return cells.empty() ? emitConstant(false)
                                             : emitCellPresence(cells.front());
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<T, bool>) {
                return ctxCell;
              } else if constexpr (std::is_same_v<T,
                                                        std::shared_ptr<ValueExpr>>) {
                if (t) {
                  if (const auto *shape =
                          std::get_if<std::shared_ptr<ShapeLiteral>>(&t->kind);
                      shape && *shape) {
                    return compileShapePredicate(**shape, ctxCell);
                  }
                  if (const auto named =
                          compileShapePredicateValue(*t, ctxCell))
                    return named;
                  const auto value = compileValueExpr(*t, ctxCell);
                  if (value)
                    if (const auto named =
                            shapePredicateOfCell(*value, ctxCell))
                      return named;
                  return value ? *value : emitConstant(false);
                }
                return emitConstant(false);
              } else if constexpr (std::is_same_v<
                                             T, std::shared_ptr<ShapeLiteral>>) {
                return t ? std::optional{compileShapePredicate(*t, ctxCell)}
                               : std::optional{emitConstant(false)};
              } else if constexpr (std::is_same_v<T, std::shared_ptr<
                                                               FunctionInvocation>>) {
                if (t) {
                  const ValueExpr call{.kind = t};
                  if (const auto named =
                          compileShapePredicateValue(call, ctxCell))
                    return named;
                  const auto value = compileValueExpr(call, ctxCell);
                  if (value)
                    if (const auto named =
                            shapePredicateOfCell(*value, ctxCell))
                      return named;
                  return value;
                }
                return emitConstant(false);
              } else {
                throw std::invalid_argument(
                    "unsupported VQL predicate requires graph lowering");
              }
            },
            pt.kind);
      }

      if (!factCell) factCell = emitConstant(false);
      if (factor.negated) {
        CellRef notOp = emitOp(OpcodeKind::Not, "#NOT");
        emitInput(notOp, *factCell);
        CellRef notOut = core_.arena().makeCell();
        emitOutput(notOp, notOut);
        factCell = notOut;
      }

      const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#BOOL_AND");
      emitInput(branch, *factCell);
      emitInput(branch, nextTerm);
    }
    const auto success = emitOp(OpcodeKind::CopyCellRefs, "#BOOL_TRUE");
    emitInput(success, trueValue);
    emitOutput(success, result);
    const auto done = emitOp(OpcodeKind::BranchFalseCell, "#BOOL_OR_DONE");
    emitInput(done, falseValue);
    emitInput(done, end);
    append(nextTerm);
  }
  append(end);
  return result;
}

namespace {

/// The text of a string literal operand, when @p expr is exactly one.
const std::string *textLiteral(const std::shared_ptr<ValueExpr> &expr) {
  if (!expr) return nullptr;
  const auto *literal = std::get_if<ScalarLiteral>(&expr->kind);
  return literal == nullptr ? nullptr
                            : std::get_if<std::string>(&literal->value);
}

bool compareText(const std::string &left, const CompOp op,
                 const std::string &right) {
  switch (op) {
  case CompOp::Equal:
    return left == right;
  case CompOp::NotEqual:
    return left != right;
  case CompOp::LessThan:
    return left < right;
  case CompOp::LessEqual:
    return left <= right;
  case CompOp::GreaterThan:
    return left > right;
  case CompOp::GreaterEqual:
    return left >= right;
  }
  return false;
}

} // namespace

void VQLCompiler::refuseShapeCells(const std::span<const CellRef> cells,
                                   const std::string_view use) const {
  for (const CellRef cell : cells) {
    const auto kind = handleKindOf(cell);
    if (kind != HandleKind::Shape && kind != HandleKind::UnrecordedShape)
      continue;
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "a shape value cannot be used as " + std::string{use} +
            ": it is syntax resolved while the query is compiled, not a "
            "value the graph computes with (use it in a shape rule or "
            "target, a predicate, any/all/none, project or apply_shape, a "
            "call argument that is used as a shape, a let, or return it)");
  }
}

bool VQLCompiler::namesBareVariable(const ValueExpr &expression) {
  if (std::holds_alternative<std::string>(expression.kind)) return true;
  const auto *path =
      std::get_if<std::shared_ptr<PathExpression>>(&expression.kind);
  return path && *path && namesBareVariable(**path);
}

bool VQLCompiler::namesBareVariable(const PathExpression &path) {
  return path.anchor.kind == AnchorKind::Variable && path.steps.empty() &&
         path.anchorPredicates.empty() && !path.anchorProjection &&
         !path.isFunctor;
}

void VQLCompiler::refuseHandleOperand(const HandleKind kind,
                                      const std::string_view op) const {
  if (kind == HandleKind::None) return;
  if (kind == HandleKind::ViewHandle || kind == HandleKind::Lens)
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        std::string{"a "} +
            (kind == HandleKind::ViewHandle ? "dynamic view" : "lens") +
            " value cannot be an operand of '" + std::string{op} +
            "': it is syntax resolved while the query is compiled, not a "
            "value the graph computes with");
  throw QueryRefusal(
      QueryErrorCategory::Unsupported,
      "a shape value cannot be used as an operand of '" + std::string{op} +
          "': it is syntax resolved while the query is compiled, not a value "
          "the graph computes with");
}

namespace {
std::string_view compOpText(const CompOp op) {
  switch (op) {
  case CompOp::Equal:
    return "=";
  case CompOp::NotEqual:
    return "!=";
  case CompOp::LessThan:
    return "<";
  case CompOp::LessEqual:
    return "<=";
  case CompOp::GreaterThan:
    return ">";
  case CompOp::GreaterEqual:
    return ">=";
  }
  return "?";
}
} // namespace

std::optional<CellRef>
VQLCompiler::compileComparison(const ComparisonExpr &comp, CellRef) {
  // Two string literals are text, so they compare as text. Minting a literal
  // infers a number from text that looks like one, and "nan" then compared as
  // an IEEE NaN, making a string unequal to itself.
  if (const auto *left = textLiteral(comp.left)) {
    if (const auto *right = textLiteral(comp.right))
      return emitConstant(compareText(*left, comp.op, *right));
  }
  const auto context = ambientContext_;
  const std::string operand =
      "an operand of '" + std::string{compOpText(comp.op)} + "'";
  const auto compileOperand = [&](const std::shared_ptr<ValueExpr> &side) {
    if (!side) return std::vector<CellRef>{core_.home()};
    const ShapeUseScope scope{*this, false, operand};
    return compileArgument(*side, context);
  };
  const std::vector<CellRef> lhs = compileOperand(comp.left);
  const std::vector<CellRef> rhs = compileOperand(comp.right);

  using Comparison      = zigzag::vortex::VqlComparisonKind;
  Comparison comparison = Comparison::Equal;
  switch (comp.op) {
  case CompOp::Equal:
    comparison = Comparison::Equal;
    break;
  case CompOp::NotEqual:
    comparison = Comparison::NotEqual;
    break;
  case CompOp::LessThan:
    comparison = Comparison::LessThan;
    break;
  case CompOp::LessEqual:
    comparison = Comparison::LessEqual;
    break;
  case CompOp::GreaterThan:
    comparison = Comparison::GreaterThan;
    break;
  case CompOp::GreaterEqual:
    comparison = Comparison::GreaterEqual;
    break;
  }

  for (const auto *operand : {&lhs, &rhs})
    for (const CellRef cell : *operand)
      refuseHandleOperand(handleKindOf(cell), compOpText(comp.op));

  // An empty operand makes every comparison false, including !=. Both
  // expressions have already been compiled left-to-right, as their effects
  // belong to operand preparation rather than to individual pairs.
  if (lhs.empty() || rhs.empty()) return emitConstant(false);

  // Versions form a tree, so text or numeric order would answer where history
  // cannot ("10" is after "9"; siblings "2a1" and "2b1" are unordered).
  // Causal folding is only defined for a known singleton. A stream that
  // contains a known version cannot be reduced to its first member.
  const auto scalarCell =
      [&](const std::vector<CellRef> &values) -> std::optional<CellRef> {
    if (values.size() != 1 || dynamicStreams_.contains(values.front()))
      return std::nullopt;
    return values.front();
  };
  const auto boundCell =
      [&](const ValueExpr *operand,
          const std::vector<CellRef> &values) -> std::optional<CellRef> {
    if (!operand) return std::nullopt;
    if (const auto *name = std::get_if<std::string>(&operand->kind)) {
      const auto found = varBindings_.find(*name);
      if (found != varBindings_.end() && found->second && scalarCell(values))
        return found->second;
    }
    return scalarCell(values);
  };
  const auto versionOf =
      [&](const ValueExpr *operand,
          const std::vector<CellRef> &values) -> std::optional<MicroversionId> {
    if (!operand) return std::nullopt;
    const bool written =
        std::holds_alternative<std::shared_ptr<VersionExpr>>(operand->kind);
    if (!written && !std::holds_alternative<std::string>(operand->kind))
      return std::nullopt;
    const auto cell = boundCell(operand, values);
    if (!cell) return std::nullopt;
    const auto known = versionVars_.find(*cell);
    if (known == versionVars_.end()) return std::nullopt;
    return known->second;
  };
  const auto hasKnownVersion = [&](const ValueExpr *operand,
                                   const std::vector<CellRef> &values) {
    if (const auto *name =
            operand ? std::get_if<std::string>(&operand->kind) : nullptr) {
      const auto bound = varBindings_.find(*name);
      if (bound != varBindings_.end() && bound->second &&
          versionVars_.contains(*bound->second))
        return true;
    }
    return std::ranges::any_of(values, [&](const CellRef cell) {
      return versionVars_.contains(cell);
    });
  };
  const auto leftVersion     = versionOf(comp.left.get(), lhs);
  const auto rightVersion    = versionOf(comp.right.get(), rhs);
  const bool leftHasVersion  = hasKnownVersion(comp.left.get(), lhs);
  const bool rightHasVersion = hasKnownVersion(comp.right.get(), rhs);
  if ((leftHasVersion && !leftVersion) || (rightHasVersion && !rightVersion))
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "causal version comparison requires a singleton known version; "
        "multi-valued or dynamic version streams are not lowered");
  if (leftVersion && rightVersion)
    return emitConstant(CellValue(
        compareVersionsCausally(comp.op, *leftVersion, *rightVersion)));
  if (leftVersion || rightVersion) {
    // The other side is neither a known version nor a compile-time constant
    // (a run-time string, a function's result, a path): its text may or may
    // not be a version, and comparing it as text would order versions wrongly.
    // Refuse by name rather than answer in text order.
    const auto *other       = leftVersion ? comp.right.get() : comp.left.get();
    const auto &otherValues = leftVersion ? rhs : lhs;
    if (otherValues.size() != 1 ||
        dynamicStreams_.contains(otherValues.front())) {
      constexpr std::array<std::string_view, 6> opNames{"=", "!=", "<",
                                                        ">", "<=", ">="};
      const auto opName = opNames[static_cast<std::size_t>(comp.op)];
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "version comparison '" + std::string(opName) +
              "' against a run-time " +
              (other ? valueFormName(*other) : std::string{"stream"}) +
              " is not lowered; multi-valued or dynamic stream operands "
              "require a causal version representation");
    }
    const CellRef compiledOther = otherValues.front();
    const CellRef otherCell =
        boundCell(other, otherValues).value_or(compiledOther);
    if (other && !constantCells_.contains(otherCell)) {
      constexpr std::array<std::string_view, 6> opNames{"=", "!=", "<",
                                                        ">", "<=", ">="};
      const auto opName = opNames[static_cast<std::size_t>(comp.op)];
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "version comparison '" + std::string(opName) +
              "' against a run-time " + valueFormName(*other) +
              " is not lowered; compare against a version literal or a "
              "constant");
    }
  }

  return compileCartesianComparison(comparison, lhs, rhs);
}

CellRef VQLCompiler::compileCartesianComparison(
    const zigzag::vortex::VqlComparisonKind comparison,
    const std::span<const CellRef> left, const std::span<const CellRef> right) {
  const CellRef falseValue = emitConstant(false);
  const CellRef trueValue  = emitConstant(true);
  const CellRef result     = core_.arena().makeCell();
  const CellRef initialize =
      emitOp(OpcodeKind::CopyCellRefs, "#VQL_COMPARE_FALSE");
  emitInput(initialize, falseValue);
  emitOutput(initialize, result);

  const auto branchFalse = [&](const CellRef condition, const CellRef target) {
    const CellRef branch =
        emitOp(OpcodeKind::BranchFalseCell, "#VQL_COMPARE_BRANCH");
    emitInput(branch, condition);
    emitInput(branch, target);
  };
  const auto jump = [&](const CellRef target) {
    branchFalse(falseValue, target);
  };
  const auto fresh = [&] { return core_.arena().makeCell(); };

  const CellRef outerIterator = fresh();
  const CellRef outerBegin =
      emitOp(OpcodeKind::StreamBegin, "#VQL_COMPARE_LEFT_BEGIN");
  for (const CellRef input : left) emitInput(outerBegin, input);
  emitOutput(outerBegin, outerIterator);
  const CellRef outerNext =
      emitOp(OpcodeKind::IteratorNext, "#VQL_COMPARE_LEFT_NEXT");
  emitInput(outerNext, outerIterator);
  const CellRef leftItem    = fresh();
  const CellRef leftPresent = fresh();
  emitOutput(outerNext, leftItem);
  emitOutput(outerNext, leftPresent);
  const CellRef outerDone = mintOpcode(OpcodeKind::Nop, "#VQL_COMPARE_DONE");
  branchFalse(leftPresent, outerDone);

  const CellRef innerIterator = fresh();
  const CellRef innerBegin =
      emitOp(OpcodeKind::StreamBegin, "#VQL_COMPARE_RIGHT_BEGIN");
  for (const CellRef input : right) emitInput(innerBegin, input);
  emitOutput(innerBegin, innerIterator);
  const CellRef innerNext =
      emitOp(OpcodeKind::IteratorNext, "#VQL_COMPARE_RIGHT_NEXT");
  emitInput(innerNext, innerIterator);
  const CellRef rightItem    = fresh();
  const CellRef rightPresent = fresh();
  emitOutput(innerNext, rightItem);
  emitOutput(innerNext, rightPresent);
  const CellRef innerDone =
      mintOpcode(OpcodeKind::Nop, "#VQL_COMPARE_RIGHT_DONE");
  branchFalse(rightPresent, innerDone);

  const CellRef compare = emitOp(OpcodeKind::VqlCompare, "#VQL_COMPARE_PAIR");
  emitInputConstant(compare, CellValue(static_cast<std::int64_t>(comparison)));
  emitInput(compare, leftItem);
  emitInput(compare, rightItem);
  const CellRef pairMatches = fresh();
  emitOutput(compare, pairMatches);
  const CellRef innerContinue =
      mintOpcode(OpcodeKind::Nop, "#VQL_COMPARE_CONTINUE_RIGHT");
  branchFalse(pairMatches, innerContinue);

  const CellRef copyTrue =
      emitOp(OpcodeKind::CopyCellRefs, "#VQL_COMPARE_TRUE");
  emitInput(copyTrue, trueValue);
  emitOutput(copyTrue, result);
  const CellRef found = mintOpcode(OpcodeKind::Nop, "#VQL_COMPARE_FOUND");
  jump(found);

  appendOpcode(innerContinue);
  jump(innerNext);
  appendOpcode(innerDone);
  jump(outerNext);
  appendOpcode(outerDone);
  appendOpcode(found);
  return result;
}

std::optional<CellRef> VQLCompiler::compileValueExpr(const ValueExpr &expr,
                                                     CellRef ctxCell) {
  return std::visit(
      [this, ctxCell](const auto &val) -> std::optional<CellRef> {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, ScalarLiteral>) {
          return std::visit(
              [this](const auto &lit) -> CellRef {
                return emitConstant(CellValue(lit));
              },
              val.value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          // VariableRef
          auto it = varBindings_.find(val);
          if (it != varBindings_.end()) {
            if (it->second) {
              const std::array<CellRef, 1> held{*it->second};
              refuseShapeCells(held, "a value here");
            }
            return it->second ? std::optional{emitFirstCell(*it->second)}
                              : std::nullopt;
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<PathExpression>>) {
          if (val) {
            const auto result = compilePathExpression(*val, ctxCell);
            return result ? std::optional{emitFirstCell(*result)}
                          : std::nullopt;
          }
          throw std::invalid_argument("null compiled path expression");
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<FunctionInvocation>>) {
          if (!val) return std::nullopt;
          if (const auto builtin = compileBuiltinInvocation(*val, ctxCell))
            return builtin;
          const auto definition = definitions_.find(val->name);
          if (definition == definitions_.end()) {
            if (isSemanticShapeBuiltin(val->name))
              return compileSemanticShapeValue(*val, ctxCell);
            if (isSemanticQuantifierBuiltin(val->name))
              return compileSemanticQuantifierValue(*val, ctxCell);
            if (isLensFunction(val->name))
              return compileLensValue(*val, ctxCell);
            if (isPathFunction(val->name))
              return compilePathFunctionValue(*val, ctxCell);
            throwUnresolvedFunction(*val, isKnownUnloweredFunction(val->name)
                                              ? QueryErrorCategory::Unsupported
                                              : QueryErrorCategory::Unknown);
          }
          requirePositionalArguments(*val);
          if (const auto shape = compileShapeCall(*val)) return shape;
          const auto results =
              compileCall(definition->second, val->args, ctxCell, false);
          return results.empty()
                     ? std::nullopt
                     : std::optional{emitFirstCell(results.front())};
        } else if constexpr (std::is_same_v<T, std::shared_ptr<
                                                   RuntimeClosureInvocation>>) {
          if (!val)
            throw std::invalid_argument("null runtime closure invocation");
          // `ctxCell` is sometimes a home-cell fallback for scalar
          // expressions; only the ambient optional carries caller context.
          const auto results = compileClosureInvocation(
              *val, ambientContext_, zigzag::vortex::ClosureCallMode::Explicit);
          return results.empty()
                     ? std::nullopt
                     : std::optional{emitFirstCell(results.front())};
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<BinaryValueExpr>>) {
          if (!val || !val->left || !val->right)
            throw std::invalid_argument("invalid binary value expression");
          if (functorArgument(*val->left) || functorArgument(*val->right))
            throw std::invalid_argument("VQL + does not accept path functors");

          const ShapeUseScope leftUse{*this, false, "an operand of '+'"};
          const auto leftValues    = compileArgument(*val->left, ctxCell);
          const CellRef leftStream = normalizeFrameStream(leftValues);
          const ShapeUseScope rightUse{*this, false, "an operand of '+'"};
          const auto rightValues    = compileArgument(*val->right, ctxCell);
          const CellRef rightStream = normalizeFrameStream(rightValues);
          for (const auto *operand : {&leftValues, &rightValues})
            for (const CellRef cell : *operand) {
              const auto kind = handleKindOf(cell);
              refuseHandleOperand(kind, "+");
            }
          for (const auto *operand : {&leftValues, &rightValues})
            rejectViewHandlesAtRunTime(
                *operand,
                "VQL a dynamic view handle cannot be an operand of '+': its "
                "text is a copy taken when the view was made, not a value the "
                "graph computes with");
          const CellRef add = emitOp(OpcodeKind::VqlAdd, "#VQL_ADD");
          emitInput(add, leftStream);
          emitInput(add, rightStream);
          const CellRef result = core_.arena().makeCell();
          emitOutput(add, result);
          const CellRef box = emitOp(OpcodeKind::VqlBoxScalar, "#BOX_SCALAR");
          emitInput(box, result);
          const CellRef boxed = core_.arena().makeCell();
          emitOutput(box, boxed);
          return boxed;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<BooleanExpr>>) {
          if (val) {
            return compileBooleanExpr(*val, ctxCell);
          }
          throw std::invalid_argument("null compiled boolean expression");
        } else if constexpr (std::is_same_v<T, std::shared_ptr<PathLambda>>) {
          if (!val) throw std::invalid_argument("null VQL closure lambda");
          return compileClosureLambda(*val);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ShapeLiteral>>) {
          if (!val) throw std::invalid_argument("null compiled shape literal");
          return compileShapeValue(*val, ctxCell);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<VersionExpr>>) {
          if (!val) throw std::invalid_argument("null compiled version value");
          const auto cell = compileVersionValue(*val, ctxCell);
          if (cell) {
            if (const auto text = core_.render(*cell);
                text && std::holds_alternative<std::string>(*text)) {
              try {
                versionVars_.insert_or_assign(
                    *cell, MicroversionId::parse(std::get<std::string>(*text)));
              } catch (const std::invalid_argument &) {
                // Not a version after all; it compares as the text it is.
              }
            }
          }
          return cell;
        } else {
          throw std::invalid_argument(
              "value expression requires Vortex graph lowering");
        }
      },
      expr.kind);
}

CellRef VQLCompiler::linkAsExecutable(CellRef entryOp) {
  // Link posward off home along +d.spin
  zigzag::expectWritten(
      core_.arena().link(core_.home(), core_.dims().spin, false, entryOp));
  // Spawn main execution cursor
  return vm_.spawnCursor(entryOp, "main");
}

void VQLCompiler::linkAsLibrary(CellRef entryOp, std::string_view moduleName,
                                std::string_view symbolName) {
  core_.exportSymbol(core_.getOrCreateModule(moduleName), symbolName, entryOp);
}

std::string VQLCompiler::disassemble(std::optional<CellRef> entryOp,
                                     std::size_t maxInstructions) const {
  if (!entryOp) return "(empty opcode stream)";

  std::ostringstream out;
  CellRef cur = *entryOp;
  std::unordered_set<CellRef> visited;

  while (maxInstructions-- > 0) {
    if (!visited.insert(cur).second) {
      out << "  ... (cycle detected to cell " << cur << ")\n";
      break;
    }

    auto kindOpt          = vm_.getOpcodeKind(cur);
    std::string_view mnem = kindOpt ? opcodeMnemonic(*kindOpt) : "UNKNOWN";

    std::string label = core_.arena().textOf(cur);

    out << "  [0x" << std::hex << std::setw(4) << std::setfill('0') << cur
        << std::dec << "] #" << std::left << std::setw(8) << std::setfill(' ')
        << mnem;

    if (!label.empty()) {
      out << " \"" << label << "\"";
    }

    // Inputs
    std::vector<CellRef> inCells = core_.inputsOf(cur);
    if (!inCells.empty()) {
      out << " in: [";
      for (std::size_t i = 0; i < inCells.size(); ++i) {
        if (i > 0) out << ", ";
        CellRef inC = inCells[i];
        out << "c#" << inC;
        std::string txt = core_.arena().textOf(inC);
        if (!txt.empty()) {
          out << "(\"" << txt << "\")";
        } else if (auto iVal = core_.arena().asInt64(inC)) {
          out << "(" << *iVal << ")";
        }
      }
      out << "]";
    }

    // Outputs
    std::vector<CellRef> outCells = core_.outputsOf(cur);
    if (!outCells.empty()) {
      out << " -> out: [";
      for (std::size_t i = 0; i < outCells.size(); ++i) {
        if (i > 0) out << ", ";
        out << "c#" << outCells[i];
      }
      out << "]";
    }

    out << "\n";

    const auto next = core_.arena().linked(cur, core_.dims().spin, false);
    if (!next) break;
    cur = *next;
  }

  return out.str();
}

} // namespace xanadu::vql
