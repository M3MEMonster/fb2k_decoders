#include "stdafx.h"
#include "resource.h"
#include "guids.h"
#include "subsong_db.h"
#include <SDK/autoplaylist.h>

typedef CListControlFb2kColors<CListControlSimple> FileListControl_t;
typedef CListControlFb2kColors<CListControlOwnerDataCells> SubsongListControl_t;

namespace {

struct file_entry {
    pfc::string8 path;
    pfc::string8 display_name;
    uint32_t total_subsongs = 0;
};

struct subsong_entry {
    uint32_t subsong_id = 0;
    pfc::string8 name;
    bool checked = true;
};

static std::unordered_set<void*> g_dsm_proxy_set;
static bool dsm_is_proxy_already_installed(autoplaylist_client::ptr client);

class subsong_autoplaylist_client_proxy : public autoplaylist_client_v2 {
public:
    explicit subsong_autoplaylist_client_proxy(autoplaylist_client::ptr inner) : m_inner(inner) {
        m_inner_v2 ^= inner;
        g_dsm_proxy_set.insert(static_cast<autoplaylist_client*>(this));
    }

    ~subsong_autoplaylist_client_proxy() {
        g_dsm_proxy_set.erase(static_cast<autoplaylist_client*>(this));
    }

    GUID get_guid() override { return m_inner->get_guid(); }

    void filter(metadb_handle_list_cref data, bool* out) override {
        m_inner->filter(data, out);

        auto& db = subsong_database::get();
        std::unordered_map<std::string, std::set<uint32_t>> cache;
        uint32_t removed = 0;

        const size_t n = data.get_count();
        for (size_t i = 0; i < n; ++i) {
            if (!out[i]) continue;
            auto h = data[i];
            const char* path = h->get_path();
            auto it = cache.find(path);
            if (it == cache.end()) {
                it = cache.emplace(path, db.get_excluded(path)).first;
            }
            if (!it->second.empty() && it->second.count(h->get_subsong_index()) > 0) {
                out[i] = false;
                ++removed;
            }
        }

        if (removed > 0 || m_filterLogCounter < 3) {
            FB2K_console_formatter() << "[DSM] autoplaylist proxy filter called: items="
                                     << (uint32_t)n << ", removed=" << removed;
            ++m_filterLogCounter;
        }
    }

    bool sort(metadb_handle_list_cref p_items, t_size* p_orderbuffer) override {
        return m_inner->sort(p_items, p_orderbuffer);
    }

    void get_configuration(stream_writer* p_stream, abort_callback& p_abort) override {
        m_inner->get_configuration(p_stream, p_abort);
    }

    void show_ui(t_size p_source_playlist) override {
        auto apm = autoplaylist_manager::get();
        autoplaylist_manager_v2::ptr apm_v2;
        apm->service_query_t(apm_v2);

        t_uint32 flags = 0;
        if (apm_v2.is_valid()) {
            try { flags = apm_v2->get_client_flags(p_source_playlist); } catch (...) {}
        }

        apm->remove_client(p_source_playlist);
        apm->add_client(m_inner, p_source_playlist, flags);

        m_inner->show_ui(p_source_playlist);
        // Do not re-wrap immediately here: autoplaylist configuration UI may keep
        // using the same client instance after show_ui() returns.
    }

    void set_full_refresh_notify(completion_notify::ptr notify) override {
        if (m_inner_v2.is_valid()) m_inner_v2->set_full_refresh_notify(notify);
    }

    bool show_ui_available() override {
        return m_inner_v2.is_valid() ? m_inner_v2->show_ui_available() : true;
    }

    void get_display_name(pfc::string_base& out) override {
        if (m_inner_v2.is_valid()) {
            m_inner_v2->get_display_name(out);
        } else {
            out = "Autoplaylist";
        }
    }

private:
    autoplaylist_client::ptr m_inner;
    autoplaylist_client_v2::ptr m_inner_v2;
    uint32_t m_filterLogCounter = 0;
};

static bool dsm_is_proxy_already_installed(autoplaylist_client::ptr client) {
    return g_dsm_proxy_set.count(client.get_ptr()) > 0;
}

// @param force_refresh  When true (e.g. after Apply), remove+add even for
//                       already-proxied playlists to force content rebuild.
//                       When false (startup), skip playlists that already
//                       have the proxy installed to avoid the remove/add gap.
static void dsm_install_and_refresh_autoplaylists(bool force_refresh = false) {
    autoplaylist_manager::ptr apm;
    try {
        apm = autoplaylist_manager::get();
    } catch (...) {
        FB2K_console_formatter() << "[DSM] autoplaylist manager unavailable";
        return;
    }

    autoplaylist_manager_v2::ptr apm_v2;
    apm->service_query_t(apm_v2);

    auto plm = playlist_manager::get();
    const size_t total = plm->get_playlist_count();
    for (size_t p = 0; p < total; ++p) {
        bool is_auto = false;
        try {
            is_auto = apm->is_client_present(p);
        } catch (...) {
            continue;
        }
        if (!is_auto) continue;

        try {
            autoplaylist_client::ptr client = apm->query_client(p);
            bool already_proxied = dsm_is_proxy_already_installed(client);

            // On startup, skip playlists that already have our proxy — avoids
            // the brief unfiltered window between remove_client / add_client.
            if (already_proxied && !force_refresh) continue;

            autoplaylist_client::ptr wrapped = client;
            if (!already_proxied) {
                wrapped = new service_impl_t<subsong_autoplaylist_client_proxy>(client);
            }

            t_uint32 flags = 0;
            if (apm_v2.is_valid()) flags = apm_v2->get_client_flags(p);

            apm->remove_client(p);
            apm->add_client(wrapped, p, flags);
            FB2K_console_formatter() << "[DSM] autoplaylist proxy active, index=" << (uint32_t)p;
        } catch (...) {
            FB2K_console_formatter() << "[DSM] autoplaylist refresh failed, index=" << (uint32_t)p;
        }
    }
}

static void dsm_reinstall_autoplaylist_proxy_on_startup() {
    dsm_install_and_refresh_autoplaylists();
}

static std::set<uint32_t> parse_range_input(const char* text, uint32_t max_id) {
    std::set<uint32_t> result;
    if (!text || !*text) return result;

    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        uint32_t start = 0;
        while (*p >= '0' && *p <= '9') {
            start = start * 10 + (*p - '0');
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '-') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            uint32_t end = 0;
            while (*p >= '0' && *p <= '9') {
                end = end * 10 + (*p - '0');
                p++;
            }
            if (end < start) std::swap(start, end);
            for (uint32_t i = start; i <= end && i <= max_id + 100; i++) {
                result.insert(i);
            }
        } else {
            result.insert(start);
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }
    return result;
}

static pfc::string8 build_range_string(const std::set<uint32_t>& indices) {
    pfc::string8 out;
    if (indices.empty()) return out;

    auto it = indices.begin();
    uint32_t range_start = *it;
    uint32_t range_end = *it;
    ++it;

    auto flush = [&]() {
        if (out.get_length() > 0) out << ", ";
        if (range_start == range_end) {
            out << range_start;
        } else {
            out << range_start << "-" << range_end;
        }
    };

    while (it != indices.end()) {
        if (*it == range_end + 1) {
            range_end = *it;
        } else {
            flush();
            range_start = *it;
            range_end = *it;
        }
        ++it;
    }
    flush();
    return out;
}

class CSubsongPreferences;

class CSubsongListHost : public IListControlOwnerDataSource, public IListControlOwnerDataCells {
public:
    CSubsongPreferences* m_dialog = nullptr;
    std::vector<subsong_entry> m_subsongs;

    size_t listGetItemCount(ctx_t) override {
        return m_subsongs.size();
    }

    pfc::string8 listGetSubItemText(ctx_t, size_t item, size_t subItem) override {
        if (item >= m_subsongs.size()) return "";
        switch (subItem) {
            case 0: return "";
            case 1: { pfc::string8 s; s << m_subsongs[item].subsong_id; return s; }
            case 2: return m_subsongs[item].name;
            default: return "";
        }
    }

    bool listIsColumnEditable(ctx_t, size_t) override { return false; }
    void listSetEditField(ctx_t, size_t, size_t, const char*) override {}
    void listSubItemClicked(ctx_t, size_t, size_t) override {}
    void listSelChanged(ctx_t) override {}
    void listItemAction(ctx_t, size_t) override {}
    bool listRemoveItems(ctx_t, pfc::bit_array const&) override { return false; }

    // IListControlOwnerDataCells
    CListControl::cellType_t listCellType(cellsCtx_t, size_t item, size_t subItem) override {
        if (subItem == 0) return &PFC_SINGLETON(CListCell_Checkbox);
        return &PFC_SINGLETON(CListCell_Text);
    }

    bool listCellCheckState(cellsCtx_t, size_t item, size_t subItem) override {
        if (subItem == 0 && item < m_subsongs.size()) return m_subsongs[item].checked;
        return false;
    }

    void listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state) override;
};

class CSubsongPreferences : public CDialogImpl<CSubsongPreferences>, public preferences_page_instance {
public:
    CSubsongPreferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
        , m_subsong_list(&m_subsong_host, &m_subsong_host)
    {
        m_subsong_host.m_dialog = this;
    }

    enum { IDD = IDD_SUBSONG_MANAGER };

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (HasChanged()) state |= preferences_state::changed;
        return state;
    }

    void apply() override {
        apply_range_to_checkboxes();
        sync_range_from_checkboxes();

        // Remember current file selection so we can restore it after refresh.
        size_t prev_sel = m_file_list.GetSingleSel();
        pfc::string8 prev_path;
        if (prev_sel != SIZE_MAX && prev_sel < m_files.size())
            prev_path = m_files[prev_sel].path;

        auto& db = subsong_database::get();

        std::vector<std::string> affected_paths;
        for (auto& [path, excluded] : m_pending_exclusions) {
            affected_paths.push_back(path);
        }

        for (auto& [path, excluded] : m_pending_exclusions) {
            if (excluded.empty()) {
                db.clear_exclusions(path.c_str());
            } else {
                db.set_excluded(path.c_str(), excluded);
            }
        }

        db.save();

        for (auto& path : affected_paths) {
            rechapter_file(path.c_str());
        }
        notify_metadb_for_autoplaylists(affected_paths);
        refresh_all_autoplaylists();

        m_pending_exclusions.clear();
        m_has_pending_changes = false;

        scan_playlist();

        // Restore selection to the previously selected file if still present.
        size_t restore_sel = SIZE_MAX;
        if (!prev_path.is_empty()) {
            for (size_t i = 0; i < m_files.size(); ++i) {
                if (pfc::stricmp_ascii(m_files[i].path.c_str(), prev_path.c_str()) == 0) {
                    restore_sel = i;
                    break;
                }
            }
        }

        if (restore_sel != SIZE_MAX) {
            m_file_list.SelectSingle(restore_sel);
            load_subsongs_for_selection();
        } else {
            m_subsong_host.m_subsongs.clear();
            m_subsong_list.ReloadData();
        }

        m_callback->on_state_changed();
    }

    void reset() override {
        m_pending_exclusions.clear();
        m_has_pending_changes = false;
        load_subsongs_for_selection();
        m_callback->on_state_changed();
    }

    void notify_changed() {
        m_has_pending_changes = true;
        update_pending_from_checkboxes();
        sync_range_from_checkboxes();
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CSubsongPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_RANGE_INPUT, EN_CHANGE, OnRangeOrModeChanged)
        COMMAND_HANDLER_EX(IDC_MODE_COMBO, CBN_SELCHANGE, OnRangeOrModeChanged)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM) {
        m_file_list.CreateInDialog(*this, IDC_FILE_LIST);
        m_file_list.SetSelectionModeSingle();
        m_subsong_list.CreateInDialog(*this, IDC_SUBSONG_LIST);

        m_dark.AddDialogWithControls(*this);

        auto DPI = m_file_list.GetDPI();
        m_file_list.AddColumn("File Name", MulDiv(130, DPI.cx, 96));
        m_file_list.AddColumnAutoWidth("Subsongs");

        m_subsong_list.AddColumn("", MulDiv(24, DPI.cx, 96));
        m_subsong_list.AddColumn("Index", MulDiv(40, DPI.cx, 96));
        m_subsong_list.AddColumnAutoWidth("Name");

        CComboBox combo(GetDlgItem(IDC_MODE_COMBO));
        combo.AddString(L"Include");
        combo.AddString(L"Exclude");
        combo.SetCurSel(0);

        m_file_list.onSelChange = [this]() {
            size_t sel = m_file_list.GetSingleSel();
            load_subsongs_for_selection();
        };

        scan_playlist();
        return FALSE;
    }

    void OnRangeOrModeChanged(UINT, int, CWindow) {
        if (m_updating_range_ui) return;
        if (m_file_list.GetSingleSel() != SIZE_MAX && !m_subsong_host.m_subsongs.empty()) {
            m_has_pending_changes = true;
            m_callback->on_state_changed();
        }
    }

    void apply_range_to_checkboxes() {
        size_t sel = m_file_list.GetSingleSel();
        if (sel == SIZE_MAX || sel >= m_files.size()) return;
        if (m_subsong_host.m_subsongs.empty()) return;

        CString range_text;
        GetDlgItemText(IDC_RANGE_INPUT, range_text);
        pfc::string8 range_utf8 = pfc::stringcvt::string_utf8_from_wide(range_text.GetString()).get_ptr();
        if (range_utf8.is_empty()) return;

        CComboBox combo(GetDlgItem(IDC_MODE_COMBO));
        bool is_include = (combo.GetCurSel() == 0);

        uint32_t max_id = 0;
        for (auto& sub : m_subsong_host.m_subsongs) {
            if (sub.subsong_id > max_id) max_id = sub.subsong_id;
        }

        auto parsed = parse_range_input(range_utf8.c_str(), max_id);

        for (auto& sub : m_subsong_host.m_subsongs) {
            bool in_set = parsed.count(sub.subsong_id) > 0;
            sub.checked = is_include ? in_set : !in_set;
        }

        update_pending_from_checkboxes();
    }

    static uint32_t query_real_subsong_count(const char* path) {
        try {
            input_info_reader::ptr reader;
            abort_callback_dummy abort;
            input_entry::g_open_for_info_read(reader, nullptr, path, abort, true);
            return reader->get_subsong_count();
        } catch (...) {
            return 0;
        }
    }

    void scan_playlist() {
        m_files.clear();
        m_file_list.SetItemCount(0);

        auto plm = playlist_manager::get();
        size_t active = plm->get_active_playlist();
        if (active == SIZE_MAX) return;

        metadb_handle_list items;
        plm->playlist_get_all_items(active, items);

        std::unordered_map<std::string, std::set<uint32_t>> path_subsongs;
        for (size_t i = 0; i < items.get_count(); i++) {
            auto h = items[i];
            path_subsongs[std::string(h->get_path())].insert(h->get_subsong_index());
        }

        auto& db = subsong_database::get();
        std::set<std::string> seen_paths;

        for (auto& [path, observed] : path_subsongs) {
            pfc::string8 lower_path;
            lower_path.convert_to_lower_ascii(path.c_str());
            std::string key(lower_path.c_str());
            if (seen_paths.count(key)) continue;

            uint32_t observed_count = (uint32_t)observed.size();
            bool has_db_exclusions = db.has_exclusions(path.c_str());

            if (observed_count <= 1 && !has_db_exclusions)
                continue;

            seen_paths.insert(key);

            // Query the decoder for the true subsong count rather than using
            // the playlist's observed count (which reflects exclusion filters).
            uint32_t real_count = query_real_subsong_count(path.c_str());
            if (real_count == 0) real_count = observed_count;
            add_file_entry(path, real_count);
        }
    }

    void add_file_entry(const std::string& path, uint32_t total) {
        file_entry fe;
        fe.path = path.c_str();
        // Extract just the filename — foobar paths may contain both / and \ separators.
        const char* p = path.c_str();
        const char* last_sep = nullptr;
        for (const char* c = p; *c; ++c) {
            if (*c == '/' || *c == '\\') last_sep = c;
        }
        fe.display_name = last_sep ? (last_sep + 1) : p;
        fe.total_subsongs = total;
        m_files.push_back(std::move(fe));

        size_t idx = m_file_list.AddItem(m_files.back().display_name.c_str());
        pfc::string8 count_str;
        count_str << m_files.back().total_subsongs;
        m_file_list.SetItemText(idx, 1, count_str.c_str());
    }

    void load_subsongs_for_selection() {
        size_t sel = m_file_list.GetSingleSel();
        m_subsong_host.m_subsongs.clear();

        if (sel == SIZE_MAX || sel >= m_files.size()) {
            m_subsong_list.ReloadData();
            return;
        }

        auto& file = m_files[sel];
        auto& db = subsong_database::get();
        std::set<uint32_t> excluded;

        auto pending_it = m_pending_exclusions.find(std::string(file.path.c_str()));
        if (pending_it != m_pending_exclusions.end()) {
            excluded = pending_it->second;
        } else {
            excluded = db.get_excluded(file.path.c_str());
        }

        try {
            input_info_reader::ptr reader;
            abort_callback_dummy abort;
            input_entry::g_open_for_info_read(reader, nullptr, file.path.c_str(), abort, true);

            uint32_t count = reader->get_subsong_count();

            for (uint32_t i = 0; i < count; i++) {
                uint32_t id = reader->get_subsong(i);
                subsong_entry se;
                se.subsong_id = id;
                se.checked = (excluded.find(id) == excluded.end());

                try {
                    file_info_impl info;
                    reader->get_info(id, info, abort);
                    const char* title = info.meta_get("title", 0);
                    if (title) se.name = title;
                } catch (...) {
                }

                if (se.name.is_empty()) {
                    se.name << "Subsong " << id;
                }

                m_subsong_host.m_subsongs.push_back(std::move(se));
            }
        } catch (...) {
        }

        m_subsong_list.ReloadData();
        sync_range_from_checkboxes();
    }

    void update_pending_from_checkboxes() {
        size_t sel = m_file_list.GetSingleSel();
        if (sel == SIZE_MAX || sel >= m_files.size()) return;

        std::set<uint32_t> excluded;
        for (auto& sub : m_subsong_host.m_subsongs) {
            if (!sub.checked) excluded.insert(sub.subsong_id);
        }

        m_pending_exclusions[std::string(m_files[sel].path.c_str())] = excluded;
    }

    void sync_range_from_checkboxes() {
        CComboBox combo(GetDlgItem(IDC_MODE_COMBO));
        bool is_include = (combo.GetCurSel() == 0);

        std::set<uint32_t> display_set;
        if (is_include) {
            for (auto& sub : m_subsong_host.m_subsongs) {
                if (sub.checked) display_set.insert(sub.subsong_id);
            }
        } else {
            for (auto& sub : m_subsong_host.m_subsongs) {
                if (!sub.checked) display_set.insert(sub.subsong_id);
            }
        }

        pfc::string8 range_str = build_range_string(display_set);
        m_updating_range_ui = true;
        SetDlgItemText(IDC_RANGE_INPUT,
            pfc::stringcvt::string_wide_from_utf8(range_str.c_str()).get_ptr());
        m_updating_range_ui = false;
    }

    void rechapter_file(const char* path) {
        std::vector<uint32_t> all_subsong_ids;
        try {
            input_info_reader::ptr reader;
            abort_callback_dummy abort;
            input_entry::g_open_for_info_read(reader, nullptr, path, abort, true);
            uint32_t count = reader->get_subsong_count();
            for (uint32_t i = 0; i < count; i++) {
                all_subsong_ids.push_back(reader->get_subsong(i));
            }
        } catch (...) {
            return;
        }

        if (all_subsong_ids.empty()) return;

        auto& db = subsong_database::get();
        auto enabled = db.get_enabled_subsongs(path, all_subsong_ids);
        if (enabled.empty()) enabled = all_subsong_ids;

        auto metadb_api = metadb::get();
        metadb_handle_list new_handles;
        for (uint32_t id : enabled) {
            new_handles += metadb_api->handle_create(make_playable_location(path, id));
        }

        auto plm = playlist_manager::get();
        size_t num_playlists = plm->get_playlist_count();

        for (size_t p = 0; p < num_playlists; p++) {
            const size_t item_count = plm->playlist_get_item_count(p);
            if (item_count == 0) continue;

            pfc::bit_array_bittable to_remove(item_count);
            bool any_match = false;
            bool any_sel = false;
            size_t insert_base = SIZE_MAX;

            for (size_t i = 0; i < item_count; i++) {
                auto item = plm->playlist_get_item_handle(p, i);
                if (metadb::path_compare(path, item->get_path()) == 0) {
                    to_remove.set(i, true);
                    any_match = true;
                    if (insert_base == SIZE_MAX) insert_base = i;
                    any_sel = any_sel || plm->playlist_is_item_selected(p, i);
                }
            }

            if (!any_match) continue;
            if (!plm->playlist_remove_items(p, to_remove)) continue;

            if (insert_base != SIZE_MAX) {
                plm->playlist_insert_items(p, insert_base, new_handles, pfc::bit_array_val(any_sel));
            }
        }

        if (new_handles.get_count() > 0) {
            try {
                metadb_io_v2::get()->load_info_async(
                    new_handles,
                    metadb_io::load_info_force,
                    core_api::get_main_window(),
                    metadb_io_v3::op_flag_delay_ui,
                    nullptr);
            } catch (...) {}
        }
    }

    void notify_metadb_for_autoplaylists(const std::vector<std::string>& paths) {
        if (paths.empty()) return;

        metadb_hint_list::ptr hints;
        try {
            hints = metadb_io_v2::get()->create_hint_list();
        } catch (...) {
            FB2K_console_formatter() << "[DSM] create_hint_list failed";
            return;
        }

        size_t ok_count = 0;
        for (auto& path : paths) {
            try {
                input_info_reader::ptr reader;
                abort_callback_dummy abort;
                // from_redirect = false: use our redirect view (filtered subsongs)
                input_entry::g_open_for_info_read(reader, nullptr, path.c_str(), abort, false);
                hints->add_hint_reader(path.c_str(), reader, abort);
                ++ok_count;
            } catch (const std::exception& e) {
                FB2K_console_formatter() << "[DSM] hint_reader failed: " << path.c_str() << " ; " << e.what();
            } catch (...) {
                FB2K_console_formatter() << "[DSM] hint_reader failed: " << path.c_str();
            }
        }

        if (ok_count == 0) {
            FB2K_console_formatter() << "[DSM] no hint entries to commit";
            return;
        }

        try {
            hints->on_done();
            FB2K_console_formatter() << "[DSM] metadb hints committed: " << (uint32_t)ok_count;
        } catch (...) {
            FB2K_console_formatter() << "[DSM] hints on_done failed";
        }
    }

    void refresh_all_autoplaylists() {
        dsm_install_and_refresh_autoplaylists(/*force_refresh=*/true);
    }

    bool HasChanged() {
        return m_has_pending_changes || !m_pending_exclusions.empty();
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CCoreDarkModeHooks m_dark;

    FileListControl_t m_file_list;
    CSubsongListHost m_subsong_host;
    SubsongListControl_t m_subsong_list;

    std::vector<file_entry> m_files;
    std::unordered_map<std::string, std::set<uint32_t>> m_pending_exclusions;
    bool m_has_pending_changes = false;
    bool m_updating_range_ui = false;
};

void CSubsongListHost::listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state) {
    if (subItem == 0 && item < m_subsongs.size()) {
        m_subsongs[item].checked = state;
        if (m_dialog) m_dialog->notify_changed();
    }
}

class prefs_page_subsong : public preferences_page_impl<CSubsongPreferences> {
public:
    const char* get_name() override { return "Subsong Manager"; }
    GUID get_guid() override { return guid_prefs_subsong; }
    GUID get_parent_guid() override { return guid_prefs_branch; }
};

static preferences_page_factory_t<prefs_page_subsong> g_prefs_subsong_factory;
// Two init stages: after_library_init installs proxies early (before autoplaylist
// content is populated from the library); after_ui_init catches any playlists
// that weren't available yet.  The skip-if-already-proxied guard in
// dsm_install_and_refresh_autoplaylists() prevents the redundant remove/add gap.
FB2K_ON_INIT_STAGE(dsm_reinstall_autoplaylist_proxy_on_startup, init_stages::after_library_init);
FB2K_ON_INIT_STAGE(dsm_reinstall_autoplaylist_proxy_on_startup, init_stages::after_ui_init);

} // namespace
