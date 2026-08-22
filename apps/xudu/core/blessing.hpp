/**
 * @file blessing.hpp
 * @brief Author endorsements / blessings of third-party link packages.
 */
#ifndef XUDU_BLESSING_HPP
#define XUDU_BLESSING_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mutable_link.hpp"
#include "swarm.hpp"

namespace xudu {

/**
 * @struct Blessing
 * @brief An author endorsement / blessing of a third-party link package.
 *
 * Authors and trusted curators can issue signed endorsements that elevate
 * public link packages in the prominence ranking.
 */
struct Blessing {
  PublicKey author;
  std::string targetDocument;  ///< e.g. "btpk:<author_pubkey>:<salt>"
  std::string endorsedPackage; ///< e.g. "btpk:<curator_pubkey>:<salt>"
  std::string note;
  std::uint64_t timestamp{};

  Signature signature;

  [[nodiscard]] std::string describe() const;
};

[[nodiscard]] std::string blessingSigningBuffer(const Blessing &blessing);
[[nodiscard]] std::string encodeBlessing(const Blessing &blessing);
[[nodiscard]] std::optional<Blessing> decodeBlessing(std::string_view encoded);
[[nodiscard]] bool verifyBlessing(const Blessing &blessing);

[[nodiscard]] Blessing createBlessing(const MutableKeys &keys,
                                      std::string targetDocument,
                                      std::string endorsedPackage,
                                      std::string note,
                                      std::uint64_t timestamp);

} // namespace xudu

#endif // XUDU_BLESSING_HPP
