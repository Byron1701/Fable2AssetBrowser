#include "Level/Loading/F3EngineLevelParser.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}
void putstr(std::vector<uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}
void putfloat(std::vector<uint8_t>& b, float f) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(f));
    std::memcpy(&bits, &f, sizeof(bits));
    put32(b, bits);
}
std::vector<uint8_t> header(uint32_t count) {
    const std::string magic = "LevelGraphicsFile";
    std::vector<uint8_t> b(magic.begin(), magic.end());
    put32(b, 13);
    put32(b, count);
    return b;
}
void putType2(std::vector<uint8_t>& b) {
    put32(b, 2);
    putstr(b, "art\\tree.mdl");
    putstr(b, "art\\tree_shadow.mdl");
    putstr(b, "art\\tree_lod.mdl");
    put32(b, 1); // F3 v13 count is little-endian.
    b.insert(b.end(), {1, 2, 3, 4});
    put64(b, 0x1122334455667788ull);
    for (int i = 0; i < 20; ++i) putfloat(b, float(i));
}
void putType21(std::vector<uint8_t>& b) {
    put32(b, 21);
    putstr(b, "art\\foliage.mdl");
    for (int i = 0; i < 10; ++i) b.push_back(static_cast<uint8_t>(i));
    put32(b, 1);
    b.insert(b.end(), 68, 0); // Fixed header.
    b.insert(b.end(), 22, 0); // Primary record preserved raw.
    put32(b, 1);
    for (int i = 0; i < 8; ++i) putfloat(b, float(i + 1));
}
}

int main(int argc, char** argv) {
    using Level::F3::ParseEngineLevelV13;

    auto valid = header(5);
    put32(valid, 4);
    putstr(valid, "worlds\\fable3\\sample.ehf");
    put64(valid, 0x123456789abcdef0ull);
    put32(valid, 5);
    putstr(valid, "worlds\\fable3\\sample.water");
    put32(valid, 32);
    putstr(valid, "worlds\\fable3\\sample.mist");
    putType2(valid);
    putType21(valid);

    const auto parsed = ParseEngineLevelV13(valid);
    assert(parsed.ok && parsed.complete);
    assert(parsed.version == 13 && parsed.declared_record_count == 5);
    assert(parsed.records.size() == 5);
    assert(parsed.records[0].type == 4);
    assert(parsed.records[0].path == "worlds\\fable3\\sample.ehf");
    assert(parsed.records[0].has_resource_key);
    assert(parsed.records[0].resource_key == 0x123456789abcdef0ull);
    assert(parsed.records[1].type == 5);
    assert(parsed.records[2].type == 32);
    assert(parsed.records[3].type == 2);
    assert(parsed.records[3].instance_count == 1);
    assert(parsed.records[3].paths.size() == 3);
    assert(parsed.records[3].instances.size() == 1);
    assert(parsed.records[3].instances[0].hash == 0x1122334455667788ull);
    assert(parsed.records[3].instances[0].values.size() == 20);
    assert(parsed.records[4].type == 21);
    assert(parsed.records[4].instance_count == 1);
    assert(parsed.records[4].secondary_instance_count == 1);
    assert(parsed.records[4].instances[0].raw.size() == 22);
    assert(parsed.records[4].secondary_instances[0].values.size() == 8);

    auto unsupported = header(2);
    put32(unsupported, 67);
    put32(unsupported, 0xfeedbeef);
    const auto partial = ParseEngineLevelV13(unsupported);
    assert(!partial.ok && !partial.complete);
    assert(partial.records.size() == 1);
    assert(partial.records[0].raw.size() == 8);
    assert(!partial.error.empty());

    auto wrong_version = header(0);
    wrong_version[17] = 12;
    const auto wrong = ParseEngineLevelV13(wrong_version);
    assert(!wrong.ok && wrong.version == 12);

    auto truncated = header(1);
    truncated.push_back(4);
    const auto short_file = ParseEngineLevelV13(truncated);
    assert(!short_file.ok && !short_file.error.empty());

    // Optional arguments are real .engine_level files extracted from the
    // Library Fable3.zip corpus. This lets the same test executable validate
    // the parser against the full sample set without bundling game assets.
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        if (!file) {
            std::cerr << "Cannot open sample: " << argv[i] << "\n";
            return 1;
        }
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        const auto sample = ParseEngineLevelV13(bytes);
        if (!sample.ok || !sample.complete ||
            sample.records.size() != sample.declared_record_count) {
            std::cerr << "Sample failed: " << argv[i] << ": "
                      << sample.error << "\n";
            return 1;
        }
        std::cout << "PASS " << argv[i] << ": "
                  << sample.records.size() << " records\n";
    }

    return 0;
}
