/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/steam_user_stats.h"
#include "dll/settings_parser.h"
#include "dll/local_storage.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <conio.h> // _getch
#include <cstdarg> // va_list, va_start, va_end
#include <gdiplus.h>


// ============================================================
// Console helpers (Windows only)
// ============================================================

static void console_open(const std::string &title)
{
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
    SetConsoleTitleA(title.c_str());

    // Enable VT escape sequences on Windows 10+
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, mode);
    }
}

static void console_close()
{
    // Redirect std handles away from console first
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
    freopen("NUL", "r", stdin);

    // Hide the console window before freeing to avoid a lingering window
    HWND hConWnd = GetConsoleWindow();
    if (hConWnd) ShowWindow(hConWnd, SW_HIDE);

    FreeConsole();
}

static void console_clear()
{
    printf("\033[2J\033[H");
    fflush(stdout);
}

// Wait for Enter key
static void console_wait_enter()
{
    for (;;) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n') break;
    }
}


// ============================================================
// HTTP helpers (curl)
// ============================================================

static size_t curl_write_string(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    ((std::string*)userp)->append((char*)contents, total);
    return total;
}

struct CurlMem {
    std::vector<char> data;
};

static size_t curl_write_mem(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    auto *mem = (CurlMem*)userp;
    mem->data.insert(mem->data.end(), (char*)contents, (char*)contents + total);
    return total;
}

static std::string http_get(const std::string &url, long timeout = 20L)
{
    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoldbergEmulator/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return {};

    return resp;
}

static CurlMem http_get_binary(const std::string &url, long timeout = 15L)
{
    CurlMem mem;
    CURL *curl = curl_easy_init();
    if (!curl) return mem;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoldbergEmulator/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) mem.data.clear();
    return mem;
}


// ============================================================
// File helpers
// ============================================================

static std::string extract_filename(const std::string &url)
{
    if (url.empty()) return {};
    auto pos = url.rfind('/');
    if (pos == std::string::npos) return {};
    return url.substr(pos + 1);
}


// ============================================================
// Progress printing helpers
// ============================================================

static void print_ok(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  \033[32m[OK]\033[0m "); // green OK
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void print_fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  \033[31m[!!]\033[0m "); // red fail
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void print_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  \033[36m*\033[0m "); // cyan bullet
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}


// ============================================================
// Steam interface detection (scan DLL for version strings)
// ============================================================

// Interface name prefixes to search for in the DLL binary
// Each is a prefix optionally followed by digits (like "SteamFriends023")
static const char *interface_patterns[] = {
    "STEAMAPPS_INTERFACE_VERSION",
    "SteamApps",
    "STEAMAPPLIST_INTERFACE_VERSION",
    "STEAMAPPTICKET_INTERFACE_VERSION",
    "SteamClient",
    "STEAMCONTROLLER_INTERFACE_VERSION",
    "SteamController",
    "SteamFriends",
    "SteamGameServerStats",
    "SteamGameCoordinator",
    "SteamGameServer",
    "STEAMHTMLSURFACE_INTERFACE_VERSION_",
    "STEAMHTTP_INTERFACE_VERSION",
    "SteamInput",
    "STEAMINVENTORY_INTERFACE_V",
    "SteamMatchMakingServers",
    "SteamMatchMaking",
    "SteamMatchGameSearch",
    "SteamParties",
    "STEAMMUSIC_INTERFACE_VERSION",
    "STEAMMUSICREMOTE_INTERFACE_VERSION",
    "SteamNetworkingMessages",
    "SteamNetworkingSockets",
    "SteamNetworkingUtils",
    "SteamNetworking",
    "STEAMPARENTALSETTINGS_INTERFACE_VERSION",
    "STEAMREMOTEPLAY_INTERFACE_VERSION",
    "STEAMREMOTESTORAGE_INTERFACE_VERSION",
    "STEAMSCREENSHOTS_INTERFACE_VERSION",
    "STEAMTIMELINE_INTERFACE_V",
    "STEAMUGC_INTERFACE_VERSION",
    "SteamUser",
    "STEAMUSERSTATS_INTERFACE_VERSION",
    "SteamUtils",
    "STEAMVIDEO_INTERFACE_V",
    "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION",
    "SteamMasterServerUpdater",
};

// Scan DLL binary for all interface version strings
static std::vector<std::string> scan_interfaces(const std::string &dll_path)
{
    std::ifstream file(std::filesystem::u8path(dll_path), std::ios::binary);
    if (!file) return {};

    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::vector<std::string> results;
    std::string content(data.begin(), data.end());

    for (const char *prefix : interface_patterns) {
        size_t plen = strlen(prefix);
        size_t pos = 0;
        bool found = false;

        while ((pos = content.find(prefix, pos)) != std::string::npos) {
            found = true;
            // Collect trailing digits (if any)
            size_t end = pos + plen;
            while (end < content.size() && isdigit((unsigned char)content[end])) {
                ++end;
            }
            results.push_back(content.substr(pos, end - pos));
            pos = end;
        }

        // Special handling: in newer SDKs only SteamClient017 is valid
        if (strcmp(prefix, "SteamClient") == 0 && found) {
            auto it = std::find(results.begin(), results.end(), "SteamClient017");
            results.erase(
                std::remove_if(results.begin(), results.end(),
                    [&](const std::string &s) {
                        return s.find("SteamClient") == 0 && s != "SteamClient017";
                    }),
                results.end()
            );
        }
    }

    // Sort and deduplicate
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    return results;
}

// ============================================================
// Language detection via Steam Store API
// ============================================================

// Map Steam Store display names to internal language codes
static const char *map_store_lang_to_code(const std::string &name)
{
    struct LangMap { const char *display; const char *code; };
    static const LangMap table[] = {
        {"English",                "english"},
        {"Portuguese - Brazil",    "brazilian"},
        {"Portuguese - Portugal",  "portuguese"},
        {"Portuguese",             "portuguese"},
        {"French",                 "french"},
        {"Italian",                "italian"},
        {"German",                 "german"},
        {"Japanese",               "japanese"},
        {"Russian",                "russian"},
        {"Simplified Chinese",     "schinese"},
        {"Traditional Chinese",    "tchinese"},
        {"Spanish - Latin America","latam"},
        {"Spanish - Spain",        "spanish"},
        {"Spanish",                "spanish"},
        {"Korean",                 "koreana"},
        {"Polish",                 "polish"},
        {"Dutch",                  "dutch"},
        {"Turkish",                "turkish"},
        {"Swedish",                "swedish"},
        {"Norwegian",              "norwegian"},
        {"Danish",                 "danish"},
        {"Czech",                  "czech"},
        {"Romanian",               "romanian"},
        {"Hungarian",              "hungarian"},
        {"Finnish",                "finnish"},
        {"Thai",                   "thai"},
        {"Vietnamese",             "vietnamese"},
        {"Arabic",                 "arabic"},
        {"Ukrainian",              "ukrainian"},
        {"Greek",                  "greek"},
        {"Bulgarian",              "bulgarian"},
        {"Croatian",               "croatian"},
        {"Indonesian",             "indonesian"},
        {"Malay",                  "malay"},
        {"Slovak",                 "slovak"},
    };
    for (auto &m : table) {
        if (name == m.display) return m.code;
    }
    return nullptr;
}

// Fetch supported languages from store API and map to internal codes
static std::vector<std::string> fetch_supported_languages(uint32 appid)
{
    std::string url = "https://store.steampowered.com/api/appdetails?appids="
        + std::to_string(appid) + "&cc=us&l=en";
    std::string resp = http_get(url, 10L);
    if (resp.empty()) return {};

    try {
        nlohmann::json j = nlohmann::json::parse(resp);
        std::string appid_str = std::to_string(appid);
        auto &data = j[appid_str]["data"];
        if (data.is_null()) return {};

        std::string raw = data.value("supported_languages", std::string{});
        if (raw.empty()) return {};

        // Split on <br> first (extraneous text follows)
        static const std::string br_tag = "<br";
        auto br_pos = raw.find(br_tag);
        if (br_pos != std::string::npos) raw = raw.substr(0, br_pos);

        // Strip remaining HTML tags and asterisks
        std::string plain;
        bool in_tag = false;
        for (char ch : raw) {
            if (ch == '<') { in_tag = true; continue; }
            if (ch == '>') { in_tag = false; continue; }
            if (!in_tag && ch != '*') plain += ch;
        }

        std::vector<std::string> codes;
        size_t start = 0, end;
        while ((end = plain.find(',', start)) != std::string::npos) {
            std::string part = plain.substr(start, end - start);
            // trim
            auto first = part.find_first_not_of(" \t\r\n");
            auto last = part.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) part = part.substr(first, last - first + 1);

            const char *code = map_store_lang_to_code(part);
            if (code) codes.emplace_back(code);
            start = end + 1;
        }
        // Last part
        {
            std::string part = plain.substr(start);
            auto first = part.find_first_not_of(" \t\r\n");
            auto last = part.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) part = part.substr(first, last - first + 1);

            const char *code = map_store_lang_to_code(part);
            if (code) codes.emplace_back(code);
        }

        return codes;
    } catch (...) {
        return {};
    }
}

// ============================================================
// Steam Store API search
// ============================================================

struct SteamSearchResult {
    uint32 appid = 0;
    std::string name;
    std::string tiny_image_url;
};

static std::string url_encode(const std::string &s)
{
    std::string encoded;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char buf[4] = {};
            snprintf(buf, sizeof(buf), "%%%02X", (int)c);
            encoded += buf;
        }
    }
    return encoded;
}

static std::vector<SteamSearchResult> steam_store_search(const std::string &term)
{
    std::vector<SteamSearchResult> results;

    std::string url = "https://store.steampowered.com/api/storesearch?term="
        + url_encode(term) + "&cc=us&l=en-us";

    std::string resp = http_get(url, 10L);
    if (resp.empty()) return results;

    try {
        auto json = nlohmann::json::parse(resp);
        auto items = json["items"];
        if (!items.is_array()) return results;

        for (const auto &item : items) {
            SteamSearchResult r;
            r.appid = item.value("id", 0);
            r.name = item.value("name", std::string{});
            r.tiny_image_url = item.value("tiny_image", std::string{});

            // The API returns type as a string (e.g. "app"), not an integer.
            std::string type = item.value("type", std::string{});
            if (type != "app") continue;

            if (r.appid && !r.name.empty()) {
                results.push_back(std::move(r));
            }
        }
    } catch (...) {}

    // Fallback: If no results found and term contains spaces, try searching without spaces
    if (results.empty() && term.find(' ') != std::string::npos) {
        std::string unspaced = term;
        unspaced.erase(std::remove(unspaced.begin(), unspaced.end(), ' '), unspaced.end());
        if (!unspaced.empty()) {
            return steam_store_search(unspaced);
        }
    }

    return results;
}


// ============================================================
// Derive search name from executable path
// ============================================================

static std::string derive_search_name()
{
    std::string exe_path = get_full_exe_path();

    size_t sep = exe_path.rfind('\\');
    if (sep == std::string::npos) sep = exe_path.rfind('/');
    if (sep == std::string::npos) return {};
    std::string fname = exe_path.substr(sep + 1);

    size_t dot = fname.rfind('.');
    if (dot != std::string::npos) fname = fname.substr(0, dot);

    static const char *suffixes[] = {
        "-Win64-Shipping",
        "-Shipping",
        "-DebugGame",
        "-UE4Editor",
        "-UE5Editor",
        "-Game",
        "-Client",
        "-Server",
        "-Linux",
        "-LNI",
    };

    for (const char *suf : suffixes) {
        size_t pos = fname.rfind(suf);
        if (pos != std::string::npos && pos + strlen(suf) == fname.size()) {
            fname = fname.substr(0, pos);
            break;
        }
    }

    // Insert spaces before uppercase letters (PascalCase -> words)
    std::string spaced;
    for (size_t i = 0; i < fname.size(); ++i) {
        if (i > 0 && isupper(fname[i]) && !isupper(fname[i-1]) && !isdigit(fname[i])) {
            if (islower(fname[i-1]) || isdigit(fname[i-1])) {
                spaced += ' ';
            }
        }
        spaced += fname[i];
    }
    fname = std::move(spaced);

    if (fname.empty()) {
        fname = exe_path.substr(sep + 1);
        dot = fname.rfind('.');
        if (dot != std::string::npos) fname = fname.substr(0, dot);
    }

    return fname;
}


// ============================================================
// GDI+ helper: decode JPEG data to HBITMAP
// Requires linking gdiplus.lib (MSVC) or -lgdiplus (MinGW)
// ============================================================

static HBITMAP decode_jpeg_data(const std::vector<char> &data)
{
    if (data.empty()) return nullptr;

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hg) return nullptr;

    void *ptr = GlobalLock(hg);
    if (ptr) {
        memcpy(ptr, data.data(), data.size());
        GlobalUnlock(hg);
    } else {
        GlobalFree(hg);
        return nullptr;
    }

    IStream *stream = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK) {
        GlobalFree(hg);
        return nullptr;
    }

    Gdiplus::Bitmap gdi_bitmap(stream);
    stream->Release();

    if (gdi_bitmap.GetLastStatus() != Gdiplus::Ok) return nullptr;

    HBITMAP hbm = nullptr;
    gdi_bitmap.GetHBITMAP(Gdiplus::Color(0, 0, 0), &hbm);
    return hbm;
}


// ============================================================
// Win32 AppID Search Dialog
// ============================================================

struct ThumbnailJob {
    uint32 appid;
    std::string url;
    HWND hwnd;
};

struct ThumbnailData {
    uint32 appid;
    std::vector<char> data;
};

static DWORD WINAPI ThumbnailDownloadProc(LPVOID param)
{
    auto *job = (ThumbnailJob*)param;

    auto *td = new ThumbnailData();
    td->appid = job->appid;

    CurlMem mem = http_get_binary(job->url, 15L);
    if (!mem.data.empty()) {
        td->data.assign(mem.data.begin(), mem.data.end());
    }

    PostMessageA(job->hwnd, WM_APP, (WPARAM)td, 0);
    delete job;
    return 0;
}

struct AppIDSearchState {
    std::vector<SteamSearchResult> results;
    HWND hwnd_list{};
    HWND hwnd_search_edit{};
    HWND hwnd_search_btn{};  // stored so we can disable it during search
    HWND hwnd_manual_edit{};
    HWND hwnd_select_btn{};
    HWND hwnd_status{};
    std::map<uint32, HBITMAP> thumbnails;
    int pending_thumbnails{0};
    uint32 result_appid{0};
    bool closing{false};
    bool is_searching{false}; // true while a background search thread is running
    ULONG_PTR gdiplus_token{};
};

// ============================================================
// Async search thread
// ============================================================

struct SearchJob {
    std::string term;
    HWND hwnd;
};

static DWORD WINAPI SearchThreadProc(LPVOID param)
{
    auto *job = (SearchJob*)param;
    auto *results = new std::vector<SteamSearchResult>(steam_store_search(job->term));
    PostMessageA(job->hwnd, WM_APP + 1, (WPARAM)results, 0);
    delete job;
    return 0;
}

static void start_search_async(AppIDSearchState *state, HWND hwnd, const std::string &term)
{
    if (state->is_searching || term.empty()) return;
    state->is_searching = true;
    SendMessageA(state->hwnd_status, WM_SETTEXT, 0, (LPARAM)"Searching Steam...");
    EnableWindow(state->hwnd_select_btn, FALSE);
    EnableWindow(state->hwnd_search_btn, FALSE);

    auto *job = new SearchJob();
    job->term = term;
    job->hwnd = hwnd;
    HANDLE hThread = CreateThread(nullptr, 0, SearchThreadProc, job, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        delete job;
        state->is_searching = false;
        EnableWindow(state->hwnd_search_btn, TRUE);
        SendMessageA(state->hwnd_status, WM_SETTEXT, 0, (LPARAM)"Search failed. Try again.");
    }
}

static void cleanup_thumbnails(AppIDSearchState *state)
{
    for (auto &[appid, hbm] : state->thumbnails) {
        if (hbm) DeleteObject(hbm);
    }
    state->thumbnails.clear();
}

static uint32 get_selected_appid(HWND hwnd_list)
{
    LRESULT sel = SendMessageA(hwnd_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return 0;
    LRESULT data = SendMessageA(hwnd_list, LB_GETITEMDATA, sel, 0);
    if (data == LB_ERR) return 0;
    return (uint32)data;
}

static void populate_list(AppIDSearchState *state, HWND hwnd)
{
    SendMessageA(state->hwnd_list, LB_RESETCONTENT, 0, 0);
    cleanup_thumbnails(state);

    for (const auto &r : state->results) {
        LRESULT idx = SendMessageA(state->hwnd_list, LB_ADDSTRING, 0, (LPARAM)r.name.c_str());
        if (idx != LB_ERR) {
            SendMessageA(state->hwnd_list, LB_SETITEMDATA, idx, (LPARAM)r.appid);
        }
    }

    if (!state->results.empty()) {
        SendMessageA(state->hwnd_list, LB_SETCURSEL, 0, 0);
    }

    state->pending_thumbnails = 0;
    for (const auto &r : state->results) {
        if (r.tiny_image_url.empty()) continue;
        state->pending_thumbnails++;
        auto *job = new ThumbnailJob();
        job->appid = r.appid;
        job->url = r.tiny_image_url;
        job->hwnd = hwnd;
        HANDLE hThread = CreateThread(nullptr, 0, ThumbnailDownloadProc, job, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            delete job;
            state->pending_thumbnails--;
        }
    }

    if (state->results.empty()) {
        SendMessageA(state->hwnd_status, WM_SETTEXT, 0, (LPARAM)"No results found. Try a different search term.");
        EnableWindow(state->hwnd_select_btn, FALSE);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Found %zu result(s). Double-click or press Select.", state->results.size());
        SendMessageA(state->hwnd_status, WM_SETTEXT, 0, (LPARAM)buf);
    }
}

static std::string get_dll_filepath()
{
    char buf[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&get_dll_filepath, &hMod);
    if (hMod && GetModuleFileNameA(hMod, buf, sizeof(buf))) {
        return buf;
    }
    return Local_Storage::get_program_path();
}

// Subclass proc for search edit control — ENTER key triggers search
static LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CHAR && wParam == VK_RETURN) {
        // Find parent dialog and send Search button command
        HWND hParent = GetParent(hwnd);
        if (hParent) SendMessageA(hParent, WM_COMMAND, 100, 0);
        return 0; // swallow the ding
    }
    return CallWindowProcA((WNDPROC)GetPropA(hwnd, "ORIG_WNDPROC"), hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK AppIDDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto *state = (AppIDSearchState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        auto *cs = (CREATESTRUCT*)lParam;
        auto *s = (AppIDSearchState*)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        state = s;

        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&state->gdiplus_token, &gsi, nullptr);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Row 1: "Search Steam:" label + edit + Search button
        HWND hwnd_lbl1 = CreateWindowExA(0, "STATIC", "Search Steam:",
            WS_CHILD | WS_VISIBLE, 12, 14, 84, 18,
            hwnd, nullptr, nullptr, nullptr);
        SendMessageA(hwnd_lbl1, WM_SETFONT, (WPARAM)hFont, TRUE);

        state->hwnd_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            100, 10, 356, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_search_edit, WM_SETFONT, (WPARAM)hFont, TRUE);

        state->hwnd_search_btn = CreateWindowExA(0, "BUTTON", "Search",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            462, 10, 80, 24, hwnd, (HMENU)100, nullptr, nullptr);
        SendMessageA(state->hwnd_search_btn, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Status bar
        state->hwnd_status = CreateWindowExA(0, "STATIC",
            "Enter a game name and press Search, or wait for the auto-detected search.",
            WS_CHILD | WS_VISIBLE, 12, 40, 530, 18,
            hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_status, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Listbox (owner-drawn with thumbnails)
        state->hwnd_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
            LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOTIFY,
            12, 62, 530, 216, hwnd, (HMENU)200, nullptr, nullptr);
        SendMessageA(state->hwnd_list, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Bottom bar: "Or enter AppID manually:" + edit + Select + Cancel
        HWND hwnd_lbl2 = CreateWindowExA(0, "STATIC", "Or enter AppID manually:",
            WS_CHILD | WS_VISIBLE, 12, 290, 150, 18,
            hwnd, nullptr, nullptr, nullptr);
        SendMessageA(hwnd_lbl2, WM_SETFONT, (WPARAM)hFont, TRUE);

        state->hwnd_manual_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
            166, 286, 90, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_manual_edit, WM_SETFONT, (WPARAM)hFont, TRUE);

        state->hwnd_select_btn = CreateWindowExA(0, "BUTTON", "Select",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            262, 286, 80, 26, hwnd, (HMENU)101, nullptr, nullptr);
        SendMessageA(state->hwnd_select_btn, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hwnd_cancel = CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            348, 286, 80, 26, hwnd, (HMENU)102, nullptr, nullptr);
        SendMessageA(hwnd_cancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Subclass search edit so ENTER triggers search
        SetPropA(state->hwnd_search_edit, "ORIG_WNDPROC",
            (HANDLE)SetWindowLongPtrA(state->hwnd_search_edit, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclassProc));

        // Pre-populate if results exist (shouldn't happen now that search is async)
        if (!state->results.empty()) {
            populate_list(state, hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        state->closing = true;
        state->result_appid = 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (state) {
            cleanup_thumbnails(state);
            Gdiplus::GdiplusShutdown(state->gdiplus_token);
        }
        // Do NOT call PostQuitMessage here — it would post WM_QUIT to the thread
        // queue and break the game's own message loop after the dialog closes.
        return 0;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (id == 100) { // Search
            char buf[256] = {};
            GetWindowTextA(state->hwnd_search_edit, buf, sizeof(buf));
            std::string term = buf;
            auto start = term.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) term = term.substr(start);
            auto end = term.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) term = term.substr(0, end + 1);
            if (term.empty()) break;

            start_search_async(state, hwnd, term);
            return 0;
        }

        if (id == 101) { // Select
            uint32 appid = get_selected_appid(state->hwnd_list);
            if (!appid) {
                char buf[32] = {};
                GetWindowTextA(state->hwnd_manual_edit, buf, sizeof(buf));
                if (buf[0]) {
                    try { appid = (uint32)std::stoul(buf); } catch (...) {}
                }
            }

            if (appid) {
                state->closing = true;
                state->result_appid = appid;
                DestroyWindow(hwnd);
            } else {
                SendMessageA(state->hwnd_status, WM_SETTEXT, 0,
                    (LPARAM)"Select a game from the list or enter an AppID manually.");
                MessageBeep(MB_ICONWARNING);
            }
            return 0;
        }

        if (id == 102) { // Cancel
            state->closing = true;
            state->result_appid = 0;
            DestroyWindow(hwnd);
            return 0;
        }

        if (id == 200 && code == LBN_DBLCLK) { // Listbox double-click
            uint32 appid = get_selected_appid(state->hwnd_list);
            if (appid) {
                state->closing = true;
                state->result_appid = appid;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        break;
    }

    case WM_APP: {
        auto *td = (ThumbnailData*)wParam;
        if (td) {
            if (!td->data.empty() && state && !state->closing) {
                HBITMAP hbm = decode_jpeg_data(td->data);
                if (hbm) {
                    state->thumbnails[td->appid] = hbm;
                    InvalidateRect(state->hwnd_list, nullptr, TRUE);
                }
            }
            delete td;
        }
        return 0;
    }

    case WM_APP + 1: { // Async search results
        auto *results = (std::vector<SteamSearchResult>*)wParam;
        if (state) {
            state->is_searching = false;
            EnableWindow(state->hwnd_search_btn, TRUE);
            if (results && !state->closing) {
                state->results = std::move(*results);
                populate_list(state, hwnd);
            } else if (!state->closing) {
                SendMessageA(state->hwnd_status, WM_SETTEXT, 0,
                    (LPARAM)"Search failed. Check your connection and try again.");
                EnableWindow(state->hwnd_select_btn, state->results.empty() ? FALSE : TRUE);
            }
        }
        delete results;
        return 0;
    }

    case WM_MEASUREITEM: {
        auto *mis = (MEASUREITEMSTRUCT*)lParam;
        if (mis->CtlType == ODT_LISTBOX) {
            mis->itemHeight = 52;
            return TRUE;
        }
        return FALSE;
    }

    case WM_DRAWITEM: {
        auto *dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlType != ODT_LISTBOX || !state) return FALSE;
        // itemID == -1 means the listbox is empty and Windows is asking us to
        // draw a focus rect on a phantom item — nothing to render.
        if (dis->itemID == (UINT)-1) return TRUE;

        uint32 appid = (uint32)dis->itemData;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;

        // Background
        if (dis->itemState & ODS_SELECTED) {
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_HIGHLIGHT));
            SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        SetBkMode(hdc, TRANSPARENT);

        // Thumbnail area: 120x46, 4px from left, centered vertically
        const int THUMB_W = 120, THUMB_H = 46;
        RECT rcImg = rc;
        rcImg.left  += 4;
        rcImg.top   += (52 - THUMB_H) / 2;  // center in 52px row
        rcImg.bottom = rcImg.top + THUMB_H;
        rcImg.right  = rcImg.left + THUMB_W;

        auto it = state->thumbnails.find(appid);
        if (it != state->thumbnails.end() && it->second) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            if (hdcMem) {
                HGDIOBJ hOld = SelectObject(hdcMem, it->second);
                // Query actual bitmap dimensions instead of assuming 231x87
                BITMAP bm = {};
                GetObject(it->second, sizeof(bm), &bm);
                int src_w = bm.bmWidth  > 0 ? bm.bmWidth  : 231;
                int src_h = bm.bmHeight > 0 ? bm.bmHeight : 87;
                SetStretchBltMode(hdc, HALFTONE);
                StretchBlt(hdc, rcImg.left, rcImg.top, THUMB_W, THUMB_H,
                    hdcMem, 0, 0, src_w, src_h, SRCCOPY);
                SelectObject(hdcMem, hOld);
                DeleteDC(hdcMem);
            }
        } else {
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rcImg.left, rcImg.top, rcImg.right, rcImg.bottom);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
        }

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        // Text starts after thumbnail + 8px gap
        const int TEXT_LEFT = rcImg.right + 8;

        // Game name — upper half of the row
        char name_buf[256] = {};
        SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)name_buf);
        RECT rcName = { TEXT_LEFT, rc.top + 6, rc.right - 6, rc.top + 28 };
        DrawTextA(hdc, name_buf, -1, &rcName,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        // AppID — lower half, dimmed
        char appid_buf[32];
        snprintf(appid_buf, sizeof(appid_buf), "AppID: %u", appid);
        SetTextColor(hdc, dis->itemState & ODS_SELECTED ?
            GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_GRAYTEXT));
        RECT rcID = { TEXT_LEFT, rc.top + 28, rc.right - 6, rc.bottom - 4 };
        DrawTextA(hdc, appid_buf, -1, &rcID,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, hOldFont);
        return TRUE;
    }

    case WM_DELETEITEM:
        return TRUE;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}


// ============================================================
// Run the AppID search dialog; returns 0 if cancelled
// ============================================================

static uint32 run_appid_dialog(const std::string &auto_search_name)
{
    AppIDSearchState state;

    const char *CLASS_NAME = "GoldbergAppIDSearch";

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = AppIDDlgProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    // Use GetClassInfoExA instead of a static bool so registration survives
    // DLL reloads and handles failure correctly.
    WNDCLASSEXA existing = { sizeof(WNDCLASSEXA) };
    if (!GetClassInfoExA(GetModuleHandleA(nullptr), CLASS_NAME, &existing)) {
        if (!RegisterClassExA(&wc)) return 0;
    }

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rcWin = { 0, 0, 554, 330 };
    AdjustWindowRectEx(&rcWin, dwStyle, FALSE, 0);
    int win_w = rcWin.right - rcWin.left;
    int win_h = rcWin.bottom - rcWin.top;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_w - win_w) / 2;
    int y = (screen_h - win_h) / 2;

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "Goldberg Emulator - AppID Selection",
        dwStyle,
        x, y, win_w, win_h,
        nullptr, nullptr, GetModuleHandleA(nullptr), &state
    );

    if (!hwnd) {
        cleanup_thumbnails(&state);
        return 0;
    }

    SendMessageA(state.hwnd_search_edit, WM_SETTEXT, 0, (LPARAM)auto_search_name.c_str());

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Kick off initial search in background — window is already visible so
    // the user sees it immediately instead of stalling for up to 10 seconds.
    if (!auto_search_name.empty()) {
        start_search_async(&state, hwnd, auto_search_name);
    }

    // Modal message loop — use PeekMessage so we don't block on GetMessage
    // after DestroyWindow (which would require PostQuitMessage, poisoning the
    // game's own message queue).
    MSG msg = {};
    while (IsWindow(hwnd)) {
        if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else {
            Sleep(1);
        }
    }

    // Drain remaining WM_APP / WM_APP+1 messages to free memory
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP) {
            delete (ThumbnailData*)msg.wParam;
        } else if (msg.message == WM_APP + 1) {
            delete (std::vector<SteamSearchResult>*)msg.wParam;
        }
    }

    return state.result_appid;
}


// ============================================================
// Fetch SteamDB RSS -> latest build_id
// ============================================================

static uint32 fetch_latest_build(uint32 appid)
{
    std::string url = "https://steamdb.info/api/PatchnotesRSS/?appid=" + std::to_string(appid);
    std::string resp = http_get(url, 10L);
    if (resp.empty()) return 0;

    // Parse <guid>build#NUMBER</guid>
    std::string tag = "build#";
    auto pos = resp.find(tag);
    if (pos == std::string::npos) return 0;

    pos += tag.size();
    auto end = resp.find_first_not_of("0123456789", pos);
    if (end == std::string::npos) return 0;

    try {
        return static_cast<uint32>(std::stoul(resp.substr(pos, end - pos)));
    } catch (...) {
        return 0;
    }
}


// ============================================================
// Fetch GetSchemaForGame for one language
// Returns empty JSON if fails
// ============================================================

static nlohmann::json fetch_schema_lang(const std::string &api_key, uint32 appid, const std::string &lang)
{
    std::string url = "https://api.steampowered.com/ISteamUserStats/GetSchemaForGame/v2/?key="
        + api_key + "&appid=" + std::to_string(appid) + "&l=" + lang;

    std::string raw = http_get(url, 30L);
    if (raw.empty()) return {};

    try {
        return nlohmann::json::parse(raw);
    } catch (...) {
        return {};
    }
}


// ============================================================
// MAIN: run_first_time_setup()
// ============================================================

bool Steam_User_Stats::run_first_time_setup()
{
    uint32 appid = settings->get_local_game_id().AppID();
    std::string search_name = derive_search_name();
    if (!appid) {
        appid = run_appid_dialog(search_name);
        if (!appid) return false;

        settings->set_game_id(CGameID(appid));
        local_storage->setAppId(appid);
    }

    std::string game_name = search_name.empty() ? "Unknown Game" : search_name;
    std::string settings_path = Local_Storage::get_game_settings_path();
    std::string dll_path = get_dll_filepath();

    // --- Open console ---
    console_open("Goldberg Emulator - Auto Setup");
    printf("=== Goldberg Emulator - Auto Setup ===\n\n");
    printf("Game:     \033[1m%s\033[0m\n", game_name.c_str());
    printf("AppID:    %u\n\n", appid);
    printf("DLL Path: %s\n\n", dll_path.c_str());

    // --- Prompt to start ---
    printf("Will generate steam_settings files for this game.\n");
    printf("Press ENTER to start");
    fflush(stdout);
    console_wait_enter();
    printf("\n\n");

    // --- Check API key ---
    std::string api_key = settings->steam_api_key;
    if (api_key.empty()) {
        printf("Enter your Steam Web API key\n");
        printf("(get one free at \033[4mhttps://steamcommunity.com/dev/apikey\033[0m):\n> ");
        fflush(stdout);

        std::getline(std::cin, api_key);
        // Trim whitespace
        auto trim_start = api_key.find_first_not_of(" \t\r\n");
        if (trim_start != std::string::npos) api_key = api_key.substr(trim_start);
        auto trim_end = api_key.find_last_not_of(" \t\r\n");
        if (trim_end != std::string::npos) api_key = api_key.substr(0, trim_end + 1);

        if (api_key.empty()) {
            print_fail("No API key provided, aborting");
            printf("\nPress ENTER to continue...\n");
            console_wait_enter();
            console_close();
            return false;
        }
    }

    // ============================================================
    // STEP 1: Fetch latest build from SteamDB
    // ============================================================
    printf("Step 1/5: Fetching latest build info...\n");
    fflush(stdout);

    uint32 latest_build = fetch_latest_build(appid);
    if (latest_build) {
        print_ok("Latest build: %u", latest_build);
    } else {
        print_info("Could not fetch build info (SteamDB may be unreachable)");
    }

    // ============================================================
    // STEP 2: Fetch English schema
    // ============================================================
    printf("\nStep 2/5: Fetching achievement schema (English)...\n");
    fflush(stdout);

    nlohmann::json schema_en = fetch_schema_lang(api_key, appid, "english");
    if (schema_en.is_null() || schema_en["game"].is_null() ||
        schema_en["game"]["availableGameStats"].is_null())
    {
        print_fail("Failed to fetch schema - check your API key and internet connection");
        printf("\nPress ENTER to continue...\n");
        console_wait_enter();
        console_close();
        return false;
    }

    auto &game_en = schema_en["game"];
    auto &game_name_api = game_en["gameName"];
    std::string app_name = game_name_api.is_string() ? game_name_api.get<std::string>() : game_name;

    auto achs_en = game_en["availableGameStats"]["achievements"];
    auto stats_en = game_en["availableGameStats"]["stats"];

    if (achs_en.is_null() || !achs_en.is_array() || achs_en.empty()) {
        print_info("No achievements found for this appid (app has no achievements or is not a game)");
    } else {
        print_ok("Found %zu achievements", achs_en.size());
    }

    if (stats_en.is_array() && !stats_en.empty()) {
        print_ok("Found %zu stat definitions", stats_en.size());
    }

    // ============================================================
    // STEP 3: Try Spanish translations (only if game has them)
    // ============================================================
    printf("\nStep 3/5: Fetching Spanish translations...\n");
    fflush(stdout);

    // Build a map: achievement name -> { displayName: {lang->str}, description: {lang->str} }
    std::map<std::string, nlohmann::json> ach_translations;

    // First populate English from schema_en
    if (achs_en.is_array()) {
        int idx = 0;
        for (const auto &ach : achs_en) {
            std::string name = ach.value("name", std::string{});
            if (name.empty()) continue;
            nlohmann::json entry;
            entry["displayName"]["english"] = ach.value("displayName", std::string{});
            entry["description"]["english"] = ach.value("description", std::string{});
            entry["hidden"] = ach.value("hidden", 0);
            entry["icon"] = ach.value("icon", std::string{});
            entry["icongray"] = ach.value("icongray", std::string{});
            // Store token and unlock_percentage for later
            entry["token_name"] = "NEW_ACHIEVEMENT_1_" + std::to_string(idx) + "_NAME";
            entry["token_desc"] = "NEW_ACHIEVEMENT_1_" + std::to_string(idx) + "_DESC";
            entry["unlock_percentage"] = ach.value("unlock_percentage", 0.0);
            ach_translations[name] = entry;
            ++idx;
        }
    }

    bool has_spanish = false;
    if (!ach_translations.empty()) {
        print_info("Fetching Spanish...");
        fflush(stdout);

        nlohmann::json schema_es = fetch_schema_lang(api_key, appid, "spanish");
        if (!schema_es.is_null()) {
            auto achs_es = schema_es["game"]["availableGameStats"]["achievements"];
            if (achs_es.is_array()) {
                int merged = 0;
                for (const auto &ach : achs_es) {
                    std::string name = ach.value("name", std::string{});
                    if (name.empty()) continue;

                    auto it = ach_translations.find(name);
                    if (it == ach_translations.end()) continue;

                    std::string dn = ach.value("displayName", std::string{});
                    std::string desc = ach.value("description", std::string{});

                    if (!dn.empty() && dn != it->second["displayName"]["english"].get<std::string>()) {
                        it->second["displayName"]["spanish"] = dn;
                        merged++;
                    }
                    if (!desc.empty() && desc != it->second["description"]["english"].get<std::string>()) {
                        it->second["description"]["spanish"] = desc;
                    }
                }

                if (merged > 0) {
                    print_ok("  Spanish: %d translations", merged);
                    has_spanish = true;
                } else {
                    print_info("  (Spanish not available - same as English)");
                }
            }
        } else {
            print_info("  (skipped - no response)");
        }
    }

    // ============================================================
    // STEP 4: Download icons
    // ============================================================
    printf("\nStep 4/5: Downloading achievement icons...\n");
    fflush(stdout);

    std::string icons_dir = settings_path + "ach_images" + PATH_SEPARATOR;
    std::string locked_dir = icons_dir + "locked" + PATH_SEPARATOR;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(icons_dir), ec);
    std::filesystem::create_directories(std::filesystem::u8path(locked_dir), ec);

    // First pass: count total downloadable icons
    int dl_total = 0;
    for (auto &[name, entry] : ach_translations) {
        if (!entry["icon"].get<std::string>().empty()) dl_total++;
        if (!entry["icongray"].get<std::string>().empty()) dl_total++;
    }

    int icons_downloaded = 0;
    int icons_total = dl_total;
    if (dl_total == 0) {
        print_info("No icons to download");
    } else {
        for (auto &[name, entry] : ach_translations) {
            auto dl_one = [&](const std::string &key, const std::string &subdir) {
                std::string url = entry[key].get<std::string>();
                if (url.empty()) return;

                std::string fname = extract_filename(url);
                if (fname.empty()) return;
                std::string full_path = subdir + fname;

                if (!file_size_(full_path)) {
                    auto img = http_get_binary(url);
                    if (!img.data.empty()) {
                        std::ofstream fout(std::filesystem::u8path(full_path), std::ios::binary);
                        fout.write(img.data.data(), img.data.size());
                    }
                }

                icons_downloaded++;
                int pct = icons_downloaded * 100 / icons_total;
                int bar_w = 40;
                int filled = icons_downloaded * bar_w / icons_total;
                printf("\r  [");
                for (int i = 0; i < bar_w; i++) putchar(i < filled ? '#' : '.');
                printf("] %d%% (%d/%d)", pct, icons_downloaded, icons_total);
                fflush(stdout);
            };

            dl_one("icon", icons_dir);
            dl_one("icongray", locked_dir);
        }
        printf("\n");
        print_ok("Downloaded %d/%d icons", icons_downloaded, icons_total);
    }

    // ============================================================
    // STEP 5: Write all files
    // ============================================================
    printf("\nStep 5/5: Writing files...\n");
    fflush(stdout);

    // Fetch supported languages from store API
    std::vector<std::string> lang_codes = fetch_supported_languages(appid);
    if (lang_codes.empty()) lang_codes = {"english"};
    int lang_count = (int)lang_codes.size();

    // Scan DLL for steam interface versions
    std::vector<std::string> interfaces = scan_interfaces(dll_path);

    // --- 5a: steam_appid.txt ---
    {
        std::string filepath = settings_path + "steam_appid.txt";
        std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
        if (fout) {
            fout << appid;
            print_ok("steam_appid.txt");
        } else {
            print_fail("steam_appid.txt (write error)");
        }
    }

    // --- 5b: supported_languages.txt ---
    {
        std::string filepath = settings_path + "supported_languages.txt";
        std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
        if (fout) {
            for (auto &code : lang_codes) {
                fout << code << "\n";
            }
            print_ok("supported_languages.txt (%d languages)", lang_count);
        } else {
            print_fail("supported_languages.txt (write error)");
        }
    }

    // --- 5c: steam_interfaces.txt ---
    {
        std::string filepath = settings_path + "steam_interfaces.txt";
        std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
        if (fout) {
            for (auto &iface : interfaces) {
                fout << iface << "\n";
            }
            print_ok("steam_interfaces.txt (%zu interfaces)", interfaces.size());
        } else {
            print_fail("steam_interfaces.txt (write error)");
        }
    }

    // --- 5d: achievements.json ---
    {
        nlohmann::json ach_array = nlohmann::json::array();
        for (const auto &[name, entry] : ach_translations) {
            nlohmann::json ach;
            ach["name"] = name;
            ach["hidden"] = entry["hidden"].get<int>();

            // Localized displayName + token (matching manual format)
            nlohmann::json dn_obj;
            dn_obj["english"] = entry["displayName"]["english"].get<std::string>();
            dn_obj["token"] = entry["token_name"].get<std::string>();
            ach["displayName"] = dn_obj;

            // Localized description + token
            nlohmann::json desc_obj;
            desc_obj["english"] = entry["description"]["english"].get<std::string>();
            desc_obj["token"] = entry["token_desc"].get<std::string>();
            ach["description"] = desc_obj;

            // Spanish translations if available
            if (entry["displayName"].contains("spanish")) {
                ach["displayName"]["spanish"] = entry["displayName"]["spanish"].get<std::string>();
            }
            if (entry["description"].contains("spanish")) {
                ach["description"]["spanish"] = entry["description"]["spanish"].get<std::string>();
            }

            // Icon paths (relative to steam_settings/)
            {
                std::string icon_fname = extract_filename(entry["icon"].get<std::string>());
                if (!icon_fname.empty()) {
                    ach["icon"] = "ach_images/" + icon_fname;
                }
            }
            // Locked icon -> ach_images/locked/
            {
                std::string icon_fname = extract_filename(entry["icongray"].get<std::string>());
                if (!icon_fname.empty()) {
                    ach["icon_gray"] = "ach_images/locked/" + icon_fname;
                }
            }

            // Unlock percentage from Steam API
            if (entry.contains("unlock_percentage")) {
                ach["unlock_percentage"] = entry["unlock_percentage"].get<double>();
            }

            ach_array.push_back(std::move(ach));
        }

        if (!ach_array.empty()) {
            std::string filepath = settings_path + std::string(achievements_user_file);
            std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
            if (fout) {
                fout << std::setw(2) << ach_array;
                print_ok("achievements.json (%zu achievements)", ach_array.size());
            } else {
                print_fail("achievements.json (write error)");
            }
        }
    }

    // --- 5e: stats.json ---
    {
        nlohmann::json stats_array = nlohmann::json::array();
        if (stats_en.is_array()) {
            for (const auto &s : stats_en) {
                nlohmann::json entry;
                entry["name"] = s.value("name", std::string{});
                if (entry["name"].get<std::string>().empty()) continue;

                // Determine type name
                int raw_type = s.value("type", 0);
                static const char *type_names[] = {"int", "float", "avgrate"};
                if (raw_type >= 1 && raw_type <= 3) {
                    entry["type"] = type_names[raw_type - 1];
                } else {
                    entry["type"] = "int";
                }

                // Default value
                auto def_val = s["defaultvalue"];
                if (def_val.is_number()) {
                    if (raw_type == 1) {
                        entry["default"] = std::to_string(def_val.get<int32>());
                    } else {
                        entry["default"] = std::to_string(def_val.get<float>());
                    }
                } else {
                    entry["default"] = "0";
                }

                stats_array.push_back(std::move(entry));
            }
        }

        if (!stats_array.empty()) {
            std::string filepath = settings_path + "stats.json";
            std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
            if (fout) {
                fout << std::setw(2) << stats_array;
                print_ok("stats.json (%zu stats)", stats_array.size());
            } else {
                print_fail("stats.json (write error)");
            }
        }
    }

    // --- 5f: branches.json ---
    {
        nlohmann::json branches_array = nlohmann::json::array();
        nlohmann::json public_branch;
        public_branch["name"] = "public";
        public_branch["description"] = "";
        public_branch["protected"] = false;
        public_branch["build_id"] = latest_build;
        public_branch["time_updated"] = (uint32)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        branches_array.push_back(std::move(public_branch));

        std::string filepath = settings_path + "branches.json";
        std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
        if (fout) {
            fout << std::setw(2) << branches_array;
            print_ok("branches.json");
        } else {
            print_fail("branches.json (write error)");
        }
    }

    // ============================================================
    // Done!
    // ============================================================
    printf("\n\033[1;32mAll files generated successfully!\033[0m\n");
    printf("Game: %s (AppID: %u)\n\n", app_name.c_str(), appid);

    if (latest_build) {
        printf("  Branch: public (build %u)\n", latest_build);
    }
    printf("  Achievements: %zu\n", ach_translations.size());
    printf("  Languages: %d\n", lang_count);
    printf("  Icons: %d/%d\n\n", icons_downloaded, icons_total);
    printf("\033[1;33mREADY TO PLAY\033[0m\n");
    printf("Press ENTER to start the game...\n");
    fflush(stdout);

    console_wait_enter();
    console_close();

    return true;
}