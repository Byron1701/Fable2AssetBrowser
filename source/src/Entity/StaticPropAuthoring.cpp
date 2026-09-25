#include "StaticPropAuthoring.h"

#include "../GDB/GdbEdit.h"
#include "../GDB/F3Gdb.h"
#include "../Utilities/GameBackup.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace StaticPropAuthoring {
namespace {

constexpr uint32_t kNull = 0x811C9DC5u;
constexpr uint32_t kParent = 0x5F6317D5u;
constexpr uint32_t kStaticEntityBase = 0x586E4A9Fu;
constexpr uint32_t kStaticMeshComponent = 0x29CF50D1u;
constexpr uint32_t kStaticMeshBase = 0x43A7E822u;
constexpr uint32_t kModelFile = 0x0C17DB4Eu;
constexpr uint32_t kStaticTransformComponent = 0xF73572C4u;
constexpr uint32_t kStaticTransformBase = 0x991EF747u;
constexpr uint32_t kPositionTemplate = 0xC53BF803u;
constexpr uint32_t kRotationTemplate = 0x3607DC7Cu;

uint32_t fnv1(const std::string& value) {
    uint32_t hash = 0x811C9DC5u;
    for (unsigned char c : value) {
        hash *= 0x01000193u;
        hash ^= uint32_t(c);
    }
    return hash;
}

std::string normalize_model_path(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

std::filesystem::path globals_path(const std::string& root_dir) {
    namespace fs = std::filesystem;
    const fs::path root(root_dir);
    // State::root_dir points at the game's data directory. Accept a
    // game root as well so this helper remains safe for older callers.
    const bool is_data_dir =
        root.filename().string() == "data" ||
        root.filename().string() == "Data";
    return (is_data_dir ? root : root / "data") / "Globals" /
           "globals.gdb";
}

bool read_file(const std::filesystem::path& path,
               std::vector<uint8_t>& bytes,
               std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not read " + path.string();
        return false;
    }
    const std::streamoff length = input.tellg();
    if (length <= 0) {
        error = "The GDB is empty: " + path.string();
        return false;
    }
    input.seekg(0);
    bytes.resize(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) {
        error = "Short read of " + path.string();
        return false;
    }
    return true;
}

bool write_file_atomically(const std::filesystem::path& path,
                           const std::vector<uint8_t>& bytes,
                           std::string& error) {
    const std::filesystem::path temporary =
        path.string() + ".entity_save_tmp";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    {
        std::ofstream output(temporary,
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not create " + temporary.string();
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            error = "Could not write " + temporary.string();
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, ec);
        error = "Could not replace " + path.string() +
                " (Windows error " + std::to_string(code) +
                "). The original file was left untouched.";
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "Could not replace " + path.string() + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

GdbEdit::Field field(uint32_t hash, uint8_t type, uint32_t value) {
    GdbEdit::Field result;
    result.hash = hash;
    result.type = type;
    result.value = value;
    return result;
}

bool local_reference(const GdbEdit::GdbFile& gdb,
                     uint32_t record,
                     uint32_t field_hash,
                     uint32_t expected) {
    GdbEdit::Field value;
    return gdb.FindLocalField(record, field_hash, value) &&
           value.type == 6 && value.value == expected;
}

bool catalog_from_gdb(const F3Gdb::File& gdb,
                      std::vector<CatalogEntry>& entries) {
    entries.clear();

    auto find_record = [&gdb](uint32_t hash) -> const F3Gdb::Record* {
        return gdb.record_by_hash(hash);
    };

    auto local_reference = [&](uint32_t record_hash,
                               uint32_t field_hash,
                               uint32_t expected) -> bool {
        const auto* record = find_record(record_hash);
        if (!record || !record->row_type) return false;
        for (size_t i = 0; i < record->values.size(); ++i) {
            const auto* f = record->field(i);
            if (f && f->column_hash == field_hash &&
                f->data_type == 0x0600 &&
                record->values[i] == expected) {
                return true;
            }
        }
        return false;
    };

    for (const auto& record : gdb.records()) {
        const auto* name = gdb.name_for_record(record.hash);
        if (!name || name->text.empty()) continue;

        if (!local_reference(record.hash, kParent, kStaticEntityBase) ||
            !local_reference(record.hash, kStaticTransformComponent,
                             kStaticTransformBase)) {
            continue;
        }

        const F3Gdb::Record* graphics = nullptr;
        for (size_t i = 0; i < record.values.size(); ++i) {
            const auto* f = record.field(i);
            if (f && f->column_hash == kStaticMeshComponent &&
                f->data_type == 0x0600) {
                graphics = find_record(record.values[i]);
                break;
            }
        }
        if (!graphics) continue;

        if (!local_reference(graphics->hash, kParent, kStaticMeshBase))
            continue;

        uint32_t model_hash = 0;
        bool found_model = false;
        for (size_t i = 0; i < graphics->values.size(); ++i) {
            const auto* f = graphics->field(i);
            if (f && f->column_hash == kModelFile &&
                (f->data_type == 0x0400 || f->data_type == 0x0700)) {
                model_hash = graphics->values[i];
                found_model = model_hash != 0 && model_hash != kNull;
                break;
            }
        }
        if (!found_model) continue;

        CatalogEntry entry;
        entry.internal_name = name->text;
        entry.entity_hash = record.hash;
        entry.model_path_hash = model_hash;
        if (const auto* model = gdb.string_by_hash(model_hash)) {
            entry.model_path = model->text;
        }
        entry.transform_component_field = kStaticTransformComponent;
        entry.transform_component_template = kStaticTransformBase;
        entry.position_template = kPositionTemplate;
        entry.rotation_template = kRotationTemplate;
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(),
              [](const CatalogEntry& left, const CatalogEntry& right) {
                  return left.internal_name < right.internal_name;
              });
    return true;
}

}

bool IsValidInternalName(const std::string& value) {
    if (value.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(value.front())) &&
        value.front() != '_') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

bool Save(const std::string& root_dir,
          const Definition& definition,
          CatalogEntry& saved,
          std::string& result,
          std::string& error) {
    saved = CatalogEntry{};
    result.clear();
    error.clear();
    if (!GameBackup::RequireBackup(error)) return false;
    if (!IsValidInternalName(definition.internal_name)) {
        error = "Entity ID must start with a letter or underscore and "
                "contain only letters, numbers, and underscores.";
        return false;
    }
    const std::string model_path =
        normalize_model_path(definition.model_path);
    if (model_path.empty()) {
        error = "Select a model for the static prop.";
        return false;
    }

    const std::filesystem::path globals = globals_path(root_dir);
    std::vector<uint8_t> original;
    if (!read_file(globals, original, error)) {
        return false;
    }

    GdbEdit::GdbFile gdb;
    if (!gdb.Parse(original, error)) {
        error = "Could not parse globals.gdb: " + error;
        return false;
    }
    if (gdb.ResolveNamedRecord(definition.internal_name) != 0) {
        error = "An entity definition already uses that ID.";
        return false;
    }

    const uint32_t model_hash = fnv1(model_path);
    const uint32_t graphics_record = gdb.AllocRecordHash();
    if (!gdb.AddRecord(
            graphics_record,
            {field(kModelFile, 4, model_hash),
             field(kParent, 6, kStaticMeshBase)},
            0)) {
        error = "Could not create the static mesh component.";
        return false;
    }

    const uint32_t entity_record = gdb.AllocRecordHash();
    if (!gdb.AddRecord(
            entity_record,
            {field(kStaticMeshComponent, 6, graphics_record),
             field(kParent, 6, kStaticEntityBase),
             field(kStaticTransformComponent, 6, kStaticTransformBase)},
            0)) {
        error = "Could not create the static prop entity.";
        return false;
    }
    gdb.AddNameMapping(definition.internal_name, entity_record);
    gdb.AddDictString(fnv1(definition.internal_name),
                      definition.internal_name);
    gdb.AddDictString(model_hash, model_path);

    if (!write_file_atomically(globals, gdb.Serialize(), error)) {
        return false;
    }

    saved.internal_name = definition.internal_name;
    saved.model_path = model_path;
    saved.entity_hash = entity_record;
    saved.model_path_hash = model_hash;
    saved.transform_component_field = kStaticTransformComponent;
    saved.transform_component_template = kStaticTransformBase;
    saved.position_template = kPositionTemplate;
    saved.rotation_template = kRotationTemplate;

    std::ostringstream message;
    message << "Saved static prop " << definition.internal_name
            << " to globals.gdb. It has a model and transform only: no AI, "
               "targeting, action-use, sale-sign, readable, inventory, or "
               "physics behaviour.";
    result = message.str();
    return true;
}

bool LoadCatalog(const std::string& root_dir,
                 std::vector<CatalogEntry>& entries,
                 std::string& error) {
    entries.clear();
    error.clear();
    std::vector<uint8_t> bytes;
    if (!read_file(globals_path(root_dir), bytes, error)) return false;
    F3Gdb::File gdb;
    if (!gdb.Parse(bytes, error)) {
        error = "Could not parse globals.gdb: " + error;
        return false;
    }
    return catalog_from_gdb(gdb, entries);
}

}
