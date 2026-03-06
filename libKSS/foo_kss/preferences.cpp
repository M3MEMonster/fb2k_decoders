#include "stdafx.h"

#include "kss_config.h"
#include "resource.h"

#include <SDK/cfg_var.h>
#include <SDK/metadb.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <commctrl.h>

// {E6A8471D-7EFA-4488-B4E6-31E4B27CFA2D}
static constexpr GUID guid_cfg_disabled = {
    0xe6a8471d, 0x7efa, 0x4488, { 0xb4, 0xe6, 0x31, 0xe4, 0xb2, 0x7c, 0xfa, 0x2d }
};
// {3153F39D-02E9-4B61-A5C2-168A702853D0}
static constexpr GUID guid_cfg_default_duration_seconds = {
    0x3153f39d, 0x02e9, 0x4b61, { 0xa5, 0xc2, 0x16, 0x8a, 0x70, 0x28, 0x53, 0xd0 }
};
// {F32B6A78-BBE2-4AE0-BFF6-9E87E9A214A1}
static constexpr GUID guid_cfg_play_indefinitely = {
    0xf32b6a78, 0xbbe2, 0x4ae0, { 0xbf, 0xf6, 0x9e, 0x87, 0xe9, 0xa2, 0x14, 0xa1 }
};

static cfg_string cfg_disabled(guid_cfg_disabled, "");
static cfg_uint cfg_default_duration_seconds(guid_cfg_default_duration_seconds, 180);
static cfg_bool cfg_play_indefinitely(guid_cfg_play_indefinitely, false);

struct fmt_desc { const char* key; const char* label; };
static const fmt_desc g_fmts[] = {
    { "kss", "KSS - KSS/KSCC/KSSX music data" },
    { "mgs", "MGS - MGSDRV music data" },
    { "bgm", "BGM - BGM format" },
    { "opx", "OPX - OPX format" },
    { "mpk", "MPK - MPK 103/106 format" },
    { "mbm", "MBM - MBM format" },
};
static constexpr int NUM_FMTS = (int)(sizeof(g_fmts) / sizeof(g_fmts[0]));

static std::set<std::string> parse_set(const char* s) {
    std::set<std::string> r;
    std::istringstream iss(s ? s : "");
    std::string tok;
    while (std::getline(iss, tok, ';')) if (!tok.empty()) r.insert(tok);
    return r;
}

static std::string serialize_set(const std::set<std::string>& s) {
    std::string out;
    for (const auto& e : s) {
        if (!out.empty()) out += ';';
        out += e;
    }
    return out;
}

static unsigned clamp_duration_seconds(unsigned v) {
    return (std::max)(1u, (std::min)(v, 24u * 60u * 60u));
}

static pfc::string8 format_mss(unsigned totalSeconds) {
    const unsigned minutes = totalSeconds / 60;
    const unsigned seconds = totalSeconds % 60;
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%u:%02u", minutes, seconds);
    return pfc::string8(buf);
}

static bool parse_mss(const char* text, unsigned& outSeconds) {
    if (text == nullptr || text[0] == '\0') return false;
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(text, &end, 10);
        if (end == text || (end != nullptr && *end != '\0')) return false;
        outSeconds = clamp_duration_seconds(static_cast<unsigned>(v));
        return true;
    }
    unsigned mins = 0, secs = 0; char tail = 0;
    if (std::sscanf(text, "%u:%u%c", &mins, &secs, &tail) != 2) return false;
    if (secs >= 60) return false;
    outSeconds = clamp_duration_seconds(mins * 60 + secs);
    return true;
}

static bool is_kss_ext(const char* ext) {
    if (!ext) return false;
    for (int i = 0; i < NUM_FMTS; i++) if (_stricmp(ext, g_fmts[i].key) == 0) return true;
    return false;
}

static void refresh_kss_metadb() {
    metadb_handle_list handles;
    auto plm = playlist_manager::get();
    const size_t playlistCount = plm->get_playlist_count();
    for (size_t p = 0; p < playlistCount; ++p) {
        metadb_handle_list items;
        plm->playlist_get_all_items(p, items);
        for (size_t i = 0; i < items.get_count(); ++i) {
            auto h = items[i];
            const char* ext = pfc::string_extension(h->get_path());
            if (is_kss_ext(ext)) handles += h;
        }
    }
    if (handles.get_count() > 0) {
        metadb_io_v2::get()->load_info_async(handles, metadb_io::load_info_force, core_api::get_main_window(), metadb_io_v3::op_flag_delay_ui, nullptr);
    }
}

namespace kss_config {

bool is_format_enabled(const char* ext) {
    if (!ext) return false;
    std::string e(ext);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    bool known = false;
    for (int i = 0; i < NUM_FMTS; i++) if (e == g_fmts[i].key) { known = true; break; }
    if (!known) return false;
    auto dis = parse_set(cfg_disabled.get_ptr());
    return dis.find(e) == dis.end();
}

unsigned default_duration_seconds() {
    return clamp_duration_seconds((unsigned)cfg_default_duration_seconds.get_value());
}

bool play_indefinitely() {
    return cfg_play_indefinitely;
}

} // namespace kss_config

class CKSSPrefs : public CDialogImpl<CKSSPrefs>, public preferences_page_instance {
public:
    enum { IDD = IDD_KSS_PREFS };
    CKSSPrefs(preferences_page_callback::ptr cb) : m_cb(cb) {}

    fb2k::hwnd_t get_wnd() override { return m_hWnd; }

    t_uint32 get_state() override {
        t_uint32 s = preferences_state::resettable;
        if (m_dis != m_saved_dis || m_seconds != m_saved_seconds || m_indefinite != m_saved_indefinite) s |= preferences_state::changed;
        return s;
    }

    void apply() override {
        cfg_disabled = serialize_set(m_dis).c_str();
        cfg_default_duration_seconds = clamp_duration_seconds(m_seconds);
        cfg_play_indefinitely = m_indefinite;
        m_saved_dis = m_dis;
        m_saved_seconds = m_seconds;
        m_saved_indefinite = m_indefinite;
        refresh_kss_metadb();
        m_cb->on_state_changed();
    }

    void reset() override {
        m_dis.clear();
        m_seconds = 180;
        m_indefinite = false;
        m_saved_dis = m_dis;
        m_saved_seconds = m_seconds;
        m_saved_indefinite = m_indefinite;
        refresh_checks();
        SetDlgItemTextW(IDC_DEFAULT_DURATION, pfc::stringcvt::string_wide_from_utf8(format_mss(m_seconds).c_str()).get_ptr());
        CheckDlgButton(IDC_PLAY_INDEFINITELY, BST_UNCHECKED);
        m_cb->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CKSSPrefs)
        MSG_WM_INITDIALOG(OnInit)
        NOTIFY_HANDLER_EX(IDC_FORMAT_LIST, LVN_ITEMCHANGED, OnChanged)
        COMMAND_HANDLER_EX(IDC_DEFAULT_DURATION, EN_CHANGE, OnDurationChanged)
        COMMAND_HANDLER_EX(IDC_PLAY_INDEFINITELY, BN_CLICKED, OnIndefiniteChanged)
    END_MSG_MAP()

private:
    BOOL OnInit(CWindow, LPARAM) {
        m_lv = GetDlgItem(IDC_FORMAT_LIST);
        ListView_SetExtendedListViewStyle(m_lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMN col{}; col.mask = LVCF_WIDTH | LVCF_TEXT; col.cx = 300; col.pszText = const_cast<LPWSTR>(L"Format");
        ListView_InsertColumn(m_lv, 0, &col);

        m_dis = parse_set(cfg_disabled.get_ptr());
        m_saved_dis = m_dis;
        m_seconds = kss_config::default_duration_seconds();
        m_saved_seconds = m_seconds;
        m_indefinite = kss_config::play_indefinitely();
        m_saved_indefinite = m_indefinite;

        m_init = true;
        for (int i = 0; i < NUM_FMTS; i++) {
            pfc::stringcvt::string_wide_from_utf8 wl(g_fmts[i].label);
            LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = const_cast<LPWSTR>(wl.get_ptr());
            ListView_InsertItem(m_lv, &it);
            ListView_SetCheckState(m_lv, i, m_dis.find(g_fmts[i].key) == m_dis.end() ? TRUE : FALSE);
        }
        SetDlgItemTextW(IDC_DEFAULT_DURATION, pfc::stringcvt::string_wide_from_utf8(format_mss(m_seconds).c_str()).get_ptr());
        CheckDlgButton(IDC_PLAY_INDEFINITELY, m_indefinite ? BST_CHECKED : BST_UNCHECKED);
        m_init = false;

        RECT rc; ::GetClientRect(m_lv, &rc);
        ListView_SetColumnWidth(m_lv, 0, rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL));
        return FALSE;
    }

    LRESULT OnChanged(LPNMHDR ph) {
        if (m_init) return 0;
        auto* p = reinterpret_cast<LPNMLISTVIEW>(ph);
        if (!(p->uChanged & LVIF_STATE)) return 0;
        if (!((p->uNewState ^ p->uOldState) & LVIS_STATEIMAGEMASK)) return 0;
        int i = p->iItem;
        if (i < 0 || i >= NUM_FMTS) return 0;
        if (ListView_GetCheckState(m_lv, i)) m_dis.erase(g_fmts[i].key); else m_dis.insert(g_fmts[i].key);
        m_cb->on_state_changed();
        return 0;
    }

    void OnDurationChanged(UINT, int, CWindow) {
        CStringW textW; GetDlgItemText(IDC_DEFAULT_DURATION, textW);
        pfc::stringcvt::string_utf8_from_wide conv(textW.GetString());
        unsigned parsed = 0;
        if (parse_mss(conv.get_ptr(), parsed)) {
            m_seconds = parsed;
            m_cb->on_state_changed();
        }
    }

    void OnIndefiniteChanged(UINT, int, CWindow) {
        m_indefinite = IsDlgButtonChecked(IDC_PLAY_INDEFINITELY) == BST_CHECKED;
        m_cb->on_state_changed();
    }

    void refresh_checks() {
        m_init = true;
        for (int i = 0; i < NUM_FMTS; i++) ListView_SetCheckState(m_lv, i, m_dis.find(g_fmts[i].key) == m_dis.end() ? TRUE : FALSE);
        m_init = false;
    }

private:
    preferences_page_callback::ptr m_cb;
    HWND m_lv = nullptr;
    bool m_init = false;
    std::set<std::string> m_dis, m_saved_dis;
    unsigned m_seconds = 180, m_saved_seconds = 180;
    bool m_indefinite = false, m_saved_indefinite = false;
};

class prefs_page_kss : public preferences_page_v3 {
public:
    const char* get_name() override { return "LIBKSS"; }
    GUID get_guid() override {
        // {5D3E12B9-17B8-4A58-A463-68DFA127F8F3}
        return { 0x5d3e12b9, 0x17b8, 0x4a58, { 0xa4, 0x63, 0x68, 0xdf, 0xa1, 0x27, 0xf8, 0xf3 } };
    }
    GUID get_parent_guid() override { return preferences_page::guid_input; }
    preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr cb) override {
        auto* d = new service_impl_t<CKSSPrefs>(cb);
        d->Create(parent);
        return d;
    }
};

static preferences_page_factory_t<prefs_page_kss> g_prefs;
