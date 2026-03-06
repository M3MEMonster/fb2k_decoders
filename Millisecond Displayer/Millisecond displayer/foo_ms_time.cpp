#include "stdafx.h"
#include "resource.h"
#include <atomic>
#include <cstdio>

// ==========================================================================
// Component declaration
// ==========================================================================

DECLARE_COMPONENT_VERSION(
	"Millisecond Time Display", "1.0",
	"Provides millisecond-precision playback time and track length.\n\n"
	"UI element: embeddable panel with smooth ~60 fps updates.\n"
	"Title format fields (global, ~1 s resolution):\n"
	"  %playback_time_ms%  -  current playback time  [HH:]MM:SS.mmm\n"
	"  %length_ms%         -  track length            [HH:]MM:SS.mmm"
);

VALIDATE_COMPONENT_FILENAME("foo_ms_time.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;

namespace {

static const char * const k_default_pattern = "%playback_time_ms%[ / %length_ms%]";

// ==========================================================================
// Shared helper: format seconds -> [H:]MM:SS.mmm
// ==========================================================================

static void format_time_ms(double seconds, pfc::string_base & out) {
	if (!(seconds >= 0)) seconds = 0;

	int total_ms  = static_cast<int>(seconds * 1000.0 + 0.5);
	int ms        = total_ms % 1000;
	int total_sec = total_ms / 1000;
	int sec       = total_sec % 60;
	int total_min = total_sec / 60;
	int min       = total_min % 60;
	int hr        = total_min / 60;

	char buf[32];
	if (hr > 0)
		std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", hr, min, sec, ms);
	else
		std::snprintf(buf, sizeof(buf), "%d:%02d.%03d", min, sec, ms);
	out = buf;
}

// ==========================================================================
// Global titleformat fields — work everywhere at ~1 s resolution
// ==========================================================================

static std::atomic<double>         g_cached_time   { 0.0 };
static std::atomic<metadb_handle*> g_cached_handle { nullptr };

class global_play_callback : public play_callback_static {
public:
	unsigned get_flags() override {
		return flag_on_playback_new_track | flag_on_playback_stop
		     | flag_on_playback_time      | flag_on_playback_seek;
	}
	void on_playback_starting(play_control::t_track_command, bool) override {}
	void on_playback_new_track(metadb_handle_ptr p) override {
		g_cached_handle.store(p.get_ptr(), std::memory_order_relaxed);
		g_cached_time.store(0.0, std::memory_order_relaxed);
	}
	void on_playback_stop(play_control::t_stop_reason) override {
		g_cached_handle.store(nullptr, std::memory_order_relaxed);
		g_cached_time.store(0.0, std::memory_order_relaxed);
	}
	void on_playback_seek(double t) override {
		g_cached_time.store(t, std::memory_order_relaxed);
		dispatch_refresh_playing();
	}
	void on_playback_pause(bool) override {}
	void on_playback_edited(metadb_handle_ptr) override {}
	void on_playback_dynamic_info(const file_info &) override {}
	void on_playback_dynamic_info_track(const file_info &) override {}
	void on_playback_time(double t) override {
		g_cached_time.store(t, std::memory_order_relaxed);
		dispatch_refresh_playing();
	}
	void on_volume_change(float) override {}
private:
	static void dispatch_refresh_playing() {
		metadb_handle_ptr h;
		if (playback_control::get()->get_now_playing(h))
			metadb_io::get()->dispatch_refresh(h);
	}
};

static play_callback_static_factory_t<global_play_callback> g_global_cb;

class global_field_provider : public metadb_display_field_provider {
public:
	enum { field_playback_time_ms, field_length_ms, field_count };

	t_uint32 get_field_count() override { return field_count; }

	void get_field_name(t_uint32 idx, pfc::string_base & out) override {
		switch (idx) {
		case field_playback_time_ms: out = "playback_time_ms"; break;
		case field_length_ms:        out = "length_ms";        break;
		}
	}

	bool process_field(t_uint32 idx, metadb_handle * handle,
	                   titleformat_text_out * out) override {
		switch (idx) {
		case field_playback_time_ms: {
			if (handle != g_cached_handle.load(std::memory_order_relaxed))
				return false;
			pfc::string8 buf;
			format_time_ms(g_cached_time.load(std::memory_order_relaxed), buf);
			out->write(titleformat_inputtypes::unknown, buf.get_ptr(), buf.get_length());
			return true;
		}
		case field_length_ms: {
			double len = handle->get_length();
			if (len <= 0) return false;
			pfc::string8 buf;
			format_time_ms(len, buf);
			out->write(titleformat_inputtypes::meta, buf.get_ptr(), buf.get_length());
			return true;
		}
		default: return false;
		}
	}
};

static service_factory_single_t<global_field_provider> g_global_fields;

// ==========================================================================
// Live titleformat_hook — resolves fields using playback_get_position()
// Used inside the UI element for true millisecond-precision rendering.
// ==========================================================================

class ms_time_hook : public titleformat_hook {
public:
	bool process_field(titleformat_text_out * p_out, const char * p_name,
	                   t_size p_name_length, bool & p_found_flag) override {
		if (pfc::stricmp_ascii_ex(p_name, p_name_length,
		                          "playback_time_ms", SIZE_MAX) == 0) {
			double pos = playback_control::get()->playback_get_position();
			pfc::string8 buf;
			format_time_ms(pos, buf);
			p_out->write(titleformat_inputtypes::unknown, buf.get_ptr(), buf.get_length());
			p_found_flag = true;
			return true;
		}
		if (pfc::stricmp_ascii_ex(p_name, p_name_length,
		                          "length_ms", SIZE_MAX) == 0) {
			double len = playback_control::get()->playback_get_length_ex();
			if (len <= 0) { p_found_flag = false; return false; }
			pfc::string8 buf;
			format_time_ms(len, buf);
			p_out->write(titleformat_inputtypes::meta, buf.get_ptr(), buf.get_length());
			p_found_flag = true;
			return true;
		}
		p_found_flag = false;
		return false;
	}

	bool process_function(titleformat_text_out *, const char *, t_size,
	                      titleformat_hook_function_params *, bool & p_found_flag) override {
		p_found_flag = false;
		return false;
	}
};

// ==========================================================================
// Configuration dialog
// ==========================================================================

class CConfigDialog : public CDialogImpl<CConfigDialog> {
public:
	enum { IDD = IDD_CONFIG };

	CConfigDialog(const char * pattern) : m_pattern(pattern) {}

	BEGIN_MSG_MAP_EX(CConfigDialog)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_ID_HANDLER_EX(IDOK, OnOK)
		COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
	END_MSG_MAP()

	pfc::string8 m_pattern;

private:
	BOOL OnInitDialog(CWindow, LPARAM) {
		uSetDlgItemText(*this, IDC_PATTERN, m_pattern);
		CEdit edit(GetDlgItem(IDC_PATTERN));
		edit.SetSel(0, -1);
		edit.SetFocus();
		return FALSE;
	}
	void OnOK(UINT, int, CWindow) {
		uGetDlgItemText(*this, IDC_PATTERN, m_pattern);
		EndDialog(IDOK);
	}
	void OnCancel(UINT, int, CWindow) {
		EndDialog(IDCANCEL);
	}
};

// ==========================================================================
// UI Element — smooth millisecond-precision display panel
// ==========================================================================

// {D3A7B1C2-4E5F-6A89-0B1C-2D3E4F5A6B7C}
static const GUID guid_ms_time_elem = {
	0xd3a7b1c2, 0x4e5f, 0x6a89,
	{ 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b, 0x7c }
};

static ui_element_config::ptr make_config(const GUID & guid, const char * pattern) {
	return ui_element_config::g_create(guid, pattern,
	                                   strlen(pattern));
}

static pfc::string8 parse_config(ui_element_config::ptr cfg) {
	auto data = static_cast<const char *>(cfg->get_data());
	auto size = cfg->get_data_size();
	if (data == nullptr || size == 0)
		return k_default_pattern;
	return pfc::string8(data, size);
}

class CTimeDisplayElem
	: public ui_element_instance
	, public CWindowImpl<CTimeDisplayElem>
	, private play_callback_impl_base {
public:
	DECLARE_WND_CLASS_EX(
		TEXT("{D3A7B1C2-4E5F-6A89-0B1C-2D3E4F5A6B7C}"),
		CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS, (-1));

	CTimeDisplayElem(ui_element_config::ptr config,
	                 ui_element_instance_callback_ptr callback)
		: m_callback(callback)
		, m_config(config)
		, play_callback_impl_base(0)
	{
		m_pattern = parse_config(config);
		titleformat_compiler::get()->compile_safe_ex(m_script, m_pattern);
	}

	void initialize_window(HWND parent) {
		WIN32_OP(Create(parent, nullptr, nullptr,
		                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN) != nullptr);
	}

	// --- ui_element_instance interface ---
	HWND get_wnd() override { return *this; }

	void set_configuration(ui_element_config::ptr config) override {
		m_config  = config;
		m_pattern = parse_config(config);
		titleformat_compiler::get()->compile_safe_ex(m_script, m_pattern);
		Invalidate();
	}

	ui_element_config::ptr get_configuration() override { return m_config; }

	static GUID  g_get_guid()    { return guid_ms_time_elem; }
	static GUID  g_get_subclass(){ return ui_element_subclass_utility; }
	static void  g_get_name(pfc::string_base & out) { out = "Millisecond Time Display"; }
	static const char * g_get_description() {
		return "Displays playback time and track length with millisecond precision (~60 fps).";
	}
	static ui_element_config::ptr g_get_default_configuration() {
		return make_config(g_get_guid(), k_default_pattern);
	}

	void notify(const GUID & what, t_size param1, const void *, t_size) override {
		if (what == ui_element_notify_colors_changed ||
		    what == ui_element_notify_font_changed) {
			Invalidate();
		} else if (what == ui_element_notify_visibility_changed) {
			m_visible = (param1 != 0);
			if (m_visible) sync_timer_to_playback();
			else           stop_timer();
		}
	}

	// --- ATL message map ---
	BEGIN_MSG_MAP_EX(CTimeDisplayElem)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_PAINT(OnPaint)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MSG_WM_TIMER(OnTimer)
		MSG_WM_CONTEXTMENU(OnContextMenu)
	END_MSG_MAP()

private:
	enum { TIMER_ID = 1, TIMER_MS = 16 };   // ~60 fps

	// --- Message handlers ---

	int OnCreate(LPCREATESTRUCT) {
		play_callback_reregister(
			flag_on_playback_starting | flag_on_playback_new_track |
			flag_on_playback_stop     | flag_on_playback_pause     |
			flag_on_playback_seek     | flag_on_playback_edited    |
			flag_on_playback_dynamic_info_track,
			false);
		sync_timer_to_playback();
		return 0;
	}

	void OnDestroy() {
		stop_timer();
		play_callback_reregister(0);
	}

	BOOL OnEraseBkgnd(CDCHandle) { return TRUE; }

	void OnTimer(UINT_PTR id) {
		if (id == TIMER_ID) Invalidate();
	}

	void OnPaint(CDCHandle) {
		CPaintDC pdc(*this);
		CRect rc;
		GetClientRect(&rc);

		CDC mem;
		mem.CreateCompatibleDC(pdc);
		CBitmap bmp;
		bmp.CreateCompatibleBitmap(pdc, rc.Width(), rc.Height());
		HBITMAP old_bmp = mem.SelectBitmap(bmp);

		COLORREF bg = m_callback->query_std_color(ui_color_background);
		CBrush bg_brush;
		bg_brush.CreateSolidBrush(bg);
		mem.FillRect(&rc, bg_brush);

		mem.SetTextColor(m_callback->query_std_color(ui_color_text));
		mem.SetBkMode(TRANSPARENT);
		HFONT font = (HFONT)m_callback->query_font_ex(ui_font_default);
		HFONT old_font = mem.SelectFont(font);

		pfc::string8 text;
		evaluate(text);

		pfc::stringcvt::string_wide_from_utf8 wtext(text);
		CRect text_rc = rc;
		text_rc.DeflateRect(4, 0);
		mem.DrawText(wtext, -1, &text_rc,
		             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		mem.SelectFont(old_font);
		pdc.BitBlt(0, 0, rc.Width(), rc.Height(), mem, 0, 0, SRCCOPY);
		mem.SelectBitmap(old_bmp);
	}

	void OnContextMenu(CWindow, CPoint pt) {
		if (pt.x == -1 && pt.y == -1) {
			CRect rc;
			GetClientRect(&rc);
			pt = rc.CenterPoint();
			ClientToScreen(&pt);
		}

		enum { CMD_CONFIGURE = 1 };
		CMenu menu;
		menu.CreatePopupMenu();
		menu.AppendMenu(MF_STRING, CMD_CONFIGURE, TEXT("Configure..."));

		int cmd = menu.TrackPopupMenu(
			TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
			pt.x, pt.y, *this);

		if (cmd == CMD_CONFIGURE) {
			CConfigDialog dlg(m_pattern);
			if (dlg.DoModal(*this) == IDOK) {
				m_pattern = dlg.m_pattern;
				titleformat_compiler::get()->compile_safe_ex(m_script, m_pattern);
				m_config = make_config(g_get_guid(), m_pattern);
				Invalidate();
			}
		}
	}

	// --- Play callbacks ---

	void on_playback_starting(play_control::t_track_command, bool paused) override {
		if (!paused) start_timer();
	}
	void on_playback_new_track(metadb_handle_ptr) override {
		sync_timer_to_playback();
	}
	void on_playback_stop(play_control::t_stop_reason) override {
		stop_timer();
		Invalidate();
	}
	void on_playback_seek(double) override {
		Invalidate();
	}
	void on_playback_pause(bool paused) override {
		if (paused) { stop_timer(); Invalidate(); }
		else start_timer();
	}
	void on_playback_edited(metadb_handle_ptr) override { Invalidate(); }
	void on_playback_dynamic_info_track(const file_info &) override { Invalidate(); }

	// --- Helpers ---

	void evaluate(pfc::string_base & out) {
		if (m_script.is_empty()) return;
		ms_time_hook hook;
		auto pc = playback_control::get();
		if (!pc->playback_format_title(&hook, out, m_script, nullptr,
		                               playback_control::display_level_all)) {
			if (pc->is_playing())
				out = "Opening\xe2\x80\xa6";
			else
				out.reset();
		}
	}

	void sync_timer_to_playback() {
		auto pc = playback_control::get();
		if (pc->is_playing() && !pc->is_paused() && m_visible)
			start_timer();
		else
			stop_timer();
		Invalidate();
	}

	void start_timer() {
		if (!m_timer_on) {
			SetTimer(TIMER_ID, TIMER_MS);
			m_timer_on = true;
		}
	}

	void stop_timer() {
		if (m_timer_on) {
			KillTimer(TIMER_ID);
			m_timer_on = false;
		}
	}

	// --- Data ---

	ui_element_config::ptr m_config;
	pfc::string8           m_pattern;
	titleformat_object::ptr m_script;
	bool m_timer_on = false;
	bool m_visible  = true;

protected:
	const ui_element_instance_callback_ptr m_callback;
};

class ms_time_ui_element : public ui_element_impl_withpopup<CTimeDisplayElem> {
public:
	bool get_popup_specs(ui_size & defSize, pfc::string_base & title) override {
		defSize = { 220, 24 };
		title = "Millisecond Time";
		return true;
	}
};
static service_factory_single_t<ms_time_ui_element> g_ui_element_factory;

} // anonymous namespace
