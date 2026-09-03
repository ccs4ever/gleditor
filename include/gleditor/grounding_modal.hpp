/**
 * @file grounding_modal.hpp
 * @brief Modal form for prompting, selecting, and verifying parent source media
 * for external quotes and clips.
 */
#ifndef GLEDITOR_GROUNDING_MODAL_HPP
#define GLEDITOR_GROUNDING_MODAL_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/form.hpp>
#include <gleditor/source_grounder.hpp>

namespace gleditor {

/**
 * @class GroundingModal
 * @brief Manages interactive provenance grounding for imported/pasted excerpts.
 */
class GroundingModal {
public:
  using OnGrounded = std::function<void(SubspanMatch)>;
  using OnReject   = std::function<void(std::string reason)>;

  explicit GroundingModal(std::string fontName = "Sans 12");
  ~GroundingModal() = default;

  [[nodiscard]] Form &form() { return form_; }
  [[nodiscard]] const Form &form() const { return form_; }

  /**
   * @brief Present modal to ground a text quotation in a parent source.
   *
   * Rejects paste if dismissed (Escape), left empty, or unmatched.
   */
  void openForQuote(std::string quoteSnippet,
                    std::vector<KnownSource> knownSources,
                    OnGrounded onGrounded, OnReject onReject);

  /**
   * @brief Present modal to ground a binary excerpt in a parent source.
   */
  void openForBinary(std::vector<std::uint8_t> excerptBytes,
                     std::vector<KnownSource> knownSources,
                     OnGrounded onGrounded, OnReject onReject);

private:
  Form form_;
};

} // namespace gleditor

#endif // GLEDITOR_GROUNDING_MODAL_HPP
