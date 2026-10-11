#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Level::F3 {

struct EngineLevelRecord {
    uint32_t type = 0;
    size_t offset = 0;
    size_t size = 0;
    std::string path;
    uint64_t resource_key = 0;
    bool has_resource_key = false;
    std::vector<uint8_t> raw;
};

struct EngineLevelParseResult {
    bool ok = false;
    bool complete = false;
    uint32_t version = 0;
    uint32_t declared_record_count = 0;
    std::vector<EngineLevelRecord> records;
    std::string error;
};

// Independent Fable III parser. This deliberately does not use the Fable II
// EngineLevelParser or its byte reader.
EngineLevelParseResult ParseEngineLevelV13(
    const std::vector<uint8_t>& bytes);

} // namespace Level::F3
