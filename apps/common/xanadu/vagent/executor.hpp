#ifndef COMMON_XANADU_VAGENT_EXECUTOR_HPP
#define COMMON_XANADU_VAGENT_EXECUTOR_HPP

// The §3.2 executor callback (design/vagent-actor-executor.md): the
// adapter installs this on MeshActorRuntime::setExecutor, and each pump
// service becomes one actor turn — base choice point before m0, per-turn
// dewey reset, $turn bump, bounded d.seen journal, `on` handler dispatch
// through clauses_of/body_of/runFragment, then the commit or failure
// protocol with zero residuals. Statement-segmented bodies, beliefs,
// selection, intentions and the outbox drain are M2–M4; this skeleton
// runs the turn protocol and the event-dispatch seam.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "../vortex/mesh_actor.hpp"
#include "../vortex/worker_context.hpp"

namespace xanadu::vagent {

class VagentExecutor {
public:
  /// The same settings-layer read channel the runtime holds (§4.1) —
  /// `vagent.journal_size`/`vagent.compact_every` read through it; an
  /// absent reader falls back to the compiled-in defaults.
  explicit VagentExecutor(
      std::function<std::optional<std::int64_t>(std::string_view)>
          settingReader = {});

  /// One §3.2 turn. A turn fault returns ExecutionFailed — the pump maps
  /// it to ActorDown, which is what fail_step means mesh-side.
  [[nodiscard]] std::expected<std::string, zigzag::vortex::mesh::ActorError>
  operator()(const zigzag::vortex::mesh::ActorExecution &exec,
             zigzag::vortex::WorkerContext &ctx);

private:
  [[nodiscard]] std::int64_t setting(std::string_view key,
                                     std::int64_t fallback) const;

  std::function<std::optional<std::int64_t>(std::string_view)> settingReader_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_EXECUTOR_HPP
