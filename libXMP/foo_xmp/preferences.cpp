#include "stdafx.h"

#ifdef _WIN32

#include "resource.h"
#include "xmp_config.h"

#include <SDK/cfg_var.h>

#include <algorithm>
#include <set>
#include <string>
#include <sstream>
#include <commctrl.h>

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
                  public preferences_page_instance {
public:
    enum { IDD = IDD_XMP_PREFS };

    CXMPPrefs(preferences_page_callback::ptr cb) : m_cb(cb) {}

    fb2k::hwnd_t get_wnd() override { return m_hWnd; }

    t_uint32 get_state() override {
        t_uint32 s = preferences_state::resettable;
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
        m_init = true;
        RefreshChecks();
        m_init = false;
        m_cb->on_state_changed();
    }

    BEGIN_MSG_MAP_EX(CXMPPrefs)
        MSG_WM_INITDIALOG(OnInit)
        NOTIFY_HANDLER_EX(IDC_FORMAT_LIST, LVN_ITEMCHANGED, OnChanged)
    END_MSG_MAP()

private:
    BOOL OnInit(CWindow, LPARAM) {
        m_lv = GetDlgItem(IDC_FORMAT_LIST);
        ListView_SetExtendedListViewStyle(m_lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN col{};
        col.mask = LVCF_WIDTH | LVCF_TEXT;
        col.cx = 600;
        col.pszText = const_cast<LPWSTR>(L"Format");
        ListView_InsertColumn(m_lv, 0, &col);

        m_dis   = parse_set(cfg_disabled.get_ptr());
        m_saved = m_dis;

        m_init = true;
        for (int i = 0; i < NUM_FMTS; i++) {
            pfc::stringcvt::string_wide_from_utf8 wl(g_fmts[i].label);
            LVITEMW it{};
            it.mask    = LVIF_TEXT;
            it.iItem   = i;
            it.pszText = const_cast<LPWSTR>(wl.get_ptr());
            ListView_InsertItem(m_lv, &it);
            ListView_SetCheckState(m_lv, i,
                m_dis.find(g_fmts[i].key) == m_dis.end() ? TRUE : FALSE);
        }
        m_init = false;

        RECT rc;
        ::GetClientRect(m_lv, &rc);
        ListView_SetColumnWidth(m_lv, 0,
            rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL));
        return FALSE;
    }

    LRESULT OnChanged(LPNMHDR ph) {
        if (m_init) return 0;
        auto* p = reinterpret_cast<LPNMLISTVIEW>(ph);
        if (!(p->uChanged & LVIF_STATE)) return 0;
        if (!((p->uNewState ^ p->uOldState) & LVIS_STATEIMAGEMASK)) return 0;

        int i = p->iItem;
        if (i < 0 || i >= NUM_FMTS) return 0;

        if (ListView_GetCheckState(m_lv, i))
            m_dis.erase(g_fmts[i].key);
        else
            m_dis.insert(g_fmts[i].key);

        m_cb->on_state_changed();
        return 0;
    }

    void RefreshChecks() {
        for (int i = 0; i < NUM_FMTS; i++)
            ListView_SetCheckState(m_lv, i,
                m_dis.find(g_fmts[i].key) == m_dis.end() ? TRUE : FALSE);
    }

    preferences_page_callback::ptr m_cb;
    HWND m_lv = nullptr;
    bool m_init = false;
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
