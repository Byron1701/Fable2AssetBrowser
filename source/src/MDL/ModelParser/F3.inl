// Fable III MDL adapter. The binary reader owns F3 parsing; this layer
// translates its result into the Asset Browser's existing MDL structures.

static bool parse_f3_mdl_info(const std::vector<unsigned char>& data,
                              MDLInfo& out)
{
    const std::vector<std::uint8_t> bytes(data.begin(), data.end());
    if (!F3MDL::Reader::IsF3MDL(bytes)) return false;

    F3MDL::Reader reader;
    std::string error;
    if (!reader.Load(bytes, &error)) return false;

    const auto& skel = reader.GetSkeleton();
    const auto& mats = reader.GetMaterials();
    const auto& meshes = reader.GetMeshes();

    out = {};
    out.Magic = "F3MDL";
    out.HeaderSize = 0x0C;
    out.BoneCount = static_cast<std::uint32_t>(skel.bones.size());
    out.BoneTransformCount = out.BoneCount;
    out.HasBoneTransforms = !skel.bones.empty();

    out.Bones.reserve(skel.bones.size());
    out.BoneTransforms.reserve(skel.bones.size());
    for (std::size_t i = 0; i < skel.bones.size(); ++i) {
        const auto& b = skel.bones[i];

        MDLBoneInfo bi;
        bi.Name = "bone_" + std::to_string(i);
        bi.ParentID = b.parent;
        out.Bones.push_back(std::move(bi));

        out.BoneTransforms.push_back({
            b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w,
            b.translation.x, b.translation.y, b.translation.z,
            b.scale.x, b.scale.y, b.scale.z,
            b.extra
        });
    }

    out.MeshCount = static_cast<std::uint32_t>(meshes.size());
    out.Meshes.reserve(meshes.size());
    out.MeshBuffers.reserve(meshes.size());

    for (const auto& m : meshes) {
        MDLMeshInfo mi;
        mi.MeshName = m.name;
        mi.MaterialCount = 1;

        MDLMaterialInfo mat;
        if (m.materialIndex < mats.size()) {
            const auto& fm = mats[m.materialIndex];
            mat.DiffuseTexName  = fm.textures[0];
            mat.NormalTexName   = fm.textures[1];
            mat.SpecularTexName = fm.textures[2];
            mat.MetallicTexName = fm.textures[3];
            mat.ExtraTexName    = fm.textures[4];
        }
        mi.Materials.push_back(std::move(mat));
        out.Meshes.push_back(std::move(mi));

        MDLMeshBufferInfo mb;
        mb.VertexCount = static_cast<std::uint32_t>(m.vertices.size());
        mb.FaceCount = static_cast<std::uint32_t>(m.triangles.size() * 3);
        mb.SubMeshCount = 1;
        mb.MeshIndex = m.meshIndex;
        mb.SubMeshes.push_back({
            0,
            static_cast<std::uint32_t>(m.triangles.size()),
            0,
            0
        });
        out.MeshBuffers.push_back(std::move(mb));
    }

    return true;
}

static bool build_f3_mdl_geometry(const std::vector<unsigned char>& data,
                                  std::vector<MDLMeshGeom>& out)
{
    out.clear();

    const std::vector<std::uint8_t> bytes(data.begin(), data.end());
    if (!F3MDL::Reader::IsF3MDL(bytes)) return false;

    F3MDL::Reader reader;
    std::string error;
    if (!reader.Load(bytes, &error)) return false;

    const auto& mats = reader.GetMaterials();
    const auto& meshes = reader.GetMeshes();

    bool any = false;
    out.reserve(meshes.size());

    for (const auto& m : meshes) {
        if (m.vertices.empty() || m.triangles.empty()) continue;

        MDLMeshGeom g;
        g.positions.resize(m.vertices.size() * 3);
        g.normals.resize(m.vertices.size() * 3);
        g.uvs.resize(m.vertices.size() * 2);
        g.indices.reserve(m.triangles.size() * 3);

        if (m.skeletal) {
            g.bone_ids.assign(m.vertices.size() * 4, 0);
            g.bone_weights.assign(m.vertices.size() * 4, 0.0f);
        }

        for (std::size_t v = 0; v < m.vertices.size(); ++v) {
            const auto& src = m.vertices[v];

            g.positions[v * 3 + 0] = src.position.x;
            g.positions[v * 3 + 1] = src.position.y;
            g.positions[v * 3 + 2] = src.position.z;

            g.normals[v * 3 + 0] = src.normal.x;
            g.normals[v * 3 + 1] = src.normal.y;
            g.normals[v * 3 + 2] = src.normal.z;

            g.uvs[v * 2 + 0] = src.uv.x;
            g.uvs[v * 2 + 1] = src.uv.y;

            if (m.skeletal) {
                float sum = 0.0f;
                int out_bone = 0;

                for (int k = 0; k < 4; ++k) {
                    if (src.boneWeights[k] == 0 || out_bone >= 4) continue;

                    const std::uint8_t local_id = src.boneIndices[k];
                    std::uint32_t bone_id = local_id;

                    // F3 stores a per-mesh bone table. Vertex bone indices
                    // refer to that local table, so translate them to the
                    // skeleton bone IDs consumed by the existing renderer.
                    if (local_id < m.boneIds.size()) {
                        bone_id = m.boneIds[local_id];
                    }

                    if (bone_id > 0xFFFFu) continue;

                    g.bone_ids[v * 4 + out_bone] =
                        static_cast<std::uint16_t>(bone_id);
                    g.bone_weights[v * 4 + out_bone] =
                        static_cast<float>(src.boneWeights[k]) / 255.0f;
                    sum += g.bone_weights[v * 4 + out_bone];
                    ++out_bone;
                }

                if (sum > 1e-6f) {
                    for (int k = 0; k < out_bone; ++k) {
                        g.bone_weights[v * 4 + k] /= sum;
                    }
                } else {
                    g.bone_ids[v * 4] = 0;
                    g.bone_weights[v * 4] = 1.0f;
                }
            }
        }

        for (const auto& tri : m.triangles) {
            g.indices.push_back(tri[0]);
            g.indices.push_back(tri[1]);
            g.indices.push_back(tri[2]);
        }

        if (m.materialIndex < mats.size()) {
            const auto& mat = mats[m.materialIndex];
            g.diffuse_tex_name  = mat.textures[0];
            g.normal_tex_name   = mat.textures[1];
            g.specular_tex_name = mat.textures[2];
            g.metallic_tex_name = mat.textures[3];
            g.extra_tex_name    = mat.textures[4];
        }

        g.name = m.name.empty()
            ? ("mesh_" + std::to_string(m.meshIndex))
            : m.name;
        g.MeshIndex = m.meshIndex;
        g.SubMeshIndex = 0;
        g.alpha_test = true;
        g.cloth_sim = false;
        g.is_entity_model = m.skeletal;

        out.push_back(std::move(g));
        any = true;
    }

    return any;
}
