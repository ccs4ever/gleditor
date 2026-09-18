/**
 * @file merkle_domain.hpp
 * @brief RFC 6962 leaf/interior domain-separation tags, shared by every
 * Merkle-style hash tree in this module.
 */
#ifndef XANADU_MERKLE_DOMAIN_HPP
#define XANADU_MERKLE_DOMAIN_HPP

namespace xanadu {

// RFC 6962 section 2.1 domain tags. Leaves and interior nodes must hash into
// separate domains, or SHA256(l || r) is reachable two ways -- as an interior
// node, and as a leaf whose content happens to be those 64 bytes -- and a
// forged proof can pass off a fabricated leaf as a subtree. The tags are the
// whole defence, so they are named once here rather than spelled inline in
// each tree. Sharing the constant does not couple the trees themselves:
// MerkleLedger, PublicationLedger, PaymentReceipt's hash, and
// EnginePipeline's oracle-vote tree remain separate implementations over
// separate record types; only the fixed protocol byte is shared.
constexpr char kMerkleLeafDomain     = '\x00';
constexpr char kMerkleInteriorDomain = '\x01';

} // namespace xanadu

#endif // XANADU_MERKLE_DOMAIN_HPP
