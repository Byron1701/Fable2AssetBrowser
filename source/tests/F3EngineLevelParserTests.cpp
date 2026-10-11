#include "Level/Loading/F3EngineLevelParser.h"

#include <cassert>
#include <cstdint>
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
std::vector<uint8_t> header(uint32_t count) {
    const std::string magic = "LevelGraphicsFile";
    std::vector<uint8_t> b(magic.begin(), magic.end());
    put32(b, 13);
    put32(b, count);
    return b;
}
}

int main() {
    using Level::F3::ParseEngineLevelV13;

    auto valid = header(3);
    put32(valid, 4);
    putstr(valid, "worlds\\fable3\\sample.ehf");
    put64(valid, 0x123456789abcdef0ull);
    put32(valid, 5);
    putstr(valid, "worlds\\fable3\\sample.water");
    put32(valid, 32);
    putstr(valid, "worlds\\fable3\\sample.mist");

    const auto parsed = ParseEngineLevelV13(valid);
    assert(parsed.ok && parsed.complete);
    assert(parsed.version == 13 && parsed.declared_record_count == 3);
    assert(parsed.records.size() == 3);
    assert(parsed.records[0].type == 4);
    assert(parsed.records[0].path == "worlds\\fable3\\sample.ehf");
    assert(parsed.records[0].has_resource_key);
    assert(parsed.records[0].resource_key == 0x123456789abcdef0ull);
    assert(parsed.records[1].type == 5);
    assert(parsed.records[2].type == 32);

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

    return 0;
}
