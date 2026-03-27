#include "stdafx.h"

#ifdef _WIN32

#include "resource.h"
#include "xmp_config.h"

#include <SDK/cfg_var.h>
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>
#include <libPPUI/CListControlOwnerData.h>
#include <libPPUI/CListControl-Cells.h>

#include <algorithm>
#include <set>
#include <string>
#include <sstream>

// ============================================================================
// Persistent config: semicolon-delimited list of disabled extension keys
// ============================================================================

// {F1A2B3C4-D5E6-4F70-8192-A3B4C5D6E7F8}
static constexpr GUID guid_cfg_disabled = {
    0xf1a2b3c4, 0xd5e6, 0x4f70,
    { 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 }
};

static cfg_string cfg_disabled(guid_cfg_disabled, "");

static std::set<std::string> parse_set(const char* s) {
    std::set<std::string> r;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ';'))
        if (!tok.empty()) r.insert(tok);
    return r;
}

static std::string serialize_set(const std::set<std::string>& s) {
    std::string out;
    for (auto& e : s) {
        if (!out.empty()) out += ';';
        out += e;
    }
    return out;
}

// ============================================================================
// Format table — every file extension we advertise, sorted alphabetically
// ============================================================================

struct fmt_desc { const char* key; const char* label; };

static const fmt_desc g_fmts[] = {
    { "669",   "669   - Composer 669 / UNIS 669" },
    { "abk",   "ABK   - AMOS Music Bank" },
    { "amf",   "AMF   - ASYLUM / DSMI Advanced Module Format" },
    { "arch",  "ARCH  - Archimedes Tracker" },
    { "coco",  "COCO  - Coconizer" },
    { "dbm",   "DBM   - DigiBooster Pro" },
    { "digi",  "DIGI  - DIGI Booster" },
    { "dsym",  "DSYM  - Digital Symphony" },
    { "dt",    "DT    - Digital Tracker" },
    { "dtm",   "DTM   - Digital Tracker" },
    { "emod",  "EMOD  - Quadra Composer" },
    { "far",   "FAR   - Farandole Composer" },
    { "flt",   "FLT   - Startrekker" },
    { "flx",   "FLX   - Flextrax" },
    { "fnk",   "FNK   - Funktracker" },
    { "gdm",   "GDM   - General DigiMusic" },
    { "hmn",   "HMN   - His Master's Noise" },
    { "ice",   "ICE   - Ice Tracker / Soundtracker 2.6" },
    { "imf",   "IMF   - Imago Orpheus" },
    { "ims",   "IMS   - Images Music System" },
    { "it",    "IT    - Impulse Tracker" },
    { "j2b",   "J2B   - Galaxy Music System 5.0" },
    { "liq",   "LIQ   - Liquid Tracker" },
    { "m15",   "M15   - Ultimate Soundtracker (15-instrument)" },
    { "mdl",   "MDL   - Digitrakker" },
    { "med",   "MED   - OctaMED" },
    { "mfp",   "MFP   - Magnetic Fields Packer" },
    { "mgt",   "MGT   - Megatracker" },
    { "mmd",   "MMD   - OctaMED (generic)" },
    { "mmd0",  "MMD0  - OctaMED (MMD0)" },
    { "mmd1",  "MMD1  - OctaMED (MMD1)" },
    { "mmd2",  "MMD2  - OctaMED (MMD2)" },
    { "mmd3",  "MMD3  - OctaMED (MMD3)" },
    { "mod",   "MOD   - ProTracker / NoiseTracker" },
    { "mtn",   "MTN   - Soundtracker 2.6 / Ice Tracker" },
    { "mtm",   "MTM   - MultiTracker" },
    { "musx",  "MUSX  - Archimedes Tracker" },
    { "nst",   "NST   - NoiseTracker" },
    { "okt",   "OKT   - Oktalyzer" },
    { "ps16",  "PS16  - Epic MegaGames MASI 16" },
    { "psm",   "PSM   - Epic MegaGames MASI / Protracker Studio" },
    { "pt3",   "PT3   - Protracker 3" },
    { "pt36",  "PT36  - Protracker 3.6" },
    { "ptm",   "PTM   - Poly Tracker" },
    { "rtm",   "RTM   - Real Tracker" },
    { "s3m",   "S3M   - Scream Tracker 3" },
    { "sfx",   "SFX   - SoundFX" },
    { "stim",  "STIM  - Slamtilt" },
    { "stk",   "STK   - Soundtracker" },
    { "stm",   "STM   - Scream Tracker 2" },
    { "stx",   "STX   - STMIK 0.2" },
    { "sym",   "SYM   - Digital Symphony" },
    { "ult",   "ULT   - UltraTracker" },
    { "umx",   "UMX   - Unreal Music Package" },
    { "wow",   "WOW   - Mod's Grave" },
    { "xm",    "XM    - FastTracker II" },
    { "xmf",   "XMF   - Astroidea XMF" },
};

static constexpr int NUM_FMTS = (int)(sizeof(g_fmts) / sizeof(g_fmts[0]));

// ============================================================================
// Public query used by input_xmp
// ============================================================================

namespace xmp_config {

bool is_format_enabled(const char* ext) {
    if (!ext) return false;

    std::string e(ext);
    for (auto& c : e) c = (char)tolower((unsigned char)c);

    bool known = false;
    for (int i = 0; i < NUM_FMTS; i++) {
        if (e == g_fmts[i].key) { known = true; break; }
    }
    if (!known) return false;

    auto dis = parse_set(cfg_disabled.get_ptr());
    return dis.find(e) == dis.end();
}

} // namespace xmp_config

// ============================================================================
// Preferences dialog
// ============================================================================

class CXMPPrefs : public CDialogImpl<CXMPPrefs>,
                  public preferences_page_instance,
                  private IListControlOwnerDataSource,
                  private IListControlOwnerDataCells {
public:
    enum { IDD = IDD_XMP_PREFS };

    CXMPPrefs(preferences_page_callback::ptr cb)
        : m_cb(cb)
        , m_list(static_cast<IListControlOwnerDataSource*>(this),
                 static_cast<IListControlOwnerDataCells*>(this))
    {}

    fb2k::hwnd_t get_wnd() override { return m_hWnd; }

    t_uint32 get_state() override {
        t_uint32 s = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (m_dis != m_saved) s |= preferences_state::changed;
        return s;
    }

    void apply() override {
        cfg_disabled = serialize_set(m_dis).c_str();
        m_saved = m_dis;
        m_cb->on_state_changed();
    }

    void reset() override {
        m_dis.clear();
        m_saved = m_dis;
        m_list.ReloadData();
        m_cb->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CXMPPrefs)
        MSG_WM_INITDIALOG(OnInit)
    END_MSG_MAP()

private:
    BOOL OnInit(CWindow, LPARAM) {
        m_list.CreateInDialog(*this, IDC_FORMAT_LIST);
        m_dark.AddDialogWithControls(*this);

        auto DPI = m_list.GetDPI();
        m_list.AddColumn("On",        MulDiv(36,  DPI.cx, 96));
        m_list.AddColumn("Extension", MulDiv(55,  DPI.cx, 96));
        m_list.AddColumn("Format",    MulDiv(380, DPI.cx, 96));

        m_dis   = parse_set(cfg_disabled.get_ptr());
        m_saved = m_dis;

        m_list.ReloadData();
        return FALSE;
    }

    // IListControlOwnerDataSource
    size_t listGetItemCount(ctx_t) override { return (size_t)NUM_FMTS; }
    pfc::string8 listGetSubItemText(ctx_t, size_t item, size_t subItem) override {
        if (item >= (size_t)NUM_FMTS) return "";
        switch (subItem) {
        case 0: return "";
        case 1: return g_fmts[item].key;
        case 2: return g_fmts[item].label;
        default: return "";
        }
    }
    void listSubItemClicked(ctx_t, size_t, size_t) override {}

    // IListControlOwnerDataCells
    CListControl::cellType_t listCellType(cellsCtx_t, size_t, size_t subItem) override {
        if (subItem == 0) return &PFC_SINGLETON(CListCell_Checkbox);
        return &PFC_SINGLETON(CListCell_Text);
    }
    bool listCellCheckState(cellsCtx_t, size_t item, size_t subItem) override {
        if (subItem == 0 && item < (size_t)NUM_FMTS)
            return m_dis.find(g_fmts[item].key) == m_dis.end();
        return false;
    }
    void listCellSetCheckState(cellsCtx_t, size_t item, size_t subItem, bool state) override {
        if (subItem == 0 && item < (size_t)NUM_FMTS) {
            if (state) m_dis.erase(g_fmts[item].key);
            else       m_dis.insert(g_fmts[item].key);
            m_cb->on_state_changed();
        }
    }

    preferences_page_callback::ptr m_cb;
    fb2k::CDarkModeHooks m_dark;
    CListControlOwnerDataCells m_list;
    std::set<std::string> m_dis, m_saved;
};

// ============================================================================
// Page registration under Preferences -> Playback -> Decoding
// ============================================================================

class prefs_page_xmp : public preferences_page_v3 {
public:
    const char* get_name() override { return "libXMP"; }

    GUID get_guid() override {
        return { 0xa2b3c4d5, 0xe6f7, 0x4890,
                 { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0xa7, 0xb8 } };
    }

    GUID get_parent_guid() override { return preferences_page::guid_input; }

    preferences_page_instance::ptr instantiate(
            HWND parent, preferences_page_callback::ptr cb) override {
        auto* d = new service_impl_t<CXMPPrefs>(cb);
        d->Create(parent);
        return d;
    }
};

static preferences_page_factory_t<prefs_page_xmp> g_prefs;

#endif // _WIN32
