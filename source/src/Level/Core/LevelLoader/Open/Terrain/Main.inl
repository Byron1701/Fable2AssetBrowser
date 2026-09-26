    if (bail_if_cancelled("pre-heightfield")) return false;

    const std::string level_name_lower = [&]() {
        std::string s = entry.name;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }();
    // Fable 3 also contains UI map levels (for example
    // MistpeakValleyMap). They use terrain resources for the miniature
    // world-map presentation but are not gameplay terrain levels. In
    // particular they can have an EHF/GHF without the sibling terrain LMP.
    // Do not send these heavyweight preview terrains through the normal
    // gameplay TerrainMesh path.
    const bool is_ui_map_level =
        level_name_lower.size() >= 3 &&
        level_name_lower.compare(level_name_lower.size() - 3, 3, "map") == 0;
    if (is_ui_map_level) {
        OutputLog::info("terrain: skipping UI map level '" + entry.name +
                        "' (miniature world-map terrain)");
    }

    if (!is_ui_map_level && (!res.ehf_path.empty() || !res.ghf_path.empty())) {
        HeightfieldFiles hf;
        loader_progress_update(32, 100, "Loading heightfield files...");
        if (!LoadHeightfieldFiles(res.ehf_path, res.ghf_path,
                                  res.hdb_path, res.genv_path, hf)) {
            OutputLog::error("heightfield load failed: " + hf.error);
        } else if (S.cancel_requested.load()) {
            OutputLog::warn("level load cancelled during heightfield load");
            return false;
        } else {
            g_pending_terrain_lightmap = {};
            if (res.has_terrain_lightmap_key && !res.lmp_path.empty()) {
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
            }

            std::ostringstream hos;
            hos << "heightfield loaded:"
                << "  ehf=" << hf.ehf_bytes.size() << "B"
                << "  ghf=" << hf.ghf_bytes_compressed.size() << "B (gz)"
                << " -> " << hf.ghf_bytes_raw.size() << "B (raw)";
            OutputLog::success(hos.str());

            if (!hf.ghf_bytes_raw.empty()) {
                GhfHeights hg;
                loader_progress_update(45, 100, "Decoding height grid...");
                bool ghf_ok = DecodeGhfHeights(hf.ghf_bytes_raw, hg);
                if (!ghf_ok) {
                    ghf_ok = DecodeF3GhfHeights(hf.ghf_bytes_raw, hg);
                }
                if (!ghf_ok) {
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

                    TerrainMesh mesh;
                    loader_progress_update(58, 100, "Building terrain mesh...");
                    if (S.cancel_requested.load()) {
                        OutputLog::warn("level load cancelled before terrain mesh build");
                        return false;
                    }
                    // Gameplay GHF files can be substantially larger than the
                    // viewport needs for an initial load. Building the full vertex/
                    // normal/index arrays on Windows was causing the loader to stall
                    // here at 58%. Keep the source heightfield intact, but construct
                    // a bounded TerrainMesh preview until the dedicated streaming
                    // terrain renderer consumes the native grid.
                    constexpr size_t kTerrainPreviewVertices = 1048576;
                    const size_t full_vertex_count =
                        size_t(hg.width) * size_t(hg.height);
                    Level::GhfHeights preview_hg;
                    const Level::GhfHeights* render_hg = &hg;
                    if (full_vertex_count > kTerrainPreviewVertices) {
                        const double scale =
                            std::sqrt(double(full_vertex_count) /
                                      double(kTerrainPreviewVertices));
                        const uint32_t pw = std::max<uint32_t>(
                            2, uint32_t(std::ceil(double(hg.width) / scale)));
                        const uint32_t ph = std::max<uint32_t>(
                            2, uint32_t(std::ceil(double(hg.height) / scale)));
                        preview_hg = hg;
                        preview_hg.width = pw;
                        preview_hg.height = ph;
                        preview_hg.heights.resize(size_t(pw) * ph);
                        preview_hg.min_height =
                            std::numeric_limits<float>::infinity();
                        preview_hg.max_height =
                            -std::numeric_limits<float>::infinity();
                        for (uint32_t py = 0; py < ph; ++py) {
                            const uint32_t sy = std::min<uint32_t>(
                                hg.height - 1,
                                uint32_t((uint64_t(py) * (hg.height - 1)) /
                                         std::max<uint32_t>(1, ph - 1)));
                            for (uint32_t px = 0; px < pw; ++px) {
                                const uint32_t sx = std::min<uint32_t>(
                                    hg.width - 1,
                                    uint32_t((uint64_t(px) * (hg.width - 1)) /
                                             std::max<uint32_t>(1, pw - 1)));
                                const float v =
                                    hg.heights[size_t(sy) * hg.width + sx];
                                preview_hg.heights[size_t(py) * pw + px] = v;
                                preview_hg.min_height =
                                    std::min(preview_hg.min_height, v);
                                preview_hg.max_height =
                                    std::max(preview_hg.max_height, v);
                            }
                        }
                        OutputLog::info(
                            "  terrain mesh preview: " +
                            std::to_string(hg.width) + "x" +
                            std::to_string(hg.height) + " -> " +
                            std::to_string(pw) + "x" +
                            std::to_string(ph) +
                            " vertices=" +
                            std::to_string(size_t(pw) * ph));
                        render_hg = &preview_hg;
                    }
#ifdef _WIN32
                    const bool terrain_built = BuildTerrainMesh(*render_hg, mesh);
#else
                    const bool terrain_built =
                        BuildTerrainMesh(*render_hg, mesh);
#endif
                    if (!terrain_built) {
                        OutputLog::error("  terrain mesh build failed");
                    } else {
                        const size_t tri_count = mesh.indices.size() / 3;
                        std::ostringstream mos;
                        mos << "  terrain mesh: verts=" << (mesh.positions.size() / 3)
                            << "  tris=" << tri_count;
                        OutputLog::success(mos.str());

                        g_pending_terrain_mesh        = std::move(mesh);
                        g_pending_terrain_label       = entry.name;
                        g_pending_terrain_level_entry = entry;
                        g_pending_terrain_ehf_bytes   = hf.ehf_bytes;
                        g_pending_adjacent_terrain_meshes.clear();
