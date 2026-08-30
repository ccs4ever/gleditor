/**
 * @file zzstructure_loader.hpp
 * @brief Loads a ZigZag Slice document from disk or YAML string.
 */
#ifndef ZIGZAG_ZZSTRUCTURE_LOADER_HPP
#define ZIGZAG_ZZSTRUCTURE_LOADER_HPP

#include "zzstructure.hpp"

#include <expected>
#include <string>

namespace zigzag {

/**
 * @brief Loads and validates a Slice (a zzstructure YAML file).
 *
 * @param path File path to load.
 * @return Parsed document or LoadError on failure.
 */
[[nodiscard]] std::expected<ZzStructureDocument, LoadError>
loadZzStructure(const std::string &path);

/**
 * @brief Parses and validates a Slice from an in-memory YAML string.
 *
 * @param yamlText YAML content.
 * @param originLabel Label describing the source (e.g. filename or snippet).
 * @return Parsed document or LoadError on failure.
 */
[[nodiscard]] std::expected<ZzStructureDocument, LoadError>
parseZzStructure(const std::string &yamlText,
                 const std::string &originLabel = "inline");

/**
 * @brief Serializes a ZigZag structure document to a YAML string.
 */
[[nodiscard]] std::string serializeZzStructure(const ZzStructureDocument &doc);

/**
 * @brief Saves a ZigZag structure document to a YAML file on disk.
 */
[[nodiscard]] bool saveZzStructure(const ZzStructureDocument &doc,
                                   const std::string &path);

} // namespace zigzag

#endif // ZIGZAG_ZZSTRUCTURE_LOADER_HPP
