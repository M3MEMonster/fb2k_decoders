#include "stdafx.h"
#include <helpers/atl-misc.h>
#include <SDK/coreDarkMode.h>
#include <libPPUI/win32_op.h>
#include "resource.h"

// ---------------------------------------------------------------------------
// WIN32_OP_FAIL stubs — normally provided by libPPUI.lib, redefined here so
// we don't need to link that library just for the preferences dialog.
// ---------------------------------------------------------------------------
PFC_NORETURN PFC_NOINLINE void WIN32_OP_FAIL() {
    throw exception_win32(::GetLastError());
}
PFC_NORETURN PFC_NOINLINE void WIN32_OP_FAIL_CRITICAL(const char*) {
    pfc::crash();
}
#ifdef _DEBUG
void WIN32_OP_D_FAIL(const wchar_t* msg, const wchar_t* file, unsigned line) {
    _wassert(msg, file, line);
}
#endif

// ---------------------------------------------------------------------------
// GUIDs
// ---------------------------------------------------------------------------

// cfg_ayumi_duration_ms  {A7C2E4F1-3B5D-4890-9A1C-2E3F4D5E6F70}
static const GUID guid_cfg_duration =
{ 0xa7c2e4f1, 0x3b5d, 0x4890, { 0x9a, 0x1c, 0x2e, 0x3f, 0x4d, 0x5e, 0x6f, 0x70 } };

// cfg_ayumi_play_indefinitely {3D9FAE64-8C25-4D95-9E73-AC3A492EE9C1}
static const GUID guid_cfg_play_indefinitely =
{ 0x3d9fae64, 0x8c25, 0x4d95, { 0x9e, 0x73, 0xac, 0x3a, 0x49, 0x2e, 0xe9, 0xc1 } };

// Preferences page GUID  {B8D3F502-4C6E-5901-AB2D-3F4E5F607182}
static const GUID guid_prefs_ayumi =
{ 0xb8d3f502, 0x4c6e, 0x5901, { 0xab, 0x2d, 0x3f, 0x4e, 0x5f, 0x60, 0x71, 0x82 } };

// ---------------------------------------------------------------------------
// Config variable  (stored as whole milliseconds)
// ---------------------------------------------------------------------------

static constexpr unsigned kDefaultDurationMs = 180000; // 3:00

cfg_int cfg_ayumi_duration_ms(guid_cfg_duration, (int)kDefaultDurationMs);
cfg_bool cfg_ayumi_play_indefinitely(guid_cfg_play_indefinitely, false);

// Accessor used by the decoder
unsigned GetAyumiDurationMs()
{
    int v = (int)cfg_ayumi_duration_ms;
    return (v > 0) ? (unsigned)v : kDefaultDurationMs;
}

bool GetAyumiPlayIndefinitely()
{
    return cfg_ayumi_play_indefinitely;
}

// ---------------------------------------------------------------------------
// M:SS helpers
// ---------------------------------------------------------------------------

// Format milliseconds → "M:SS" string (e.g. 180000 → "3:00")
static void FormatMSS(unsigned ms, wchar_t* buf, int bufLen)
{
    unsigned totalSec = ms / 1000;
    unsigned m  = totalSec / 60;
    unsigned s  = totalSec % 60;
    wsprintfW(buf, L"%u:%02u", m, s);
}

// Parse "M:SS" string → milliseconds.  Returns 0 on invalid input.
static unsigned ParseMSS(const wchar_t* str)
{
    if (!str || !str[0]) return 0;

    // Find ':'
    const wchar_t* colon = str;
    while (*colon && *colon != L':') ++colon;

    if (!*colon) return 0; // no colon

    // Minutes: everything before ':'
    unsigned m = 0;
    for (const wchar_t* p = str; p < colon; ++p)
    {
        if (*p < L'0' || *p > L'9') return 0;
        m = m * 10 + (*p - L'0');
    }

    // Seconds: everything after ':'
    const wchar_t* secStr = colon + 1;
    if (!secStr[0]) return 0;
    unsigned s = 0;
    for (const wchar_t* p = secStr; *p; ++p)
    {
        if (*p < L'0' || *p > L'9') return 0;
        s = s * 10 + (*p - L'0');
    }

    if (s >= 60) return 0;

    return (m * 60 + s) * 1000;
}

// ---------------------------------------------------------------------------
// Preferences dialog
// ---------------------------------------------------------------------------

class CAyumiPreferences : public CDialogImpl<CAyumiPreferences>,
                          public preferences_page_instance
{
public:
    explicit CAyumiPreferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
    {}

    enum { IDD = IDD_AYUMI_PREFERENCES };

    // --- preferences_page_instance ---

    t_uint32 get_state() override
    {
        t_uint32 state = preferences_state::resettable
                       | preferences_state::dark_mode_supported;
        if (HasChanged())
            state |= preferences_state::changed;
        return state;
    }

    void apply() override
    {
        unsigned ms = GetEditMs();
        if (ms > 0)
            cfg_ayumi_duration_ms = (int)ms;
        cfg_ayumi_play_indefinitely =
            (::IsDlgButtonChecked(m_hWnd, IDC_CHECK_PLAY_INDEFINITELY) == BST_CHECKED);
        m_callback->on_state_changed();

        // Force foobar2000 to re-read track info for all Ayumi files
        // so the new duration is reflected immediately in the playlist.
        try {
            metadb_handle_list affected;

            // Collect from Media Library
            struct AyumiEnumCB : library_manager::enum_callback {
                metadb_handle_list& items;
                AyumiEnumCB(metadb_handle_list& l) : items(l) {}
                bool on_item(const metadb_handle_ptr& p_item) override {
                    const char* ext = pfc::string_extension(p_item->get_path());
                    if (ext && (
                        _stricmp(ext, "fxm") == 0 ||
                        _stricmp(ext, "amad") == 0))
                        items.add_item(p_item);
                    return true;
                }
            } cb(affected);
            library_manager::get()->enum_items(cb);

            // Also collect from all playlists (covers items not in library)
            auto pm = playlist_manager::get();
            t_size playlistCount = pm->get_playlist_count();
            for (t_size p = 0; p < playlistCount; ++p) {
                metadb_handle_list pl;
                pm->playlist_get_all_items(p, pl);
                for (t_size i = 0; i < pl.get_count(); ++i) {
                    const char* ext = pfc::string_extension(pl[i]->get_path());
                    if (ext && (
                        _stricmp(ext, "fxm") == 0 ||
                        _stricmp(ext, "amad") == 0))
                        affected.add_item(pl[i]);
                }
            }

            affected.remove_duplicates();

            if (affected.get_count() > 0)
                metadb_io_v2::get()->load_info_async(
                    affected,
                    metadb_io_v2::load_info_force,
                    core_api::get_main_window(),
                    metadb_io_v2::op_flag_silent,
                    nullptr);
        } catch (...) {}
    }

    void reset() override
    {
        SetEditFromMs(kDefaultDurationMs);
        ::CheckDlgButton(m_hWnd, IDC_CHECK_PLAY_INDEFINITELY, BST_UNCHECKED);
        m_callback->on_state_changed();
    }

    // --- ATL message map ---

    BEGIN_MSG_MAP_EX(CAyumiPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_EDIT_DURATION, EN_CHANGE, OnEditChanged)
        COMMAND_HANDLER_EX(IDC_CHECK_PLAY_INDEFINITELY, BN_CLICKED, OnIndefiniteChanged)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM)
    {
        SetEditFromMs(GetAyumiDurationMs());
        ::CheckDlgButton(
            m_hWnd,
            IDC_CHECK_PLAY_INDEFINITELY,
            GetAyumiPlayIndefinitely() ? BST_CHECKED : BST_UNCHECKED);
        m_dark.AddDialogWithControls(*this);
        return FALSE;
    }

    void OnEditChanged(UINT, int, CWindow)
    {
        m_callback->on_state_changed();
    }

    void OnIndefiniteChanged(UINT, int, CWindow)
    {
        m_callback->on_state_changed();
    }

    // Read the edit box, parse M:SS → ms.  Returns 0 if invalid.
    unsigned GetEditMs() const
    {
        wchar_t buf[32] = {};
        ::GetDlgItemTextW(m_hWnd, IDC_EDIT_DURATION, buf, _countof(buf));
        return ParseMSS(buf);
    }

    void SetEditFromMs(unsigned ms)
    {
        wchar_t buf[32];
        FormatMSS(ms, buf, _countof(buf));
        ::SetDlgItemTextW(m_hWnd, IDC_EDIT_DURATION, buf);
    }

    bool HasChanged() const
    {
        unsigned ms = GetEditMs();
        const bool durationChanged = (ms > 0 && ms != (unsigned)(int)cfg_ayumi_duration_ms);
        const bool indefiniteChanged =
            (::IsDlgButtonChecked(m_hWnd, IDC_CHECK_PLAY_INDEFINITELY) == BST_CHECKED)
            != (bool)cfg_ayumi_play_indefinitely;
        return durationChanged || indefiniteChanged;
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CCoreDarkModeHooks m_dark;
};

// ---------------------------------------------------------------------------
// Register under Playback > Decoding
// ---------------------------------------------------------------------------

class prefs_page_ayumi : public preferences_page_impl<CAyumiPreferences>
{
public:
    const char* get_name() override { return "Ayumi"; }
    GUID        get_guid() override { return guid_prefs_ayumi; }
    GUID        get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<prefs_page_ayumi> g_prefs_ayumi_factory;
