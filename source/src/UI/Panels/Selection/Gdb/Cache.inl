bool open_gdb_viewer_for_bnk_entry(const std::string& bnk_path,
                                   int file_index,
                                   const std::string& file_name)
{
    if (bnk_path.empty() || file_index < 0) return false;

    std::vector<uint8_t> gdb_bytes;
    try {
        gdb_bytes = BnkCache::extract_bytes(bnk_path, file_index);
    } catch (const std::exception& ex) {
        OutputLog::error("GDB viewer: failed to extract " + file_name +
                         " (" + ex.what() + ")");
        return false;
    } catch (...) {
        OutputLog::error("GDB viewer: failed to extract " + file_name);
        return false;
    }
    if (gdb_bytes.empty()) {
        OutputLog::warn("GDB viewer: empty GDB " + file_name);
        return false;
    }

    // Fable III PC GDBs use the separate little-endian format.  Do not
    // send them through the legacy Fable II GdbView (which expects GDB\\0
    // and big-endian fields); that path otherwise produces an apparently
    // valid viewer with zero rows.
    const bool is_f3_gdb = gdb_bytes.size() >= 4 &&
                           gdb_bytes[0] == 0 && gdb_bytes[1] == 0 &&
                           gdb_bytes[2] == 0 && gdb_bytes[3] == 0;
    if (is_f3_gdb) {
        F3Gdb::File f3_file;
        std::string f3_error;
        if (!f3_file.Parse(gdb_bytes, f3_error)) {
            OutputLog::error("F3 GDB viewer: failed to parse " + file_name +
                             " (" + (f3_error.empty() ? "invalid GDB" : f3_error) + ")");
            return false;
        }

        S.gdb_view_rows.clear();
        S.gdb_view_rows.reserve(f3_file.records().size());
        size_t named = 0;
        for (const F3Gdb::Record& rec : f3_file.records()) {
            GdbViewerRow row;
            row.record_index = rec.index;
            row.hash = rec.hash;
            row.indexed_record = true;
            if (rec.row_type) {
                row.f3_fields.reserve(rec.row_type->fields.size());
                for (size_t fi = 0; fi < rec.row_type->fields.size(); ++fi) {
                    const F3Gdb::Field& field = rec.row_type->fields[fi];
                    GdbViewerField vf;
                    vf.column_hash = field.column_hash;
                    vf.data_id = field.data_id;
                    vf.data_type = field.data_type;
                    if (fi < rec.values.size()) vf.raw_value = rec.values[fi];
                    if (const F3Gdb::StringEntry* column = f3_file.string_by_hash(field.column_hash)) {
                        vf.column_name = column->text;
                    }
                    const uint32_t raw = vf.raw_value;
                    if (field.data_type == static_cast<uint16_t>(F3Gdb::DataType::StringHash) ||
                        field.data_type == static_cast<uint16_t>(F3Gdb::DataType::ObjectHash) ||
                        field.data_type == static_cast<uint16_t>(F3Gdb::DataType::ObjectLink)) {
                        if (const F3Gdb::StringEntry* resolved = f3_file.string_by_hash(raw)) {
                            vf.resolved_value = resolved->text;
                        } else if (const F3Gdb::Record* target = f3_file.record_by_hash(raw)) {
                            if (const F3Gdb::StringEntry* target_name = f3_file.name_for_record(target->hash)) {
                                vf.resolved_value = target_name->text;
                            }
                        }
                    }
                    row.f3_fields.push_back(std::move(vf));
                }
            }
            const F3Gdb::StringEntry* name = f3_file.name_for_record(rec.hash);
            if (name) {
                row.name = name->text;
                row.hash_name = name->text;
                ++named;
            }
            S.gdb_view_rows.push_back(std::move(row));
        }

        std::ostringstream title;
        title << std::filesystem::path(file_name).filename().string()
              << "  rows=" << S.gdb_view_rows.size()
              << "  row-names=" << named
              << "  F3 records=" << f3_file.header().record_count
              << "  row-types=" << f3_file.row_types().size()
              << "  strings=" << f3_file.strings().size();
        S.gdb_view_title = title.str();
        S.gdb_view_filter.clear();
        S.show_gdb_render = true;
        S.show_lua_render = false;

        OutputLog::success("F3 GDB viewer opened: " + file_name + " (" +
                           std::to_string(S.gdb_view_rows.size()) + " rows)");
        return true;
    }

    std::vector<uint8_t> save_bytes;
    std::vector<std::pair<uint32_t, std::string>> save_hash_to_name;
    if (find_save_sibling_bytes(bnk_path, file_name, save_bytes)) {
        save_hash_to_name = parse_save_hash_to_name(save_bytes);
    }

    std::vector<Gdb::RecordRow> records =
        Gdb::Build010RecordRows(gdb_bytes, save_hash_to_name);

    std::unordered_map<uint32_t, std::string> save_name_by_hash;
    save_name_by_hash.reserve(save_hash_to_name.size() * 2);
    for (const auto& kv : save_hash_to_name) {
        save_name_by_hash.emplace(kv.first, kv.second);
    }

    std::unordered_map<uint32_t, std::string> model_name_by_hash;
    model_name_by_hash.reserve(S.all_mdl_files.size() * 2);
    for (const FlatAssetEntry& e : S.all_mdl_files) {
        if (e.full_path.empty()) continue;
        model_name_by_hash.emplace(gdb_fnv1_model_path_hash(e.full_path),
                                   e.full_path);
    }

    S.gdb_view_rows.clear();
    S.gdb_view_rows.reserve(records.size());
    size_t named = 0;
    size_t parent_named = 0;
    size_t model_named = 0;
    size_t skeleton_refs = 0;
    for (const Gdb::RecordRow& rec : records) {
        GdbViewerRow row;
        row.record_index = rec.index;
        row.name = rec.name;
        auto hash_it = save_name_by_hash.find(rec.hash);
        if (hash_it != save_name_by_hash.end()) {
            row.hash_name = hash_it->second;
            if (row.name.empty()) row.name = row.hash_name;
        }
        if (!row.name.empty() || !row.hash_name.empty()) ++named;
        if (rec.parent_hash != 0) {
            auto parent_it = save_name_by_hash.find(rec.parent_hash);
            if (parent_it != save_name_by_hash.end()) {
                row.parent_name = parent_it->second;
                ++parent_named;
            }
        }
        row.hash = rec.hash;
        row.parent_hash = rec.parent_hash;
        row.model_path_hash = rec.model_path_hash;
        row.skeleton_file_hash = rec.skeleton_file_hash;
        row.retarget_skeleton_file_hash = rec.retarget_skeleton_file_hash;
        row.skeleton_file_name = rec.skeleton_file_name;
        row.retarget_skeleton_file_name = rec.retarget_skeleton_file_name;
        if (row.skeleton_file_hash != 0 ||
            row.retarget_skeleton_file_hash != 0) {
            ++skeleton_refs;
        }
        row.model_path_hashes = rec.model_path_hashes;
        for (uint32_t model_hash : row.model_path_hashes) {
            auto model_it = model_name_by_hash.find(model_hash);
            if (model_it != model_name_by_hash.end()) {
                if (row.model_path_name.empty()) {
                    row.model_path_name = model_it->second;
                }
                row.model_path_names.push_back(model_it->second);
                ++model_named;
            } else {
                row.model_path_names.emplace_back();
            }
        }
        row.indexed_record = true;
        S.gdb_view_rows.push_back(std::move(row));
    }

    std::ostringstream title;
    title << std::filesystem::path(file_name).filename().string()
          << "  rows=" << S.gdb_view_rows.size()
          << "  row-names=" << named
          << "  parent-names=" << parent_named
          << "  model-names=" << model_named
          << "  skeleton-refs=" << skeleton_refs
          << "  save-map=" << save_hash_to_name.size();
    S.gdb_view_title = title.str();
    S.gdb_view_filter.clear();
    S.show_gdb_render = true;
    S.show_lua_render = false;

    OutputLog::success("GDB viewer opened: " + file_name + " (" +
                       std::to_string(S.gdb_view_rows.size()) + " rows)");
    if (save_hash_to_name.empty()) {
        OutputLog::warn("GDB viewer: no .save sibling names resolved for " +
                        file_name);
    }
    return true;
}
