#include "payment_verifier.hpp"

#include <iostream>

namespace xudu::identity {

bool AlwaysAcceptVerifier::verify(const TcSettleRequestMsg &request,
                                  const std::uint64_t priceAtomicUnits) {
  accepted_++;
  // On every call, not once at startup: this is a node handing over paywalled
  // content it has not been paid for, and a line in the log is the only thing
  // that will say so.
  std::cerr << "xudu: transcopyright: accepting " << priceAtomicUnits
            << " atomic units WITHOUT VERIFICATION for key "
            << request.keyId.toHex()
            << "\n      no settlement backend is configured, so this node is "
               "giving paywalled content away\n";
  return true;
}

} // namespace xudu::identity
