#include "F3BNKReader.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <cstring>
#include <zlib.h>

namespace {
constexpr std::size_t kContentChunkSize = 0x8000;
constexpr std::uint32_t kExpectedIndexVersion = 4;

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("F3 BNK: failed to open " + p.string());
    const auto end = f.tellg();
    if (end < 0) throw std::runtime_error("F3 BNK: failed to size " + p.string());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    f.seekg(0, std::ios::beg);
    if (!data.empty())
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f && !data.empty()) throw std::runtime_error("F3 BNK: failed to read " + p.string());
    return data;
}

void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& data) {
    if (!p.parent_path().empty())
        std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("F3 BNK: failed to create " + p.string());
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) throw std::runtime_error("F3 BNK: failed to write " + p.string());
}
}

std::uint32_t F3BNKReader::read_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) |
           (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}

std::int32_t F3BNKReader::read_be_i32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(read_be32(p));
}

std::uint32_t F3BNKReader::fnv1a_path(const std::string& s) {
    std::uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) {
        h = (h * 0x01000193u) ^ c;
    }
    return h;
}

bool F3BNKReader::IsF3BNK(const std::vector<std::uint8_t>& b) {
    if (b.size() < 9) return false;
    const std::uint32_t total = read_be32(b.data());
    const std::uint32_t version = read_be32(b.data() + 4);
    const std::uint8_t compressed = b[8];
    return total == b.size() &&
           version == kExpectedIndexVersion &&
           (compressed == 0 || compressed == 1);
}

bool F3BNKReader::IsF3BNK(const std::string& path) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::uint8_t h[9]{};
        f.read(reinterpret_cast<char*>(h), sizeof(h));
        if (f.gcount() != static_cast<std::streamsize>(sizeof(h))) return false;
        const std::uint32_t total = read_be32(h);
        const std::uint32_t version = read_be32(h + 4);
        return total == std::filesystem::file_size(path) &&
               version == kExpectedIndexVersion &&
               (h[8] == 0 || h[8] == 1);
    } catch (...) {
        return false;
    }
}

F3BNKReader::F3BNKReader(const std::string& bnk_path) {
    _index = read_file(bnk_path);
    const std::filesystem::path p(bnk_path);
    std::filesystem::path dat = p;
    dat += ".dat";
    if (!std::filesystem::exists(dat))
        throw std::runtime_error("F3 BNK: missing content file " + dat.string());
    _content = read_file(dat);
    parse();
}

F3BNKReader::F3BNKReader(const std::vector<std::uint8_t>& index_bytes,
                         const std::vector<std::uint8_t>& content_bytes)
    : _index(index_bytes), _content(content_bytes) {
    parse();
}

void F3BNKReader::parse() {
    if (!IsF3BNK(_index))
        throw std::runtime_error("F3 BNK: invalid index header");
    parse_index();
}

std::vector<std::uint8_t> F3BNKReader::inflate_zlib_stream(
    const std::uint8_t* data, std::size_t size, std::size_t expected_size) {

    if (size > std::numeric_limits<uInt>::max())
        throw std::runtime_error("F3 BNK: compressed stream is too large for zlib");

    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        throw std::runtime_error("F3 BNK: inflateInit failed");

    std::vector<std::uint8_t> out;
    out.reserve(expected_size);

    z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    z.avail_in = static_cast<uInt>(size);

    std::uint8_t buffer[65536];
    for (;;) {
        z.next_out = buffer;
        z.avail_out = sizeof(buffer);
        const int ret = inflate(&z, Z_NO_FLUSH);
        const std::size_t produced = sizeof(buffer) - z.avail_out;
        out.insert(out.end(), buffer, buffer + produced);

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&z);
            throw std::runtime_error("F3 BNK: zlib decompression failed");
        }
        if (ret == Z_BUF_ERROR && z.avail_in == 0) break;
        if (z.avail_in == 0 && produced == 0) break;
    }

    inflateEnd(&z);

    if (out.size() != expected_size)
        throw std::runtime_error(
            "F3 BNK: decompressed size mismatch (expected " +
            std::to_string(expected_size) + ", got " +
            std::to_string(out.size()) + ")");

    return out;
}

std::vector<std::uint8_t> F3BNKReader::decompress_index(
    const std::vector<std::uint8_t>& b,
    bool& content_compressed) {

    if (b.size() < 9)
        throw std::runtime_error("F3 BNK: index is too small");

    const std::uint32_t total = read_be32(b.data());
    const std::uint32_t version = read_be32(b.data() + 4);
    content_compressed = b[8] != 0;

    if (total != b.size())
        throw std::runtime_error("F3 BNK: index size field does not match file size");
    if (version != kExpectedIndexVersion)
        throw std::runtime_error("F3 BNK: unsupported index version");

    std::size_t pos = 9;
    std::vector<std::uint8_t> compressed;
    std::size_t expected_total = 0;

    while (pos < b.size()) {
        if (b.size() - pos < 8)
            throw std::runtime_error("F3 BNK: truncated index chunk header");

        const std::uint32_t compressed_size = read_be32(b.data() + pos);
        const std::uint32_t decompressed_size = read_be32(b.data() + pos + 4);
        pos += 8;

        if (compressed_size == 0)
            throw std::runtime_error("F3 BNK: zero-sized index chunk");

        if (compressed_size > b.size() - pos)
            throw std::runtime_error("F3 BNK: index chunk extends beyond file");

        compressed.insert(compressed.end(), b.begin() + static_cast<std::ptrdiff_t>(pos),
                          b.begin() + static_cast<std::ptrdiff_t>(pos + compressed_size));
        expected_total += decompressed_size;
        pos += compressed_size;
    }

    if (compressed.empty())
        throw std::runtime_error("F3 BNK: no compressed index chunks");

    return inflate_zlib_stream(compressed.data(), compressed.size(), expected_total);
}

void F3BNKReader::parse_index() {
    bool content_compressed = false;
    const auto raw = decompress_index(_index, content_compressed);
    _content_compressed = content_compressed;

    if (raw.size() < 8)
        throw std::runtime_error("F3 BNK: decompressed index is too small");

    const std::uint32_t unknown = read_be32(raw.data());
    (void)unknown;
    const std::uint32_t count = read_be32(raw.data() + 4);

    std::size_t pos = 8;
    struct Pending {
        FileEntry e;
    };
    std::vector<Pending> pending;
    pending.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (content_compressed) {
            if (raw.size() - pos < 20)
                throw std::runtime_error("F3 BNK: truncated file entry");

            FileEntry e;
            e.name_hash = read_be32(raw.data() + pos); pos += 4;
            e.offset = read_be32(raw.data() + pos); pos += 4;
            e.uncompressed_size = read_be32(raw.data() + pos); pos += 4;
            e.compressed_size = read_be32(raw.data() + pos); pos += 4;
            const std::uint32_t chunk_count = read_be32(raw.data() + pos); pos += 4;

            if (chunk_count == 0 || chunk_count > 100000)
                throw std::runtime_error("F3 BNK: invalid chunk count");

            if (raw.size() - pos < std::size_t(chunk_count) * 4)
                throw std::runtime_error("F3 BNK: truncated chunk-size table");

            e.decompressed_chunk_sizes.resize(chunk_count);
            for (std::uint32_t j = 0; j < chunk_count; ++j) {
                e.decompressed_chunk_sizes[j] = read_be32(raw.data() + pos);
                pos += 4;
            }
            e.compressed = true;
            pending.push_back({std::move(e)});
        } else {
            if (raw.size() - pos < 12)
                throw std::runtime_error("F3 BNK: truncated uncompressed file entry");

            FileEntry e;
            e.name_hash = read_be32(raw.data() + pos); pos += 4;
            e.offset = read_be32(raw.data() + pos); pos += 4;
            e.uncompressed_size = read_be32(raw.data() + pos); pos += 4;
            e.compressed_size = 0;
            e.compressed = false;
            pending.push_back({std::move(e)});
        }
    }

    for (auto& p : pending) {
        if (raw.size() - pos < 4)
            throw std::runtime_error("F3 BNK: truncated file-name length");
        const std::uint32_t length_with_nul = read_be32(raw.data() + pos);
        pos += 4;

        if (length_with_nul == 0 || length_with_nul > 1'000'000)
            throw std::runtime_error("F3 BNK: invalid file-name length");

        const std::size_t name_bytes = static_cast<std::size_t>(length_with_nul - 1);
        // The length includes the terminating NUL. The NUL is part of the
        // record and is followed by exactly 28 bytes of metadata.
        if (raw.size() - pos < static_cast<std::size_t>(length_with_nul) + 28)
            throw std::runtime_error("F3 BNK: truncated file-name record");

        p.e.name.assign(reinterpret_cast<const char*>(raw.data() + pos), name_bytes);
        pos += static_cast<std::size_t>(length_with_nul);
        pos += 28;

        if (fnv1a_path(p.e.name) != p.e.name_hash)
            throw std::runtime_error("F3 BNK: filename hash mismatch for " + p.e.name);

        _files.push_back(std::move(p.e));
    }

    if (pos != raw.size())
        throw std::runtime_error("F3 BNK: unexpected bytes at end of decompressed index");
}

std::vector<std::uint8_t> F3BNKReader::extract_entry(const FileEntry& e) const {
    if (e.offset > _content.size() ||
        static_cast<std::uint64_t>(e.offset) + e.compressed_size > _content.size())
        throw std::runtime_error("F3 BNK: content entry extends beyond .dat");

    if (!e.compressed) {
        if (static_cast<std::uint64_t>(e.offset) + e.uncompressed_size > _content.size())
            throw std::runtime_error("F3 BNK: uncompressed entry extends beyond .dat");
        return std::vector<std::uint8_t>(
            _content.begin() + e.offset,
            _content.begin() + e.offset + e.uncompressed_size);
    }

    if (e.decompressed_chunk_sizes.empty())
        throw std::runtime_error("F3 BNK: compressed entry has no chunks");

    const std::size_t total_expected =
        std::accumulate(e.decompressed_chunk_sizes.begin(),
                        e.decompressed_chunk_sizes.end(), std::size_t(0));

    if (total_expected != e.uncompressed_size)
        throw std::runtime_error("F3 BNK: chunk sizes do not sum to uncompressed size");

    const std::vector<std::uint8_t> compressed(
        _content.begin() + e.offset,
        _content.begin() + e.offset + e.compressed_size);

    std::vector<std::uint8_t> out;
    out.reserve(e.uncompressed_size);

    std::size_t comp_pos = 0;
    for (std::size_t i = 0; i < e.decompressed_chunk_sizes.size(); ++i) {
        const std::size_t comp_size =
            (i + 1 < e.decompressed_chunk_sizes.size())
                ? std::min(kContentChunkSize, compressed.size() - comp_pos)
                : compressed.size() - comp_pos;

        if (comp_pos >= compressed.size() || comp_size == 0)
            throw std::runtime_error("F3 BNK: invalid compressed chunk bounds");

        z_stream z{};
        if (inflateInit(&z) != Z_OK)
            throw std::runtime_error("F3 BNK: inflateInit failed");

        z.next_in = const_cast<Bytef*>(
            reinterpret_cast<const Bytef*>(compressed.data() + comp_pos));
        z.avail_in = static_cast<uInt>(std::min<std::size_t>(
            comp_size, std::numeric_limits<uInt>::max()));

        std::vector<std::uint8_t> chunk(e.decompressed_chunk_sizes[i]);
        z.next_out = chunk.data();
        z.avail_out = static_cast<uInt>(chunk.size());

        const int ret = inflate(&z, Z_SYNC_FLUSH);
        const std::size_t produced = chunk.size() - z.avail_out;
        inflateEnd(&z);

        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
            throw std::runtime_error("F3 BNK: content decompression failed for " + e.name);
        if (produced != chunk.size())
            throw std::runtime_error("F3 BNK: content chunk size mismatch for " + e.name);

        out.insert(out.end(), chunk.begin(), chunk.end());
        comp_pos += comp_size;
    }

    return out;
}

std::vector<std::uint8_t> F3BNKReader::extract_index_bytes(int index) const {
    if (index < 0 || index >= static_cast<int>(_files.size()))
        throw std::runtime_error("F3 BNK: file index out of range");
    return extract_entry(_files[static_cast<std::size_t>(index)]);
}

std::vector<std::uint8_t> F3BNKReader::extract_file_bytes(const std::string& name) const {
    for (const auto& e : _files)
        if (e.name == name)
            return extract_entry(e);
    throw std::runtime_error("F3 BNK: file not found: " + name);
}

void F3BNKReader::extract_file(const std::string& name, const std::string& out_path) const {
    write_file(out_path, extract_file_bytes(name));
}

void F3BNKReader::extract_all(const std::filesystem::path& out_dir) const {
    std::filesystem::create_directories(out_dir);
    for (const auto& e : _files) {
        std::filesystem::path target = out_dir / e.name;
        std::filesystem::create_directories(target.parent_path());
        write_file(target, extract_entry(e));
    }
}
