#include "common/xanadu/vagent/host_bindings.hpp"

#include <gleditor/logging.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "common/xanadu/vagent/runtime_abi.hpp"

namespace xanadu::vagent {

namespace {
constexpr const char *kLog = "vortex.agent";
/// A binding whose `start` threw reports this; the exception text stays in the
/// log, since a cause travels to the agent as data.
constexpr std::string_view kCauseFault = "fault";
} // namespace

EffectResult EffectResult::success(WireValue value) {
  return EffectResult{true, std::move(value), {}};
}

EffectResult EffectResult::failure(std::string cause) {
  return EffectResult{false, WireValue::unit(), std::move(cause)};
}

namespace detail {

struct EffectShared {
  std::uint64_t token{};
  std::string skill;
  EffectSink sink;
  /// Set by whoever ends the effect first (the binding, a cancel, a
  /// preemption); the others then do nothing.
  std::atomic<bool> claimed{false};
};

struct Queued {
  std::shared_ptr<EffectShared> shared;
  std::string method;
  WireList args;
};

struct Binding {
  std::shared_ptr<NativeSkill> skill;
  BindingOptions options;
  std::vector<std::shared_ptr<EffectShared>> live;
  std::deque<Queued> queue;
};

struct BindingCore {
  mutable std::mutex mutex;
  std::map<std::string, Binding, std::less<>> bindings;
  std::uint64_t nextToken{0};
};

struct CompletionAccess {
  static EffectCompletion make(std::shared_ptr<EffectShared> shared,
                               const std::shared_ptr<BindingCore> &core) {
    EffectCompletion completion;
    completion.shared_ = std::move(shared);
    completion.core_   = core;
    return completion;
  }
  static const std::shared_ptr<EffectShared> &
  shared(const EffectCompletion &c) {
    return c.shared_;
  }
  static std::shared_ptr<BindingCore> core(const EffectCompletion &c) {
    return c.core_.lock();
  }
};

namespace {

/// A queued effect that has been given the binding's slot and is to be started
/// once the lock is dropped.
struct Promoted {
  Queued queued;
  std::shared_ptr<NativeSkill> skill;
};

/// Hand the slot to the head of the queue. Caller holds the lock.
std::optional<Promoted> promoteLocked(Binding &binding) {
  if (binding.options.contention != Contention::Queue ||
      !binding.live.empty() || binding.queue.empty()) {
    return std::nullopt;
  }
  Promoted next{std::move(binding.queue.front()), binding.skill};
  binding.queue.pop_front();
  binding.live.push_back(next.queued.shared);
  return next;
}

void run(const std::shared_ptr<BindingCore> &core, Promoted next);

/// End @p shared with @p result unless something already did, free its slot,
/// post the result, and start whatever was waiting for the slot.
void complete(const std::shared_ptr<BindingCore> &core,
              const std::shared_ptr<EffectShared> &shared,
              EffectResult result) {
  if (shared->claimed.exchange(true)) return;
  std::optional<Promoted> next;
  if (core) {
    const std::lock_guard lock(core->mutex);
    if (const auto found = core->bindings.find(shared->skill);
        found != core->bindings.end()) {
      std::erase(found->second.live, shared);
      next = promoteLocked(found->second);
    }
  }
  if (shared->sink) shared->sink(std::move(result));
  if (next) run(core, std::move(*next));
}

void run(const std::shared_ptr<BindingCore> &core, Promoted next) {
  const auto shared = next.queued.shared;
  EffectRequest request{shared->token, std::move(next.queued.method),
                        std::move(next.queued.args)};
  try {
    next.skill->start(request, CompletionAccess::make(shared, core));
  } catch (const std::exception &e) {
    GLEDITOR_LOG_DEBUG(kLog, "native skill {} threw starting {}: {}",
                       shared->skill, request.method, e.what());
    complete(core, shared, EffectResult::failure(std::string(kCauseFault)));
  }
}

} // namespace

void completeEffect(const EffectCompletion &completion, EffectResult result) {
  const auto &shared = CompletionAccess::shared(completion);
  if (shared)
    complete(CompletionAccess::core(completion), shared, std::move(result));
}

} // namespace detail

void EffectCompletion::operator()(EffectResult result) const {
  detail::completeEffect(*this, std::move(result));
}

bool EffectCompletion::finished() const noexcept {
  return shared_ == nullptr || shared_->claimed.load();
}

HostBindings::HostBindings() : core_(std::make_shared<detail::BindingCore>()) {}

HostBindings::~HostBindings() = default;

HostBindings *HostBindings::bind(std::string name,
                                 std::shared_ptr<NativeSkill> skill,
                                 BindingOptions options) {
  const std::lock_guard lock(core_->mutex);
  auto &binding   = core_->bindings[std::move(name)];
  binding.skill   = std::move(skill);
  binding.options = std::move(options);
  return this;
}

HostBindings *HostBindings::unbind(std::string_view name) {
  const std::lock_guard lock(core_->mutex);
  if (const auto found = core_->bindings.find(name);
      found != core_->bindings.end()) {
    core_->bindings.erase(found);
  }
  return this;
}

bool HostBindings::bound(std::string_view name) const {
  const std::lock_guard lock(core_->mutex);
  return core_->bindings.contains(name);
}

bool HostBindings::preempts(std::string_view name,
                            std::string_view method) const {
  const std::lock_guard lock(core_->mutex);
  const auto found = core_->bindings.find(name);
  return found != core_->bindings.end() &&
         found->second.options.preempting.contains(method);
}

std::uint64_t HostBindings::start(std::string_view name,
                                  std::string_view method, WireList args,
                                  bool preempting, EffectSink sink) {
  auto shared   = std::make_shared<detail::EffectShared>();
  shared->skill = std::string(name);
  shared->sink  = std::move(sink);

  std::vector<std::shared_ptr<detail::EffectShared>> victims;
  std::shared_ptr<NativeSkill> skill;
  std::shared_ptr<NativeSkill> victimSkill;
  std::optional<EffectResult> refused;
  bool launch = false;
  {
    const std::lock_guard lock(core_->mutex);
    shared->token    = ++core_->nextToken;
    const auto found = core_->bindings.find(name);
    if (found == core_->bindings.end()) {
      refused = EffectResult::failure(std::string(kEffectCauseUnbound));
    } else {
      auto &binding = found->second;
      skill         = binding.skill;
      preempting    = preempting || binding.options.preempting.contains(method);
      if (preempting) {
        victims     = std::exchange(binding.live, {});
        victimSkill = binding.skill;
      }
      const bool busy = !binding.live.empty() &&
                        binding.options.contention != Contention::Shared;
      if (busy && binding.options.contention == Contention::Refuse) {
        refused = EffectResult::failure(std::string(kEffectCauseBusy));
      } else if (busy) {
        binding.queue.push_back(
            detail::Queued{shared, std::string(method), std::move(args)});
      } else {
        binding.live.push_back(shared);
        launch = true;
      }
    }
  }
  // Nothing below holds the lock: the binding and the sink are foreign code.
  for (const auto &victim : victims) {
    victimSkill->cancel(victim->token);
    if (!victim->claimed.exchange(true) && victim->sink) {
      victim->sink(EffectResult::failure(std::string(kEffectCausePreempted)));
    }
  }
  if (refused) {
    if (!shared->claimed.exchange(true) && shared->sink) {
      shared->sink(std::move(*refused));
    }
  } else if (launch) {
    detail::run(core_,
                detail::Promoted{detail::Queued{shared, std::string(method),
                                                std::move(args)},
                                 std::move(skill)});
  }
  return shared->token;
}

void HostBindings::cancel(std::uint64_t token) {
  std::shared_ptr<NativeSkill> skill;
  std::optional<detail::Promoted> next;
  bool wasLive = false;
  {
    const std::lock_guard lock(core_->mutex);
    for (auto &[name, binding] : core_->bindings) {
      const auto live = std::ranges::find_if(
          binding.live, [&](const auto &e) { return e->token == token; });
      if (live != binding.live.end()) {
        // Already being reported: the result is on its way and wins.
        if ((*live)->claimed.exchange(true)) return;
        binding.live.erase(live);
        skill   = binding.skill;
        wasLive = true;
        next    = detail::promoteLocked(binding);
        break;
      }
      const auto queued =
          std::ranges::find_if(binding.queue, [&](const auto &q) {
            return q.shared->token == token;
          });
      if (queued != binding.queue.end()) {
        queued->shared->claimed.store(true);
        binding.queue.erase(queued);
        return;
      }
    }
  }
  if (wasLive && skill) skill->cancel(token);
  if (next) detail::run(core_, std::move(*next));
}

std::size_t HostBindings::running(std::string_view name) const {
  const std::lock_guard lock(core_->mutex);
  const auto found = core_->bindings.find(name);
  return found == core_->bindings.end() ? 0 : found->second.live.size();
}

std::size_t HostBindings::queued(std::string_view name) const {
  const std::lock_guard lock(core_->mutex);
  const auto found = core_->bindings.find(name);
  return found == core_->bindings.end() ? 0 : found->second.queue.size();
}

} // namespace xanadu::vagent
