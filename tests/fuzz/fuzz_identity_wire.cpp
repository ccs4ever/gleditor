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

#include <xudu/core/identity/identity_serialization.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }
  const std::span<const std::uint8_t> bytes(data, size);

  using namespace xudu::identity;

  // The envelope, as on_extended sees it.
  const auto frame = decodeExtendedMessage(bytes);

  // Payloads. Fed both the raw input and, when the envelope parsed, the
  // payload it delimited -- a decoder that only ever sees whole buffers is
  // not the decoder that runs in production.
  const auto tryAll = [](std::span<const std::uint8_t> payload) {
    static_cast<void>(decodeIdentityEntry(payload));
    static_cast<void>(decodeVoteEntry(payload));
    static_cast<void>(decodeBlockHeader(payload));
    static_cast<void>(decodeOracleAttestation(payload));
    static_cast<void>(decodePeerChallenge(payload));
    static_cast<void>(decodePeerChallengeResponse(payload));
    static_cast<void>(decodeIdentityQuery(payload));
    static_cast<void>(decodeEmailVerifyRequest(payload));
    static_cast<void>(decodeTcInvoiceQuery(payload));
    static_cast<void>(decodeTcInvoiceResponse(payload));
    static_cast<void>(decodeTcSettleRequest(payload));
    static_cast<void>(decodeTcKeyDelivery(payload));
  };

  tryAll(bytes);
  if (frame) {
    tryAll(frame->payload);
  }

  return 0;
}
