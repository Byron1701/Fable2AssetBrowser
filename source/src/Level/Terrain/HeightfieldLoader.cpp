#include "HeightfieldLoader.h"

#include "Utilities/State.h"
#include "BNKCore.cpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
    out = {};
    if (!hg.ok || hg.width < 2 || hg.height < 2 ||
        hg.heights.size() != size_t(hg.width) * size_t(hg.height)) {
        return false;
    }

    const uint32_t W = hg.width;
    const uint32_t H = hg.height;
    const float    tile = hg.tile_size > 0.f ? hg.tile_size : 0.5f;
    const float    origin_x = hg.f3_format ? hg.origin_x : 0.0f;
    const float    origin_z = hg.f3_format ? hg.origin_z : 0.0f;
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
            out.positions[i * 3 + 0] = origin_x + float(x) * tile;
            out.positions[i * 3 + 1] = hg.heights[i];
            out.positions[i * 3 + 2] = origin_z + float(y) * tile;
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


bool DecodeF3GhfHeights(const std::vector<uint8_t>& bytes, GhfHeights& out)
{
    out = {};
    constexpr size_t H = 28, R = 14;
    if (bytes.size() < H) { out.error = "F3 GHF too small"; return false; }
    auto le32=[&](size_t o)->uint32_t { return uint32_t(bytes[o]) | (uint32_t(bytes[o+1])<<8) | (uint32_t(bytes[o+2])<<16) | (uint32_t(bytes[o+3])<<24); };
    auto lef=[&](size_t o)->float { uint32_t u=le32(o); float f; std::memcpy(&f,&u,4); return f; };
    const uint32_t w=le32(0x0c), h=le32(0x10);
    if (!w || !h || w>8192 || h>8192 || H + uint64_t(w)*h*R > bytes.size()) {
        out.error = "F3 GHF dimensions/body are invalid"; return false;
    }
    out.f3_format=true; out.origin_x=lef(0); out.origin_z=lef(4); out.base_height=lef(0x14);
    out.width=w; out.height=h; out.tile_size=0.5f; out.heights.resize(size_t(w)*h);
    out.min_height=std::numeric_limits<float>::infinity();
    out.max_height=-std::numeric_limits<float>::infinity();
    for(size_t i=0;i<out.heights.size();++i) {
        const float v=lef(H+i*R);
        if(!std::isfinite(v)) { out.error="F3 GHF contains non-finite elevation"; return false; }
        out.heights[i]=v + out.base_height;
        out.min_height=std::min(out.min_height,out.heights[i]);
        out.max_height=std::max(out.max_height,out.heights[i]);
    }
    out.ok=true; return true;
}

bool ParseF3EHF(const std::vector<uint8_t>& bytes, TerrainMesh& out, std::string* stats)
{
    out={}; if(stats) stats->clear();
    static constexpr char M[]="HeightFieldGraphicsFile";
    if(bytes.size()<0x47 || std::memcmp(bytes.data(),M,sizeof(M)-1)!=0) return false;
    auto le32=[&](size_t o)->uint32_t { return uint32_t(bytes[o]) | (uint32_t(bytes[o+1])<<8) | (uint32_t(bytes[o+2])<<16) | (uint32_t(bytes[o+3])<<24); };
    auto lef=[&](size_t o)->float { uint32_t u=le32(o); float f; std::memcpy(&f,&u,4); return f; };
    const uint32_t W=le32(0x23), H=le32(0x27);
    const float spacing=lef(0x2b);
    if(W<2||H<2||W>8192||H>8192||!(spacing>0.f && std::isfinite(spacing))) return false;
    const uint32_t pcx=le32(0x37), pcy=le32(0x3b);
    const uint32_t pw=le32(0x3f), ph=le32(0x43);
    if(pw!=32||ph!=32 || pcx==0 || pcy==0) return false;
    const size_t stride=32 + 33ull*33ull*8ull;
    if(0x47ull + uint64_t(pcx)*pcy*stride > bytes.size()) return false;
    out.width=W; out.height=H; out.min_height=std::numeric_limits<float>::infinity(); out.max_height=-std::numeric_limits<float>::infinity();
    out.positions.reserve(size_t(W)*H*3); out.normals.resize(size_t(W)*H*3); out.uvs.resize(size_t(W)*H*2);
    out.indices.reserve(size_t(W-1)*(H-1)*6);
    std::vector<float> heights(size_t(W)*H,0.f);
    for(uint32_t py=0;py<pcy;++py) for(uint32_t px=0;px<pcx;++px) {
        const size_t po=0x47 + (size_t(py)*pcx+px)*stride;
        const float ox=lef(po), oz=lef(po+4);
        const uint32_t vpw=le32(po+24), vph=le32(po+28);
        if(vpw!=33||vph!=33) return false;
        const float sx=(lef(po+16)-ox)/32.f, sz=(lef(po+20)-oz)/32.f;
        if(!(sx>0.f)||!(sz>0.f)) return false;
        for(uint32_t z=0;z<33;++z) for(uint32_t x=0;x<33;++x) {
            const uint32_t gx=px*32+x, gz=py*32+z;
            if(gx>=W||gz>=H) continue;
            const float hv=lef(po+32+(size_t(z)*33+x)*8);
            if(!std::isfinite(hv)) return false;
            heights[size_t(gz)*W+gx]=hv;
        }
    }
    for(uint32_t z=0;z<H;++z) for(uint32_t x=0;x<W;++x) {
        const size_t i=size_t(z)*W+x; const float wx=lef(0x1b)+x*spacing, wz=lef(0x1f)+z*spacing;
        out.positions.insert(out.positions.end(),{wx,heights[i],wz});
        out.uvs[i*2]=float(x)/float(W-1); out.uvs[i*2+1]=float(z)/float(H-1);
        out.min_height=std::min(out.min_height,heights[i]); out.max_height=std::max(out.max_height,heights[i]);
    }
    auto at=[&](int x,int z){x=std::clamp(x,0,int(W)-1);z=std::clamp(z,0,int(H)-1);return heights[size_t(z)*W+x];};
    for(uint32_t z=0;z<H;++z) for(uint32_t x=0;x<W;++x){float nx=at(int(x)-1,z)-at(int(x)+1,z),ny=2.f*spacing,nz=at(x,int(z)-1)-at(x,int(z)+1);float l=std::sqrt(nx*nx+ny*ny+nz*nz);if(l>1e-6f){nx/=l;ny/=l;nz/=l;}const size_t i=(size_t(z)*W+x)*3;out.normals[i]=nx;out.normals[i+1]=ny;out.normals[i+2]=nz;}
    size_t k=0; for(uint32_t z=0;z+1<H;++z) for(uint32_t x=0;x+1<W;++x){uint32_t a=z*W+x,b=a+1,c=(z+1)*W+x,d=c+1;out.indices.insert(out.indices.end(),{a,c,b,b,c,d});}
    out.ok=true; if(stats){*stats=std::to_string(pcx*pcy)+" F3 EHF patches, "+std::to_string(W)+"x"+std::to_string(H)+" vertices";} return true;
}

}
