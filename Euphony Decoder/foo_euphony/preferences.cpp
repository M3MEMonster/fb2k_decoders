#include "stdafx.h"

#include "euphony_config.h"
#include "resource.h"

#include <SDK/cfg_var.h>
#include <SDK/metadb.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// {20B46A70-20DF-4E50-9654-47E0F7C9F4BE}
static constexpr GUID guid_cfg_default_duration_seconds = {
    0x20b46a70, 0x20df, 0x4e50, { 0x96, 0x54, 0x47, 0xe0, 0xf7, 0xc9, 0xf4, 0xbe }
};
// {9A82CD22-45E4-4A22-AF93-BE3009CCF5D4}
static constexpr GUID guid_cfg_play_indefinitely = {
    0x9a82cd22, 0x45e4, 0x4a22, { 0xaf, 0x93, 0xbe, 0x30, 0x09, 0xcc, 0xf5, 0xd4 }
};

static cfg_uint cfg_default_duration_seconds(guid_cfg_default_duration_seconds, 180);
static cfg_bool cfg_play_indefinitely(guid_cfg_play_indefinitely, false);

static unsigned clamp_duration_seconds(unsigned seconds) {
    return (std::max)(1u, (std::min)(seconds, 24u * 60u * 60u));
}

static pfc::string8 format_mss(unsigned totalSeconds) {
    const unsigned minutes = totalSeconds / 60;
    const unsigned seconds = totalSeconds % 60;
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%u:%02u", minutes, seconds);
    return pfc::string8(buf);
}

static bool parse_mss(const char* text, unsigned& outSeconds) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    const char* colon = std::strchr(text, ':');
    if (colon == nullptr) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(text, &end, 10);
        if (end == text || (end != nullptr && *end != '\0')) {
            return false;
        }
        outSeconds = clamp_duration_seconds(static_cast<unsigned>(v));
        return true;
    }
    unsigned mins = 0;
    unsigned secs = 0;
    char tail = 0;
    if (std::sscanf(text, "%u:%u%c", &mins, &secs, &tail) != 2) {
        return false;
    }
    if (secs >= 60) {
        return false;
    }
    outSeconds = clamp_duration_seconds(mins * 60 + secs);
    return true;
}

static void refresh_euphony_metadb() {
    metadb_handle_list handles;
    auto plm = playlist_manager::get();
    const size_t playlistCount = plm->get_playlist_count();
    for (size_t p = 0; p < playlistCount; ++p) {
        metadb_handle_list items;
        plm->playlist_get_all_items(p, items);
        for (size_t i = 0; i < items.get_count(); ++i) {
            auto h = items[i];
            const char* ext = pfc::string_extension(h->get_path());
            if (ext != nullptr && _stricmp(ext, "eup") == 0) {
                handles += h;
            }
        }
    }
    if (handles.get_count() > 0) {
        metadb_io_v2::get()->load_info_async(
            handles,
            metadb_io::load_info_force,
            core_api::get_main_window(),
            metadb_io_v3::op_flag_delay_ui,
            nullptr);
    }
}

namespace euphony_config {

unsigned default_duration_seconds() {
    const auto raw = static_cast<unsigned>(cfg_default_duration_seconds.get_value());
    return clamp_duration_seconds(raw);
}

bool play_indefinitely() {
    return cfg_play_indefinitely;
}

} // namespace euphony_config

class CEuphonyPrefs : public CDialogImpl<CEuphonyPrefs>, public preferences_page_instance {
public:
    enum { IDD = IDD_EUPHONY_PREFS };

    CEuphonyPrefs(preferences_page_callback::ptr cb) : m_cb(cb) {}

    fb2k::hwnd_t get_wnd() override { return m_hWnd; }

    t_uint32 get_state() override {
        t_uint32 s = preferences_state::resettable;
        if (is_changed()) {
            s |= preferences_state::changed;
        }
        return s;
    }

    void apply() override {
        cfg_default_duration_seconds = clamp_duration_seconds(m_seconds);
        cfg_play_indefinitely = m_indefinite;
        m_saved_seconds = m_seconds;
        m_saved_indefinite = m_indefinite;
        refresh_euphony_metadb();
        m_cb->on_state_changed();
    }

    void reset() override {
        m_seconds = 180;
        m_indefinite = false;
        m_saved_seconds = m_seconds;
        m_saved_indefinite = m_indefinite;
        refresh_ui();
        m_cb->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CEuphonyPrefs)
        MSG_WM_INITDIALOG(OnInit)
        COMMAND_HANDLER_EX(IDC_DEFAULT_DURATION, EN_CHANGE, OnDurationChanged)
        COMMAND_HANDLER_EX(IDC_PLAY_INDEFINITELY, BN_CLICKED, OnIndefiniteClicked)
    END_MSG_MAP()

private:
    BOOL OnInit(CWindow, LPARAM) {
        m_seconds = euphony_config::default_duration_seconds();
        m_indefinite = euphony_config::play_indefinitely();
        m_saved_seconds = m_seconds;
        m_saved_indefinite = m_indefinite;
        refresh_ui();
        return FALSE;
    }

    void OnDurationChanged(UINT, int, CWindow) {
        CStringW textW;
        GetDlgItemText(IDC_DEFAULT_DURATION, textW);
        pfc::stringcvt::string_utf8_from_wide conv(textW.GetString());
        pfc::string8 text = conv.get_ptr();
        unsigned parsed = 0;
        if (parse_mss(text.c_str(), parsed)) {
            m_seconds = parsed;
            m_cb->on_state_changed();
        }
    }

    void OnIndefiniteClicked(UINT, int, CWindow) {
        m_indefinite = IsDlgButtonChecked(IDC_PLAY_INDEFINITELY) == BST_CHECKED;
        m_cb->on_state_changed();
    }

    bool is_changed() const {
        return m_seconds != m_saved_seconds || m_indefinite != m_saved_indefinite;
    }

    void refresh_ui() {
        SetDlgItemTextW(IDC_DEFAULT_DURATION, pfc::stringcvt::string_wide_from_utf8(format_mss(m_seconds).c_str()).get_ptr());
        CheckDlgButton(IDC_PLAY_INDEFINITELY, m_indefinite ? BST_CHECKED : BST_UNCHECKED);
    }

private:
    preferences_page_callback::ptr m_cb;
    unsigned m_seconds = 180;
    bool m_indefinite = false;
    unsigned m_saved_seconds = 180;
    bool m_saved_indefinite = false;
};

class prefs_page_euphony : public preferences_page_v3 {
public:
    const char* get_name() override { return "Euphony"; }

    GUID get_guid() override {
        // {5E9B8AB1-AD42-4291-9D65-34CF9A7E16A8}
        return { 0x5e9b8ab1, 0xad42, 0x4291, { 0x9d, 0x65, 0x34, 0xcf, 0x9a, 0x7e, 0x16, 0xa8 } };
    }

    GUID get_parent_guid() override { return preferences_page::guid_input; }

    preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr cb) override {
        auto* d = new service_impl_t<CEuphonyPrefs>(cb);
        d->Create(parent);
        return d;
    }
};

static preferences_page_factory_t<prefs_page_euphony> g_prefs;
