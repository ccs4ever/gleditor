/**
 * @file cpp26.hpp
 * @brief Standard-shaped C++26 function_ref and optional (with optional
 *        references and range support), native where the standard library
 *        has them and from the pinned fallbacks where it does not.
 *
 * Selection is per facility, in cpp26_select.hpp: -std=c++2c alone does not
 * mean the standard library implements every C++26 API.
 */
#ifndef GLEDITOR_CPP26_HPP
#define GLEDITOR_CPP26_HPP

#include <functional>
#include <optional>

#include <gleditor/cpp26_select.hpp>

#if !GLEDITOR_CPP26_NATIVE_FUNCTION_REF
#include <std23/function_ref.h>
#endif

#if !GLEDITOR_CPP26_NATIVE_OPTIONAL
#include <beman/optional/optional.hpp>
#endif

namespace gleditor::cpp26 {

#if GLEDITOR_CPP26_NATIVE_FUNCTION_REF
using std::function_ref;
#else
using std23::function_ref;
#endif

#if GLEDITOR_CPP26_NATIVE_OPTIONAL
using std::bad_optional_access;
using std::in_place;
using std::in_place_t;
using std::make_optional;
using std::nullopt;
using std::nullopt_t;
using std::optional;
#else
using beman::optional::bad_optional_access;
using beman::optional::in_place;
using beman::optional::in_place_t;
using beman::optional::make_optional;
using beman::optional::nullopt;
using beman::optional::nullopt_t;
using beman::optional::optional;
#endif

} // namespace gleditor::cpp26

#endif // GLEDITOR_CPP26_HPP
