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
    printf("  \033[32m\u2713\033[0m "); // green checkmark
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void print_fail(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  \033[31m\u2717\033[0m "); // red cross
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
        + url_encode(term) + "&cc=US&l=en";

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

            int type = item.value("type", 0);
            if (type != 0 && type != 1) continue;

            if (r.appid && !r.name.empty()) {
                results.push_back(std::move(r));
            }
        }
    } catch (...) {}

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
    HWND hwnd_manual_edit{};
    HWND hwnd_select_btn{};
    HWND hwnd_status{};
    std::map<uint32, HBITMAP> thumbnails;
    int pending_thumbnails{0};
    uint32 result_appid{0};
    bool closing{false};
    ULONG_PTR gdiplus_token{};
};

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
        EnableWindow(state->hwnd_select_btn, TRUE);
    }
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

        // "Search Steam:"
        CreateWindowExA(0, "STATIC", "Search Steam:",
            WS_CHILD | WS_VISIBLE, 12, 12, 80, 22,
            hwnd, nullptr, nullptr, nullptr);

        // Search edit
        state->hwnd_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            96, 10, 330, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_search_edit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Search button
        CreateWindowExA(0, "BUTTON", "Search",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            432, 10, 70, 24, hwnd, (HMENU)100, nullptr, nullptr);

        // Status
        state->hwnd_status = CreateWindowExA(0, "STATIC", "Enter a game name to search, or press Search to use auto-detected name.",
            WS_CHILD | WS_VISIBLE, 12, 38, 490, 18,
            hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_status, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Listbox (owner-drawn with thumbnails)
        state->hwnd_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
            LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOTIFY,
            12, 60, 490, 250, hwnd, (HMENU)200, nullptr, nullptr);
        SendMessageA(state->hwnd_list, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Manual AppID
        CreateWindowExA(0, "STATIC", "Or enter AppID manually:",
            WS_CHILD | WS_VISIBLE, 12, 318, 140, 22,
            hwnd, nullptr, nullptr, nullptr);

        state->hwnd_manual_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
            156, 316, 100, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageA(state->hwnd_manual_edit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Select button
        state->hwnd_select_btn = CreateWindowExA(0, "BUTTON", "Select",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            270, 316, 80, 26, hwnd, (HMENU)101, nullptr, nullptr);
        SendMessageA(state->hwnd_select_btn, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Cancel button
        CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            360, 316, 80, 26, hwnd, (HMENU)102, nullptr, nullptr);

        // Pre-populate if results exist
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
        PostQuitMessage(0);
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

            SendMessageA(state->hwnd_status, WM_SETTEXT, 0, (LPARAM)"Searching Steam...");

            MSG dummy;
            while (PeekMessageA(&dummy, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&dummy);
                DispatchMessageA(&dummy);
            }

            state->results = steam_store_search(term);
            populate_list(state, hwnd);
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

        // Thumbnail (120x45)
        RECT rcImg = rc;
        rcImg.left += 4;
        rcImg.top += 3;
        rcImg.bottom = rcImg.top + 45;
        rcImg.right = rcImg.left + 120;

        auto it = state->thumbnails.find(appid);
        if (it != state->thumbnails.end() && it->second) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            if (hdcMem) {
                SelectObject(hdcMem, it->second);
                SetStretchBltMode(hdc, HALFTONE);
                StretchBlt(hdc, rcImg.left, rcImg.top,
                    120, 45, hdcMem, 0, 0, 231, 87, SRCCOPY);
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

        // Game name
        char name_buf[256] = {};
        SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)name_buf);

        RECT rcName = rc;
        rcName.left += 132;
        rcName.top += 5;
        rcName.right -= 5;
        DrawTextA(hdc, name_buf, -1, &rcName,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        // AppID (dimmed)
        char appid_buf[32];
        snprintf(appid_buf, sizeof(appid_buf), "AppID: %u", appid);
        RECT rcID = rc;
        rcID.left += 132;
        rcID.top += 26;
        rcID.right -= 5;
        SetTextColor(hdc, dis->itemState & ODS_SELECTED ?
            GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_GRAYTEXT));
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
    const char *CLASS_NAME = "GoldbergAppIDSearch";

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = AppIDDlgProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    static bool registered = false;
    if (!registered) {
        if (!RegisterClassExA(&wc)) return 0;
        registered = true;
    }

    // Run initial search
    AppIDSearchState state;
    if (!auto_search_name.empty()) {
        state.results = steam_store_search(auto_search_name);
    }

    int win_w = 520;
    int win_h = 370;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_w - win_w) / 2;
    int y = (screen_h - win_h) / 2;

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "Goldberg Emulator - AppID Selection",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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

    // Modal message loop
    MSG msg = {};
    while (IsWindow(hwnd) && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Drain remaining WM_APP (thumbnail) messages to free memory
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP) {
            delete (ThumbnailData*)msg.wParam;
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
    if (!appid) {
        std::string search_name = derive_search_name();
        appid = run_appid_dialog(search_name);
        if (!appid) return false;

        settings->set_game_id(CGameID(appid));
        local_storage->setAppId(appid);
    }

    const char *game_name = settings->get_local_name();
    if (!game_name || !*game_name) game_name = "Unknown Game";

    std::string settings_path = Local_Storage::get_game_settings_path();

    // --- Open console ---
    console_open("Goldberg Emulator - Auto Setup");
    printf("=== Goldberg Emulator - Auto Setup ===\n\n");
    printf("Game:  \033[1m%s\033[0m\n", game_name);
    printf("AppID: %u\n\n", appid);

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
        print_fail("Failed to fetch schema — check your API key and internet connection");
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
        for (const auto &ach : achs_en) {
            std::string name = ach.value("name", std::string{});
            if (name.empty()) continue;
            nlohmann::json entry;
            entry["displayName"]["english"] = ach.value("displayName", std::string{});
            entry["description"]["english"] = ach.value("description", std::string{});
            entry["hidden"] = ach.value("hidden", 0);
            entry["icon"] = ach.value("icon", std::string{});
            entry["icongray"] = ach.value("icongray", std::string{});
            ach_translations[name] = entry;
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
                    print_info("  (Spanish not available — same as English)");
                }
            }
        } else {
            print_info("  (skipped — no response)");
        }
    }

    // ============================================================
    // STEP 4: Download icons
    // ============================================================
    printf("\nStep 4/5: Downloading achievement icons...\n");
    fflush(stdout);

    std::string icons_dir = settings_path + "achievement_images" + PATH_SEPARATOR;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(icons_dir), ec);

    int icons_downloaded = 0;
    int icons_total = 0;
    for (auto &[name, entry] : ach_translations) {
        auto download_icon = [&](const std::string &key, const std::string &url) {
            if (url.empty()) return;
            icons_total++;
            std::string fname = extract_filename(url);
            if (fname.empty()) return;
            std::string full_path = icons_dir + fname;
            if (file_size_(full_path)) {
                icons_downloaded++;
                return; // already cached
            }
            auto img = http_get_binary(url);
            if (!img.data.empty()) {
                std::ofstream fout(std::filesystem::u8path(full_path), std::ios::binary);
                fout.write(img.data.data(), img.data.size());
                icons_downloaded++;
            }
        };

        download_icon("icon", entry["icon"].get<std::string>());
        download_icon("icongray", entry["icongray"].get<std::string>());
    }

    if (icons_total > 0) {
        print_ok("Downloaded %d/%d icons", icons_downloaded, icons_total);
    } else {
        print_info("No icons to download");
    }

    // ============================================================
    // STEP 5: Write all files
    // ============================================================
    printf("\nStep 5/5: Writing files...\n");
    fflush(stdout);

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
            fout << "english\n";
            if (has_spanish) fout << "spanish\n";
            print_ok("supported_languages.txt (english%s)", has_spanish ? " + spanish" : "");
        } else {
            print_fail("supported_languages.txt (write error)");
        }
    }

    // --- 5c: achievements.json ---
    {
        nlohmann::json ach_array = nlohmann::json::array();
        for (const auto &[name, entry] : ach_translations) {
            nlohmann::json ach;
            ach["name"] = name;
            ach["hidden"] = entry["hidden"].get<int>() ? "1" : "0";

            // Simplify displayName/description to simple string if only English
            auto &dn = entry["displayName"];
            auto &desc = entry["description"];
            if (dn.size() == 1 && dn.contains("english")) {
                ach["displayName"] = dn["english"].get<std::string>();
            } else {
                ach["displayName"] = dn;
            }
            if (desc.size() == 1 && desc.contains("english")) {
                ach["description"] = desc["english"].get<std::string>();
            } else {
                ach["description"] = desc;
            }

            // Icon paths (relative to steam_settings/)
            {
                std::string icon_fname = extract_filename(entry["icon"].get<std::string>());
                if (!icon_fname.empty()) {
                    ach["icon"] = "achievement_images" PATH_SEPARATOR + icon_fname;
                }
            }
            {
                std::string icon_fname = extract_filename(entry["icongray"].get<std::string>());
                if (!icon_fname.empty()) {
                    ach["icon_gray"] = "achievement_images" PATH_SEPARATOR + icon_fname;
                }
            }

            ach["icon_handle"] = Settings::UNLOADED_IMAGE_HANDLE;
            ach["icon_gray_handle"] = Settings::UNLOADED_IMAGE_HANDLE;

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

    // --- 5d: stats.json ---
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

    // --- 5e: branches.json ---
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
    printf("  Languages: %s\n", has_spanish ? "english, spanish" : "english");
    printf("  Icons: %d/%d\n\n", icons_downloaded, icons_total);
    printf("\033[1;33mREADY TO PLAY\033[0m\n");
    printf("Press ENTER to start the game...\n");
    fflush(stdout);

    console_wait_enter();
    console_close();

    return true;
}
