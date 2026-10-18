    if (bail_if_cancelled("pre-heightfield")) return false;

    if (!res.ehf_path.empty() || !res.ghf_path.empty()) {
        if (info.version == 13) {
            OutputLog::warn("F3 GHF DIAG 1/5: selected path='" +
                            (res.ghf_path.empty() ? std::string("(missing)") : res.ghf_path) +
                            "'");
        }
        HeightfieldFiles hf;
        loader_progress_update(32, 100, "Loading heightfield files...");
        if (!LoadHeightfieldFiles(res.ehf_path, res.ghf_path,
                                  res.hdb_path, res.genv_path, hf,
                                  info.version == 13)) {
            OutputLog::error("heightfield load failed: " + hf.error);
        } else if (S.cancel_requested.load()) {
            OutputLog::warn("level load cancelled during heightfield load");
            return false;
        } else {
            g_pending_terrain_lightmap = {};
            if (info.version != 13 &&
                res.has_terrain_lightmap_key && !res.lmp_path.empty()) {
                std::vector<uint8_t> lmp_bytes;
                if (!load_text_sibling(res.lmp_path, lmp_bytes)) {
                    OutputLog::warn("terrain lightmap: sibling LMP not found: " +
                                    res.lmp_path);
                } else {
                    Level::TerrainLightmap decoded;
                    if (!Level::DecodeTerrainLightmap(
                            lmp_bytes, res.terrain_lightmap_key, decoded)) {
                        OutputLog::warn("terrain lightmap: " + decoded.error);
                    } else if (hf.ehf_header.ok &&
                               (decoded.sample_width != hf.ehf_header.u0 ||
                                decoded.sample_height != hf.ehf_header.u1)) {
                        std::ostringstream mismatch;
                        mismatch << "terrain lightmap: LMP sample grid "
                                 << decoded.sample_width << "x"
                                 << decoded.sample_height
                                 << " does not match EHF "
                                 << hf.ehf_header.u0 << "x"
                                 << hf.ehf_header.u1;
                        OutputLog::warn(mismatch.str());
                    } else {
                        std::ostringstream loaded;
                        loaded << "terrain lightmap: decoded LMP key 0x"
                               << std::hex << decoded.key << std::dec << "  "
                               << decoded.texture_width << "x"
                               << decoded.texture_height << " PF"
                               << decoded.pixel_format;
                        OutputLog::success(loaded.str());
                        g_pending_terrain_lightmap = std::move(decoded);
                    }
                }
            } else if (info.version == 13 && res.has_terrain_lightmap_key) {
                OutputLog::info("F3 terrain: legacy LMP decoder skipped");
            }

            std::ostringstream hos;
            hos << "heightfield loaded:"
                << "  ehf=" << hf.ehf_bytes.size() << "B"
                << "  ghf=" << hf.ghf_bytes_compressed.size() << "B (compressed)"
                << " -> " << hf.ghf_bytes_raw.size() << "B (decompressed)";
            OutputLog::success(hos.str());

            if (info.version == 13) {
                std::ostringstream provenance;
                provenance << "F3 GHF DIAG 2/5: extracted from BNK='"
                           << (hf.ghf_source_bnk.empty() ? "(unknown)" : hf.ghf_source_bnk)
                           << "', entry index=" << hf.ghf_source_index;
                OutputLog::warn(provenance.str());

                std::ostringstream sizes;
                sizes << "F3 GHF DIAG 3/5: compressed bytes="
                       << hf.ghf_bytes_compressed.size()
                       << ", decompressed bytes=" << hf.ghf_bytes_raw.size();
                OutputLog::warn(sizes.str());

                if (hf.ghf_bytes_raw.size() >= 0x14) {
                    const uint8_t* gh = hf.ghf_bytes_raw.data();
                    const uint32_t raw_width =
                        uint32_t(gh[0x0C]) | (uint32_t(gh[0x0D]) << 8) |
                        (uint32_t(gh[0x0E]) << 16) | (uint32_t(gh[0x0F]) << 24);
                    const uint32_t raw_height =
                        uint32_t(gh[0x10]) | (uint32_t(gh[0x11]) << 8) |
                        (uint32_t(gh[0x12]) << 16) | (uint32_t(gh[0x13]) << 24);
                    std::ostringstream raw_header;
                    raw_header << "F3 GHF DIAG 4/5: decompressed header dimensions="
                               << raw_width << "x" << raw_height
                               << ", raw header bytes available=" << hf.ghf_bytes_raw.size();
                    OutputLog::warn(raw_header.str());
                } else {
                    OutputLog::warn("F3 GHF DIAG 4/5: decompressed GHF is shorter than 0x14 bytes; cannot read dimensions");
                }
            }

            if (!hf.ghf_bytes_raw.empty()) {
                GhfHeights hg;
                loader_progress_update(45, 100, "Decoding height grid...");
                const bool ghf_decoded = (info.version == 13)
                    ? DecodeF3GhfHeights(hf.ghf_bytes_raw, hg)
                    : DecodeGhfHeights(hf.ghf_bytes_raw, hg);
                if (!ghf_decoded) {
                    OutputLog::error("  .ghf decode failed: " + hg.error);
                } else {
                    if (hg.tile_size <= 0.0f) {
                        const float ehf_tile = hf.ehf_header.ok
                                             ? hf.ehf_header.f2 : 0.0f;
                        const float fallback =
                            (ehf_tile > 0.0f && std::isfinite(ehf_tile))
                                ? ehf_tile : 0.5f;
                        std::ostringstream tos;
                        tos << "  .ghf tile_size was 0 - using .ehf f2 = "
                            << fallback << " (world = "
                            << (hg.width  - 1) * fallback << " x "
                            << (hg.height - 1) * fallback << ")";
                        OutputLog::info(tos.str());
                        hg.tile_size = fallback;
                    }

                    std::ostringstream gos;
                    gos << "  .ghf heightmap: " << hg.width << "x" << hg.height
                        << "  tile=" << hg.tile_size
                        << "  h=[" << hg.min_height << ".." << hg.max_height << "]";
                    OutputLog::success(gos.str());
                    if (info.version == 13) {
                        std::ostringstream decoded_diag;
                        decoded_diag << "F3 GHF DIAG 5/5: decoded grid="
                                     << hg.width << "x" << hg.height
                                     << ", samples=" << hg.heights.size()
                                     << ", height range=[" << hg.min_height
                                     << ".." << hg.max_height << "]"
                                     << ", origin=(" << hg.origin_x << ", "
                                     << hg.origin_z << "), base_height="
                                     << hg.base_height;
                        OutputLog::warn(decoded_diag.str());
                    }

                    TerrainMesh mesh;
                    loader_progress_update(58, 100, "Building terrain mesh...");
                    if (S.cancel_requested.load()) {
                        OutputLog::warn("level load cancelled before terrain mesh build");
                        return false;
                    }

                    // Keep a separate log marker around the synchronous mesh
                    // build. The progress label otherwise remains at 58% even
                    // if a later terrain handoff/render step is where loading
                    // actually stalls.
                    {
                        std::ostringstream start_mesh;
                        start_mesh << "  terrain mesh build starting: "
                                   << hg.width << "x" << hg.height
                                   << " vertices=" << (size_t(hg.width) * hg.height)
                                   << " decoded_samples=" << hg.heights.size()
                                   << " heights=[" << hg.min_height
                                   << ".." << hg.max_height << "]";
                        if (info.version == 13) {
                            start_mesh << " origin=(" << hg.origin_x << ", "
                                       << hg.origin_z << ") base_height="
                                       << hg.base_height;
                            OutputLog::warn("F3 GHF DIAG 5/5 (immediately before BuildF3TerrainMesh): "
                                            + start_mesh.str());
                        } else {
                            OutputLog::info(start_mesh.str());
                        }
                    }
#ifdef _WIN32
                    const bool terrain_built = (info.version == 13)
                        ? BuildF3TerrainMesh(hg, mesh)
                        : BuildTerrainMesh(hg, mesh);
#else
                    constexpr size_t kLinuxTerrainPreviewVertices = 262144;
                    const size_t full_vertex_count =
                        size_t(hg.width) * size_t(hg.height);
                    Level::GhfHeights preview_hg;
                    const Level::GhfHeights* render_hg = &hg;
                    if (full_vertex_count > kLinuxTerrainPreviewVertices) {
                        preview_hg = make_linux_preview_heightfield(
                            hg, kLinuxTerrainPreviewVertices);
                        render_hg = &preview_hg;
                    }
                    const bool terrain_built = (info.version == 13)
                        ? BuildF3TerrainMesh(*render_hg, mesh)
                        : BuildTerrainMesh(*render_hg, mesh);
#endif
                    // Keep F3 mesh diagnostics in the level-loading dialog,
                    // where the user can see the active load stage.
                    if (info.version == 13) {
                        std::ostringstream trace;
                        trace << "F3 TRACE after BuildF3TerrainMesh: "
                              << "built=" << (terrain_built ? "yes" : "no")
                              << ", source=" << hg.width << "x" << hg.height
                              << ", mesh vertices=" << (mesh.positions.size() / 3)
                              << ", indices=" << mesh.indices.size();
                        if (terrain_built && mesh.positions.size() >= 3) {
                            float min_y = mesh.positions[1];
                            float max_y = mesh.positions[1];
                            for (size_t i = 1; i < mesh.positions.size(); i += 3) {
                                const float y = mesh.positions[i];
                                if (y < min_y) min_y = y;
                                if (y > max_y) max_y = y;
                            }
                            trace << ", mesh Y=[" << min_y << ".." << max_y << "]";
                        }
                        OutputLog::warn(trace.str());
                        loader_progress_update(60, 100, trace.str());
                    }
                    if (!terrain_built) {
                        OutputLog::error("  terrain mesh build failed");
                    } else {
                        const size_t tri_count = mesh.indices.size() / 3;
                        {
                            std::ostringstream finished_mesh;
                            finished_mesh << "  terrain mesh build finished: verts="
                                          << (mesh.positions.size() / 3)
                                          << " tris=" << tri_count;
                            OutputLog::info(finished_mesh.str());
                        }
                        std::ostringstream mos;
                        mos << "  terrain mesh: verts=" << (mesh.positions.size() / 3)
                            << "  tris=" << tri_count;
                        OutputLog::success(mos.str());

                        g_pending_terrain_mesh        = std::move(mesh);
                        g_pending_terrain_is_f3       = (info.version == 13);
                        g_pending_terrain_label       = entry.name;
                        g_pending_terrain_level_entry = entry;
                        g_pending_terrain_ehf_bytes   = hf.ehf_bytes;
                        g_pending_adjacent_terrain_meshes.clear();
