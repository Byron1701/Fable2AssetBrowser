struct EhfRenderTileDesc {
    uint32_t cell_x = 0;
    uint32_t cell_y = 0;
    uint32_t cell_w = 0;
    uint32_t cell_h = 0;
    uint32_t sub_w  = 0;
    uint32_t sub_h  = 0;
    float    min_x  = 0.0f;
    float    min_y  = 0.0f;
    float    max_x  = 0.0f;
    float    max_y  = 0.0f;
    bool     bbox_ok = false;
};

struct EhfEmbeddedBc1Mip {
    size_t   offset = 0;
    uint32_t header_w = 0;
    uint32_t header_h = 0;
    uint32_t raw_size = 0;
    uint32_t comp_size = 0;
};

static uint32_t ehf_be32(const std::vector<uint8_t>& d, size_t off)
{
    return (uint32_t(d[off + 0]) << 24) |
           (uint32_t(d[off + 1]) << 16) |
           (uint32_t(d[off + 2]) <<  8) |
            uint32_t(d[off + 3]);
}

static bool ehf_skip_tex_blob(const std::vector<uint8_t>& ehf,
                              size_t limit,
                              size_t& pos)
{
    if (pos + 0x60 > limit) return false;
    if (ehf_be32(ehf, pos) != 0xFFFFFFFEu) return false;

    const uint32_t pf = ehf_be32(ehf, pos + 0x18);
    const uint32_t mt = ehf_be32(ehf, pos + 0x20);
    if (mt < 0x54 || mt > 0x200) return false;

    const size_t table = pos + mt;
    if (table + 8 > limit) return false;
    size_t next;
    if (pf == 98u) {
        const uint32_t tw = ehf_be32(ehf, pos + 0x10);
        const uint32_t th = ehf_be32(ehf, pos + 0x14);
        next = pos + mt + size_t(tw) * size_t(th) * 2u;
    } else {
        const uint32_t comp_size = ehf_be32(ehf, table + 4);
        next = table + 8 + size_t(comp_size);
    }
    if (next > limit) return false;
    pos = next;
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: parsed " << out.size()
              << " tiles; placed_exact=" << (placed_exact ? "true" : "false");
        TerrainMeshTrace(trace.str());
    }
    return true;
}

static bool parse_ehf_render_tiles(const std::vector<uint8_t>& ehf,
                                   uint32_t terrain_cells_w,
                                   std::vector<EhfRenderTileDesc>& out)
{
    out.clear();
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: entered; ehf_bytes=" << ehf.size()
              << " terrain_cells_w=" << terrain_cells_w;
        TerrainMeshTrace(trace.str());
    }
    if (ehf.size() < 63) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: EHF shorter than 63 bytes");
        return false;
    }
    const uint32_t body_off  = ehf_be32(ehf, 55);
    const uint32_t body_size = ehf_be32(ehf, 59);
    const size_t body_end = size_t(body_off) + size_t(body_size);
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: header body_off=" << body_off
              << " body_size=" << body_size
              << " body_end=" << body_end;
        TerrainMeshTrace(trace.str());
    }
    if (body_end > ehf.size()) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: body_end exceeds EHF size");
        return false;
    }

    size_t pos = body_off;
    if (!ehf_skip_tex_blob(ehf, body_end, pos)) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: first ehf_skip_tex_blob");
        return false;
    }
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: first texture blob skipped; pos=" << pos;
        TerrainMeshTrace(trace.str());
    }
    if (!ehf_skip_tex_blob(ehf, body_end, pos)) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: second ehf_skip_tex_blob");
        return false;
    }
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: second texture blob skipped; pos=" << pos;
        TerrainMeshTrace(trace.str());
    }
    if (pos + 8 > body_end) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: fewer than 8 bytes remain before tile count");
        return false;
    }

    pos += 4;
    const uint32_t count = ehf_be32(ehf, pos);
    pos += 4;
    {
        std::ostringstream trace;
        trace << "parse_ehf_render_tiles: tile count=" << count
              << " count_field_end=" << pos
              << " body_end=" << body_end;
        TerrainMeshTrace(trace.str());
    }
    if (count == 0 || count > 4096) {
        TerrainMeshTrace("parse_ehf_render_tiles: failed: tile count outside 1..4096");
        return false;
    }

    auto ehf_bef32 = [&](size_t off) -> float {
        const uint32_t u = ehf_be32(ehf, off);
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    };

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 16 > body_end) {
            std::ostringstream trace;
            trace << "parse_ehf_render_tiles: failed: tile " << i
                  << " descriptor exceeds body_end; pos=" << pos
                  << " body_end=" << body_end;
            TerrainMeshTrace(trace.str());
            return false;
        }
        EhfRenderTileDesc t;
        t.cell_w = ehf_be32(ehf, pos + 0);
        t.cell_h = ehf_be32(ehf, pos + 4);
        t.sub_w  = ehf_be32(ehf, pos + 8);
        t.sub_h  = ehf_be32(ehf, pos + 12);
        pos += 16;
        if (t.cell_w == 0 || t.cell_h == 0 ||
            t.sub_w == 0 || t.sub_h == 0 ||
            t.sub_w > 1024 || t.sub_h > 1024)
        {
            std::ostringstream trace;
            trace << "parse_ehf_render_tiles: failed: tile " << i
                  << " invalid dimensions cell=" << t.cell_w << "x" << t.cell_h
                  << " sub=" << t.sub_w << "x" << t.sub_h;
            TerrainMeshTrace(trace.str());
            return false;
        }
        const size_t grid_bytes =
            size_t(t.sub_w) * size_t(t.sub_h) * 160u + 24u;
        if (pos + grid_bytes > body_end) {
            std::ostringstream trace;
            trace << "parse_ehf_render_tiles: failed: tile " << i
                  << " grid exceeds body_end; grid_bytes=" << grid_bytes
                  << " pos=" << pos << " body_end=" << body_end;
            TerrainMeshTrace(trace.str());
            return false;
        }
        {
            const size_t bb = pos + grid_bytes - 24u;
            t.min_x = ehf_bef32(bb + 0);
            t.min_y = ehf_bef32(bb + 4);
            t.max_x = ehf_bef32(bb + 12);
            t.max_y = ehf_bef32(bb + 16);
            t.bbox_ok = std::isfinite(t.min_x) && std::isfinite(t.min_y) &&
                        std::isfinite(t.max_x) && std::isfinite(t.max_y) &&
                        t.max_x > t.min_x && t.max_y > t.min_y &&
                        std::fabs(t.min_x) < 100000.0f &&
                        std::fabs(t.min_y) < 100000.0f;
        }
        pos += grid_bytes;
        out.push_back(t);
    }

    bool placed_exact = false;
    {
        size_t bbox_count = 0;
        std::vector<float> sx, sy;
        float gmin_x = std::numeric_limits<float>::infinity();
        float gmin_y = std::numeric_limits<float>::infinity();
        for (const EhfRenderTileDesc& t : out) {
            if (!t.bbox_ok) continue;
            ++bbox_count;
            sx.push_back(float(t.cell_w) / (t.max_x - t.min_x));
            sy.push_back(float(t.cell_h) / (t.max_y - t.min_y));
            gmin_x = std::min(gmin_x, t.min_x);
            gmin_y = std::min(gmin_y, t.min_y);
        }
        if (bbox_count == out.size() && !sx.empty()) {
            auto median = [](std::vector<float>& v) {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            const float scale_x = median(sx);
            const float scale_y = median(sy);
            if (std::isfinite(scale_x) && std::isfinite(scale_y) &&
                scale_x > 0.01f && scale_y > 0.01f)
            {
                placed_exact = true;
                for (EhfRenderTileDesc& t : out) {
                    const float fx = (t.min_x - gmin_x) * scale_x;
                    const float fy = (t.min_y - gmin_y) * scale_y;
                    const long cx = std::lround(fx);
                    const long cy = std::lround(fy);
                    if (cx < 0 || cy < 0 || cx > 65535 || cy > 65535) {
                        placed_exact = false;
                        break;
                    }
                    t.cell_x = uint32_t(cx);
                    t.cell_y = uint32_t(cy);
                }
            }
        }
    }

    if (!placed_exact) {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t row_h = 0;
        for (EhfRenderTileDesc& t : out) {
            if (terrain_cells_w > 0 &&
                x > 0 &&
                x + t.cell_w > terrain_cells_w)
            {
                y += row_h;
                x = 0;
                row_h = 0;
            }
            t.cell_x = x;
            t.cell_y = y;
            x += t.cell_w;
            row_h = std::max(row_h, t.cell_h);
            if (terrain_cells_w > 0 && x >= terrain_cells_w) {
                y += row_h;
                x = 0;
                row_h = 0;
            }
        }
    }
    return true;
}
