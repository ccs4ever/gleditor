/**
 * @file package_inspector.cpp
 * @brief Implementation of ELF binary verification, shadowing enforcement, and
 * topological dependency sorting.
 */
#include "common/xanadu/vagent/cluster/package_inspector.hpp"

#include <algorithm>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <sstream>

#include "common/xanadu/vagent/cluster/package_format.hpp"

namespace xanadu::vagent::cluster {

namespace {

bool isRpathTokenPermitted(std::string_view token) noexcept {
  return token == "$ORIGIN" || token == "$ORIGIN/lib" || token == "${ORIGIN}" ||
         token == "${ORIGIN}/lib";
}

bool validateRpathString(std::string_view pathStr) noexcept {
  std::size_t start = 0;
  while (start < pathStr.size()) {
    const std::size_t delim      = pathStr.find(':', start);
    const std::string_view token = (delim == std::string_view::npos)
                                       ? pathStr.substr(start)
                                       : pathStr.substr(start, delim - start);
    if (!token.empty() && !isRpathTokenPermitted(token)) {
      return false;
    }
    if (delim == std::string_view::npos) break;
    start = delim + 1;
  }
  return true;
}

std::string getStringFromTable(std::span<const std::uint8_t> strtab,
                               std::size_t offset) {
  if (offset >= strtab.size()) return "";
  const char *start = reinterpret_cast<const char *>(strtab.data() + offset);
  const std::size_t maxLen = strtab.size() - offset;
  const std::size_t len    = ::strnlen(start, maxLen);
  return std::string(start, len);
}

// Map a virtual address to file offset using ELF PT_LOAD headers
std::size_t vaddrToOffset64(std::span<const std::uint8_t> bytes,
                            const Elf64_Ehdr *ehdr, std::uint64_t vaddr) {
  if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;
  const std::size_t phEnd = ehdr->e_phoff + sizeof(Elf64_Phdr) * ehdr->e_phnum;
  if (phEnd > bytes.size()) return 0;

  const auto *phdrs =
      reinterpret_cast<const Elf64_Phdr *>(bytes.data() + ehdr->e_phoff);
  for (std::uint16_t i = 0; i < ehdr->e_phnum; ++i) {
    if (phdrs[i].p_type == PT_LOAD) {
      if (vaddr >= phdrs[i].p_vaddr &&
          vaddr < phdrs[i].p_vaddr + phdrs[i].p_filesz) {
        return phdrs[i].p_offset + (vaddr - phdrs[i].p_vaddr);
      }
    }
  }
  return 0;
}

std::expected<InspectedElf, InspectorError>
inspectElf64(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(Elf64_Ehdr)) {
    return std::unexpected(InspectorError::CorruptBinary);
  }

  const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(bytes.data());
  InspectedElf info;

  // 1. Program headers scan
  const Elf64_Phdr *dynamicPhdr = nullptr;
  if (ehdr->e_phoff > 0 && ehdr->e_phnum > 0) {
    const std::size_t phTableSize = sizeof(Elf64_Phdr) * ehdr->e_phnum;
    if (ehdr->e_phoff + phTableSize > bytes.size()) {
      return std::unexpected(InspectorError::CorruptBinary);
    }
    const auto *phdrs =
        reinterpret_cast<const Elf64_Phdr *>(bytes.data() + ehdr->e_phoff);
    for (std::uint16_t i = 0; i < ehdr->e_phnum; ++i) {
      if (phdrs[i].p_type == PT_INTERP) {
        info.hasInterp = true;
      } else if (phdrs[i].p_type == PT_DYNAMIC) {
        dynamicPhdr = &phdrs[i];
      }
    }
  }

  // 2. Section headers scan for SHT_DYNAMIC & SHT_STRTAB
  const Elf64_Shdr *dynamicShdr = nullptr;
  const Elf64_Shdr *dynstrShdr  = nullptr;

  if (ehdr->e_shoff > 0 && ehdr->e_shnum > 0) {
    const std::size_t shTableSize = sizeof(Elf64_Shdr) * ehdr->e_shnum;
    if (ehdr->e_shoff + shTableSize <= bytes.size()) {
      const auto *shdrs =
          reinterpret_cast<const Elf64_Shdr *>(bytes.data() + ehdr->e_shoff);
      for (std::uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        if (shdrs[i].sh_type == SHT_DYNAMIC) {
          dynamicShdr = &shdrs[i];
          if (shdrs[i].sh_link < ehdr->e_shnum) {
            dynstrShdr = &shdrs[shdrs[i].sh_link];
          }
        }
      }
    }
  }

  // Locate dynamic entries
  const Elf64_Dyn *dynEntries = nullptr;
  std::size_t dynCount        = 0;

  if (dynamicShdr != nullptr) {
    if (dynamicShdr->sh_offset + dynamicShdr->sh_size > bytes.size()) {
      return std::unexpected(InspectorError::CorruptBinary);
    }
    dynEntries = reinterpret_cast<const Elf64_Dyn *>(bytes.data() +
                                                     dynamicShdr->sh_offset);
    dynCount   = dynamicShdr->sh_size / sizeof(Elf64_Dyn);
  } else if (dynamicPhdr != nullptr) {
    if (dynamicPhdr->p_offset + dynamicPhdr->p_filesz > bytes.size()) {
      return std::unexpected(InspectorError::CorruptBinary);
    }
    dynEntries = reinterpret_cast<const Elf64_Dyn *>(bytes.data() +
                                                     dynamicPhdr->p_offset);
    dynCount   = dynamicPhdr->p_filesz / sizeof(Elf64_Dyn);
  }

  if (dynEntries == nullptr || dynCount == 0) {
    // Shared libraries must have a dynamic section
    return info;
  }

  // Locate string table
  std::span<const std::uint8_t> strtab;

  if (dynstrShdr != nullptr &&
      dynstrShdr->sh_offset + dynstrShdr->sh_size <= bytes.size()) {
    strtab = bytes.subspan(dynstrShdr->sh_offset, dynstrShdr->sh_size);
  } else {
    // Resolve DT_STRTAB and DT_STRSZ from dynamic entries
    std::uint64_t strtabVaddr = 0;
    std::uint64_t strtabSize  = 0;
    for (std::size_t i = 0; i < dynCount; ++i) {
      if (dynEntries[i].d_tag == DT_STRTAB) {
        strtabVaddr = dynEntries[i].d_un.d_ptr;
      } else if (dynEntries[i].d_tag == DT_STRSZ) {
        strtabSize = dynEntries[i].d_un.d_val;
      }
    }
    if (strtabVaddr != 0 && strtabSize != 0) {
      const std::size_t strOffset = vaddrToOffset64(bytes, ehdr, strtabVaddr);
      if (strOffset > 0 && strOffset + strtabSize <= bytes.size()) {
        strtab = bytes.subspan(strOffset, strtabSize);
      }
    }
  }

  if (strtab.empty()) {
    return info;
  }

  // Read DT tags
  for (std::size_t i = 0; i < dynCount; ++i) {
    const auto tag = dynEntries[i].d_tag;
    const auto val = dynEntries[i].d_un.d_val;

    if (tag == DT_NULL) {
      break;
    } else if (tag == DT_SONAME) {
      info.soname = getStringFromTable(strtab, val);
    } else if (tag == DT_NEEDED) {
      info.needed.push_back(getStringFromTable(strtab, val));
    } else if (tag == DT_RUNPATH) {
      info.runpaths.push_back(getStringFromTable(strtab, val));
    } else if (tag == DT_RPATH) {
      info.rpaths.push_back(getStringFromTable(strtab, val));
    }
  }

  return info;
}

} // namespace

std::string_view toString(InspectorError error) noexcept {
  switch (error) {
  case InspectorError::Ok:
    return "Ok";
  case InspectorError::MissingSonameTag:
    return "MissingSonameTag";
  case InspectorError::InvalidSonamePath:
    return "InvalidSonamePath";
  case InspectorError::InsecureRpath:
    return "InsecureRpath";
  case InspectorError::InterpForbidden:
    return "InterpForbidden";
  case InspectorError::SystemLibraryShadowingForbidden:
    return "SystemLibraryShadowingForbidden";
  case InspectorError::UnresolvedLibraryDependency:
    return "UnresolvedLibraryDependency";
  case InspectorError::CyclicLibraryDependency:
    return "CyclicLibraryDependency";
  case InspectorError::UnsupportedFormat:
    return "UnsupportedFormat";
  case InspectorError::CorruptBinary:
    return "CorruptBinary";
  case InspectorError::FileNotFound:
    return "FileNotFound";
  }
  return "UnknownError";
}

std::expected<InspectedElf, InspectorError>
inspectElfBytes(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 4) {
    return std::unexpected(InspectorError::CorruptBinary);
  }

  if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
      bytes[3] != 'F') {
    return std::unexpected(InspectorError::UnsupportedFormat);
  }

  const std::uint8_t elfClass = bytes[EI_CLASS];
  if (elfClass == ELFCLASS64) {
    return inspectElf64(bytes);
  }

  return std::unexpected(InspectorError::UnsupportedFormat);
}

std::expected<InspectedElf, InspectorError>
inspectElf(const std::filesystem::path &path) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    return std::unexpected(InspectorError::FileNotFound);
  }

  std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(is)),
                                   std::istreambuf_iterator<char>());
  return inspectElfBytes(buffer);
}

std::expected<InspectedElf, InspectorError>
validateBundledLibraryBytes(std::span<const std::uint8_t> bytes,
                            std::string_view expectedSoname) {
  std::string err;
  if (!validateSoname(expectedSoname, err)) {
    return std::unexpected(InspectorError::InvalidSonamePath);
  }

  if (isShadowingForbidden(expectedSoname)) {
    return std::unexpected(InspectorError::SystemLibraryShadowingForbidden);
  }

  auto inspected = inspectElfBytes(bytes);
  if (!inspected) {
    return std::unexpected(inspected.error());
  }

  if (inspected->hasInterp) {
    return std::unexpected(InspectorError::InterpForbidden);
  }

  if (inspected->soname.empty() || inspected->soname != expectedSoname) {
    return std::unexpected(InspectorError::MissingSonameTag);
  }

  for (const auto &rp : inspected->runpaths) {
    if (!validateRpathString(rp)) {
      return std::unexpected(InspectorError::InsecureRpath);
    }
  }

  for (const auto &rp : inspected->rpaths) {
    if (!validateRpathString(rp)) {
      return std::unexpected(InspectorError::InsecureRpath);
    }
  }

  return inspected;
}

std::expected<InspectedElf, InspectorError>
validateBundledLibrary(const std::filesystem::path &path,
                       std::string_view expectedSoname) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    return std::unexpected(InspectorError::FileNotFound);
  }

  std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(is)),
                                   std::istreambuf_iterator<char>());
  return validateBundledLibraryBytes(buffer, expectedSoname);
}

std::expected<std::vector<std::string>, InspectorError>
topologicalSortLibraries(
    const std::vector<LibraryDependencyNode> &libraries,
    const std::vector<std::string> &declaredSystemLibraries,
    std::string_view rootModuleSoname) {

  std::set<std::string> bundledSet;
  std::set<std::string> systemSet(declaredSystemLibraries.begin(),
                                  declaredSystemLibraries.end());

  for (const auto &lib : libraries) {
    if (isShadowingForbidden(lib.soname)) {
      return std::unexpected(InspectorError::SystemLibraryShadowingForbidden);
    }
    if (systemSet.contains(lib.soname)) {
      return std::unexpected(InspectorError::SystemLibraryShadowingForbidden);
    }
    bundledSet.insert(lib.soname);
  }

  // Check unresolved dependencies
  for (const auto &lib : libraries) {
    for (const auto &dep : lib.needed) {
      if (bundledSet.contains(dep)) {
        continue;
      }
      if (systemSet.contains(dep) || isShadowingForbidden(dep)) {
        continue;
      }
      return std::unexpected(InspectorError::UnresolvedLibraryDependency);
    }
  }

  // Build dependency graph for Kahn's algorithm
  // In-degree: number of bundled libraries that this library directly depends
  // on
  std::map<std::string, std::size_t> inDegree;
  std::map<std::string, std::vector<std::string>>
      dependents; // dependency -> dependents

  for (const auto &lib : libraries) {
    inDegree[lib.soname] = 0;
  }

  for (const auto &lib : libraries) {
    for (const auto &dep : lib.needed) {
      if (bundledSet.contains(dep)) {
        // dep must be loaded before lib
        dependents[dep].push_back(lib.soname);
        ++inDegree[lib.soname];
      }
    }
  }

  // Queue of libraries with in-degree 0 (deepest dependencies / leaf nodes)
  std::queue<std::string> q;
  for (const auto &[soname, deg] : inDegree) {
    if (deg == 0) {
      q.push(soname);
    }
  }

  std::vector<std::string> sorted;
  sorted.reserve(libraries.size());

  while (!q.empty()) {
    const std::string curr = q.front();
    q.pop();
    sorted.push_back(curr);

    auto it = dependents.find(curr);
    if (it != dependents.end()) {
      for (const auto &depName : it->second) {
        if (--inDegree[depName] == 0) {
          q.push(depName);
        }
      }
    }
  }

  if (sorted.size() != libraries.size()) {
    // Cycle detected!
    return std::unexpected(InspectorError::CyclicLibraryDependency);
  }

  // If rootModuleSoname was specified and exists, place it last
  if (!rootModuleSoname.empty()) {
    auto rootIt = std::find(sorted.begin(), sorted.end(), rootModuleSoname);
    if (rootIt != sorted.end() && rootIt != (sorted.end() - 1)) {
      std::string rootName = *rootIt;
      sorted.erase(rootIt);
      sorted.push_back(std::move(rootName));
    }
  }

  return sorted;
}

} // namespace xanadu::vagent::cluster
