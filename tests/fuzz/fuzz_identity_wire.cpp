/**
 * @file fuzz_identity_wire.cpp
 * @brief LLVM libFuzzer harness for the BEP 10 identity peer-wire decoders.
 *
 * Everything reached from here parses bytes a peer chose, before that peer
 * has authenticated -- so these decoders are the outermost attack surface the
 * identity subsystem has, and the only one an attacker can reach without
 * holding a key. The frame decoder goes first because on_extended calls it
 * first, then each payload decoder gets the frame's contents whatever the
 * declared type, since a peer is free to lie about that.
 */
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

#include "common/xanadu/identity/identity_serialization.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::span<const std::uint8_t> bytes(data, size);

  using namespace xanadu::identity;

  // The envelope, as on_extended sees it.
  const auto frame = decodeExtendedMessage(bytes);

  // Payloads. Fed both the raw input and, when the envelope parsed, the
  // payload it delimited -- a decoder that only ever sees whole buffers is
  // not the decoder that runs in production.
  const auto tryAll = [](std::span<const std::uint8_t> payload) {
    std::ignore = decodeIdentityEntry(payload);
    // Hand-rolled stride parsing over a peer-supplied string, so worth
    // reaching even though it is the newest of these.
    std::ignore = decodeIdentityResponse(payload);
    std::ignore = decodeVoteEntry(payload);
    std::ignore = decodeBlockHeader(payload);
    std::ignore = decodeOracleAttestation(payload);
    std::ignore = decodePeerChallenge(payload);
    std::ignore = decodePeerChallengeResponse(payload);
    std::ignore = decodeIdentityQuery(payload);
    std::ignore = decodeEmailVerifyRequest(payload);
    std::ignore = decodeTcInvoiceQuery(payload);
    std::ignore = decodeTcInvoiceResponse(payload);
    std::ignore = decodeTcSettleRequest(payload);
    std::ignore = decodeTcKeyDelivery(payload);
  };

  tryAll(bytes);
  if (frame) {
    tryAll(frame->payload);
  }

  return 0;
}
