#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace F3Tex {

constexpr std::size_t kHeaderSize = 0x5C;
constexpr std::uint32_t kSignature = 0xF3BBCBABu;
constexpr std::uint32_t kVersion = 4u;
constexpr std::uint32_t kFormatBC1 = 0x23u;
constexpr std::size_t kMaxMipOffsets = 11;

struct Mip {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Info {
    std::uint32_t version = 0;
    std::uint32_t data_size = 0;
    std::uint32_t unknown_0c = 0;
    std::uint32_t unknown_10 = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t unknown_20 = 0;
    std::uint32_t unknown_24 = 0;
    std::vector<Mip> mips;
};

bool IsF3Tex(const std::vector<std::uint8_t>& data);
bool Parse(const std::vector<std::uint8_t>& data, Info& out, std::string* error = nullptr);

} // namespace F3Tex
