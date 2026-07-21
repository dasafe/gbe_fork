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


// Map Steam store display names to language codes
static const std::map<std::string, std::string> STEAM_LANG_MAP = {
    {"english",                  "english"},
    {"french",                   "french"},
    {"italian",                  "italian"},
    {"german",                   "german"},
    {"spanish - spain",          "spanish"},
    {"spanish",                  "spanish"},
    {"japanese",                 "japanese"},
    {"korean",                   "korean"},
    {"portuguese - portugal",    "portuguese"},
    {"portuguese",               "portuguese"},
    {"russian",                  "russian"},
    {"simplified chinese",       "schinese"},
    {"traditional chinese",      "tchinese"},
    {"polish",                   "polish"},
    {"dutch",                    "dutch"},
    {"turkish",                  "turkish"},
    {"czech",                    "czech"},
    {"swedish",                  "swedish"},
    {"portuguese - brazil",      "brazilian"},
    {"thai",                     "thai"},
    {"vietnamese",               "vietnamese"},
    {"arabic",                   "arabic"},
    {"ukrainian",                "ukrainian"},
    {"spanish - latin america",  "latam"},
    {"latin american spanish",   "latam"},
};

static std::string strip_html(const std::string &s)
{
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

static std::vector<std::string> fetch_supported_languages(uint32 appid)
{
    std::string url = "https://store.steampowered.com/api/appdetails?appids=" + std::to_string(appid);
    std::string resp = http_get(url, 15L);
    if (resp.empty()) return {};

    try {
        auto j = nlohmann::json::parse(resp);
        auto &data = j[std::to_string(appid)]["data"];
        if (data.is_null() || !data["supported_languages"].is_string()) return {};

        std::string raw = data["supported_languages"].get<std::string>();
        raw = strip_html(raw);

        std::vector<std::string> codes;
        size_t start = 0, end;
        while ((end = raw.find(',', start)) != std::string::npos) {
            std::string token = raw.substr(start, end - start);
            // trim
            auto first = token.find_first_not_of(" \t\r\n");
            auto last = token.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) {
                token = token.substr(first, last - first + 1);
            }
            // lowercase for lookup
            std::string lower = common_helpers::to_lower(token);
            auto it = STEAM_LANG_MAP.find(lower);
            if (it != STEAM_LANG_MAP.end() && it->second != "english") {
                codes.push_back(it->second);
            }
            start = end + 1;
        }
        // last token
        if (start < raw.size()) {
            std::string token = raw.substr(start);
            auto first = token.find_first_not_of(" \t\r\n");
            auto last = token.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) {
                token = token.substr(first, last - first + 1);
            }
            std::string lower = common_helpers::to_lower(token);
            auto it = STEAM_LANG_MAP.find(lower);
            if (it != STEAM_LANG_MAP.end() && it->second != "english") {
                codes.push_back(it->second);
            }
        }

        return codes;
    } catch (...) {
        return {};
    }
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
    if (!appid) return false;

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
    // STEP 1: Fetch supported languages from store API
    // ============================================================
    printf("Step 1/6: Fetching supported languages...\n");
    fflush(stdout);

    std::vector<std::string> supported_langs = fetch_supported_languages(appid);
    if (supported_langs.empty()) {
        print_info("Could not fetch from store API, will check all common languages");
        // Fallback: use all non-english common languages
        for (const auto &[name, code] : STEAM_LANG_MAP) {
            if (code != "english" &&
                std::find(supported_langs.begin(), supported_langs.end(), code) == supported_langs.end()) {
                supported_langs.push_back(code);
            }
        }
    }
    // Deduplicate and remove english from the translation list
    {
        std::set<std::string> dedup;
        for (auto &l : supported_langs) dedup.insert(l);
        supported_langs.assign(dedup.begin(), dedup.end());
        auto it = std::find(supported_langs.begin(), supported_langs.end(), "english");
        if (it != supported_langs.end()) supported_langs.erase(it);
    }

    // Build the full list including english for supported_languages.txt
    std::vector<std::string> all_langs = supported_langs;
    {
        std::set<std::string> dedup;
        dedup.insert("english");
        for (auto &l : all_langs) dedup.insert(l);
        all_langs.assign(dedup.begin(), dedup.end());
    }

    print_ok("%zu supported languages (%zu + english)", all_langs.size(), supported_langs.size());

    // ============================================================
    // STEP 2: Fetch latest build from SteamDB
    // ============================================================
    printf("\nStep 2/6: Fetching latest build info...\n");
    fflush(stdout);

    uint32 latest_build = fetch_latest_build(appid);
    if (latest_build) {
        print_ok("Latest build: %u", latest_build);
    } else {
        print_info("Could not fetch build info (SteamDB may be unreachable)");
    }

    // ============================================================
    // STEP 3: Fetch English schema
    // ============================================================
    printf("\nStep 3/6: Fetching achievement schema (English)...\n");
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
    // STEP 4: Fetch translations (additional languages)
    // ============================================================
    printf("\nStep 4/6: Fetching translations...\n");
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

    if (!ach_translations.empty() && !supported_langs.empty()) {
        // Only fetch translations for the languages the game supports
        for (const auto &code : supported_langs) {
            // Look up a human-readable label for the progress line
            auto label_it = std::find_if(STEAM_LANG_MAP.begin(), STEAM_LANG_MAP.end(),
                [&](const auto &pair) { return pair.second == code; });
            const char *label = (label_it != STEAM_LANG_MAP.end()) ? label_it->first.c_str() : code.c_str();

            print_info("Fetching %s...", label);
            fflush(stdout);

            nlohmann::json schema_lang = fetch_schema_lang(api_key, appid, code);
            if (schema_lang.is_null()) {
                print_info("  (skipped — no response)");
                continue;
            }

            auto achs_lang = schema_lang["game"]["availableGameStats"]["achievements"];
            if (!achs_lang.is_array()) {
                continue;
            }

            int merged = 0;
            for (const auto &ach : achs_lang) {
                std::string name = ach.value("name", std::string{});
                if (name.empty()) continue;

                auto it = ach_translations.find(name);
                if (it == ach_translations.end()) continue;

                std::string dn = ach.value("displayName", std::string{});
                std::string desc = ach.value("description", std::string{});

                // Only add if different from English (i.e., game actually has translation)
                if (!dn.empty() && dn != it->second["displayName"]["english"].get<std::string>()) {
                    it->second["displayName"][code] = dn;
                    merged++;
                }
                if (!desc.empty() && desc != it->second["description"]["english"].get<std::string>()) {
                    it->second["description"][code] = desc;
                }
            }

            if (merged > 0) {
                print_ok("  %s: %d translations", label, merged);
            }
        }
    }

    // ============================================================
    // STEP 5: Download icons
    // ============================================================
    printf("\nStep 5/6: Downloading achievement icons...\n");
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
    // STEP 6: Write all files
    // ============================================================
    printf("\nStep 6/6: Writing files...\n");
    fflush(stdout);

    // --- 6a: steam_appid.txt ---
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

    // --- 6b: supported_languages.txt ---
    {
        std::string filepath = settings_path + "supported_languages.txt";
        std::ofstream fout(std::filesystem::u8path(filepath), std::ios::trunc);
        if (fout) {
            for (const auto &lang : all_langs) {
                fout << lang << "\n";
            }
            print_ok("supported_languages.txt (%zu languages)", all_langs.size());
        } else {
            print_fail("supported_languages.txt (write error)");
        }
    }

    // --- 6c: achievements.json ---
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

    // --- 6d: stats.json ---
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

    // --- 6e: branches.json ---
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
    printf("  Languages: %zu\n", all_langs.size());
    printf("  Icons: %d/%d\n\n", icons_downloaded, icons_total);
    printf("\033[1;33mREADY TO PLAY\033[0m\n");
    printf("Press ENTER to start the game...\n");
    fflush(stdout);

    console_wait_enter();
    console_close();

    return true;
}
