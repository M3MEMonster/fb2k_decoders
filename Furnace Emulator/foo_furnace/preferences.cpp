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
    { &furnace_cfg::cfg_fur_enabled, ".fur", "Furnace Tracker native format" },
    { &furnace_cfg::cfg_dmf_enabled, ".dmf", "DefleMask format" },
    { &furnace_cfg::cfg_ftm_enabled, ".ftm", "FamiTracker format" },
    { &furnace_cfg::cfg_dnm_enabled, ".dnm", "DefleMask-converted FamiTracker" },
    { &furnace_cfg::cfg_0cc_enabled, ".0cc", "0CC-FamiTracker variant" },
    { &furnace_cfg::cfg_eft_enabled, ".eft", "Enhanced FamiTracker variant" },
    { &furnace_cfg::cfg_tfm_enabled, ".tfm", "TurboSound FM format" },
    { &furnace_cfg::cfg_tfe_enabled, ".tfe", "TurboSound FM Extended format" },
};
static constexpr size_t kNumEntries = _countof(g_entries);

class CFurnacePreferences : public CDialogImpl<CFurnacePreferences>,
                            public preferences_page_instance,
                            private IListControlOwnerDataSource,
                            private IListControlOwnerDataCells
{
public:
    CFurnacePreferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
        , m_list(static_cast<IListControlOwnerDataSource*>(this),
                 static_cast<IListControlOwnerDataCells*>(this))
    {}

    enum { IDD = IDD_PREFERENCES };

    t_uint32 get_state() override;
    void apply() override;
    void reset() override;

    BEGIN_MSG_MAP_EX(CFurnacePreferences)
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

BOOL CFurnacePreferences::OnInitDialog(CWindow, LPARAM)
{
    m_list.CreateInDialog(*this, IDC_LIST);
    m_dark.AddDialogWithControls(*this);

    auto DPI = m_list.GetDPI();
    m_list.AddColumn("On",          MulDiv(36, DPI.cx, 96));
    m_list.AddColumn("ID",          MulDiv(50, DPI.cx, 96));
    m_list.AddColumn("Description", MulDiv(400, DPI.cx, 96));

    for (size_t i = 0; i < kNumEntries; i++)
        m_localState[i] = g_entries[i].cfgVar->get();

    m_localDuration = furnace_cfg::cfg_default_duration_sec.get();
    SetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, m_localDuration, FALSE);

    m_list.ReloadData();
    return FALSE;
}

void CFurnacePreferences::OnDurationChange(UINT, int, CWindow)
{
    BOOL ok = FALSE;
    int val = (int)GetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, &ok, FALSE);
    if (ok && val > 0)
    {
        m_localDuration = val;
        OnChanged();
    }
}

pfc::string8 CFurnacePreferences::listGetSubItemText(ctx_t, size_t item, size_t subItem)
{
    if (item >= kNumEntries) return "";
    switch (subItem) {
    case 0: return "";
    case 1: return g_entries[item].id;
    case 2: return g_entries[item].description;
    default: return "";
    }
}

CListControl::cellType_t CFurnacePreferences::listCellType(cellsCtx_t, size_t, size_t subItem)
{
    if (subItem == 0)
        return &PFC_SINGLETON(CListCell_Checkbox);
    return &PFC_SINGLETON(CListCell_Text);
}

bool CFurnacePreferences::listCellCheckState(cellsCtx_t, size_t item, size_t subItem)
{
    if (subItem == 0 && item < kNumEntries)
        return m_localState[item];
    return false;
}

void CFurnacePreferences::listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state)
{
    if (subItem == 0 && item < kNumEntries)
    {
        m_localState[item] = state;
        OnChanged();
    }
}

t_uint32 CFurnacePreferences::get_state()
{
    t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
    if (HasChanged()) state |= preferences_state::changed;
    return state;
}

void CFurnacePreferences::apply()
{
    bool durationChanged = (m_localDuration != furnace_cfg::cfg_default_duration_sec.get());

    for (size_t i = 0; i < kNumEntries; i++)
        *g_entries[i].cfgVar = m_localState[i];
    if (m_localDuration > 0)
        furnace_cfg::cfg_default_duration_sec = m_localDuration;
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
                    if (stricmp_utf8(dot, "fur") == 0 ||
                        stricmp_utf8(dot, "dmf") == 0 ||
                        stricmp_utf8(dot, "ftm") == 0 ||
                        stricmp_utf8(dot, "dnm") == 0 ||
                        stricmp_utf8(dot, "0cc") == 0 ||
                        stricmp_utf8(dot, "eft") == 0 ||
                        stricmp_utf8(dot, "tfm") == 0 ||
                        stricmp_utf8(dot, "tfe") == 0)
                    {
                        filtered.add_item(allItems[i]);
                    }
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

void CFurnacePreferences::reset()
{
    for (size_t i = 0; i < kNumEntries; i++)
        m_localState[i] = true;
    m_localDuration = 180;
    SetDlgItemInt(IDC_DEFAULT_DURATION_EDIT, m_localDuration, FALSE);
    m_list.ReloadData();
    OnChanged();
}

bool CFurnacePreferences::HasChanged()
{
    for (size_t i = 0; i < kNumEntries; i++)
    {
        if (m_localState[i] != g_entries[i].cfgVar->get())
            return true;
    }
    if (m_localDuration != furnace_cfg::cfg_default_duration_sec.get())
        return true;
    return false;
}

void CFurnacePreferences::OnChanged()
{
    m_callback->on_state_changed();
}

// {7A3B4C5D-6E7F-4809-1A2B-3C4D5E6F7081}
static constexpr GUID guid_prefs_page = { 0x7a3b4c5d, 0x6e7f, 0x4809, { 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81 } };

class preferences_page_furnace : public preferences_page_impl<CFurnacePreferences> {
public:
    const char* get_name() override { return "Furnace Emulator"; }
    GUID get_guid() override { return guid_prefs_page; }
    GUID get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<preferences_page_furnace> g_preferences_factory;

} // anonymous namespace
