#include "F3MDLReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace F3MDL {

namespace {
constexpr std::size_t kFixedHeaderAfterHash = 12; // hash + 8-byte pad
constexpr std::size_t kType7PreTexture = 28;
constexpr std::size_t kType7PostTexture = 84;
// F3 type-11 layout observed in stone_wall/dog samples:
//   pre-texture parameter block = 32 bytes
//   texture block = size-prefixed
//   post-texture parameter block = 136 bytes
constexpr std::size_t kType11PreTexture = 40;
constexpr std::size_t kType11PostTexture = 132;

bool IsPrintable(unsigned char c) {
    return c >= 0x20 && c <= 0x7e;
}
}

void Reader::SetError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool Reader::Need(std::size_t n, std::string* error) const {
    if (cursor_ > bytes_.size() || n > bytes_.size() - cursor_) {
        std::ostringstream ss;
        ss << "F3 MDL truncated at 0x" << std::hex << cursor_
           << ": need 0x" << n << " bytes, have 0x"
           << (bytes_.size() - cursor_);
        SetError(error, ss.str());
        return false;
    }
    return true;
}

bool Reader::Skip(std::size_t n, std::string* error) {
    if (!Need(n, error)) return false;
    cursor_ += n;
    return true;
}

std::uint8_t Reader::ReadU8() {
    if (cursor_ >= bytes_.size())
        throw std::runtime_error("F3 MDL read past end (u8)");
    return bytes_[cursor_++];
}

std::uint16_t Reader::ReadU16() {
    if (bytes_.size() - std::min(cursor_, bytes_.size()) < 2)
        throw std::runtime_error("F3 MDL read past end (u16)");
    const std::uint16_t v = static_cast<std::uint16_t>(bytes_[cursor_]) |
                            (static_cast<std::uint16_t>(bytes_[cursor_ + 1]) << 8);
    cursor_ += 2;
    return v;
}

std::int16_t Reader::ReadI16() {
    return static_cast<std::int16_t>(ReadU16());
}

std::uint32_t Reader::ReadU32() {
    if (bytes_.size() - std::min(cursor_, bytes_.size()) < 4)
        throw std::runtime_error("F3 MDL read past end (u32)");
    const std::uint32_t v =
        static_cast<std::uint32_t>(bytes_[cursor_]) |
        (static_cast<std::uint32_t>(bytes_[cursor_ + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes_[cursor_ + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes_[cursor_ + 3]) << 24);
    cursor_ += 4;
    return v;
}

std::int32_t Reader::ReadI32() {
    return static_cast<std::int32_t>(ReadU32());
}

float Reader::ReadF32() {
    const std::uint32_t u = ReadU32();
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::string Reader::ReadCString(std::string* error) {
    const std::size_t start = cursor_;
    while (cursor_ < bytes_.size() && bytes_[cursor_] != 0) {
        ++cursor_;
    }
    if (cursor_ >= bytes_.size()) {
        SetError(error, "Unterminated F3 MDL string");
        return {};
    }
    std::string s(reinterpret_cast<const char*>(bytes_.data() + start), cursor_ - start);
    ++cursor_;
    return s;
}

std::array<float, 10> Reader::ReadFloat10() {
    std::array<float, 10> a{};
    for (float& v : a) v = ReadF32();
    return a;
}

std::array<float, 6> Reader::ReadFloat6() {
    std::array<float, 6> a{};
    for (float& v : a) v = ReadF32();
    return a;
}

float Reader::HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u;
    std::uint32_t exponent = (h >> 10) & 0x1fu;
    std::uint32_t fraction = h & 0x3ffu;

    if (exponent == 0) {
        if (fraction == 0) {
            const std::uint32_t bits = sign << 31;
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

        // Subnormal half: normalize the mantissa using a signed exponent
        // adjustment rather than underflowing the unsigned exponent field.
        int e = -14;
        while ((fraction & 0x400u) == 0) {
            fraction <<= 1;
            --e;
        }
        fraction &= 0x3ffu;

        const std::uint32_t exponent32 =
            static_cast<std::uint32_t>(e + 127);
        const std::uint32_t bits =
            (sign << 31) | (exponent32 << 23) | (fraction << 13);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    } else if (exponent == 31) {
        const std::uint32_t bits = (sign << 31) | 0x7f800000u | (fraction << 13);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    exponent = exponent + (127 - 15);
    const std::uint32_t bits = (sign << 31) | (exponent << 23) | (fraction << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool Reader::LooksLikeTexturePath(const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    bool printable = true;
    for (unsigned char c : s) {
        if (!IsPrintable(c)) { printable = false; break; }
    }
    if (!printable) return false;

    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(".tex") != std::string::npos ||
           lower.find("\\") != std::string::npos ||
           lower.find("/") != std::string::npos;
}

bool Reader::IsF3MDL(const std::vector<std::uint8_t>& bytes) {
    // Fable III MDLs do not have the older ASCII "MeshFile" magic.  They
    // begin with a 32-bit model hash followed by eight zero bytes.  This
    // function is deliberately only a FORMAT IDENTIFIER: structural
    // validation belongs in Parse().  Returning false here for a structurally
    // unusual F3 file causes the caller to fall through to the legacy parser,
    // which interprets the binary using an incompatible layout and can crash
    // the Browser.
    if (bytes.size() < 0x0c) return false;

    for (std::size_t i = 4; i < 12; ++i) {
        if (bytes[i] != 0) return false;
    }

    return true;
}

bool Reader::Load(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SetError(error, "Unable to open MDL: " + path);
        return false;
    }
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    bytes_.resize(size);
    if (size && !file.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(size))) {
        SetError(error, "Unable to read MDL: " + path);
        return false;
    }
    try {
        return Parse(error);
    } catch (const std::exception& e) {
        SetError(error, std::string("F3 MDL parse exception at 0x") + [&]{ std::ostringstream s; s << std::hex << cursor_; return s.str(); }() + ": " + e.what());
        return false;
    } catch (...) {
        SetError(error, "F3 MDL parse exception at unknown location");
        return false;
    }
}

bool Reader::Load(const std::vector<std::uint8_t>& bytes, std::string* error) {
    bytes_ = bytes;
    try {
        return Parse(error);
    } catch (const std::exception& e) {
        SetError(error, std::string("F3 MDL parse exception at 0x") + [&]{ std::ostringstream s; s << std::hex << cursor_; return s.str(); }() + ": " + e.what());
        return false;
    } catch (...) {
        SetError(error, "F3 MDL parse exception at unknown location");
        return false;
    }
}

bool Reader::Parse(std::string* error) {
    cursor_ = 0;
    header_ = {};
    skeleton_ = {};
    materials_.clear();
    meshes_.clear();

    if (!IsF3MDL(bytes_)) {
        SetError(error, "Not recognised as a Fable III MDL");
        return false;
    }

    header_.fnvHash = ReadU32();
    if (!Skip(8, error)) return false;

    if (!ParseSkeleton(error)) return false;

    header_.origin = ReadFloat10();
    header_.materialCount = ReadU32();
    header_.staticMeshCount = ReadU32();
    header_.skeletalMeshCount = ReadU32();
    header_.planeMeshCount = ReadU32();
    header_.unknownMeshCount0 = ReadU32();
    header_.unknownMeshCount1 = ReadU32();

    if (!Skip(1, error)) return false; // header pad

    // Node table. These are hash strings, not the skeleton hierarchy.
    if (!Need(4, error)) return false;
    const std::uint32_t nodeCount = ReadU32();
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        if (ReadCString(error).empty() && error && !error->empty()) return false;
    }

    if (!ParseMaterials(error)) return false;

    const std::uint32_t renderMeshCount = header_.staticMeshCount + header_.skeletalMeshCount;
    meshes_.reserve(renderMeshCount);
    for (std::uint32_t i = 0; i < renderMeshCount; ++i) {
        MDLMeshGeom mesh;
        const bool skeletal = i >= header_.staticMeshCount;
        if (!ParseMesh(i, skeletal, mesh, error)) return false;
        meshes_.push_back(std::move(mesh));
    }

    return true;
}

bool Reader::ParseSkeleton(std::string* error) {
    if (!Need(1, error)) return false;
    const std::uint8_t dummyCount = ReadU8();
    if (dummyCount > 64) {
        SetError(error, "Implausible F3 MDL dummy count");
        return false;
    }

    skeleton_.dummies.reserve(dummyCount);
    for (std::uint8_t i = 0; i < dummyCount; ++i) {
        Dummy d;
        d.nameHash = ReadU32();
        d.id = ReadI32();
        skeleton_.dummies.push_back(d);
    }

    const std::uint32_t hierarchyCount = ReadU32();
    if (hierarchyCount > 4096) {
        SetError(error, "Implausible F3 MDL hierarchy count");
        return false;
    }
    skeleton_.hierarchyNameHashes.resize(hierarchyCount);
    skeleton_.hierarchyParents.resize(hierarchyCount);
    for (std::uint32_t i = 0; i < hierarchyCount; ++i) {
        skeleton_.hierarchyNameHashes[i] = ReadU32();
        skeleton_.hierarchyParents[i] = ReadI32();
    }

    const std::uint32_t boneCount = ReadU32();
    if (boneCount > 4096) {
        SetError(error, "Implausible F3 MDL bone count");
        return false;
    }
    skeleton_.bones.reserve(boneCount);
    for (std::uint32_t i = 0; i < boneCount; ++i) {
        Bone b;
        b.nameHash = skeleton_.hierarchyNameHashes.size() > i
                   ? skeleton_.hierarchyNameHashes[i] : 0;
        b.parent = skeleton_.hierarchyParents.size() > i
                 ? skeleton_.hierarchyParents[i] : -1;

        // Keshire's importer interprets the 11 floats as:
        // quat(x,y,z,w), translation(x,y,z), scale(x,y,z), extra.
        const float m0 = ReadF32();
        const float m1 = ReadF32();
        const float m2 = ReadF32();
        const float m3 = ReadF32();
        b.translation = {ReadF32(), ReadF32(), ReadF32()};
        b.rotation = {m0, m1, m2, m3};
        b.scale = {ReadF32(), ReadF32(), ReadF32()};
        b.extra = ReadF32();
        skeleton_.bones.push_back(b);
    }

    return true;
}

bool Reader::ParseMaterials(std::string* error) {
    materials_.reserve(header_.materialCount);

    for (std::uint32_t i = 0; i < header_.materialCount; ++i) {
        Material mat;
        mat.fileOffset = cursor_;

        if (!Skip(4, error)) return false; // material preamble
        mat.name = ReadCString(error);
        if (error && !error->empty()) return false;
        if (!Skip(4, error)) return false; // constant/hash field
        mat.type = ReadU32();

        // These sizes are validated against both supplied F3 MDLs.
        // Type 11 is materially different from the older Keshire skip logic:
        // its texture block contains three path strings followed by binary
        // parameter data, so reading six strings is incorrect.
        std::size_t pre = 0;
        switch (mat.type) {
            case 1: pre = 4; break;
            case 2: pre = 4; break;
            case 7: pre = kType7PreTexture; break;
            case 11: pre = kType11PreTexture; break;
            default:
                SetError(error, "Unsupported F3 material type " + std::to_string(mat.type) +
                                " at 0x" + [&]{ std::ostringstream s; s << std::hex << mat.fileOffset; return s.str(); }());
                return false;
        }
        if (!Skip(pre, error)) return false;

        mat.textureBlockSize = ReadU32();
        const std::size_t blockStart = cursor_;
        if (!Need(mat.textureBlockSize, error)) return false;

        // Extract actual .tex/path strings without assuming six strings.
        // The supplied type-7 blocks contain three paths plus padding; type-11
        // likewise contains three paths plus binary material parameters.
        std::size_t p = blockStart;
        while (p < blockStart + mat.textureBlockSize && mat.textures[5].empty()) {
            const std::size_t candidate = p;
            while (p < blockStart + mat.textureBlockSize && bytes_[p] != 0) ++p;
            if (p > candidate) {
                std::string s(reinterpret_cast<const char*>(bytes_.data() + candidate), p - candidate);
                if (LooksLikeTexturePath(s)) {
                    auto it = std::find_if(mat.textures.begin(), mat.textures.end(),
                                           [](const std::string& v){ return v.empty(); });
                    if (it != mat.textures.end()) *it = s;
                }
            }
            if (p < blockStart + mat.textureBlockSize) ++p;
            else break;
            // Skip runs of binary zeros naturally; any printable binary run
            // that is not a path is ignored.
        }
        cursor_ = blockStart + mat.textureBlockSize;

        // The post-texture parameter sizes below include the two floats that
        // Keshire reads immediately after the texture strings. In the actual
        // F3 files the texture-block size already leads directly into this
        // complete post-texture region.
        std::size_t post = 0;
        switch (mat.type) {
            case 1: post = 0; break;
            case 2: post = 12; break;
            case 7: post = kType7PostTexture; break;
            case 11: post = kType11PostTexture; break;
        }
        if (!Skip(post, error)) return false;

        materials_.push_back(std::move(mat));
    }

    return true;
}

bool Reader::ParseMesh(std::uint32_t meshIndex, bool skeletal, MDLMeshGeom& mesh, std::string* error) {
    mesh.meshIndex = meshIndex;
    mesh.skeletal = skeletal;

    if (skeletal) {
        mesh.name = "AnimatedObject"; // Keshire convention; source MDL has no mesh name here.
    } else {
        mesh.name = ReadCString(error);
        if (error && !error->empty()) return false;
    }

    if (!Skip(1, error)) return false; // pad

    const std::uint32_t iMesh = ReadU32();
    const std::uint32_t iMaterial = ReadU32();
    const std::uint32_t nTris = ReadU32();
    const std::uint32_t unknown = ReadU32();
    const std::uint32_t nVerts = ReadU32();

    mesh.meshIndex = iMesh;
    mesh.materialIndex = iMaterial;

    // Validate allocation-driving counts before touching the vectors.  A bad
    // interpretation of an F3 mesh header must become a parse error, never a
    // giant allocation or subsequent out-of-bounds access.
    if (nVerts > 2'000'000u) {
        SetError(error, "Implausible F3 MDL vertex count " + std::to_string(nVerts) +
                         " at mesh " + std::to_string(meshIndex) +
                         " (offset 0x" + [&]{ std::ostringstream s; s << std::hex << cursor_; return s.str(); }() + ")");
        return false;
    }
    if (nTris > 4'000'000u) {
        SetError(error, "Implausible F3 MDL triangle count " + std::to_string(nTris) +
                         " at mesh " + std::to_string(meshIndex));
        return false;
    }

    if (iMaterial >= materials_.size()) {
        SetError(error, "F3 MDL mesh references invalid material index " + std::to_string(iMaterial));
        return false;
    }

    if (!skeletal) mesh.origin = ReadFloat10();

    const std::uint32_t splitCount = ReadU32();
    if (splitCount > 1024) {
        SetError(error, "Implausible F3 MDL mesh split count");
        return false;
    }

    mesh.splits.reserve(splitCount);
    std::uint64_t totalTris = 0;
    for (std::uint32_t s = 0; s < splitCount; ++s) {
        MeshSplit split;
        split.unknown = ReadU32();
        ReadU8(); // pad
        split.triangleCount = ReadU32();
        split.indexStart = ReadU32();
        split.bounds = ReadFloat6();
        if (skeletal) split.animatedUnknown = ReadU32();
        totalTris += split.triangleCount;
        mesh.splits.push_back(split);
    }

    if (totalTris != nTris) {
        // Keep parsing because the file's explicit split totals are the native
        // geometry count, but reject only impossible sizes.
        if (totalTris > 10'000'000) {
            SetError(error, "Implausible F3 MDL triangle count");
            return false;
        }
    }

    if (skeletal) {
        const std::uint32_t groupCount = ReadU32();
        if (groupCount > 1024) {
            SetError(error, "Implausible F3 MDL bone-group count");
            return false;
        }
        for (std::uint32_t g = 0; g < groupCount; ++g) {
            const std::uint32_t count = ReadU32();
            if (count > 256) {
                SetError(error, "Implausible F3 MDL bone-group size");
                return false;
            }
            for (std::uint32_t j = 0; j < count; ++j) {
                mesh.boneIds.push_back(ReadU32());
            }
        }
    }

    mesh.vertices.resize(nVerts);
    for (std::uint32_t i = 0; i < nVerts; ++i) {
        Vertex& v = mesh.vertices[i];
        v.skinned = skeletal;
        if (skeletal) {
            const std::uint16_t hx = ReadU16();
            const std::uint16_t hy = ReadU16();
            const std::uint16_t hz = ReadU16();
            ReadU16(); // fourth packed half, currently unknown
            v.position = {HalfToFloat(hx), HalfToFloat(hy), HalfToFloat(hz)};
            for (auto& b : v.boneIndices) b = ReadU8();
            for (auto& w : v.boneWeights) w = ReadU8();
            v.uv = {HalfToFloat(ReadU16()), HalfToFloat(ReadU16())};
        } else {
            const std::uint16_t hx = ReadU16();
            const std::uint16_t hy = ReadU16();
            const std::uint16_t hz = ReadU16();
            ReadU16(); // fourth packed half, unknown
            const std::uint16_t hu = ReadU16();
            const std::uint16_t hv = ReadU16();
            v.position = {HalfToFloat(hx), HalfToFloat(hy), HalfToFloat(hz)};
            v.uv = {HalfToFloat(hu), -HalfToFloat(hv)}; // Keshire convention
        }
    }

    // Second packed vertex stream. Keshire uses the first three half-floats as
    // the normal. The remaining five values are retained only by the game.
    for (std::uint32_t i = 0; i < nVerts; ++i) {
        Vertex& v = mesh.vertices[i];
        v.normal.x = HalfToFloat(ReadU16());
        v.normal.y = HalfToFloat(ReadU16());
        v.normal.z = HalfToFloat(ReadU16());
        ReadU16();
        ReadU16(); ReadU16(); ReadU16(); ReadU16();
    }

    if (skeletal) {
        const bool hasMorph = ReadU8() != 0;
        if (hasMorph) {
            if (!Skip(static_cast<std::size_t>(nVerts) * 8, error)) return false;
        }
    }

    mesh.triangles.reserve(static_cast<std::size_t>(totalTris));
    for (std::uint64_t i = 0; i < totalTris; ++i) {
        const std::int16_t a = ReadI16();
        const std::int16_t b = ReadI16();
        const std::int16_t c = ReadI16();
        if (a < 0 || b < 0 || c < 0 ||
            static_cast<std::uint32_t>(a) >= nVerts ||
            static_cast<std::uint32_t>(b) >= nVerts ||
            static_cast<std::uint32_t>(c) >= nVerts) {
            SetError(error, "F3 MDL contains an out-of-range triangle index");
            return false;
        }
        mesh.triangles.push_back({static_cast<std::uint16_t>(a),
                                  static_cast<std::uint16_t>(b),
                                  static_cast<std::uint16_t>(c)});
    }

    // Dynamic cloth follows the ordinary triangle list. We do not expose it
    // as render geometry yet, but we must consume the exact structure so the
    // next mesh begins at the correct offset.
    const std::uint32_t clothCount = ReadU32();
    for (std::uint32_t c = 0; c < clothCount; ++c) {
        const std::uint32_t n1 = ReadU32();
        const std::uint32_t n2 = ReadU32();
        const std::uint32_t nCVerts = ReadU32();
        const std::uint32_t n4 = ReadU32();
        const std::uint32_t n5 = ReadU32();
        const std::uint32_t nCTris = ReadU32();
        const std::uint32_t n7 = ReadU32();
        const std::uint32_t n8 = ReadU32();
        const std::uint32_t n9 = ReadU32();
        (void)n1; (void)n2; (void)n4; (void)n5; (void)n9;

        if (skeletal) {
            if (!Skip(static_cast<std::size_t>(nCVerts) * 4, error)) return false;
            if (!Skip(static_cast<std::size_t>(nCVerts) * 16, error)) return false;
        }

        const std::uint32_t n12 = ReadU32();
        const std::uint32_t n13 = ReadU32();
        const std::uint32_t n14 = ReadU32();
        const std::uint32_t n15 = ReadU32();
        const std::uint32_t n16 = ReadU32();
        (void)n13; (void)n15; (void)n16;

        if (!Skip(24, error)) return false; // six floats
        ReadU8(); // boolean
        ReadU8(); // boolean

        if (!Skip(static_cast<std::size_t>(nCVerts) * 12, error)) return false;
        if (!Skip(static_cast<std::size_t>(nCVerts) * 16, error)) return false;
        if (!Skip(static_cast<std::size_t>(nCVerts) * 16, error)) return false;
        if (!Skip(static_cast<std::size_t>(nCVerts) * 4, error)) return false;
        if (!Skip(static_cast<std::size_t>(nCTris) * 6, error)) return false;
        if (!Skip(static_cast<std::size_t>(n7) * 4, error)) return false;
        if (!Skip(static_cast<std::size_t>(n7) * 16, error)) return false;

        if (skeletal) {
            if (!Skip(n8, error)) return false;
        } else {
            if (!Skip(nCVerts, error)) return false;
        }
        if (!Skip(static_cast<std::size_t>(n12) * 32, error)) return false;
        if (!Skip(static_cast<std::size_t>(n14) * 56, error)) return false;
        const std::uint32_t tailCount = skeletal ? (n8 - n5) : (nCVerts - n5);
        if (!Skip(static_cast<std::size_t>(tailCount) * 8, error)) return false;
    }

    (void)unknown;
    return true;
}

} // namespace F3MDL
