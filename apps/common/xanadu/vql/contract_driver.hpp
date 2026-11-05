/**
 * @file contract_driver.hpp
 * @brief Backend-neutral continuation driver for VQL structural contracts.
 */
#ifndef COMMON_XANADU_VQL_CONTRACT_DRIVER_HPP
#define COMMON_XANADU_VQL_CONTRACT_DRIVER_HPP

#include <concepts>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <variant>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql::detail {

enum class ScopePolicy { CommitOnSuccess, RollbackAlways };

/// One structural walk for synchronous matching and graph construction.
/// Callbacks, subject views and continuations are borrowed for the enclosing
/// call. A backend consumes them before returning; graph continuations emit
/// jumps and never become runtime closures. Arm and element builders can be
/// invoked repeatedly, so they must not consume shared continuation state.
template <class Backend> class ContractDriver {
public:
  using Bindings     = typename Backend::Bindings;
  using Continuation = typename Backend::Continuation;
  using Context      = typename Backend::Context;
  using Dimension    = typename Backend::Dimension;
  using Subject      = typename Backend::Subject;
  using Subjects     = typename Backend::Subjects;
  using MissingField = typename Backend::MissingField;

  explicit ContractDriver(Backend &backend) : backend_(backend) {}

  void matchPattern(const DestructurePattern &pattern, Context context,
                    Bindings &bindings, const Bindings &defaultScope,
                    MissingField missing, Continuation success,
                    Continuation failure) {
    backend_.validContext(
        context,
        [&] {
          backend_.scope(
              bindings, ScopePolicy::CommitOnSuccess,
              [&](Bindings &trial, Continuation matched, Continuation refused) {
                backend_.allOf(
                    std::span{pattern.fields},
                    [&](const DestructureField &field, Continuation next,
                        Continuation failed) {
                      backend_.consumeWork();
                      matchField(field, context, trial, defaultScope, missing,
                                 next, failed);
                    },
                    matched, refused);
              },
              success, failure);
        },
        failure);
  }

  void matchType(const TypeExpr &type, const Subjects &subjects,
                 Bindings *captures, const Bindings &defaultScope,
                 Continuation success, Continuation failure) {
    backend_.consumeWork();
    backend_.validateType(type);
    Bindings ignored;
    Bindings &target = captures ? *captures : ignored;
    backend_.anyArm(
        type.unionType.members.size(), target,
        captures ? ScopePolicy::CommitOnSuccess : ScopePolicy::RollbackAlways,
        [&](std::size_t index, Bindings &trial, Continuation matched,
            Continuation refused) {
          matchPrimary(type.unionType.members[index], subjects, trial,
                       defaultScope, captures != nullptr, matched, refused);
        },
        success, failure);
  }

private:
  void matchField(const DestructureField &field, Context context,
                  Bindings &trial, const Bindings &defaultScope,
                  MissingField missing, Continuation success,
                  Continuation failure) {
    if (field.isContext) {
      backend_.withSingleton(backend_.subjectOf(context),
                             [&](const Subjects &bound) {
                               const auto capture = [&] {
                                 captureField(field, trial, bound, success);
                               };
                               if (field.type)
                                 matchType(*field.type, bound, &trial,
                                           defaultScope, capture, failure);
                               else
                                 capture();
                             });
      return;
    }
    const Dimension dimension = backend_.resolveDimension(field.dimName);
    const auto found          = [&](Subject subject) {
      backend_.withSingleton(subject, [&](const Subjects &bound) {
        checkFieldValue(field, bound, trial, defaultScope, missing, success,
                                 failure);
      });
    };
    const auto absent = [&] {
      evaluateMissingField(field, context, trial, defaultScope, missing,
                           success, failure);
    };
    backend_.probeLink(context, dimension, field.direction, found, [&] {
      if (field.isBidir) {
        const auto reverse = field.direction == zigzag::DimVector::POS
                                 ? zigzag::DimVector::NEG
                                 : zigzag::DimVector::POS;
        backend_.probeLink(context, dimension, reverse, found, absent);
      } else {
        absent();
      }
    });
  }

  void evaluateMissingField(const DestructureField &field, Context context,
                            Bindings &trial, const Bindings &defaultScope,
                            MissingField missing, Continuation success,
                            Continuation failure) {
    const auto check = [&](const Subjects &values) {
      checkFieldValue(field, values, trial, defaultScope, missing, success,
                      failure);
    };
    if (field.defaultValue)
      backend_.evaluateDefault(*field.defaultValue, context, defaultScope,
                               check, failure);
    else
      check(backend_.emptySubjects());
  }

  void checkFieldValue(const DestructureField &field, const Subjects &values,
                       Bindings &trial, const Bindings &defaultScope,
                       MissingField missing, Continuation success,
                       Continuation failure) {
    const auto capture = [&] { captureField(field, trial, values, success); };
    if (field.type) {
      matchType(*field.type, values, &trial, defaultScope, capture, failure);
    } else {
      backend_.empty(
          values,
          [&] {
            if (missing == MissingField::Refuse)
              backend_.resume(failure);
            else
              capture();
          },
          capture);
    }
  }

  void captureField(const DestructureField &field, Bindings &bindings,
                    const Subjects &values, Continuation success) {
    if (field.isContext) backend_.capture(bindings, ".", values);
    if (!field.varName.empty())
      backend_.capture(bindings, field.varName, values);
    backend_.resume(success);
  }

  void matchPrimary(const PrimaryType &type, const Subjects &subjects,
                    Bindings &captures, const Bindings &defaultScope,
                    bool captureEnabled, Continuation success,
                    Continuation failure) {
    if (const auto *generic = std::get_if<GenericType>(&type.kind)) {
      Bindings ignored;
      backend_.scope(
          ignored, ScopePolicy::RollbackAlways,
          [&](Bindings &, Continuation matched, Continuation refused) {
            backend_.allOf(
                subjects,
                [&](const Subject &cell, Continuation next,
                    Continuation failed) {
                  Bindings privateBindings;
                  backend_.withSingleton(cell, [&](const Subjects &one) {
                    matchType(
                        *generic->args.front(), one, nullptr, defaultScope,
                        [&] {
                          if (type.refinement)
                            matchPattern(type.refinement->pattern,
                                         backend_.contextOf(cell),
                                         privateBindings, defaultScope,
                                         MissingField::Refuse, next, failed);
                          else
                            backend_.resume(next);
                        },
                        failed);
                  });
                },
                matched, refused);
          },
          success, failure);
      return;
    }
    backend_.empty(
        subjects, [&] { backend_.resume(type.isOptional ? success : failure); },
        [&] {
          backend_.single(
              subjects,
              [&](Subject cell) {
                matchAtom(type, cell, captures, defaultScope, captureEnabled,
                          success, failure);
              },
              failure);
        });
  }

  void matchAtom(const PrimaryType &type, Subject cell, Bindings &captures,
                 const Bindings &defaultScope, bool captureEnabled,
                 Continuation success, Continuation failure) {
    const auto refinement = [&] {
      if (type.refinement)
        matchPattern(type.refinement->pattern, backend_.contextOf(cell),
                     captures, defaultScope, MissingField::Refuse, success,
                     failure);
      else
        backend_.resume(success);
    };
    std::visit(
        [&](const auto &held) {
          using T = std::remove_cvref_t<decltype(held)>;
          if constexpr (std::same_as<T, BasicType>) {
            backend_.probeBasic(held, cell, refinement, failure);
          } else if constexpr (std::same_as<T, ShapeType>) {
            backend_.validSubject(
                cell,
                [&] {
                  matchPattern(held.pattern, backend_.contextOf(cell), captures,
                               defaultScope, MissingField::Refuse, refinement,
                               failure);
                },
                failure);
          } else if constexpr (std::same_as<T, std::shared_ptr<TypeExpr>>) {
            backend_.withSingleton(cell, [&](const Subjects &one) {
              matchType(*held, one, captureEnabled ? &captures : nullptr,
                        defaultScope, refinement, failure);
            });
          } else {
            backend_.resume(failure);
          }
        },
        type.kind);
  }

  Backend &backend_;
};

} // namespace xanadu::vql::detail
#endif // COMMON_XANADU_VQL_CONTRACT_DRIVER_HPP
