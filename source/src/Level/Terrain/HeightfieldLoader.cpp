#include "HeightfieldLoader.h"

#include "Utilities/State.h"
#include "BNKCore.cpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <climits>

namespace Level {

namespace {

uint32_t be_u32(const uint8_t* p) {
    return  (uint32_t(p[0]) << 24)
          | (uint32_t(p[1]) << 16)
          | (uint32_t(p[2]) <<  8)
          |  uint32_t(p[3]);
}
float be_f32(const uint8_t* p) {
    uint32_t u = be_u32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string normalize_key(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

bool gunzip(const std::vector<uint8_t>& in,
            std::vector<uint8_t>&       out,
            std::string&                err)
{
    out.clear();
    if (in.size() < 2 || in.size() > std::numeric_limits<uInt>::max()) {
        err = "compressed .ghf size is invalid";
        return false;
    }

    // The supplied F3 assets use gzip, but inflateInit2(15 + 32) also
    // accepts a zlib-wrapped stream. Check either wrapper before inflating.
    const bool gzip_header = in[0] == 0x1F && in[1] == 0x8B;
    const bool zlib_header = (in[0] & 0x0F) == 8 &&
        ((uint32_t(in[0]) << 8) | in[1]) % 31 == 0;
    if (!gzip_header && !zlib_header) {
        err = "not a gzip or zlib stream (header mismatch)";
        return false;
    }

    constexpr size_t kMaxInflatedBytes = 512u * 1024u * 1024u;
    z_stream zs{};
    zs.next_in   = const_cast<Bytef*>(in.data());
    zs.avail_in  = static_cast<uInt>(in.size());
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        err = "inflateInit2 failed";
        return false;
    }

    const size_t initial_size = std::min(
        kMaxInflatedBytes, std::max<size_t>(64u * 1024u, in.size() * 4u));
    out.resize(initial_size);
    size_t produced = 0;
    while (true) {
        zs.next_out  = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);
        int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK) {
            if (zs.avail_out == 0) {
                if (out.size() >= kMaxInflatedBytes) {
                    inflateEnd(&zs);
                    out.clear();
                    err = "inflated .ghf exceeds the 512 MiB safety limit";
                    return false;
                }
                out.resize(std::min(kMaxInflatedBytes, out.size() * 2u));
            }
            continue;
        }
        inflateEnd(&zs);
        err = std::string("inflate failed: ") + (zs.msg ? zs.msg : "?");
        out.clear();
        return false;
    }
    inflateEnd(&zs);
    out.resize(produced);
    return true;
}

bool extract_bnk_file_by_relpath(const std::string&     relative_path,
                                 std::vector<uint8_t>&  out_bytes,
                                 std::string&           err)
{
    out_bytes.clear();
    const std::string key = normalize_key(relative_path);

    for (const auto& fe : S.all_heightfield_files) {
        if (normalize_key(fe.full_path) != key) continue;
        try {
            auto bytes = BnkCache::extract_bytes(fe.bnk_path, fe.file_index);
            out_bytes.assign(bytes.begin(), bytes.end());
            return !out_bytes.empty();
        } catch (...) {
            err = "BnkCache::extract_bytes threw for " + relative_path;
            return false;
        }
    }

    for (const auto& bnk_path : S.bnk_paths) {
        int idx = BnkCache::find_index(bnk_path, key);
        if (idx < 0) continue;
        try {
            auto bytes = BnkCache::extract_bytes(bnk_path, idx);
            out_bytes.assign(bytes.begin(), bytes.end());
            return !out_bytes.empty();
        } catch (...) {
            err = "BnkCache::extract_bytes threw for " + relative_path;
            return false;
        }
    }
    err = "no BNK contains " + relative_path;
    return false;
}

void parse_ehf_header(const std::vector<uint8_t>& bytes,
                      HeightfieldHeader&          out)
{
    out = {};
    static constexpr char   kMagic[]   = "HeightFieldGraphicsFile";
    static constexpr size_t kMagicLen  = sizeof(kMagic) - 1;
    static constexpr size_t kHeaderLen = 63;

    if (bytes.size() < kHeaderLen) return;
    if (std::memcmp(bytes.data(), kMagic, kMagicLen) != 0) return;

    const uint8_t* p = bytes.data();
    out.magic.assign(kMagic);
    out.version      = be_u32(p + kMagicLen);
    out.prefix_float = be_f32(p + kMagicLen + 4);
    out.f0           = be_f32(p + 27);
    out.f1           = be_f32(p + 31);
    out.u0           = be_u32(p + 35);
    out.u1           = be_u32(p + 39);
    out.f2           = be_f32(p + 43);
    out.f3           = be_f32(p + 47);
    out.f4           = be_f32(p + 51);
    out.body_offset  = be_u32(p + 55);
    out.body_size    = be_u32(p + 59);
    out.ok           = true;
}
 
void parse_f3_ehf_header(const std::vector<uint8_t>& bytes,
                         HeightfieldHeader& out)
{
    out = {};
    static constexpr char kMagic[] = "HeightFieldGraphicsFile";
    static constexpr size_t kMagicLen = sizeof(kMagic) - 1;
    constexpr size_t kHeaderLen = 63;
    if (bytes.size() < kHeaderLen ||
        std::memcmp(bytes.data(), kMagic, kMagicLen) != 0) {
        return;
    }

    auto u32le = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) |
               (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
    };
    auto f32le = [&](const uint8_t* p) -> float {
        const uint32_t bits = u32le(p);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    // F3 EHF v18 uses little-endian header values. Do not interpret its
    // trailing header floats as the F2 body offset/size pair.
    out.magic.assign(kMagic);
    out.version = u32le(bytes.data() + kMagicLen);
    out.prefix_float = f32le(bytes.data() + kMagicLen + 4);
    out.f0 = f32le(bytes.data() + kMagicLen + 8);
    out.u0 = u32le(bytes.data() + kMagicLen + 12);
    out.u1 = u32le(bytes.data() + kMagicLen + 16);
    out.f2 = f32le(bytes.data() + kMagicLen + 20);
    out.f3 = f32le(bytes.data() + kMagicLen + 24);
    out.f4 = f32le(bytes.data() + kMagicLen + 28);
    out.f3_layout = true;

    if (out.version == 0 || out.version > 64 ||
        out.u0 < 2 || out.u1 < 2 ||
        out.u0 > 8192 || out.u1 > 8192 ||
        !std::isfinite(out.f2) || out.f2 <= 0.0f) {
        out = {};
        return;
    }
    out.ok = true;
}


}

const FlatAssetEntry* FindHeightfieldByPath(const std::string& relative_path)
{
    const std::string key = normalize_key(relative_path);
    for (const auto& fe : S.all_heightfield_files) {
        if (normalize_key(fe.full_path) == key) return &fe;
    }
    return nullptr;
}

bool LoadHeightfieldFiles(const std::string& ehf_path,
                          const std::string& ghf_path,
                          const std::string& ,
                          const std::string& ,
                          HeightfieldFiles&  out,
                          bool f3_format)
{
    out = {};

    std::string err;

    if (!ehf_path.empty()) {
        if (!extract_bnk_file_by_relpath(ehf_path, out.ehf_bytes, err)) {
            out.error = ".ehf load failed: " + err;
            return false;
        }
        if (f3_format) {
            parse_f3_ehf_header(out.ehf_bytes, out.ehf_header);
            if (!out.ehf_header.ok) {
                out.error = ".ehf F3 header invalid or unsupported";
                return false;
            }
        } else {
            parse_ehf_header(out.ehf_bytes, out.ehf_header);
            if (out.ehf_header.magic.empty()) {
                out.error = ".ehf magic mismatch - not a HeightFieldGraphicsFile";
                return false;
            }
        }
    }

    if (!ghf_path.empty()) {
        if (!extract_bnk_file_by_relpath(ghf_path, out.ghf_bytes_compressed, err)) {
            out.error = ".ghf load failed: " + err;
            return false;
        }
        if (!gunzip(out.ghf_bytes_compressed, out.ghf_bytes_raw, err)) {
            out.error = ".ghf gunzip failed: " + err;
            return false;
        }
    }

    out.ok = true;
    return true;
}

bool DecodeGhfHeights(const std::vector<uint8_t>& bytes, GhfHeights& out)
{
    out = {};
    if (bytes.size() < 0x14) {
        out.error = ".ghf too small to hold header";
        return false;
    }

    out.tile_size = be_f32(bytes.data() + 0x00);
    out.width  = be_u32(bytes.data() + 0x0C);
    out.height = be_u32(bytes.data() + 0x10);

    if (out.width == 0 || out.height == 0 ||
        out.width > 8192 || out.height > 8192) {
        out.error = ".ghf dimensions implausible";
        return false;
    }

    constexpr size_t kCellStride = 14;
    const size_t cells = static_cast<size_t>(out.width)
                       * static_cast<size_t>(out.height);
    const size_t need  = 0x14 + cells * kCellStride;
    if (bytes.size() < need) {
        out.error = ".ghf truncated - header says " +
                    std::to_string(out.width) + "x" +
                    std::to_string(out.height) +
                    " but body is too short";
        return false;
    }

    out.heights.resize(cells);
    out.min_height =  std::numeric_limits<float>::infinity();
    out.max_height = -std::numeric_limits<float>::infinity();

    const uint8_t* p = bytes.data() + 0x14;
    for (size_t i = 0; i < cells; ++i) {
        const float h = be_f32(p + i * kCellStride);
        out.heights[i] = h;
        if (h < out.min_height) out.min_height = h;
        if (h > out.max_height) out.max_height = h;
    }

    out.ok = true;
    return true;
}

bool DecodeF3GhfHeights(const std::vector<uint8_t>& bytes,
                        GhfHeights& out)
{
    out = {};

    // Confirmed from the F3 sample in the Library:
    // 28-byte header, followed by width*height records of 14 bytes each.
    // Header fields are little-endian; each record begins with its height.
    constexpr size_t kHeaderSize = 28;
    constexpr size_t kRecordStride = 14;
    constexpr size_t kHeightOffset = 0;

    if (bytes.size() < kHeaderSize) {
        out.error = ".ghf too small for the F3 28-byte header";
        return false;
    }

    auto u32le = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) |
               (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
    };
    auto f32le = [&](const uint8_t* p) -> float {
        const uint32_t bits = u32le(p);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    out.origin_x = f32le(bytes.data() + 0x00);
    out.origin_z = f32le(bytes.data() + 0x04);
    out.width = u32le(bytes.data() + 0x0C);
    out.height = u32le(bytes.data() + 0x10);
    out.base_height = f32le(bytes.data() + 0x14);
    out.tile_size = 0.5f;

    if (!std::isfinite(out.origin_x) || !std::isfinite(out.origin_z) ||
        !std::isfinite(out.base_height) ||
        std::fabs(out.origin_x) > 1.0e7f ||
        std::fabs(out.origin_z) > 1.0e7f ||
        out.width < 2 || out.height < 2 ||
        out.width > 8192 || out.height > 8192) {
        out.error = ".ghf F3 header contains invalid origin or dimensions";
        return false;
    }

    const uint64_t cells64 = uint64_t(out.width) * uint64_t(out.height);
    constexpr uint64_t kMaxF3HeightSamples = 16ull * 1024ull * 1024ull;
    if (cells64 > kMaxF3HeightSamples) {
        out.error = ".ghf F3 sample count exceeds the 16M safety limit";
        return false;
    }
    const size_t cells = static_cast<size_t>(cells64);
    if (cells > (std::numeric_limits<size_t>::max() - kHeaderSize) /
                    kRecordStride) {
        out.error = ".ghf F3 dimensions overflow the file-size calculation";
        return false;
    }

    const size_t expected_size = kHeaderSize + cells * kRecordStride;
    if (bytes.size() != expected_size) {
        out.error = ".ghf F3 size mismatch: expected " +
                    std::to_string(expected_size) + " bytes for " +
                    std::to_string(out.width) + "x" +
                    std::to_string(out.height) + ", got " +
                    std::to_string(bytes.size());
        return false;
    }

    out.heights.resize(cells);
    out.min_height = std::numeric_limits<float>::infinity();
    out.max_height = -std::numeric_limits<float>::infinity();

    const uint8_t* records = bytes.data() + kHeaderSize;
    for (size_t i = 0; i < cells; ++i) {
        const float value =
            f32le(records + i * kRecordStride + kHeightOffset);
        if (!std::isfinite(value) || value < -1.0e4f || value > 1.0e4f) {
            out.heights.clear();
            out.error = ".ghf F3 contains an invalid height at sample " +
                        std::to_string(i);
            return false;
        }
        out.heights[i] = value;
        out.min_height = std::min(out.min_height, value);
        out.max_height = std::max(out.max_height, value);
    }

    out.ok = true;
    return true;
}

bool BuildTerrainMesh(const GhfHeights& hg, TerrainMesh& out)
{
    out = {};
    if (!hg.ok || hg.width < 2 || hg.height < 2 ||
        hg.heights.size() != size_t(hg.width) * size_t(hg.height)) {
        return false;
    }

    const uint32_t W = hg.width;
    const uint32_t H = hg.height;
    const float    tile = hg.tile_size > 0.f ? hg.tile_size : 0.5f;
    const size_t   N    = size_t(W) * size_t(H);
    const size_t   tris = size_t(W - 1) * size_t(H - 1) * 2;

    out.width  = W;
    out.height = H;
    out.min_height = hg.min_height;
    out.max_height = hg.max_height;

    out.positions.resize(N * 3);
    out.normals.resize  (N * 3);
    out.uvs.resize      (N * 2);
    out.indices.resize  (tris * 3);

    constexpr float kUvRepeatsPerWu = 0.125f;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            out.positions[i * 3 + 0] = hg.origin_x + float(x) * tile;
            out.positions[i * 3 + 1] = hg.heights[i];
            out.positions[i * 3 + 2] = hg.origin_z + float(y) * tile;
            out.uvs[i * 2 + 0]       = float(x) * tile * kUvRepeatsPerWu;
            out.uvs[i * 2 + 1]       = float(y) * tile * kUvRepeatsPerWu;
        }
    }

    auto h_at = [&](int xi, int yi) -> float {
        if (xi < 0) xi = 0; else if (xi >= int(W)) xi = int(W) - 1;
        if (yi < 0) yi = 0; else if (yi >= int(H)) yi = int(H) - 1;
        return hg.heights[size_t(yi) * W + size_t(xi)];
    };
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float hl = h_at(int(x) - 1, int(y));
            const float hr = h_at(int(x) + 1, int(y));
            const float hd = h_at(int(x),     int(y) - 1);
            const float hu = h_at(int(x),     int(y) + 1);
            float nx = (hl - hr);
            float ny = 2.f * tile;
            float nz = (hd - hu);
            float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
            else             { nx = 0.f;  ny = 1.f;  nz = 0.f;  }
            const size_t i = size_t(y) * W + x;
            out.normals[i * 3 + 0] = nx;
            out.normals[i * 3 + 1] = ny;
            out.normals[i * 3 + 2] = nz;
        }
    }

    size_t k = 0;
    for (uint32_t y = 0; y + 1 < H; ++y) {
        for (uint32_t x = 0; x + 1 < W; ++x) {
            const uint32_t i00 = uint32_t(size_t(y    ) * W + (x    ));
            const uint32_t i10 = uint32_t(size_t(y    ) * W + (x + 1));
            const uint32_t i01 = uint32_t(size_t(y + 1) * W + (x    ));
            const uint32_t i11 = uint32_t(size_t(y + 1) * W + (x + 1));

            out.indices[k++] = i00;
            out.indices[k++] = i01;
            out.indices[k++] = i10;

            out.indices[k++] = i10;
            out.indices[k++] = i01;
            out.indices[k++] = i11;
        }
    }

    out.ok = true;
    return true;
}

// F3 has its own mesh-build entry point and allocation ceiling. The F2
// BuildTerrainMesh implementation above is intentionally left unchanged.
bool BuildF3TerrainMesh(const GhfHeights& hg, TerrainMesh& out)
{
    out = {};
    constexpr size_t kMaxF3MeshVertices = 4u * 1024u * 1024u;
    const uint64_t vertex_count = uint64_t(hg.width) * uint64_t(hg.height);
    if (!hg.ok || hg.width < 2 || hg.height < 2 ||
        vertex_count > kMaxF3MeshVertices ||
        hg.heights.size() != static_cast<size_t>(vertex_count) ||
        !std::isfinite(hg.origin_x) || !std::isfinite(hg.origin_z) ||
        !std::isfinite(hg.tile_size) || hg.tile_size <= 0.0f) {
        return false;
    }

    const uint32_t W = hg.width;
    const uint32_t H = hg.height;
    const float    tile = hg.tile_size > 0.f ? hg.tile_size : 0.5f;
    const size_t   N    = size_t(W) * size_t(H);
    const size_t   tris = size_t(W - 1) * size_t(H - 1) * 2;

    out.width  = W;
    out.height = H;
    out.min_height = hg.min_height;
    out.max_height = hg.max_height;

    out.positions.resize(N * 3);
    out.normals.resize  (N * 3);
    out.uvs.resize      (N * 2);
    out.indices.resize  (tris * 3);

    constexpr float kUvRepeatsPerWu = 0.125f;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            out.positions[i * 3 + 0] = hg.origin_x + float(x) * tile;
            out.positions[i * 3 + 1] = hg.heights[i];
            out.positions[i * 3 + 2] = hg.origin_z + float(y) * tile;
            out.uvs[i * 2 + 0]       = float(x) * tile * kUvRepeatsPerWu;
            out.uvs[i * 2 + 1]       = float(y) * tile * kUvRepeatsPerWu;
        }
    }

    auto h_at = [&](int xi, int yi) -> float {
        if (xi < 0) xi = 0; else if (xi >= int(W)) xi = int(W) - 1;
        if (yi < 0) yi = 0; else if (yi >= int(H)) yi = int(H) - 1;
        return hg.heights[size_t(yi) * W + size_t(xi)];
    };
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float hl = h_at(int(x) - 1, int(y));
            const float hr = h_at(int(x) + 1, int(y));
            const float hd = h_at(int(x),     int(y) - 1);
            const float hu = h_at(int(x),     int(y) + 1);
            float nx = (hl - hr);
            float ny = 2.f * tile;
            float nz = (hd - hu);
            float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
            else             { nx = 0.f;  ny = 1.f;  nz = 0.f;  }
            const size_t i = size_t(y) * W + x;
            out.normals[i * 3 + 0] = nx;
            out.normals[i * 3 + 1] = ny;
            out.normals[i * 3 + 2] = nz;
        }
    }

    size_t k = 0;
    for (uint32_t y = 0; y + 1 < H; ++y) {
        for (uint32_t x = 0; x + 1 < W; ++x) {
            const uint32_t i00 = uint32_t(size_t(y    ) * W + (x    ));
            const uint32_t i10 = uint32_t(size_t(y    ) * W + (x + 1));
            const uint32_t i01 = uint32_t(size_t(y + 1) * W + (x    ));
            const uint32_t i11 = uint32_t(size_t(y + 1) * W + (x + 1));

            out.indices[k++] = i00;
            out.indices[k++] = i01;
            out.indices[k++] = i10;

            out.indices[k++] = i10;
            out.indices[k++] = i01;
            out.indices[k++] = i11;
        }
    }

    out.ok = true;
    return true;
}

}
