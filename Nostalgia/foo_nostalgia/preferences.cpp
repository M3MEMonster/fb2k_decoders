#include <helpers/foobar2000+atl.h>
#include "cfg_vars.h"
#include "resource.h"

#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>
#include <libPPUI/CListControlOwnerData.h>
#include <libPPUI/CListControl-Cells.h>

namespace {

struct DecoderEntry {
    cfg_bool* cfgVar;
    const char* id;
    const char* description;
};

static DecoderEntry g_entries[] = {
    { &nostalgia_cfg::cfg_goattracker_enabled,  ".sng",      "GoatTracker 2.77 (C64 SID tracker)" },
    { &nostalgia_cfg::cfg_protrekkr_enabled,    ".ptk",      "ProTrekkr 2.8.2 (modular tracker with TB-303 emulation)" },
    { &nostalgia_cfg::cfg_megatracker_enabled,  "(content)", "MegaTracker Player 4.3.1 (Apple IIgs: SoundSmith, NoiseTracker GS, SMUS)" },
    { &nostalgia_cfg::cfg_skaletracker_enabled, ".skm",      "Skale Tracker 0.81 (Windows tracker)" },
    { &nostalgia_cfg::cfg_klystrack_enabled,    ".kt",       "Klystrack module (via libksnd)" },
    { &nostalgia_cfg::cfg_monotone_enabled,    ".mon",      "MONOTONE module (SoLoud monotone format)" },
    { &nostalgia_cfg::cfg_facsoundtracker_enabled, ".mus",   "FAC SoundTracker module (.mus, OPL2 synthesis)" },
};
static constexpr size_t kNumEntries = _countof(g_entries);

class CNostalgiaPreferences : public CDialogImpl<CNostalgiaPreferences>,
                                 public preferences_page_instance,
                                 private IListControlOwnerDataSource,
                                 private IListControlOwnerDataCells
{
public:
    CNostalgiaPreferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
        , m_list(static_cast<IListControlOwnerDataSource*>(this),
                 static_cast<IListControlOwnerDataCells*>(this))
    {}

    enum { IDD = IDD_PREFERENCES };

    t_uint32 get_state() override;
    void apply() override;
    void reset() override;

    BEGIN_MSG_MAP_EX(CNostalgiaPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_DEFAULT_DURATION_EDIT, EN_CHANGE, OnDurationChange)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM);
    void OnDurationChange(UINT, int, CWindow);
    bool HasChanged();
    void OnChanged();

    size_t listGetItemCount(ctx_t) override { return kNumEntries; }
    pfc::string8 listGetSubItemText(ctx_t, size_t item, size_t subItem) override;
    void listSubItemClicked(ctx_t, size_t, size_t) override {}

    CListControl::cellType_t listCellType(cellsCtx_t, size_t item, size_t subItem) override;
    bool listCellCheckState(cellsCtx_t, size_t item, size_t subItem) override;
    void listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state) override;

    const preferences_page_callback::ptr m_callback;
    fb2k::CDarkModeHooks m_dark;
    CListControlOwnerDataCells m_list;
    bool m_localState[kNumEntries] = {};
    int m_localDuration = 180;
};

BOOL CNostalgiaPreferences::OnInitDialog(CWindow, LPARAM)
{
    m_list.CreateInDialog(*this, IDC_LIST);
    m_dark.AddDialogWithControls(*this);

    auto DPI = m_list.GetDPI();
    m_list.AddColumn("On",          MulDiv(36, DPI.cx, 96));
    m_list.AddColumn("ID",          MulDiv(70, DPI.cx, 96));
    m_list.AddColumn("Description", MulDiv(400, DPI.cx, 96));

    for (size_t i = 0; i < kNumEntries; i++)
        m_localState[i] = g_entries[i].cfgVar->get();

    m_localDuration = nostalgia_cfg::cfg_default_duration_sec.get();
    SetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, m_localDuration, FALSE);

    m_list.ReloadData();
    return FALSE;
}

void CNostalgiaPreferences::OnDurationChange(UINT, int, CWindow)
{
    BOOL ok = FALSE;
    int val = (int)GetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, &ok, FALSE);
    if (ok && val > 0)
    {
        m_localDuration = val;
        OnChanged();
    }
}

pfc::string8 CNostalgiaPreferences::listGetSubItemText(ctx_t, size_t item, size_t subItem)
{
    if (item >= kNumEntries) return "";
    switch (subItem) {
    case 0: return "";
    case 1: return g_entries[item].id;
    case 2: return g_entries[item].description;
    default: return "";
    }
}

CListControl::cellType_t CNostalgiaPreferences::listCellType(cellsCtx_t, size_t, size_t subItem)
{
    if (subItem == 0)
        return &PFC_SINGLETON(CListCell_Checkbox);
    return &PFC_SINGLETON(CListCell_Text);
}

bool CNostalgiaPreferences::listCellCheckState(cellsCtx_t, size_t item, size_t subItem)
{
    if (subItem == 0 && item < kNumEntries)
        return m_localState[item];
    return false;
}

void CNostalgiaPreferences::listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state)
{
    if (subItem == 0 && item < kNumEntries)
    {
        m_localState[item] = state;
        OnChanged();
    }
}

t_uint32 CNostalgiaPreferences::get_state()
{
    t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
    if (HasChanged()) state |= preferences_state::changed;
    return state;
}

void CNostalgiaPreferences::apply()
{
    bool durationChanged = (m_localDuration != nostalgia_cfg::cfg_default_duration_sec.get());

    for (size_t i = 0; i < kNumEntries; i++)
        *g_entries[i].cfgVar = m_localState[i];
    if (m_localDuration > 0)
        nostalgia_cfg::cfg_default_duration_sec = m_localDuration;
    OnChanged();

    if (durationChanged)
    {
        try {
            static_api_ptr_t<playlist_manager> pm;
            metadb_handle_list allItems;
            t_size plCount = pm->get_playlist_count();
            for (t_size p = 0; p < plCount; p++)
            {
                metadb_handle_list pl;
                pm->playlist_get_all_items(p, pl);
                allItems.add_items(pl);
            }

            metadb_handle_list filtered;
            for (t_size i = 0; i < allItems.get_count(); i++)
            {
                const char* path = allItems[i]->get_path();
                const char* slash = strrchr(path, '/');
                if (!slash) slash = strrchr(path, '\\');
                const char* filename = slash ? slash + 1 : path;
                const char* dot = strrchr(filename, '.');
                if (dot)
                {
                    dot++;
                    if (stricmp_utf8(dot, "sng") == 0 ||
                        stricmp_utf8(dot, "ptk") == 0 ||
                        stricmp_utf8(dot, "skm") == 0 ||
                        stricmp_utf8(dot, "kt") == 0 ||
                        stricmp_utf8(dot, "mon") == 0 ||
                        stricmp_utf8(dot, "mus") == 0)
                    {
                        filtered.add_item(allItems[i]);
                    }
                }
                else
                {
                    filtered.add_item(allItems[i]);
                }
            }

            if (filtered.get_count() > 0)
            {
                static_api_ptr_t<metadb_io_v2>()->load_info_async(
                    filtered,
                    metadb_io_v2::load_info_force,
                    core_api::get_main_window(),
                    metadb_io_v2::op_flag_delay_ui | metadb_io_v2::op_flag_background,
                    nullptr);
            }
        } catch (...) {}
    }
}

void CNostalgiaPreferences::reset()
{
    for (size_t i = 0; i < kNumEntries; i++)
        m_localState[i] = true;
    m_localDuration = 180;
    SetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, m_localDuration, FALSE);
    m_list.ReloadData();
    OnChanged();
}

bool CNostalgiaPreferences::HasChanged()
{
    for (size_t i = 0; i < kNumEntries; i++)
    {
        if (m_localState[i] != g_entries[i].cfgVar->get())
            return true;
    }
    if (m_localDuration != nostalgia_cfg::cfg_default_duration_sec.get())
        return true;
    return false;
}

void CNostalgiaPreferences::OnChanged()
{
    m_callback->on_state_changed();
}

static constexpr GUID guid_prefs_page = { 0x5e6f7081, 0x9203, 0x4a45, { 0xb6, 0xc7, 0xd8, 0xe9, 0xfa, 0x0b, 0x1c, 0x2d } };

class preferences_page_nostalgia : public preferences_page_impl<CNostalgiaPreferences> {
public:
    const char* get_name() override { return "Nostalgia"; }
    GUID get_guid() override { return guid_prefs_page; }
    GUID get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<preferences_page_nostalgia> g_preferences_factory;

} // anonymous namespace
