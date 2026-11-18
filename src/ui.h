#pragma once
// The two windows of the app: "All devices" (cards with a drag-and-drop order) and "Settings".
// Both are owned by the tray window, so they close with it, and both follow the light/dark theme.
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "brands.h"
#include "devices.h"

namespace ui {

struct DeviceCard {
    std::wstring key;
    std::wstring name;
    std::wstring source;   // already localized ("2.4 GHz receiver", "USB cable", "Bluetooth")
    DeviceKind kind = DeviceKind::Other;
    int percent = -1;
    bool charging = false;
    bool low = false;
    bool asleep = false;
    bool connected = true;
    bool hidden = false;   // not shown on the taskbar (hidden by hand, by favorites or because it sleeps)
    bool favorite = false;
    bool bluetooth = false;
    bool custom = false;   // accent from the settings instead of the brand
    BrandColor customColor{};
    Accent accent = Accent::None;
    int millivolts = 0;
};

struct DevicesActions {
    std::function<void(const std::vector<std::wstring>&)> reorder;  // new top-to-bottom order of all keys
    std::function<void(const std::wstring&)> toggleFavorite;
    std::function<void(const std::wstring&)> toggleHidden;
    std::function<void(const std::wstring&)> rename;
};

void ShowDevices(HWND owner, const std::vector<DeviceCard>& cards, const DevicesActions& actions);
void UpdateDevices(const std::vector<DeviceCard>& cards);
bool DevicesOpen();

struct SettingsState {
    bool widget = true;         // taskbar widget (true) or tray icons (false)
    bool background = false;
    bool stacked = false;
    bool hideLastKnown = false;
    bool onlyFavorites = false;
    bool autostart = false;
    int theme = 0;              // 0 automatic, 1 light, 2 dark
    int accent = 0;             // 0 no accent, 1 brand colors, 2 custom color
    COLORREF accentColor = RGB(30, 190, 90);
    int pollSeconds = 60;
    std::wstring language;      // "" = follow Windows
    std::wstring monitor;       // "all", "main" or a monitor id
    std::wstring version;       // shown in the title area, e.g. "1.2.0"
};

struct SettingsActions {
    std::function<void(const SettingsState&)> apply;
    std::function<void()> refresh;
    std::function<void()> checkUpdates;
    std::function<void()> showDevices;
};

void ShowSettings(HWND owner, const SettingsState& state, const SettingsActions& actions);
bool SettingsOpen();
void SetDarkTheme(bool dark);  // repaint both windows when the effective theme changes
void CloseAll();

}  // namespace ui