/**
 * @file cpp26_inplace_vector.hpp
 * @brief C++26 fixed-capacity vector selection for application-local buffers.
 *
 * This header stays separate from cpp26.hpp so the large container
 * implementation is parsed only by code that uses it.
 */
#ifndef COMMON_CPP26_INPLACE_VECTOR_HPP
#define COMMON_CPP26_INPLACE_VECTOR_HPP

#include <version>

#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202603L
#include <inplace_vector>
#else
#include <beman/inplace_vector/inplace_vector.hpp>
#endif

namespace common::cpp26 {

#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_inplace_vector) && __cpp_lib_inplace_vector >= 202603L
using std::inplace_vector;
#else
using beman::inplace_vector::inplace_vector;
#endif

} // namespace common::cpp26

#endif // COMMON_CPP26_INPLACE_VECTOR_HPP
