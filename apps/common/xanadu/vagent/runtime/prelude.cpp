#include "common/xanadu/vagent/runtime/prelude.hpp"

#include <algorithm>
#include <array>

namespace xanadu::vagent::runtime {

namespace {

// `source` and `space` are Opt: a host delivery has neither (§5.5).
constexpr std::array kImplicit = {
    PreludeField{"source", "Agent", true},
    PreludeField{"space", "Space", true},
    PreludeField{"time", "Time", false},
    PreludeField{"ordinal", "Int", false},
};

constexpr std::array kGoal         = {PreludeField{"intention", "Intention"},
                                      PreludeField{"goal", "Str"}};
constexpr std::array kGoalFailed   = {PreludeField{"intention", "Intention"},
                                      PreludeField{"goal", "Str"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kPlanFailed   = {PreludeField{"intention", "Intention"},
                                      PreludeField{"plan", "Str"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kEffectFailed = {PreludeField{"capacity", "Str"},
                                      PreludeField{"method", "Str"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kFault        = {PreludeField{"intention", "Intention"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kChild        = {PreludeField{"child", "Agent"}};
constexpr std::array kChildCause   = {PreludeField{"child", "Agent"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kDelivery     = {PreludeField{"to", "Agent"},
                                      PreludeField{"kind", "Str"},
                                      PreludeField{"cause", "Str"}};
constexpr std::array kOrphaned     = {PreludeField{"former_parent", "Agent"}};
constexpr std::array kMoved        = {PreludeField{"name", "Agent"}};
constexpr std::array kUpgrade      = {PreludeField{"from_version", "Str"}};

constexpr std::array kEvents = {
    PreludeEvent{"Initialize", {}},
    PreludeEvent{"Destroy", {}},
    PreludeEvent{"GoalAchieved", kGoal},
    PreludeEvent{"GoalFailed", kGoalFailed},
    PreludeEvent{"GoalDropped", kGoal},
    PreludeEvent{"PlanFailed", kPlanFailed},
    PreludeEvent{"EffectFailed", kEffectFailed},
    PreludeEvent{"ConditionFault", kFault},
    PreludeEvent{"ChildDown", kChild},
    PreludeEvent{"ChildStopping", kChildCause},
    PreludeEvent{"Orphaned", kOrphaned},
    PreludeEvent{"SpawnFailed", kChild},
    PreludeEvent{"DeliveryFailed", kDelivery},
    PreludeEvent{"Moved", kMoved},
    PreludeEvent{"Upgrade", kUpgrade},
};

} // namespace

std::span<const PreludeEvent> preludeEvents() { return kEvents; }

std::span<const PreludeField> preludeImplicitFields() { return kImplicit; }

const PreludeEvent *findPreludeEvent(std::string_view name) {
  const auto it = std::ranges::find(kEvents, name, &PreludeEvent::name);
  return it == kEvents.end() ? nullptr : &*it;
}

} // namespace xanadu::vagent::runtime
