/**
 * @file xuzz_app.hpp
 * @brief Sovereign application orchestrator unifying Xanadoc and Zigzag.
 */
#ifndef XUZZ_APP_HPP
#define XUZZ_APP_HPP

#include <memory>
#include <string>

namespace xuzz {

struct CliOptions;

class XuzzApp {
public:
  XuzzApp();
  ~XuzzApp();

  XuzzApp(const XuzzApp &)            = delete;
  XuzzApp &operator=(const XuzzApp &) = delete;

  int run(int argc, char **argv);

private:
  int executeCheckAuthorship(const std::string &where);
  int executeRaster(const std::string &slicePath, const std::string &storePath);
  int executeLensRaster(const std::string &lensPath);
  int executeSessionExport(const CliOptions &opts);
  int executeSessionImport(CliOptions &opts, int argc, char **argv);
};

} // namespace xuzz

#endif // XUZZ_APP_HPP
