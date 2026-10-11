#include "F3EngineLevelParser.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace Level::F3 {
namespace {

constexpr char kMagic[] = "LevelGraphicsFile";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr size_t kMaxStringBytes = 1u << 20;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

    size_t position() const { return pos_; }
    size_t size() const { return data_.size(); }
    bool seek(size_t p) {
        if (p > data_.size()) return false;
        pos_ = p;
        return true;
    }
    bool u32le(uint32_t& out) {
        if (!available(4)) return false;
        out = uint32_t(data_[pos_]) |
              (uint32_t(data_[pos_ + 1]) << 8) |
              (uint32_t(data_[pos_ + 2]) << 16) |
              (uint32_t(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }
    bool u64le(uint64_t& out) {
        if (!available(8)) return false;
        out = 0;
        for (unsigned i = 0; i < 8; ++i)
            out |= uint64_t(data_[pos_ + i]) << (i * 8);
        pos_ += 8;
        return true;
    }
    bool cstr(std::string& out) {
        const size_t start = pos_;
        const size_t limit = std::min(data_.size(), start + kMaxStringBytes);
        size_t end = start;
        while (end < limit && data_[end] != 0) ++end;
        if (end == limit) return false;
        out.assign(reinterpret_cast<const char*>(data_.data() + start),
                   end - start);
        pos_ = end + 1;
        return true;
    }
    bool skip(size_t n) {
        if (!available(n)) return false;
        pos_ += n;
        return true;
    }
    std::vector<uint8_t> slice(size_t begin, size_t end) const {
        if (begin > end || end > data_.size()) return {};
        return std::vector<uint8_t>(data_.begin() + begin, data_.begin() + end);
    }

private:
    bool available(size_t n) const {
        return pos_ <= data_.size() && n <= data_.size() - pos_;
    }
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
};

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
    if (!r.seek(kMagicSize) ||
        !r.u32le(out.version) ||
        !r.u32le(out.declared_record_count)) {
        out.error = "Truncated Fable III engine-level header.";
        return out;
    }
    if (out.version != 13) {
        std::ostringstream msg;
        msg << "Fable III parser expects version 13; file declares version "
            << out.version << ".";
        out.error = msg.str();
        return out;
    }

    // The version-13 header is verified against the 15 .engine_level samples
    // in Library/Fable 3 Asset Browser/fable3.zip. Do not reuse the Fable II
    // version-11/12 record layouts: version 13 changes the record structure.
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

        // These simple resource records are shared by the observed v13
        // samples. Type 4 carries an additional eight-byte resource key.
        if (rec.type == 4 || rec.type == 5 || rec.type == 32) {
            if (!r.cstr(rec.path)) {
                out.error = "Missing or unterminated path in record at offset " +
                            std::to_string(rec.offset) + ".";
                return out;
            }
            if (rec.type == 4) {
                if (!r.u64le(rec.resource_key)) {
                    out.error = "Truncated resource key in record at offset " +
                                std::to_string(rec.offset) + ".";
                    return out;
                }
                rec.has_resource_key = true;
            }
            rec.size = r.position() - rec.offset;
            rec.raw = r.slice(rec.offset, r.position());
            out.records.push_back(std::move(rec));
            continue;
        }

        // Never guess a record size or scan ahead for a byte pattern that
        // merely resembles a record type. Keep the undecoded tail for later
        // reverse engineering and return an explicit incomplete result.
        rec.size = bytes.size() - rec.offset;
        rec.raw = r.slice(rec.offset, bytes.size());
        out.records.push_back(std::move(rec));
        std::ostringstream msg;
        msg << "Version-13 record type " << out.records.back().type
            << " at offset 0x" << std::hex << out.records.back().offset
            << " has no verified layout yet; preserved " << std::dec
            << out.records.back().raw.size() << " bytes from this point.";
        out.error = msg.str();
        return out;
    }

    if (r.position() != bytes.size()) {
        out.error = "Parsed declared records but " +
                    std::to_string(bytes.size() - r.position()) +
                    " trailing bytes remain unclassified.";
        return out;
    }

    out.ok = true;
    out.complete = true;
    return out;
}

} // namespace Level::F3
