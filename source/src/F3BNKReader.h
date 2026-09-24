#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class F3BNKReader {
public:
    struct FileEntry {
        std::string name;
        std::uint32_t name_hash = 0;
        std::uint32_t offset = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t uncompressed_size = 0;
        std::vector<std::uint32_t> decompressed_chunk_sizes;
        bool compressed = false;

        std::uint32_t size() const { return uncompressed_size; }
    };

    explicit F3BNKReader(const std::string& bnk_path);
    F3BNKReader(const std::vector<std::uint8_t>& index_bytes,
                const std::vector<std::uint8_t>& content_bytes);

    const std::vector<FileEntry>& list_files() const { return _files; }

    std::vector<std::uint8_t> extract_index_bytes(int index) const;
    std::vector<std::uint8_t> extract_file_bytes(const std::string& name) const;

    void extract_file(const std::string& name, const std::string& out_path) const;
    void extract_all(const std::filesystem::path& out_dir) const;

    static bool IsF3BNK(const std::string& bnk_path);
    static bool IsF3BNK(const std::vector<std::uint8_t>& index_bytes);

private:
    std::vector<std::uint8_t> _index;
    std::vector<std::uint8_t> _content;
    std::filesystem::path _content_path;
    std::uint64_t _content_size = 0;
    std::vector<FileEntry> _files;
    bool _content_compressed = false;

    void parse();
    void parse_index();
    std::vector<std::uint8_t> extract_entry(const FileEntry& e) const;

    static std::uint32_t read_be32(const std::uint8_t* p);
    static std::int32_t read_be_i32(const std::uint8_t* p);
    static std::uint32_t fnv1a_path(const std::string& s);

    static std::vector<std::uint8_t> inflate_zlib_stream(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t expected_size);

    static std::vector<std::uint8_t> decompress_index(
        const std::vector<std::uint8_t>& index_bytes,
        bool& content_compressed);
};
