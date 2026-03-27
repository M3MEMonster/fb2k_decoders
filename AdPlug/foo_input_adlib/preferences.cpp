#include "stdafx.h"
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>
#include "resource.h"
#include "adlib_config.h"

// ---------------------------------------------------------------------------
// GUIDs
// ---------------------------------------------------------------------------

// cfg_adlib_samplerate  {F1E2D3C4-B5A6-4978-8192-A3B4C5D6E7F8}
static const GUID guid_cfg_samplerate =
{ 0xf1e2d3c4, 0xb5a6, 0x4978, { 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 } };

// cfg_adlib_core        {E2D3C4B5-A697-5869-92A3-B4C5D6E7F809}
static const GUID guid_cfg_core =
{ 0xe2d3c4b5, 0xa697, 0x5869, { 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09 } };

// cfg_adlib_equalizer   {D3C4B5A6-9708-675A-A3B4-C5D6E7F8091A}
static const GUID guid_cfg_equalizer =
{ 0xd3c4b5a6, 0x9708, 0x675a, { 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a } };

// cfg_adlib_surround    {A6978B50-601B-941D-D5E6-F7081A2B3C4D}
static const GUID guid_cfg_surround =
{ 0xa6978b50, 0x601b, 0x941d, { 0xd5, 0xe6, 0xf7, 0x08, 0x1a, 0x2b, 0x3c, 0x4d } };

// Preferences page GUID {C4B5A697-8019-762B-B4C5-D6E7F8091A2B}
static const GUID guid_prefs_adlib =
{ 0xc4b5a697, 0x8019, 0x762b, { 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a, 0x2b } };

//---------------------------------------------------------------------------
// Persistent config variables
// ---------------------------------------------------------------------------

cfg_int  cfg_adlib_samplerate(guid_cfg_samplerate, kAdLibDefaultSampleRateIdx);
cfg_int  cfg_adlib_core      (guid_cfg_core,        kAdLibDefaultCoreIdx);
cfg_int  cfg_adlib_equalizer (guid_cfg_equalizer,   kAdLibDefaultEqualizerIdx);
cfg_bool cfg_adlib_surround  (guid_cfg_surround,    kAdLibDefaultSurround);

// ---------------------------------------------------------------------------
// Sample rate table  (must match the combo order in OnInitDialog)
// ---------------------------------------------------------------------------

static const unsigned kSampleRates[] =
{
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 49716, 64000, 88200, 96000
};
static constexpr int kSampleRateCount = (int)(sizeof(kSampleRates) / sizeof(kSampleRates[0]));

unsigned GetAdLibSampleRate()
{
    int idx = (int)cfg_adlib_samplerate;
    if (idx < 0 || idx >= kSampleRateCount)
        idx = kAdLibDefaultSampleRateIdx;
    return kSampleRates[idx];
}

// ---------------------------------------------------------------------------
// Preferences dialog
// ---------------------------------------------------------------------------

class CAdLibPreferences : public CDialogImpl<CAdLibPreferences>,
                          public preferences_page_instance
{
public:
    explicit CAdLibPreferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
    {}

    enum { IDD = IDD_ADLIB_PREFERENCES };

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
        cfg_adlib_samplerate = GetComboSel(IDC_COMBO_SAMPLERATE);
        cfg_adlib_core       = GetComboSel(IDC_COMBO_CORE);
        cfg_adlib_equalizer  = GetComboSel(IDC_COMBO_EQUALIZER);
        cfg_adlib_surround   = (GetCheckState(IDC_CHECK_SURROUND) == BST_CHECKED);
        m_callback->on_state_changed();
    }

    void reset() override
    {
        SetComboSel(IDC_COMBO_SAMPLERATE, kAdLibDefaultSampleRateIdx);
        SetComboSel(IDC_COMBO_CORE,       kAdLibDefaultCoreIdx);
        SetComboSel(IDC_COMBO_EQUALIZER,  kAdLibDefaultEqualizerIdx);
        SetCheckState(IDC_CHECK_SURROUND, kAdLibDefaultSurround ? BST_CHECKED : BST_UNCHECKED);
        m_callback->on_state_changed();
    }

    // --- ATL message map ---

    BEGIN_MSG_MAP_EX(CAdLibPreferences)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_HANDLER_EX(IDC_COMBO_SAMPLERATE, CBN_SELCHANGE, OnControlChanged)
        COMMAND_HANDLER_EX(IDC_COMBO_CORE,       CBN_SELCHANGE, OnControlChanged)
        COMMAND_HANDLER_EX(IDC_COMBO_EQUALIZER,  CBN_SELCHANGE, OnControlChanged)
        COMMAND_HANDLER_EX(IDC_CHECK_SURROUND,   BN_CLICKED,    OnControlChanged)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow, LPARAM)
    {
        // Sample rate combo
        for (int i = 0; i < kSampleRateCount; ++i)
        {
            wchar_t buf[32];
            wsprintfW(buf, L"%u Hz", kSampleRates[i]);
            SendDlgItemMessage(IDC_COMBO_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)buf);
        }

        // Core combo
        SendDlgItemMessage(IDC_COMBO_CORE, CB_ADDSTRING, 0, (LPARAM)L"Harekiet's");
        SendDlgItemMessage(IDC_COMBO_CORE, CB_ADDSTRING, 0, (LPARAM)L"Ken Silverman's");
        SendDlgItemMessage(IDC_COMBO_CORE, CB_ADDSTRING, 0, (LPARAM)L"Jarek Burczynski's");
        SendDlgItemMessage(IDC_COMBO_CORE, CB_ADDSTRING, 0, (LPARAM)L"Nuked OPL3");

        // Equalizer combo
        SendDlgItemMessage(IDC_COMBO_EQUALIZER, CB_ADDSTRING, 0, (LPARAM)L"ESS FM");
        SendDlgItemMessage(IDC_COMBO_EQUALIZER, CB_ADDSTRING, 0, (LPARAM)L"None");

        // Restore saved selections
        int srIdx   = (int)cfg_adlib_samplerate;
        int coreIdx = (int)cfg_adlib_core;
        int eqIdx   = (int)cfg_adlib_equalizer;

        if (srIdx   < 0 || srIdx   >= kSampleRateCount) srIdx   = kAdLibDefaultSampleRateIdx;
        if (coreIdx < 0 || coreIdx >= 4)                coreIdx = kAdLibDefaultCoreIdx;
        if (eqIdx   < 0 || eqIdx   >= 2)                eqIdx   = kAdLibDefaultEqualizerIdx;

        SetComboSel(IDC_COMBO_SAMPLERATE, srIdx);
        SetComboSel(IDC_COMBO_CORE,       coreIdx);
        SetComboSel(IDC_COMBO_EQUALIZER,  eqIdx);
        SetCheckState(IDC_CHECK_SURROUND,
            (bool)cfg_adlib_surround ? BST_CHECKED : BST_UNCHECKED);

        m_dark.AddDialogWithControls(*this);
        return FALSE;
    }

    void OnControlChanged(UINT, int, CWindow)
    {
        m_callback->on_state_changed();
    }

    // Helpers
    int GetComboSel(int id) const
    {
        return (int)::SendDlgItemMessage(m_hWnd, id, CB_GETCURSEL, 0, 0);
    }

    void SetComboSel(int id, int idx)
    {
        ::SendDlgItemMessage(m_hWnd, id, CB_SETCURSEL, (WPARAM)idx, 0);
    }

    LRESULT GetCheckState(int id) const
    {
        return ::SendDlgItemMessage(m_hWnd, id, BM_GETCHECK, 0, 0);
    }

    void SetCheckState(int id, int state)
    {
        ::SendDlgItemMessage(m_hWnd, id, BM_SETCHECK, (WPARAM)state, 0);
    }

    bool HasChanged() const
    {
        return GetComboSel(IDC_COMBO_SAMPLERATE) != (int)cfg_adlib_samplerate
            || GetComboSel(IDC_COMBO_CORE)       != (int)cfg_adlib_core
            || GetComboSel(IDC_COMBO_EQUALIZER)  != (int)cfg_adlib_equalizer
            || (GetCheckState(IDC_CHECK_SURROUND) == BST_CHECKED) != (bool)cfg_adlib_surround;
    }

    const preferences_page_callback::ptr m_callback;
    fb2k::CDarkModeHooks m_dark;
};

// ---------------------------------------------------------------------------
// AdLib page under the existing Playback > Decoding branch
// ---------------------------------------------------------------------------

class prefs_page_adlib : public preferences_page_impl<CAdLibPreferences>
{
public:
    const char* get_name() override { return "AdLib"; }
    GUID        get_guid() override { return guid_prefs_adlib; }
    // preferences_page::guid_input is the SDK GUID for the built-in
    // "Decoding" branch under Playback — no branch factory needed.
    GUID        get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<prefs_page_adlib> g_prefs_adlib_factory;
