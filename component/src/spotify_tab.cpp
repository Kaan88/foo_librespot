#include "stdafx.h"

#include "spotify_tab.h"

#include "component_helpers.h"
#include "resource.h"
#include "spotify_links.h"
#include "spotify_playlists.h"

#include "bridge/process.h"

#include <SDK/console.h>
#include <SDK/coreDarkMode.h>
#include <SDK/metadb.h>
#include <SDK/playlist.h>
#include <SDK/popup_message.h>
#include <SDK/ui.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <commctrl.h>
#include <ole2.h>
#include <windows.h>

namespace flp {
class spotify_panel_impl;
namespace {

const GUID kSpotifyTabGuid = { 0xc8a54f61, 0x2b7d, 0x4e93, { 0x91, 0x0c, 0x5a, 0x6e, 0x24, 0xf8, 0x3b, 0xd7 } };

constexpr UINT kMsgRunSearch = WM_APP + 1;
constexpr UINT kMsgClearSearch = WM_APP + 2;
constexpr UINT_PTR kTimerSingleClick = 1;
constexpr UINT_PTR kTimerRetryLibrary = 2;
constexpr int kSearchLimit = 25;
// WM_DPICHANGED is only sent to top-level windows; children get this one
// (Windows 10 1703+) after the parent moved to a monitor with another DPI.
constexpr UINT kMsgDpiChangedAfterParent = 0x02E3;

enum class node_kind { section, item, message, loading };

struct tree_node {
	node_kind kind = node_kind::message;
	std::string text;
	std::string uri;
	std::string item_kind; // playlist, album, artist, collection, track, top_tracks
	std::string name;
	// Section id of a section node ("playlists", "recently_played", ...).
	std::string section_id;
	// The item came from a library section, so it is most likely followed;
	// only used to label the Follow/Unfollow entry when the bridge has not
	// answered yet.
	bool likely_followed = false;
	bool editable = false;
	bool expandable = false;
	bool loaded = false;
	bool loading = false;
	uint32_t id = 0;
	HTREEITEM item = nullptr;
	// Track nodes from search / artist pages: the full info, so a drag or the
	// selection carries titles before the track was ever played.
	std::shared_ptr<const track_info> track;
};

struct library_snapshot {
	enum class state_t { ok, not_logged_in, connecting, error };
	state_t state = state_t::error;
	std::string error;
	std::vector<library_section> sections;
};

struct async_state {
	std::atomic<bool> alive{ true };
	std::atomic<uint64_t> library_generation{ 0 };
	std::atomic<uint64_t> search_generation{ 0 };
	std::atomic<uint64_t> open_generation{ 0 };
	class spotify_panel_impl* owner = nullptr;
};

// A cancel token for panel work: stops when the panel goes away or foobar2000
// quits, and optionally when `superseded` says a newer request replaced it.
cancel_token panel_token(const std::shared_ptr<async_state>& state,
	std::function<bool()> superseded = std::function<bool()>()) {
	cancel_token token;
	token.is_aborting = [state, superseded] {
		return !state->alive.load() || app_is_quitting() || (superseded && superseded());
	};
	return token;
}

// The window's DPI; GetDpiForWindow (Windows 10 1607+) follows per-monitor
// DPI, older systems report the system DPI.
int window_dpi(HWND wnd) {
	using get_dpi_t = UINT(WINAPI*)(HWND);
	static const get_dpi_t getDpi = [] {
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		return user32 == nullptr
			? nullptr
			: reinterpret_cast<get_dpi_t>(GetProcAddress(user32, "GetDpiForWindow"));
	}();
	if (getDpi != nullptr && wnd != nullptr) {
		const UINT dpi = getDpi(wnd);
		if (dpi > 0) return static_cast<int>(dpi);
	}
	int dpi = 96;
	HDC screen = GetDC(wnd);
	if (screen != nullptr) {
		const int value = GetDeviceCaps(screen, LOGPIXELSX);
		if (value > 0) dpi = value;
		ReleaseDC(wnd, screen);
	}
	return dpi;
}

// Minimal IDropSource for dragging tracks out of the tree.
class tree_drop_source : public IDropSource {
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (out == nullptr) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDropSource) {
			*out = static_cast<IDropSource*>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG refs = --m_refs;
		if (refs == 0) delete this;
		return refs;
	}
	HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
		if (escape) return DRAGDROP_S_CANCEL;
		if ((keys & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
	std::atomic<ULONG> m_refs{ 1 };
};

// "Copy link": the https form, like the track menu's Copy link. Liked Songs
// has a fixed web address; anything else falls back to the URI.
std::string web_link(const std::string& uri) {
	const std::string url = spotify_web_url(uri);
	return url.empty() ? uri : url;
}

std::string trim(const std::string& value) {
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' ||
		value[begin] == '\n')) {
		++begin;
	}
	while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
		value[end - 1] == '\r' || value[end - 1] == '\n')) {
		--end;
	}
	return value.substr(begin, end - begin);
}

std::string artists_text(const track_info& track) {
	std::string out;
	for (size_t i = 0; i < track.artists.size(); ++i) {
		if (i != 0) out += ", ";
		out += track.artists[i];
	}
	return out;
}

bool is_openable_item(const tree_node& node) { return !node.uri.empty() && !node.item_kind.empty(); }

// Small name prompt for "New playlist..." and "Rename...", with the same dark
// mode hookup as the Add Spotify link dialog.
class playlist_name_dialog {
public:
	std::string text;

	INT_PTR run(HWND parent, const wchar_t* caption, const std::string& initial) {
		m_caption = caption;
		m_initial = initial;
		return DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_PLAYLIST_NAME),
			parent, &playlist_name_dialog::dialog_proc, reinterpret_cast<LPARAM>(this));
	}

private:
	static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
		if (message == WM_INITDIALOG) {
			auto* self = reinterpret_cast<playlist_name_dialog*>(lparam);
			SetWindowLongPtrW(wnd, GWLP_USERDATA, lparam);
			self->m_dark.AddDialogWithControls(wnd);
			SetWindowTextW(wnd, self->m_caption.c_str());
			SetDlgItemTextW(wnd, IDC_PLAYLIST_NAME, wide_from_utf8(self->m_initial).c_str());
			SendDlgItemMessageW(wnd, IDC_PLAYLIST_NAME, EM_SETSEL, 0,
				static_cast<LPARAM>(-1));
			SetFocus(GetDlgItem(wnd, IDC_PLAYLIST_NAME));
			return FALSE;
		}

		auto* self = reinterpret_cast<playlist_name_dialog*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
		if (self == nullptr) return FALSE;

		if (message == WM_COMMAND) {
			switch (LOWORD(wparam)) {
			case IDOK: {
				HWND edit = GetDlgItem(wnd, IDC_PLAYLIST_NAME);
				const int length = GetWindowTextLengthW(edit);
				std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
				if (length > 0) GetWindowTextW(edit, wide.data(), length + 1);
				wide.resize(static_cast<size_t>(length));
				self->text = utf8_from_wide(wide);
				EndDialog(wnd, IDOK);
				return TRUE;
			}
			case IDCANCEL:
				EndDialog(wnd, IDCANCEL);
				return TRUE;
			default:
				break;
			}
		}
		return FALSE;
	}

	std::wstring m_caption;
	std::string m_initial;
	fb2k::CCoreDarkModeHooks m_dark;
};

} // namespace

const GUID spotify_tab_guid = kSpotifyTabGuid;

class spotify_panel_impl : public spotify_panel {
public:
	spotify_panel_impl(HWND parent, spotify_panel_host& host, bool visible) : m_host(host) {
		m_state = std::make_shared<async_state>();
		m_state->owner = this;

		static ATOM windowClass = 0;
		if (windowClass == 0) {
			WNDCLASSEXW wc = {};
			wc.cbSize = sizeof(wc);
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = &spotify_panel_impl::window_proc;
			wc.hInstance = core_api::get_my_instance();
			wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			wc.lpszClassName = L"foo_librespot.spotify_tab";
			windowClass = RegisterClassExW(&wc);
		}
		if (windowClass == 0) throw exception_win32(GetLastError());

		// WS_EX_CONTROLPARENT: hosts that do dialog-style Tab handling include
		// the search box, Refresh and the tree.
		m_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"foo_librespot.spotify_tab", L"Spotify",
			WS_CHILD | WS_CLIPCHILDREN | (visible ? WS_VISIBLE : 0), 0, 0, 320, 240, parent,
			nullptr, core_api::get_my_instance(), this);
		if (m_wnd == nullptr) throw exception_win32(GetLastError());
	}

	~spotify_panel_impl() override {
		m_state->alive = false;
		m_state->owner = nullptr;
		if (m_wnd != nullptr && IsWindow(m_wnd)) DestroyWindow(m_wnd);
		if (m_background_brush != nullptr) {
			DeleteObject(m_background_brush);
			m_background_brush = nullptr;
		}
	}

	HWND get_wnd() const override { return m_wnd; }

	void set_default_focus() override {
		if (m_search != nullptr) SetFocus(m_search);
	}

	void on_style_changed() override {
		apply_colors();
		apply_fonts();
		if (m_wnd != nullptr) InvalidateRect(m_wnd, nullptr, TRUE);
	}

private:
	static LRESULT CALLBACK window_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
		auto* self = reinterpret_cast<spotify_panel_impl*>(
			GetWindowLongPtrW(wnd, GWLP_USERDATA));
		if (message == WM_NCCREATE) {
			auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
			self = reinterpret_cast<spotify_panel_impl*>(create->lpCreateParams);
			SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			if (self != nullptr) self->m_wnd = wnd;
		}
		if (self != nullptr) return self->handle_message(message, wparam, lparam);
		return DefWindowProcW(wnd, message, wparam, lparam);
	}

	LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
		switch (message) {
		case WM_CREATE:
			try {
				create_controls();
			} catch (...) {
				return -1;
			}
			return 0;
		case WM_SIZE: {
			// Catches a DPI change on hosts that do not forward
			// WM_DPICHANGED_AFTERPARENT.
			const int dpi = window_dpi(m_wnd);
			if (dpi != m_dpi) m_dpi = dpi;
			layout();
			return 0;
		}
		case kMsgDpiChangedAfterParent:
			m_dpi = window_dpi(m_wnd);
			layout();
			InvalidateRect(m_wnd, nullptr, TRUE);
			return 0;
		case WM_CONTEXTMENU:
			if (reinterpret_cast<HWND>(wparam) == m_tree) {
				on_tree_context_menu(lparam);
				return 0;
			}
			break;
		case WM_ERASEBKGND: {
			RECT rect;
			GetClientRect(m_wnd, &rect);
			FillRect(reinterpret_cast<HDC>(wparam), &rect, background_brush());
			return 1;
		}
		case WM_PAINT: {
			PAINTSTRUCT paint;
			BeginPaint(m_wnd, &paint);
			EndPaint(m_wnd, &paint);
			return 0;
		}
		case WM_CTLCOLORSTATIC: {
			HDC dc = reinterpret_cast<HDC>(wparam);
			SetTextColor(dc, text_color());
			SetBkColor(dc, background_color());
			return reinterpret_cast<LRESULT>(background_brush());
		}
		case WM_CTLCOLOREDIT: {
			HDC dc = reinterpret_cast<HDC>(wparam);
			SetTextColor(dc, text_color());
			SetBkColor(dc, background_color());
			return reinterpret_cast<LRESULT>(background_brush());
		}
		case WM_COMMAND:
			if (LOWORD(wparam) == kRefreshButtonId && HIWORD(wparam) == BN_CLICKED) {
				request_library();
				return 0;
			}
			break;
		case WM_DPICHANGED:
			m_dpi = static_cast<int>(HIWORD(wparam));
			layout();
			return 0;
		case WM_SETFOCUS:
			if (m_search != nullptr) {
				SetFocus(m_search);
				return 0;
			}
			break;
		case WM_TIMER:
			if (wparam == kTimerSingleClick) {
				KillTimer(m_wnd, kTimerSingleClick);
				const uint32_t id = m_pending_click;
				m_pending_click = 0;
				if (id != 0) {
					tree_node* node = find_node(id);
					if (node != nullptr && is_openable_item(*node)) open_item(node, false, false);
				}
				return 0;
			}
			if (wparam == kTimerRetryLibrary) {
				KillTimer(m_wnd, kTimerRetryLibrary);
				if (m_library_state == library_snapshot::state_t::connecting) request_library();
				return 0;
			}
			break;
		case WM_NOTIFY:
			return handle_notify(reinterpret_cast<NMHDR*>(lparam));
		case kMsgRunSearch:
			run_search();
			return 0;
		case kMsgClearSearch:
			// Invalidate a search that is still in flight.
			++m_state->search_generation;
			SetWindowTextW(m_search, L"");
			if (m_has_search) {
				m_has_search = false;
				m_search_error.clear();
				m_searchResults = search_result();
				rebuild_tree();
			}
			return 0;
		case WM_NCDESTROY: {
			HWND wnd = m_wnd;
			if (m_state) {
				m_state->owner = nullptr;
				m_state->alive = false;
			}
			KillTimer(wnd, kTimerSingleClick);
			KillTimer(wnd, kTimerRetryLibrary);
			m_selection.release();
			SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
			m_wnd = nullptr;
			return DefWindowProcW(wnd, message, wparam, lparam);
		}
		default:
			break;
		}
		return DefWindowProcW(m_wnd, message, wparam, lparam);
	}

	void create_controls() {
		INITCOMMONCONTROLSEX commonControls = {};
		commonControls.dwSize = sizeof(commonControls);
		commonControls.dwICC = ICC_TREEVIEW_CLASSES;
		InitCommonControlsEx(&commonControls);

		m_dpi = window_dpi(m_wnd);

		m_search = CreateWindowExW(0, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
			0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
			core_api::get_my_instance(), nullptr);
		SendMessageW(m_search, EM_SETCUEBANNER, TRUE,
			reinterpret_cast<LPARAM>(L"Search Spotify \x2014 press Enter"));
		SetWindowSubclass(m_search, &spotify_panel_impl::search_subclass, 1,
			reinterpret_cast<DWORD_PTR>(this));

		m_refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButtonId)),
			core_api::get_my_instance(), nullptr);

		m_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
				TVS_LINESATROOT | TVS_SHOWSELALWAYS,
			0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)),
			core_api::get_my_instance(), nullptr);

		m_status = CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
			0, 0, 10, 10, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)),
			core_api::get_my_instance(), nullptr);

		if (m_search == nullptr || m_refresh == nullptr || m_tree == nullptr ||
			m_status == nullptr) {
			throw exception_win32(GetLastError());
		}
		// Tab / Shift+Tab move between the search box, Refresh and the tree
		// even when the host does no dialog-style keyboard handling.
		SetWindowSubclass(m_refresh, &spotify_panel_impl::tab_subclass, 2,
			reinterpret_cast<DWORD_PTR>(this));
		SetWindowSubclass(m_tree, &spotify_panel_impl::tab_subclass, 2,
			reinterpret_cast<DWORD_PTR>(this));

		m_dark.AddControls(m_wnd);

		apply_fonts();
		apply_colors();
		layout();
		set_status("Loading Spotify library\xE2\x80\xA6");
		request_library();
	}

	static LRESULT CALLBACK search_subclass(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam,
		UINT_PTR, DWORD_PTR reference) {
		auto* self = reinterpret_cast<spotify_panel_impl*>(reference);
		if (self != nullptr && self->handle_tab_key(wnd, message, wparam)) return 0;
		if (message == WM_KEYDOWN && self != nullptr) {
			if (wparam == VK_RETURN) {
				PostMessageW(self->m_wnd, kMsgRunSearch, 0, 0);
				return 0;
			}
			if (wparam == VK_ESCAPE) {
				PostMessageW(self->m_wnd, kMsgClearSearch, 0, 0);
				return 0;
			}
		}
		return DefSubclassProc(wnd, message, wparam, lparam);
	}

	static LRESULT CALLBACK tab_subclass(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam,
		UINT_PTR, DWORD_PTR reference) {
		auto* self = reinterpret_cast<spotify_panel_impl*>(reference);
		if (self != nullptr && self->handle_tab_key(wnd, message, wparam)) return 0;
		if (message == WM_NCDESTROY) {
			RemoveWindowSubclass(wnd, &spotify_panel_impl::tab_subclass, 2);
		}
		return DefSubclassProc(wnd, message, wparam, lparam);
	}

	// Tab cycles search box -> Refresh -> tree (Shift+Tab backwards).
	bool handle_tab_key(HWND wnd, UINT message, WPARAM wparam) {
		if (message == WM_CHAR && wparam == L'\t') return true; // no beep
		if (message != WM_KEYDOWN || wparam != VK_TAB) return false;
		if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) return false;
		const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const HWND order[] = { m_search, m_refresh, m_tree };
		size_t at = 0;
		for (size_t i = 0; i < 3; ++i) {
			if (order[i] == wnd) at = i;
		}
		const HWND next = order[(at + (back ? 2 : 1)) % 3];
		if (next != nullptr) SetFocus(next);
		return true;
	}

	int scaled(int value) const { return MulDiv(value, m_dpi, 96); }

	void layout() {
		if (m_wnd == nullptr || m_search == nullptr) return;
		RECT rect;
		GetClientRect(m_wnd, &rect);
		const int pad = scaled(6);
		const int editHeight = scaled(24);
		const int buttonWidth = scaled(82);
		const int statusHeight = scaled(18);
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;

		MoveWindow(m_search, pad, pad, std::max(10, width - pad * 3 - buttonWidth), editHeight,
			TRUE);
		MoveWindow(m_refresh, width - pad - buttonWidth, pad, buttonWidth, editHeight, TRUE);

		const int treeTop = pad + editHeight + pad;
		const int treeHeight = std::max(10, height - treeTop - statusHeight - pad);
		MoveWindow(m_tree, pad, treeTop, std::max(10, width - pad * 2), treeHeight, TRUE);
		MoveWindow(m_status, pad, height - statusHeight - pad / 2, std::max(10, width - pad * 2),
			statusHeight, TRUE);
	}

	void apply_fonts() {
		if (m_tree == nullptr) return;
		const HFONT listFont = m_host.panel_list_font();
		if (listFont != nullptr) {
			SendMessageW(m_tree, WM_SETFONT, reinterpret_cast<WPARAM>(listFont), TRUE);
		}
		const HFONT defaultFont = m_host.panel_default_font();
		if (defaultFont != nullptr) {
			SendMessageW(m_search, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
			SendMessageW(m_refresh, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
			SendMessageW(m_status, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
		}
	}

	bool dark_mode() const { return m_host.panel_dark_mode(); }

	COLORREF background_color() const {
		COLORREF color = 0;
		if (m_host.panel_background(color)) return color;
		return dark_mode() ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
	}

	COLORREF text_color() const {
		COLORREF color = 0;
		if (m_host.panel_text(color)) return color;
		return dark_mode() ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT);
	}

	HBRUSH background_brush() {
		if (m_background_brush == nullptr) {
			m_background_brush = CreateSolidBrush(background_color());
		}
		return m_background_brush;
	}

	void apply_colors() {
		if (m_background_brush != nullptr) {
			DeleteObject(m_background_brush);
			m_background_brush = nullptr;
		}
		if (m_tree != nullptr && IsWindow(m_tree)) {
			TreeView_SetBkColor(m_tree, background_color());
			TreeView_SetTextColor(m_tree, text_color());
			TreeView_SetLineColor(m_tree, dark_mode() ? text_color() : CLR_DEFAULT);
		}
		if (m_wnd != nullptr) InvalidateRect(m_wnd, nullptr, TRUE);
	}

	void set_status(const std::string& text) {
		if (m_status != nullptr) SetWindowTextW(m_status, wide_from_utf8(text).c_str());
	}

	// ---- tree helpers -------------------------------------------------

	tree_node* add_node(HTREEITEM parent, node_kind kind, const std::string& text,
		const std::string& uri, const std::string& itemKind, const std::string& name,
		bool expandable) {
		auto node = std::make_unique<tree_node>();
		node->kind = kind;
		node->text = text;
		node->uri = uri;
		node->item_kind = itemKind;
		node->name = name;
		node->expandable = expandable;
		node->id = ++m_next_node_id;

		TVINSERTSTRUCTW insert = {};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
		const std::wstring wide = wide_from_utf8(text);
		insert.item.pszText = const_cast<LPWSTR>(wide.c_str());
		insert.item.lParam = reinterpret_cast<LPARAM>(node.get());
		insert.item.cChildren = expandable ? 1 : 0;
		node->item = TreeView_InsertItem(m_tree, &insert);

		tree_node* raw = node.get();
		m_nodes.push_back(std::move(node));
		return raw;
	}

	tree_node* find_node(uint32_t id) const {
		for (const auto& node : m_nodes) {
			if (node->id == id) return node.get();
		}
		return nullptr;
	}

	tree_node* node_from_pointer(LPARAM param) const {
		const auto* raw = reinterpret_cast<tree_node*>(param);
		for (const auto& node : m_nodes) {
			if (node.get() == raw) return node.get();
		}
		return nullptr;
	}

	tree_node* node_from_item(HTREEITEM item) const {
		if (item == nullptr) return nullptr;
		TVITEMW info = {};
		info.mask = TVIF_PARAM;
		info.hItem = item;
		if (!TreeView_GetItem(m_tree, &info)) return nullptr;
		return node_from_pointer(info.lParam);
	}

	tree_node* selected_node() const {
		return node_from_item(TreeView_GetSelection(m_tree));
	}

	void clear_tree() {
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;
		TreeView_DeleteAllItems(m_tree);
		m_nodes.clear();
	}

	void rebuild_tree() {
		clear_tree();

		if (m_has_search) add_search_nodes();

		const bool hasLibrary = !m_library.empty();
		if (!hasLibrary) {
			if (m_library_loading) {
				add_node(TVI_ROOT, node_kind::loading, "Loading Spotify library\xE2\x80\xA6",
					std::string(), std::string(), std::string(), false);
			} else if (!m_library_error.empty()) {
				add_node(TVI_ROOT, node_kind::message, m_library_error, std::string(),
					std::string(), std::string(), false);
			} else {
				add_node(TVI_ROOT, node_kind::message, "No Spotify library loaded. Click Refresh.",
					std::string(), std::string(), std::string(), false);
			}
			return;
		}

		const library_section* liked = find_section("liked");
		const library_section* albums = find_section("albums");
		const library_section* artists = find_section("artists");
		const library_section* madeForYou = find_section("made_for_you");
		const library_section* playlists = find_section("playlists");

		tree_node* yourLibrary = add_node(TVI_ROOT, node_kind::section, "Your library",
			std::string(), std::string(), std::string(), true);
		if (liked != nullptr) {
			for (const library_item& item : liked->items) add_item_node(yourLibrary->item, item, true);
		}
		if (liked == nullptr || liked->items.empty()) {
			add_node(yourLibrary->item, node_kind::message, "Liked Songs unavailable",
				std::string(), std::string(), std::string(), false);
		}

		tree_node* albumsNode = add_node(yourLibrary->item, node_kind::section, "Albums",
			std::string(), std::string(), std::string(), true);
		if (albums != nullptr) {
			for (const library_item& item : albums->items) add_item_node(albumsNode->item, item, true);
		}
		if (albums == nullptr || albums->items.empty()) {
			add_node(albumsNode->item, node_kind::message, "No saved albums",
				std::string(), std::string(), std::string(), false);
		}

		tree_node* artistsNode = add_node(yourLibrary->item, node_kind::section, "Artists",
			std::string(), std::string(), std::string(), true);
		if (artists != nullptr) {
			for (const library_item& item : artists->items) add_item_node(artistsNode->item, item, true);
		}
		if (artists == nullptr || artists->items.empty()) {
			add_node(artistsNode->item, node_kind::message, "No followed artists",
				std::string(), std::string(), std::string(), false);
		}

		// v5: Recently played sits just below Your library. A section the bridge
		// reports as empty is left out entirely.
		add_section_node("recently_played", "Recently played");

		tree_node* madeNode = add_node(TVI_ROOT, node_kind::section,
			madeForYou != nullptr && !madeForYou->title.empty() ? madeForYou->title : "Made for you",
			std::string(), std::string(), std::string(), true);
		if (madeForYou != nullptr) {
			for (const library_item& item : madeForYou->items) add_item_node(madeNode->item, item, true);
		}
		if (madeForYou == nullptr || madeForYou->items.empty()) {
			add_node(madeNode->item, node_kind::message, "No made-for-you playlists",
				std::string(), std::string(), std::string(), false);
		}

		tree_node* playlistsNode = add_node(TVI_ROOT, node_kind::section,
			playlists != nullptr && !playlists->title.empty() ? playlists->title : "Your playlists",
			std::string(), std::string(), std::string(), true);
		playlistsNode->section_id = "playlists";
		if (playlists != nullptr) {
			for (const library_item& item : playlists->items) {
				add_item_node(playlistsNode->item, item, true);
			}
		}
		if (playlists == nullptr || playlists->items.empty()) {
			add_node(playlistsNode->item, node_kind::message, "No Spotify playlists",
				std::string(), std::string(), std::string(), false);
		}

		// v5: present but empty until a bridge has a top-list source.
		add_section_node("top_tracks", "Top tracks");
		add_section_node("top_artists", "Top artists");

		TreeView_Expand(m_tree, yourLibrary->item, TVE_EXPAND);
	}

	// Adds a top-level library section, unless the bridge reported it empty.
	tree_node* add_section_node(const char* id, const char* fallback_title) {
		const library_section* section = find_section(id);
		if (section == nullptr || section->items.empty()) return nullptr;
		tree_node* node = add_node(TVI_ROOT, node_kind::section,
			section->title.empty() ? fallback_title : section->title, std::string(),
			std::string(), std::string(), true);
		node->section_id = id;
		for (const library_item& item : section->items) add_item_node(node->item, item, false);
		return node;
	}

	tree_node* add_item_node(HTREEITEM parent, const library_item& item,
		bool likely_followed = false) {
		std::string text = item.name;
		if (!item.subtitle.empty()) text += " \xE2\x80\x94 " + item.subtitle;
		const bool expandable = item.kind == "artist";
		tree_node* node = add_node(parent, node_kind::item, text, item.uri, item.kind, item.name,
			expandable);
		node->likely_followed = likely_followed;
		node->editable = item.editable;
		return node;
	}

	void add_track_node(HTREEITEM parent, const track_info& track) {
		library_item item;
		item.uri = track.uri;
		item.kind = "track";
		item.name = track.title;
		item.subtitle = artists_text(track);
		tree_node* node = add_item_node(parent, item);
		node->track = std::make_shared<const track_info>(track);
	}

	const library_section* find_section(const char* id) const {
		for (const library_section& section : m_library) {
			if (section.id == id) return &section;
		}
		return nullptr;
	}

	void add_search_nodes() {
		tree_node* root = add_node(TVI_ROOT, node_kind::section, "Search results", std::string(),
			std::string(), std::string(), true);

		if (!m_search_error.empty()) {
			add_node(root->item, node_kind::message, m_search_error, std::string(), std::string(),
				std::string(), false);
			TreeView_Expand(m_tree, root->item, TVE_EXPAND);
			return;
		}

		bool any = false;
		if (!m_searchResults.tracks.empty()) {
			tree_node* group = add_node(root->item, node_kind::section, "Tracks", std::string(),
				std::string(), std::string(), true);
			for (const track_info& track : m_searchResults.tracks) {
				add_track_node(group->item, track);
			}
			any = true;
		}
		if (!m_searchResults.albums.empty()) {
			tree_node* group = add_node(root->item, node_kind::section, "Albums", std::string(),
				std::string(), std::string(), true);
			for (const library_item& item : m_searchResults.albums) add_item_node(group->item, item);
			any = true;
		}
		if (!m_searchResults.artists.empty()) {
			tree_node* group = add_node(root->item, node_kind::section, "Artists", std::string(),
				std::string(), std::string(), true);
			for (const library_item& item : m_searchResults.artists) add_item_node(group->item, item);
			any = true;
		}
		if (!m_searchResults.playlists.empty()) {
			tree_node* group = add_node(root->item, node_kind::section, "Playlists", std::string(),
				std::string(), std::string(), true);
			for (const library_item& item : m_searchResults.playlists) add_item_node(group->item, item);
			any = true;
		}
		if (!any) {
			add_node(root->item, node_kind::message, "No matches", std::string(), std::string(),
				std::string(), false);
		}
		TreeView_Expand(m_tree, root->item, TVE_EXPAND);
	}

	// ---- bridge work --------------------------------------------------

	void request_library() {
		KillTimer(m_wnd, kTimerRetryLibrary);
		const uint64_t generation = ++m_state->library_generation;
		m_library_loading = true;
		m_library_state = library_snapshot::state_t::connecting;
		m_library_error.clear();
		set_status("Loading Spotify library\xE2\x80\xA6");
		rebuild_tree();

		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state,
			[state, generation] { return state->library_generation.load() != generation; });

		fb2k::splitTask([state, generation, cancel] {
			library_snapshot snapshot;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				const bridge_status status = client.status(cancel, 5000, 15000);
				if (!status.logged_in) {
					snapshot.state = library_snapshot::state_t::not_logged_in;
				} else if (status.state == "error") {
					snapshot.state = library_snapshot::state_t::error;
					snapshot.error = status.last_error.empty()
						? "Spotify: the bridge reported an error. Click Refresh to retry."
						: "Spotify: " + status.last_error;
				} else if (status.state != "connected") {
					snapshot.state = library_snapshot::state_t::connecting;
					snapshot.error = status.last_error;
				} else {
					snapshot.sections = client.library(cancel);
					snapshot.state = library_snapshot::state_t::ok;
				}
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				snapshot.state = library_snapshot::state_t::error;
				snapshot.error = user_error_message(failure);
			}

			fb2k::inMainThread([state, generation, snapshot] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (state->library_generation.load() != generation) return;
				state->owner->on_library_loaded(snapshot);
			});
		});
	}

	void on_library_loaded(const library_snapshot& snapshot) {
		m_library_loading = false;
		m_library_state = snapshot.state;
		switch (snapshot.state) {
		case library_snapshot::state_t::ok:
			m_library = snapshot.sections;
			m_library_error.clear();
			KillTimer(m_wnd, kTimerRetryLibrary);
			if (m_library.empty()) {
				m_library_error = "Spotify: no playlists, albums or artists found.";
				set_status(m_library_error);
			} else {
				set_status("Click an item to open it; double-click to play. Right-click for more.");
			}
			break;
		case library_snapshot::state_t::not_logged_in:
			m_library.clear();
			m_library_error =
				"Spotify: not logged in \xE2\x80\x94 Preferences > Tools > foo_librespot";
			KillTimer(m_wnd, kTimerRetryLibrary);
			set_status(m_library_error);
			break;
		case library_snapshot::state_t::connecting:
			m_library_error = "Spotify: connecting\xE2\x80\xA6";
			if (!snapshot.error.empty()) m_library_error += " (" + snapshot.error + ")";
			set_status(m_library_error);
			SetTimer(m_wnd, kTimerRetryLibrary, 3000, nullptr);
			break;
		case library_snapshot::state_t::error:
			m_library.clear();
			m_library_error = snapshot.error.empty() ? "Spotify: library unavailable"
													 : snapshot.error;
			KillTimer(m_wnd, kTimerRetryLibrary);
			set_status(m_library_error);
			break;
		}
		rebuild_tree();
	}

	void run_search() {
		const int length = GetWindowTextLengthW(m_search);
		std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
		if (length > 0) GetWindowTextW(m_search, wide.data(), length + 1);
		wide.resize(static_cast<size_t>(length));
		const std::string query = trim(utf8_from_wide(wide));
		if (query.empty()) {
			PostMessageW(m_wnd, kMsgClearSearch, 0, 0);
			return;
		}

		const uint64_t generation = ++m_state->search_generation;
		set_status("Searching for \xE2\x80\x9C" + query + "\xE2\x80\x9D\xE2\x80\xA6");
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state,
			[state, generation] { return state->search_generation.load() != generation; });

		fb2k::splitTask([state, generation, query, cancel] {
			search_result result;
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				result = client.search(query, kSearchLimit, cancel);
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, generation, result, error, query] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (state->search_generation.load() != generation) return;
				state->owner->on_search_loaded(query, result, error);
			});
		});
	}

	void on_search_loaded(const std::string& query, const search_result& result,
		const std::string& error) {
		m_searchResults = result;
		m_search_error = error;
		m_has_search = true;
		rebuild_tree();
		if (!error.empty()) {
			set_status(error);
		} else {
			const size_t count = result.tracks.size() + result.albums.size() + result.artists.size() +
				result.playlists.size();
			set_status("Search results for \xE2\x80\x9C" + query + "\xE2\x80\x9D (" +
				std::to_string(count) + ")");
		}
	}

	void open_item(const tree_node* node, bool play, bool force) {
		if (node == nullptr || !is_openable_item(*node)) return;
		open_target(node->uri, node->name, play, force);
	}

	void open_target(const std::string& uri, const std::string& name, bool play, bool force) {
		if (uri.empty()) return;

		const uint64_t generation = ++m_state->open_generation;
		set_status((play ? "Opening and playing " : "Opening ") + name +
			(force ? " (sync)" : "") + "\xE2\x80\xA6");

		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state,
			[state, generation] { return state->open_generation.load() != generation; });

		open_context_async(uri, name, play, force,
			[state, generation](bool ok, const std::string& message, t_size playlist) {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (state->open_generation.load() != generation) return;
				state->owner->on_open_done(ok, message, playlist);
			},
			cancel);
	}

	void on_open_done(bool ok, const std::string& message, t_size playlist) {
		if (!ok) {
			set_status(message);
			console::print("foo_librespot: ", message.c_str());
			return;
		}
		playlist_manager::ptr manager = playlist_manager::get();
		pfc::string8 name;
		if (playlist != pfc::infinite_size && manager->playlist_get_name(playlist, name)) {
			set_status(std::string("Opened ") + name.c_str());
		} else {
			set_status("Opened the Spotify playlist.");
		}
	}

	void add_target_to_active(const std::string& uri, const std::string& name) {
		if (uri.empty()) return;
		set_status("Adding " + name + " to the active playlist\xE2\x80\xA6");
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		add_context_to_active_async(
			uri,
			[state](bool ok, const std::string& message, t_size) {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!ok) state->owner->set_status(message);
				else state->owner->set_status("Added to the active playlist.");
			},
			cancel);
	}

	// Resolves `uri` on a worker thread and appends every track to
	// foobar2000's playback queue with preloaded metadata.
	void queue_target(const std::string& uri, const std::string& name) {
		if (uri.empty()) return;
		set_status("Adding " + name + " to the playback queue\xE2\x80\xA6");
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, uri, cancel] {
			resolve_result resolved;
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				resolved = client.resolve_context(uri, cancel);
				if (resolved.tracks.empty()) error = "Spotify: no tracks found";
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, resolved, error] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!error.empty()) {
					state->owner->report_error(error);
					return;
				}
				const size_t queued = state->owner->add_to_queue(resolved.tracks);
				if (queued == 0) {
					state->owner->report_error("Spotify: no tracks found");
					return;
				}
				state->owner->set_status("Added " + std::to_string(queued) +
					(queued == 1 ? " track" : " tracks") + " to the playback queue.");
			});
		});
	}

	// Appends handles for `tracks` to foobar2000's playback queue. Main thread.
	size_t add_to_queue(const std::vector<track_info>& tracks) {
		metadb_handle_list handles;
		metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
		for (const track_info& track : tracks) {
			const std::string path = track_path(track);
			if (path.empty()) continue;
			metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
			if (handle.is_empty()) continue;
			handles.add_item(handle);
			file_info_impl info;
			fill_file_info(track, info);
			hints->add_hint_forced(handle, info, t_filestats(), false);
		}
		if (handles.get_count() == 0) return 0;
		hints->on_done();
		playlist_manager::ptr manager = playlist_manager::get();
		for (t_size i = 0; i < handles.get_count(); ++i) manager->queue_add_item(handles[i]);
		return handles.get_count();
	}

	void report_error(const std::string& message) {
		set_status(message);
		console::print("foo_librespot: ", message.c_str());
	}

	// Reads the follow state of one item in the background, so the next menu
	// open labels Follow/Unfollow correctly. Never blocks the UI.
	void refresh_follow_async(const std::string& uri) {
		if (uri.empty() || m_follow_inflight.count(uri) != 0) return;
		m_follow_inflight.insert(uri);
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, uri, cancel] {
			bool following = false;
			bool ok = false;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				const std::vector<bool> flags = client.following({ uri }, cancel, 2000, 8000);
				if (!flags.empty()) {
					following = flags.front();
					ok = true;
				}
			} catch (...) {
				// The menu keeps using its likely label.
			}
			fb2k::inMainThread([state, uri, following, ok] {
				if (!state->alive.load() || state->owner == nullptr) return;
				state->owner->m_follow_inflight.erase(uri);
				if (ok) {
					state->owner->m_follow_cache[uri] = follow_state{ following, GetTickCount64() };
				}
			});
		});
	}

	void set_follow_target(const std::string& uri, const std::string& name, const std::string& kind,
		bool follow) {
		const bool album = kind == "album";
		const std::string done = follow
			? (album ? "Saved \"" + name + "\" to Your Library." : "Following " + name + ".")
			: (album ? "Removed \"" + name + "\" from Your Library." : "Unfollowed " + name + ".");
		set_status("Updating your Spotify library\xE2\x80\xA6");

		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, uri, cancel, follow, done] {
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				if (follow) client.follow({ uri }, cancel);
				else client.unfollow({ uri }, cancel);
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, uri, follow, error, done] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!error.empty()) {
					state->owner->report_error(error);
					return;
				}
				state->owner->m_follow_cache[uri] = follow_state{ follow, GetTickCount64() };
				state->owner->request_library();
				state->owner->set_status(done);
			});
		});
	}

	void new_playlist() {
		playlist_name_dialog dialog;
		if (dialog.run(m_wnd, L"New playlist", std::string()) != IDOK) return;
		const std::string name = trim(dialog.text);
		if (name.empty()) return;
		set_status("Creating the playlist\xE2\x80\xA6");

		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, name, cancel] {
			std::string error;
			std::string created = name;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				const playlist_create_result result = client.playlist_create(name, cancel);
				if (!result.playlist.name.empty()) created = result.playlist.name;
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, created, error] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!error.empty()) {
					state->owner->report_error(error);
					return;
				}
				state->owner->request_library();
				state->owner->set_status("Created playlist \"" + created + "\".");
			});
		});
	}

	void rename_playlist(const std::string& uri, const std::string& current) {
		playlist_name_dialog dialog;
		if (dialog.run(m_wnd, L"Rename playlist", current) != IDOK) return;
		const std::string name = trim(dialog.text);
		if (name.empty() || name == current) return;
		set_status("Renaming the playlist\xE2\x80\xA6");

		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, uri, name, cancel] {
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				client.playlist_rename(uri, name, cancel);
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, name, error] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!error.empty()) {
					state->owner->report_error(error);
					return;
				}
				state->owner->request_library();
				state->owner->set_status("Renamed the playlist to \"" + name + "\".");
			});
		});
	}

	void delete_playlist(const std::string& uri, const std::string& name) {
		const std::string message = "Remove \"" + name + "\" from your Spotify library?\n\n"
			"The playlist itself stays on Spotify; this only removes it from your library and "
			"from this panel.";
		popup_message_v3::query_t query;
		query.title = "Delete playlist";
		query.msg = message.c_str();
		query.buttons = popup_message_v3::buttonYes | popup_message_v3::buttonNo;
		query.defButton = popup_message_v3::buttonNo;
		query.icon = popup_message_v3::iconQuestion;
		query.wndParent = m_wnd;
		if (popup_message_v3::get()->show_query_modal(query) != popup_message_v3::buttonYes) return;

		set_status("Removing the playlist\xE2\x80\xA6");
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);
		fb2k::splitTask([state, uri, cancel] {
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				client.playlist_delete(uri, cancel);
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, error] {
				if (!state->alive.load() || state->owner == nullptr) return;
				if (!error.empty()) {
					state->owner->report_error(error);
					return;
				}
				state->owner->request_library();
				state->owner->set_status("Removed the playlist from your Spotify library.");
			});
		});
	}

	void collect_descendant_items(HTREEITEM parent, std::vector<HTREEITEM>& out) const {
		for (HTREEITEM child = TreeView_GetChild(m_tree, parent); child != nullptr;
			child = TreeView_GetNextSibling(m_tree, child)) {
			out.push_back(child);
			collect_descendant_items(child, out);
		}
	}

	// Deletes an item's children and drops their nodes from m_nodes, so stale
	// pointers are never handed out by find_node().
	void forget_children(HTREEITEM parent) {
		std::vector<HTREEITEM> doomed;
		collect_descendant_items(parent, doomed);
		while (HTREEITEM child = TreeView_GetChild(m_tree, parent)) {
			TreeView_DeleteItem(m_tree, child);
		}
		if (!doomed.empty()) {
			m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(),
							  [&doomed](const std::unique_ptr<tree_node>& node) {
								  return node->item != nullptr &&
									  std::find(doomed.begin(), doomed.end(), node->item) !=
										  doomed.end();
							  }),
				m_nodes.end());
		}
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;
	}

	void load_artist(const tree_node* node) {
		if (node == nullptr || node->item_kind != "artist" || node->loaded || node->loading) return;
		tree_node* mutableNode = find_node(node->id);
		if (mutableNode == nullptr) return;
		mutableNode->loading = true;
		add_node(mutableNode->item, node_kind::loading, "Loading albums\xE2\x80\xA6",
			std::string(), std::string(), std::string(), false);
		// The expansion that triggered this load is already in progress (or the
		// caller expands right after), so do not expand again from here.

		const uint32_t id = node->id;
		const std::string uri = node->uri;
		const std::shared_ptr<async_state> state = m_state;
		const cancel_token cancel = panel_token(state);

		fb2k::splitTask([state, id, uri, cancel] {
			artist_result result;
			std::string error;
			try {
				ensure_bridge_started(fb2k::mainAborter());
				bridge_client client;
				result = client.artist(uri, cancel);
			} catch (const cancelled&) {
				return;
			} catch (const std::exception& failure) {
				error = user_error_message(failure);
			}

			fb2k::inMainThread([state, id, result, error] {
				if (!state->alive.load() || state->owner == nullptr) return;
				state->owner->on_artist_loaded(id, result, error);
			});
		});
	}

	void on_artist_loaded(uint32_t id, const artist_result& result, const std::string& error) {
		tree_node* node = find_node(id);
		if (node == nullptr) return; // the tree was rebuilt meanwhile

		forget_children(node->item);
		node->loading = false;

		if (!error.empty()) {
			add_node(node->item, node_kind::message, error, std::string(), std::string(),
				std::string(), false);
			node->loaded = true;
			return;
		}

		const bool hasAlbums = !result.albums.empty() || !result.singles.empty();
		if (!result.top_tracks.empty()) {
			tree_node* topNode = add_node(node->item, node_kind::section, "Top tracks",
				std::string(), std::string(), std::string(), true);
			for (const track_info& track : result.top_tracks) {
				add_track_node(topNode->item, track);
			}
			TreeView_Expand(m_tree, topNode->item, TVE_EXPAND);
		}
		if (!result.albums.empty() && !result.singles.empty()) {
			tree_node* albumsNode = add_node(node->item, node_kind::section, "Albums",
				std::string(), std::string(), std::string(), true);
			for (const library_item& album : result.albums) {
				add_item_node(albumsNode->item, album);
			}
			tree_node* singlesNode = add_node(node->item, node_kind::section, "Singles",
				std::string(), std::string(), std::string(), true);
			for (const library_item& single : result.singles) {
				add_item_node(singlesNode->item, single);
			}
			TreeView_Expand(m_tree, albumsNode->item, TVE_EXPAND);
			TreeView_Expand(m_tree, singlesNode->item, TVE_EXPAND);
		} else {
			for (const library_item& album : result.albums) {
				add_item_node(node->item, album);
			}
			for (const library_item& single : result.singles) {
				add_item_node(node->item, single);
			}
		}
		if (!hasAlbums) {
			add_node(node->item, node_kind::message, "No albums", std::string(), std::string(),
				std::string(), false);
		}
		node->loaded = true;
		TreeView_Expand(m_tree, node->item, TVE_EXPAND);
	}

	// ---- notifications ------------------------------------------------

	LRESULT handle_notify(NMHDR* header) {
		if (header == nullptr || header->hwndFrom != m_tree) return 0;
		switch (header->code) {
		case NM_CLICK:
			return on_tree_click(false);
		case NM_DBLCLK:
			return on_tree_click(true);
		case NM_RCLICK:
			return on_tree_right_click();
		case NM_SETFOCUS:
			acquire_selection();
			return 0;
		case NM_KILLFOCUS:
			// The holder is released with the focus (ui_selection_holder's
			// contract); kept, a tree refresh would overwrite the selection of
			// whatever window the user is in now.
			m_selection.release();
			return 0;
		case TVN_SELCHANGEDW: {
			auto* info = reinterpret_cast<NMTREEVIEWW*>(header);
			publish_selection(node_from_pointer(info->itemNew.lParam));
			return 0;
		}
		case TVN_BEGINDRAGW: {
			auto* info = reinterpret_cast<NMTREEVIEWW*>(header);
			begin_drag(node_from_pointer(info->itemNew.lParam));
			return 0;
		}
		case TVN_ITEMEXPANDINGW: {
			auto* info = reinterpret_cast<NMTREEVIEWW*>(header);
			if (info->action == TVE_EXPAND) {
				tree_node* node = node_from_pointer(info->itemNew.lParam);
				if (node != nullptr && node->item_kind == "artist") load_artist(node);
			}
			return 0;
		}
		case TVN_KEYDOWN: {
			auto* info = reinterpret_cast<NMTVKEYDOWN*>(header);
			if (info->wVKey == VK_RETURN) {
				tree_node* node = selected_node();
				if (node != nullptr && is_openable_item(*node)) open_item(node, true, false);
			}
			return 0;
		}
		default:
			break;
		}
		return 0;
	}

	tree_node* node_at_cursor() {
		POINT point = {};
		GetCursorPos(&point);
		ScreenToClient(m_tree, &point);
		TVHITTESTINFO hit = {};
		hit.pt = point;
		const HTREEITEM item = reinterpret_cast<HTREEITEM>(
			SendMessageW(m_tree, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));
		if (item == nullptr || (hit.flags & TVHT_ONITEMBUTTON) != 0) return nullptr;
		if ((hit.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON)) == 0) return nullptr;
		return node_from_item(item);
	}

	LRESULT on_tree_click(bool doubleClick) {
		// Any click replaces a pending single-click open.
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;

		// Some comctl32 versions also deliver NM_CLICK for the second button-up
		// of a double click; ignore that one.
		if (!doubleClick && GetTickCount64() < m_ignore_click_until) return 0;

		tree_node* node = node_at_cursor();
		if (node == nullptr) return 0;

		if (node->kind == node_kind::section ||
			(node->kind == node_kind::item && node->item_kind == "artist")) {
			if (doubleClick) return 0; // the first click already toggled it
			const bool expanded = TreeView_GetItemState(m_tree, node->item, TVIS_EXPANDED) != 0;
			TreeView_Expand(m_tree, node->item, expanded ? TVE_COLLAPSE : TVE_EXPAND);
			return 0;
		}
		if (!is_openable_item(*node)) return 0;

		if (doubleClick) {
			m_ignore_click_until = GetTickCount64() + GetDoubleClickTime();
			TreeView_SelectItem(m_tree, node->item);
			open_item(node, true, false);
			return 0;
		}
		m_pending_click = node->id;
		SetTimer(m_wnd, kTimerSingleClick, GetDoubleClickTime(), nullptr);
		return 0;
	}

	enum menu_command : unsigned {
		kMenuOpen = 1,
		kMenuPlay,
		kMenuAddToPlaylist,
		kMenuQueue,
		kMenuSync,
		kMenuFollow,
		kMenuRename,
		kMenuDelete,
		kMenuNewPlaylist,
		kMenuCopy,
	};

	static bool kind_queues_tracks(const std::string& kind) {
		return kind == "track" || kind == "album" || kind == "playlist" || kind == "collection" ||
			kind == "top_tracks";
	}

	static const wchar_t* follow_label(const std::string& kind, bool followed) {
		if (kind == "album") {
			return followed ? L"Remove from Your Library" : L"Save to Your Library";
		}
		return followed ? L"Unfollow" : L"Follow";
	}

	// ---- selection and drag ---------------------------------------------

	// Handles for a track node, with its info preloaded when known. Albums,
	// playlists and artists would need resolving first, so they give none.
	metadb_handle_list node_handles(const tree_node* node) const {
		metadb_handle_list handles;
		if (node == nullptr || node->kind != node_kind::item || node->item_kind != "track") {
			return handles;
		}
		const std::string path = node->track ? track_path(*node->track) : node->uri;
		if (!is_spotify_track_uri(path)) return handles;
		metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
		if (handle.is_empty()) return handles;
		if (node->track) {
			metadb_hint_list_v3::ptr hints = metadb_hint_list_v3::create();
			file_info_impl info;
			fill_file_info(*node->track, info);
			hints->add_hint_forced(handle, info, t_filestats(), false);
			hints->on_done();
		}
		handles.add_item(handle);
		return handles;
	}

	// The tree took the focus: it now owns foobar2000's "current selection"
	// (Properties, the context menu, the selection-following components).
	void acquire_selection() {
		if (m_selection.is_empty()) m_selection = ui_selection_manager::get()->acquire();
		publish_selection(selected_node());
	}

	void publish_selection(const tree_node* node) {
		if (m_selection.is_empty()) return;
		m_selection->set_selection(node_handles(node));
	}

	void begin_drag(const tree_node* node) {
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;
		const metadb_handle_list handles = node_handles(node);
		if (handles.get_count() == 0) return;
		pfc::com_ptr_t<IDataObject> data =
			playlist_incoming_item_filter::get()->create_dataobject_ex(handles);
		if (data.is_empty()) return;
		IDropSource* source = new tree_drop_source();
		DWORD effect = DROPEFFECT_NONE;
		DoDragDrop(data.get_ptr(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
			&effect);
		source->Release();
	}

	// ---- context menu ---------------------------------------------------

	LRESULT on_tree_right_click() {
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;
		tree_node* node = node_at_cursor();
		if (node == nullptr) return 0; // the tree sends WM_CONTEXTMENU instead
		POINT point = {};
		GetCursorPos(&point);
		show_context_menu(node, point);
		return 1; // handled: no WM_CONTEXTMENU on top
	}

	// WM_CONTEXTMENU from the tree: the Apps key and Shift+F10 (lparam -1)
	// open the menu for the selected item, below it.
	void on_tree_context_menu(LPARAM lparam) {
		if (lparam != static_cast<LPARAM>(-1)) return; // mouse: NM_RCLICK handled it
		tree_node* node = selected_node();
		if (node == nullptr) return;
		RECT rect = {};
		POINT point = {};
		if (TreeView_GetItemRect(m_tree, node->item, &rect, TRUE)) {
			point.x = rect.left;
			point.y = rect.bottom;
		}
		ClientToScreen(m_tree, &point);
		show_context_menu(node, point);
	}

	void show_context_menu(tree_node* node, POINT point) {
		KillTimer(m_wnd, kTimerSingleClick);
		m_pending_click = 0;
		const bool openable = is_openable_item(*node);
		const bool playlistSection =
			node->kind == node_kind::section && node->section_id == "playlists";
		if (!openable && !playlistSection) return;
		TreeView_SelectItem(m_tree, node->item);

		const std::string uri = node->uri;
		const std::string name = node->name;
		const std::string kind = node->item_kind;
		const bool editablePlaylist = kind == "playlist" && node->editable;
		// On Spotify, deleting your own playlist is unfollowing it, so an
		// editable playlist offers Delete... instead of Unfollow.
		const bool followable = openable && !editablePlaylist &&
			(kind == "artist" || kind == "album" || kind == "playlist");
		bool followed = node->likely_followed;
		if (followable) {
			const auto cached = m_follow_cache.find(uri);
			if (cached != m_follow_cache.end() &&
				GetTickCount64() - cached->second.at < kFollowTtlMs) {
				followed = cached->second.following;
			} else {
				refresh_follow_async(uri); // right next time
			}
		}

		HMENU menu = CreatePopupMenu();
		if (menu == nullptr) return;
		if (openable) {
			AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open");
			AppendMenuW(menu, MF_STRING, kMenuPlay, L"Open and play");
			AppendMenuW(menu, MF_STRING, kMenuAddToPlaylist, L"Add to current playlist");
			if (kind_queues_tracks(kind)) {
				AppendMenuW(menu, MF_STRING, kMenuQueue, L"Add to playback queue");
			}
			// Tracks open into the browse playlist, which is never synced.
			if (kind != "track") AppendMenuW(menu, MF_STRING, kMenuSync, L"Sync now");
		}
		if (followable || editablePlaylist || playlistSection) {
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			if (followable) {
				AppendMenuW(menu, MF_STRING, kMenuFollow, follow_label(kind, followed));
			}
			if (editablePlaylist) {
				AppendMenuW(menu, MF_STRING, kMenuRename, L"Rename...");
				AppendMenuW(menu, MF_STRING, kMenuDelete, L"Delete...");
			}
			if (playlistSection) {
				AppendMenuW(menu, MF_STRING, kMenuNewPlaylist, L"New playlist...");
			}
		}
		if (openable) {
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(menu, MF_STRING, kMenuCopy, L"Copy link");
			SetMenuDefaultItem(menu, kMenuOpen, FALSE);
		}

		const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x,
			point.y, 0, m_wnd, nullptr);
		DestroyMenu(menu);

		switch (command) {
		case kMenuOpen: open_target(uri, name, false, false); break;
		case kMenuPlay: open_target(uri, name, true, false); break;
		case kMenuAddToPlaylist: add_target_to_active(uri, name); break;
		case kMenuQueue: queue_target(uri, name); break;
		case kMenuSync: open_target(uri, name, false, true); break;
		case kMenuFollow: set_follow_target(uri, name, kind, !followed); break;
		case kMenuRename: rename_playlist(uri, name); break;
		case kMenuDelete: delete_playlist(uri, name); break;
		case kMenuNewPlaylist: new_playlist(); break;
		case kMenuCopy: copy_to_clipboard(web_link(uri)); break;
		default: break;
		}
	}

	void copy_to_clipboard(const std::string& text) {
		const std::wstring wide = wide_from_utf8(text);
		if (!OpenClipboard(m_wnd)) return;
		EmptyClipboard();
		const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (memory != nullptr) {
			void* target = GlobalLock(memory);
			if (target != nullptr) {
				memcpy(target, wide.c_str(), bytes);
				GlobalUnlock(memory);
				if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
			} else {
				GlobalFree(memory);
			}
		}
		CloseClipboard();
	}

	static constexpr int kSearchEditId = 3001;
	static constexpr int kRefreshButtonId = 3002;
	static constexpr int kTreeId = 3003;
	static constexpr int kStatusId = 3004;

	spotify_panel_host& m_host;
	std::shared_ptr<async_state> m_state;
	ui_selection_holder::ptr m_selection;

	HWND m_wnd = nullptr;
	HWND m_search = nullptr;
	HWND m_refresh = nullptr;
	HWND m_tree = nullptr;
	HWND m_status = nullptr;
	HBRUSH m_background_brush = nullptr;
	int m_dpi = 96;

	fb2k::CCoreDarkModeHooks m_dark;

	std::vector<std::unique_ptr<tree_node>> m_nodes;
	uint32_t m_next_node_id = 0;
	uint32_t m_pending_click = 0;
	ULONGLONG m_ignore_click_until = 0;

	// Follow state for the context menu, refreshed in the background.
	struct follow_state {
		bool following = false;
		ULONGLONG at = 0;
	};
	static constexpr ULONGLONG kFollowTtlMs = 5ULL * 60 * 1000;
	std::map<std::string, follow_state> m_follow_cache;
	std::set<std::string> m_follow_inflight;

	std::vector<library_section> m_library;
	bool m_library_loading = false;
	std::string m_library_error;
	library_snapshot::state_t m_library_state = library_snapshot::state_t::connecting;

	bool m_has_search = false;
	search_result m_searchResults;
	std::string m_search_error;
};

std::unique_ptr<spotify_panel> create_spotify_panel(HWND parent, spotify_panel_host& host,
	bool visible) {
	return std::make_unique<spotify_panel_impl>(parent, host, visible);
}

namespace {

// Default UI element: the panel plus colours/fonts from the DUI callback.
class spotify_tab_instance : public ui_element_instance, private spotify_panel_host {
public:
	spotify_tab_instance(HWND parent, ui_element_config::ptr config,
		ui_element_instance_callback::ptr callback)
		: m_callback(callback), m_config(config) {
		m_panel = create_spotify_panel(parent, *this, true);
	}

	HWND get_wnd() override { return m_panel ? m_panel->get_wnd() : nullptr; }

	void set_configuration(ui_element_config::ptr config) override { m_config = config; }
	ui_element_config::ptr get_configuration() override {
		return m_config.is_valid() ? m_config : ui_element_config::g_create_empty(kSpotifyTabGuid);
	}
	GUID get_guid() override { return kSpotifyTabGuid; }
	GUID get_subclass() override { return ui_element_subclass_media_library_viewers; }
	double get_focus_priority() override { return 1.0; }
	void set_default_focus() override {
		if (m_panel) m_panel->set_default_focus();
	}

	void notify(const GUID& what, t_size, const void*, t_size) override {
		if (what == ui_element_notify_colors_changed || what == ui_element_notify_font_changed) {
			if (m_panel) m_panel->on_style_changed();
		}
	}

private:
	bool panel_dark_mode() override { return m_callback->is_dark_mode(); }
	bool panel_background(COLORREF& out) override {
		t_ui_color color = 0;
		if (!m_callback->query_color(ui_color_background, color)) return false;
		out = color;
		return true;
	}
	bool panel_text(COLORREF& out) override {
		t_ui_color color = 0;
		if (!m_callback->query_color(ui_color_text, color)) return false;
		out = color;
		return true;
	}
	HFONT panel_list_font() override { return m_callback->query_font_ex(ui_font_lists); }
	HFONT panel_default_font() override { return m_callback->query_font_ex(ui_font_default); }

	ui_element_instance_callback::ptr m_callback;
	ui_element_config::ptr m_config;
	// Last: destroyed (window first) before the callback it uses.
	std::unique_ptr<spotify_panel> m_panel;
};

class spotify_tab_element : public ui_element {
public:
	GUID get_guid() override { return kSpotifyTabGuid; }
	GUID get_subclass() override { return ui_element_subclass_media_library_viewers; }
	void get_name(pfc::string_base& out) override { out = "Spotify"; }

	ui_element_instance_ptr instantiate(fb2k::hwnd_t parent, ui_element_config::ptr config,
		ui_element_instance_callback::ptr callback) override {
		return fb2k::service_new<spotify_tab_instance>(static_cast<HWND>(parent), config,
			callback);
	}

	ui_element_config::ptr get_default_configuration() override {
		return ui_element_config::g_create_empty(kSpotifyTabGuid);
	}
	ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr) override {
		return nullptr;
	}
	bool get_description(pfc::string_base& out) override {
		out = "Browse your Spotify library and open playlists, albums, artists and Liked Songs "
			  "as synced foobar2000 playlists.";
		return true;
	}
};

static service_factory_single_t<spotify_tab_element> g_spotify_tab_factory;

} // namespace

} // namespace flp
