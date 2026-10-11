#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Level::F3 {

struct EngineLevelInstance {
    size_t offset = 0;
    uint64_t hash = 0;
    std::array<uint8_t, 4> flags{};
    // Type 2: 20 little-endian floats. Type 21: decoded loop values.
    std::vector<float> values;
    std::vector<uint8_t> raw;
};

struct EngineLevelRecord {
    uint32_t type = 0;
    size_t offset = 0;
    size_t size = 0;
    std::string path;
    std::vector<std::string> paths;
    uint64_t resource_key = 0;
    bool has_resource_key = false;

    // Type 2 instance records.
    uint32_t instance_count = 0;
    std::vector<EngineLevelInstance> instances;

    // Type 21 scatter/placement records. The ten-byte metadata is preserved
    // separately from the 68-byte fixed header.
    uint64_t metadata_hash = 0;
    std::array<uint8_t, 2> metadata_flags{};
    std::vector<uint8_t> fixed_header;
    uint32_t secondary_instance_count = 0;
    std::vector<EngineLevelInstance> secondary_instances;

    // Exact bytes belonging to this record, including its header.
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

// Independent Fable III parser. It does not call or share the Fable II parser.
EngineLevelParseResult ParseEngineLevelV13(
    const std::vector<uint8_t>& bytes);

} // namespace Level::F3
