// The two windows of the app: "All devices" (owner-drawn cards with drag-and-drop order) and
// "Settings" (native controls, applied immediately). Both are plain top-level windows owned by the
// tray window, follow the light/dark theme, and are DPI-scaled from the window's own DPI.
#include "ui.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "i18n.h"
#include "widget.h"

namespace ui {
namespace {

HINSTANCE g_inst;
bool g_dark = false;
HWND g_devWnd = nullptr;
HWND g_setWnd = nullptr;

// ---- helpers ----------------------------------------------------------------------------------

int Dpi(HWND w) {
    UINT d = w ? GetDpiForWindow(w) : 0;
    return d ? (int)d : 96;
}

HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback = L"Segoe UI") {
    for (const wchar_t* f : { face, fallback }) {
        // Antialiased rather than ClearType: on the dark cards the subpixel fringes make some
        // percentages look bolder than their neighbours depending on where they land on the pixel grid.
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

void UseDarkTitleBar(HWND w, bool dark) {
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static Fn set = [] {
        HMODULE d = LoadLibraryW(L"dwmapi.dll");
        return d ? (Fn)GetProcAddress(d, "DwmSetWindowAttribute") : nullptr;
    }();
    if (!set) return;
    BOOL v = dark ? TRUE : FALSE;
    set(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof(v));
}

void ThemeControl(HWND w, bool dark, const wchar_t* cls = L"DarkMode_Explorer") {
    using Fn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static Fn fn = [] {
        HMODULE u = LoadLibraryW(L"uxtheme.dll");
        return u ? (Fn)GetProcAddress(u, "SetWindowTheme") : nullptr;
    }();
    if (fn) fn(w, dark ? cls : nullptr, nullptr);
}

COLORREF Mix(COLORREF a, COLORREF b, int percentB) {
    auto mix = [&](int x, int y) { return (x * (100 - percentB) + y * percentB) / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

struct Palette {
    COLORREF bg, card, hover, line, text, sub, good, red;
};

Palette Theme() {
    if (g_dark) return { RGB(28, 28, 30), RGB(43, 43, 46), RGB(56, 56, 60), RGB(68, 68, 72), RGB(245, 245, 247),
                         RGB(166, 166, 172), RGB(48, 209, 88), RGB(255, 105, 97) };
    return { RGB(246, 246, 248), RGB(255, 255, 255), RGB(240, 240, 243), RGB(226, 226, 230), RGB(24, 24, 26),
             RGB(104, 104, 110), RGB(0, 140, 60), RGB(200, 32, 40) };
}

wchar_t KindGlyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;
        case DeviceKind::Mouse:    return 0xE962;
        case DeviceKind::Keyboard: return 0xE765;
        case DeviceKind::Gamepad:  return 0xE7FC;
        case DeviceKind::Phone:    return 0xE8EA;
        default:                   return bluetooth ? 0xE702 : 0xE772;
    }
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void RoundBox(HDC dc, const RECT& r, int radius, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

// =============================================================================================
// "All devices"
// =============================================================================================

std::vector<DeviceCard> g_cards;
DevicesActions g_devActions;
int g_scroll = 0;
int g_dragFrom = -1;  // card under the mouse when the drag started
int g_dragTo = -1;    // insertion position while dragging
POINT g_dragPt{};
bool g_dragActive = false;

int CardHeight(HWND w) { return MulDiv(64, Dpi(w), 96); }
int CardGap(HWND w) { return MulDiv(8, Dpi(w), 96); }
int ContentTop(HWND w) { return MulDiv(92, Dpi(w), 96); }
int CardTop(HWND w, int i) {
    return ContentTop(w) + MulDiv(8, Dpi(w), 96) - g_scroll + i * (CardHeight(w) + CardGap(w));
}

int MaxScroll(HWND w) {
    RECT rc;
    GetClientRect(w, &rc);
    int content = MulDiv(8, Dpi(w), 96) + (int)g_cards.size() * (CardHeight(w) + CardGap(w));
    int room = rc.bottom - ContentTop(w);
    return std::max(0, content - room);
}

void ClampScroll(HWND w) {
    g_scroll = std::max(0, std::min(g_scroll, MaxScroll(w)));
}

int HitCard(HWND w, int y) {
    if (y < ContentTop(w)) return -1;
    for (int i = 0; i < (int)g_cards.size(); i++) {
        int top = CardTop(w, i);
        if (y >= top && y < top + CardHeight(w)) return i;
    }
    return -1;
}

int DropIndex(HWND w, int y) {
    int step = CardHeight(w) + CardGap(w);
    int top = ContentTop(w) + MulDiv(8, Dpi(w), 96) - g_scroll;
    int i = (y - top + step / 2) / step;
    return std::max(0, std::min((int)g_cards.size(), i));
}

COLORREF CardAccent(const DeviceCard& c) {
    if (c.custom) return RGB(c.customColor.r, c.customColor.g, c.customColor.b);
    if (c.accent != Accent::None) {
        BrandColor bc = AccentColor(c.accent, !g_dark);
        return RGB(bc.r, bc.g, bc.b);
    }
    return Theme().text;
}

std::wstring CardStatus(const DeviceCard& c) {
    std::wstring s = c.source;
    if (!c.connected) s = T(L"off / not connected");
    else if (c.asleep && c.percent < 0) s += std::wstring(L", ") + T(L"asleep");
    if (c.charging) s += std::wstring(L", ") + T(L"charging");
    if (c.hidden) s += std::wstring(L", ") + T(L"hidden");
    return s;
}

// Created once per repaint: creating and deleting a font per card made GDI reuse handles in a way
// that sometimes drew a percentage with a stale realization of an earlier font.
struct CardFonts {
    HFONT icon, pct, name, sub;
};

void DrawCard(HDC dc, const RECT& r, const DeviceCard& c, bool dragging, const CardFonts& f) {
    Palette p = Theme();
    int dpi = Dpi(g_devWnd);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    RoundBox(dc, r, S(12), dragging ? p.hover : p.card);

    COLORREF accent = CardAccent(c);
    COLORREF text = p.text;
    COLORREF sub = p.sub;
    if (c.hidden) { text = Mix(text, p.card, 45); sub = Mix(sub, p.card, 35); accent = Mix(accent, p.card, 45); }

    // Icon
    HGDIOBJ old = SelectObject(dc, f.icon);
    SetTextColor(dc, accent);
    wchar_t glyph = KindGlyph(c.kind, c.bluetooth);
    RECT ir{ r.left + S(16), r.top, r.left + S(16) + S(30), r.bottom };
    DrawTextW(dc, &glyph, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);

    // Percentage on the right
    std::wstring pct = c.percent >= 0 ? std::to_wstring(c.percent) + L"%" : L"-";
    old = SelectObject(dc, f.pct);
    SetTextColor(dc, c.low && !c.charging ? p.red : text);
    SIZE ps{};
    GetTextExtentPoint32W(dc, pct.c_str(), (int)pct.size(), &ps);
    int pctRight = r.right - S(16);
    RECT pr{ pctRight - ps.cx, r.top, pctRight, r.top + S(38) };
    DrawTextW(dc, pct.c_str(), -1, &pr, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);

    if (c.charging) {  // bolt left of the percentage
        old = SelectObject(dc, f.icon);
        SetTextColor(dc, RGB(0, 150, 240));
        wchar_t b = 0xE945;
        RECT br{ pctRight - ps.cx - S(22), r.top, pctRight - ps.cx - S(2), r.top + S(38) };
        DrawTextW(dc, &b, 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    int textLeft = r.left + S(56);
    int textRight = std::max<int>(textLeft + S(40), (int)(pctRight - ps.cx - S(28)));

    // Name (a star in front of the name marks a favorite)
    std::wstring name = (c.favorite ? std::wstring(L"\u2605  ") : L"") + c.name;
    old = SelectObject(dc, f.name);
    SetTextColor(dc, text);
    RECT nr{ textLeft, r.top + S(10), textRight, r.top + S(34) };
    DrawTextW(dc, name.c_str(), -1, &nr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);

    // Source, charging, hidden
    old = SelectObject(dc, f.sub);
    SetTextColor(dc, sub);
    std::wstring status = CardStatus(c);
    RECT sr{ textLeft, r.top + S(34), textRight, r.top + S(54) };
    DrawTextW(dc, status.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);

    // Level bar under the percentage
    if (c.percent >= 0) {
        int barW = std::max<int>(S(56), (int)ps.cx);
        int x = pctRight - barW;
        int y = r.top + S(50);
        RECT track{ x, y, x + barW, y + S(3) };
        Fill(dc, track, p.line);
        int fill = std::max(c.percent > 0 ? 1 : 0, barW * c.percent / 100);
        RECT part{ x, y, x + fill, y + S(3) };
        Fill(dc, part, c.charging ? RGB(0, 150, 240) : accent);
    }
}

void PaintDevices(HWND w) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(w, &ps); return; }

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    Palette p = Theme();
    Fill(dc, rc, p.bg);
    SetBkMode(dc, TRANSPARENT);

    int dpi = Dpi(w);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

    // Header
    HFONT title = MakeFont(S(20), FW_SEMIBOLD, L"Segoe UI Variable Display");
    HGDIOBJ old = SelectObject(dc, title);
    SetTextColor(dc, p.text);
    RECT tr{ S(20), S(14), W - S(20), S(42) };
    DrawTextW(dc, T(L"All devices"), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(title);

    HFONT sub = MakeFont(S(12), FW_NORMAL, L"Segoe UI Variable Text");
    old = SelectObject(dc, sub);
    SetTextColor(dc, p.sub);
    RECT sr{ S(20), S(44), W - S(20), S(88) };
    DrawTextW(dc, T(L"Drag the cards to change the order. Right-click a card for more actions."), -1, &sr,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(sub);

    RECT line{ 0, ContentTop(w) - S(2), W, ContentTop(w) };
    Fill(dc, line, p.line);

    HRGN clip = CreateRectRgn(0, ContentTop(w), W, H);
    SelectClipRgn(dc, clip);
    int cardH = CardHeight(w), gap = CardGap(w);
    CardFonts fonts;
    fonts.icon = MakeFont(S(26), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    fonts.pct = MakeFont(S(20), FW_SEMIBOLD, L"Segoe UI Variable Text");
    fonts.name = MakeFont(S(15), FW_SEMIBOLD, L"Segoe UI Variable Text");
    fonts.sub = MakeFont(S(12), FW_NORMAL, L"Segoe UI Variable Text");
    for (int i = 0; i < (int)g_cards.size(); i++) {
        int top = CardTop(w, i);
        if (top + cardH < ContentTop(w) || top > H) continue;
        RECT r{ S(14), top, W - S(14), top + cardH };
        DrawCard(dc, r, g_cards[i], g_dragActive && i == g_dragFrom, fonts);
    }
    DeleteObject(fonts.icon);
    DeleteObject(fonts.pct);
    DeleteObject(fonts.name);
    DeleteObject(fonts.sub);
    if (g_dragActive && g_dragTo >= 0) {
        int y = ContentTop(w) + S(8) - g_scroll + g_dragTo * (cardH + gap) - gap / 2;
        RECT marker{ S(14), y - S(1), W - S(14), y + S(2) };
        Fill(dc, marker, p.good);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // A slim scrollbar so it is obvious that there are more cards below the fold.
    int maxScroll = MaxScroll(w);
    if (maxScroll > 0) {
        int viewTop = ContentTop(w) + S(8), viewH = H - viewTop - S(8);
        int total = viewH + maxScroll;
        int thumbH = std::max<int>(S(24), viewH * viewH / total);
        int thumbY = viewTop + (viewH - thumbH) * g_scroll / maxScroll;
        RECT thumb{ W - S(8), thumbY, W - S(5), thumbY + thumbH };
        RoundBox(dc, thumb, S(3), p.line);
    }

    if (g_cards.empty()) {
        HFONT f = MakeFont(S(14), FW_NORMAL, L"Segoe UI Variable Text");
        old = SelectObject(dc, f);
        SetTextColor(dc, p.sub);
        RECT er{ S(20), ContentTop(w) + S(24), W - S(20), ContentTop(w) + S(60) };
        DrawTextW(dc, T(L"No devices with battery info found"), -1, &er, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(f);
    }

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(w, &ps);
}

void ReorderLocal(int from, int to) {
    if (from < 0 || from >= (int)g_cards.size()) return;
    if (to > from) to--;
    to = std::max(0, std::min(to, (int)g_cards.size() - 1));
    if (to == from) return;
    DeviceCard moved = g_cards[from];
    g_cards.erase(g_cards.begin() + from);
    g_cards.insert(g_cards.begin() + to, moved);
    if (g_devActions.reorder) {
        std::vector<std::wstring> keys;
        for (const auto& c : g_cards) keys.push_back(c.key);
        g_devActions.reorder(keys);
    }
}

void DevicesContextMenu(HWND w, int x, int y) {
    POINT pt{ x, y };
    ScreenToClient(w, &pt);
    int i = HitCard(w, pt.y);
    if (i < 0) return;
    const DeviceCard& c = g_cards[i];
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, c.hidden ? T(L"Show") : T(L"Hide"));
    AppendMenuW(m, MF_STRING | (c.favorite ? MF_CHECKED : 0), 2, T(L"Favorite"));
    AppendMenuW(m, MF_STRING, 3, T(L"Rename..."));
    POINT screen{ x, y };
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen.x, screen.y, 0, w, nullptr);
    DestroyMenu(m);
    std::wstring key = c.key;
    if (cmd == 1 && g_devActions.toggleHidden) g_devActions.toggleHidden(key);
    if (cmd == 2 && g_devActions.toggleFavorite) g_devActions.toggleFavorite(key);
    if (cmd == 3 && g_devActions.rename) g_devActions.rename(key);
    InvalidateRect(w, nullptr, FALSE);
}

LRESULT CALLBACK DevicesProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            UseDarkTitleBar(w, g_dark);
            return 0;
        case WM_PAINT: PaintDevices(w); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: ClampScroll(w); InvalidateRect(w, nullptr, FALSE); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            int dpi = Dpi(w);
            mm->ptMinTrackSize.x = MulDiv(380, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(300, dpi, 96);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scroll -= delta / WHEEL_DELTA * (CardHeight(w) + CardGap(w)) * 2;
            ClampScroll(w);
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(w);
            int y = GET_Y_LPARAM(lp);
            int i = HitCard(w, y);
            if (i >= 0) {
                g_dragFrom = i;
                g_dragTo = i;
                g_dragPt = { GET_X_LPARAM(lp), y };
                g_dragActive = false;
                SetCapture(w);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (GetCapture() != w || g_dragFrom < 0) return 0;
            int y = GET_Y_LPARAM(lp);
            if (!g_dragActive && (std::abs(y - g_dragPt.y) > 6 || std::abs(GET_X_LPARAM(lp) - g_dragPt.x) > 6)) g_dragActive = true;
            if (g_dragActive) {
                g_dragTo = DropIndex(w, y);
                // Scroll when the pointer reaches the header or the bottom edge.
                RECT rc;
                GetClientRect(w, &rc);
                if (y < ContentTop(w) + 10) g_scroll = std::max(0, g_scroll - CardHeight(w) / 3);
                if (y > rc.bottom - 10) g_scroll = std::min(MaxScroll(w), g_scroll + CardHeight(w) / 3);
            }
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            int from = g_dragFrom, to = g_dragTo;
            bool active = g_dragActive;
            g_dragFrom = g_dragTo = -1;
            g_dragActive = false;
            if (GetCapture() == w) ReleaseCapture();
            if (active && from >= 0) ReorderLocal(from, to);
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_dragFrom = g_dragTo = -1;
            g_dragActive = false;
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_CONTEXTMENU: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (x == -1 && y == -1) {  // keyboard: use the first card
                RECT rc;
                GetClientRect(w, &rc);
                POINT pt{ rc.right / 2, ContentTop(w) + CardHeight(w) / 2 };
                ClientToScreen(w, &pt);
                x = pt.x;
                y = pt.y;
            }
            DevicesContextMenu(w, x, y);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { DestroyWindow(w); return 0; }
            if (wp == VK_DOWN || wp == VK_UP || wp == VK_NEXT || wp == VK_PRIOR || wp == VK_HOME || wp == VK_END) {
                int step = CardHeight(w) + CardGap(w);
                if (wp == VK_DOWN) g_scroll += step;
                if (wp == VK_UP) g_scroll -= step;
                if (wp == VK_NEXT) g_scroll += step * 3;
                if (wp == VK_PRIOR) g_scroll -= step * 3;
                if (wp == VK_HOME) g_scroll = 0;
                if (wp == VK_END) g_scroll = MaxScroll(w);
                ClampScroll(w);
                InvalidateRect(w, nullptr, FALSE);
                return 0;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(w); return 0;
        case WM_DESTROY:
            if (GetCapture() == w) ReleaseCapture();
            g_devWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void RegisterDevicesClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DevicesProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WirelessBatteryDevices";
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    done = true;
}

// =============================================================================================
// "Settings"
// =============================================================================================

enum : int {
    ID_MODE_WIDGET = 1001, ID_MODE_TRAY, ID_BACKGROUND, ID_STACKED,
    ID_HIDE_LAST, ID_ONLY_FAVORITES, ID_POLL,
    ID_THEME, ID_ACCENT, ID_ACCENT_COLOR,
    ID_AUTOSTART, ID_LANGUAGE, ID_MONITOR,
    ID_ALL_DEVICES, ID_REFRESH, ID_CHECK_UPDATES, ID_CLOSE, ID_VERSION,
};

SettingsState g_state;
SettingsActions g_setActions;
std::vector<std::wstring> g_langCodes;
std::vector<std::wstring> g_monitorIds;
std::vector<int> g_pollValues{ 1, 5, 30, 60 };
HBRUSH g_bgBrush = nullptr;
HFONT g_setFont = nullptr;

COLORREF SettingBg() { return g_dark ? RGB(32, 32, 34) : RGB(250, 250, 251); }
COLORREF SettingText() { return g_dark ? RGB(240, 240, 242) : RGB(22, 22, 24); }

void RebuildBgBrush() {
    if (g_bgBrush) DeleteObject(g_bgBrush);
    g_bgBrush = CreateSolidBrush(SettingBg());
}

HWND Item(HWND w, int id) { return GetDlgItem(w, id); }
int ComboSel(HWND w, int id) {
    int i = (int)SendDlgItemMessageW(w, id, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : i;
}
void SetCombo(HWND w, int id, int sel) { SendDlgItemMessageW(w, id, CB_SETCURSEL, (WPARAM)sel, 0); }

void FillCombo(HWND w, int id, const std::vector<std::wstring>& items, int sel) {
    HWND c = Item(w, id);
    if (!c) return;
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (const auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SetCombo(w, id, sel);
}

std::vector<std::wstring> ThemeItems() { return { T(L"Automatic"), T(L"Light"), T(L"Dark") }; }
std::vector<std::wstring> AccentItems() { return { T(L"No accent"), T(L"Brand colors"), T(L"Custom color...") }; }
std::vector<std::wstring> PollItems() {
    std::vector<std::wstring> v;
    for (int s : g_pollValues) {
        if (s == 60) v.push_back(T(L"1 minute"));
        else if (s == 1) v.push_back(T(L"1 second"));
        else v.push_back(TF(L"{n} seconds", { { L"n", std::to_wstring(s) } }));
    }
    return v;
}

int PollIndex() {
    for (int i = 0; i < (int)g_pollValues.size(); i++)
        if (g_pollValues[i] == g_state.pollSeconds) return i;
    return (int)g_pollValues.size() - 1;
}

int MonitorIndex() {
    for (int i = 0; i < (int)g_monitorIds.size(); i++)
        if (g_monitorIds[i] == g_state.monitor) return i;
    return 0;
}

int LanguageIndex() {
    if (g_state.language.empty()) return 0;
    for (int i = 0; i < (int)g_langCodes.size(); i++)
        if (!_wcsicmp(g_langCodes[i].c_str(), g_state.language.c_str())) return i + 1;
    return 0;
}

void SetSettingTexts(HWND w) {
    SetWindowTextW(w, (std::wstring(T(L"Settings")) + L" - Wireless Battery " + g_state.version).c_str());
    SetWindowTextW(Item(w, ID_MODE_WIDGET), T(L"Show on the taskbar (icon + percentage)"));
    SetWindowTextW(Item(w, ID_MODE_TRAY), T(L"Show as tray icons"));
    SetWindowTextW(Item(w, ID_BACKGROUND), T(L"Background behind taskbar text (for transparent taskbars)"));
    SetWindowTextW(Item(w, ID_STACKED), T(L"Icons above percentages (narrower)"));
    SetWindowTextW(Item(w, ID_HIDE_LAST), T(L"Hide last known / asleep devices"));
    SetWindowTextW(Item(w, ID_ONLY_FAVORITES), T(L"Show only favorites"));
    SetWindowTextW(Item(w, ID_AUTOSTART), T(L"Start with Windows"));
    SetWindowTextW(Item(w, ID_ALL_DEVICES), T(L"All devices..."));
    SetWindowTextW(Item(w, ID_REFRESH), T(L"Refresh now"));
    SetWindowTextW(Item(w, ID_CHECK_UPDATES), T(L"Check for updates..."));
    SetWindowTextW(Item(w, ID_CLOSE), T(L"Close"));
    std::wstring version = L"Wireless Battery " + g_state.version;
    SetWindowTextW(Item(w, ID_VERSION), version.c_str());
    // Labels are found by id; the section titles are painted by the window.
    for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[16] = {};
        GetClassNameW(c, cls, 16);
        if (_wcsicmp(cls, L"Static") == 0) {
            int id = GetDlgCtrlID(c);
            switch (id) {
                case 2101: SetWindowTextW(c, T(L"Polling rate")); break;
                case 2102: SetWindowTextW(c, T(L"Theme")); break;
                case 2103: SetWindowTextW(c, T(L"Accent color")); break;
                case 2104: SetWindowTextW(c, T(L"Language")); break;
                case 2105: SetWindowTextW(c, T(L"Show the widget on")); break;
            }
        }
    }
    // The swatch is only meaningful in the custom-colour mode.
    EnableWindow(Item(w, ID_ACCENT_COLOR), g_state.accent == 2);
    InvalidateRect(w, nullptr, FALSE);  // the painted section titles
    FillCombo(w, ID_THEME, ThemeItems(), g_state.theme);
    FillCombo(w, ID_ACCENT, AccentItems(), g_state.accent);
    FillCombo(w, ID_POLL, PollItems(), PollIndex());
    std::vector<std::wstring> langs{ T(L"Automatic (Windows language)") };
    auto available = i18n::Available();
    for (const auto& code : g_langCodes) {
        std::wstring label = code;
        for (const auto& l : available)
            if (!_wcsicmp(l.code.c_str(), code.c_str())) { label = l.name; break; }
        langs.push_back(label);
    }
    FillCombo(w, ID_LANGUAGE, langs, LanguageIndex());
    if (Item(w, ID_MONITOR)) {
        std::vector<std::wstring> mons{ T(L"All monitors"), T(L"Main monitor only") };
        for (const auto& o : widget::OtherMonitors()) mons.push_back(o.label);
        while (mons.size() < g_monitorIds.size()) mons.push_back(T(L"Chosen monitor (not connected, shown on main)"));
        FillCombo(w, ID_MONITOR, mons, MonitorIndex());
    }
}

void ReadSettings(HWND w) {
    g_state.widget = IsDlgButtonChecked(w, ID_MODE_WIDGET) == BST_CHECKED;
    g_state.background = IsDlgButtonChecked(w, ID_BACKGROUND) == BST_CHECKED;
    g_state.stacked = IsDlgButtonChecked(w, ID_STACKED) == BST_CHECKED;
    g_state.hideLastKnown = IsDlgButtonChecked(w, ID_HIDE_LAST) == BST_CHECKED;
    g_state.onlyFavorites = IsDlgButtonChecked(w, ID_ONLY_FAVORITES) == BST_CHECKED;
    g_state.autostart = IsDlgButtonChecked(w, ID_AUTOSTART) == BST_CHECKED;
    g_state.theme = ComboSel(w, ID_THEME);
    g_state.accent = ComboSel(w, ID_ACCENT);
    g_state.pollSeconds = g_pollValues[ComboSel(w, ID_POLL)];
    int lang = ComboSel(w, ID_LANGUAGE);
    g_state.language = lang <= 0 || lang - 1 >= (int)g_langCodes.size() ? L"" : g_langCodes[lang - 1];
    int mon = ComboSel(w, ID_MONITOR);
    if (mon >= 0 && mon < (int)g_monitorIds.size()) g_state.monitor = g_monitorIds[mon];
}

void ApplySettings(HWND w) {
    ReadSettings(w);
    if (g_setActions.apply) g_setActions.apply(g_state);
    SetSettingTexts(w);  // the language may have changed
    InvalidateRect(Item(w, ID_ACCENT_COLOR), nullptr, TRUE);
}

void CreateSettingsControls(HWND w) {
    int dpi = Dpi(w);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    if (g_setFont) DeleteObject(g_setFont);
    g_setFont = MakeFont(MulDiv(9, dpi, 72), FW_NORMAL, L"Segoe UI Variable Text");
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(cw), S(ch), w, (HMENU)(INT_PTR)id, g_inst,
                                 nullptr);
        if (c && g_setFont) SendMessageW(c, WM_SETFONT, (WPARAM)g_setFont, TRUE);
        return c;
    };

    // Sections are drawn by the window itself (see PaintSettings): a native group box clips its
    // caption with a 9 pt font, and its caption colour cannot follow the theme.
    add(L"BUTTON", T(L"Show on the taskbar (icon + percentage)"), BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 24, 52, 430, 22,
        ID_MODE_WIDGET);
    add(L"BUTTON", T(L"Show as tray icons"), BS_AUTORADIOBUTTON | WS_TABSTOP, 24, 78, 430, 22, ID_MODE_TRAY);
    add(L"BUTTON", T(L"Background behind taskbar text (for transparent taskbars)"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 106, 430, 22,
        ID_BACKGROUND);
    add(L"BUTTON", T(L"Icons above percentages (narrower)"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 132, 430, 22, ID_STACKED);

    add(L"BUTTON", T(L"Hide last known / asleep devices"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 200, 430, 22, ID_HIDE_LAST);
    add(L"BUTTON", T(L"Show only favorites"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 226, 430, 22, ID_ONLY_FAVORITES);
    add(L"STATIC", T(L"Polling rate"), SS_LEFT, 24, 260, 170, 22, 2101);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 256, 170, 200, ID_POLL);

    add(L"STATIC", T(L"Theme"), SS_LEFT, 24, 334, 170, 22, 2102);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 330, 170, 200, ID_THEME);
    add(L"STATIC", T(L"Accent color"), SS_LEFT, 24, 366, 170, 22, 2103);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 362, 170, 200, ID_ACCENT);
    add(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 382, 362, 56, 24, ID_ACCENT_COLOR);

    add(L"BUTTON", T(L"Start with Windows"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 438, 430, 22, ID_AUTOSTART);
    add(L"STATIC", T(L"Language"), SS_LEFT, 24, 470, 170, 22, 2104);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 466, 240, 200, ID_LANGUAGE);
    add(L"STATIC", T(L"Show the widget on"), SS_LEFT, 24, 502, 170, 22, 2105);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 200, 498, 240, 200, ID_MONITOR);

    add(L"BUTTON", T(L"All devices..."), BS_PUSHBUTTON | WS_TABSTOP, 24, 552, 150, 32, ID_ALL_DEVICES);
    add(L"BUTTON", T(L"Refresh now"), BS_PUSHBUTTON | WS_TABSTOP, 182, 552, 130, 32, ID_REFRESH);
    add(L"BUTTON", T(L"Check for updates..."), BS_PUSHBUTTON | WS_TABSTOP, 320, 552, 152, 32, ID_CHECK_UPDATES);
    add(L"BUTTON", T(L"Close"), BS_PUSHBUTTON | WS_TABSTOP, 354, 598, 118, 30, ID_CLOSE);
    add(L"STATIC", L"", SS_LEFT, 24, 602, 320, 22, ID_VERSION);

    // Initial state
    CheckRadioButton(w, ID_MODE_WIDGET, ID_MODE_TRAY, g_state.widget ? ID_MODE_WIDGET : ID_MODE_TRAY);
    CheckDlgButton(w, ID_BACKGROUND, g_state.background ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_STACKED, g_state.stacked ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_HIDE_LAST, g_state.hideLastKnown ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_ONLY_FAVORITES, g_state.onlyFavorites ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, ID_AUTOSTART, g_state.autostart ? BST_CHECKED : BST_UNCHECKED);

    SetSettingTexts(w);
}

// Section headers: "View", "Devices", "Appearance", "System" with a hairline under each.
void PaintSettings(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    FillRect(dc, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    int dpi = Dpi(w);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    const struct {
        const wchar_t* title;
        int y;
    } sections[] = {
        { T(L"View"), 20 }, { T(L"Devices"), 168 }, { T(L"Appearance"), 302 }, { T(L"System"), 406 },
    };
    HFONT f = MakeFont(S(12), FW_SEMIBOLD, L"Segoe UI Variable Text");
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, SettingText());
    for (const auto& s : sections) {
        RECT tr{ S(24), S(s.y), rc.right - S(24), S(s.y + 20) };
        DrawTextW(dc, T(s.title), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    DeleteObject(f);
    HBRUSH line = CreateSolidBrush(g_dark ? RGB(60, 60, 65) : RGB(226, 226, 230));
    for (const auto& s : sections) {
        RECT r{ S(24), S(s.y + 24), rc.right - S(24), S(s.y + 25) };
        FillRect(dc, &r, line);
    }
    DeleteObject(line);
    EndPaint(w, &ps);
}

void PaintAccentButton(HWND w, const DRAWITEMSTRUCT* d) {
    if (d->CtlID != ID_ACCENT_COLOR) return;
    Fill(d->hDC, d->rcItem, g_state.accentColor);
    if (d->itemState & ODS_DISABLED) {
        HBRUSH veil = CreateSolidBrush(g_dark ? RGB(48, 48, 52) : RGB(214, 214, 218));
        RECT r = d->rcItem;
        InflateRect(&r, -1, -1);
        FillRect(d->hDC, &r, veil);
        DeleteObject(veil);
    }
    HBRUSH frame = CreateSolidBrush(g_dark ? RGB(120, 120, 126) : RGB(160, 160, 166));
    FrameRect(d->hDC, &d->rcItem, frame);
    DeleteObject(frame);
    if (d->itemState & ODS_SELECTED) {
        RECT r = d->rcItem;
        InflateRect(&r, -1, -1);
        HBRUSH b = CreateSolidBrush(Mix(g_state.accentColor, RGB(0, 0, 0), 20));
        FrameRect(d->hDC, &r, b);
        DeleteObject(b);
    }
    (void)w;
}

LRESULT CALLBACK SettingsProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            RebuildBgBrush();
            CreateSettingsControls(w);
            UseDarkTitleBar(w, g_dark);
            return 0;
        case WM_PAINT:
            PaintSettings(w);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(w, &rc);
            FillRect((HDC)wp, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, SettingText());
            SetBkColor(dc, SettingBg());
            return (LRESULT)g_bgBrush;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, SettingText());
            SetBkColor(dc, g_dark ? RGB(45, 45, 48) : RGB(255, 255, 255));
            static HBRUSH darkEdit = nullptr;
            if (!darkEdit) darkEdit = CreateSolidBrush(RGB(45, 45, 48));
            return (LRESULT)(g_dark ? darkEdit : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DRAWITEM:
            PaintAccentButton(w, (const DRAWITEMSTRUCT*)lp);
            return TRUE;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == ID_ALL_DEVICES) { if (g_setActions.showDevices) g_setActions.showDevices(); return 0; }
            if (id == ID_REFRESH) { if (g_setActions.refresh) g_setActions.refresh(); return 0; }
            if (id == ID_CHECK_UPDATES) { if (g_setActions.checkUpdates) g_setActions.checkUpdates(); return 0; }
            if (id == ID_CLOSE) { DestroyWindow(w); return 0; }
            if (id == ID_ACCENT_COLOR) {
                CHOOSECOLORW cc{ sizeof(cc) };
                COLORREF custom[16] = {};
                cc.hwndOwner = w;
                cc.rgbResult = g_state.accentColor;
                cc.lpCustColors = custom;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) {
                    g_state.accentColor = cc.rgbResult;
                    g_state.accent = 2;
                    SetCombo(w, ID_ACCENT, 2);
                    ApplySettings(w);
                }
                return 0;
            }
            if (code == CBN_SELCHANGE || code == BN_CLICKED) {
                ApplySettings(w);
                return 0;
            }
            return 0;
        }
        case WM_DESTROY:
            g_setWnd = nullptr;
            if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
            if (g_setFont) { DeleteObject(g_setFont); g_setFont = nullptr; }
            return 0;
        case WM_CLOSE: DestroyWindow(w); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void RegisterSettingsClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WirelessBatterySettings";
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    done = true;
}

void ApplySettingTheme(HWND w) {
    UseDarkTitleBar(w, g_dark);
    for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[16] = {};
        GetClassNameW(c, cls, 16);
        // Combo boxes use the "CFD" theme class, buttons and check boxes the "Explorer" one.
        // Group boxes stay classic: their caption is drawn in a colour we cannot override.
        if (_wcsicmp(cls, L"ComboBox") == 0 || _wcsicmp(cls, L"Edit") == 0) {
            ThemeControl(c, g_dark, L"DarkMode_CFD");
        } else if (_wcsicmp(cls, L"Button") == 0 && (GetWindowLongW(c, GWL_STYLE) & 0xF) != BS_GROUPBOX) {
            ThemeControl(c, g_dark);
        }
    }
    RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

}  // namespace

void ShowDevices(HWND owner, const std::vector<DeviceCard>& cards, const DevicesActions& actions) {
    if (!g_inst) g_inst = (HINSTANCE)GetModuleHandleW(nullptr);
    g_cards = cards;
    g_devActions = actions;
    if (g_devWnd && IsWindow(g_devWnd)) {
        ClampScroll(g_devWnd);
        InvalidateRect(g_devWnd, nullptr, FALSE);
        SetForegroundWindow(g_devWnd);
        return;
    }
    RegisterDevicesClass();
    int dpi = owner ? Dpi(owner) : 96;
    RECT rc{ 0, 0, MulDiv(560, dpi, 96), MulDiv(620, dpi, 96) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    g_scroll = 0;
    g_devWnd = CreateWindowExW(0, L"WirelessBatteryDevices", T(L"All devices"), style, x, y, w, h, owner, nullptr, g_inst, nullptr);
    if (g_devWnd) {
        ShowWindow(g_devWnd, SW_SHOW);
        SetForegroundWindow(g_devWnd);
    }
}

void UpdateDevices(const std::vector<DeviceCard>& cards) {
    if (!g_devWnd || !IsWindow(g_devWnd)) return;
    g_cards = cards;
    ClampScroll(g_devWnd);
    InvalidateRect(g_devWnd, nullptr, FALSE);
}

bool DevicesOpen() { return g_devWnd && IsWindow(g_devWnd); }

void ShowSettings(HWND owner, const SettingsState& state, const SettingsActions& actions) {
    if (!g_inst) g_inst = (HINSTANCE)GetModuleHandleW(nullptr);
    g_state = state;
    g_setActions = actions;
    // The language and monitor lists are captured once per open, so indices stay stable.
    g_langCodes.clear();
    for (const auto& l : i18n::Available()) g_langCodes.push_back(l.code);
    g_monitorIds = { L"all", L"main" };
    for (const auto& m : widget::OtherMonitors()) g_monitorIds.push_back(m.id);
    bool monitorKnown = false;
    for (const auto& id : g_monitorIds) monitorKnown |= id == g_state.monitor;
    if (!monitorKnown && !g_state.monitor.empty()) g_monitorIds.push_back(g_state.monitor);
    if (g_state.pollSeconds != 1 && g_state.pollSeconds != 5 && g_state.pollSeconds != 30 && g_state.pollSeconds != 60) {
        g_pollValues = { 1, 5, 30, 60, g_state.pollSeconds };
        std::sort(g_pollValues.begin(), g_pollValues.end());
    } else {
        g_pollValues = { 1, 5, 30, 60 };
    }

    if (g_setWnd && IsWindow(g_setWnd)) {
        SetForegroundWindow(g_setWnd);
        return;
    }
    RegisterSettingsClass();
    int dpi = owner ? Dpi(owner) : 96;
    RECT rc{ 0, 0, MulDiv(480, dpi, 96), MulDiv(640, dpi, 96) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    g_setWnd = CreateWindowExW(0, L"WirelessBatterySettings", T(L"Settings"), style, x, y, w, h, owner, nullptr, g_inst, nullptr);
    if (g_setWnd) {
        ApplySettingTheme(g_setWnd);
        ShowWindow(g_setWnd, SW_SHOW);
        SetForegroundWindow(g_setWnd);
    }
}

bool SettingsOpen() { return g_setWnd && IsWindow(g_setWnd); }

void SetDarkTheme(bool dark) {
    if (g_dark == dark) return;
    g_dark = dark;
    RebuildBgBrush();
    if (g_devWnd && IsWindow(g_devWnd)) {
        UseDarkTitleBar(g_devWnd, g_dark);
        InvalidateRect(g_devWnd, nullptr, FALSE);
    }
    if (g_setWnd && IsWindow(g_setWnd)) ApplySettingTheme(g_setWnd);
}

void CloseAll() {
    if (g_devWnd && IsWindow(g_devWnd)) DestroyWindow(g_devWnd);
    if (g_setWnd && IsWindow(g_setWnd)) DestroyWindow(g_setWnd);
}

}  // namespace ui