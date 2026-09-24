/**
 * @file cpp26.hpp
 * @brief Standard-shaped C++26 library facilities for application code.
 *
 * These aliases are selected per library feature, since -std=c++2c alone
 * does not mean the selected standard library implements every C++26 API.
 * Keep them inside application binaries: native and fallback types need not
 * have the same ABI.
 */
#ifndef COMMON_CPP26_HPP
#define COMMON_CPP26_HPP

#include <functional>
#include <optional>
#include <version>

#if defined(GLEDITOR_CPP26_FORCE_FALLBACK) ||                                  \
    !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202603L
#include <std23/function_ref.h>
#endif

#if defined(GLEDITOR_CPP26_FORCE_FALLBACK) || !defined(__cpp_lib_optional) ||  \
    __cpp_lib_optional < 202506L ||                                            \
    !defined(__cpp_lib_optional_range_support) ||                              \
    __cpp_lib_optional_range_support < 202406L
#include <beman/optional/optional.hpp>
#endif

namespace common::cpp26 {

#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202603L
using std::function_ref;
#else
using std23::function_ref;
#endif

#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) && defined(__cpp_lib_optional) &&  \
    __cpp_lib_optional >= 202506L &&                                           \
    defined(__cpp_lib_optional_range_support) &&                               \
    __cpp_lib_optional_range_support >= 202406L
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

} // namespace common::cpp26

#endif // COMMON_CPP26_HPP
