#include "F3BNKReader.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

namespace {
void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

// Fable III's path hash is FNV-1: multiply first, then XOR each byte.
std::uint32_t fnv1(const std::string& text) {
    std::uint32_t hash = 0x811c9dc5u;
    for (unsigned char c : text) hash = (hash * 0x01000193u) ^ c;
    return hash;
}

void append_path_record(std::vector<std::uint8_t>& raw, const std::string& path) {
    append_be32(raw, static_cast<std::uint32_t>(path.size() + 1));
    raw.insert(raw.end(), path.begin(), path.end());
    raw.push_back(0);
    for (int i = 0; i < 6; ++i) append_be32(raw, 0);
    append_be32(raw, 16);
}

std::vector<std::uint8_t> make_test_index() {
    const std::string first = "first.bin";
    const std::string second = "folder\\second.bin";
    std::vector<std::uint8_t> raw;
    append_be32(raw, 0);
    append_be32(raw, 2);
    append_be32(raw, fnv1(first)); append_be32(raw, 0); append_be32(raw, 2);
    append_be32(raw, fnv1(second)); append_be32(raw, 4); append_be32(raw, 2);
    append_path_record(raw, first);
    append_path_record(raw, second);

    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    const int rc = compress2(compressed.data(), &compressed_size, raw.data(),
                             static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION);
    assert(rc == Z_OK);
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> bnk;
    append_be32(bnk, 0);
    append_be32(bnk, 4);
    bnk.push_back(0);
    append_be32(bnk, static_cast<std::uint32_t>(compressed.size()));
    append_be32(bnk, static_cast<std::uint32_t>(raw.size()));
    bnk.insert(bnk.end(), compressed.begin(), compressed.end());
    const auto total = static_cast<std::uint32_t>(bnk.size());
    bnk[0] = static_cast<std::uint8_t>(total >> 24);
    bnk[1] = static_cast<std::uint8_t>(total >> 16);
    bnk[2] = static_cast<std::uint8_t>(total >> 8);
    bnk[3] = static_cast<std::uint8_t>(total);
    return bnk;
}
}

int main() {
    const auto index = make_test_index();
    const std::vector<std::uint8_t> content = {'A', 'B', 0xEE, 0xEE, 'C', 'D'};
    assert(F3BNKReader::IsF3BNK(index));
    F3BNKReader reader(index, content);
    const auto& files = reader.list_files();

    assert(files.size() == 2);
    assert(files[0].name == "first.bin");
    assert(files[0].name_hash == fnv1("first.bin"));
    assert(files[0].offset == 0);
    assert(files[0].size() == 2);
    assert(files[1].name == "folder\\second.bin");
    assert(files[1].name_hash == fnv1("folder\\second.bin"));
    assert(files[1].offset == 4);
    assert(files[1].size() == 2);
    assert((reader.extract_index_bytes(0) == std::vector<std::uint8_t>{'A', 'B'}));
    assert((reader.extract_file_bytes("folder\\second.bin") ==
            std::vector<std::uint8_t>{'C', 'D'}));
    return 0;
}
