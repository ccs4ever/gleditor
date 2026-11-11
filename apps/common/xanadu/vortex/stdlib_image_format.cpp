#include "common/xanadu/vortex/stdlib_image_format.hpp"

#include <array>
#include <string_view>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {
namespace {

struct OpcodeAbiEntry {
  OpcodeKind kind;
  std::string_view label;
};

constexpr std::array kOpcodeAbi{
    OpcodeAbiEntry{OpcodeKind::Add, "Add"},
    OpcodeAbiEntry{OpcodeKind::Sub, "Sub"},
    OpcodeAbiEntry{OpcodeKind::Mul, "Mul"},
    OpcodeAbiEntry{OpcodeKind::Div, "Div"},
    OpcodeAbiEntry{OpcodeKind::DivMod, "DivMod"},
    OpcodeAbiEntry{OpcodeKind::Mod, "Mod"},
    OpcodeAbiEntry{OpcodeKind::Neg, "Neg"},
    OpcodeAbiEntry{OpcodeKind::Eq, "Eq"},
    OpcodeAbiEntry{OpcodeKind::Neq, "Neq"},
    OpcodeAbiEntry{OpcodeKind::Lt, "Lt"},
    OpcodeAbiEntry{OpcodeKind::Lte, "Lte"},
    OpcodeAbiEntry{OpcodeKind::Gt, "Gt"},
    OpcodeAbiEntry{OpcodeKind::Gte, "Gte"},
    OpcodeAbiEntry{OpcodeKind::And, "And"},
    OpcodeAbiEntry{OpcodeKind::Or, "Or"},
    OpcodeAbiEntry{OpcodeKind::Not, "Not"},
    OpcodeAbiEntry{OpcodeKind::Link, "Link"},
    OpcodeAbiEntry{OpcodeKind::Break, "Break"},
    OpcodeAbiEntry{OpcodeKind::New, "New"},
    OpcodeAbiEntry{OpcodeKind::Value, "Value"},
    OpcodeAbiEntry{OpcodeKind::Splice, "Splice"},
    OpcodeAbiEntry{OpcodeKind::Clone, "Clone"},
    OpcodeAbiEntry{OpcodeKind::Jump, "Jump"},
    OpcodeAbiEntry{OpcodeKind::Branch, "Branch"},
    OpcodeAbiEntry{OpcodeKind::Call, "Call"},
    OpcodeAbiEntry{OpcodeKind::Return, "Return"},
    OpcodeAbiEntry{OpcodeKind::Halt, "Halt"},
    OpcodeAbiEntry{OpcodeKind::Nop, "Nop"},
    OpcodeAbiEntry{OpcodeKind::Bind, "Bind"},
    OpcodeAbiEntry{OpcodeKind::Resolve, "Resolve"},
    OpcodeAbiEntry{OpcodeKind::Assert, "Assert"},
    OpcodeAbiEntry{OpcodeKind::Abs, "Abs"},
    OpcodeAbiEntry{OpcodeKind::Min, "Min"},
    OpcodeAbiEntry{OpcodeKind::Max, "Max"},
    OpcodeAbiEntry{OpcodeKind::Clamp, "Clamp"},
    OpcodeAbiEntry{OpcodeKind::Trim, "Trim"},
    OpcodeAbiEntry{OpcodeKind::ToLower, "ToLower"},
    OpcodeAbiEntry{OpcodeKind::ToUpper, "ToUpper"},
    OpcodeAbiEntry{OpcodeKind::StrLen, "StrLen"},
    OpcodeAbiEntry{OpcodeKind::ByteAt, "ByteAt"},
    OpcodeAbiEntry{OpcodeKind::StrSlice, "StrSlice"},
    OpcodeAbiEntry{OpcodeKind::IntToStr, "IntToStr"},
    OpcodeAbiEntry{OpcodeKind::StrToInt, "StrToInt"},
    OpcodeAbiEntry{OpcodeKind::Unify, "Unify"},
    OpcodeAbiEntry{OpcodeKind::IsVar, "IsVar"},
    OpcodeAbiEntry{OpcodeKind::MakeVar, "MakeVar"},
    OpcodeAbiEntry{OpcodeKind::MakeTerm, "MakeTerm"},
    OpcodeAbiEntry{OpcodeKind::Deref, "Deref"},
    OpcodeAbiEntry{OpcodeKind::Choice, "Choice"},
    OpcodeAbiEntry{OpcodeKind::Fail, "Fail"},
    OpcodeAbiEntry{OpcodeKind::Cut, "Cut"},
    OpcodeAbiEntry{OpcodeKind::ChoiceDepth, "ChoiceDepth"},
    OpcodeAbiEntry{OpcodeKind::Linked, "Linked"},
    OpcodeAbiEntry{OpcodeKind::Find, "Find"},
    OpcodeAbiEntry{OpcodeKind::FieldSet, "FieldSet"},
    OpcodeAbiEntry{OpcodeKind::RunCall, "RunCall"},
    OpcodeAbiEntry{OpcodeKind::RunResume, "RunResume"},
    OpcodeAbiEntry{OpcodeKind::ScanPush, "ScanPush"},
    OpcodeAbiEntry{OpcodeKind::ScanPop, "ScanPop"},
    OpcodeAbiEntry{OpcodeKind::ScanTab, "ScanTab"},
    OpcodeAbiEntry{OpcodeKind::ScanMove, "ScanMove"},
    OpcodeAbiEntry{OpcodeKind::ScanMatch, "ScanMatch"},
    OpcodeAbiEntry{OpcodeKind::ScanMany, "ScanMany"},
    OpcodeAbiEntry{OpcodeKind::ScanUpto, "ScanUpto"},
    OpcodeAbiEntry{OpcodeKind::ScanAny, "ScanAny"},
    OpcodeAbiEntry{OpcodeKind::ScanFind, "ScanFind"},
    OpcodeAbiEntry{OpcodeKind::ScanPos, "ScanPos"},
    OpcodeAbiEntry{OpcodeKind::ScanSubject, "ScanSubject"},
};

// An enum insertion/reordering must not silently change the table's meaning.
static_assert(kOpcodeAbi.size() ==
              static_cast<std::size_t>(kLastOpcodeKind) + 1);
static_assert([] {
  for (std::size_t i = 0; i < kOpcodeAbi.size(); ++i)
    if (static_cast<std::size_t>(kOpcodeAbi[i].kind) != i ||
        kOpcodeAbi[i].label.empty())
      return false;
  return true;
}());

} // namespace

std::size_t stdlibImageOpcodeCount() noexcept { return kOpcodeAbi.size(); }

std::size_t stdlibImageVerbCount() noexcept {
  std::size_t count = 0;
  for (unsigned raw = 0; raw <= xanadu::structureVerbMask; ++raw)
    if (std::string_view{xanadu::structureVerbName(
            static_cast<xanadu::StructureVerb>(raw))} != "unknown")
      ++count;
  return count;
}

std::string stdlibImageAbiText() {
  std::string text{"vortex-stdlib-abi-v1\n"};
  for (const auto &[kind, label] : kOpcodeAbi) {
    text += "opcode\t" + std::to_string(static_cast<unsigned>(kind)) + "\t";
    text += label;
    text += '\n';
  }
  // Use the authoritative Structure verb names, including any future verb
  // added within the wire mask, instead of duplicating that label table.
  for (unsigned raw = 0; raw <= xanadu::structureVerbMask; ++raw) {
    const std::string_view label =
        xanadu::structureVerbName(static_cast<xanadu::StructureVerb>(raw));
    if (label == "unknown") continue;
    text += "verb\t" + std::to_string(raw) + "\t";
    text += label;
    text += '\n';
  }
  text += "last-content-opcode\t" +
          std::to_string(static_cast<unsigned>(kLastContentDecodableOpcode)) +
          "\nlast-opcode\t" +
          std::to_string(static_cast<unsigned>(kLastOpcodeKind)) + "\n";
  return text;
}

std::string stdlibImageEpoch() {
  return xanadu::sha256Hex(stdlibImageAbiText());
}

} // namespace zigzag::vortex
