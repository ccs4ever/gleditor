/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree representation for Prolog on Vortex / Vlog.
 */
#ifndef COMMON_XANADU_VPROLOG_AST_HPP
#define COMMON_XANADU_VPROLOG_AST_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vprolog {

struct Term;

struct Var {
  std::string name;
  bool isAnonymous{false};

  [[nodiscard]] bool operator==(const Var &other) const = default;
};

struct Atom {
  std::string name;

  [[nodiscard]] bool operator==(const Atom &other) const = default;
};

struct Number {
  std::variant<std::int64_t, double> value{0};

  [[nodiscard]] bool isFloat() const noexcept {
    return std::holds_alternative<double>(value);
  }
  [[nodiscard]] std::int64_t asInt() const noexcept {
    return isFloat() ? static_cast<std::int64_t>(std::get<double>(value))
                     : std::get<std::int64_t>(value);
  }
  [[nodiscard]] double asFloat() const noexcept {
    return isFloat() ? std::get<double>(value)
                     : static_cast<double>(std::get<std::int64_t>(value));
  }
  [[nodiscard]] bool operator==(const Number &other) const = default;
};

struct String {
  std::string value;

  [[nodiscard]] bool operator==(const String &other) const = default;
};

struct Compound {
  std::string functor;
  std::vector<Term> args;

  [[nodiscard]] bool operator==(const Compound &other) const;
};

struct List {
  std::vector<Term> elements;
  std::shared_ptr<Term> tail{nullptr}; // e.g. [H | T]

  [[nodiscard]] bool operator==(const List &other) const;
};

struct Term {
  std::variant<Var, Atom, Number, String, Compound, List> node;

  Term() : node(Atom{"[]"}) {}
  Term(Var v) : node(std::move(v)) {}
  Term(Atom a) : node(std::move(a)) {}
  Term(Number n) : node(n) {}
  Term(String s) : node(std::move(s)) {}
  Term(Compound c) : node(std::move(c)) {}
  Term(List l) : node(std::move(l)) {}

  [[nodiscard]] bool isVar() const noexcept {
    return std::holds_alternative<Var>(node);
  }
  [[nodiscard]] bool isAtom() const noexcept {
    return std::holds_alternative<Atom>(node);
  }
  [[nodiscard]] bool isNumber() const noexcept {
    return std::holds_alternative<Number>(node);
  }
  [[nodiscard]] bool isString() const noexcept {
    return std::holds_alternative<String>(node);
  }
  [[nodiscard]] bool isCompound() const noexcept {
    return std::holds_alternative<Compound>(node);
  }
  [[nodiscard]] bool isList() const noexcept {
    return std::holds_alternative<List>(node);
  }

  [[nodiscard]] const Var *asVar() const noexcept {
    return std::get_if<Var>(&node);
  }
  [[nodiscard]] const Atom *asAtom() const noexcept {
    return std::get_if<Atom>(&node);
  }
  [[nodiscard]] const Number *asNumber() const noexcept {
    return std::get_if<Number>(&node);
  }
  [[nodiscard]] const String *asString() const noexcept {
    return std::get_if<String>(&node);
  }
  [[nodiscard]] const Compound *asCompound() const noexcept {
    return std::get_if<Compound>(&node);
  }
  [[nodiscard]] const List *asList() const noexcept {
    return std::get_if<List>(&node);
  }

  [[nodiscard]] Term canonicalizeList() const;

  [[nodiscard]] bool operator==(const Term &other) const = default;
};

inline bool Compound::operator==(const Compound &other) const {
  return functor == other.functor && args == other.args;
}

inline bool List::operator==(const List &other) const {
  if (elements != other.elements) return false;
  if (!tail && !other.tail) return true;
  if (!tail || !other.tail) return false;
  return *tail == *other.tail;
}

inline Term Term::canonicalizeList() const {
  if (const auto *list = asList()) {
    Term result = list->tail ? *list->tail : Term(Atom{"[]"});
    for (auto it = list->elements.rbegin(); it != list->elements.rend(); ++it) {
      Compound c;
      c.functor = ".";
      c.args.push_back(it->canonicalizeList());
      c.args.push_back(std::move(result));
      result = Term(std::move(c));
    }
    return result;
  }
  if (const auto *comp = asCompound()) {
    Compound c;
    c.functor = comp->functor;
    for (const auto &arg : comp->args) {
      c.args.push_back(arg.canonicalizeList());
    }
    return Term(std::move(c));
  }
  return *this;
}

struct Clause {
  Term head;
  std::vector<Term> body;  // Conjunction of goals
  bool isQuery{false};     // ?- ...
  bool isDirective{false}; // :- ...
  SourceLocation loc{};

  [[nodiscard]] bool isFact() const noexcept {
    return body.empty() && !isQuery && !isDirective;
  }
  [[nodiscard]] bool isRule() const noexcept {
    return !body.empty() && !isQuery && !isDirective;
  }
};

struct Program {
  std::vector<Clause> clauses;
};

std::string formatTerm(const Term &term);
std::string formatClause(const Clause &clause);

} // namespace xanadu::vprolog

#endif // COMMON_XANADU_VPROLOG_AST_HPP
