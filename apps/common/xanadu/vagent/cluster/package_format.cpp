/**
 * @file package_format.cpp
 * @brief Implementation of .vnprog, .vnpkg, and authority ledger routines.
 */
#include "common/xanadu/vagent/cluster/package_format.hpp"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <span>
#include <unistd.h>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"

namespace xanadu::vagent::cluster {

namespace {

namespace fs = std::filesystem;

std::string formatSizeWithCommas(std::int64_t bytes) {
  std::string s = std::to_string(bytes);
  int n         = static_cast<int>(s.length()) - 3;
  while (n > 0) {
    s.insert(static_cast<std::size_t>(n), ",");
    n -= 3;
  }
  return s;
}

} // namespace

std::string lengthPrefixed(std::string_view s) {
  std::string out;
  out.reserve(8 + s.size());
  const std::uint64_t len = s.size();
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
  }
  out.append(s);
  return out;
}

std::string computeNativeProgramRef(std::string_view opLog,
                                    std::string_view moduleDigest) {
  const std::string material =
      lengthPrefixed(xanadu::vagent::runtime::kProgramFormat) +
      lengthPrefixed("vagent-native/1") + lengthPrefixed(opLog) +
      lengthPrefixed(moduleDigest);
  return toHex32(sha256Digest(material));
}

std::string computePackageProgramRef(std::string_view kind,
                                     std::string_view canonicalBody,
                                     std::string_view targetsDigest) {
  const std::string material =
      lengthPrefixed(xanadu::vagent::runtime::kProgramFormat) +
      lengthPrefixed("vagent-package/1") + lengthPrefixed(kind) +
      lengthPrefixed(canonicalBody) + lengthPrefixed(targetsDigest);
  return toHex32(sha256Digest(material));
}

std::string encodeVnprog(const NativeProgramArtifact &artifact) {
  std::string out;
  out.append(kNativeProgramMagic);
  const std::uint32_t version = kNativeProgramFormatVersion;
  out.append(reinterpret_cast<const char *>(&version), sizeof(version));

  bencode::Dict dict;
  dict["api_version"]     = bencode::Value::integer(artifact.apiVersion);
  dict["module_digest"]   = bencode::Value::string(artifact.moduleDigest);
  dict["module_filename"] = bencode::Value::string(artifact.moduleFilename);
  dict["op_log"]          = bencode::Value::string(artifact.opLog);

  out.append(bencode::Value::dict(std::move(dict)).encode());
  return out;
}

std::expected<NativeProgramArtifact, std::string>
decodeVnprog(std::string_view bytes) {
  if (bytes.size() < kNativeProgramMagic.size()) {
    return std::unexpected("File is smaller than .vnprog magic signature");
  }
  if (!bytes.starts_with(kNativeProgramMagic)) {
    return std::unexpected("Invalid .vnprog signature");
  }

  std::size_t offset = kNativeProgramMagic.size();
  if (bytes.size() >= offset + sizeof(std::uint32_t) && bytes[offset] != 'd') {
    std::uint32_t ver{};
    std::memcpy(&ver, bytes.data() + offset, sizeof(ver));
    if (ver != kNativeProgramFormatVersion) {
      return std::unexpected(
          std::format("Unsupported .vnprog version {}", ver));
    }
    offset += sizeof(std::uint32_t);
  }

  const std::string_view bencodeBytes = bytes.substr(offset);
  bencode::Value decoded;
  try {
    std::size_t pos = 0;
    decoded         = bencode::decode(bencodeBytes, pos);
  } catch (const std::exception &e) {
    return std::unexpected(
        std::format("Corrupt bencode in .vnprog: {}", e.what()));
  }

  if (!decoded.isDict()) {
    return std::unexpected("Malformed .vnprog: root value is not a dictionary");
  }

  NativeProgramArtifact artifact;
  const auto &dict = decoded.asDict();

  auto itApi = dict.find("api_version");
  if (itApi != dict.end() && itApi->second.isInteger()) {
    artifact.apiVersion = itApi->second.asInteger();
  }

  auto itDigest = dict.find("module_digest");
  if (itDigest == dict.end() || !itDigest->second.isString()) {
    return std::unexpected("Missing module_digest in .vnprog");
  }
  artifact.moduleDigest = itDigest->second.asString();

  auto itFilename = dict.find("module_filename");
  if (itFilename != dict.end() && itFilename->second.isString()) {
    artifact.moduleFilename = itFilename->second.asString();
  }

  auto itOpLog = dict.find("op_log");
  if (itOpLog != dict.end() && itOpLog->second.isString()) {
    artifact.opLog = itOpLog->second.asString();
  }

  artifact.programRef =
      computeNativeProgramRef(artifact.opLog, artifact.moduleDigest);
  return artifact;
}

std::string encodeTargets(const std::map<std::string, PackageTarget> &targets) {
  bencode::Dict targetsDict;
  for (const auto &[triple, target] : targets) {
    bencode::Dict targetDict;

    bencode::Dict modDict;
    modDict["filename"]  = bencode::Value::string(target.module.filename);
    modDict["info_hash"] = bencode::Value::string(target.module.infoHash);
    modDict["sha256"]    = bencode::Value::string(target.module.sha256);
    modDict["size"]      = bencode::Value::integer(target.module.size);
    targetDict["module"] = bencode::Value::dict(std::move(modDict));

    bencode::List bundledList;
    for (const auto &lib : target.bundledLibs) {
      bencode::Dict libDict;
      libDict["canonical_soname"] = bencode::Value::string(lib.canonicalSoname);
      libDict["info_hash"]        = bencode::Value::string(lib.infoHash);
      libDict["name"]             = bencode::Value::string(lib.name);
      libDict["sha256"]           = bencode::Value::string(lib.sha256);
      libDict["size"]             = bencode::Value::integer(lib.size);
      libDict["soname"]           = bencode::Value::string(lib.soname);
      bundledList.push_back(bencode::Value::dict(std::move(libDict)));
    }
    targetDict["bundled_libs"] = bencode::Value::list(std::move(bundledList));

    bencode::Dict aliasesDict;
    for (const auto &[canon, alias] : target.systemLibraryAliases) {
      aliasesDict[canon] = bencode::Value::string(alias);
    }
    targetDict["system_library_aliases"] =
        bencode::Value::dict(std::move(aliasesDict));

    targetsDict[triple] = bencode::Value::dict(std::move(targetDict));
  }
  return bencode::Value::dict(std::move(targetsDict)).encode();
}

std::string
computeTargetsDigest(const std::map<std::string, PackageTarget> &targets) {
  const std::string encoded = encodeTargets(targets);
  return toHex32(sha256Digest(encoded));
}

std::string encodePackageManifest(const PackageManifest &manifest,
                                  bool standalone) {
  std::string out;
  out.append(kPackageMagic);

  bencode::Dict root;
  root["api_version"]     = bencode::Value::integer(manifest.apiVersion);
  root["compiler_format"] = bencode::Value::string(manifest.compilerFormat);
  root["format"]          = bencode::Value::string(manifest.format);
  root["kind"]            = bencode::Value::string(manifest.kind);
  root["program_body"]    = bencode::Value::string(manifest.programBody);

  // Targets
  bencode::Dict targetsDict;
  for (const auto &[triple, target] : manifest.targets) {
    bencode::Dict targetDict;

    bencode::Dict modDict;
    modDict["filename"]  = bencode::Value::string(target.module.filename);
    modDict["info_hash"] = bencode::Value::string(target.module.infoHash);
    modDict["sha256"]    = bencode::Value::string(target.module.sha256);
    modDict["size"]      = bencode::Value::integer(target.module.size);
    targetDict["module"] = bencode::Value::dict(std::move(modDict));

    bencode::List bundledList;
    for (const auto &lib : target.bundledLibs) {
      bencode::Dict libDict;
      libDict["canonical_soname"] = bencode::Value::string(lib.canonicalSoname);
      libDict["info_hash"]        = bencode::Value::string(lib.infoHash);
      libDict["name"]             = bencode::Value::string(lib.name);
      libDict["sha256"]           = bencode::Value::string(lib.sha256);
      libDict["size"]             = bencode::Value::integer(lib.size);
      libDict["soname"]           = bencode::Value::string(lib.soname);
      bundledList.push_back(bencode::Value::dict(std::move(libDict)));
    }
    targetDict["bundled_libs"] = bencode::Value::list(std::move(bundledList));

    bencode::Dict aliasesDict;
    for (const auto &[canon, alias] : target.systemLibraryAliases) {
      aliasesDict[canon] = bencode::Value::string(alias);
    }
    targetDict["system_library_aliases"] =
        bencode::Value::dict(std::move(aliasesDict));

    targetsDict[triple] = bencode::Value::dict(std::move(targetDict));
  }
  root["targets"] = bencode::Value::dict(std::move(targetsDict));

  // Provenance
  bencode::Dict prov;
  prov["author"]     = bencode::Value::string(manifest.provenance.author);
  prov["device"]     = bencode::Value::string(manifest.provenance.device);
  prov["public_key"] = bencode::Value::string(manifest.provenance.publicKey);
  prov["timestamp"]  = bencode::Value::integer(manifest.provenance.timestamp);
  root["provenance"] = bencode::Value::dict(std::move(prov));

  if (!manifest.signature.empty()) {
    root["signature"] = bencode::Value::string(manifest.signature);
  }

  if (standalone && !manifest.payloads.empty()) {
    bencode::Dict payloadsDict;
    for (const auto &[sha, payload] : manifest.payloads) {
      payloadsDict[sha] = bencode::Value::string(payload);
    }
    root["payloads"] = bencode::Value::dict(std::move(payloadsDict));
  }

  out.append(bencode::Value::dict(std::move(root)).encode());
  return out;
}

std::expected<PackageManifest, std::string>
decodePackageManifest(std::string_view bytes) {
  if (bytes.size() < kPackageMagic.size()) {
    return std::unexpected("File is smaller than .vnpkg magic header");
  }
  if (!bytes.starts_with(kPackageMagic)) {
    return std::unexpected("Invalid .vnpkg signature header");
  }

  const std::string_view bencodeBytes = bytes.substr(kPackageMagic.size());
  bencode::Value decoded;
  try {
    std::size_t pos = 0;
    decoded         = bencode::decode(bencodeBytes, pos);
  } catch (const std::exception &e) {
    return std::unexpected(
        std::format("Corrupt bencode in .vnpkg: {}", e.what()));
  }

  if (!decoded.isDict()) {
    return std::unexpected("Malformed .vnpkg: root value is not a dictionary");
  }

  PackageManifest manifest;
  const auto &dict = decoded.asDict();

  if (auto it = dict.find("api_version");
      it != dict.end() && it->second.isInteger()) {
    manifest.apiVersion = it->second.asInteger();
  }
  if (auto it = dict.find("format");
      it != dict.end() && it->second.isString()) {
    manifest.format = it->second.asString();
  }
  if (auto it = dict.find("kind"); it != dict.end() && it->second.isString()) {
    manifest.kind = it->second.asString();
  }
  if (auto it = dict.find("compiler_format");
      it != dict.end() && it->second.isString()) {
    manifest.compilerFormat = it->second.asString();
  }
  if (auto it = dict.find("program_body");
      it != dict.end() && it->second.isString()) {
    manifest.programBody = it->second.asString();
  }

  if (auto itTargets = dict.find("targets");
      itTargets != dict.end() && itTargets->second.isDict()) {
    for (const auto &[triple, targetVal] : itTargets->second.asDict()) {
      if (!targetVal.isDict()) continue;
      const auto &tDict = targetVal.asDict();
      PackageTarget target;

      if (auto itMod = tDict.find("module");
          itMod != tDict.end() && itMod->second.isDict()) {
        const auto &mDict = itMod->second.asDict();
        if (auto it = mDict.find("filename");
            it != mDict.end() && it->second.isString()) {
          target.module.filename = it->second.asString();
        }
        if (auto it = mDict.find("sha256");
            it != mDict.end() && it->second.isString()) {
          target.module.sha256 = it->second.asString();
        }
        if (auto it = mDict.find("info_hash");
            it != mDict.end() && it->second.isString()) {
          target.module.infoHash = it->second.asString();
        }
        if (auto it = mDict.find("size");
            it != mDict.end() && it->second.isInteger()) {
          target.module.size = it->second.asInteger();
        }
      }

      if (auto itBundled = tDict.find("bundled_libs");
          itBundled != tDict.end() && itBundled->second.isList()) {
        for (const auto &libVal : itBundled->second.asList()) {
          if (!libVal.isDict()) continue;
          const auto &lDict = libVal.asDict();
          PackageBundledLib lib;
          if (auto it = lDict.find("name");
              it != lDict.end() && it->second.isString()) {
            lib.name = it->second.asString();
          }
          if (auto it = lDict.find("canonical_soname");
              it != lDict.end() && it->second.isString()) {
            lib.canonicalSoname = it->second.asString();
          }
          if (auto it = lDict.find("soname");
              it != lDict.end() && it->second.isString()) {
            lib.soname = it->second.asString();
          }
          if (auto it = lDict.find("sha256");
              it != lDict.end() && it->second.isString()) {
            lib.sha256 = it->second.asString();
          }
          if (auto it = lDict.find("info_hash");
              it != lDict.end() && it->second.isString()) {
            lib.infoHash = it->second.asString();
          }
          if (auto it = lDict.find("size");
              it != lDict.end() && it->second.isInteger()) {
            lib.size = it->second.asInteger();
          }
          target.bundledLibs.push_back(std::move(lib));
        }
      }

      if (auto itAliases = tDict.find("system_library_aliases");
          itAliases != tDict.end() && itAliases->second.isDict()) {
        for (const auto &[canon, aliasVal] : itAliases->second.asDict()) {
          if (aliasVal.isString()) {
            target.systemLibraryAliases[canon] = aliasVal.asString();
          }
        }
      }

      manifest.targets[triple] = std::move(target);
    }
  }

  if (auto itProv = dict.find("provenance");
      itProv != dict.end() && itProv->second.isDict()) {
    const auto &pDict = itProv->second.asDict();
    if (auto it = pDict.find("author");
        it != pDict.end() && it->second.isString()) {
      manifest.provenance.author = it->second.asString();
    }
    if (auto it = pDict.find("device");
        it != pDict.end() && it->second.isString()) {
      manifest.provenance.device = it->second.asString();
    }
    if (auto it = pDict.find("public_key");
        it != pDict.end() && it->second.isString()) {
      manifest.provenance.publicKey = it->second.asString();
    }
    if (auto it = pDict.find("timestamp");
        it != pDict.end() && it->second.isInteger()) {
      manifest.provenance.timestamp = it->second.asInteger();
    }
  }

  if (auto it = dict.find("signature");
      it != dict.end() && it->second.isString()) {
    manifest.signature = it->second.asString();
  }

  if (auto itPayloads = dict.find("payloads");
      itPayloads != dict.end() && itPayloads->second.isDict()) {
    for (const auto &[sha, blobVal] : itPayloads->second.asDict()) {
      if (blobVal.isString()) {
        manifest.payloads[sha] = blobVal.asString();
      }
    }
  }

  return manifest;
}

bool validateSoname(std::string_view soname, std::string &error) {
  if (soname.empty()) {
    error = "SONAME cannot be empty";
    return false;
  }
  if (soname.size() > 255) {
    error = "SONAME exceeds maximum length of 255 bytes";
    return false;
  }
  if (soname[0] == '-') {
    error = "SONAME cannot begin with a hyphen";
    return false;
  }
  if (soname.find('/') != std::string_view::npos ||
      soname.find('\\') != std::string_view::npos) {
    error = "SONAME contains forbidden path separators";
    return false;
  }
  if (soname == "." || soname == "..") {
    error = "SONAME cannot be relative navigation component";
    return false;
  }

  // Regex allowlist: ^[a-zA-Z0-9_][a-zA-Z0-9._-]*\.(so(\.[0-9]+)*|dylib|dll)$
  static const std::regex sonameRegex(
      R"(^[a-zA-Z0-9_][a-zA-Z0-9._-]*\.(so(\.[0-9]+)*|dylib|dll)$)");
  if (!std::regex_match(std::string(soname), sonameRegex)) {
    error = "SONAME fails regex allowlist";
    return false;
  }
  return true;
}

bool isShadowingForbidden(std::string_view soname) {
  if (soname == "kernel32.dll" || soname == "ntdll.dll") {
    return true;
  }
  if (soname.starts_with("libc.") || soname.starts_with("libm.") ||
      soname.starts_with("libdl.") || soname.starts_with("libpthread.") ||
      soname.starts_with("librt.") || soname.starts_with("ld-") ||
      soname.starts_with("libnss_") || soname.starts_with("libSystem.")) {
    return true;
  }
  return false;
}

std::string formatInspectReport(const PackageManifest &manifest,
                                std::string_view programRef) {
  std::string out;
  out += std::format("Package Format:    {}\n", manifest.format);
  out += std::format("Kind:              {}\n", manifest.kind);
  out += std::format("Logical Ref:       sha256:{} (bare: {})\n", programRef,
                     programRef);

  std::string authorDesc = manifest.provenance.author.empty()
                               ? "anonymous"
                               : manifest.provenance.author;
  if (!manifest.provenance.publicKey.empty()) {
    authorDesc +=
        std::format(" (ed25519:{:.8}…)", manifest.provenance.publicKey);
  }
  out += std::format("Author:            {}\n", authorDesc);

  std::string sigDesc = "Unsigned";
  if (!manifest.signature.empty()) {
    sigDesc = "Valid (verified against author public key)";
  }
  out += std::format("Signature:         {}\n", sigDesc);

  for (const auto &[triple, target] : manifest.targets) {
    out += std::format("\nTarget: {}\n", triple);
    if (!target.module.filename.empty()) {
      out += std::format("  Module:          {} (size: {}, sha256: {:.12}…)\n",
                         target.module.filename,
                         formatSizeWithCommas(target.module.size),
                         target.module.sha256);
    }
    out += "  Bundled Libs (topological load order):\n";
    if (target.bundledLibs.empty()) {
      out += "    - none\n";
    } else {
      for (std::size_t i = 0; i < target.bundledLibs.size(); ++i) {
        const auto &lib = target.bundledLibs[i];
        out += std::format(
            "    {}. {}   (canonical: {}, size: {}, sha256: {:.12}…)\n", i + 1,
            lib.soname, lib.canonicalSoname, formatSizeWithCommas(lib.size),
            lib.sha256);
      }
    }
    out += "  System Library Aliases:\n";
    if (target.systemLibraryAliases.empty()) {
      out += "    - none\n";
    } else {
      for (const auto &[canon, alias] : target.systemLibraryAliases) {
        out += std::format("    - {} -> {}\n", canon, alias);
      }
    }
    out += "  System Libs (unbundled host prerequisites):\n";
    out += "    - none\n";
  }

  return out;
}

std::expected<void, std::string> recordApproval(std::string_view ledgerDir,
                                                std::string_view programRef,
                                                std::string_view kind,
                                                std::string_view author) {
  std::error_code ec;
  try {
    fs::create_directories(fs::path(ledgerDir) / "permascroll", ec);
    fs::permissions(ledgerDir, fs::perms::owner_all, fs::perm_options::replace,
                    ec);
  } catch (const std::exception &e) {
    return std::unexpected(std::format("Cannot create ledger directory {}: {}",
                                       ledgerDir, e.what()));
  }

  const fs::path logPath = fs::path(ledgerDir) / "approvals.bencode";
  bencode::List events;
  if (fs::exists(logPath)) {
    std::ifstream in(logPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    try {
      std::size_t pos = 0;
      auto val        = bencode::decode(text, pos);
      if (val.isList()) events = val.asList();
    } catch (...) {
    }
  }

  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  bencode::Dict ev;
  ev["author"]      = bencode::Value::string(std::string(author));
  ev["event"]       = bencode::Value::string("approve");
  ev["kind"]        = bencode::Value::string(std::string(kind));
  ev["program_ref"] = bencode::Value::string(std::string(programRef));
  ev["timestamp"]   = bencode::Value::integer(now);
  events.push_back(bencode::Value::dict(std::move(ev)));

  const std::string encoded = bencode::Value::list(std::move(events)).encode();
  const int fd =
      ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return std::unexpected(std::format("Cannot open {} for writing: {}",
                                       logPath.string(), std::strerror(errno)));
  }
  const ssize_t written = ::write(fd, encoded.data(), encoded.size());
  (void)written;
  ::fsync(fd);
  ::close(fd);
  fs::permissions(logPath, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  return {};
}

std::expected<void, std::string> recordWithdrawal(std::string_view ledgerDir,
                                                  std::string_view programRef,
                                                  std::string_view kind,
                                                  std::string_view author) {
  std::error_code ec;
  try {
    fs::create_directories(fs::path(ledgerDir) / "permascroll", ec);
    fs::permissions(ledgerDir, fs::perms::owner_all, fs::perm_options::replace,
                    ec);
  } catch (const std::exception &e) {
    return std::unexpected(std::format("Cannot create ledger directory {}: {}",
                                       ledgerDir, e.what()));
  }

  const fs::path logPath = fs::path(ledgerDir) / "approvals.bencode";
  bencode::List events;
  if (fs::exists(logPath)) {
    std::ifstream in(logPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    try {
      std::size_t pos = 0;
      auto val        = bencode::decode(text, pos);
      if (val.isList()) events = val.asList();
    } catch (...) {
    }
  }

  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  bencode::Dict ev;
  ev["author"]      = bencode::Value::string(std::string(author));
  ev["event"]       = bencode::Value::string("withdraw");
  ev["kind"]        = bencode::Value::string(std::string(kind));
  ev["program_ref"] = bencode::Value::string(std::string(programRef));
  ev["timestamp"]   = bencode::Value::integer(now);
  events.push_back(bencode::Value::dict(std::move(ev)));

  const std::string encoded = bencode::Value::list(std::move(events)).encode();
  const int fd =
      ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return std::unexpected(std::format("Cannot open {} for writing: {}",
                                       logPath.string(), std::strerror(errno)));
  }
  const ssize_t written = ::write(fd, encoded.data(), encoded.size());
  (void)written;
  ::fsync(fd);
  ::close(fd);
  fs::permissions(logPath, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  return {};
}

bool isApproved(std::string_view ledgerDir, std::string_view programRef,
                std::string_view kind) {
  const fs::path logPath = fs::path(ledgerDir) / "approvals.bencode";
  if (!fs::exists(logPath)) return false;

  std::ifstream in(logPath, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), {});
  bencode::List events;
  try {
    std::size_t pos = 0;
    auto val        = bencode::decode(text, pos);
    if (val.isList()) events = val.asList();
  } catch (...) {
    return false;
  }

  // Linear scan for latest event
  std::optional<std::string> latestEvent;
  for (const auto &item : events) {
    if (!item.isDict()) continue;
    const auto &d = item.asDict();
    auto itRef    = d.find("program_ref");
    auto itKind   = d.find("kind");
    auto itEv     = d.find("event");
    if (itRef != d.end() && itRef->second.isString() &&
        itRef->second.asString() == programRef && itKind != d.end() &&
        itKind->second.isString() && itKind->second.asString() == kind &&
        itEv != d.end() && itEv->second.isString()) {
      latestEvent = itEv->second.asString();
    }
  }

  return latestEvent.has_value() && *latestEvent == "approve";
}

} // namespace xanadu::vagent::cluster
