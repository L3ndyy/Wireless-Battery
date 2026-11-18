// Self-update: asks GitHub for the latest release, compares it with WB_VERSION, downloads
// "WirelessBattery.exe" from that release and swaps it in. The app is a portable single exe, so the
// update is just a file replacement; an elevated helper is used when the install folder needs admin
// rights (Program Files), while per-user installs update without any prompt.
#include "update.h"

#include <winhttp.h>

#include <shellapi.h>

#include <cctype>
#include <cstdio>
#include <vector>

#include "i18n.h"
#include "version.h"

namespace {

// The repository the app updates from. Change both lines together with the release tag.
const wchar_t* kApiHost = L"api.github.com";
const wchar_t* kLatestPath = L"/repos/L3ndyy/Wireless-Battery/releases/latest";
const wchar_t* kAssetName = L"WirelessBattery.exe";

std::wstring Utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// GET over HTTPS. `body` receives the response, or `saveTo` receives it as a file.
bool HttpGet(const std::wstring& host, const std::wstring& path, std::string& body, int& status, const std::wstring& saveTo) {
    status = 0;
    HINTERNET session = WinHttpOpen(L"WirelessBattery/" WB_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (request) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        WinHttpAddRequestHeaders(request, L"User-Agent: WirelessBattery\r\nAccept: application/vnd.github+json\r\n", -1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, len = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
            if (code >= 200 && code < 300) {
                if (!saveTo.empty()) {
                    file = CreateFileW(saveTo.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) code = 0;
                }
                bool good = !saveTo.empty() ? file != INVALID_HANDLE_VALUE : true;
                while (good) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail) || !avail) break;
                    std::string chunk(avail, 0);
                    DWORD got = 0;
                    if (!WinHttpReadData(request, chunk.data(), avail, &got)) { good = false; break; }
                    chunk.resize(got);
                    if (file != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        if (!WriteFile(file, chunk.data(), got, &written, nullptr) || written != got) { good = false; break; }
                    } else {
                        body += chunk;
                    }
                }
                ok = good;
            }
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok && !saveTo.empty()) DeleteFileW(saveTo.c_str());
    return ok;
}

// First string value of "key": "value" in the JSON, with the usual escapes undone.
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat, from);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    std::string raw;
    for (size_t i = p + 1; i < json.size(); i++) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char n = json[++i];
            raw += n == 'n' ? '\n' : n;
        } else if (json[i] == '"') {
            break;
        } else {
            raw += json[i];
        }
    }
    return raw;
}

std::vector<int> VersionParts(const std::wstring& version) {
    std::vector<int> parts;
    int cur = -1;
    for (wchar_t c : version) {
        if (iswdigit(c)) {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - L'0');
        } else if (cur >= 0) {
            parts.push_back(cur);
            cur = -1;
        }
    }
    if (cur >= 0) parts.push_back(cur);
    return parts;
}

// > 0 when `a` is newer than `b`.
int CompareVersions(const std::wstring& a, const std::wstring& b) {
    std::vector<int> x = VersionParts(a), y = VersionParts(b);
    for (size_t i = 0; i < x.size() || i < y.size(); i++) {
        int xv = i < x.size() ? x[i] : 0, yv = i < y.size() ? y[i] : 0;
        if (xv != yv) return xv > yv ? 1 : -1;
    }
    return 0;
}

bool SplitUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    const std::wstring prefix = L"https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    size_t slash = url.find(L'/', prefix.size());
    if (slash == std::wstring::npos) return false;
    host = url.substr(prefix.size(), slash - prefix.size());
    path = url.substr(slash);
    return !host.empty();
}

std::wstring FindAssetUrl(const std::string& json) {
    size_t at = 0;
    while ((at = json.find("\"browser_download_url\"", at)) != std::string::npos) {
        std::string name = JsonString(json, "browser_download_url", at);
        std::wstring url = Utf8(name);
        size_t tail = wcslen(kAssetName);
        if (url.size() >= tail + 1 && url.compare(url.size() - tail, tail, kAssetName) == 0 &&
            url[url.size() - tail - 1] == L'/')
            return url;
        at += 22;
    }
    return {};
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

void RestartAfterExit(const std::wstring& exe) {
    // The old process is still alive for a moment and holds the single-instance mutex; let it exit
    // first, then start the new exe as this (medium integrity) process, not elevated.
    std::wstring cmd = L"/c ping -n 4 127.0.0.1 >nul & start \"\" \"" + exe + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(L"cmd.exe", cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void Info(HWND owner, const std::wstring& text, UINT flags) {
    MessageBoxW(owner, text.c_str(), T(L"Update"), flags | MB_SETFOREGROUND | MB_TOPMOST);
}

// Replaces the running exe. Tries in-process first (per-user and portable installs), then asks for
// administrator rights through the downloaded exe itself for Program Files installs.
// 1 = installed, 0 = failed, -1 = the user declined the administrator prompt.
int InstallUpdate(HWND owner, const std::wstring& downloaded) {
    std::wstring target = SelfPath();
    if (target.empty()) return 0;
    std::wstring old = target + L".old";

    if (MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (CopyFileW(downloaded.c_str(), target.c_str(), FALSE)) return 1;
        MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);  // put the old one back
    }

    std::wstring args = L"--apply-update \"" + target + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = downloaded.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? -1 : 0;
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0 ? 1 : 0;
}

}  // namespace

namespace update {

void CheckAndPrompt(HWND owner, bool manual) {
    std::string json;
    int status = 0;
    bool ok = HttpGet(kApiHost, kLatestPath, json, status, L"");
    if (!ok || status != 200) {
        if (manual) Info(owner, T(L"Could not check for updates. Check your connection."), MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring latest = Utf8(JsonString(json, "tag_name"));
    if (latest.empty()) {
        if (manual) Info(owner, T(L"Could not check for updates. Check your connection."), MB_OK | MB_ICONWARNING);
        return;
    }
    if (CompareVersions(latest, WB_VERSION) <= 0) {
        if (manual) Info(owner, TF(L"You have the latest version ({version}).", { { L"version", WB_VERSION } }), MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring url = FindAssetUrl(json);
    if (url.empty()) {
        if (manual) Info(owner, T(L"Could not check for updates. Check your connection."), MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring text = TF(L"A new version is available: {version} (you have {current}). Update now?",
                           { { L"version", latest }, { L"current", WB_VERSION } });
    if (MessageBoxW(owner, text.c_str(), T(L"Update"), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES)
        return;

    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring file = std::wstring(tempDir) + L"WirelessBattery-update.exe";
    DeleteFileW(file.c_str());
    std::wstring host, path;
    if (!SplitUrl(url, host, path) || !HttpGet(host, path, json, status, file) || status < 200 || status > 299) {
        Info(owner, T(L"The update could not be downloaded. Try again later."), MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring target = SelfPath();
    int installed = InstallUpdate(owner, file);
    if (installed == -1) {
        Info(owner, T(L"Update canceled."), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (installed != 1) {
        Info(owner, T(L"The update could not be installed. Try again later."), MB_OK | MB_ICONWARNING);
        return;
    }
    Info(owner, T(L"The update is ready. Wireless Battery will restart."), MB_OK | MB_ICONINFORMATION);
    RestartAfterExit(target);
    PostMessageW(owner, WM_CLOSE, 0, 0);
}

int ApplyFromCommandLine(const wchar_t* cmdLine) {
    const wchar_t* p = wcsstr(cmdLine, L"--apply-update");
    if (!p) return 2;
    p += wcslen(L"--apply-update");
    while (*p == L' ') p++;
    std::wstring target;
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) target += *p;
    } else {
        for (; *p && *p != L' '; p++) target += *p;
    }
    if (target.empty()) return 2;
    std::wstring self = SelfPath();
    if (self.empty()) return 2;
    std::wstring old = target + L".old";
    // The old process may still be closing; retry for a few seconds.
    for (int i = 0; i < 50; i++) {
        if (MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (CopyFileW(self.c_str(), target.c_str(), FALSE)) return 0;
            MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        Sleep(200);
    }
    return 1;
}

void CleanupOldVersion() {
    std::wstring self = SelfPath();
    if (!self.empty()) DeleteFileW((self + L".old").c_str());
}

}  // namespace update
