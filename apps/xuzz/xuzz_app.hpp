/**
 * @file xuzz_app.hpp
 * @brief Sovereign application orchestrator unifying Xanadoc and Zigzag.
 */
#ifndef XUZZ_APP_HPP
#define XUZZ_APP_HPP

#include <memory>
#include <string>

namespace xuzz {

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
};

} // namespace xuzz

#endif // XUZZ_APP_HPP
