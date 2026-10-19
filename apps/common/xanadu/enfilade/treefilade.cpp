#include "common/xanadu/enfilade/treefilade.hpp"

namespace xanadu::enfilade {

TreefiladeNode::TreefiladeNode(std::string name) : name_(std::move(name)) {}

TreefiladeNode *TreefiladeNode::getOrCreateChild(std::string_view childName) {
  const std::string key(childName);
  auto it = children_.find(key);
  if (it != children_.end()) {
    return it->second.get();
  }
  auto child     = std::make_unique<TreefiladeNode>(key);
  auto *ptr      = child.get();
  children_[key] = std::move(child);
  return ptr;
}

void TreefiladeNode::recordMetric(double val, std::uint32_t flags) {
  summary_.add(val, flags);
}

void TreefiladeNode::recompute() noexcept {
  if (children_.empty()) {
    return;
  }
  StatWid combined{};
  for (const auto &[name, child] : children_) {
    child->recompute();
    combined = StatWid::combine(combined, child->summary());
  }
  summary_ = combined;
}

Treefilade::Treefilade() : root_("fleet") {}

void Treefilade::recordMetric(std::string_view region, std::string_view zone,
                              std::string_view host, double latencyMs,
                              std::uint32_t flags) {
  auto *regNode  = root_.getOrCreateChild(region);
  auto *zoneNode = regNode->getOrCreateChild(zone);
  auto *hostNode = zoneNode->getOrCreateChild(host);

  hostNode->recordMetric(latencyMs, flags);
  zoneNode->recordMetric(latencyMs, flags);
  regNode->recordMetric(latencyMs, flags);
  root_.recordMetric(latencyMs, flags);
}

void Treefilade::collectAlerts(std::vector<std::string> &outAlerts) const {
  if (root_.summary().anomalyFlags == AnomalyNone) {
    return;
  }

  for (const auto &[regName, regNode] : root_.children()) {
    if (regNode->summary().anomalyFlags != AnomalyNone) {
      for (const auto &[zoneName, zoneNode] : regNode->children()) {
        if (zoneNode->summary().anomalyFlags != AnomalyNone) {
          for (const auto &[hostName, hostNode] : zoneNode->children()) {
            if (hostNode->summary().anomalyFlags != AnomalyNone) {
              std::string alert =
                  "ALERT: [" + regName + "/" + zoneName + "/" + hostName + "]";
              if ((hostNode->summary().anomalyFlags & AnomalyHighLatency) !=
                  0U) {
                alert += " HighLatency (p99=" +
                         std::to_string(hostNode->summary().quantile(0.99)) +
                         "ms)";
              }
              if ((hostNode->summary().anomalyFlags & AnomalyErrorSpike) !=
                  0U) {
                alert += " ErrorSpike";
              }
              if ((hostNode->summary().anomalyFlags & AnomalySaturation) !=
                  0U) {
                alert += " Saturation";
              }
              outAlerts.push_back(alert);
            }
          }
        }
      }
    }
  }
}

} // namespace xanadu::enfilade
