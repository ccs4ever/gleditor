/**
 * @file lower_context.cpp
 * @brief The shared lowering helpers (card L0): emission, scopes, tables, and
 *        the run-time error encoding. Nothing here is card-specific.
 */
#include "common/xanadu/vlace/lower_context.hpp"

#include <algorithm>
#include <charconv>
#include <format>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vlace::lower {

using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

namespace {

/// Marks an Assert message as a VLace failure rather than a VM one.
constexpr std::string_view failureTag = "VLACE";
constexpr char failureSeparator       = '|';

/// The text of LoweringBudgetExceeded, so the reworder recognises it.
constexpr std::string_view loweringBudgetText =
    "VLace lowering cell budget exhausted";

/// The VM texts that mean the budget ran out, one per budget the VM keeps.
constexpr std::string_view budgetTexts[] = {
    "activation frame depth limit exceeded",
    "Exceeded max cycles",
    "Vortex evaluation work budget exhausted",
    loweringBudgetText,
};

std::optional<CompileError> codeFromName(std::string_view name) {
  constexpr auto first = static_cast<std::uint8_t>(CompileError::LexicalError);
  constexpr auto last =
      static_cast<std::uint8_t>(CompileError::BudgetExhausted);
  for (std::uint8_t c = first; c <= last; ++c) {
    const auto code = static_cast<CompileError>(c);
    if (compileErrorName(code) == name) return code;
  }
  return std::nullopt;
}

std::optional<std::size_t> parseCount(std::string_view text) {
  std::size_t value{};
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

} // namespace

// -- CtorTable ----------------------------------------------------------------

CtorTable *CtorTable::declare(CtorInfo info) {
  for (const auto &field : info.fields) fieldOwners_[field] = info.name;
  ctors_[info.name] = std::move(info);
  return this;
}

std::optional<CtorInfo> CtorTable::find(std::string_view ctor) const {
  const auto it = ctors_.find(std::string(ctor));
  if (it == ctors_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> CtorTable::fieldOwner(std::string_view field) const {
  const auto it = fieldOwners_.find(std::string(field));
  if (it == fieldOwners_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::size_t> CtorTable::fieldIndex(std::string_view ctor,
                                                 std::string_view field) const {
  const auto info = find(ctor);
  if (!info) return std::nullopt;
  for (std::size_t i = 0; i < info->fields.size(); ++i) {
    if (info->fields[i] == field) return i;
  }
  return std::nullopt;
}

// -- LowerState ---------------------------------------------------------------

const LowerState::KernelDims &
LowerState::kernelDims(zigzag::vortex::VortexCore &core) {
  if (!kernelDims_) {
    kernelDims_ =
        KernelDims{.thunk     = core.findOrMintDimension("d.thunk"),
                   .memo      = core.findOrMintDimension("d.memo"),
                   .blackhole = core.findOrMintDimension("d.blackhole")};
  }
  return *kernelDims_;
}

// -- LowerContext -------------------------------------------------------------

LowerContext::LowerContext(Compiler &compiler)
    : compiler_(compiler), core_(compiler.core()), vm_(compiler.vm()),
      state_(compiler.lowerState()) {
  scopes_.push_back(Scope{.kind = ScopeKind::Frame, .names = {}});
}

void LowerContext::chargeCells(std::size_t count) {
  generatedCells_ += count;
  if (generatedCells_ > maxGeneratedCells) {
    throw LoweringBudgetExceeded(*rewordBudgetFailure(loweringBudgetText));
  }
}

CellRef LowerContext::emitOp(OpcodeKind kind, std::string_view label) {
  chargeCells(1);
  const CellRef op = vm_.mintOpcode(kind, label);
  placeLabel(op);
  return op;
}

CellRef LowerContext::mintLabel(std::string_view name) {
  chargeCells(1);
  return vm_.mintOpcode(OpcodeKind::Nop, name);
}

void LowerContext::placeLabel(CellRef label) {
  if (!block_.entry) block_.entry = label;
  if (block_.tail) {
    zigzag::expectWritten(
        core_.arena().link(*block_.tail, core_.dims().spin, false, label));
  }
  block_.tail = label;
}

CellRef LowerContext::branchIfFalse(Operand cond, CellRef label) {
  const CellRef branch = emitOp(OpcodeKind::BranchFalseCell, "#vlace_branch");
  emitInput(branch, cond.cell);
  emitInput(branch, label);
  return branch;
}

CellRef LowerContext::jumpTo(CellRef label) {
  return branchIfFalse(emitConstant(CellValue{false}), label);
}

void LowerContext::emitInput(CellRef op, CellRef operand) {
  core_.bindInput(op, operand);
}

void LowerContext::emitOutput(CellRef op, CellRef target) {
  core_.bindOutput(op, target);
  block_.locals.push_back(target);
}

Operand LowerContext::emitConstant(const CellValue &value) {
  chargeCells(1);
  const CellRef cell = core_.arena().makeCell();
  // Offset 0 with no length replaces the cell's whole value.
  core_.writeValue(cell, 0, std::nullopt, value);
  return Operand(cell, Evaluation::Whnf);
}

Operand LowerContext::newRegister() {
  chargeCells(1);
  const CellRef cell = core_.arena().makeCell();
  block_.locals.push_back(cell);
  return Operand(cell, Evaluation::Unknown);
}

Block LowerContext::swapBlock(Block next) noexcept {
  return std::exchange(block_, std::move(next));
}

CellRef LowerContext::emitRuntimeFailure(CompileError code, SourceLocation loc,
                                         std::string_view detail) {
  const CellRef op = emitOp(OpcodeKind::Assert, "#vlace_fail");
  emitInput(op, emitConstant(CellValue{false}).cell);
  emitInput(
      op,
      emitConstant(CellValue{encodeRuntimeFailure(code, loc, detail)}).cell);
  return op;
}

LowerContext::ScopeGuard LowerContext::pushScope(ScopeKind kind) {
  scopes_.push_back(Scope{.kind = kind, .names = {}});
  return ScopeGuard(this);
}

void LowerContext::popScope() noexcept { scopes_.pop_back(); }

LowerContext *LowerContext::addCapture(std::string name, Operand outer,
                                       Operand inner) {
  auto frame = std::ranges::find_if(
      scopes_.rbegin(), scopes_.rend(),
      [](const Scope &s) { return s.kind == ScopeKind::Frame; });
  frame->names.insert_or_assign(name, inner);
  frame->captures.push_back(
      Capture{.name = std::move(name), .outer = outer, .inner = inner});
  return this;
}

std::span<const LowerContext::Capture> LowerContext::captures() const {
  const auto frame = std::ranges::find_if(
      scopes_.rbegin(), scopes_.rend(),
      [](const Scope &s) { return s.kind == ScopeKind::Frame; });
  return frame->captures;
}

LowerContext *LowerContext::bind(std::string name, Operand operand) {
  scopes_.back().names.insert_or_assign(std::move(name), operand);
  return this;
}

std::optional<ScopeHit> LowerContext::lookup(std::string_view name) const {
  const std::string key(name);
  std::size_t frames = 0;
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    const auto hit = it->names.find(key);
    if (hit != it->names.end())
      return ScopeHit{.operand = hit->second, .frameDepth = frames};
    if (it->kind == ScopeKind::Frame) ++frames;
  }
  return std::nullopt;
}

// -- Errors -------------------------------------------------------------------

std::unexpected<EvalError> fail(CompileError code, std::string detail,
                                SourceLocation loc) {
  return std::unexpected(
      EvalError{.code = code, .detail = std::move(detail), .loc = loc});
}

std::unexpected<EvalError> notLowered(std::string_view what,
                                      SourceLocation loc) {
  return fail(CompileError::UnexpectedToken,
              std::format("VM lowering is not implemented for VLace {}", what),
              loc);
}

std::string encodeRuntimeFailure(CompileError code, SourceLocation loc,
                                 std::string_view detail) {
  return std::format("{}{}{}{}{}:{}{}{}", failureTag, failureSeparator,
                     compileErrorName(code), failureSeparator, loc.line,
                     loc.column, failureSeparator, detail);
}

std::optional<EvalError> decodeRuntimeFailure(std::string_view message) {
  // The detail is last and may itself contain the separator, so split only the
  // first three fields.
  const auto field = [&message]() -> std::optional<std::string_view> {
    const auto at = message.find(failureSeparator);
    if (at == std::string_view::npos) return std::nullopt;
    const auto head = message.substr(0, at);
    message.remove_prefix(at + 1);
    return head;
  };
  const auto tag = field();
  if (!tag || *tag != failureTag) return std::nullopt;
  const auto name = field();
  const auto pos  = field();
  if (!name || !pos) return std::nullopt;
  const auto code  = codeFromName(*name);
  const auto colon = pos->find(':');
  if (!code || colon == std::string_view::npos) return std::nullopt;
  const auto line   = parseCount(pos->substr(0, colon));
  const auto column = parseCount(pos->substr(colon + 1));
  if (!line || !column) return std::nullopt;
  return EvalError{.code   = *code,
                   .detail = std::string(message),
                   .loc    = SourceLocation{.line = *line, .column = *column}};
}

std::optional<EvalError> rewordBudgetFailure(std::string_view vmMessage) {
  for (const auto text : budgetTexts) {
    if (vmMessage.find(text) != std::string_view::npos) {
      return EvalError{
          .code   = CompileError::BudgetExhausted,
          .detail = std::format("Vlace evaluation budget exhausted: {}", text),
          .loc    = {}};
    }
  }
  return std::nullopt;
}

EvalError failureToEval(const zigzag::vortex::ExecutionResult &r) {
  if (auto encoded = decodeRuntimeFailure(r.errorMessage)) return *encoded;
  if (auto budget = rewordBudgetFailure(r.errorMessage)) return *budget;
  return EvalError{.code = CompileError::UnexpectedToken,
                   .detail =
                       std::format("vlace: VM failure: {}", r.errorMessage),
                   .loc = {}};
}

} // namespace xanadu::vlace::lower
