#include "stdafx.h"
#include "resource.h"
#include "guids.h"
#include "duration_db.h"

typedef CListControlFb2kColors<CListControlOwnerData> DurationListControl_t;

namespace {

static pfc::string8 format_duration(double seconds) {
    if (seconds <= 0) return "0:00";
    int total_ms = (int)(seconds * 1000 + 0.5);
    int ms = total_ms % 1000;
    int total_secs = total_ms / 1000;
    int secs = total_secs % 60;
    int mins = total_secs / 60;
    pfc::string8 out;
    out << mins << ":" << pfc::format_int(secs, 2);
    if (ms > 0) out << "." << pfc::format_int(ms, 3);
    return out;
}

static double parse_duration(const char* str) {
    if (!str || !*str) return 0;

    double result = 0;
    int parts[3] = {0, 0, 0};
    int part_count = 0;
    double frac = 0;

    const char* p = str;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            parts[part_count] = parts[part_count] * 10 + (*p - '0');
        } else if (*p == ':') {
            if (part_count < 2) part_count++;
        } else if (*p == '.') {
            p++;
            int frac_int = 0;
            int frac_digits = 0;
            while (*p >= '0' && *p <= '9') {
                frac_int = frac_int * 10 + (*p - '0');
                frac_digits++;
                p++;
            }
            double divisor = 1.0;
            for (int i = 0; i < frac_digits; i++) divisor *= 10.0;
            frac = frac_int / divisor;
            continue;
        }
        p++;
    }

    if (part_count == 0) {
        result = parts[0];
    } else if (part_count == 1) {
        result = parts[0] * 60.0 + parts[1];
    } else {
        result = parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
    }

    return result + frac;
}

class CDurationPreferences;

class CDurationListHost : public IListControlOwnerDataSource {
public:
    CDurationPreferences* m_dialog = nullptr;

    std::vector<const duration_record*> m_filtered;
    std::unordered_map<std::string, double> m_pending_edits;
    std::set<std::string> m_pending_deletes;
    pfc::string8 m_search_query;

    void refresh_filter() {
        auto& db = duration_database::get();
        m_filtered = db.search(m_search_query.c_str());

        auto it = std::remove_if(m_filtered.begin(), m_filtered.end(),
            [this](const duration_record* r) {
                return m_pending_deletes.count(std::string(r->hash.c_str())) > 0;
            });
        m_filtered.erase(it, m_filtered.end());
    }

    double get_display_duration(size_t item) const {
        if (item >= m_filtered.size()) return 0;
        auto* rec = m_filtered[item];
        auto it = m_pending_edits.find(std::string(rec->hash.c_str()));
        if (it != m_pending_edits.end()) return it->second;
        return rec->custom_duration;
    }

    bool has_changes() const {
        return !m_pending_edits.empty() || !m_pending_deletes.empty();
    }

    size_t listGetItemCount(ctx_t) override {
        return m_filtered.size();
    }

    pfc::string8 listGetSubItemText(ctx_t, size_t item, size_t subItem) override {
        if (item >= m_filtered.size()) return "";
        auto* rec = m_filtered[item];
        switch (subItem) {
            case 0: return rec->file_name;
            case 1: return rec->song_name;
            case 2: { pfc::string8 s; s << rec->subsong_index; return s; }
            case 3: return format_duration(get_display_duration(item));
            default: return "";
        }
    }

    bool listIsColumnEditable(ctx_t, size_t subItem) override {
        return subItem == 3;
    }

    void listSetEditField(ctx_t, size_t item, size_t subItem, const char* val) override;

    bool listRemoveItems(ctx_t, pfc::bit_array const& mask) override;

    uint32_t listGetEditFlags(ctx_t, size_t, size_t) override { return 0; }
    void listSubItemClicked(ctx_t, size_t, size_t subItem) override {
        m_last_clicked_subitem = subItem;
    }
    void listSelChanged(ctx_t) override {}
    void listItemAction(ctx_t ctx, size_t item) override {
        if (m_last_clicked_subitem == 3) {
            auto* list = const_cast<CListControlOwnerData*>(ctx);
            list->TableEdit_Start(item, 3);
        }
    }

    size_t m_last_clicked_subitem = SIZE_MAX;

    bool listKeyDown(ctx_t, UINT nChar, UINT, UINT) override;
};

class CDurationPreferences : public CDialogImpl<CDurationPreferences>, public preferences_page_instance {
public:
    CDurationPreferences(preferences_page_callback::ptr callback)
        : m_callback(callback), m_list(&m_host)
    {
        m_host.m_dialog = this;
    }

    enum { IDD = IDD_DURATION_MANAGER };

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (m_host.has_changes()) state |= preferences_state::changed;
        return state;
    }

    void apply() override {
        auto& db = duration_database::get();
        metadb_handle_list affected;
        auto metadb_api = metadb::get();

        // Capture impacted items before mutating DB so deletions can be refreshed too.
        for (auto& [hash, _dur] : m_host.m_pending_edits) {
            auto rec = db.lookup_by_hash(hash.c_str());
            if (rec) {
                affected += metadb_api->handle_create(
                    make_playable_location(rec->full_path.c_str(), rec->subsong_index));
            }
        }
        for (auto& hash : m_host.m_pending_deletes) {
            auto rec = db.lookup_by_hash(hash.c_str());
            if (rec) {
                affected += metadb_api->handle_create(
                    make_playable_location(rec->full_path.c_str(), rec->subsong_index));
            }
        }

        for (auto& [hash, dur] : m_host.m_pending_edits) {
            db.update_duration(hash.c_str(), dur);
        }

        for (auto& hash : m_host.m_pending_deletes) {
            db.remove(hash.c_str());
        }

        db.save();

        m_host.m_pending_edits.clear();
        m_host.m_pending_deletes.clear();
        m_host.refresh_filter();
        m_list.ReloadData();

        refresh_metadb(affected);
        m_callback->on_state_changed();
    }

    void reset() override {
        m_host.m_pending_edits.clear();
        m_host.m_pending_deletes.clear();
        m_host.refresh_filter();
        m_list.ReloadData();
        m_callback->on_state_changed();
    }

    void notify_changed() {
        m_callback->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CDurationPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_SEARCH, EN_CHANGE, OnSearchChange)
        MSG_WM_CONTEXTMENU(OnContextMenu)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM) {
        m_list.CreateInDialog(*this, IDC_DURATION_LIST);
        m_dark.AddDialogWithControls(*this);

        auto DPI = m_list.GetDPI();
        m_list.AddColumn("File Name", MulDiv(120, DPI.cx, 96));
        m_list.AddColumn("Song Name", MulDiv(120, DPI.cx, 96));
        m_list.AddColumn("Subsong", MulDiv(50, DPI.cx, 96));
        m_list.AddColumnAutoWidth("Duration");

        duration_database::get().load();
        m_host.refresh_filter();
        m_list.ReloadData();

        return FALSE;
    }

    void OnSearchChange(UINT, int, CWindow) {
        CString text;
        GetDlgItemText(IDC_SEARCH, text);
        m_host.m_search_query = pfc::stringcvt::string_utf8_from_wide(text.GetString()).get_ptr();
        m_host.refresh_filter();
        m_list.ReloadData();
    }

    void OnContextMenu(CWindow wnd, CPoint pt) {
        if (wnd.m_hWnd != m_list.m_hWnd) {
            SetMsgHandled(FALSE);
            return;
        }

        if (m_list.GetSelectedCount() == 0) return;

        enum { CMD_EDIT = 1, CMD_DELETE = 2 };
        CMenu menu;
        menu.CreatePopupMenu();
        menu.AppendMenu(MF_STRING, CMD_EDIT, L"Edit Duration");
        menu.AppendMenu(MF_STRING, CMD_DELETE, L"Delete");

        if (pt.x == -1 && pt.y == -1) {
            RECT rc;
            m_list.GetWindowRect(&rc);
            pt.x = rc.left + 10;
            pt.y = rc.top + 10;
        }

        int cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, m_hWnd);

        if (cmd == CMD_EDIT) {
            size_t sel = m_list.GetFirstSelected();
            if (sel != SIZE_MAX) {
                m_list.TableEdit_Start(sel, 3);
            }
        } else if (cmd == CMD_DELETE) {
            m_list.RequestRemoveSelection();
        }
    }

    void refresh_metadb(const metadb_handle_list& handlesIn) {
        if (handlesIn.get_count() == 0) return;
        metadb_handle_list handles = handlesIn;
        handles.remove_duplicates();
        abort_callback_dummy abort;

        metadb_handle_list existing;
        for (t_size i = 0; i < handles.get_count(); ++i) {
            auto h = handles[i];
            bool exists = false;
            try {
                exists = filesystem::g_exists(h->get_path(), abort);
            } catch (...) {
                exists = false;
            }
            if (!exists) continue;
            existing += h;
        }

        if (existing.get_count() > 0) {
            metadb_io_v2::get()->load_info_async(
                existing,
                metadb_io::load_info_force,
                m_hWnd,
                metadb_io_v2::op_flag_delay_ui,
                nullptr);
        }
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CCoreDarkModeHooks m_dark;
    CDurationListHost m_host;
    DurationListControl_t m_list;
};

void CDurationListHost::listSetEditField(ctx_t, size_t item, size_t subItem, const char* val) {
    if (subItem != 3 || item >= m_filtered.size()) return;
    double dur = parse_duration(val);
    if (dur >= 0) {
        m_pending_edits[std::string(m_filtered[item]->hash.c_str())] = dur;
        if (m_dialog) m_dialog->notify_changed();
    }
}

bool CDurationListHost::listRemoveItems(ctx_t, pfc::bit_array const& mask) {
    for (size_t i = 0; i < m_filtered.size(); i++) {
        if (mask[i]) {
            m_pending_deletes.insert(std::string(m_filtered[i]->hash.c_str()));
        }
    }
    refresh_filter();
    if (m_dialog) m_dialog->notify_changed();
    return true;
}

bool CDurationListHost::listKeyDown(ctx_t ctx, UINT nChar, UINT, UINT) {
    if (nChar == VK_DELETE) {
        auto* list = const_cast<CListControlOwnerData*>(ctx);
        list->RequestRemoveSelection();
        return true;
    }
    return false;
}

static preferences_branch_factory g_prefs_branch(
    guid_prefs_branch,
    preferences_page::guid_tools,
    "Duration & Subsong Manager"
);

class prefs_page_duration : public preferences_page_impl<CDurationPreferences> {
public:
    const char* get_name() override { return "Duration Manager"; }
    GUID get_guid() override { return guid_prefs_duration; }
    GUID get_parent_guid() override { return guid_prefs_branch; }
};

static preferences_page_factory_t<prefs_page_duration> g_prefs_duration_factory;

} // namespace
