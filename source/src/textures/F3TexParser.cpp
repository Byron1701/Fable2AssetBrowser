#include "F3TexParser.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace F3Tex {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return (std::uint32_t(p[0])      ) |
           (std::uint32_t(p[1]) <<  8) |
           (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

bool range_ok(std::size_t offset, std::size_t size, std::size_t total) {
    return offset <= total && size <= total - offset;
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::size_t bc1_size(std::uint32_t width, std::uint32_t height) {
    const std::size_t bx = (static_cast<std::size_t>(width) + 3u) / 4u;
    const std::size_t by = (static_cast<std::size_t>(height) + 3u) / 4u;
    return bx * by * 8u;
}

} // namespace

bool IsF3Tex(const std::vector<std::uint8_t>& data) {
    return data.size() >= kHeaderSize &&
           le32(data.data()) == kSignature;
}

bool Parse(const std::vector<std::uint8_t>& data, Info& out, std::string* error) {
    out = Info{};

    if (data.size() < kHeaderSize)
        return fail(error, "F3 TEX is smaller than its 0x5C-byte header");

    if (le32(data.data()) != kSignature)
        return fail(error, "invalid F3 TEX signature");

    out.version   = le32(data.data() + 0x04);
    out.data_size = le32(data.data() + 0x08);
    out.unknown_0c = le32(data.data() + 0x0C);
    out.unknown_10 = le32(data.data() + 0x10);
    out.width     = le32(data.data() + 0x14);
    out.height    = le32(data.data() + 0x18);
    out.format    = le32(data.data() + 0x1C);
    out.unknown_20 = le32(data.data() + 0x20);
    out.unknown_24 = le32(data.data() + 0x24);

    const std::uint32_t mip_count = le32(data.data() + 0x28);

    if (out.version != kVersion)
        return fail(error, "unsupported F3 TEX version " + std::to_string(out.version));

    if (out.width == 0 || out.height == 0)
        return fail(error, "F3 TEX has zero width or height");

    if (out.width > 16384 || out.height > 16384)
        return fail(error, "F3 TEX dimensions are implausibly large");

    if (mip_count == 0 || mip_count > kMaxMipOffsets)
        return fail(error, "invalid F3 TEX mip count " + std::to_string(mip_count));

    const std::size_t data_end = kHeaderSize + static_cast<std::size_t>(out.data_size);
    if (data_end != data.size())
        return fail(error, "F3 TEX data size does not match file size");

    out.mips.reserve(mip_count);

    std::uint32_t previous_offset = 0;
    for (std::uint32_t i = 0; i < mip_count; ++i) {
        const std::uint32_t offset = le32(data.data() + 0x2C + i * 4u);

        if (offset < kHeaderSize || static_cast<std::size_t>(offset) >= data_end)
            return fail(error, "F3 TEX mip offset is outside the texture");

        if (i > 0 && offset <= previous_offset)
            return fail(error, "F3 TEX mip offsets are not strictly increasing");

        previous_offset = offset;
    }

    for (std::uint32_t i = 0; i < mip_count; ++i) {
        const std::uint32_t offset = le32(data.data() + 0x2C + i * 4u);
        const std::uint32_t next =
            (i + 1u < mip_count)
                ? le32(data.data() + 0x2C + (i + 1u) * 4u)
                : static_cast<std::uint32_t>(data_end);

        if (next <= offset)
            return fail(error, "F3 TEX mip has an invalid size");

        const std::uint32_t width =
            std::max<std::uint32_t>(1u, out.width >> std::min<std::uint32_t>(i, 31u));
        const std::uint32_t height =
            std::max<std::uint32_t>(1u, out.height >> std::min<std::uint32_t>(i, 31u));

        const std::size_t size = static_cast<std::size_t>(next) - offset;

        // 0x23 is confirmed by the supplied 128x128 and 1024x1024 files
        // to be standard DXT1/BC1 payload: each mip has 8 bytes per 4x4 block.
        if (out.format == kFormatBC1) {
            const std::size_t expected = bc1_size(width, height);
            if (size != expected) {
                std::ostringstream os;
                os << "F3 TEX BC1 mip " << i << " has " << size
                   << " bytes; expected " << expected;
                return fail(error, os.str());
            }
        }

        out.mips.push_back(Mip{
            offset,
            static_cast<std::uint32_t>(size),
            width,
            height
        });
    }

    return true;
}

} // namespace F3Tex
