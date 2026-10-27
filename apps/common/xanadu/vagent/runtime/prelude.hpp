/**
 * @file prelude.hpp
 * @brief The declarations every vagent unit sees without writing them (§10):
 *        the built-in events and the implicit event fields of §5.5.
 *
 * One table, read by the checker and the compiler alike, so the two cannot
 * disagree about a field's name or type. Types are spelled as in source
 * (`Agent`, `Space`) and each reader resolves them with its own rules.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_PRELUDE_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_PRELUDE_HPP

#include <span>
#include <string_view>

namespace xanadu::vagent::runtime {

struct PreludeField {
  std::string_view name;
  std::string_view type; ///< the base type name
  bool optional{false};  ///< the field is `Opt<type>`
};

struct PreludeEvent {
  std::string_view name;
  std::span<const PreludeField> fields;
};

/// `Initialize`, `Destroy`, `GoalAchieved`, ... `Moved`, in §10's order.
[[nodiscard]] std::span<const PreludeEvent> preludeEvents();

/// `source`, `space`, `time`, `ordinal`: on every event, declared or built in,
/// and never written by a program (§5.5).
[[nodiscard]] std::span<const PreludeField> preludeImplicitFields();

[[nodiscard]] const PreludeEvent *findPreludeEvent(std::string_view name);

} // namespace xanadu::vagent::runtime

#endif
