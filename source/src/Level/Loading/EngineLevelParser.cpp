#include "Level/Core/LevelLoader.h"
#include "Level/Loading/LevelBinaryReader.h"
#include "UI/OutputLog.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Level {

bool ParseF3EngineLevel(const std::vector<uint8_t>& bytes,
                        EngineLevelInfo& out)
{
    out = {};
    constexpr char kMagic[] = "LevelGraphicsFile";
    constexpr size_t kMagicLen = sizeof(kMagic) - 1;

    if (bytes.size() < kMagicLen + 8 ||
        std::memcmp(bytes.data(), kMagic, kMagicLen) != 0) {
        out.error = "F3 LevelGraphicsFile magic mismatch";
        return false;
    }

    LeReader r{bytes.data(), bytes.size(), kMagicLen};
    if (!r.u32(out.version) || !r.u32(out.entry_count)) {
        out.error = "truncated F3 LevelGraphicsFile header";
        return false;
    }
    if (out.version != 13) {
        out.error = "unsupported F3 LevelGraphicsFile version " +
                    std::to_string(out.version);
        return false;
    }
    if (out.entry_count > (1u << 20)) {
        out.error = "F3 entry count looks corrupt";
        return false;
    }

    // F3 v13 type-2 records are observed directly in the supplied
    // defaultscenario.engine_level:
    //   four C-strings
    //   two reserved bytes (00 00)
    //   repeated 92-byte records:
    //     uint32 flags
    //     uint64 hash
    //     20 little-endian float32 values
    //
    // There is no 32-bit instance-count field here.  The end of a type-2
    // block is the next top-level LevelGraphicsFile entry.  We therefore
    // locate that boundary only at 92-byte record boundaries.
    auto read_entry_signature = [&](size_t pos) -> bool {
        if (pos + 4 > bytes.size()) return false;
        const uint32_t type =
            uint32_t(bytes[pos]) |
            (uint32_t(bytes[pos + 1]) << 8) |
            (uint32_t(bytes[pos + 2]) << 16) |
            (uint32_t(bytes[pos + 3]) << 24);

        auto has_cstr = [&](size_t at, std::string* value = nullptr) -> bool {
            if (at >= bytes.size()) return false;
            const size_t limit = std::min(bytes.size(), at + size_t(4096));
            size_t q = at;
            while (q < limit) {
                if (bytes[q] == 0) {
                    if (value) {
                        value->assign(reinterpret_cast<const char*>(bytes.data() + at),
                                      q - at);
                    }
                    return true;
                }
                ++q;
            }
            return false;
        };

        size_t q = pos + 4;
        std::string a;
        switch (type) {
        case 4:
        case 5:
        case 32:
            if (!has_cstr(q, &a)) return false;
            return a.rfind("worlds\\", 0) == 0;
        case 2: {
            for (int i = 0; i < 4; ++i) {
                if (!has_cstr(q, &a)) return false;
                q = bytes.data() + q == nullptr ? q : q;
                while (q < bytes.size() && bytes[q] != 0) ++q;
                if (q >= bytes.size()) return false;
                ++q;
            }
            std::string first;
            size_t p = pos + 4;
            if (!has_cstr(p, &first)) return false;
            if (first.size() < 4 ||
                first.substr(first.size() - 4) != ".mdl") return false;
            for (int i = 0; i < 4; ++i) {
                while (p < bytes.size() && bytes[p] != 0) ++p;
                if (p >= bytes.size()) return false;
                ++p;
            }
            return p + 2 <= bytes.size() &&
                   bytes[p] == 0 && bytes[p + 1] == 0;
        }
        case 21: {
            if (!has_cstr(q, &a)) return false;
            return a.find("\\") != std::string::npos &&
                   a.find(".mdl") != std::string::npos;
        }
        default:
            return false;
        }
    };

    auto find_next_entry = [&](size_t from) -> size_t {
        for (size_t p = from; p + 4 <= bytes.size(); ++p) {
            if (read_entry_signature(p)) return p;
        }
        return bytes.size();
    };

    out.entries.reserve(out.entry_count);
    for (uint32_t i = 0; i < out.entry_count; ++i) {
        EngineLevelEntry e;
        e.offset = r.i;

        if (!r.u32(e.type)) {
            out.error = "truncated F3 entry " + std::to_string(i);
            return false;
        }

        switch (e.type) {
        case 2: {
            PropBlock b;
            b.offset = e.offset;
            b.type = e.type;

            if (!r.cstr(b.model_path) ||
                !r.cstr(b.shadow_model_path) ||
                !r.cstr(b.lod_model_path) ||
                !r.cstr(b.extra_model_path)) {
                out.error = "truncated F3 type-2 model paths";
                return false;
            }

            e.str_a = b.model_path;
            e.str_b = b.lod_model_path;

            if (!r.skip(2)) {
                out.error = "truncated F3 type-2 reserved bytes";
                return false;
            }

            const size_t records_begin = r.i;
            size_t next_entry = records_begin;
            while (next_entry + 4 <= bytes.size()) {
                if (next_entry != records_begin &&
                    read_entry_signature(next_entry)) {
                    break;
                }
                if (next_entry + 92 > bytes.size()) {
                    next_entry = bytes.size();
                    break;
                }
                next_entry += 92;
            }

            if (next_entry > bytes.size() ||
                next_entry < records_begin ||
                (next_entry - records_begin) % 92 != 0) {
                out.error = "invalid F3 type-2 record block boundary";
                return false;
            }

            const size_t record_count = (next_entry - records_begin) / 92;
            b.instances.reserve(record_count);

            for (size_t j = 0; j < record_count; ++j) {
                PropInstance p;
                p.record_file_offset = static_cast<uint32_t>(r.i);
                p.record_size = 92;
                p.pos_file_offset = static_cast<uint32_t>(r.i + 12);
                p.lev_rec_kind = 1;

                uint32_t flags = 0;
                if (!r.u32(flags) || !r.u64(p.hash)) {
                    out.error = "truncated F3 type-2 record header";
                    return false;
                }
                p.flags[0] = static_cast<uint8_t>(flags & 0xff);
                p.flags[1] = static_cast<uint8_t>((flags >> 8) & 0xff);
                p.flags[2] = static_cast<uint8_t>((flags >> 16) & 0xff);

                for (float& v : p.values) {
                    if (!r.f32(v)) {
                        out.error = "truncated F3 type-2 record";
                        return false;
                    }
                }

                b.instances.push_back(p);
            }

            if (r.i != next_entry) {
                out.error = "F3 type-2 parser lost record alignment";
                return false;
            }

            out.prop_blocks.push_back(std::move(b));
            e.size = r.i - e.offset;
            out.entries.push_back(std::move(e));
            --i;
            continue;
        }

        case 21: {
            // F3 v13 type-21 entries are present in the same supplied file.
            // Their exact sub-record variants are not required for locating
            // the following top-level entry, so keep the two resource strings
            // and advance to the next verified top-level signature.
            if (!r.cstr(e.str_a) || !r.cstr(e.str_b)) {
                out.error = "truncated F3 type-21 strings";
                return false;
            }

            const size_t next = find_next_entry(r.i);
            if (next == bytes.size()) {
                r.i = bytes.size();
            } else {
                r.i = next;
            }
            break;
        }

        case 4:
        case 5:
        case 32:
            if (!r.cstr(e.str_a)) {
                out.error = "truncated F3 string entry";
                return false;
            }
            if (e.type == 4) {
                if (!r.u64(e.resource_key)) {
                    out.error = "truncated F3 type-4 resource key";
                    return false;
                }
                e.has_resource_key = true;
            }
            break;

        default:
            out.error = "unsupported F3 LevelGraphicsFile entry type " +
                        std::to_string(e.type) + " at 0x" +
                        [&] {
                            std::ostringstream s;
                            s << std::hex << e.offset;
                            return s.str();
                        }();
            return false;
        }

        e.size = r.i - e.offset;
        out.entries.push_back(std::move(e));
    }

    out.ok = true;
    return true;
}
}
