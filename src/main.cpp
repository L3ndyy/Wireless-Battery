// Wireless Battery - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "devices.h"
#include "i18n.h"
#include "labels.h"
#include "report.h"
#include "ui.h"
#include "update.h"
#include "version.h"
#include "widget.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr int DEFAULT_POLL_SECONDS = 60;
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT, ID_REPORT,
    ID_BRAND_COLORS, ID_STACKED, ID_UPDATE, ID_ONLY_FAVORITES, ID_SETTINGS, ID_DEVICES,
    ID_LANG_AUTO = 400, ID_LANG_ADD, ID_LANG_FIRST,  // "Language": ID_LANG_FIRST + 0..63
    ID_MONITOR_ALL = 300, ID_MONITOR_MAIN, ID_MONITOR_FIRST,  // "Show the widget on": ID_MONITOR_FIRST + 0..31
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT { DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_FAVORITE, DEV_RENAME, DEV_STRIDE = 6 };

const wchar_t* kAppName = L"Wireless Battery";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
std::wstring g_widgetMonitor = L"all";  // which taskbars show the widget: "all", "main" or one monitor id
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_stacked = false;      // icon above the percentage (menu option)
int  g_accentMode = 0;       // 0 no accent, 1 brand colors, 2 one custom color
COLORREF g_accentColor = RGB(30, 190, 90);
int  g_themeMode = 0;        // 0 follow Windows, 1 light, 2 dark
std::atomic<int> g_pollSeconds{ DEFAULT_POLL_SECONDS };

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's own short names for devices ("Mouse", "Headphones"), stored as a REG_MULTI_SZ of
// "key=name". The key belongs to the device (from the driver), so a name survives replugging.
std::map<std::wstring, std::wstring> g_names;

void LoadNames() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceNames", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceNames", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        size_t eq = entry.find(L'=');
        if (eq != std::wstring::npos && eq > 0 && eq + 1 < entry.size()) g_names[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
}

void SaveNames() {
    std::wstring multi;
    for (const auto& [key, name] : g_names) { multi += key + L"=" + name; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceNames", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

// The name to show: the user's own one, or the name the driver found.
std::wstring DisplayName(const DeviceStatus& s) {
    auto it = g_names.find(s.key);
    return it != g_names.end() ? it->second : s.name;
}

// Favorites: keep the interesting devices on the taskbar and hide the rest with one switch.
std::set<std::wstring> g_favorites;
bool g_onlyFavorites = false;

void LoadFavorites() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"FavoriteDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"FavoriteDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_favorites.insert(p);
}

void SaveFavorites() {
    std::wstring multi;
    for (const auto& k : g_favorites) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"FavoriteDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsFavorite(const std::wstring& key) { return g_favorites.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_onlyFavorites && !IsFavorite(e.st.key)) ||
           (g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0)));
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }
std::wstring LoadWidgetMonitor() {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
        return buf;
    return LoadBoolSetting(L"OtherTaskbars", true) ? L"all" : L"main";  // the earlier on/off toggle
}
void SaveWidgetMonitor(const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetWidgetMonitor(const std::wstring& v) {
    g_widgetMonitor = v;
    SaveWidgetMonitor(v);
    widget::SetMonitorChoice(v);
}
bool LoadStacked() { return LoadBoolSetting(L"StackedLayout", false); }
void SaveStacked(bool on) { SaveBoolSetting(L"StackedLayout", on); }

int LoadIntSetting(const wchar_t* name, int defaultVal) {
    DWORD v = (DWORD)defaultVal, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return defaultVal;
    return (int)v;
}
void SaveIntSetting(const wchar_t* name, int value) {
    DWORD v = (DWORD)value;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}
void SaveBrandColors(bool on) { SaveBoolSetting(L"BrandColors", on); }  // older builds read this one

int LoadThemeMode() {
    int v = LoadIntSetting(L"ThemeMode", 0);
    return v < 0 || v > 2 ? 0 : v;
}
void SaveThemeMode(int v) { SaveIntSetting(L"ThemeMode", v); }

// Older builds only had the "Brand colors" checkbox; keep that choice when AccentMode is absent.
int LoadAccentMode() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AccentMode", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v > 2 ? 0 : (int)v;
    return LoadBoolSetting(L"BrandColors", false) ? 1 : 0;
}
void SaveAccentMode(int v) {
    SaveIntSetting(L"AccentMode", v);
    SaveBrandColors(v == 1);  // keep the old value readable by older builds
}

COLORREF LoadAccentColor() {
    int v = LoadIntSetting(L"AccentColor", (int)RGB(30, 190, 90));
    return (COLORREF)v;
}
void SaveAccentColor(COLORREF c) { SaveIntSetting(L"AccentColor", (int)c); }

int LoadPollSeconds() {
    int v = LoadIntSetting(L"PollSeconds", DEFAULT_POLL_SECONDS);
    return v < 1 ? 1 : (v > 3600 ? 3600 : v);
}
void SavePollSeconds(int v) { SaveIntSetting(L"PollSeconds", v); }

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

// What colors one device's icon: nothing, its brand, or the single color chosen in the settings.
struct AccentPick {
    Accent accent = Accent::None;
    bool custom = false;
    BrandColor color{};
};

AccentPick AccentFor(const DeviceStatus& s) {
    if (g_accentMode == 2)
        return { Accent::None, true, { GetRValue(g_accentColor), GetGValue(g_accentColor), GetBValue(g_accentColor) } };
    if (g_accentMode == 1) return { BrandOf(s), false, {} };
    return {};
}

HICON IconFor(int pct, bool charging, bool dim, const AccentPick& pick) {
    Rgb fg = Foreground();
    BrandColor bc = pick.custom ? pick.color : AccentColor(pick.accent, g_lightTaskbar);
    bool branded = pick.custom || pick.accent != Accent::None;
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
            : branded         ? Rgb{ bc.r, bc.g, bc.b }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return T(L"off / not connected");
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = T(s.levelText);
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = TF(L"{percent}% (last known)", { { L"percent", std::to_wstring(e.lastPct) } });
    else t = T(L"unknown");
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += std::wstring(L", ") + T(L"charging");
    if (s.asleep) t += std::wstring(L", ") + T(L"asleep");
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, std::wstring(L"Wireless Battery\n") + T(L"No devices with battery info found"));
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = s.connected && !g_useWidget && !IsEntryHidden(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    bool dim = s.percent < 0;
    AccentPick accent = AccentFor(s);
    std::wstring sig = std::to_wstring(shown) + (s.charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"a" + std::to_wstring((int)accent.accent) +
                       (accent.custom ? L"u" + std::to_wstring((int)GetRValue(g_accentColor) * 65536 + (int)GetGValue(g_accentColor) * 256 +
                                                               (int)GetBValue(g_accentColor))
                                      : L"");
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = IconFor(shown, s.charging, dim, accent);
        e.iconSig = sig;
    }
    std::wstring tip = DisplayName(s) + L"\n" + StatusText(e);
    if (s.via == L"USB cable" && !s.charging) tip += std::wstring(L", ") + T(L"USB cable");

    const wchar_t* infoTitle = nullptr;
    std::wstring info;
    if (s.charging || shown > LOW_BATTERY + 10) e.lowWarned = false;
    if (!s.charging && s.percent >= 0 && s.percent <= LOW_BATTERY && !e.lowWarned) {
        e.lowWarned = true;
        infoTitle = T(L"Low battery");
        info = TF(L"{name} is at {percent}%", { { L"name", DisplayName(s) }, { L"percent", std::to_wstring(s.percent) } });
    }
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip, infoTitle, info);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return e.st.connected && !IsEntryHidden(e); }

void RefreshDevicesWindow();  // defined with the settings block below

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        if (ka != kb) return ka < kb;
        return a->st.name != b->st.name ? a->st.name < b->st.name : a->st.key < b->st.key;  // same name: keep a fixed order
    });
    return list;
}

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    // Two devices of one kind share an icon: give each a tiny caption so they can be told apart.
    std::vector<LabelInput> labelIn;
    for (auto* e : list) labelIn.push_back({ e->st.kind, DisplayName(e->st) });
    std::vector<std::wstring> labels = ShortLabels(labelIn, 3);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        if (e->st.percent >= 0) it.text = T(e->st.levelText);
        it.charging = e->st.charging;
        it.wired = e->st.via == L"USB cable" && !e->st.charging;
        it.dim = e->st.percent < 0;
        it.low = !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        it.label = labels[items.size()];
        AccentPick accent = AccentFor(e->st);
        it.accent = accent.accent;
        it.custom = accent.custom;
        it.customColor = accent.color;
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += DisplayName(e->st) + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = std::wstring(L"Wireless Battery: ") + T(L"No devices with battery info found");
    widget::SetItems(items, tip);
}

void OnResults(std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        ApplyEntry(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while.
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        // A mouse that switches between its dongle and its USB cable comes back under a new key.
        // If the same device is live again under another key, drop the stale copy right away
        // instead of listing the mouse twice for the whole grace period.
        if (inGrace) {
            for (auto& s : results)
                if (s.key != it->first && s.kind == e.st.kind && !s.name.empty() && s.name == e.st.name) {
                    inGrace = false;
                    break;
                }
        }
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
    RefreshDevicesWindow();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

// Switches the UI language ("" = follow Windows) and redraws everything that shows text.
void SetLanguage(const std::wstring& code);
// Redraws the "All devices" window if it is open (defined with the settings block below).
void RefreshDevicesWindow();

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void SetLanguage(const std::wstring& code) {
    i18n::SetChoice(code);
    RedrawAll();  // tray tooltips and the placeholder text
    UpdateWidget();
}

void SetStacked(bool on) {
    g_stacked = on;
    SaveStacked(on);
    widget::SetStacked(on);
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

// An empty name (or the factory one) restores the name the driver found.
void SetDeviceName(const std::wstring& key, const std::wstring& factory, const std::wstring& name) {
    if (name.empty() || name == factory) g_names.erase(key);
    else g_names[key] = name;
    SaveNames();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void ToggleFavorite(const std::wstring& key) {
    if (!g_favorites.erase(key)) g_favorites.insert(key);
    SaveFavorites();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetOnlyFavorites(bool on) {
    g_onlyFavorites = on;
    SaveBoolSetting(L"OnlyFavorites", on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

// ---- renaming a device ------------------------------------------------------------------------

struct RenameState {
    std::wstring name;
    bool done = false, ok = false;
};

LRESULT CALLBACK RenameProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<RenameState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND:
            if (!st) break;
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[128] = {};
                GetDlgItemTextW(wnd, 101, buf, ARRAYSIZE(buf));
                st->name = buf;
                st->ok = true;
                DestroyWindow(wnd);
            } else if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(wnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(wnd);
            return 0;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Asks for a shorter name ("Mouse", "Headphones"); the factory name is shown above the box, and an
// empty box restores it. Blocks until the little window closes (the device poller keeps running).
std::wstring AskDeviceName(const std::wstring& current, const std::wstring& factory) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = RenameProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"WirelessBatteryRename";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = g_hwnd ? GetDpiForWindow(g_hwnd) : 0;
    if (!dpi) dpi = 96;
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    RECT rc{ 0, 0, S(420), S(150) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    HWND wnd = CreateWindowExW(exStyle, L"WirelessBatteryRename", T(L"Device name"), style, x, y, w, h, g_hwnd,
                               nullptr, g_inst, nullptr);
    if (!wnd) return current;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD childStyle, int cx, int cy, int cw, int ch, int id,
                     DWORD childEx = 0) {
        HWND c = CreateWindowExW(childEx, cls, text, WS_CHILD | WS_VISIBLE | childStyle, cx, cy, cw, ch, wnd,
                                 (HMENU)(INT_PTR)id, g_inst, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    child(L"STATIC", factory.c_str(), SS_ENDELLIPSIS, S(16), S(14), S(420) - S(32), S(20), -1);
    child(L"EDIT", current.c_str(), ES_AUTOHSCROLL, S(16), S(38), S(420) - S(32), S(26), 101, WS_EX_CLIENTEDGE);
    child(L"BUTTON", T(L"OK"), BS_DEFPUSHBUTTON | WS_TABSTOP, S(420) - S(16) - S(90), S(150) - S(16) - S(30), S(90), S(30), IDOK);
    child(L"BUTTON", T(L"Cancel"), BS_PUSHBUTTON | WS_TABSTOP, S(420) - S(16) - S(90) - S(8) - S(90), S(150) - S(16) - S(30),
          S(90), S(30), IDCANCEL);

    RenameState st;
    st.name = current;
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)&st);
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    HWND edit = GetDlgItem(wnd, 101);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    return st.ok ? st.name : current;
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void StartUpdateCheck(bool manual);

// ---- settings and windows ---------------------------------------------------------------------

// The chosen theme; automatic follows the taskbar's own light/dark setting.
void ApplyEffectiveTheme() {
    bool light = g_themeMode == 1 ? true : g_themeMode == 2 ? false : ReadLightTaskbar();
    if (light != g_lightTaskbar) {
        g_lightTaskbar = light;
        ApplyMenuTheme(light);
        RedrawAll();
        widget::SetLightTheme(light);
    }
    ui::SetDarkTheme(!light);
}

std::vector<ui::DeviceCard> BuildCards() {
    std::vector<ui::DeviceCard> out;
    for (TrayEntry* e : OrderedEntries(false)) {
        const DeviceStatus& s = e->st;
        ui::DeviceCard c;
        c.key = s.key;
        c.name = DisplayName(s);
        c.source = T(s.via);
        c.kind = s.kind;
        c.percent = s.percent >= 0 ? s.percent : e->lastPct;
        c.charging = s.charging;
        c.low = !s.charging && s.percent >= 0 && s.percent <= LOW_BATTERY;
        c.asleep = s.asleep || (s.percent < 0 && e->lastPct >= 0);
        c.connected = s.connected;
        c.hidden = IsEntryHidden(*e);
        c.favorite = IsFavorite(s.key);
        c.bluetooth = s.via == L"Bluetooth";
        AccentPick accent = AccentFor(s);
        c.accent = accent.accent;
        c.custom = accent.custom;
        c.customColor = accent.color;
        c.millivolts = s.millivolts;
        out.push_back(c);
    }
    return out;
}

void RefreshDevicesWindow() {
    if (ui::DevicesOpen()) ui::UpdateDevices(BuildCards());
}

ui::DevicesActions DeviceActions() {
    ui::DevicesActions a;
    a.reorder = [](const std::vector<std::wstring>& keys) {
        std::vector<std::wstring> order = keys;
        for (const auto& k : g_order)  // keep the places of devices that are not in the window right now
            if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
        g_order = order;
        SaveOrder();
        UpdateWidget();
    };
    a.toggleFavorite = [](const std::wstring& key) {
        ToggleFavorite(key);
        RefreshDevicesWindow();
    };
    a.toggleHidden = [](const std::wstring& key) {
        ToggleHidden(key);
        RefreshDevicesWindow();
    };
    a.rename = [](const std::wstring& key) {
        auto it = g_entries.find(key);
        if (it == g_entries.end()) return;
        std::wstring name = AskDeviceName(DisplayName(it->second.st), it->second.st.name);
        SetDeviceName(key, it->second.st.name, name);
        RefreshDevicesWindow();
    };
    return a;
}

ui::SettingsState CurrentSettings() {
    ui::SettingsState s;
    s.widget = g_wantWidget;
    s.background = g_widgetBackground;
    s.stacked = g_stacked;
    s.hideLastKnown = g_hideLastKnown;
    s.onlyFavorites = g_onlyFavorites;
    s.autostart = AutostartEnabled();
    s.theme = g_themeMode;
    s.accent = g_accentMode;
    s.accentColor = g_accentColor;
    s.pollSeconds = g_pollSeconds.load();
    s.language = i18n::Choice();
    s.monitor = g_widgetMonitor;
    s.version = WB_VERSION;
    return s;
}

void ApplySettings(const ui::SettingsState& s) {
    if (s.widget != g_wantWidget) SetMode(s.widget);
    if (s.background != g_widgetBackground) SetWidgetBackground(s.background);
    if (s.stacked != g_stacked) SetStacked(s.stacked);
    if (s.hideLastKnown != g_hideLastKnown) SetHideLastKnown(s.hideLastKnown);
    if (s.onlyFavorites != g_onlyFavorites) SetOnlyFavorites(s.onlyFavorites);
    if (s.autostart != AutostartEnabled()) SetAutostart(s.autostart);
    if (s.theme != g_themeMode) {
        g_themeMode = s.theme;
        SaveThemeMode(s.theme);
        ApplyEffectiveTheme();
    }
    if (s.accent != g_accentMode || s.accentColor != g_accentColor) {
        g_accentMode = s.accent;
        g_accentColor = s.accentColor;
        SaveAccentMode(s.accent);
        SaveAccentColor(s.accentColor);
        RedrawAll();
        UpdateWidget();
    }
    bool languageChanged = s.language.empty() ? !i18n::Choice().empty() : _wcsicmp(s.language.c_str(), i18n::Choice().c_str()) != 0;
    if (languageChanged) SetLanguage(s.language);
    if (s.monitor != g_widgetMonitor) SetWidgetMonitor(s.monitor);
    int poll = s.pollSeconds < 1 ? 1 : s.pollSeconds;
    if (poll != g_pollSeconds.load()) {
        g_pollSeconds = poll;
        SavePollSeconds(poll);
        SetEvent(g_refreshEvt);  // pick up the new interval now instead of after the old one
    }
    RefreshDevicesWindow();
}

void ShowAllDevices() { ui::ShowDevices(g_hwnd, BuildCards(), DeviceActions()); }

void ShowSettingsWindow() {
    ui::SettingsActions a;
    a.apply = ApplySettings;
    a.refresh = [] { SetEvent(g_refreshEvt); };
    a.checkUpdates = [] { StartUpdateCheck(true); };
    a.showDevices = ShowAllDevices;
    ui::ShowSettings(g_hwnd, CurrentSettings(), a);
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_DEVICES, T(L"All devices..."));
    AppendMenuW(m, MF_STRING, ID_SETTINGS, T(L"Settings..."));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"No devices with battery info found"));
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"Point at a device to show, hide or move it"));
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? T(L"Show on the taskbar") : T(L"Show in the tray"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? T(L"Move left") : T(L"Move up"));
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? T(L"Move right") : T(L"Move down"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (IsFavorite(k) ? MF_CHECKED : 0), base + DEV_FAVORITE, T(L"Favorite"));
        AppendMenuW(sub, MF_STRING, base + DEV_RENAME, T(L"Rename..."));
        std::wstring line = DisplayName(e->st) + L":  " + StatusText(*e) + L"   [" + T(e->st.via) + L"]";
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH, T(L"Refresh now"));
    AppendMenuW(m, MF_STRING, ID_REPORT, T(L"Request support for a device..."));
    AppendMenuW(m, MF_STRING, ID_SUPPORT, T(L"Support the project..."));
    AppendMenuW(m, MF_STRING, ID_UPDATE, T(L"Check for updates..."));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, T(L"Exit"));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (i18n::Rtl() ? TPM_LAYOUTRTL : 0), pt.x, pt.y, 0,
                              g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_DEVICES: ShowAllDevices(); break;
        case ID_SETTINGS: ShowSettingsWindow(); break;
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_UPDATE: StartUpdateCheck(true); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_FAVORITE: ToggleFavorite(key); break;
                    case DEV_RENAME: {
                        auto it = g_entries.find(key);
                        if (it != g_entries.end()) {
                            std::wstring name = AskDeviceName(DisplayName(it->second.st), it->second.st.name);
                            SetDeviceName(key, it->second.st.name, name);
                        }
                        break;
                    }
                }
                RefreshDevicesWindow();
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

// `--demo`: one sample device of every brand instead of real ones, for screenshots and for checking the
// brand colors without owning the hardware. Keys, vendor ids and names are mixed on purpose, so every way
// of recognising a brand gets used.
bool g_demo = false;

// ---- self-update ------------------------------------------------------------------------------

HANDLE g_updateThread = nullptr;

DWORD WINAPI UpdateCheckThread(LPVOID manual) {
    if (!manual) {
        Sleep(2500);  // let the tray icon and the first poll appear first
        if (g_demo) return 0;
    }
    update::CheckAndPrompt(g_hwnd, manual != nullptr);
    return 0;
}

void StartUpdateCheck(bool manual) {
    if (g_updateThread) {
        if (WaitForSingleObject(g_updateThread, 0) == WAIT_TIMEOUT) return;  // a check is already running
        CloseHandle(g_updateThread);
    }
    g_updateThread = CreateThread(nullptr, 0, UpdateCheckThread, manual ? (LPVOID)1 : nullptr, 0, nullptr);
}

std::vector<DeviceStatus> DemoDevices() {
    auto make = [](const wchar_t* key, const wchar_t* name, DeviceKind kind, int pct, unsigned short vid, bool charging) {
        DeviceStatus s;
        s.key = key;
        s.name = name;
        s.kind = kind;
        s.percent = pct;
        s.vid = vid;
        s.charging = charging;
        s.via = L"demo";
        return s;
    };
    return {
        make(L"razer:demo", L"Razer DeathAdder V3 Pro", DeviceKind::Mouse, 78, 0x1532, false),
        make(L"demo:logi", L"Logitech G PRO X 2 LIGHTSPEED", DeviceKind::Headset, 64, 0x046D, false),
        make(L"steelseries:demo", L"SteelSeries Arctis Nova 7", DeviceKind::Headset, 55, 0, false),
        make(L"hsc:1b1c:0a6b", L"Corsair HS80 RGB Wireless", DeviceKind::Headset, 81, 0x1B1C, false),
        make(L"hyperx:demo", L"HyperX Cloud III Wireless", DeviceKind::Headset, 47, 0, false),
        make(L"rog:demo", L"ROG Harpe Ace", DeviceKind::Mouse, 90, 0, false),
        make(L"zowie:demo", L"ZOWIE EC2-CW", DeviceKind::Mouse, 66, 0x04A5, false),
        make(L"bt:demo-ps", L"Wireless Controller", DeviceKind::Gamepad, 38, 0x054C, false),
        make(L"xbox:demo", L"Xbox Wireless Controller", DeviceKind::Gamepad, 72, 0x045E, false),
        make(L"bt:demo-buds", L"Galaxy Buds3 Pro", DeviceKind::Headset, 59, 0, false),
        make(L"airpods:demo", L"AirPods Pro", DeviceKind::Headset, 85, 0, false),
        make(L"jabra:demo", L"Jabra Evolve2 65", DeviceKind::Headset, 23, 0x0B0E, false),
        make(L"jbl:demo", L"JBL Quantum 910", DeviceKind::Headset, 70, 0x0ECB, false),
        make(L"coolermaster:demo", L"Cooler Master MM712", DeviceKind::Mouse, 12, 0x2516, false),
        make(L"steam:demo", L"Steam Controller", DeviceKind::Gamepad, 50, 0x28DE, true),
        make(L"bt:demo-plain", L"Bluetooth Headphones", DeviceKind::Headset, 100, 0, false),
    };
}

DWORD WINAPI Worker(LPVOID) {
    // At sign-in a USB receiver may enumerate before its wireless link is ready.
    constexpr DWORD startupPollDelays[] = { 3000, 7000, 15000 };
    size_t startupPoll = 0;
    HANDLE events[2] = { g_stopEvt, g_refreshEvt };
    for (;;) {
        auto* v = new std::vector<DeviceStatus>(g_demo ? DemoDevices() : PollAllDevices());
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        DWORD delay = (DWORD)std::max(1, g_pollSeconds.load()) * 1000;
        if (startupPoll < ARRAYSIZE(startupPollDelays)) delay = startupPollDelays[startupPoll++];
        if (WaitForMultipleObjects(2, events, FALSE, delay) == WAIT_OBJECT_0) return 0;
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshSoon(5000);
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_WIDGET) Reconcile();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") && g_themeMode == 0) ApplyEffectiveTheme();
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            ui::CloseAll();
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `WirelessBattery.exe --list` prints what it finds, `--dump` every HID collection.
int ListToConsole(bool dump) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"WirelessBattery-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        Accent accent = BrandOf(s);
        std::wstring line = s.name + L"  [" + s.via + L"]  " +
                            (accent != Accent::None ? L"{" + std::wstring(StyleOf(accent).name) + L"}  " : L"");
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.charging) line += L", charging";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--apply-update")) return update::ApplyFromCommandLine(cmdLine);
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        i18n::Init();
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && FindWindowW(L"WirelessBatteryWnd", nullptr); i++) Sleep(100);
        }
        return 0;
    }
    g_inst = inst;
    g_demo = cmdLine && wcsstr(cmdLine, L"--demo");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    update::CleanupOldVersion();  // left behind by the update that started this copy

    g_themeMode = LoadThemeMode();
    g_lightTaskbar = g_themeMode == 1 ? true : g_themeMode == 2 ? false : ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_widgetBackground = LoadWidgetBackground();
    g_accentMode = LoadAccentMode();
    g_accentColor = LoadAccentColor();
    g_pollSeconds = LoadPollSeconds();
    g_stacked = LoadStacked();
    widget::SetStacked(g_stacked);
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    i18n::Init();
    g_widgetMonitor = LoadWidgetMonitor();
    widget::SetMonitorChoice(g_widgetMonitor);
    LoadHidden();
    LoadNames();
    LoadFavorites();
    g_onlyFavorites = LoadBoolSetting(L"OnlyFavorites", false);
    LoadOrder();
    ui::SetDarkTheme(!g_lightTaskbar);
    Reconcile();
    // Opens a window right away (the list fills in with the first poll); used when driving the UI from a script.
    if (cmdLine && wcsstr(cmdLine, L"--devices")) ShowAllDevices();
    if (cmdLine && wcsstr(cmdLine, L"--settings")) ShowSettingsWindow();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    StartUpdateCheck(false);  // asks about a newer release a couple of seconds after startup

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (g_worker) CloseHandle(g_worker);
    if (g_updateThread) {
        WaitForSingleObject(g_updateThread, 3000);
        CloseHandle(g_updateThread);
    }
    ReleaseMutex(mutex);
    return 0;
}
