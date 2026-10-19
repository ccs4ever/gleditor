/**
 * @file proc_lens.cpp
 * @brief Implementation of ProcLens.
 */
#include "common/xanadu/zigzag/proc_lens.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace zigzag {

ProcLens::ProcLens(std::vector<ProcessSnapshot> syntheticTree)
    : processes_(std::move(syntheticTree)) {}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
ProcLens::dimensions() const {
  return {
      {"d.child", GenericDim::Containment},
      {"d.sibling", GenericDim::Sequence},
      {"d.threads", std::nullopt},
      {"d.state", std::nullopt},
      {"d.cpu", std::nullopt},
      {"d.rss", std::nullopt},
  };
}

void ProcLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  for (const auto &proc : processes_) {
    const std::string key = "proc:" + std::to_string(proc.pid);

    SourceNode node{
        .key     = key,
        .content = proc.comm,
        .attributes =
            {
                {"d.pid", std::to_string(proc.pid)},
                {"d.ppid", std::to_string(proc.ppid)},
                {"d.state", std::string(1, proc.state)},
                {"d.cpu", std::to_string(proc.cpuPercent)},
                {"d.rss", std::to_string(proc.rssBytes)},
            },
        .order = proc.pid,
    };
    emit(node);

    // Thread nodes
    for (const auto tid : proc.threadTids) {
      if (tid == proc.pid) {
        continue; // Primary thread represented by process node
      }
      const std::string tKey = "thread:" + std::to_string(tid);
      SourceNode tNode{
          .key     = tKey,
          .content = "thread:" + std::to_string(tid),
          .attributes =
              {
                  {"d.tid", std::to_string(tid)},
                  {"d.tgid", std::to_string(proc.pid)},
              },
          .order = tid,
      };
      emit(tNode);
    }
  }
}

void ProcLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  // Group processes by parent PID
  std::map<std::uint32_t, std::vector<std::uint32_t>> childrenByParent;
  for (const auto &proc : processes_) {
    childrenByParent[proc.ppid].push_back(proc.pid);
  }

  for (auto &[ppid, children] : childrenByParent) {
    if (children.empty()) {
      continue;
    }
    // Sort children by PID for deterministic birth order
    std::sort(children.begin(), children.end());

    const std::string parentKey    = "proc:" + std::to_string(ppid);
    const std::string firstbornKey = "proc:" + std::to_string(children.front());

    // 1. Parent links to firstborn child along d.child
    emit(SourceEdge{
        .from      = parentKey,
        .to        = firstbornKey,
        .dimension = "d.child",
        .dir       = DimVector::POS,
    });

    // 2. Siblings link to one another along orthogonal d.sibling
    for (std::size_t i = 0; i + 1 < children.size(); ++i) {
      emit(SourceEdge{
          .from      = "proc:" + std::to_string(children[i]),
          .to        = "proc:" + std::to_string(children[i + 1]),
          .dimension = "d.sibling",
          .dir       = DimVector::POS,
      });
    }
  }

  // Thread ranks along d.threads
  for (const auto &proc : processes_) {
    if (proc.threadTids.empty()) {
      continue;
    }
    std::string currKey = "proc:" + std::to_string(proc.pid);
    for (const auto tid : proc.threadTids) {
      if (tid == proc.pid) {
        continue;
      }
      const std::string nextThreadKey = "thread:" + std::to_string(tid);
      emit(SourceEdge{
          .from      = currKey,
          .to        = nextThreadKey,
          .dimension = "d.threads",
          .dir       = DimVector::POS,
      });
      currKey = nextThreadKey;
    }
  }
}

void ProcLens::setProcessTree(std::vector<ProcessSnapshot> tree) {
  processes_ = std::move(tree);
}

void ProcLens::refreshFromHost() {
  std::vector<ProcessSnapshot> liveProcs;
  namespace fs = std::filesystem;

  std::error_code ec;
  if (!fs::exists("/proc", ec)) {
    return;
  }

  for (const auto &entry : fs::directory_iterator("/proc", ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    const auto name   = entry.path().filename().string();
    std::uint32_t pid = 0;
    const auto [ptr, rEc] =
        std::from_chars(name.data(), name.data() + name.size(), pid);
    if (rEc != std::errc{} || pid == 0) {
      continue;
    }

    std::ifstream statFile(entry.path() / "stat");
    if (!statFile.is_open()) {
      continue;
    }

    std::string statLine;
    if (!std::getline(statFile, statLine)) {
      continue;
    }

    // Parse /proc/[pid]/stat: pid (comm) state ppid ...
    const auto openParen  = statLine.find('(');
    const auto closeParen = statLine.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos ||
        closeParen <= openParen) {
      continue;
    }

    const std::string comm =
        statLine.substr(openParen + 1, closeParen - openParen - 1);
    const std::string rest = statLine.substr(closeParen + 2);

    std::istringstream restStream(rest);
    char state         = 'R';
    std::uint32_t ppid = 0;
    restStream >> state >> ppid;

    liveProcs.push_back(ProcessSnapshot{
        .pid        = pid,
        .ppid       = ppid,
        .comm       = comm,
        .state      = state,
        .cpuPercent = 0.0,
        .rssBytes   = 4096,
        .threadTids = {pid},
    });
  }

  if (!liveProcs.empty()) {
    processes_ = std::move(liveProcs);
  }
}

std::expected<void, std::string>
ProcLens::dispatchCommand(const std::uint32_t pid,
                          const std::string_view command) {
  const auto it =
      std::find_if(processes_.begin(), processes_.end(),
                   [pid](const ProcessSnapshot &p) { return p.pid == pid; });
  if (it == processes_.end()) {
    return std::unexpected(std::format("Process {} does not exist", pid));
  }

  if (command == "kill" || command.starts_with("kill ")) {
    it->state = 'Z'; // Mark terminated/zombie
    return {};
  }
  if (command == "pause") {
    it->state = 'T'; // Stopped
    return {};
  }
  if (command == "resume") {
    it->state = 'R'; // Running
    return {};
  }

  return std::unexpected(std::format("Unknown control action: '{}'", command));
}

} // namespace zigzag
