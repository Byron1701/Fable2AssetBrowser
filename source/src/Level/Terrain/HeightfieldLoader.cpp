#include "HeightfieldLoader.h"

#include "Utilities/State.h"
#include "UI/OutputLog.h"
#include "BNKCore.cpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

namespace Level {

void TerrainMeshTrace(const std::string& message) noexcept {
    try {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        std::error_code ec;
        const std::filesystem::path path =
            std::filesystem::temp_directory_path(ec) /
            "Fable3AssetBrowser_TerrainMeshTrace.txt";
        if (ec) return;
        std::ofstream stream(path, std::ios::out | std::ios::app);
        if (!stream) return;
        stream << message << '\n';
        stream.flush();
    } catch (...) {
    }
}

namespace {uint32_t be_u32(const uint8_t* p) {
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
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B) {
        err = "not a gzip stream (magic mismatch)";
        return false;
    }

    z_stream zs{};
    zs.next_in   = const_cast<Bytef*>(in.data());
    zs.avail_in  = static_cast<uInt>(in.size());
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        err = "inflateInit2 failed";
        return false;
    }

    out.resize(in.size() * 4);
    size_t produced = 0;
    while (true) {
        zs.next_out  = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);
        int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out.size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK) {
            if (zs.avail_out == 0) out.resize(out.size() * 2);
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

    // Fable III PC EHF is a separate little-endian header layout.
    // Recognise it here without changing the existing Fable II branch below.
    auto le_u32_local = [&](size_t off) -> uint32_t {
        return uint32_t(bytes[off]) |
               (uint32_t(bytes[off + 1]) << 8) |
               (uint32_t(bytes[off + 2]) << 16) |
               (uint32_t(bytes[off + 3]) << 24);
    };
    auto le_f32_local = [&](size_t off) -> float {
        uint32_t u = le_u32_local(off);
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    };
    if (bytes.size() >= 0x47) {
        const uint32_t f3_w = le_u32_local(0x23);
        const uint32_t f3_h = le_u32_local(0x27);
        const float f3_spacing = le_f32_local(0x2B);
        const float f3_pcx = le_f32_local(0x37);
        const float f3_pcy = le_f32_local(0x3B);
        const float f3_cells_x = le_f32_local(0x3F);
        const float f3_cells_y = le_f32_local(0x43);
        if (f3_w >= 2 && f3_w <= 8192 &&
            f3_h >= 2 && f3_h <= 8192 &&
            std::isfinite(f3_spacing) && f3_spacing > 0.0f &&
            (f3_w - 1) % 32 == 0 && (f3_h - 1) % 32 == 0 &&
            std::fabs(f3_pcx - float((f3_w - 1) / 32)) < 1e-4f &&
            std::fabs(f3_pcy - float((f3_h - 1) / 32)) < 1e-4f &&
            std::fabs(f3_cells_x - 32.0f) < 1e-4f &&
            std::fabs(f3_cells_y - 32.0f) < 1e-4f) {
            out.magic.assign(kMagic);
            out.version = le_u32_local(0x17);
            out.f0 = le_f32_local(0x1B);
            out.f1 = le_f32_local(0x1F);
            out.u0 = f3_w;
            out.u1 = f3_h;
            out.f2 = f3_spacing;
            out.body_offset = 0x47;
            out.body_size = uint32_t(bytes.size() - 0x47);
            out.ok = true;
            return;
        }
    }

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
                          HeightfieldFiles&  out)
{
    out = {};

    std::string err;

    if (!ehf_path.empty()) {
        if (!extract_bnk_file_by_relpath(ehf_path, out.ehf_bytes, err)) {
            out.error = ".ehf load failed: " + err;
            return false;
        }
        parse_ehf_header(out.ehf_bytes, out.ehf_header);
        if (out.ehf_header.magic.empty()) {
            out.error = ".ehf magic mismatch - not a HeightFieldGraphicsFile";
            return false;
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

bool BuildTerrainMesh(const GhfHeights& hg, TerrainMesh& out)
{
    TerrainMeshTrace("BuildTerrainMesh: entered");
    out = {};
    if (!hg.ok || hg.width < 2 || hg.height < 2 ||
        hg.heights.size() != size_t(hg.width) * size_t(hg.height)) {
        TerrainMeshTrace("BuildTerrainMesh: rejected invalid height grid");
        OutputLog::error("  terrain mesh build rejected: invalid height grid");
        return false;
    }

    const uint32_t W = hg.width;
    const uint32_t H = hg.height;
    const float    tile = hg.tile_size > 0.f ? hg.tile_size : 0.5f;

    // This builder is shared by the existing F2 terrain path and the F3 path.
    // Protect the common allocation stage rather than changing either file
    // format's decoder.
    constexpr size_t kMaxTerrainMeshBytes = 512ull * 1024ull * 1024ull;

    const size_t w = size_t(W);
    const size_t h = size_t(H);
    if (w > std::numeric_limits<size_t>::max() / h) {
        OutputLog::error("  terrain mesh build rejected: vertex-count overflow");
        return false;
    }
    const size_t N = w * h;

    if (w - 1 > std::numeric_limits<size_t>::max() / (h - 1) ||
        (w - 1) * (h - 1) > std::numeric_limits<size_t>::max() / 2) {
        OutputLog::error("  terrain mesh build rejected: triangle-count overflow");
        return false;
    }
    const size_t tris = (w - 1) * (h - 1) * 2;
    TerrainMeshTrace("BuildTerrainMesh: counts grid=" + std::to_string(W) + "x" + std::to_string(H) + " verts=" + std::to_string(N) + " tris=" + std::to_string(tris));

    if (N > std::numeric_limits<size_t>::max() / 3 ||
        tris > std::numeric_limits<size_t>::max() / 3) {
        OutputLog::error("  terrain mesh build rejected: allocation-size overflow");
        return false;
    }

    const size_t position_floats = N * 3;
    const size_t normal_floats   = N * 3;
    const size_t uv_floats       = N * 2;
    const size_t index_values    = tris * 3;

    if (position_floats > std::numeric_limits<size_t>::max() - normal_floats ||
        position_floats + normal_floats >
            std::numeric_limits<size_t>::max() - uv_floats) {
        OutputLog::error("  terrain mesh build rejected: float-count overflow");
        return false;
    }
    const size_t total_float_values =
        position_floats + normal_floats + uv_floats;

    if (total_float_values >
            std::numeric_limits<size_t>::max() / sizeof(float) ||
        index_values >
            std::numeric_limits<size_t>::max() / sizeof(uint32_t)) {
        OutputLog::error("  terrain mesh build rejected: byte-size overflow");
        return false;
    }

    const size_t float_bytes = total_float_values * sizeof(float);
    const size_t index_bytes = index_values * sizeof(uint32_t);
    if (float_bytes > std::numeric_limits<size_t>::max() - index_bytes) {
        OutputLog::error("  terrain mesh build rejected: total-byte overflow");
        return false;
    }
    const size_t mesh_bytes = float_bytes + index_bytes;

    TerrainMeshTrace("BuildTerrainMesh: allocation bytes=" + std::to_string(mesh_bytes));

    OutputLog::info(
        "  terrain mesh allocation: grid=" + std::to_string(W) + "x" +
        std::to_string(H) +
        "  verts=" + std::to_string(N) +
        "  tris=" + std::to_string(tris) +
        "  estimated=" + std::to_string(mesh_bytes / (1024ull * 1024ull)) +
        " MiB");

    if (mesh_bytes > kMaxTerrainMeshBytes) {
        OutputLog::error(
            "  terrain mesh build rejected: estimated allocation " +
            std::to_string(mesh_bytes / (1024ull * 1024ull)) +
            " MiB exceeds 512 MiB safety limit");
        return false;
    }

    out.width  = W;
    out.height = H;
    out.min_height = hg.min_height;
    out.max_height = hg.max_height;
    out.origin_x = hg.f3_format ? hg.origin_x : 0.0f;
    out.origin_z = hg.f3_format ? hg.origin_z : 0.0f;

    try {
        TerrainMeshTrace("BuildTerrainMesh: before positions.resize");
        out.positions.resize(position_floats);
        TerrainMeshTrace("BuildTerrainMesh: after positions.resize");
        TerrainMeshTrace("BuildTerrainMesh: before normals.resize");
        out.normals.resize  (normal_floats);
        TerrainMeshTrace("BuildTerrainMesh: after normals.resize");
        TerrainMeshTrace("BuildTerrainMesh: before uvs.resize");
        out.uvs.resize      (uv_floats);
        TerrainMeshTrace("BuildTerrainMesh: after uvs.resize");
        TerrainMeshTrace("BuildTerrainMesh: before indices.resize");
        out.indices.resize  (index_values);
        TerrainMeshTrace("BuildTerrainMesh: after indices.resize");
    } catch (const std::bad_alloc&) {
        OutputLog::error(
            "  terrain mesh build failed: std::bad_alloc while allocating " +
            std::to_string(mesh_bytes / (1024ull * 1024ull)) + " MiB");
        out = {};
        return false;
    } catch (const std::length_error&) {
        OutputLog::error(
            "  terrain mesh build failed: std::length_error for " +
            std::to_string(W) + "x" + std::to_string(H) + " grid");
        out = {};
        return false;
    }

    TerrainMeshTrace("BuildTerrainMesh: allocations complete; entering vertex loop");

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

    TerrainMeshTrace("BuildTerrainMesh: completed successfully");
    out.ok = true;
    return true;
}


bool DecodeF3GhfHeights(const std::vector<uint8_t>& bytes,
                        GhfHeights& out)
{
    out = {};
    constexpr size_t kHeader = 28;
    constexpr size_t kRecord = 14;
    if (bytes.size() < kHeader) {
        out.error = "F3 GHF too small";
        return false;
    }

    auto le_u32 = [&](size_t o) -> uint32_t {
        return uint32_t(bytes[o]) |
               (uint32_t(bytes[o + 1]) << 8) |
               (uint32_t(bytes[o + 2]) << 16) |
               (uint32_t(bytes[o + 3]) << 24);
    };
    auto le_f32 = [&](size_t o) -> float {
        uint32_t u = le_u32(o);
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    };

    const uint32_t w = le_u32(0x0C);
    const uint32_t h = le_u32(0x10);
    const uint64_t need = uint64_t(kHeader) + uint64_t(w) * uint64_t(h) * kRecord;
    if (w < 2 || h < 2 || w > 8192 || h > 8192 || need > bytes.size()) {
        out.error = "F3 GHF dimensions/body are invalid";
        return false;
    }

    out.f3_format = true;
    out.origin_x = le_f32(0x00);
    out.origin_z = le_f32(0x04);
    out.base_height = le_f32(0x14);
    out.width = w;
    out.height = h;
    out.tile_size = 0.5f;
    out.heights.resize(size_t(w) * size_t(h));
    out.min_height = std::numeric_limits<float>::infinity();
    out.max_height = -std::numeric_limits<float>::infinity();

    for (size_t i = 0; i < out.heights.size(); ++i) {
        const float v = le_f32(kHeader + i * kRecord);
        if (!std::isfinite(v)) {
            out.error = "F3 GHF contains non-finite elevation";
            return false;
        }
        // The F3 GHF record float is the elevation sample itself. The
        // +0x14 header field is metadata, not an additional height offset.
        out.heights[i] = v;
        out.min_height = std::min(out.min_height, v);
        out.max_height = std::max(out.max_height, v);
    }

    out.ok = true;
    return true;
}

bool DecodeF3EhfTerrainMaterials(const std::vector<uint8_t>& bytes,
                                 F3EhfTerrainMaterials& out)
{
    out = {};
    constexpr char kMagic[] = "HeightFieldGraphicsFile";
    constexpr size_t kMagicLen = sizeof(kMagic) - 1;
    constexpr size_t kHeader = 0x47;
    constexpr size_t kPatchStride = 10440;

    if (bytes.size() < kHeader ||
        std::memcmp(bytes.data(), kMagic, kMagicLen) != 0) {
        out.error = "F3 EHF magic/header mismatch";
        return false;
    }

    auto le_u32 = [&](size_t o) -> uint32_t {
        return uint32_t(bytes[o]) |
               (uint32_t(bytes[o + 1]) << 8) |
               (uint32_t(bytes[o + 2]) << 16) |
               (uint32_t(bytes[o + 3]) << 24);
    };
    auto le_f32 = [&](size_t o) -> float {
        uint32_t u = le_u32(o);
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    };

    const uint32_t width = le_u32(0x23);
    const uint32_t height = le_u32(0x27);
    const float spacing = le_f32(0x2B);
    const uint32_t patch_x = uint32_t(le_f32(0x37));
    const uint32_t patch_y = uint32_t(le_f32(0x3B));
    const uint64_t patch_end = uint64_t(kHeader) +
        uint64_t(patch_x) * uint64_t(patch_y) * kPatchStride;
    if (width < 2 || height < 2 || width > 8192 || height > 8192 ||
        patch_x == 0 || patch_y == 0 ||
        (width - 1) / 32 != patch_x || (height - 1) / 32 != patch_y ||
        !std::isfinite(spacing) || spacing <= 0.0f || patch_end > bytes.size()) {
        out.error = "F3 EHF patch-grid header is invalid";
        return false;
    }

    // Verified F3 material table: count followed by entries consisting of
    // diffuse path, normal path, and 13 bytes of entry metadata. This is the
    // byte sequence present in both supplied Bowerstone Castle and Brightwall
    // Village EHF files. It is deliberately independent of the F2 EHF body.
    size_t pos = size_t(patch_end);
    if (pos + 4 > bytes.size()) {
        out.error = "F3 EHF ends before material-table count";
        return false;
    }
    const uint32_t count = le_u32(pos);
    pos += 4;
    if (count == 0 || count > 256) {
        out.error = "F3 EHF material count is implausible";
        return false;
    }

    out.materials.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        F3EhfTerrainMaterial m;
        std::string* dst[2] = { &m.diffuse, &m.normal };
        for (int s = 0; s < 2; ++s) {
            const size_t start = pos;
            while (pos < bytes.size() && bytes[pos] != 0) {
                ++pos;
                if (pos - start > 512) {
                    out.error = "F3 EHF material texture path is too long";
                    return false;
                }
            }
            if (pos >= bytes.size()) {
                out.error = "F3 EHF material texture path is unterminated";
                return false;
            }
            dst[s]->assign(reinterpret_cast<const char*>(bytes.data() + start),
                           pos - start);
            ++pos;
        }
        if (pos + 13 > bytes.size()) {
            out.error = "F3 EHF material metadata is truncated";
            return false;
        }
        pos += 13;
        out.materials.push_back(std::move(m));
    }

    out.count = count;
    out.ok = true;
    return true;
}

bool DecodeF3EhfHeights(const std::vector<uint8_t>& bytes,
                        GhfHeights& out)
{
    out = {};
    constexpr char kMagic[] = "HeightFieldGraphicsFile";
    constexpr size_t kMagicLen = sizeof(kMagic) - 1;
    constexpr size_t kHeader = 0x47;
    constexpr size_t kPatchStride = 10440;
    constexpr uint32_t kPatchCells = 32;
    constexpr uint32_t kPatchWidth = 33;
    constexpr uint32_t kPatchHeight = 33;
    constexpr size_t kPatchHeader = 32;
    constexpr size_t kVertexStride = 8;

    if (bytes.size() < kHeader ||
        std::memcmp(bytes.data(), kMagic, kMagicLen) != 0) {
        out.error = "F3 EHF magic/header mismatch";
        return false;
    }

    auto le_u32 = [&](size_t o) -> uint32_t {
        return uint32_t(bytes[o]) |
               (uint32_t(bytes[o + 1]) << 8) |
               (uint32_t(bytes[o + 2]) << 16) |
               (uint32_t(bytes[o + 3]) << 24);
    };
    auto le_f32 = [&](size_t o) -> float {
        uint32_t u = le_u32(o);
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    };

    const float origin_x = le_f32(0x1B);
    const float origin_z = le_f32(0x1F);
    const uint32_t width = le_u32(0x23);
    const uint32_t height = le_u32(0x27);
    const float spacing = le_f32(0x2B);
    const float patch_count_x_f = le_f32(0x37);
    const float patch_count_y_f = le_f32(0x3B);
    const float patch_cells_x = le_f32(0x3F);
    const float patch_cells_y = le_f32(0x43);

    if (!std::isfinite(origin_x) || !std::isfinite(origin_z) ||
        !std::isfinite(spacing) || spacing <= 0.0f ||
        width < 2 || height < 2 || width > 8192 || height > 8192) {
        out.error = "F3 EHF file-level fields are invalid";
        return false;
    }
    if ((width - 1) % kPatchCells != 0 ||
        (height - 1) % kPatchCells != 0) {
        out.error = "F3 EHF dimensions are not a 32-cell patch grid";
        return false;
    }

    const uint32_t patch_count_x = (width - 1) / kPatchCells;
    const uint32_t patch_count_y = (height - 1) / kPatchCells;
    if (std::fabs(patch_count_x_f - float(patch_count_x)) > 1e-4f ||
        std::fabs(patch_count_y_f - float(patch_count_y)) > 1e-4f ||
        std::fabs(patch_cells_x - float(kPatchCells)) > 1e-4f ||
        std::fabs(patch_cells_y - float(kPatchCells)) > 1e-4f) {
        out.error = "F3 EHF patch-grid header fields disagree with resolution";
        return false;
    }

    const uint64_t patch_count =
        uint64_t(patch_count_x) * uint64_t(patch_count_y);
    const uint64_t required =
        uint64_t(kHeader) + patch_count * uint64_t(kPatchStride);
    if (required > bytes.size()) {
        out.error = "F3 EHF ends before its declared patch grid";
        return false;
    }

    out.f3_format = true;
    out.origin_x = origin_x;
    out.origin_z = origin_z;
    out.width = width;
    out.height = height;
    out.tile_size = spacing;
    out.heights.assign(size_t(width) * size_t(height), 0.0f);
    std::vector<uint8_t> written(out.heights.size(), 0);
    out.min_height = std::numeric_limits<float>::infinity();
    out.max_height = -std::numeric_limits<float>::infinity();

    for (uint32_t px = 0; px < patch_count_x; ++px) {
        for (uint32_t py = 0; py < patch_count_y; ++py) {
            const uint64_t patch_index =
                uint64_t(px) * uint64_t(patch_count_y) + py;
            const size_t off =
                kHeader + size_t(patch_index) * kPatchStride;
            if (off + kPatchHeader > bytes.size()) {
                out.error = "F3 EHF patch header is truncated";
                out = {};
                return false;
            }

            const float min_x = le_f32(off + 0);
            const float min_y = le_f32(off + 4);
            const float min_z = le_f32(off + 8);
            const float max_x = le_f32(off + 12);
            const float max_y = le_f32(off + 16);
            const float max_z = le_f32(off + 20);
            const uint32_t patch_w = le_u32(off + 24);
            const uint32_t patch_h = le_u32(off + 28);

            if (patch_w != kPatchWidth || patch_h != kPatchHeight ||
                !std::isfinite(min_x) || !std::isfinite(min_y) ||
                !std::isfinite(min_z) || !std::isfinite(max_x) ||
                !std::isfinite(max_y) || !std::isfinite(max_z) ||
                max_x <= min_x || max_y <= min_y || max_z < min_z) {
                out.error = "F3 EHF patch header is invalid";
                out = {};
                return false;
            }

            const float expected_x =
                origin_x + float(px * kPatchCells) * spacing;
            const float expected_y =
                origin_z + float(py * kPatchCells) * spacing;
            if (std::fabs(min_x - expected_x) >
                    std::max(1e-3f, spacing * 1e-4f) ||
                std::fabs(min_y - expected_y) >
                    std::max(1e-3f, spacing * 1e-4f) ||
                std::fabs((max_x - min_x) / 32.0f - spacing) > 1e-3f ||
                std::fabs((max_y - min_y) / 32.0f - spacing) > 1e-3f) {
                out.error = "F3 EHF patch position/spacing does not match file header";
                out = {};
                return false;
            }

            const size_t samples_off = off + kPatchHeader;
            const size_t samples_bytes =
                size_t(kPatchWidth) * size_t(kPatchHeight) * kVertexStride;
            if (samples_off + samples_bytes > bytes.size()) {
                out.error = "F3 EHF patch elevation data is truncated";
                out = {};
                return false;
            }

            for (uint32_t vy = 0; vy < kPatchHeight; ++vy) {
                for (uint32_t vx = 0; vx < kPatchWidth; ++vx) {
                    const size_t src =
                        samples_off +
                        (size_t(vy) * kPatchWidth + vx) * kVertexStride;
                    const float h = le_f32(src);
                    if (!std::isfinite(h) ||
                        h < min_z - 1e-2f || h > max_z + 1e-2f) {
                        out.error = "F3 EHF patch contains invalid elevation";
                        out = {};
                        return false;
                    }

                    const uint32_t gx = px * kPatchCells + vx;
                    const uint32_t gy = py * kPatchCells + vy;
                    const size_t dst = size_t(gy) * width + gx;
                    if (!written[dst]) {
                        out.heights[dst] = h;
                        written[dst] = 1;
                    } else if (std::fabs(out.heights[dst] - h) > 1e-3f) {
                        out.error = "F3 EHF overlapping patch elevations disagree";
                        out = {};
                        return false;
                    }
                    out.min_height = std::min(out.min_height, h);
                    out.max_height = std::max(out.max_height, h);
                }
            }
        }
    }

    for (uint8_t v : written) {
        if (!v) {
            out.error = "F3 EHF patch grid left an unwritten terrain sample";
            out = {};
            return false;
        }
    }

    out.ok = true;
    return true;
}


}
