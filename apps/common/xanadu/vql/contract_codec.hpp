/**
 * @file contract_codec.hpp
 * @brief Portable, versioned encoding for VQL type contract descriptors.
 */
#ifndef COMMON_XANADU_VQL_CONTRACT_CODEC_HPP
#define COMMON_XANADU_VQL_CONTRACT_CODEC_HPP

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

/// A malformed, unsupported-version, or unencodable contract descriptor.
class ContractDescriptorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Bounds are checked before allocating a parser AST for imported descriptors.
/// Callers can supply tighter limits for a constrained execution environment.
struct ContractDescriptorLimits {
  std::size_t maxBytes{64 * 1024};
  std::size_t maxNesting{128};
};

/// Encode a type AST as `VQL_CONTRACT/1\n` followed by canonical VQL syntax.
[[nodiscard]] std::string encodeTypeDescriptor(const TypeExpr &type);

/// Decode a portable contract descriptor without process-local state.
[[nodiscard]] TypeExpr
decodeTypeDescriptor(std::string_view descriptor,
                     ContractDescriptorLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CONTRACT_CODEC_HPP
