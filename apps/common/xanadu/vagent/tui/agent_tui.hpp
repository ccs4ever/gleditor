/**
 * @file agent_tui.hpp
 * @brief Terminal User Interface dashboard for querying, monitoring, and
 * orchestrating vagent agents.
 */
#ifndef COMMON_XANADU_VAGENT_TUI_AGENT_TUI_HPP
#define COMMON_XANADU_VAGENT_TUI_AGENT_TUI_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace xanadu::vagent::tui {

enum class Pane { Monitoring, Querying, Orchestrating };

struct ActorRow {
  identity::ActorPid pid;
  std::string shortPid;
  std::string name;
  std::string agentType;
  std::string programRef;
  std::string status; // "RUNNING", "QUIESCENT", "STOPPED", "BUSY"
  std::uint64_t fuelRemaining{0};
  std::uint64_t cycleCount{0};
  std::size_t scratchCells{0};
  std::size_t epistemicCells{0};
  std::size_t mailboxDepth{0};
  std::size_t outboxCount{0};
  std::uint64_t statDropped{0};
  std::uint64_t statMalformed{0};
  std::uint64_t statDeliveryFailed{0};
  std::optional<std::int64_t> deadlineMs;
};

struct BeliefRecord {
  std::string predicate;
  std::string key;
  std::string source;
  std::string status; // "current", "retired", "stale", "fact"
  std::optional<double> acquiredUncertainty;
  double certaintyScore{1.0}; // 0.0 to 1.0 (epistemic arbitration score)
  std::optional<std::int64_t> incarnation;
  std::string details;
};

struct EpistemicScore {
  std::string predicate;
  std::string principal;
  std::string key;
  double uncertainty{0.0};
  double certaintyScore{1.0};
  std::string status;
  std::int64_t incarnation{0};
};

class AgentTui {
public:
  AgentTui(zigzag::vortex::mesh::MeshActorRuntime &runtime,
           runtime::AgentExecutor &executor,
           std::shared_ptr<NCursesActor> actor = nullptr);

  // Pane management
  void setPane(Pane pane);
  [[nodiscard]] Pane activePane() const noexcept { return activePane_; }

  // Actor selection
  void selectActor(const identity::ActorPid &pid);
  void selectNextActor();
  void selectPrevActor();
  [[nodiscard]] std::optional<identity::ActorPid> selectedActor() const;

  // Monitoring Pane
  std::vector<ActorRow> refreshActorTable();
  [[nodiscard]] const std::vector<ActorRow> &actorTable() const noexcept {
    return actorRows_;
  }

  // Querying Pane
  [[nodiscard]] std::vector<std::string> queryVlog(std::string_view query);
  [[nodiscard]] std::vector<BeliefRecord>
  inspectBeliefs(const identity::ActorPid &pid);
  [[nodiscard]] std::vector<EpistemicScore>
  epistemicScores(const identity::ActorPid &pid);

  // Orchestrating Pane
  [[nodiscard]] std::expected<identity::ActorPid, std::string>
  spawn(std::string_view agentType, std::string_view name,
        runtime::WireRecord args = {});

  [[nodiscard]] std::expected<void, std::string>
  injectPercept(const identity::ActorPid &pid, std::string_view eventType,
                runtime::WireRecord fields = {});

  [[nodiscard]] std::expected<void, std::string>
  injectEvent(const identity::ActorPid &pid, std::string_view eventType,
              runtime::WireRecord fields = {});

  std::size_t step(std::size_t turns = 1);
  std::size_t advanceClock(std::chrono::milliseconds ms);

  [[nodiscard]] std::expected<void, std::string>
  upgrade(const identity::ActorPid &pid, std::string_view newProgramRef);

  [[nodiscard]] std::expected<void, std::string>
  stop(const identity::ActorPid &pid);

  [[nodiscard]] std::expected<std::string, std::string>
  executeCommand(std::string_view cmdLine);

  // User input & rendering
  void handleInput(const KeyEvent &event);
  void render();
  void tick();

  [[nodiscard]] NCursesActor &actor() noexcept { return *actor_; }
  [[nodiscard]] const NCursesActor &actor() const noexcept { return *actor_; }

  [[nodiscard]] const std::vector<std::string> &actionLog() const noexcept {
    return actionLog_;
  }
  [[nodiscard]] const std::vector<std::string> &queryResults() const noexcept {
    return queryResults_;
  }
  [[nodiscard]] const std::vector<BeliefRecord> &
  inspectedBeliefs() const noexcept {
    return cachedBeliefs_;
  }
  [[nodiscard]] const std::vector<EpistemicScore> &
  cachedEpistemicScores() const noexcept {
    return cachedScores_;
  }

  [[nodiscard]] bool shouldExit() const noexcept { return shouldExit_; }
  void requestExit() noexcept { shouldExit_ = true; }

private:
  [[nodiscard]] std::optional<identity::ActorPid>
  resolveTarget(std::string_view targetStr) const;

  void renderHeader();
  void renderMonitoringPane(int top, int bottom);
  void renderQueryingPane(int top, int bottom);
  void renderOrchestratingPane(int top, int bottom);
  void renderStatusBar();

  zigzag::vortex::mesh::MeshActorRuntime &runtime_;
  runtime::AgentExecutor &executor_;
  std::shared_ptr<NCursesActor> actor_;

  Pane activePane_{Pane::Monitoring};
  std::size_t selectedIndex_{0};
  std::vector<ActorRow> actorRows_;
  std::optional<identity::ActorPid> selectedPid_;

  // Querying state
  std::string queryInput_;
  std::vector<std::string> queryResults_;
  std::vector<BeliefRecord> cachedBeliefs_;
  std::vector<EpistemicScore> cachedScores_;

  // Orchestrating state
  std::string commandInput_;
  std::vector<std::string> actionLog_;

  std::size_t totalTurns_{0};
  bool shouldExit_{false};
};

} // namespace xanadu::vagent::tui

#endif // COMMON_XANADU_VAGENT_TUI_AGENT_TUI_HPP
