#ifndef TESTS_XUZZ_VQL_CAPTURE_WORLD_HPP
#define TESTS_XUZZ_VQL_CAPTURE_WORLD_HPP

// A file host with a real permascroll, capture store, journal and script
// runner: what a script that captures needs, shared by the capture suites so
// each tests only what it adds (design/vortex-io-m7-vql-c.md §17).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "io_frontend_conformance.hpp"

namespace vql_capture_world {

using namespace xuzz_conformance;
namespace fs = std::filesystem;
using xanadu::vortex::CaptureByteSink;
using xanadu::vortex::CaptureHostOptions;
using xanadu::vortex::CapturePublisher;
using xanadu::vortex::UserPermascrollCaptureSink;
using xanadu::vql::Parser;
using xanadu::vql::ScriptRunner;
using xanadu::vql::ScriptRunResult;
using xanadu::vql::VQLEngine;

inline auto read(const std::string &cap, const std::string &range,
                 const std::string &name, const std::string &within = {})
    -> std::string {
  return "effect { let $s := submit($" + cap + ", read_bytes(" + range + "))" +
         (within.empty() ? "" : " within " + within) +
         "; match $s { admitted($q) => await($q), refused($e) => $e } } as $" +
         name + "; ";
}

inline auto open(const std::string &path, const std::string &name)
    -> std::string {
  return "effect { open(\"" + path + "\") } as $" + name + "; ";
}

inline auto captureOf(const std::string &source, const std::string &name,
                      const std::string &range = {}) -> std::string {
  return "effect { capture($" + source + (range.empty() ? "" : ", " + range) +
         ") } as $" + name + "; ";
}

/// A sink over the real permascroll whose append waits at a gate, so a
/// capture can be caught between its intent and its bytes.
class GatedSink final : public CaptureByteSink {
public:
  explicit GatedSink(xanadu::UserPermascroll &permascroll)
      : inner_(permascroll) {}
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    {
      std::unique_lock lock(mutex_);
      ++entered_;
      changed_.notify_all();
      changed_.wait(lock, [&] { return !closed_; });
    }
    return inner_.append(bytes);
  }
  auto sync() -> bool override { return inner_.sync(); }
  auto highWaterBytes() const -> std::uint64_t override {
    return inner_.highWaterBytes();
  }
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    return inner_.read(span);
  }
  void close() {
    std::scoped_lock lock(mutex_);
    closed_ = true;
  }
  void open() {
    {
      std::scoped_lock lock(mutex_);
      closed_ = false;
    }
    changed_.notify_all();
  }
  [[nodiscard]] auto entered() -> int {
    std::scoped_lock lock(mutex_);
    return entered_;
  }
  /// Wait until a worker is parked inside append().
  auto waitEntered(const int count = 1) -> bool {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(10),
                             [&] { return entered_ >= count; });
  }

private:
  UserPermascrollCaptureSink inner_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool closed_{false};
  int entered_{};
};

/// Everything a capture script needs: a file host, a real permascroll, a
/// capture store and journal, and a runner.
struct CaptureWorld {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLEngine engine{core};
  fs::path base;
  std::shared_ptr<xanadu::UserPermascroll> perma;
  std::unique_ptr<GatedSink> sink;
  std::unique_ptr<xanadu::Store> store;
  std::unique_ptr<CapturePublisher> publisher;
  std::unique_ptr<xanadu::vortex::FileReadHost> host;
  std::unique_ptr<ScriptRunner> runner;
  /// A crash test keeps the directory for the next incarnation to recover.
  bool keepFiles{false};

  explicit CaptureWorld(
      const std::size_t retained =
          xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
      const bool attach = true, const fs::path &reuse = {}) {
    static std::uint32_t counter = 0;
    base = reuse.empty() ? fs::weakly_canonical(fs::temp_directory_path()) /
                               ("vql_io_capture_" + std::to_string(::getpid()) +
                                "_" + std::to_string(counter++))
                         : reuse;
    fs::create_directories(base / "data");
    xanadu::UserPermascroll::Config config;
    config.storageDir = base / "permascroll";
    perma             = std::make_shared<xanadu::UserPermascroll>(config);
    sink              = std::make_unique<GatedSink>(*perma);
    // A restart over the same directory finds the store the last incarnation
    // saved, as vquery's own stack does.
    store = fs::exists(base / "capture-store" / "ops.nodes")
                ? xanadu::loadStore(base / "capture-store", perma)
                : std::make_unique<xanadu::Store>(perma);
    publisher =
        std::make_unique<CapturePublisher>(*store, base / "capture-store");
    xanadu::vortex::FileReadHostOptions options;
    options.retainedPayloadBytes = retained;
    host = std::make_unique<xanadu::vortex::FileReadHost>(vm, arena, options);
    if (attach) {
      host->attachCapture(
          *sink, publisher.get(),
          CaptureHostOptions{.journalPath = base / "capture.journal"});
    }
    runner = std::make_unique<ScriptRunner>(
        engine, vm, xanadu::vql::fileScriptPorts(*host));
  }
  ~CaptureWorld() {
    sink->open();
    runner.reset();
    host.reset();
    if (!keepFiles) {
      std::error_code ec;
      fs::remove_all(base, ec);
    }
  }
  auto put(const std::string &name, const std::string &content) -> std::string {
    const auto path = base / "data" / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    return path.string();
  }
  auto start(const std::string &source) -> ScriptRunResult {
    return runner->run(Parser(source).parseQuery());
  }
  auto run(const std::string &source)
      -> std::expected<std::vector<zigzag::CellRef>, FrontendDiagnostic> {
    return xanadu::vql::driveToCompletion(
        *runner, start(source), [&] { host->pump(16); },
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  }
  auto rows(const std::string &source) -> std::vector<std::string> {
    const auto result = run(source);
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    std::vector<std::string> out;
    if (result)
      for (const auto cell : *result) out.push_back(arena.textOf(cell));
    return out;
  }
  auto failure(const std::string &source) -> std::string {
    const auto result = run(source);
    return result ? std::string{} : result.error().code;
  }
  [[nodiscard]] auto permascrollBytes() const -> std::uint64_t {
    return sink->highWaterBytes();
  }
  /// What the captures appended: the lengths on the journal's Append intents.
  /// The permascroll also grows by the publication's own scalar text, which is
  /// not capture content, so its size is not the measure.
  [[nodiscard]] auto appendedBytes() const -> std::uint64_t {
    xanadu::vortex::CaptureJournal journal(base / "capture.journal",
                                           std::nullopt);
    std::uint64_t total = 0;
    for (const auto &record : journal.recover().records) {
      if (record.kind == xanadu::vortex::CaptureJournalRecordKind::Append)
        total += record.appendLength;
    }
    return total;
  }
  auto captured(const std::string &prefix) -> std::vector<zigzag::CellRef> {
    const auto result = run(prefix + "return io_captured($c)");
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().code + ": " + result.error().message);
    return result ? *result : std::vector<zigzag::CellRef>{};
  }
  /// The text a captured cell quotes, read from the permascroll.
  [[nodiscard]] auto quotedText(const zigzag::CellRef cell) const
      -> std::string {
    return arena.textOf(cell, *perma);
  }
  [[nodiscard]] auto coordinator() -> xanadu::vortex::CaptureCoordinator & {
    return host->capture()->coordinator();
  }
};
} // namespace vql_capture_world

#endif
