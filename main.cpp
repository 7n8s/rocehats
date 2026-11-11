// RoCheats Monitor
// ---------------------------------------------------------------------------
// A small Windows tray app. It polls the public WEAO API and raises a native
// desktop notification whenever the status of a tracked product changes.
//
//   * no installer, no runtime dependencies (static MinGW-w64 build)
//   * settings + last known statuses live in %APPDATA%\RoCheatsMonitor\settings.ini
//   * the only network request it makes is GET <WEAO>/api/status/exploits
//
// Build: ./build.sh

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include "json.h"

// A few constants that older SDK headers may lack.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef NIIF_LARGE_ICON
#define NIIF_LARGE_ICON 0x00000020
#endif
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t* const APP_NAME   = L"RoCheats Monitor";
static const wchar_t* const WND_CLASS  = L"RoCheatsMonitor.MainWindow";
static const wchar_t* const MUTEX_NAME = L"Local\\RoCheatsMonitor.SingleInstance";
static const wchar_t* const RUN_KEY    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* const RUN_VALUE  = L"RoCheatsMonitor";

// WEAO asks third-party tools to identify themselves with this user agent.
static const wchar_t* const USER_AGENT = L"WEAO-3PService";
static const wchar_t* const API_URLS[] = {
    L"https://weao.xyz/api/status/exploits",
    L"https://whatexpsare.online/api/status/exploits",
    L"https://weao.gg/api/status/exploits",
};

static const wchar_t* const DEFAULT_PRODUCTS =
    L"Volt,Wave,Potassium,Isaeva,Matcha,Serotonin,Seliware,Codex,Lucid,Axis";
static const size_t MAX_PRODUCTS = 16;
static const size_t MAX_BODY = 8u * 1024u * 1024u;
static const int INTERVALS[] = { 1, 2, 5, 10, 15, 30 };  // minutes
static const int RETRY_SECONDS = 60;

static const DWORD WND_STYLE = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
static const DWORD WND_EXSTYLE = 0;

enum : UINT { WM_APP_TRAY = WM_APP + 1, WM_APP_FETCHED, WM_APP_SHOW };
enum : UINT_PTR { TIMER_POLL = 1, TIMER_TRAY = 2 };
enum { TRAY_ID = 1 };
enum { ID_OPEN = 1001, ID_CHECK, ID_TEST, ID_EXIT, ID_INTERVAL_BASE = 2000 };
enum { HIT_NONE = 0, HIT_REFRESH = 1, HIT_CELL0 = 10, HIT_BELL0 = 100 };
enum { CELL_INTERVAL = 0, CELL_NOTIFY, CELL_SOUND, CELL_AUTOSTART, CELL_COUNT };

// Palette: dark charcoal, one blue accent.
static const COLORREF C_BG         = RGB(0x0F, 0x10, 0x18);
static const COLORREF C_CARD       = RGB(0x19, 0x1A, 0x27);
static const COLORREF C_CARD_HOT   = RGB(0x27, 0x28, 0x3A);
static const COLORREF C_LINE       = RGB(0x343448);
static const COLORREF C_TEXT       = RGB(0xEC, 0xED, 0xEF);
static const COLORREF C_TEXT2      = RGB(0xA5, 0xA6, 0xB8);
static const COLORREF C_TEXT3      = RGB(0x6E, 0x70, 0x86);
static const COLORREF C_ACCENT     = RGB(0x8B, 0x78, 0xFF);
static const COLORREF C_GREEN      = RGB(0x55, 0xD6, 0xA5);
static const COLORREF C_RED        = RGB(0xFF, 0x6B, 0x83);
static const COLORREF C_AMBER      = RGB(0xF0, 0xB6, 0x65);
static const COLORREF C_TOGGLE_OFF = RGB(0x3A, 0x3D, 0x46);
static const COLORREF C_KNOB_OFF   = RGB(0xB4, 0xB8, 0xC1);
static const COLORREF C_WHITE      = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF C_CAPTION_TX = RGB(0xC4, 0xC7, 0xCE);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Product {
    std::wstring name;      // shown in the UI (upper case)
    std::wstring key;       // lower case, matched against the API "title"
    bool notify = true;     // per-product notification switch
    int updated = -1;       // last known: -1 unknown, 0 not updated, 1 updated
    int detected = -1;      // last known: -1 unknown, 0 undetected, 1 detected
    std::wstring version;
    bool tracked = true;    // false once a successful poll did not list it
};

struct Settings {
    int intervalMin = 5;
    bool notifications = true;
    bool sound = true;
    bool trayHintShown = false;
    std::wstring apiUrl;    // optional override, tried before the built-in URLs
};

struct Layout {
    int W = 0, H = 0, pad = 0;
    int listX = 0, listY = 0, listW = 0, rowH = 0;
    RECT refresh{};
    RECT cells[CELL_COUNT]{};
};

struct FetchItem {
    std::wstring title, version, platform;
    bool updated = false;
    bool hasDetected = false;
    bool detected = false;
};

struct FetchResult {
    bool ok = false;
    bool rateLimited = false;
    int retryAfterSec = 0;
    std::vector<FetchItem> items;
};

struct FetchParams {
    HWND hwnd = nullptr;
    std::vector<std::wstring> urls;
};

struct Change {
    int index;   // product index
    int kind;    // 0 = update status, 1 = detection
    bool now;    // new value
};

static HINSTANCE g_inst = nullptr;
static HWND g_hwnd = nullptr;
static HICON g_iconSmall = nullptr, g_iconLarge = nullptr;
static UINT g_msgTaskbarCreated = 0;
static double g_scale = 1.0;
static Layout g_L;
static Settings g_set;
static std::vector<Product> g_products;
static std::wstring g_iniPath;

static HFONT g_fTitle = nullptr, g_fSub = nullptr, g_fName = nullptr, g_fSmall = nullptr;
static HFONT g_fStatus = nullptr, g_fCaps = nullptr, g_fCell = nullptr, g_fCellVal = nullptr;

static int g_hot = HIT_NONE, g_pressed = HIT_NONE;
static bool g_mouseTracking = false;

static bool g_fetching = false;       // a request is in flight
static bool g_everSucceeded = false;  // at least one good response this session
static bool g_lastFailed = false;
static std::wstring g_failText;
static int g_retrySec = RETRY_SECONDS;
static std::wstring g_lastChecked;    // formatted local time of the last good response
static DWORD g_lastManualTick = 0;
static bool g_hasManualTick = false;

static bool g_autostart = false;
static bool g_trayAdded = false, g_trayV4 = false;
static int g_trayRetries = 0;
static int g_savedX = INT_MIN, g_savedY = INT_MIN;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static int S(double v) { return (int)std::floor(v * g_scale + 0.5); }
static int Max(int a, int b) { return a > b ? a : b; }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
    return s.substr(a, b - a);
}

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

static bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }

// Text from the network: drop control characters, trim, cap the length.
static std::wstring Clean(const std::wstring& in, size_t maxLen) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in)
        if (c >= 0x20 && c != 0x7F) out += c;
    out = Trim(out);
    if (out.size() > maxLen) {
        out.resize(maxLen);
        if (!out.empty() && IsHighSurrogate(out.back())) out.pop_back();
    }
    return out;
}

// Product names double as INI section names, so keep them tame.
static std::wstring CleanName(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (c < 0x20 || c == 0x7F || c == L'[' || c == L']' || c == L'=' || c == L';') continue;
        out += c;
    }
    out = Trim(out);
    if (out.size() > 24) out.resize(24);
    return Trim(out);
}

// Fits `s` into `maxChars` characters, ending with an ellipsis when cut.
static std::wstring Clip(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    size_t n = maxChars - 1;
    if (n > 0 && IsHighSurrogate(s[n - 1])) --n;
    return s.substr(0, n) + L"\u2026";
}

static std::wstring NowTime() {
    wchar_t buf[64];
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, buf, 64))
        return std::wstring();
    return buf;
}

static std::wstring IntervalShort(int minutes) { return std::to_wstring(minutes) + L" min"; }

static std::wstring IntervalLong(int minutes) {
    return std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
}

static std::wstring RetryText(int seconds) {
    if (seconds < 60) return std::to_wstring(seconds) + L" s";
    return std::to_wstring((seconds + 30) / 60) + L" min";
}

// ---------------------------------------------------------------------------
// Settings (INI file in %APPDATA%)
// ---------------------------------------------------------------------------
static void InitIniPath() {
    wchar_t base[MAX_PATH] = L"";
    std::wstring dir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, base))) {
        dir = std::wstring(base) + L"\\RoCheatsMonitor";
    } else {  // unusual: fall back to the folder the exe lives in
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = exe;
        size_t slash = dir.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    g_iniPath = dir + L"\\settings.ini";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Start the file with a UTF-16 BOM so the profile API stores Unicode.
        HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const BYTE bom[2] = { 0xFF, 0xFE };
            DWORD written = 0;
            WriteFile(h, bom, 2, &written, nullptr);
            CloseHandle(h);
        }
    }
}

static std::wstring IniGet(const std::wstring& sec, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec.c_str(), key, def, buf, 2048, g_iniPath.c_str());
    return buf;
}

static int IniGetInt(const std::wstring& sec, const wchar_t* key, int def) {
    std::wstring v = Trim(IniGet(sec, key));
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    return (end && end != v.c_str()) ? (int)n : def;
}

static void IniSet(const std::wstring& sec, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(sec.c_str(), key, value.c_str(), g_iniPath.c_str());
}

static void IniSetInt(const std::wstring& sec, const wchar_t* key, int value) {
    IniSet(sec, key, std::to_wstring(value));
}

static std::wstring ProductSection(const Product& p) { return L"Product." + p.key; }

static void SaveProduct(const Product& p) {
    const std::wstring sec = ProductSection(p);
    IniSetInt(sec, L"Notify", p.notify ? 1 : 0);
    IniSetInt(sec, L"Updated", p.updated);
    IniSetInt(sec, L"Detected", p.detected);
    IniSet(sec, L"Version", p.version);
}

static void LoadSettings() {
    const std::wstring sec = L"Settings";
    g_set.intervalMin = Clamp(IniGetInt(sec, L"IntervalMinutes", 5), 1, 1440);
    g_set.notifications = IniGetInt(sec, L"Notifications", 1) != 0;
    g_set.sound = IniGetInt(sec, L"Sound", 1) != 0;
    g_set.trayHintShown = IniGetInt(sec, L"TrayHintShown", 0) != 0;
    g_set.apiUrl = Trim(IniGet(sec, L"ApiUrl"));
    g_savedX = IniGetInt(L"Window", L"X", INT_MIN);
    g_savedY = IniGetInt(L"Window", L"Y", INT_MIN);

    std::wstring list = Trim(IniGet(sec, L"Products"));
    for (int pass = 0; pass < 2 && g_products.empty(); ++pass) {
        if (pass == 1 || list.empty()) {
            list = DEFAULT_PRODUCTS;
            IniSet(sec, L"Products", list);
        }
        size_t start = 0;
        while (g_products.size() < MAX_PRODUCTS) {
            size_t comma = list.find(L',', start);
            std::wstring name = CleanName(
                list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (!name.empty()) {
                std::wstring key = Lower(name);
                bool dup = false;
                for (const Product& q : g_products) dup = dup || q.key == key;
                if (!dup) {
                    Product p;
                    p.name = Upper(name);
                    p.key = key;
                    g_products.push_back(p);
                }
            }
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
    }

    for (Product& p : g_products) {
        const std::wstring ps = ProductSection(p);
        p.notify = IniGetInt(ps, L"Notify", 1) != 0;
        p.updated = Clamp(IniGetInt(ps, L"Updated", -1), -1, 1);
        p.detected = Clamp(IniGetInt(ps, L"Detected", -1), -1, 1);
        p.version = Clean(IniGet(ps, L"Version"), 40);
    }
}

// ---------------------------------------------------------------------------
// Start with Windows (HKCU Run key)
// ---------------------------------------------------------------------------
static std::wstring AutostartCommand() {
    wchar_t exe[1024] = L"";
    GetModuleFileNameW(nullptr, exe, 1024);
    return L"\"" + std::wstring(exe) + L"\" --tray";
}

static bool AutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(key, RUN_VALUE, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static bool SetAutostart(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (on) {
        const std::wstring cmd = AutostartCommand();
        r = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, RUN_VALUE);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Networking (runs on a worker thread)
// ---------------------------------------------------------------------------
static bool HttpGet(const std::wstring& url, DWORD& status, std::string& body) {
    status = 0;
    body.clear();

    wchar_t host[256] = L"", path[2048] = L"";
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return false;
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 10000, 15000);

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) {
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path[0] ? path : L"/", nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
        if (req) {
            if (WinHttpSendRequest(req, L"Accept: application/json\r\n", (DWORD)-1L,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD size = sizeof status;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                    WINHTTP_NO_HEADER_INDEX);
                ok = true;
                char chunk[16384];
                for (;;) {  // a read of 0 bytes marks the end of the response
                    DWORD got = 0;
                    if (!WinHttpReadData(req, chunk, sizeof chunk, &got)) { ok = false; break; }
                    if (got == 0) break;
                    if (body.size() + got > MAX_BODY) { ok = false; break; }
                    body.append(chunk, got);
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

static std::wstring JsonText(const json::Value& obj, const char* key, size_t maxLen) {
    const json::Value* v = obj.find(key);
    return (v && v->isString()) ? Clean(Utf8ToWide(v->str), maxLen) : std::wstring();
}

static DWORD WINAPI FetchThread(LPVOID param) {
    FetchParams* p = (FetchParams*)param;
    FetchResult* res = new FetchResult();

    for (const std::wstring& url : p->urls) {
        DWORD status = 0;
        std::string body;
        if (!HttpGet(url, status, body)) continue;  // network trouble: try the next mirror

        if (status == 429) {  // rate limited: back off, do not hammer the mirrors
            res->rateLimited = true;
            res->retryAfterSec = 120;
            json::Value v;
            if (json::Parse(body, v)) {
                const json::Value* info = v.find("rateLimitInfo");
                const json::Value* t = info ? info->find("remainingTime") : nullptr;
                if (t && t->isNumber() && t->num > 0 && t->num < 86400)
                    res->retryAfterSec = Clamp((int)t->num, 15, 3600);
            }
            break;
        }
        if (status != 200) continue;

        json::Value v;
        if (!json::Parse(body, v) || !v.isArray()) continue;
        std::vector<FetchItem> items;
        for (const json::Value& e : v.items) {
            if (!e.isObject()) continue;
            const json::Value* us = e.find("updateStatus");
            if (!us || !us->isBool()) continue;
            FetchItem it;
            it.title = JsonText(e, "title", 64);
            if (it.title.empty()) continue;
            it.updated = us->b;
            const json::Value* det = e.find("detected");
            it.hasDetected = det && det->isBool();
            it.detected = it.hasDetected && det->b;
            it.version = JsonText(e, "version", 40);
            it.platform = JsonText(e, "platform", 24);
            items.push_back(it);
        }
        if (items.empty()) continue;
        res->items.swap(items);
        res->ok = true;
        break;
    }

    const HWND hwnd = p->hwnd;
    delete p;
    if (!PostMessageW(hwnd, WM_APP_FETCHED, 0, (LPARAM)res)) delete res;
    return 0;
}

static void StartFetch() {
    if (g_fetching || !g_hwnd) return;
    FetchParams* p = new FetchParams();
    p->hwnd = g_hwnd;
    if (!g_set.apiUrl.empty()) p->urls.push_back(g_set.apiUrl);
    for (const wchar_t* u : API_URLS) p->urls.push_back(u);

    KillTimer(g_hwnd, TIMER_POLL);
    HANDLE thread = CreateThread(nullptr, 0, FetchThread, p, 0, nullptr);
    if (!thread) {
        delete p;
        SetTimer(g_hwnd, TIMER_POLL, RETRY_SECONDS * 1000, nullptr);
        return;
    }
    CloseHandle(thread);
    g_fetching = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ManualCheck() {
    if (g_fetching) return;
    const DWORD now = GetTickCount();
    if (g_hasManualTick && now - g_lastManualTick < 3000) return;  // be polite to the API
    g_hasManualTick = true;
    g_lastManualTick = now;
    StartFetch();
}

// ---------------------------------------------------------------------------
// What the UI shows
// ---------------------------------------------------------------------------
static bool HasStatus(const Product& p) { return p.updated >= 0 && p.tracked; }

static std::wstring Summary() {
    int tracked = 0, up = 0;
    for (const Product& p : g_products) {
        if (!HasStatus(p)) continue;
        ++tracked;
        if (p.updated == 1) ++up;
    }
    if (tracked == 0) {
        if (g_everSucceeded) return L"No products tracked";
        return g_lastFailed ? L"No data yet" : L"Checking\u2026";
    }
    return std::to_wstring(up) + L" of " + std::to_wstring(tracked) + L" updated";
}

static std::wstring Subline(COLORREF& color) {
    color = C_TEXT2;
    if (g_fetching) return L"Checking\u2026";
    if (g_lastFailed) {
        color = C_AMBER;
        return g_failText + L" \u00B7 retrying in " + RetryText(g_retrySec);
    }
    if (!g_lastChecked.empty()) return Summary() + L"  \u00B7  Checked " + g_lastChecked + L"  \u00B7  WEAO";
    return Summary() + L"  \u00B7  Live status from WEAO";
}

struct RowView {
    COLORREF dot = C_TEXT3, statusColor = C_TEXT3;
    std::wstring status, detect, version;
};

static RowView ViewOf(const Product& p) {
    RowView v;
    if (HasStatus(p)) {
        const bool up = p.updated == 1;
        v.dot = v.statusColor = up ? C_GREEN : C_RED;
        v.status = up ? L"Updated" : L"Not updated";
        v.detect = p.detected == 1 ? L"Detected" : (p.detected == 0 ? L"Undetected" : L"\u2014");
        v.version = p.version;
    } else {
        v.status = g_everSucceeded ? L"Not on WEAO" : L"\u2014";
        v.detect = L"\u2014";
    }
    return v;
}

// ---------------------------------------------------------------------------
// Layout (everything is derived from one scale factor)
// ---------------------------------------------------------------------------
static void ComputeLayout() {
    Layout& L = g_L;
    const int n = (int)g_products.size();
    L.W = S(520);
    L.pad = S(24);
    L.listX = L.pad;
    L.listW = L.W - 2 * L.pad;
    L.listY = S(108);
    L.rowH = S(40);

    const int cellH = S(42), gap = S(10);
    const int cellsY = L.listY + n * L.rowH + S(18);
    const int cellW = (L.listW - gap) / 2;
    for (int i = 0; i < CELL_COUNT; ++i) {
        const int col = i % 2, row = i / 2;
        const int x = L.listX + col * (cellW + gap);
        const int y = cellsY + row * (cellH + gap);
        SetRect(&L.cells[i], x, y, col == 1 ? L.listX + L.listW : x + cellW, y + cellH);
    }
    L.H = cellsY + 2 * cellH + gap + S(24);

    const int bs = S(30);
    SetRect(&L.refresh, L.W - L.pad - bs, S(27), L.W - L.pad, S(27) + bs);
}

static RECT RowRect(int i) {
    RECT r;
    SetRect(&r, g_L.listX, g_L.listY + i * g_L.rowH, g_L.listX + g_L.listW,
            g_L.listY + (i + 1) * g_L.rowH);
    return r;
}

static RECT BellRect(int i) {
    const RECT row = RowRect(i);
    const int s = S(28);
    RECT r;
    r.right = row.right - S(6);
    r.left = r.right - s;
    r.top = row.top + (g_L.rowH - s) / 2;
    r.bottom = r.top + s;
    return r;
}

static int HitTest(POINT pt) {
    if (PtInRect(&g_L.refresh, pt)) return HIT_REFRESH;
    for (int i = 0; i < CELL_COUNT; ++i)
        if (PtInRect(&g_L.cells[i], pt)) return HIT_CELL0 + i;
    const int n = (int)g_products.size();
    for (int i = 0; i < n; ++i) {
        const RECT b = BellRect(i);
        if (PtInRect(&b, pt)) return HIT_BELL0 + i;
    }
    return HIT_NONE;
}

static int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *(bool*)found = true;
    return 0;  // stop enumerating
}

static bool FontInstalled(const wchar_t* face) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontFoundProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// GDI exposes the semibold weight as its own family ("Segoe UI Semibold");
// fall back to regular Segoe UI in bold where that face is missing.
static HFONT MakeFont(double px, bool strong) {
    static const bool hasSemibold = FontInstalled(L"Segoe UI Semibold");
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof lf);
    lf.lfHeight = -Max(1, S(px));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (strong && hasSemibold) {
        lf.lfWeight = FW_SEMIBOLD;
        lstrcpynW(lf.lfFaceName, L"Segoe UI Semibold", LF_FACESIZE);
    } else {
        lf.lfWeight = strong ? FW_BOLD : FW_NORMAL;
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    }
    return CreateFontIndirectW(&lf);
}

static void RebuildFonts() {
    HFONT* all[] = { &g_fTitle, &g_fSub, &g_fName, &g_fSmall, &g_fStatus, &g_fCaps, &g_fCell, &g_fCellVal };
    for (HFONT* f : all) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    g_fTitle   = MakeFont(19, true);
    g_fSub     = MakeFont(12.5, false);
    g_fName    = MakeFont(13.5, true);
    g_fSmall   = MakeFont(12, false);
    g_fStatus  = MakeFont(12.5, true);
    g_fCaps    = MakeFont(10, true);
    g_fCell    = MakeFont(12, false);
    g_fCellVal = MakeFont(12, true);
}

static UINT DpiForWindow(HWND hwnd) {
    typedef UINT(WINAPI * Fn)(HWND);
    static const Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        const UINT d = fn(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(hwnd);
    const int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return d > 0 ? (UINT)d : 96;
}

static void AdjustRectForDpi(RECT* rc, UINT dpi) {
    typedef BOOL(WINAPI * Fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const Fn fn =
        (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (!fn || !fn(rc, WND_STYLE, FALSE, WND_EXSTYLE, dpi)) AdjustWindowRectEx(rc, WND_STYLE, FALSE, WND_EXSTYLE);
}

// Recomputes scale, layout and fonts for `dpi`, then sizes the window to fit.
// If the window would be taller than the screen's work area the UI is scaled down.
static void ApplyDpi(HWND hwnd, UINT dpi, const POINT* moveTo) {
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;

    const double base = dpi / 96.0;
    g_scale = base;
    int winW = 0, winH = 0;
    for (int pass = 0; pass < 2; ++pass) {
        ComputeLayout();
        RECT rc = { 0, 0, g_L.W, g_L.H };
        AdjustRectForDpi(&rc, dpi);
        winW = rc.right - rc.left;
        winH = rc.bottom - rc.top;
        if (pass == 1 || workW < 200 || workH < 200 || (winW <= workW && winH <= workH)) break;
        const double fitW = (double)(workW - (winW - g_L.W)) / g_L.W;
        const double fitH = (double)(workH - (winH - g_L.H)) / g_L.H;
        double fit = fitW < fitH ? fitW : fitH;
        if (fit >= 1.0) break;
        if (fit < 0.5) fit = 0.5;
        g_scale = base * fit * 0.99;
    }
    RebuildFonts();
    SetWindowPos(hwnd, nullptr, moveTo ? moveTo->x : 0, moveTo ? moveTo->y : 0, winW, winH,
                 SWP_NOZORDER | SWP_NOACTIVATE | (moveTo ? 0 : SWP_NOMOVE));
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Centers the window (center == true) or nudges it back inside the work area.
static void PlaceWindow(HWND hwnd, bool center) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = wr.left, y = wr.top;
    if (center) {
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    }
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void SaveWindowPos() {
    if (!g_hwnd || !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) return;
    RECT r;
    GetWindowRect(g_hwnd, &r);
    if (r.left == g_savedX && r.top == g_savedY) return;
    g_savedX = r.left;
    g_savedY = r.top;
    IniSetInt(L"Window", L"X", g_savedX);
    IniSetInt(L"Window", L"Y", g_savedY);
}

// ---------------------------------------------------------------------------
// Drawing
//   Shapes: a tiny anti-aliased software rasterizer that writes straight into
//           a 32-bit DIB (distance-based coverage, identical on every system).
//   Text:   GDI with ClearType, drawn on top of the same DIB.
// ---------------------------------------------------------------------------
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    DWORD* px = nullptr;  // top-down rows, 0x00RRGGBB
    int w = 0, h = 0;
};
static Canvas g_cv;

static void FreeCanvas() {
    if (g_cv.dc) {
        if (g_cv.oldBmp) SelectObject(g_cv.dc, g_cv.oldBmp);
        DeleteDC(g_cv.dc);
    }
    if (g_cv.bmp) DeleteObject(g_cv.bmp);
    g_cv = Canvas();
}

static bool EnsureCanvas(HDC ref, int w, int h) {
    if (g_cv.dc && g_cv.w == w && g_cv.h == h) return true;
    FreeCanvas();
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(ref);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return false;
    }
    g_cv.dc = dc;
    g_cv.bmp = bmp;
    g_cv.oldBmp = SelectObject(dc, bmp);
    g_cv.px = (DWORD*)bits;
    g_cv.w = w;
    g_cv.h = h;
    return true;
}

struct Ink {
    int r, g, b;
    float a;
};

static Ink MakeInk(COLORREF c, float alpha = 1.0f) {
    const Ink k = { GetRValue(c), GetGValue(c), GetBValue(c), alpha };
    return k;
}

struct Pt {
    float x, y;
};

static inline void Blend(DWORD& dst, const Ink& k, float coverage) {
    const float a = (coverage > 1.0f ? 1.0f : coverage) * k.a;
    if (a <= 0.0f) return;
    if (a >= 0.999f) {
        dst = ((DWORD)k.r << 16) | ((DWORD)k.g << 8) | (DWORD)k.b;
        return;
    }
    const int dr = (int)((dst >> 16) & 0xFF), dg = (int)((dst >> 8) & 0xFF), db = (int)(dst & 0xFF);
    const int r = dr + (int)lroundf((float)(k.r - dr) * a);
    const int g = dg + (int)lroundf((float)(k.g - dg) * a);
    const int b = db + (int)lroundf((float)(k.b - db) * a);
    dst = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

// Solid, pixel-aligned rectangle.
static void FillRectPx(int x, int y, int w, int h, COLORREF c) {
    const int x0 = Max(0, x), y0 = Max(0, y);
    const int x1 = x + w < g_cv.w ? x + w : g_cv.w;
    const int y1 = y + h < g_cv.h ? y + h : g_cv.h;
    const DWORD v = ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
    for (int yy = y0; yy < y1; ++yy) {
        DWORD* row = g_cv.px + (size_t)yy * (size_t)g_cv.w;
        for (int xx = x0; xx < x1; ++xx) row[xx] = v;
    }
}

// Anti-aliased rounded rectangle (radius >= half the size gives a pill / circle).
static void FillRoundRect(float x, float y, float w, float h, float radius, const Ink& ink) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (radius > w / 2.0f) radius = w / 2.0f;
    if (radius > h / 2.0f) radius = h / 2.0f;
    if (radius < 0.0f) radius = 0.0f;
    const int x0 = Max(0, (int)floorf(x)), y0 = Max(0, (int)floorf(y));
    int x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x1 > g_cv.w) x1 = g_cv.w;
    if (y1 > g_cv.h) y1 = g_cv.h;
    const float cx = x + w / 2.0f, cy = y + h / 2.0f;
    const float hx = w / 2.0f - radius, hy = h / 2.0f - radius;
    for (int yy = y0; yy < y1; ++yy) {
        DWORD* row = g_cv.px + (size_t)yy * (size_t)g_cv.w;
        const float qy = fabsf((float)yy + 0.5f - cy) - hy;
        for (int xx = x0; xx < x1; ++xx) {
            const float qx = fabsf((float)xx + 0.5f - cx) - hx;
            float d;
            if (qx <= 0.0f && qy <= 0.0f) {
                d = (qx > qy ? qx : qy) - radius;
            } else {
                const float ax = qx > 0.0f ? qx : 0.0f, ay = qy > 0.0f ? qy : 0.0f;
                d = sqrtf(ax * ax + ay * ay) - radius;
            }
            const float cov = 0.5f - d;
            if (cov > 0.0f) Blend(row[xx], ink, cov);
        }
    }
}

static void FillCircle(float cx, float cy, float radius, const Ink& ink) {
    FillRoundRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, radius, ink);
}

// Anti-aliased stroke along a polyline, with round joins and caps.
static void StrokePath(const Pt* pts, int n, bool closed, float width, const Ink& ink) {
    if (n < 2) return;
    const float half = width / 2.0f;
    float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (int i = 1; i < n; ++i) {
        if (pts[i].x < minX) minX = pts[i].x;
        if (pts[i].x > maxX) maxX = pts[i].x;
        if (pts[i].y < minY) minY = pts[i].y;
        if (pts[i].y > maxY) maxY = pts[i].y;
    }
    const int x0 = Max(0, (int)floorf(minX - half - 1.0f)), y0 = Max(0, (int)floorf(minY - half - 1.0f));
    int x1 = (int)ceilf(maxX + half + 1.0f), y1 = (int)ceilf(maxY + half + 1.0f);
    if (x1 > g_cv.w) x1 = g_cv.w;
    if (y1 > g_cv.h) y1 = g_cv.h;
    const int segs = closed ? n : n - 1;
    const float reach = (half + 0.5f) * (half + 0.5f);
    for (int yy = y0; yy < y1; ++yy) {
        DWORD* row = g_cv.px + (size_t)yy * (size_t)g_cv.w;
        const float py = (float)yy + 0.5f;
        for (int xx = x0; xx < x1; ++xx) {
            const float px = (float)xx + 0.5f;
            float best = 1e12f;
            for (int i = 0; i < segs; ++i) {
                const Pt& a = pts[i];
                const Pt& b = pts[(i + 1) % n];
                const float vx = b.x - a.x, vy = b.y - a.y;
                const float wx = px - a.x, wy = py - a.y;
                const float len2 = vx * vx + vy * vy;
                float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                const float dx = wx - t * vx, dy = wy - t * vy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best) best = d2;
            }
            if (best >= reach) continue;
            const float cov = 0.5f - (sqrtf(best) - half);
            if (cov > 0.0f) Blend(row[xx], ink, cov);
        }
    }
}

// Anti-aliased convex polygon.
static void FillConvex(const Pt* pts, int n, const Ink& ink) {
    if (n < 3) return;
    float area = 0.0f;
    float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (int i = 0; i < n; ++i) {
        const Pt& a = pts[i];
        const Pt& b = pts[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
        if (a.x < minX) minX = a.x;
        if (a.x > maxX) maxX = a.x;
        if (a.y < minY) minY = a.y;
        if (a.y > maxY) maxY = a.y;
    }
    const float sign = area >= 0.0f ? 1.0f : -1.0f;
    const int x0 = Max(0, (int)floorf(minX - 1.0f)), y0 = Max(0, (int)floorf(minY - 1.0f));
    int x1 = (int)ceilf(maxX + 1.0f), y1 = (int)ceilf(maxY + 1.0f);
    if (x1 > g_cv.w) x1 = g_cv.w;
    if (y1 > g_cv.h) y1 = g_cv.h;
    for (int yy = y0; yy < y1; ++yy) {
        DWORD* row = g_cv.px + (size_t)yy * (size_t)g_cv.w;
        const float py = (float)yy + 0.5f;
        for (int xx = x0; xx < x1; ++xx) {
            const float px = (float)xx + 0.5f;
            float d = -1e12f;  // largest signed distance to an edge (negative inside)
            for (int i = 0; i < n; ++i) {
                const Pt& a = pts[i];
                const Pt& b = pts[(i + 1) % n];
                const float ex = b.x - a.x, ey = b.y - a.y;
                const float len = sqrtf(ex * ex + ey * ey);
                if (len <= 0.0f) continue;
                const float dist = ((px - a.x) * ey - (py - a.y) * ex) / len * sign;
                if (dist > d) d = dist;
            }
            const float cov = 0.5f - d;
            if (cov > 0.0f) Blend(row[xx], ink, cov);
        }
    }
}

static void AddArc(std::vector<Pt>& out, float cx, float cy, float radius, float startDeg, float sweepDeg,
                   int steps) {
    for (int i = 0; i <= steps; ++i) {
        const float a = (startDeg + sweepDeg * (float)i / (float)steps) * 3.14159265f / 180.0f;
        const Pt p = { cx + radius * cosf(a), cy + radius * sinf(a) };
        out.push_back(p);
    }
}

static void AddBezier(std::vector<Pt>& out, Pt p0, Pt c1, Pt c2, Pt p3, int steps) {
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps, m = 1.0f - t;
        const float b0 = m * m * m, b1 = 3.0f * m * m * t, b2 = 3.0f * m * t * t, b3 = t * t * t;
        const Pt p = { b0 * p0.x + b1 * c1.x + b2 * c2.x + b3 * p3.x,
                       b0 * p0.y + b1 * c1.y + b2 * c2.y + b3 * p3.y };
        out.push_back(p);
    }
}

// Rounded card with a hairline border.
static void DrawCard(int x, int y, int w, int h, int radius, COLORREF fill) {
    const int bw = Max(1, S(1));
    const float r = (float)radius;

    // Soft depth, glass edge and a restrained specular highlight.
    FillRoundRect((float)x, (float)(y + S(3)), (float)w, (float)h, r,
                  MakeInk(RGB(0x00, 0x00, 0x00), 0.34f));
    FillRoundRect((float)x, (float)y, (float)w, (float)h, r,
                  MakeInk(RGB(0x66, 0x63, 0xA0), 0.42f));
    FillRoundRect((float)(x + bw), (float)(y + bw), (float)(w - 2 * bw), (float)(h - 2 * bw),
                  (float)(radius - bw), MakeInk(fill));

    // Top-edge reflection and a faint inner sheen, like the mouse app's panels.
    FillRoundRect((float)(x + S(2)), (float)(y + S(1)), (float)(w - S(4)), (float)S(1),
                  (float)S(1), MakeInk(C_WHITE, 0.13f));
    FillRoundRect((float)(x + S(2)), (float)(y + S(2)), (float)(w - S(4)), (float)S(7),
                  (float)S(4), MakeInk(C_WHITE, 0.025f));
}

// Circular arrow.
static void DrawRefreshIcon(const RECT& r, COLORREF color) {
    const float u = (float)g_scale;
    const float cx = (float)(r.left + r.right) / 2.0f, cy = (float)(r.top + r.bottom) / 2.0f;
    const float rad = 5.6f * u;
    const float startDeg = 40.0f, sweepDeg = 270.0f;
    const Ink ink = MakeInk(color);

    std::vector<Pt> arc;
    AddArc(arc, cx, cy, rad, startDeg, sweepDeg, 30);
    StrokePath(arc.data(), (int)arc.size(), false, 1.5f * u, ink);

    // Arrow head at the end of the arc, pointing clockwise.
    const float a = (startDeg + sweepDeg) * 3.14159265f / 180.0f;
    const float nx = cosf(a), ny = sinf(a);  // radial direction
    const float tx = -ny, ty = nx;           // clockwise tangent
    const float ex = cx + rad * nx, ey = cy + rad * ny;
    const Pt tri[3] = {
        { ex + tx * 3.0f * u, ey + ty * 3.0f * u },
        { ex + nx * 2.7f * u - tx * 0.9f * u, ey + ny * 2.7f * u - ty * 0.9f * u },
        { ex - nx * 2.7f * u - tx * 0.9f * u, ey - ny * 2.7f * u - ty * 0.9f * u },
    };
    FillConvex(tri, 3, ink);
}

// Bell; `on == false` adds a slash. `behind` is the colour underneath the icon.
static void DrawBellIcon(const RECT& r, COLORREF color, bool on, COLORREF behind) {
    const float u = (float)g_scale;
    const float ox = (float)(r.left + r.right) / 2.0f - 8.0f * u;
    const float oy = (float)(r.top + r.bottom) / 2.0f - 8.2f * u;
    auto P = [&](float x, float y) {
        const Pt p = { ox + x * u, oy + y * u };
        return p;
    };
    const Ink ink = MakeInk(color);
    const float stroke = 1.3f * u;

    std::vector<Pt> body;
    AddArc(body, ox + 8.0f * u, oy + 5.4f * u, 4.0f * u, 180.0f, 180.0f, 16);          // dome
    AddBezier(body, P(12.0f, 5.4f), P(12.0f, 10.0f), P(14.0f, 11.4f), P(14.0f, 11.4f), 10);
    body.push_back(P(2.0f, 11.4f));                                                    // rim
    AddBezier(body, P(2.0f, 11.4f), P(2.0f, 11.4f), P(4.0f, 10.0f), P(4.0f, 5.4f), 10);
    StrokePath(body.data(), (int)body.size(), true, stroke, ink);

    std::vector<Pt> clapper;
    clapper.push_back(P(6.8f, 13.9f));
    AddBezier(clapper, P(6.8f, 13.9f), P(7.2f, 15.1f), P(8.8f, 15.1f), P(9.2f, 13.9f), 8);
    StrokePath(clapper.data(), (int)clapper.size(), false, stroke, ink);

    if (!on) {
        const Pt slash[2] = { P(2.6f, 2.4f), P(13.4f, 14.0f) };
        StrokePath(slash, 2, false, 3.8f * u, MakeInk(behind));
        StrokePath(slash, 2, false, stroke, ink);
    }
}

static void DrawChevron(int x, int cy, COLORREF color) {
    const float u = (float)g_scale;
    const Pt pts[3] = {
        { (float)x, (float)cy - 2.0f * u },
        { (float)x + 4.0f * u, (float)cy + 2.0f * u },
        { (float)x + 8.0f * u, (float)cy - 2.0f * u },
    };
    StrokePath(pts, 3, false, 1.4f * u, MakeInk(color));
}

static void DrawToggle(int x, int y, bool on, bool dim) {
    const int w = S(32), h = S(18), inset = S(3);
    const float alpha = dim ? 0.4f : 1.0f;
    FillRoundRect((float)x, (float)y, (float)w, (float)h, (float)h / 2.0f,
                  MakeInk(on ? C_ACCENT : C_TOGGLE_OFF, alpha));
    const float kr = (float)(h - 2 * inset) / 2.0f;
    const float kx = on ? (float)(x + w - inset) - kr : (float)(x + inset) + kr;
    FillCircle(kx, (float)y + (float)h / 2.0f, kr, MakeInk(on ? C_WHITE : C_KNOB_OFF, alpha));
}

static bool CellState(int cell) {
    switch (cell) {
    case CELL_NOTIFY: return g_set.notifications;
    case CELL_SOUND: return g_set.sound;
    case CELL_AUTOSTART: return g_autostart;
    }
    return false;
}

static void DrawShapes(int clientW, int clientH) {
    const Layout& L = g_L;
    const int n = (int)g_products.size();
    const int bw = Max(1, S(1));

    FillRectPx(0, 0, clientW, clientH, C_BG);

    // Subtle violet ambient light behind the glass surfaces.
    FillCircle((float)clientW * 0.82f, (float)S(34), (float)S(150),
               MakeInk(C_ACCENT, 0.055f));
    FillCircle((float)S(28), (float)clientH * 0.76f, (float)S(110),
               MakeInk(RGB(0x4B, 0x70, 0xC8), 0.035f));

    // Refresh button
    {
        const RECT& r = L.refresh;
        const bool hot = g_hot == HIT_REFRESH && !g_fetching;
        if (hot)
            FillRoundRect((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top),
                          (float)S(8), MakeInk(g_pressed == HIT_REFRESH ? C_LINE : C_CARD_HOT));
        DrawRefreshIcon(r, g_fetching ? C_TEXT3 : (hot ? C_TEXT : C_TEXT2));
    }

    // Product list
    DrawCard(L.listX, L.listY, L.listW, n * L.rowH, S(10), C_CARD);
    for (int i = 0; i < n; ++i) {
        const Product& p = g_products[i];
        const RECT row = RowRect(i);
        if (i > 0) FillRectPx(row.left + S(14), row.top, L.listW - S(28), bw, C_LINE);

        const RowView v = ViewOf(p);
        FillCircle((float)(row.left + S(14)) + (float)S(8) / 2.0f, (float)row.top + (float)L.rowH / 2.0f,
                   (float)S(8) / 2.0f, MakeInk(v.dot));

        const RECT br = BellRect(i);
        const bool hot = g_hot == HIT_BELL0 + i;
        if (hot)
            FillRoundRect((float)br.left, (float)br.top, (float)(br.right - br.left),
                          (float)(br.bottom - br.top), (float)S(7), MakeInk(C_CARD_HOT));
        COLORREF bc = C_TEXT3;
        if (p.notify && g_set.notifications) bc = hot ? C_TEXT : C_TEXT2;
        DrawBellIcon(br, bc, p.notify, hot ? C_CARD_HOT : C_CARD);
    }

    // Settings cells
    for (int i = 0; i < CELL_COUNT; ++i) {
        const RECT& c = L.cells[i];
        const bool hot = g_hot == HIT_CELL0 + i;
        DrawCard(c.left, c.top, c.right - c.left, c.bottom - c.top, S(9), hot ? C_CARD_HOT : C_CARD);
        const int cy = (c.top + c.bottom) / 2;
        if (i == CELL_INTERVAL) {
            DrawChevron(c.right - S(12) - S(8), cy, hot ? C_TEXT : C_TEXT2);
        } else {
            const bool dim = i == CELL_SOUND && !g_set.notifications;
            DrawToggle(c.right - S(12) - S(32), cy - S(18) / 2, CellState(i), dim);
        }
    }
}

static void DrawTextIn(HDC dc, HFONT font, COLORREF color, int l, int t, int r, int b,
                       const std::wstring& text, UINT format) {
    if (r <= l || text.empty()) return;
    RECT rc = { l, t, r, b };
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

static int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    SIZE sz = { 0, 0 };
    HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static void DrawTexts(HDC dc) {
    const Layout& L = g_L;
    const int n = (int)g_products.size();
    SetBkMode(dc, TRANSPARENT);

    // Header
    COLORREF subColor = C_TEXT2;
    const std::wstring sub = Subline(subColor);
    const int headRight = L.refresh.left - S(12);
    DrawTextIn(dc, g_fTitle, C_TEXT, L.pad, S(22), headRight, S(48), L"RoCheats Monitor", DT_LEFT | DT_END_ELLIPSIS);
    DrawTextIn(dc, g_fSub, subColor, L.pad, S(49), headRight, S(69), sub, DT_LEFT | DT_END_ELLIPSIS);

    // Columns
    const int statusR = L.listX + L.listW - S(42);
    const int statusL = statusR - S(104);
    const int detectR = statusL - S(4);
    const int detectL = detectR - S(94);
    const int nameL = L.listX + S(32);
    const int nameR = detectL - S(8);

    const int capsT = S(84), capsB = S(104);
    SetTextCharacterExtra(dc, S(0.7));
    DrawTextIn(dc, g_fCaps, C_TEXT3, L.listX + S(14), capsT, detectL, capsB, L"PRODUCT", DT_LEFT);
    DrawTextIn(dc, g_fCaps, C_TEXT3, detectL - S(20), capsT, detectR, capsB, L"DETECTION", DT_RIGHT);
    DrawTextIn(dc, g_fCaps, C_TEXT3, statusL, capsT, statusR, capsB, L"STATUS", DT_RIGHT);
    SetTextCharacterExtra(dc, 0);

    // Rows
    for (int i = 0; i < n; ++i) {
        const Product& p = g_products[i];
        const RECT row = RowRect(i);
        const RowView v = ViewOf(p);
        const bool live = HasStatus(p);

        DrawTextIn(dc, g_fName, live ? C_TEXT : C_TEXT2, nameL, row.top, nameR, row.bottom, p.name,
                   DT_LEFT | DT_END_ELLIPSIS);
        const int versionL = nameL + TextWidth(dc, g_fName, p.name) + S(8);
        if (nameR - versionL >= S(24))
            DrawTextIn(dc, g_fSmall, C_TEXT3, versionL, row.top, nameR, row.bottom, v.version,
                       DT_LEFT | DT_END_ELLIPSIS);
        DrawTextIn(dc, g_fSmall, live ? C_TEXT2 : C_TEXT3, detectL, row.top, detectR, row.bottom,
                   v.detect, DT_RIGHT);
        DrawTextIn(dc, g_fStatus, v.statusColor, statusL, row.top, statusR, row.bottom, v.status, DT_RIGHT);
    }

    // Settings cells
    static const wchar_t* const labels[CELL_COUNT] = { L"Check every", L"Notifications", L"Sound",
                                                       L"Start with Windows" };
    for (int i = 0; i < CELL_COUNT; ++i) {
        const RECT& c = L.cells[i];
        const bool hot = g_hot == HIT_CELL0 + i;
        const bool dim = i == CELL_SOUND && !g_set.notifications;
        DrawTextIn(dc, g_fCell, dim ? C_TEXT3 : (hot ? C_TEXT : C_TEXT2), c.left + S(12), c.top,
                   c.right - S(52), c.bottom, labels[i], DT_LEFT | DT_END_ELLIPSIS);
        if (i == CELL_INTERVAL)
            DrawTextIn(dc, g_fCellVal, C_TEXT, c.left + S(12), c.top, c.right - S(12) - S(8) - S(7),
                       c.bottom, IntervalShort(g_set.intervalMin), DT_RIGHT);
    }
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w > 0 && h > 0 && EnsureCanvas(hdc, w, h)) {
        GdiFlush();  // make sure GDI is done with the bitmap before touching its pixels
        DrawShapes(w, h);
        DrawTexts(g_cv.dc);
        BitBlt(hdc, 0, 0, w, h, g_cv.dc, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Tray icon + notifications
// ---------------------------------------------------------------------------
static void InitNid(NOTIFYICONDATAW& nid) {
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
}

static std::wstring TrayTip() { return std::wstring(APP_NAME) + L"\n" + Summary(); }

static bool AddTrayIcon() {
    NOTIFYICONDATAW nid;
    InitNid(nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall;
    lstrcpynW(nid.szTip, TrayTip().c_str(), ARRAYSIZE(nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        Shell_NotifyIconW(NIM_DELETE, &nid);  // a stale icon may be left from a timed-out add
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_trayAdded = false;
            return false;
        }
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    g_trayV4 = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
    g_trayAdded = true;
    return true;
}

static void EnsureTrayIcon() {
    if (AddTrayIcon()) {
        KillTimer(g_hwnd, TIMER_TRAY);
        return;
    }
    // The taskbar may not exist yet right after sign-in; keep trying for a while.
    if (++g_trayRetries <= 60) SetTimer(g_hwnd, TIMER_TRAY, 2000, nullptr);
    else KillTimer(g_hwnd, TIMER_TRAY);
}

static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid;
    InitNid(nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

static void UpdateTrayTip() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid;
    InitNid(nid);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(nid.szTip, TrayTip().c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Native Windows notification (shown as a toast on Windows 10/11).
static void ShowNotification(const std::wstring& title, const std::wstring& text) {
    if (!g_trayAdded && !AddTrayIcon()) return;
    NOTIFYICONDATAW nid;
    InitNid(nid);
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, Clip(title, ARRAYSIZE(nid.szInfoTitle) - 1).c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, Clip(text, ARRAYSIZE(nid.szInfo) - 1).c_str(), ARRAYSIZE(nid.szInfo));
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | (g_set.sound ? 0 : NIIF_NOSOUND);
    nid.hBalloonIcon = g_iconLarge;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid) && AddTrayIcon()) Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static const wchar_t* const TXT_UP   = L"\U0001F7E2 Updated";       // green circle
static const wchar_t* const TXT_DOWN = L"\U0001F534 Not updated";   // red circle

static void NotifyChanges(const std::vector<Change>& changes) {
    if (changes.empty()) return;
    std::vector<int> prods;
    for (const Change& c : changes) {
        bool seen = false;
        for (int i : prods) seen = seen || i == c.index;
        if (!seen) prods.push_back(c.index);
    }

    std::wstring title, body;
    if (prods.size() == 1) {
        const Product& p = g_products[prods[0]];
        if (changes.size() == 1) {
            const Change& c = changes[0];
            if (c.kind == 0) {
                title = p.name + (c.now ? L" is updated" : L" is no longer updated");
                body = c.now ? std::wstring(TXT_DOWN) + L" \u2192 " + TXT_UP
                             : std::wstring(TXT_UP) + L" \u2192 " + TXT_DOWN;
                if (c.now && !p.version.empty()) body += L"\nVersion " + p.version;
            } else {
                title = p.name + (c.now ? L" is now detected" : L" is now undetected");
                body = c.now ? L"Undetected \u2192 Detected" : L"Detected \u2192 Undetected";
            }
        } else {
            title = p.name + L" status changed";
            for (const Change& c : changes) {
                if (!body.empty()) body += L"\n";
                if (c.kind == 0)
                    body += c.now ? std::wstring(TXT_DOWN) + L" \u2192 " + TXT_UP
                                  : std::wstring(TXT_UP) + L" \u2192 " + TXT_DOWN;
                else
                    body += c.now ? L"Undetected \u2192 Detected" : L"Detected \u2192 Undetected";
            }
        }
    } else {
        title = std::to_wstring(prods.size()) + L" products changed";
        const std::wstring heads[4] = { std::wstring(TXT_UP) + L": ", std::wstring(TXT_DOWN) + L": ",
                                        L"Detected: ", L"Undetected: " };
        for (int group = 0; group < 4; ++group) {
            std::wstring names;
            for (const Change& c : changes) {
                const int gidx = c.kind == 0 ? (c.now ? 0 : 1) : (c.now ? 2 : 3);
                if (gidx != group) continue;
                if (!names.empty()) names += L", ";
                names += g_products[c.index].name;
            }
            if (names.empty()) continue;
            if (!body.empty()) body += L"\n";
            body += heads[group] + names;
        }
    }
    ShowNotification(title, body);
}

// ---------------------------------------------------------------------------
// Applying a poll result
// ---------------------------------------------------------------------------
static const FetchItem* FindItem(const FetchResult& r, const Product& p) {
    const FetchItem* best = nullptr;
    for (const FetchItem& it : r.items) {
        if (Lower(it.title) != p.key) continue;
        // Same name on several platforms: prefer the Windows entry.
        if (!best || (lstrcmpiW(it.platform.c_str(), L"Windows") == 0 &&
                      lstrcmpiW(best->platform.c_str(), L"Windows") != 0))
            best = &it;
    }
    return best;
}

static void ApplyResult(const FetchResult& r) {
    std::vector<Change> changes;
    for (size_t i = 0; i < g_products.size(); ++i) {
        Product& p = g_products[i];
        const FetchItem* it = FindItem(r, p);
        if (!it) {
            p.tracked = false;
            continue;
        }
        p.tracked = true;
        bool dirty = false;
        const int nu = it->updated ? 1 : 0;
        if (p.updated != nu) {
            if (p.updated != -1) changes.push_back({ (int)i, 0, nu == 1 });  // -1 = first sighting
            p.updated = nu;
            dirty = true;
        }
        if (it->hasDetected) {
            const int nd = it->detected ? 1 : 0;
            if (p.detected != nd) {
                if (p.detected != -1) changes.push_back({ (int)i, 1, nd == 1 });
                p.detected = nd;
                dirty = true;
            }
        }
        if (p.version != it->version) {
            p.version = it->version;
            dirty = true;
        }
        if (dirty) SaveProduct(p);
    }

    if (!g_set.notifications) return;
    std::vector<Change> wanted;
    for (const Change& c : changes)
        if (g_products[c.index].notify) wanted.push_back(c);
    NotifyChanges(wanted);
}

static void OnFetched(FetchResult* r) {
    g_fetching = false;

    if (r->ok) {
        // A response that lists none of our products while we had data before is
        // more likely a hiccup than nine products vanishing at once: keep state.
        bool anyMatch = false, hadData = false;
        for (const Product& p : g_products) {
            anyMatch = anyMatch || FindItem(*r, p) != nullptr;
            hadData = hadData || p.updated != -1;
        }
        if (!anyMatch && hadData) r->ok = false;
    }

    if (r->ok) {
        ApplyResult(*r);
        g_everSucceeded = true;
        g_lastFailed = false;
        g_failText.clear();
        g_lastChecked = NowTime();
        SetTimer(g_hwnd, TIMER_POLL, (UINT)g_set.intervalMin * 60000u, nullptr);
    } else {
        g_lastFailed = true;
        g_failText = r->rateLimited ? L"Rate limited by WEAO" : L"Can't reach WEAO";
        g_retrySec = r->rateLimited ? r->retryAfterSec + 5 : RETRY_SECONDS;
        SetTimer(g_hwnd, TIMER_POLL, (UINT)g_retrySec * 1000u, nullptr);
    }
    delete r;

    UpdateTrayTip();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window behaviour
// ---------------------------------------------------------------------------
static void ShowMainWindow() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

static void HideToTray() {
    if (!g_trayAdded) {  // no tray to hide in: just minimise
        ShowWindow(g_hwnd, SW_MINIMIZE);
        return;
    }
    SaveWindowPos();
    ShowWindow(g_hwnd, SW_HIDE);
    if (!g_set.trayHintShown) {
        g_set.trayHintShown = true;
        IniSetInt(L"Settings", L"TrayHintShown", 1);
        ShowNotification(L"Still running in the tray",
                         L"RoCheats Monitor keeps checking in the background. Right-click the tray icon to exit.");
    }
}

static void ExitApp() {
    SaveWindowPos();
    DestroyWindow(g_hwnd);
}

static void SetHot(int hit) {
    if (hit == g_hot) return;
    g_hot = hit;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void RefreshHot() {
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) != g_hwnd) {
        SetHot(HIT_NONE);
        return;
    }
    ScreenToClient(g_hwnd, &pt);
    SetHot(HitTest(pt));
}

static void ShowIntervalMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const int n = (int)ARRAYSIZE(INTERVALS);
    int checked = -1;
    for (int i = 0; i < n; ++i) {
        AppendMenuW(menu, MF_STRING, ID_INTERVAL_BASE + i, IntervalLong(INTERVALS[i]).c_str());
        if (INTERVALS[i] == g_set.intervalMin) checked = i;
    }
    if (checked >= 0) CheckMenuRadioItem(menu, 0, n - 1, checked, MF_BYPOSITION);

    const RECT& c = g_L.cells[CELL_INTERVAL];
    POINT pt = { c.left, c.bottom + S(4) };
    ClientToScreen(g_hwnd, &pt);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd >= ID_INTERVAL_BASE && cmd < ID_INTERVAL_BASE + n) {
        g_set.intervalMin = INTERVALS[cmd - ID_INTERVAL_BASE];
        IniSetInt(L"Settings", L"IntervalMinutes", g_set.intervalMin);
        if (!g_fetching && !g_lastFailed)
            SetTimer(g_hwnd, TIMER_POLL, (UINT)g_set.intervalMin * 60000u, nullptr);
    }
    RefreshHot();
}

static void HandleCommand(int cmd) {
    switch (cmd) {
    case ID_OPEN: ShowMainWindow(); break;
    case ID_CHECK: ManualCheck(); break;
    case ID_TEST:
        ShowNotification(L"VOLT is updated",
                         std::wstring(TXT_DOWN) + L" \u2192 " + TXT_UP +
                             L"\nThis is a test. Real alerts look like this.");
        break;
    case ID_EXIT: ExitApp(); break;
    }
}

static void ShowTrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open");
    AppendMenuW(menu, MF_STRING | (g_fetching ? MF_GRAYED : 0), ID_CHECK, L"Check now");
    AppendMenuW(menu, MF_STRING, ID_TEST, L"Send test notification");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(menu, ID_OPEN, FALSE);

    SetForegroundWindow(g_hwnd);  // required so the menu closes when clicking elsewhere
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                        x, y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    HandleCommand(cmd);
}

static void OnTrayMessage(WPARAM wParam, LPARAM lParam) {
    switch (LOWORD(lParam)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case NIN_BALLOONUSERCLICK:
    case WM_LBUTTONDBLCLK:
        ShowMainWindow();
        break;
    case WM_LBUTTONUP:
        if (!g_trayV4) ShowMainWindow();  // version 4 sends NIN_SELECT instead
        break;
    case WM_CONTEXTMENU:
        if (g_trayV4) {
            ShowTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
            break;
        }
        // fall through
    case WM_RBUTTONUP:
        if (!g_trayV4 || LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(pt.x, pt.y);
        }
        break;
    }
}

static void Activate(int hit) {
    if (hit == HIT_REFRESH) {
        ManualCheck();
    } else if (hit == HIT_CELL0 + CELL_INTERVAL) {
        ShowIntervalMenu();
    } else if (hit == HIT_CELL0 + CELL_NOTIFY) {
        g_set.notifications = !g_set.notifications;
        IniSetInt(L"Settings", L"Notifications", g_set.notifications ? 1 : 0);
    } else if (hit == HIT_CELL0 + CELL_SOUND) {
        g_set.sound = !g_set.sound;
        IniSetInt(L"Settings", L"Sound", g_set.sound ? 1 : 0);
    } else if (hit == HIT_CELL0 + CELL_AUTOSTART) {
        const bool want = !g_autostart;
        if (SetAutostart(want)) g_autostart = want;
    } else if (hit >= HIT_BELL0 && hit < HIT_BELL0 + (int)g_products.size()) {
        Product& p = g_products[hit - HIT_BELL0];
        p.notify = !p.notify;
        SaveProduct(p);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {  // Explorer restarted
        g_trayAdded = false;
        g_trayRetries = 0;
        EnsureTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_NCCREATE:
        g_hwnd = hwnd;
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        SetHot(HitTest(pt));
        if (!g_mouseTracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            g_mouseTracking = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouseTracking = false;
        SetHot(HIT_NONE);
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_pressed = HitTest(pt);
        if (g_pressed != HIT_NONE) {
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int pressed = g_pressed;
        g_pressed = HIT_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        const int hit = HitTest(pt);
        if (hit != HIT_NONE && hit == pressed) Activate(hit);
        else InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        g_pressed = HIT_NONE;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt) != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_F5) ManualCheck();
        else if (wParam == VK_ESCAPE) HideToTray();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_POLL) {
            KillTimer(hwnd, TIMER_POLL);
            StartFetch();
        } else if (wParam == TIMER_TRAY) {
            EnsureTrayIcon();
        }
        return 0;

    case WM_APP_FETCHED:
        OnFetched((FetchResult*)lParam);
        return 0;

    case WM_APP_SHOW:
        ShowMainWindow();
        return 0;

    case WM_APP_TRAY:
        OnTrayMessage(wParam, lParam);
        return 0;

    case WM_DPICHANGED: {
        const RECT* suggested = (const RECT*)lParam;
        const POINT pt = { suggested->left, suggested->top };
        ApplyDpi(hwnd, HIWORD(wParam), &pt);
        return 0;
    }

    case WM_EXITSIZEMOVE:
        SaveWindowPos();
        return 0;

    case WM_POWERBROADCAST:
        // Timers do not account for sleep: check shortly after waking up.
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            if (!g_fetching) SetTimer(hwnd, TIMER_POLL, 10000, nullptr);
        }
        return TRUE;

    case WM_CLOSE:  // the X button hides to the tray; use the tray menu to exit
        HideToTray();
        return 0;

    case WM_ENDSESSION:
        if (wParam) {
            SaveWindowPos();
            RemoveTrayIcon();
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        KillTimer(hwnd, TIMER_TRAY);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Dark chrome
// ---------------------------------------------------------------------------
// Dark context menus. Uses the uxtheme ordinals that Windows' own apps use
// (available since Windows 10 1903); silently skipped anywhere else.
static void EnableDarkMenus() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtlGetVersion = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW vi;
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (!rtlGetVersion || rtlGetVersion(&vi) != 0) return;
    if (vi.dwMajorVersion < 10 || vi.dwBuildNumber < 18362) return;

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppModeFn)(int);
    typedef void(WINAPI * FlushMenuThemesFn)(void);
    SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2);  // ForceDark
    if (flush) flush();
}

// Dark title bar; on Windows 11 the caption also takes the window's colour.
static void StyleTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    typedef HRESULT(WINAPI * SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    SetAttrFn setAttr = (SetAttrFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!setAttr) return;
    const BOOL dark = TRUE;
    if (FAILED(setAttr(hwnd, 20, &dark, sizeof dark))) setAttr(hwnd, 19, &dark, sizeof dark);
    const COLORREF caption = C_BG, border = C_LINE, text = C_CAPTION_TX;
    setAttr(hwnd, 35, &caption, sizeof caption);  // DWMWA_CAPTION_COLOR
    setAttr(hwnd, 34, &border, sizeof border);    // DWMWA_BORDER_COLOR
    setAttr(hwnd, 36, &text, sizeof text);        // DWMWA_TEXT_COLOR
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = hInst;
    const bool startHidden = cmdLine && (wcsstr(cmdLine, L"--tray") || wcsstr(cmdLine, L"/tray"));

    // Single instance: a second launch just brings the first one to the front.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startHidden) {
            HWND other = FindWindowW(WND_CLASS, nullptr);
            if (other) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_APP_SHOW, 0, 0);
            }
        }
        CloseHandle(mutex);
        return 0;
    }

    EnableDarkMenus();

    InitIniPath();
    LoadSettings();
    g_autostart = AutostartEnabled();
    if (g_autostart) SetAutostart(true);  // keep the path current if the exe was moved

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_iconLarge = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                    GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_iconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    // Restore the last position if it is still on a connected screen.
    bool havePos = g_savedX != INT_MIN && g_savedY != INT_MIN;
    if (havePos) {
        const RECT probe = { g_savedX, g_savedY, g_savedX + 200, g_savedY + 100 };
        havePos = MonitorFromRect(&probe, MONITOR_DEFAULTTONULL) != nullptr;
    }
    HWND hwnd = CreateWindowExW(WND_EXSTYLE, WND_CLASS, APP_NAME, WND_STYLE,
                                havePos ? g_savedX : CW_USEDEFAULT, havePos ? g_savedY : CW_USEDEFAULT,
                                400, 400, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    StyleTitleBar(hwnd);
    ApplyDpi(hwnd, DpiForWindow(hwnd), nullptr);
    PlaceWindow(hwnd, !havePos);

    EnsureTrayIcon();
    StartFetch();

    if (!startHidden) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HFONT fonts[] = { g_fTitle, g_fSub, g_fName, g_fSmall, g_fStatus, g_fCaps, g_fCell, g_fCellVal };
    for (HFONT f : fonts)
        if (f) DeleteObject(f);
    if (g_iconLarge) DestroyIcon(g_iconLarge);
    if (g_iconSmall) DestroyIcon(g_iconSmall);
    FreeCanvas();
    if (mutex) CloseHandle(mutex);
    return 0;
}
