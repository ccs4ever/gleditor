/**
 * @file cpp26_select.hpp
 * @brief Which C++26 library facilities are native and which come from the
 *        pinned fallbacks, decided once for every cpp26 header.
 *
 * Each GLEDITOR_CPP26_NATIVE_* macro is 1 when the standard library provides
 * the facility and 0 when the fallback under thirdparty/ stands in for it.
 *
 * A native type and its fallback need not share a layout or symbol names, so
 * everything compiled against one libgleditor must agree on the choice. In
 * this tree that holds by construction -- the library and every program are
 * built together, with the same compiler, library and flags. An installed
 * library records its choices in <gleditor/cpp26_config.hpp> (written by
 * `make install`, never present in the source tree), and those win over a
 * probe here, so a program built later against a different standard library
 * still matches the library it links. See design/cpp26-compatibility.md.
 */
#ifndef GLEDITOR_CPP26_SELECT_HPP
#define GLEDITOR_CPP26_SELECT_HPP

#include <version>

#if __has_include(<gleditor/cpp26_config.hpp>)
#include <gleditor/cpp26_config.hpp>
#endif

#ifndef GLEDITOR_CPP26_NATIVE_FUNCTION_REF
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202603L
#define GLEDITOR_CPP26_NATIVE_FUNCTION_REF 1
#else
#define GLEDITOR_CPP26_NATIVE_FUNCTION_REF 0
#endif
#endif

// Optional references and optional-as-a-range arrive as two papers but are
// adopted together, since the fallback supplies both or neither.
#ifndef GLEDITOR_CPP26_NATIVE_OPTIONAL
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) && defined(__cpp_lib_optional) &&  \
    __cpp_lib_optional >= 202506L &&                                           \
    defined(__cpp_lib_optional_range_support) &&                               \
    __cpp_lib_optional_range_support >= 202406L
#define GLEDITOR_CPP26_NATIVE_OPTIONAL 1
#else
#define GLEDITOR_CPP26_NATIVE_OPTIONAL 0
#endif
#endif

#ifndef GLEDITOR_CPP26_NATIVE_INPLACE_VECTOR
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202603L
#define GLEDITOR_CPP26_NATIVE_INPLACE_VECTOR 1
#else
#define GLEDITOR_CPP26_NATIVE_INPLACE_VECTOR 0
#endif
#endif

#ifndef GLEDITOR_CPP26_NATIVE_CONCAT
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_ranges_concat) && __cpp_lib_ranges_concat >= 202403L
#define GLEDITOR_CPP26_NATIVE_CONCAT 1
#else
#define GLEDITOR_CPP26_NATIVE_CONCAT 0
#endif
#endif

#ifndef GLEDITOR_CPP26_NATIVE_SPAN_AT
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) && defined(__cpp_lib_span) &&      \
    __cpp_lib_span >= 202311L
#define GLEDITOR_CPP26_NATIVE_SPAN_AT 1
#else
#define GLEDITOR_CPP26_NATIVE_SPAN_AT 0
#endif
#endif

#endif // GLEDITOR_CPP26_SELECT_HPP
