#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct FlatAssetEntry;

namespace Level {

struct HeightfieldHeader {
    std::string magic;
    bool        ok = false;
    bool        f3_layout = false;
    uint32_t    version = 0;
    float       prefix_float = 0.f;
    float       f0 = 0.f;
    float       f1 = 0.f;
    uint32_t    u0 = 0;
    uint32_t    u1 = 0;
    float       f2 = 0.f;
    float       f3 = 0.f;
    float       f4 = 0.f;
    uint32_t    body_offset = 0;
    uint32_t    body_size   = 0;
};

struct HeightfieldFiles {
    bool                  ok = false;
    std::string           error;

    HeightfieldHeader     ehf_header;
    std::vector<uint8_t>  ehf_bytes;
    std::vector<uint8_t>  ghf_bytes_compressed;
    std::vector<uint8_t>  ghf_bytes_raw;
    // Extraction provenance for diagnosing F3 GHF resource selection.
    std::string           ghf_source_bnk;
    int                   ghf_source_index = -1;
};

struct GhfHeights {
    bool                  ok        = false;
    uint32_t              width     = 0;
    uint32_t              height    = 0;
    // F3 GHF stores an absolute X/Z origin in its 28-byte little-endian header.
    float                 origin_x = 0.f;
    float                 origin_z = 0.f;
    float                 base_height = 0.f; // Header metadata; not added to samples.
    float                 tile_size = 64.f;
    float                 min_height = 0.f;
    float                 max_height = 0.f;
    std::vector<float>    heights;
    std::string           error;
};

const FlatAssetEntry* FindHeightfieldByPath(const std::string& relative_path);

bool LoadHeightfieldFiles(const std::string& ehf_path,
                          const std::string& ghf_path,
                          const std::string& hdb_path,
                          const std::string& genv_path,
                          HeightfieldFiles&  out,
                          bool f3_format = false);

bool DecodeGhfHeights(const std::vector<uint8_t>& ghf_bytes_raw,
                      GhfHeights&                 out);

// Fable III GHF has a distinct 28-byte little-endian header and 14-byte records.
// Kept separate so the existing Fable II decoding route is not changed.
bool DecodeF3GhfHeights(const std::vector<uint8_t>& ghf_bytes_raw,
                        GhfHeights&                 out);

struct TerrainMesh {
    bool                  ok = false;
    uint32_t              width  = 0;
    uint32_t              height = 0;

    std::vector<float>    positions;
    std::vector<float>    normals;
    std::vector<float>    uvs;
    std::vector<uint32_t> indices;

    float                 min_height = 0.f;
    float                 max_height = 0.f;
};

bool BuildTerrainMesh(const GhfHeights& heights, TerrainMesh& out);

// Separate F3 mesh builder with its own validation and allocation limits.
bool BuildF3TerrainMesh(const GhfHeights& heights, TerrainMesh& out);

}
