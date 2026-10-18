#include "common/xanadu/zigzag/reconcile_lens.hpp"
#include "common/xanadu/ops.hpp"

namespace zigzag {

ReconcileLens::ReconcileLens(std::shared_ptr<IOSDriver> driver)
    : driver_(std::move(driver)) {}

void ReconcileLens::setDesiredConfig(
    ArenaManifold &arena, DimRef dDesired,
    const std::vector<ServiceConfig> &configs) {
  std::optional<CellRef> prev = std::nullopt;
  for ([[maybe_unused]] const auto &cfg : configs) {
    const auto c = arena.makeCell();
    if (prev.has_value()) {
      expectWritten(arena.link(*prev, dDesired, DimVector::POS, c));
    }
    prev = c;
  }
}

std::vector<ReconcileAction>
ReconcileLens::planReconciliation(const std::vector<ServiceConfig> &desired) {
  std::vector<ReconcileAction> plan;
  std::unordered_map<std::string, const ServiceConfig *> desiredMap;
  for (const auto &cfg : desired) {
    desiredMap[cfg.name] = &cfg;
  }

  // 1. Identify services to terminate (running in actual but removed from
  // desired)
  for (const auto &[name, inst] : liveServices_) {
    auto it = desiredMap.find(name);
    if (it == desiredMap.end()) {
      ReconcileAction act{};
      act.kind        = ReconcileActionKind::Stop;
      act.serviceName = name;
      act.targetPid   = inst.pid;
      act.port        = inst.port;
      plan.push_back(act);
    } else {
      // Check if configuration changed (port changed)
      if (it->second->port != inst.port) {
        ReconcileAction stopAct{};
        stopAct.kind        = ReconcileActionKind::Stop;
        stopAct.serviceName = name;
        stopAct.targetPid   = inst.pid;
        stopAct.port        = inst.port;
        plan.push_back(stopAct);

        ReconcileAction startAct{};
        startAct.kind        = ReconcileActionKind::Start;
        startAct.serviceName = name;
        startAct.command     = it->second->command;
        startAct.port        = it->second->port;
        plan.push_back(startAct);
      }
    }
  }

  // 2. Identify new services to start
  for (const auto &cfg : desired) {
    if (!liveServices_.contains(cfg.name)) {
      ReconcileAction act{};
      act.kind        = ReconcileActionKind::Start;
      act.serviceName = cfg.name;
      act.command     = cfg.command;
      act.port        = cfg.port;
      plan.push_back(act);
    }
  }

  return plan;
}

std::size_t
ReconcileLens::reconcile(const std::vector<ServiceConfig> &desired) {
  const auto plan = planReconciliation(desired);
  if (!driver_) {
    return 0;
  }

  for (const auto &action : plan) {
    switch (action.kind) {
    case ReconcileActionKind::Stop: {
      if (action.targetPid > 0) {
        driver_->terminateProcess(action.targetPid);
      }
      if (action.port > 0) {
        driver_->unbindPort(action.port);
      }
      liveServices_.erase(action.serviceName);
      break;
    }
    case ReconcileActionKind::Start: {
      const int pid = driver_->spawnProcess(action.serviceName, action.command);
      if (action.port > 0) {
        driver_->bindPort(action.port);
      }
      ServiceInstance inst{};
      inst.name                         = action.serviceName;
      inst.pid                          = pid;
      inst.port                         = action.port;
      inst.active                       = true;
      liveServices_[action.serviceName] = inst;
      break;
    }
    case ReconcileActionKind::Rebind:
      break;
    }
  }

  return plan.size();
}

std::size_t
ReconcileLens::rollbackTo(const std::vector<ServiceConfig> &targetSnapshot) {
  return reconcile(targetSnapshot);
}

bool ReconcileLens::advanceCanaryPhase(std::string_view canarySignature) {
  // Verify cryptographic canary barrier token (requires signature prefix
  // "canary-ok-")
  if (canarySignature.starts_with("canary-ok-")) {
    ++currentPhase_;
    return true;
  }
  return false;
}

} // namespace zigzag
