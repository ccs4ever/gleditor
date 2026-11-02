/**
 * @file reconciler.hpp
 * @brief State reconciler for live agent upgrades between program versions
 *        (design/vagent-shared-program-and-live-upgrade.md §4.3).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_RECONCILER_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_RECONCILER_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace xanadu::vagent::runtime {

using StateReconciler = zigzag::vortex::mesh::MeshActorRuntime::StateReconciler;

struct ReconcileOptions {
  std::string fromVersion;
  std::string toVersion;
  std::string agentType;
  std::uint64_t maxInstructions{100'000};
};

struct ReconcileOutcome {
  std::size_t preservedFields{0};
  std::size_t addedFields{0};
  std::size_t droppedFields{0};
  std::size_t preservedBeliefs{0};
  std::size_t droppedBeliefs{0};
  std::size_t preservedIntentions{0};
  std::size_t abortedIntentions{0};
  std::vector<std::string> warnings;
  std::optional<CompiledAgent> compiledAgent;
};

/// Creates a StateReconciler callback suitable for MeshActorRuntime::upgrade
StateReconciler makeStateReconciler(const CompiledProgram &fromProgram,
                                    const CompiledProgram &toProgram,
                                    std::string agentType,
                                    ReconcileOutcome *outcome = nullptr);

StateReconciler makeStateReconciler(const CompiledProgram &fromProgram,
                                    const CompiledProgram &toProgram,
                                    ReconcileOptions options,
                                    ReconcileOutcome *outcome = nullptr);

std::expected<ReconcileOutcome, zigzag::vortex::mesh::ActorError>
reconcileState(const zigzag::vortex::WorkerContext &fromContext,
               zigzag::vortex::WorkerContext &toContext,
               const CompiledProgram &fromProgram,
               const CompiledProgram &toProgram, std::string_view agentType);

std::expected<ReconcileOutcome, zigzag::vortex::mesh::ActorError>
reconcileState(const zigzag::vortex::WorkerContext &fromContext,
               zigzag::vortex::WorkerContext &toContext,
               const CompiledProgram &fromProgram,
               const CompiledProgram &toProgram,
               const ReconcileOptions &options);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_RECONCILER_HPP
