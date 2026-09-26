#include "WaterParser.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace Level {

namespace {

constexpr uint32_t kMarker       = 0x00000FECu;
constexpr uint32_t kMaxBodyCount = 256;
constexpr uint32_t kMaxTileCount = 4096;
constexpr uint32_t kMaxAuxCount  = 64;
constexpr uint32_t kMaxMaskBytes = 1u << 20;
constexpr uint32_t kMaxCells     = 1024;

struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         i = 0;
    bool little_endian = false;

    bool need(size_t k) const { return i + k <= n; }

    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        if (little_endian) {
            v = uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8) |
                (uint32_t(p[i + 2]) << 16) | (uint32_t(p[i + 3]) << 24);
        } else {
            v = (uint32_t(p[i]) << 24) | (uint32_t(p[i + 1]) << 16) |
                (uint32_t(p[i + 2]) << 8) | uint32_t(p[i + 3]);
        }
        i += 4;
        return true;
    }
    bool f32(float& v) {
        uint32_t u = 0;
        if (!u32(u)) return false;
        std::memcpy(&v, &u, sizeof(v));
        return true;
    }
    bool strz(std::string& s) {
        size_t j = i;
        while (j < n && p[j] != 0) ++j;
        if (j >= n) return false;
        s.assign(reinterpret_cast<const char*>(p + i), j - i);
        i = j + 1;
        return true;
    }
};

bool parse_body(Reader& r, WaterBody& out)
{
    uint32_t marker = 0;
    if (!r.u32(marker) || marker != kMarker) return false;

    if (!r.f32(out.param_a))     return false;
    if (!r.f32(out.base_height)) return false;
    // F3 PC stores 25 water parameters; the older F2 format stores the
    // complete 37-parameter block represented by WaterBody::params.
    const size_t param_count = r.little_endian ? 25u : out.params.size();
    for (size_t i = 0; i < param_count; ++i) {
        if (!r.f32(out.params[i])) return false;
    }

    if (!r.strz(out.normal_map_path))    return false;
    if (!r.strz(out.secondary_map_path)) return false;

    uint32_t patch_count = 0;
    if (!r.u32(patch_count) || patch_count > kMaxTileCount) return false;
    out.declared_tile_count = patch_count;

    out.tiles.reserve(patch_count);
    for (uint32_t pi = 0; pi < patch_count; ++pi) {
        if (!r.u32(marker) || marker != kMarker) return false;

        WaterTile t;
        float cells_x = 0.0f;
        float cells_z = 0.0f;
        if (!r.f32(t.cx) || !r.f32(t.cz) ||
            !r.f32(t.ex) || !r.f32(t.ez) ||
            !r.f32(cells_x) || !r.f32(cells_z)) {
            return false;
        }
        if (!(cells_x >= 1.0f) || !(cells_z >= 1.0f) ||
            cells_x > float(kMaxCells) || cells_z > float(kMaxCells)) {
            return false;
        }
        t.cells_x = int(cells_x);
        t.cells_z = int(cells_z);

        uint32_t aux_count = 0;
        if (!r.u32(aux_count) || aux_count > kMaxAuxCount) return false;
        t.aux.resize(aux_count);
        for (uint32_t k = 0; k < aux_count; ++k) {
            if (!r.u32(t.aux[k])) return false;
        }

        uint32_t mask_count = 0;
        if (!r.u32(mask_count) || mask_count > kMaxMaskBytes) return false;
        if (!r.need(mask_count)) return false;
        if (mask_count != uint32_t(t.cells_x) * uint32_t(t.cells_z)) {

            return false;
        }
        t.mask.assign(r.p + r.i, r.p + r.i + mask_count);
        r.i += mask_count;

        if (!r.u32(marker) || marker != kMarker) return false;

        out.tiles.push_back(std::move(t));
    }

    uint32_t trailing = 0;
    (void)r.u32(trailing);

    return true;
}

}

bool ParseWaterFile(const std::vector<uint8_t>& bytes, WaterScene& out)
{
    out = WaterScene{};
    if (bytes.size() < 0x0C) return false;

    Reader r{ bytes.data(), bytes.size() };

    // Fable 2 water files are version 2/big-endian. Fable 3 PC water
    // files are version 3/little-endian.
    if (bytes.size() < 4) return false;
    const uint32_t be_version = (uint32_t(bytes[0]) << 24) |
                                (uint32_t(bytes[1]) << 16) |
                                (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
    const uint32_t le_version = uint32_t(bytes[0]) |
                                (uint32_t(bytes[1]) << 8) |
                                (uint32_t(bytes[2]) << 16) |
                                (uint32_t(bytes[3]) << 24);
    if (be_version == 2) r.little_endian = false;
    else if (le_version == 3 || le_version == 2) r.little_endian = true;
    else return false;
    if (!r.u32(out.version)) return false;
    if (!r.u32(out.body_count) || out.body_count == 0 ||
        out.body_count > kMaxBodyCount) {
        return false;
    }

    const size_t table_end = 8 + size_t(out.body_count) * 4;
    if (table_end > bytes.size()) return false;

    std::vector<uint32_t> offsets(out.body_count);
    for (uint32_t k = 0; k < out.body_count; ++k) {
        if (!r.u32(offsets[k])) return false;
        if (offsets[k] < table_end || offsets[k] >= bytes.size()) return false;
    }

    out.bodies.reserve(out.body_count);
    for (uint32_t k = 0; k < out.body_count; ++k) {
        Reader br{ bytes.data(), bytes.size(), offsets[k], r.little_endian };
        WaterBody body;
        if (parse_body(br, body)) {
            out.tile_count += uint32_t(body.tiles.size());
            out.bodies.push_back(std::move(body));
        }
    }

    return !out.bodies.empty();
}

}
