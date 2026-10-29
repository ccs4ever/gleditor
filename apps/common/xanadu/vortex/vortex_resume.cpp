/**
 * @file vortex_resume.cpp
 * @brief Per-cursor resume-state registers used by compiled goal graphs.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "common/xanadu/zigzag/arena_savepoint.hpp"

namespace zigzag::vortex {
namespace {

bool isResumeScalar(const CellValue &value) {
  return std::holds_alternative<std::int64_t>(value) ||
         std::holds_alternative<bool>(value);
}

ExecutionResult resumeFailure(const CellRef opcode, std::string message) {
  return {.success       = false,
          .failingClause = opcode,
          .errorMessage  = std::move(message)};
}

} // namespace

ExecutionResult
VortexVM::executeResumeInit(const CellRef opcode,
                            const std::vector<CellValue> &inputs,
                            const std::optional<CellRef> cursor) {
  const auto inputSlots  = core_.inputsOf(opcode);
  const auto outputSlots = core_.outputsOf(opcode);
  if (!cursor) return resumeFailure(opcode, "ResumeInit requires a cursor");
  if (inputSlots.size() != 1 || inputs.size() != 1 || outputSlots.size() != 1)
    return resumeFailure(opcode, "ResumeInit expects one input and one output");
  if (!isResumeScalar(inputs.front()))
    return resumeFailure(opcode, "resume-state values must be Int64 or Bool");

  const CellRef outputSlot = outputSlots.front();
  if (!core_.arena().contains(outputSlot) ||
      streamSlots_.contains(outputSlot) ||
      iteratorSlots_.contains(outputSlot)) {
    return resumeFailure(opcode,
                         "ResumeInit output must be a scalar arena slot");
  }
  core_.arena().consumeWork();
  for (const auto &[id, state] : resumeValues_) {
    std::ignore = id;
    if (state.outputSlot == outputSlot && state.cursor != *cursor)
      return resumeFailure(
          opcode, "resume-state output is owned by another active cursor");
  }
  if (nextResumeTokenId_ == std::numeric_limits<std::uint64_t>::max())
    return resumeFailure(opcode, "resume-state token id space exhausted");

  const std::uint64_t id = nextResumeTokenId_++;
  const auto previous    = cellReferences_.find(outputSlot);
  const bool hadPrevious = previous != cellReferences_.end();
  const std::optional<CellRef> previousTarget =
      hadPrevious ? previous->second : std::nullopt;
  zigzag::ArenaSavepoint savepoint(core_.arena());
  std::optional<CellRef> tokenCell;
  bool stateInserted   = false;
  bool tokenIdInserted = false;
  try {
    tokenCell = core_.arena().makeCell();
    zigzag::expectWritten(core_.arena().setValueBits(
        *tokenCell, xanadu::ValueKind::ExternRef, id));
    const std::vector<CellValue> reference{
        static_cast<std::int64_t>(*tokenCell)};
    writeCellReferences(outputSlots, reference);
    stateInserted = resumeValues_
                        .emplace(id, ResumeValue{.tokenCell  = *tokenCell,
                                                 .outputSlot = outputSlot,
                                                 .cursor     = *cursor,
                                                 .value      = inputs.front()})
                        .second;
    if (!stateInserted)
      throw std::logic_error("duplicate resume-state token id");
    tokenIdInserted = resumeTokenIds_.emplace(*tokenCell, id).second;
    if (!tokenIdInserted)
      throw std::logic_error("duplicate resume-state token cell");
    savepoint.commit();
    pruneResumeState();
    return {};
  } catch (const std::exception &error) {
    if (tokenCell) resumeTokenIds_.erase(*tokenCell);
    if (stateInserted) resumeValues_.erase(id);
    if (hadPrevious) {
      cellReferences_[outputSlot] = previousTarget;
    } else {
      cellReferences_.erase(outputSlot);
    }
    savepoint.rollback();
    return resumeFailure(opcode, error.what());
  }
}

ExecutionResult
VortexVM::executeResumeSet(const CellRef opcode,
                           const std::vector<CellValue> &inputs,
                           const std::optional<CellRef> cursor) {
  const auto inputSlots = core_.inputsOf(opcode);
  if (!cursor) return resumeFailure(opcode, "ResumeSet requires a cursor");
  if (inputSlots.size() != 2 || inputs.size() != 2 ||
      !core_.outputsOf(opcode).empty()) {
    return resumeFailure(opcode, "ResumeSet expects two inputs and no outputs");
  }
  if (!isResumeScalar(inputs[1]))
    return resumeFailure(opcode, "resume-state values must be Int64 or Bool");

  const auto reference = cellReferences_.find(inputSlots[0]);
  if (reference == cellReferences_.end() || !reference->second)
    return resumeFailure(opcode, "ResumeSet token reference is missing");
  const CellRef tokenCell = *reference->second;
  const auto tokenId      = resumeTokenIds_.find(tokenCell);
  if (tokenId == resumeTokenIds_.end())
    return resumeFailure(opcode, "ResumeSet input is not a resume token");
  const auto state = resumeValues_.find(tokenId->second);
  if (state == resumeValues_.end() || state->second.tokenCell != tokenCell ||
      !core_.arena().contains(tokenCell)) {
    return resumeFailure(opcode, "ResumeSet token is no longer active");
  }
  const auto slot = core_.arena().slot(tokenCell);
  if (!slot ||
      slot->valueKind !=
          static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef) ||
      slot->valueBits != tokenId->second) {
    return resumeFailure(opcode, "ResumeSet token identity is invalid");
  }
  if (state->second.cursor != *cursor)
    return resumeFailure(opcode, "ResumeSet token belongs to another cursor");

  core_.arena().consumeWork();
  state->second.value = inputs[1];
  return {};
}

std::optional<CellValue>
VortexVM::resumeValueForInput(const CellRef input) const {
  if (!activeCursor_) return std::nullopt;
  if (const auto direct = resumeTokenIds_.find(input);
      direct != resumeTokenIds_.end()) {
    return resumeValueForToken(input, direct->second);
  }
  const auto reference = cellReferences_.find(input);
  if (reference == cellReferences_.end()) return std::nullopt;
  if (!reference->second) {
    return std::nullopt;
  }
  const CellRef tokenCell = *reference->second;
  const auto tokenId      = resumeTokenIds_.find(tokenCell);
  if (tokenId == resumeTokenIds_.end()) return std::nullopt;
  return resumeValueForToken(tokenCell, tokenId->second);
}

std::optional<CellValue>
VortexVM::resumeValueForToken(const CellRef tokenCell,
                              const std::uint64_t tokenId) const {
  if (!activeCursor_) return std::nullopt;
  if (!core_.arena().contains(tokenCell))
    throw std::runtime_error("resume-state token cell is no longer present");
  const auto slot = core_.arena().slot(tokenCell);
  if (!slot ||
      slot->valueKind !=
          static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef) ||
      slot->valueBits != tokenId) {
    throw std::runtime_error("resume-state token identity is invalid");
  }
  const auto state = resumeValues_.find(tokenId);
  if (state == resumeValues_.end() || state->second.tokenCell != tokenCell)
    throw std::runtime_error("resume-state token is no longer active");
  if (state->second.cursor != *activeCursor_)
    throw std::runtime_error("resume-state token belongs to another cursor");
  return state->second.value;
}

void VortexVM::clearResumeState(const CellRef cursor) noexcept {
  // Keep the ExternRef identity in registers and choice snapshots. Removing
  // its value row makes any later read fail explicitly instead of rendering
  // the VM's missing-cell sentinel as an ordinary integer.
  std::erase_if(resumeValues_, [cursor](const auto &entry) {
    return entry.second.cursor == cursor;
  });
  pruneResumeState();
}

void VortexVM::pruneResumeState() noexcept {
  std::erase_if(resumeTokenIds_, [this](const auto &entry) {
    const auto cell = core_.arena().slot(entry.first);
    return !cell ||
           cell->valueKind !=
               static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef) ||
           cell->valueBits != entry.second;
  });
  std::erase_if(resumeValues_, [this](const auto &entry) {
    const auto id = resumeTokenIds_.find(entry.second.tokenCell);
    return !core_.arena().contains(entry.second.tokenCell) ||
           id == resumeTokenIds_.end() || id->second != entry.first;
  });
}

bool VortexVM::hasResumeState(const CellRef cursor) const noexcept {
  return std::ranges::any_of(resumeValues_, [cursor](const auto &entry) {
    return entry.second.cursor == cursor;
  });
}

bool VortexVM::hasResumeState() const noexcept {
  return !resumeValues_.empty();
}

} // namespace zigzag::vortex
