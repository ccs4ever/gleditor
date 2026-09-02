#include "pgp_verify.hpp"

#include <cstdint>
#include <memory>
#include <rnp/rnp.h>
// rnp.h declares every function as returning rnp_result_t but does not pull in
// the header defining RNP_SUCCESS, so this is not redundant.
#include <rnp/rnp_err.h>

namespace xudu::identity::pgp {

namespace {

// RNP's handles are C objects with explicit destroy calls. Wrapped the same
// way transcopyright_crypto.cpp wraps OpenSSL's, so that the early returns
// below -- and there are many, because every step can fail -- cannot leak.
struct FfiCloser {
  void operator()(rnp_ffi_t ffi) const noexcept {
    if (ffi) {
      rnp_ffi_destroy(ffi);
    }
  }
};
using ScopedFfi = std::unique_ptr<std::remove_pointer_t<rnp_ffi_t>, FfiCloser>;

struct InputCloser {
  void operator()(rnp_input_t in) const noexcept {
    if (in) {
      rnp_input_destroy(in);
    }
  }
};
using ScopedInput =
    std::unique_ptr<std::remove_pointer_t<rnp_input_t>, InputCloser>;

struct VerifyCloser {
  void operator()(rnp_op_verify_t op) const noexcept {
    if (op) {
      rnp_op_verify_destroy(op);
    }
  }
};
using ScopedVerify =
    std::unique_ptr<std::remove_pointer_t<rnp_op_verify_t>, VerifyCloser>;

struct IteratorCloser {
  void operator()(rnp_identifier_iterator_t it) const noexcept {
    if (it) {
      rnp_identifier_iterator_destroy(it);
    }
  }
};
using ScopedIterator =
    std::unique_ptr<std::remove_pointer_t<rnp_identifier_iterator_t>,
                    IteratorCloser>;

ScopedInput inputFrom(const std::string_view bytes) {
  rnp_input_t raw = nullptr;
  // Copies, because RNP outlives the string_view in every caller here.
  if (rnp_input_from_memory(&raw,
                            reinterpret_cast<const std::uint8_t *>(bytes.data()),
                            bytes.size(), true) != RNP_SUCCESS) {
    return nullptr;
  }
  return ScopedInput{raw};
}

/// A fresh keyring holding only @p armoredKey. Per call rather than shared:
/// one peer's key must never be able to satisfy a lookup made on behalf of
/// another, and an empty ring each time is the cheapest way to guarantee it.
ScopedFfi ringWith(const std::string_view armoredKey) {
  if (armoredKey.empty()) {
    return nullptr;
  }
  rnp_ffi_t rawFfi = nullptr;
  if (rnp_ffi_create(&rawFfi, "GPG", "GPG") != RNP_SUCCESS) {
    return nullptr;
  }
  ScopedFfi ffi{rawFfi};

  const auto keyInput = inputFrom(armoredKey);
  if (!keyInput) {
    return nullptr;
  }
  if (rnp_import_keys(ffi.get(), keyInput.get(), RNP_LOAD_SAVE_PUBLIC_KEYS,
                      nullptr) != RNP_SUCCESS) {
    return nullptr;
  }
  return ffi;
}

} // namespace

std::optional<Fingerprint> fingerprintOf(const std::string_view armoredKey) {
  const auto ffi = ringWith(armoredKey);
  if (!ffi) {
    return std::nullopt;
  }

  rnp_identifier_iterator_t rawIt = nullptr;
  if (rnp_identifier_iterator_create(ffi.get(), &rawIt, "fingerprint") !=
      RNP_SUCCESS) {
    return std::nullopt;
  }
  ScopedIterator it{rawIt};

  const char *identifier = nullptr;
  if (rnp_identifier_iterator_next(it.get(), &identifier) != RNP_SUCCESS ||
      nullptr == identifier) {
    return std::nullopt;
  }
  // The iterator's storage belongs to it, so this has to be read before the
  // iterator is destroyed -- which Fingerprint::fromString does by copying.
  return Fingerprint::fromString(identifier);
}

bool keyMatchesFingerprint(const std::string_view armoredKey,
                           const Fingerprint &claimed) {
  const auto actual = fingerprintOf(armoredKey);
  return actual.has_value() && *actual == claimed;
}

bool verifyDetached(const std::string_view armoredKey,
                    const std::string_view message,
                    const std::string_view armoredSignature) {
  if (message.empty() || armoredSignature.empty()) {
    return false;
  }
  const auto ffi = ringWith(armoredKey);
  if (!ffi) {
    return false;
  }

  const auto dataInput = inputFrom(message);
  const auto sigInput  = inputFrom(armoredSignature);
  if (!dataInput || !sigInput) {
    return false;
  }

  rnp_op_verify_t rawOp = nullptr;
  if (rnp_op_verify_detached_create(&rawOp, ffi.get(), dataInput.get(),
                                    sigInput.get()) != RNP_SUCCESS) {
    return false;
  }
  ScopedVerify op{rawOp};

  // execute() reports failure for a bad signature as well as for a malformed
  // one, so its result alone would be enough -- but the per-signature status
  // below is checked anyway, because "no signatures at all" also executes
  // cleanly and must not read as verified.
  if (rnp_op_verify_execute(op.get()) != RNP_SUCCESS) {
    return false;
  }

  std::size_t count = 0;
  if (rnp_op_verify_get_signature_count(op.get(), &count) != RNP_SUCCESS ||
      0 == count) {
    return false;
  }

  for (std::size_t i = 0; i < count; i++) {
    rnp_op_verify_signature_t sig = nullptr;
    if (rnp_op_verify_get_signature_at(op.get(), i, &sig) != RNP_SUCCESS) {
      return false;
    }
    if (rnp_op_verify_signature_get_status(sig) != RNP_SUCCESS) {
      return false;
    }
  }
  return true;
}

} // namespace xudu::identity::pgp
