#include "F3EngineLevelParser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace Level::F3 {
namespace {

constexpr char kMagic[] = "LevelGraphicsFile";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr size_t kMaxStringBytes = 1u << 20;
constexpr uint32_t kMaxInstances = 1000000;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

    size_t position() const { return pos_; }
    size_t size() const { return bytes_.size(); }
    bool seek(size_t pos) {
        if (pos > bytes_.size()) return false;
        pos_ = pos;
        return true;
    }
    bool u8(uint8_t& out) {
        if (!available(1)) return false;
        out = bytes_[pos_++];
        return true;
    }
    bool u32le(uint32_t& out) {
        if (!available(4)) return false;
        out = uint32_t(bytes_[pos_]) |
              (uint32_t(bytes_[pos_ + 1]) << 8) |
              (uint32_t(bytes_[pos_ + 2]) << 16) |
              (uint32_t(bytes_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }
    bool u32be(uint32_t& out) {
        if (!available(4)) return false;
        out = (uint32_t(bytes_[pos_]) << 24) |
              (uint32_t(bytes_[pos_ + 1]) << 16) |
              (uint32_t(bytes_[pos_ + 2]) << 8) |
              uint32_t(bytes_[pos_ + 3]);
        pos_ += 4;
        return true;
    }
    bool u64le(uint64_t& out) {
        if (!available(8)) return false;
        out = 0;
        for (unsigned i = 0; i < 8; ++i)
            out |= uint64_t(bytes_[pos_ + i]) << (8 * i);
        pos_ += 8;
        return true;
    }
    bool f32le(float& out) {
        uint32_t bits = 0;
        if (!u32le(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    bool cstr(std::string& out) {
        const size_t start = pos_;
        const size_t limit = start + std::min(kMaxStringBytes, bytes_.size() - start);
        size_t end = start;
        while (end < limit && bytes_[end] != 0) ++end;
        if (end == limit) return false;
        out.assign(reinterpret_cast<const char*>(bytes_.data() + start), end - start);
        pos_ = end + 1;
        return true;
    }
    bool skip(size_t count) {
        if (!available(count)) return false;
        pos_ += count;
        return true;
    }
    std::vector<uint8_t> slice(size_t begin, size_t end) const {
        if (begin > end || end > bytes_.size()) return {};
        return {bytes_.begin() + begin, bytes_.begin() + end};
    }

private:
    bool available(size_t count) const {
        return pos_ <= bytes_.size() && count <= bytes_.size() - pos_;
    }
    const std::vector<uint8_t>& bytes_;
    size_t pos_ = 0;
};

bool IsPlausiblePath(const std::string& path) {
    if (path.empty()) return true;
    if (path.size() > kMaxStringBytes) return false;
    return path.find('\0') == std::string::npos;
}

} // namespace

EngineLevelParseResult ParseEngineLevelV13(
    const std::vector<uint8_t>& bytes) {
    EngineLevelParseResult out;
    Reader r(bytes);

    if (bytes.size() < kMagicSize + 8 ||
        !std::equal(kMagic, kMagic + kMagicSize, bytes.begin())) {
        out.error = "Not a LevelGraphicsFile or truncated header.";
        return out;
    }
    r.seek(kMagicSize);
    if (!r.u32le(out.version) || !r.u32le(out.declared_record_count)) {
        out.error = "Truncated Fable III engine-level header.";
        return out;
    }
    if (out.version != 13) {
        out.error = "Fable III parser expects version 13; file declares version " +
                    std::to_string(out.version) + ".";
        return out;
    }
    if (out.declared_record_count > bytes.size() / 4) {
        out.error = "Declared record count cannot fit in this file.";
        return out;
    }

    for (uint32_t i = 0; i < out.declared_record_count; ++i) {
        EngineLevelRecord rec;
        rec.offset = r.position();
        if (!r.u32le(rec.type)) {
            out.error = "Truncated record type at offset " +
                        std::to_string(rec.offset) + ".";
            return out;
        }

        if (rec.type == 4 || rec.type == 5 || rec.type == 32) {
            if (!r.cstr(rec.path) || !IsPlausiblePath(rec.path)) {
                out.error = "Missing or invalid path in resource record at offset " +
                            std::to_string(rec.offset) + ".";
                return out;
            }
            rec.paths.push_back(rec.path);
            if (rec.type == 4) {
                if (!r.u64le(rec.resource_key)) {
                    out.error = "Truncated resource key at offset " +
                                std::to_string(rec.offset) + ".";
                    return out;
                }
                rec.has_resource_key = true;
            }
        } else if (rec.type == 2) {
            // Version 13 stores three NUL-terminated model paths, then a
            // big-endian instance count. The fourth apparent string in the
            // older F2 parser is the first byte(s) of this count.
            for (unsigned p = 0; p < 3; ++p) {
                std::string path;
                if (!r.cstr(path) || !IsPlausiblePath(path)) {
                    out.error = "Invalid type-2 model path at offset " +
                                std::to_string(rec.offset) + ".";
                    return out;
                }
                rec.paths.push_back(std::move(path));
            }
            if (!r.u32be(rec.instance_count) ||
                rec.instance_count > kMaxInstances) {
                out.error = "Invalid type-2 instance count at offset " +
                            std::to_string(rec.offset) + ".";
                return out;
            }
            constexpr size_t kInstanceSize = 92;
            if (rec.instance_count > (r.size() - r.position()) / kInstanceSize) {
                out.error = "Type-2 instance array exceeds file bounds at offset " +
                            std::to_string(rec.offset) + ".";
                return out;
            }
            rec.instances.reserve(rec.instance_count);
            for (uint32_t n = 0; n < rec.instance_count; ++n) {
                EngineLevelInstance instance;
                instance.offset = r.position();
                instance.raw = r.slice(r.position(), r.position() + kInstanceSize);
                for (uint8_t& flag : instance.flags) {
                    if (!r.u8(flag)) {
                        out.error = "Truncated type-2 instance flags.";
                        return out;
                    }
                }
                uint8_t fourthFlag = 0;
                if (!r.u8(fourthFlag)) {
                    out.error = "Truncated type-2 instance header.";
                    return out;
                }
                instance.flags[3] = fourthFlag;
                if (!r.u64le(instance.hash)) {
                    out.error = "Truncated type-2 instance hash.";
                    return out;
                }
                instance.values.resize(20);
                for (float& value : instance.values) {
                    if (!r.f32le(value)) {
                        out.error = "Truncated type-2 instance transform.";
                        return out;
                    }
                }
                rec.instances.push_back(std::move(instance));
            }
        } else if (rec.type == 21) {
            // Type 21 is the F3 scatter/foliage record. The bytes immediately
            // after the model path are 10 metadata bytes, a BE count, a
            // 68-byte fixed header, 22-byte entries, a BE second count,
            // then 32-byte secondary entries.
            if (!r.cstr(rec.path) || !IsPlausiblePath(rec.path)) {
                out.error = "Invalid type-21 model path at offset " +
                            std::to_string(rec.offset) + ".";
                return out;
            }
            rec.paths.push_back(rec.path);
            if (r.position() > r.size() || r.size() - r.position() < 10) {
                out.error = "Truncated type-21 metadata.";
                return out;
            }
            const size_t metadataOffset = r.position();
            rec.metadata_hash = 0;
            for (unsigned n = 0; n < 8; ++n)
                rec.metadata_hash |= uint64_t(bytes[metadataOffset + n]) << (8 * n);
            rec.metadata_flags[0] = bytes[metadataOffset + 8];
            rec.metadata_flags[1] = bytes[metadataOffset + 9];
            if (!r.skip(10) || !r.u32be(rec.instance_count)) {
                out.error = "Truncated type-21 primary count.";
                return out;
            }
            if (rec.instance_count > kMaxInstances) {
                out.error = "Invalid type-21 primary count.";
                return out;
            }
            constexpr size_t kFixedHeaderSize = 68;
            if (r.position() > r.size() || r.size() - r.position() < kFixedHeaderSize) {
                out.error = "Truncated type-21 fixed header.";
                return out;
            }
            rec.fixed_header = r.slice(r.position(), r.position() + kFixedHeaderSize);
            if (!r.skip(kFixedHeaderSize)) {
                out.error = "Truncated type-21 fixed header.";
                return out;
            }
            constexpr size_t kPrimarySize = 22;
            if (rec.instance_count > (r.size() - r.position()) / kPrimarySize) {
                out.error = "Type-21 primary array exceeds file bounds.";
                return out;
            }
            rec.instances.reserve(rec.instance_count);
            for (uint32_t n = 0; n < rec.instance_count; ++n) {
                EngineLevelInstance instance;
                instance.offset = r.position();
                instance.raw = r.slice(r.position(), r.position() + kPrimarySize);
                // The primary record is preserved byte-for-byte. Its three
                // float position and five half-float fields are decoded in a
                // later refinement; no data is thrown away here.
                if (!r.skip(kPrimarySize)) {
                    out.error = "Truncated type-21 primary instance.";
                    return out;
                }
                rec.instances.push_back(std::move(instance));
            }
            if (!r.u32be(rec.secondary_instance_count) ||
                rec.secondary_instance_count > kMaxInstances) {
                out.error = "Invalid type-21 secondary count.";
                return out;
            }
            constexpr size_t kSecondarySize = 32;
            if (rec.secondary_instance_count >
                (r.size() - r.position()) / kSecondarySize) {
                out.error = "Type-21 secondary array exceeds file bounds.";
                return out;
            }
            rec.secondary_instances.reserve(rec.secondary_instance_count);
            for (uint32_t n = 0; n < rec.secondary_instance_count; ++n) {
                EngineLevelInstance instance;
                instance.offset = r.position();
                instance.raw = r.slice(r.position(), r.position() + kSecondarySize);
                for (unsigned f = 0; f < 8; ++f) {
                    float value = 0.0f;
                    if (!r.f32le(value)) {
                        out.error = "Truncated type-21 secondary instance.";
                        return out;
                    }
                    instance.values.push_back(value);
                }
                rec.secondary_instances.push_back(std::move(instance));
            }
        } else {
            rec.size = bytes.size() - rec.offset;
            rec.raw = r.slice(rec.offset, bytes.size());
            out.records.push_back(std::move(rec));
            std::ostringstream msg;
            msg << "Unsupported version-13 record type "
                << out.records.back().type << " at offset 0x" << std::hex
                << out.records.back().offset
                << "; preserved the undecoded tail.";
            out.error = msg.str();
            return out;
        }

        rec.size = r.position() - rec.offset;
        rec.raw = r.slice(rec.offset, r.position());
        out.records.push_back(std::move(rec));
    }

    if (r.position() != bytes.size()) {
        out.error = "Parsed " + std::to_string(out.declared_record_count) +
                    " records but " + std::to_string(bytes.size() - r.position()) +
                    " trailing bytes remain unclassified.";
        return out;
    }
    out.ok = true;
    out.complete = true;
    return out;
}

} // namespace Level::F3
