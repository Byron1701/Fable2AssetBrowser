#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Gdb::detail {

inline constexpr size_t kHeaderSize = 0x18;

inline uint32_t ReadBeU32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) |
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

inline float ReadBeF32(const uint8_t* p)
{
    uint32_t v = ReadBeU32(p);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

inline uint32_t ReadLeU32(const uint8_t* p)
{
    return uint32_t(p[0]) |
           (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline bool DetectLittleEndian(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < kHeaderSize) return false;

    const uint32_t be_count = ReadBeU32(bytes.data() + 0x04);
    const uint32_t be_a = ReadBeU32(bytes.data() + 0x08);
    const uint32_t be_b = ReadBeU32(bytes.data() + 0x0C);

    const uint32_t le_count = ReadLeU32(bytes.data() + 0x04);
    const uint32_t le_a = ReadLeU32(bytes.data() + 0x08);
    const uint32_t le_b = ReadLeU32(bytes.data() + 0x0C);

    auto plausible = [&](uint32_t count, uint32_t a, uint32_t b) {
        if (count == 0 || count > 10000000u) return false;
        const size_t schema = kHeaderSize + size_t(a);
        const size_t hashes = schema + size_t(b);
        if (schema > bytes.size() || hashes > bytes.size()) return false;
        const size_t end = hashes + size_t(count) * 4 +
                           size_t(count) * 2;
        return end <= bytes.size();
    };

    const bool be_ok = plausible(be_count, be_a, be_b);
    const bool le_ok = plausible(le_count, le_a, le_b);

    return le_ok && !be_ok;
}

inline uint32_t ReadGdbU32(const std::vector<uint8_t>& bytes,
                           const uint8_t* p)
{
    return DetectLittleEndian(bytes) ? ReadLeU32(p) : ReadBeU32(p);
}

inline float ReadGdbF32(const std::vector<uint8_t>& bytes,
                        const uint8_t* p)
{
    uint32_t v = ReadGdbU32(bytes, p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

inline constexpr uint32_t kHashParent = 0x5F6317D5;
inline constexpr uint32_t kHashVecZ = 0x050C5D45;
inline constexpr uint32_t kHashVecY = 0x050C5D46;
inline constexpr uint32_t kHashVecX = 0x050C5D47;
struct GdbView {
    const std::vector<uint8_t>& bytes;
    uint32_t count = 0;
    uint32_t size_a = 0;
    uint32_t size_b = 0;
    size_t body_start = kHeaderSize;
    size_t body_end = 0;
    size_t schema_base = 0;
    size_t hash_base = 0;
    size_t offset_base = 0;
    bool ok = false;
    bool little_endian = false;

    std::vector<size_t> record_data_offsets;

    explicit GdbView(const std::vector<uint8_t>& b) : bytes(b)
    {
        if (bytes.size() < kHeaderSize) return;
        if (bytes[0] != 'G' || bytes[1] != 'D' || bytes[2] != 'B' ||
            bytes[3] != 0) {
            return;
        }
        little_endian = DetectLittleEndian(bytes);
        auto rd = [&](const uint8_t* p) {
            return little_endian ? ReadLeU32(p) : ReadBeU32(p);
        };
        count = rd(bytes.data() + 0x04);
        size_a = rd(bytes.data() + 0x08);
        size_b = rd(bytes.data() + 0x0C);
        if (count == 0) return;
        schema_base = kHeaderSize + size_t(size_a);
        hash_base = schema_base + size_t(size_b);
        offset_base = hash_base + size_t(count) * 4;
        body_end = schema_base;
        if (body_end > bytes.size() ||
            offset_base + size_t(count) * 2 > bytes.size()) {
            return;
        }
        if (!buildRecordDataOffsets()) return;
        ok = true;
    }

    bool buildRecordDataOffsets()
    {
        record_data_offsets.clear();
        record_data_offsets.reserve(count);
        size_t cur = body_start;
        for (uint32_t i = 0; i < count; ++i) {
            if (cur + 4 > body_end) return false;
            record_data_offsets.push_back(cur);

            size_t schema_off = 0;
            uint32_t field_count = 0;
            if (!schemaAtRecordData(cur, schema_off, field_count)) {
                return false;
            }
            const size_t entry_size = 4 + size_t(field_count) * 4;
            if (entry_size < 4 || cur + entry_size > body_end) {
                return false;
            }
            cur += entry_size;
        }
        return true;
    }

    bool lookup(uint32_t hash, size_t& record) const
    {
        if (!ok) return false;
        size_t lo = 0;
        size_t hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint32_t v = (little_endian ? ReadLeU32(bytes.data() + hash_base + mid * 4) : ReadBeU32(bytes.data() + hash_base + mid * 4));
            if (v < hash) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= count) return false;
        uint32_t found = (little_endian ? ReadLeU32(bytes.data() + hash_base + lo * 4) : ReadBeU32(bytes.data() + hash_base + lo * 4));
        if (found != hash) return false;
        if (lo >= record_data_offsets.size()) return false;
        record = record_data_offsets[lo];
        return record + 4 <= body_end;
    }

    bool schemaAtRecordData(size_t record,
                            size_t& schema_off,
                            uint32_t& field_count) const
    {
        if (record + 4 > body_end) return false;
        uint32_t rel = (little_endian ? ReadLeU32(bytes.data() + record) : ReadBeU32(bytes.data() + record));
        schema_off = schema_base + size_t(rel);
        if (schema_off + 4 > hash_base) return false;
        uint32_t header = (little_endian ? ReadLeU32(bytes.data() + schema_off) : ReadBeU32(bytes.data() + schema_off));
        field_count = header >> 8;
        if (field_count > 256) {
            const uint8_t* p = bytes.data() + schema_off;
            const uint32_t count1_le =
                uint32_t(p[0]) | (uint32_t(p[1]) << 8);
            const uint32_t count2 = uint32_t(p[2]);
            field_count = count1_le + count2;
            if (field_count > 1024) return false;
        }
        return schema_off + 4 + size_t(field_count) * 8 <= hash_base;
    }

    bool schema(size_t record, size_t& schema_off, uint32_t& field_count) const
    {
        if (!ok) return false;
        return schemaAtRecordData(record, schema_off, field_count);
    }

    bool findLocal(size_t record,
                   uint32_t field_hash,
                   uint8_t expected_type,
                   size_t& slot,
                   uint8_t* found_type = nullptr) const
    {
        size_t sch = 0;
        uint32_t n = 0;
        if (!schema(record, sch, n)) return false;
        const size_t hashes = sch + 4;
        const size_t descs = hashes + size_t(n) * 4;
        for (uint32_t i = 0; i < n; ++i) {
            if ((little_endian ? ReadLeU32(bytes.data() + hashes + size_t(i) * 4) : ReadBeU32(bytes.data() + hashes + size_t(i) * 4)) != field_hash) {
                continue;
            }
            const uint32_t desc =
                (little_endian ? ReadLeU32(bytes.data() + descs + size_t(i) * 4) : ReadBeU32(bytes.data() + descs + size_t(i) * 4));
            const uint8_t type = uint8_t(desc >> 24);
            if (expected_type != 0xFF && type != expected_type) return false;
            slot = record + 4 + size_t(i) * 4;
            if (slot + 4 > body_end) return false;
            if (found_type) *found_type = type;
            return true;
        }
        return false;
    }

    bool findField(size_t record,
                   uint32_t field_hash,
                   uint8_t expected_type,
                   size_t& slot,
                   uint8_t* found_type = nullptr) const
    {
        size_t owner = 0;
        return findFieldOwner(record, field_hash, expected_type, slot, owner,
                              found_type);
    }

    bool findFieldOwner(size_t record,
                        uint32_t field_hash,
                        uint8_t expected_type,
                        size_t& slot,
                        size_t& owner,
                        uint8_t* found_type = nullptr) const
    {
        size_t cur = record;
        for (int depth = 0; depth < 64; ++depth) {
            if (findLocal(cur, field_hash, expected_type, slot, found_type)) {
                owner = cur;
                return true;
            }
            size_t parent_slot = 0;
            if (!findLocal(cur, kHashParent, 6, parent_slot, nullptr)) {
                return false;
            }
            uint32_t parent_hash = (little_endian ? ReadLeU32(bytes.data() + parent_slot) : ReadBeU32(bytes.data() + parent_slot));
            if (parent_hash == 0) return false;
            size_t parent_rec = 0;
            if (!lookup(parent_hash, parent_rec)) return false;
            if (parent_rec == cur) return false;
            cur = parent_rec;
        }
        return false;
    }

    bool readLocalFloat(size_t record, uint32_t field_hash, float& value,
                        size_t* out_slot = nullptr) const
    {
        size_t slot = 0;
        if (!findLocal(record, field_hash, 3, slot, nullptr)) return false;
        if (slot + 4 > body_end) return false;
        value = (little_endian ? [&]{ uint32_t v=ReadLeU32(bytes.data()+slot); float f; std::memcpy(&f,&v,4); return f; }() : ReadBeF32(bytes.data() + slot));
        if (!std::isfinite(value)) return false;
        if (out_slot) *out_slot = slot;
        return true;
    }

    bool readVectorFields(size_t record,
                          float& vx,
                          float& vy,
                          float& vz,
                          size_t* out_slots = nullptr) const
    {
        return readLocalFloat(record, kHashVecX, vx,
                              out_slots ? &out_slots[0] : nullptr) &&
               readLocalFloat(record, kHashVecY, vy,
                              out_slots ? &out_slots[1] : nullptr) &&
               readLocalFloat(record, kHashVecZ, vz,
                              out_slots ? &out_slots[2] : nullptr);
    }

    bool hasVectorSchema(size_t record) const
    {
        float v = 0.0f;
        return readLocalFloat(record, kHashVecZ, v) &&
               readLocalFloat(record, kHashVecY, v) &&
               readLocalFloat(record, kHashVecX, v);
    }

    bool readVec3Record(size_t record,
                        float& x,
                        float& y,
                        float& z,
                        float* raw_x = nullptr,
                        float* raw_y = nullptr,
                        float* raw_z = nullptr,
                        size_t* out_slots = nullptr) const
    {
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        if (!readVectorFields(record, vx, vy, vz, out_slots)) return false;
        if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
            return false;
        }
        x = vx;
        y = vy;
        z = vz;
        if (raw_x) *raw_x = vx;
        if (raw_y) *raw_y = vy;
        if (raw_z) *raw_z = vz;
        return true;
    }

    bool readVec3Ref(uint32_t hash,
                     float& x,
                     float& y,
                     float& z,
                     float* raw_x = nullptr,
                     float* raw_y = nullptr,
                     float* raw_z = nullptr,
                     size_t* out_slots = nullptr) const
    {
        size_t rec = 0;
        return lookup(hash, rec) &&
               readVec3Record(rec, x, y, z, raw_x, raw_y, raw_z, out_slots);
    }

    bool readRotationVec3Ref(uint32_t hash,
                             float& x,
                             float& y,
                             float& z,
                             size_t* out_slots = nullptr) const
    {
        size_t rec = 0;
        return lookup(hash, rec) &&
               readVec3Record(rec, x, y, z, nullptr, nullptr, nullptr,
                              out_slots);
    }

    bool payloadRange(size_t record,
                      size_t& payload_start,
                      size_t& payload_end) const
    {
        size_t sch = 0;
        uint32_t n = 0;
        if (!schema(record, sch, n)) return false;
        payload_start = record + 4 + size_t(n) * 4;
        if (payload_start > body_end) return false;

        payload_end = body_end;
        if (ok) {
            auto it = std::upper_bound(record_data_offsets.begin(),
                                       record_data_offsets.end(), record);
            if (it != record_data_offsets.end()) {
                payload_end = *it;
            }
        }
        payload_end = std::min(payload_end, payload_start + size_t(0x200));
        return payload_start + 4 <= payload_end;
    }
};

}
