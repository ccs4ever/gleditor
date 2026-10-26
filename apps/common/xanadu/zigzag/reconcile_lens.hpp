/**
 * @file reconcile_lens.hpp
 * @brief Autonomous reconciliation lens and control loop over declared vs
 * actual service states.
 *
 * Implements declarative state convergence (Milestone M4 Package S2):
 * - Compares desired service rank (d.desired) against actual running instances.
 * - Plans minimal start/stop/restart transition sequences.
 * - Drives external service lifecycle managers through abstract IOSDriver.
 */
#ifndef COMMON_XANADU_ZIGZAG_RECONCILE_LENS_HPP
#define COMMON_XANADU_ZIGZAG_RECONCILE_LENS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag {

/**
 * @struct ServiceConfig
 * @brief Declarative service configuration specification.
 */
struct ServiceConfig {
  std::string name;
  std::string command;
  std::optional<int> port;
  int replicas{1};

  bool operator==(const ServiceConfig &) const = default;
};

/**
 * @struct ServiceInstance
 * @brief Live physical running state of a service instance.
 */
struct ServiceInstance {
  std::string name;
  std::optional<int> pid;
  std::optional<int> port;
  bool active{false};
};

/**
 * @class IOSDriver
 * @brief OS interaction abstraction for physical side-effect management.
 */
class IOSDriver {
public:
  virtual ~IOSDriver()                                 = default;
  virtual int spawnProcess(const std::string &name,
                           const std::string &command) = 0;
  virtual void terminateProcess(int pid)               = 0;
  virtual bool bindPort(int port)                      = 0;
  virtual void unbindPort(int port)                    = 0;
};

/**
 * @class MockOSDriver
 * @brief In-memory mock tracking processes and ports to verify zero resource
 * leaks.
 */
class MockOSDriver : public IOSDriver {
public:
  int spawnProcess(const std::string &name,
                   const std::string &command) override {
    const int pid       = nextPid_++;
    liveProcesses_[pid] = name + ":" + command;
    return pid;
  }

  void terminateProcess(int pid) override { liveProcesses_.erase(pid); }

  bool bindPort(int port) override {
    if (boundPorts_.contains(port)) {
      return false;
    }
    boundPorts_.insert(port);
    return true;
  }

  void unbindPort(int port) override { boundPorts_.erase(port); }

  [[nodiscard]] std::size_t processCount() const noexcept {
    return liveProcesses_.size();
  }
  [[nodiscard]] std::size_t portCount() const noexcept {
    return boundPorts_.size();
  }
  [[nodiscard]] bool hasProcess(int pid) const noexcept {
    return liveProcesses_.contains(pid);
  }
  [[nodiscard]] bool hasPort(int port) const noexcept {
    return boundPorts_.contains(port);
  }

private:
  int nextPid_{1000};
  std::unordered_map<int, std::string> liveProcesses_;
  std::set<int> boundPorts_;
};

enum class ReconcileActionKind : std::uint8_t { Start, Stop, Rebind };

struct ReconcileAction {
  ReconcileActionKind kind{ReconcileActionKind::Start};
  std::string serviceName;
  std::string command;
  std::optional<int> port;
  std::optional<int> targetPid;
};

/**
 * @class ReconcileLens
 * @brief Declarative hypertime configuration reconciliation and rollback
 * engine.
 *
 * Implements Milestone M4 Package D3:
 * - Diff(d.actual_state, d.desired_config) -> ActionPlan
 * - Hypertime configuration rollback unwinding physical side-effects
 * (terminating processes, unbinding ports)
 * - Phased rollouts sequenced along d.phase with canary barriers
 */
class ReconcileLens {
public:
  ReconcileLens(std::shared_ptr<IOSDriver> driver);

  /**
   * @brief Sets the desired configuration rank from a set of service
   * specifications.
   */
  void setDesiredConfig(ArenaManifold &arena, DimRef dDesired,
                        const std::vector<ServiceConfig> &configs);

  /**
   * @brief Computes the delta between actual state and desired config.
   */
  [[nodiscard]] std::vector<ReconcileAction>
  planReconciliation(const std::vector<ServiceConfig> &desired);

  /**
   * @brief Executes the action plan through the OS driver and synchronizes
   * actual state.
   */
  std::size_t reconcile(const std::vector<ServiceConfig> &desired);

  /**
   * @brief Rollback: restores a historical configuration snapshot and
   * immediately unwinds all physical side effects that do not exist in the
   * target snapshot.
   */
  std::size_t rollbackTo(const std::vector<ServiceConfig> &targetSnapshot);

  /**
   * @brief Advances rollout phase along d.phase, gated by cryptographic canary
   * barrier.
   */
  bool advanceCanaryPhase(std::string_view canarySignature);

  [[nodiscard]] const std::unordered_map<std::string, ServiceInstance> &
  actualState() const noexcept {
    return liveServices_;
  }

  [[nodiscard]] std::shared_ptr<IOSDriver> driver() const noexcept {
    return driver_;
  }

private:
  std::shared_ptr<IOSDriver> driver_;
  std::unordered_map<std::string, ServiceInstance> liveServices_;
  std::uint32_t currentPhase_{0};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_RECONCILE_LENS_HPP
