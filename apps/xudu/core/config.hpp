/**
 * @file config.hpp
 * @brief What this person publishes as, kept where their other settings are.
 *
 * The authorship record needs a name, an email address and something to sign
 * with. None of those belong to a store: somebody has one identity and many
 * documents, and being asked to state it again for each of them is how it ends
 * up spelled three different ways -- or omitted, which is worse, since an
 * unsigned document is one nobody has put their name to.
 *
 * So it lives in the per-user configuration directory, under whatever XDG says
 * that is, in the same small YAML the authorship record is written in. A store
 * may still override it -- one might publish under a pen name -- and the
 * publish dialog may override that for a single publication, which is what the
 * layering is for: the file is what somebody is by default, not a rule.
 */
#ifndef XUDU_CONFIG_H
#define XUDU_CONFIG_H

#include <string>

#include "provenance.hpp"

namespace xudu {

/**
 * @brief The settings a person keeps rather than a document does.
 */
struct Config {
  /// Name, email, and which key in the keyring signs. See Author.
  Author author;

  /**
   * @brief A GnuPG home directory to sign from, when not the usual one.
   *
   * For somebody whose publishing key is kept apart from their everyday
   * keyring -- on removable media, say. Empty means whatever gpg would use on
   * its own, which is what almost everybody wants.
   */
  std::string gpgHome;

  /// Whether there is enough here to sign with.
  [[nodiscard]] bool complete() const { return author.named(); }

  [[nodiscard]] std::string toYaml() const;
  /// Read one back. Nothing when the text is not of that shape; a Config with
  /// empty fields is a different answer and means the file said nothing.
  [[nodiscard]] static std::optional<Config> fromYaml(std::string_view text);

  /**
   * @brief How this record asks to be signed.
   *
   * @param passphrase What was typed for this one signature, when the key
   *        needs one and no agent is holding it. Never kept: a passphrase in a
   *        settings file is a passphrase somebody else can read.
   */
  [[nodiscard]] SigningOptions signing(std::string passphrase = {}) const {
    return SigningOptions{gpgHome, std::move(passphrase)};
  }
};

/**
 * @brief Where the configuration file is.
 *
 * `$XDG_CONFIG_HOME/xudu/config.yaml`, falling back to `~/.config/xudu` as the
 * specification says to. `$XUDU_CONFIG` names a file directly, which is what
 * lets a test -- or somebody with two identities -- point at another one
 * without touching what is in the home directory.
 */
[[nodiscard]] std::string configPath();

/// Read the configuration, or an empty one when there is no file. A file that
/// cannot be understood is an error rather than an empty configuration: it was
/// written by somebody who meant something by it.
[[nodiscard]] Config loadConfig(const std::string &path = configPath());

/// Write it, creating the directory. The file is readable only by its owner:
/// it says who somebody is and where their signing key lives.
void saveConfig(const Config &config, const std::string &path = configPath());

} // namespace xudu

#endif // XUDU_CONFIG_H
