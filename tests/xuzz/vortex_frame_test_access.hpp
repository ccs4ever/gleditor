// The one door the frame tests use into VortexVM's private tables and its frame
// failpoint (VortexVM names FrameTestAccess as a friend). Include from tests
// only; it is defined once, in namespace zigzag::vortex, so each test binary
// links one definition.
#ifndef TESTS_XUZZ_VORTEX_FRAME_TEST_ACCESS_HPP
#define TESTS_XUZZ_VORTEX_FRAME_TEST_ACCESS_HPP

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {

/// Named in VortexVM as a friend: the one door tests use to read and seed the
/// VM's private tables and to arm the frame failpoint.
struct FrameTestAccess {
  using Sections = std::vector<std::pair<std::string, std::string>>;

  template <class Map, class Value>
  static std::string dump(const Map &map, Value &&describe) {
    std::vector<std::pair<CellRef, std::string>> rows;
    for (const auto &[key, value] : map)
      rows.emplace_back(key, describe(value));
    std::ranges::sort(rows);
    std::ostringstream out;
    for (const auto &[key, text] : rows) out << key << '=' << text << ';';
    return out.str();
  }
  static std::string dumpSet(const std::unordered_set<CellRef> &set) {
    std::vector<CellRef> keys(set.begin(), set.end());
    std::ranges::sort(keys);
    std::ostringstream out;
    for (const auto key : keys) out << key << ';';
    return out.str();
  }
  template <class Pointer> static std::string identity(const Pointer &pointer) {
    std::ostringstream out;
    out << static_cast<const void *>(pointer.get());
    return out.str();
  }

  static Sections sections(const VortexVM &vm) {
    Sections result;
    const auto optionalCell = [](const std::optional<CellRef> &cell) {
      return cell ? std::to_string(*cell) : std::string("-");
    };
    result.emplace_back("cellReferences",
                        dump(vm.cellReferences_, optionalCell));
    result.emplace_back("streams", dump(vm.streams_, [](const auto &stream) {
                          std::ostringstream out;
                          out << stream.get() << '[';
                          for (const auto cell : *stream) out << cell << ',';
                          out << ']';
                          return out.str();
                        }));
    result.emplace_back("iterators", dump(vm.iterators_, [](const auto &p) {
                          return identity(p);
                        }));
    result.emplace_back(
        "groupIterators",
        dump(vm.groupIterators_, [](const auto &p) { return identity(p); }));
    result.emplace_back("groupOwners", dump(vm.groupOwners_, [](const auto &w) {
                          return identity(w.lock());
                        }));
    result.emplace_back("streamSlots", dumpSet(vm.streamSlots_));
    result.emplace_back("iteratorSlots", dumpSet(vm.iteratorSlots_));
    result.emplace_back("groupIteratorSlots", dumpSet(vm.groupIteratorSlots_));
    result.emplace_back("activationFrames",
                        dump(vm.activationFrames_, [](const auto &stack) {
                          std::ostringstream out;
                          for (const auto &frame : stack)
                            out << frame.get() << ',';
                          return out.str();
                        }));
    result.emplace_back("cursorPC", dump(vm.cursorPC_, optionalCell));
    result.emplace_back("guardStacks", dump(vm.guardStacks_, [](const auto &s) {
                          std::ostringstream out;
                          out << s.get() << ':' << (s ? s->size() : 0);
                          return out.str();
                        }));
    result.emplace_back("guardScopes", dump(vm.guardScopes_, [](const auto &s) {
                          std::ostringstream out;
                          out << s.get() << ':' << (s ? s->size() : 0);
                          return out.str();
                        }));
    std::ostringstream scalars;
    scalars << vm.frameBackingCellsAllocated_ << '/' << vm.nextActivationId_
            << '/' << optionalCell(vm.streamCursor_) << '/'
            << vm.retainedGroupsReferences();
    result.emplace_back("scalars", scalars.str());
    return result;
  }

  static std::string digest(const VortexVM &vm) {
    std::string text;
    for (const auto &[name, body] : sections(vm))
      text += name + '|' + body + '\n';
    return text;
  }

  static void arm(VortexVM &vm, const std::optional<std::size_t> nth) {
    vm.frameProbe_.arm(nth);
  }

  static std::shared_ptr<const ActivationFrame> top(const VortexVM &vm,
                                                    const CellRef cursor) {
    const auto found = vm.activationFrames_.find(cursor);
    if (found == vm.activationFrames_.end() || found->second.empty())
      return nullptr;
    return found->second.back();
  }

  /// A recognisable entry for @p slot in every table that holds per-slot state.
  static void seedRegisters(VortexVM &vm, const CellRef slot,
                            const std::shared_ptr<const ActivationFrame> &owner,
                            const bool withGroupOwner) {
    vm.cellReferences_[slot] = slot;
    vm.streams_[slot]        = std::make_shared<std::vector<CellRef>>(1, slot);
    vm.iterators_[slot] =
        std::make_shared<IteratorState>(SequenceIteratorState{});
    vm.groupIterators_[slot] =
        std::make_shared<GroupIteratorState>(nullptr, nullptr, 0, slot, slot);
    if (withGroupOwner) vm.groupOwners_[slot] = owner;
    vm.streamSlots_.insert(slot);
    vm.iteratorSlots_.insert(slot);
    vm.groupIteratorSlots_.insert(slot);
  }

  static void seedStaleReference(VortexVM &vm, const CellRef slot) {
    vm.cellReferences_[slot] = slot;
  }
  static void seedStaleStreamArgument(VortexVM &vm, const CellRef slot) {
    vm.streams_[slot] = std::make_shared<std::vector<CellRef>>(1, slot);
    vm.streamSlots_.insert(slot);
  }

  /// A guard and a guard scope owned by the top activation, which ReturnFrame
  /// must trim from the caller-visible stack and scope list.
  static void seedGuard(VortexVM &vm, const CellRef cursor) {
    const auto frame = top(vm, cursor);
    const auto owner =
        frame ? std::optional{frame->activationId} : std::nullopt;
    auto guards = std::make_shared<std::vector<RuntimeGuardRef>>();
    guards->push_back(std::make_shared<const RuntimeGuard>(
        cursor, cursor, "seeded", std::vector<std::vector<CellRef>>{}, owner));
    vm.guardStacks_[cursor] = guards;
    auto scopes             = std::make_shared<std::vector<GuardScope>>();
    scopes->emplace_back(cursor, 0, owner);
    vm.guardScopes_[cursor] = scopes;
  }

  /// @p count entries in cellReferences_ and guardScopes_ that belong to no
  /// frame, cursor or cell the calls under test use.
  static void seedUnrelated(VortexVM &vm, const std::size_t count,
                            const CellRef firstKey) {
    for (std::size_t index = 0; index < count; ++index) {
      const auto key          = firstKey + static_cast<CellRef>(index);
      vm.cellReferences_[key] = key;
      vm.guardScopes_[key]    = std::make_shared<std::vector<GuardScope>>();
    }
  }

  static std::vector<CellRef> destinations(const VortexVM &vm,
                                           const CellRef cursor) {
    const auto frame = top(vm, cursor);
    return frame ? frame->parentResultSlots : std::vector<CellRef>{};
  }
  static std::vector<CellRef> seedableBackings(const VortexVM &vm,
                                               const CellRef cursor) {
    std::vector<CellRef> result;
    const auto frame = top(vm, cursor);
    if (!frame) return result;
    for (const auto &[logical, backing] : frame->localBacking)
      if (std::ranges::none_of(frame->resultPorts, [&](const FramePort &port) {
            return port.slot == logical;
          }))
        result.push_back(backing);
    return result;
  }
};

} // namespace zigzag::vortex

#endif // TESTS_XUZZ_VORTEX_FRAME_TEST_ACCESS_HPP
