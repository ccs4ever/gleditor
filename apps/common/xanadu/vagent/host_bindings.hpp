/**
 * @file host_bindings.hpp
 * @brief The host side of native skills (design §5.3, §6.6, §7.5): a registry
 *        of bindings by qualified skill name, each starting effects that
 *        return at once and complete later.
 *
 * A native skill has no body; its methods are effects. The host contract of
 * §5.3 is kept here, in one place, so no binding has to re-implement it:
 *
 *  - `start` returns at once. The runtime is single threaded per node, so a
 *    binding that blocks stalls every agent on it.
 *  - A result comes back through the `EffectCompletion` the binding was
 *    handed. The completion posts it (the adapter runs it as a host delivery
 *    on its own thread, between turns); it never runs agent code in the
 *    binding's call, and may be called from any thread, or from inside
 *    `start`. Only the first call counts.
 *  - `cancel` is best effort and idempotent.
 *  - A device shared by several intentions is serialised (`Queue`) or
 *    refused with cause "busy" (`Refuse`) by the registry, per binding. A
 *    method declared `preempts` starts at once instead: whatever is in
 *    flight on the binding is cancelled and reports "preempted".
 */
#ifndef COMMON_XANADU_VAGENT_HOST_BINDINGS_HPP
#define COMMON_XANADU_VAGENT_HOST_BINDINGS_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/wire.hpp"

namespace xanadu::vagent {

/// How an effect ended. `value` is meaningful when `ok`, `cause` when not.
struct EffectResult {
  bool ok{false};
  WireValue value{WireValue::unit()};
  std::string cause;

  [[nodiscard]] static EffectResult success(WireValue value = {});
  [[nodiscard]] static EffectResult failure(std::string cause);
};

/// Where a finished effect's result is posted. It must only enqueue.
using EffectSink = std::function<void(EffectResult)>;

namespace detail {
struct BindingCore;
struct EffectShared;
struct CompletionAccess;
} // namespace detail

/// The one-shot handle a binding reports through. Copies share one outcome.
class EffectCompletion {
public:
  EffectCompletion() = default;
  /// Report the result. The first call wins; later ones, and calls after the
  /// effect was cancelled or preempted, are ignored.
  void operator()(EffectResult result) const;
  /// True once the effect has been reported, cancelled or preempted.
  [[nodiscard]] bool finished() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept {
    return shared_ != nullptr;
  }

private:
  friend struct detail::CompletionAccess;
  std::shared_ptr<detail::EffectShared> shared_;
  std::weak_ptr<detail::BindingCore> core_;
};

/// What a binding is asked to start.
struct EffectRequest {
  /// Names this effect for `cancel`; unique within a registry.
  std::uint64_t token{};
  std::string method;
  WireList args;
};

class NativeSkill {
public:
  virtual ~NativeSkill() = default;
  /// Begin the effect and return at once. Report the result, now or later,
  /// through @p done.
  virtual void start(const EffectRequest &request, EffectCompletion done) = 0;
  /// Stop the effect `token` if it can be stopped. Best effort, idempotent.
  /// Nothing reported after it reaches the agent.
  virtual void cancel(std::uint64_t token) { (void)token; }
};

/// What happens when an effect is started on a binding that already has one
/// in flight.
enum class Contention : std::uint8_t {
  Shared, ///< any number run at once
  Refuse, ///< the newcomer fails at once with cause "busy"
  Queue,  ///< the newcomer waits for the one in flight, in arrival order
};

struct BindingOptions {
  Contention contention{Contention::Refuse};
  /// Methods that start at once and end whatever is in flight on the binding.
  std::set<std::string, std::less<>> preempting;
};

/// The registry. Safe to use from several threads; a binding's own methods
/// are never called with the registry's lock held.
class HostBindings {
public:
  HostBindings();
  ~HostBindings();
  HostBindings(const HostBindings &)            = delete;
  HostBindings &operator=(const HostBindings &) = delete;

  /// Register @p skill under its qualified name, replacing any binding there.
  HostBindings *bind(std::string name, std::shared_ptr<NativeSkill> skill,
                     BindingOptions options = {});
  HostBindings *unbind(std::string_view name);
  [[nodiscard]] bool bound(std::string_view name) const;
  /// Whether the binding for @p name declares @p method as preempting.
  [[nodiscard]] bool preempts(std::string_view name,
                              std::string_view method) const;

  /// Start an effect and return its token at once. @p sink is called exactly
  /// once with the result, unless the effect is cancelled first. It may run
  /// inside this call (an unbound skill, a refusal) and must only enqueue.
  /// @p preempting is the caller's own claim, joined with the binding's.
  std::uint64_t start(std::string_view name, std::string_view method,
                      WireList args, bool preempting, EffectSink sink);
  /// Cancel an effect, running or queued. No result is posted for it.
  void cancel(std::uint64_t token);

  /// Effects running, and waiting their turn, on a binding.
  [[nodiscard]] std::size_t running(std::string_view name) const;
  [[nodiscard]] std::size_t queued(std::string_view name) const;

private:
  std::shared_ptr<detail::BindingCore> core_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_HOST_BINDINGS_HPP
