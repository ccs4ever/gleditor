/**
 * @file payment_verifier.hpp
 * @brief The seam between the transcopyright protocol and actually being paid.
 *
 * Nelson's transcopyright settles a reader's micropayment directly to the
 * origin author, with no platform in between. This program implements every
 * part of that except the money: there is no xucoin here, no state channel,
 * no Lightning node. What a `micropaymentTicket` should contain, and what
 * would make one good, is a question about a settlement system that does not
 * exist yet.
 *
 * So the protocol is real and the payment is a seam. That is a deliberate
 * choice and not a stub left by accident: the encryption, the key delivery,
 * the challenge freshness and the replay protection are all things that can
 * be got right today and be wrong in ways that matter, and none of them
 * should wait on a currency being picked.
 *
 * The one thing a seam like this must not do is quietly look like the real
 * thing, which is why AlwaysAcceptVerifier says so on every call.
 */
#ifndef XUDU_IDENTITY_PAYMENT_VERIFIER_HPP
#define XUDU_IDENTITY_PAYMENT_VERIFIER_HPP

#include <string>
#include <string_view>

#include "identity_layout.hpp"

namespace xudu::identity {

/**
 * @class PaymentVerifier
 * @brief Decides whether a settlement request has actually been paid for.
 *
 * Implementations answer for one settlement system. They are asked once per
 * request, after the protocol has already established that the request is
 * fresh, addressed to a span this node sells, and quotes the challenge this
 * node issued -- so an implementation only has to answer the money question.
 */
class PaymentVerifier {
public:
  PaymentVerifier()                                   = default;
  virtual ~PaymentVerifier()                          = default;
  PaymentVerifier(const PaymentVerifier &)            = delete;
  PaymentVerifier &operator=(const PaymentVerifier &) = delete;
  PaymentVerifier(PaymentVerifier &&)                 = delete;
  PaymentVerifier &operator=(PaymentVerifier &&)      = delete;

  /**
   * @brief Whether @p request settles @p priceAtomicUnits.
   *
   * @param request The peer's settlement request, including its ticket.
   * @param priceAtomicUnits What the invoice asked for.
   * @return true only if the author has really been paid.
   */
  [[nodiscard]] virtual bool verify(const TcSettleRequestMsg &request,
                                    std::uint64_t priceAtomicUnits) = 0;

  /// Named in logs, so it is always visible which one is answering.
  [[nodiscard]] virtual std::string_view name() const = 0;

  /// Whether this verifier actually checks anything. False makes the node
  /// announce itself as giving content away, rather than leaving a reader to
  /// discover it.
  [[nodiscard]] virtual bool isReal() const = 0;
};

/**
 * @class AlwaysAcceptVerifier
 * @brief Accepts every payment. For development, and loud about it.
 *
 * Every call logs. Running a public seeder on this gives away paywalled
 * content to anyone who asks, and the log is the only thing that will say so.
 */
class AlwaysAcceptVerifier final : public PaymentVerifier {
public:
  [[nodiscard]] bool verify(const TcSettleRequestMsg &request,
                            std::uint64_t priceAtomicUnits) override;

  [[nodiscard]] std::string_view name() const override {
    return "always-accept (development)";
  }

  [[nodiscard]] bool isReal() const override { return false; }

  /// How many payments have been waved through, for tests and diagnostics.
  [[nodiscard]] std::uint64_t acceptedCount() const noexcept {
    return accepted_;
  }

private:
  std::uint64_t accepted_{0};
};

/**
 * @class RefusingVerifier
 * @brief Refuses every payment. The default.
 *
 * A node that has not been told how it gets paid has not been told how it
 * gets paid, and the safe reading of that is that it does not sell anything
 * -- not that it gives everything away. Configuring a verifier is how an
 * author opts into selling.
 */
class RefusingVerifier final : public PaymentVerifier {
public:
  [[nodiscard]] bool verify(const TcSettleRequestMsg & /*request*/,
                            std::uint64_t /*priceAtomicUnits*/) override {
    return false;
  }

  [[nodiscard]] std::string_view name() const override {
    return "refusing (no settlement configured)";
  }

  [[nodiscard]] bool isReal() const override { return false; }
};

} // namespace xudu::identity

#endif // XUDU_IDENTITY_PAYMENT_VERIFIER_HPP
