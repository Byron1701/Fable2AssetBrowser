#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <istream>

namespace F3MDL {

struct Vec2 { float x = 0.0f, y = 0.0f; };
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct Vec4 { float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f; };

struct Header {
    std::uint32_t fnvHash = 0;
    std::array<float, 10> origin{};
    std::uint32_t materialCount = 0;
    std::uint32_t staticMeshCount = 0;
    std::uint32_t skeletalMeshCount = 0;
    std::uint32_t planeMeshCount = 0;
    std::uint32_t unknownMeshCount0 = 0;
    std::uint32_t unknownMeshCount1 = 0;
};

struct Dummy {
    std::uint32_t nameHash = 0;
    std::int32_t id = 0;
};

struct Bone {
    std::uint32_t nameHash = 0;
    std::int32_t parent = -1;
    Vec3 translation{};
    Vec4 rotation{}; // x,y,z,w as stored by Keshire's importer
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float extra = 1.0f;
};

struct Skeleton {
    std::vector<Dummy> dummies;
    std::vector<std::uint32_t> hierarchyNameHashes;
    std::vector<std::int32_t> hierarchyParents;
    std::vector<Bone> bones;
};

struct Material {
    std::uint32_t type = 0;
    std::string name;
    std::array<std::string, 6> textures{};
    std::uint32_t textureBlockSize = 0;
    std::uint64_t fileOffset = 0;
};

struct MeshSplit {
    std::uint32_t unknown = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t indexStart = 0;
    std::array<float, 6> bounds{};
    std::uint32_t animatedUnknown = 0;
};

struct Vertex {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
    std::array<std::uint8_t, 4> boneIndices{{0,0,0,0}};
    std::array<std::uint8_t, 4> boneWeights{{0,0,0,0}};
    bool skinned = false;
};

// This is the geometry payload intended to map directly into the existing
// Asset Browser's MDLMeshGeom-style renderer/export path.
struct MDLMeshGeom {
    std::string name;
    std::uint32_t meshIndex = 0;
    std::uint32_t materialIndex = 0;
    bool skeletal = false;
    std::array<float, 10> origin{};
    std::vector<MeshSplit> splits;
    std::vector<Vertex> vertices;
    std::vector<std::array<std::uint16_t, 3>> triangles;
    std::vector<std::uint32_t> boneIds;
};

class Reader {
public:
    Reader() = default;

    bool Load(const std::string& path, std::string* error = nullptr);
    bool Load(const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);

    static bool IsF3MDL(const std::vector<std::uint8_t>& bytes);

    const Header& GetHeader() const { return header_; }
    const Skeleton& GetSkeleton() const { return skeleton_; }
    const std::vector<Material>& GetMaterials() const { return materials_; }
    const std::vector<MDLMeshGeom>& GetMeshes() const { return meshes_; }
    const std::vector<std::uint8_t>& GetBytes() const { return bytes_; }

    std::size_t GetFinalParsedOffset() const { return cursor_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t cursor_ = 0;
    Header header_{};
    Skeleton skeleton_{};
    std::vector<Material> materials_;
    std::vector<MDLMeshGeom> meshes_;

    bool Parse(std::string* error);
    bool ParseSkeleton(std::string* error);
    bool ParseMaterials(std::string* error);
    bool ParseMesh(std::uint32_t meshIndex, bool skeletal, MDLMeshGeom& mesh, std::string* error);

    bool Need(std::size_t n, std::string* error) const;
    bool Skip(std::size_t n, std::string* error);
    std::uint8_t ReadU8();
    std::uint16_t ReadU16();
    std::int16_t ReadI16();
    std::uint32_t ReadU32();
    std::int32_t ReadI32();
    float ReadF32();
    std::string ReadCString(std::string* error);
    std::array<float, 10> ReadFloat10();
    std::array<float, 6> ReadFloat6();

    static float HalfToFloat(std::uint16_t h);
    static bool LooksLikeTexturePath(const std::string& s);
    static void SetError(std::string* error, const std::string& message);
};

} // namespace F3MDL

// Convenience alias for projects that already use an MDLMeshGeom symbol.
// If the Asset Browser already defines its own MDLMeshGeom, remove this alias
// and copy the fields from F3MDL::MDLMeshGeom into that existing type.
