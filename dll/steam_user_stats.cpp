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
#include <chrono>
#include <random>


void Steam_User_Stats::steam_user_stats_network_low_level(void *object, Common_Message *msg)
{
    // PRINT_DEBUG_ENTRY();

    auto inst = (Steam_User_Stats *)object;
    inst->network_callback_low_level(msg);
}

void Steam_User_Stats::steam_user_stats_run_every_runcb(void *object)
{
    // PRINT_DEBUG_ENTRY();

    auto inst = (Steam_User_Stats *)object;
    inst->steam_run_callback();
}


Steam_User_Stats::Steam_User_Stats(Settings *settings, class Networking *network, Local_Storage *local_storage, class SteamCallResults *callback_results, class SteamCallBacks *callbacks, class RunEveryRunCB *run_every_runcb, Steam_Overlay* overlay):
    settings(settings),
    network(network),
    local_storage(local_storage),
    callback_results(callback_results),
    callbacks(callbacks),
    defined_achievements(nlohmann::json::object()),
    user_achievements(nlohmann::json::object()),
    run_every_runcb(run_every_runcb),
    overlay(overlay)
{
    load_achievements_db(); // steam_settings/achievements.json
    load_achievements(); // %appdata%/<emu saves folder>/<app id>/achievements.json
    process_achievement_definitions();
    
    // Auto-fetch global achievement percentages from Steam API
    // Requires disable_lan_only=true in config to bypass LAN-only Winsock hook
    if (!settings->disable_networking && settings_disable_lan_only() && !defined_achievements.empty()) {
        fetch_thread = std::thread(&Steam_User_Stats::fetch_and_update_global_percentages, this);
    }

    // Check for game updates via SteamDB RSS
    if (!settings->disable_networking && settings_disable_lan_only() && settings->check_for_game_updates) {
        update_check_thread = std::thread(&Steam_User_Stats::fetch_and_check_game_update, this);
    }

    // First-time setup: show console, generate steam_settings files
    // Trigger on file absence (not parsed content), so an empty achievements.json
    // (game with no achievements) doesn't re-run the wizard on every launch
    if (!settings->disable_networking && settings_disable_lan_only() &&
        settings->first_run_auto_setup &&
        !file_exists_(Local_Storage::get_game_settings_path() + achievements_user_file))
    {
        if (run_first_time_setup()) {
            // Reload from newly written files
            defined_achievements = nlohmann::json::object();
            user_achievements = nlohmann::json::object();
            sorted_achievement_names.clear();
            achievement_stat_trigger.clear();
            load_achievements_db();
            load_achievements();
            process_achievement_definitions();
        }
    }

    if (!settings->disable_sharing_stats_with_gameserver) {
        this->network->setCallback(CALLBACK_ID_GAMESERVER_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_stats, this);
    }
    if (settings->share_leaderboards_over_network) {
        this->network->setCallback(CALLBACK_ID_LEADERBOARDS_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_leaderboards, this);
    }
    this->network->setCallback(CALLBACK_ID_USER_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_stats, this);
    this->network->setCallback(CALLBACK_ID_USER_STATUS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_low_level, this);
    this->run_every_runcb->add(&Steam_User_Stats::steam_user_stats_run_every_runcb, this);
}

Steam_User_Stats::~Steam_User_Stats()
{
    if (!settings->disable_sharing_stats_with_gameserver) {
        this->network->rmCallback(CALLBACK_ID_GAMESERVER_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_stats, this);
    }
    if (settings->share_leaderboards_over_network) {
        this->network->rmCallback(CALLBACK_ID_LEADERBOARDS_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_leaderboards, this);
    }
    this->network->rmCallback(CALLBACK_ID_USER_STATS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_stats, this);
    this->network->rmCallback(CALLBACK_ID_USER_STATUS, settings->get_local_steam_id(), &Steam_User_Stats::steam_user_stats_network_low_level, this);
    this->run_every_runcb->remove(&Steam_User_Stats::steam_user_stats_run_every_runcb, this);

    if (fetch_thread.joinable()) {
        fetch_thread.join();
    }

    if (update_check_thread.joinable()) {
        update_check_thread.join();
    }
}


// Retrieves the number of players currently playing your game (online + offline)
// This call is asynchronous, with the result returned in NumberOfCurrentPlayers_t
STEAM_CALL_RESULT( NumberOfCurrentPlayers_t )
SteamAPICall_t Steam_User_Stats::GetNumberOfCurrentPlayers()
{
    PRINT_DEBUG_ENTRY();
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    std::random_device rd{};
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int32> distrib(117, 1017);
 
    NumberOfCurrentPlayers_t data{};
    data.m_bSuccess = 1;
    data.m_cPlayers = distrib(gen);
    auto ret = callback_results->addCallResult(data.k_iCallback, &data, sizeof(data));
    callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    return ret;
}



// --- old interface version

uint32 Steam_User_Stats::GetNumStats( CGameID nGameID )
{
    PRINT_DEBUG("old %llu", nGameID.ToUint64());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return 0;
    }
    return (uint32)settings->getStats().size();
}

const char *Steam_User_Stats::GetStatName( CGameID nGameID, uint32 iStat )
{
    PRINT_DEBUG("old %llu [%u]", nGameID.ToUint64(), iStat);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    auto &stats = settings->getStats();
    if (settings->get_local_game_id() != nGameID || iStat >= stats.size()) {
        return "";
    }
    
    return std::next(stats.begin(), iStat)->first.c_str();
}

ESteamUserStatType Steam_User_Stats::GetStatType( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s'", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (settings->get_local_game_id() != nGameID || !pchName) {
        return ESteamUserStatType::k_ESteamUserStatTypeINVALID;
    }
    
    std::string stat_name(common_helpers::to_lower(pchName));
    const auto &stats = settings->getStats();
    auto stat_info = stats.find(stat_name);
    if (stats.end() == stat_info) {
        return ESteamUserStatType::k_ESteamUserStatTypeINVALID;
    }

    switch (stat_info->second.type)
    {
    case StatInfo::STAT_TYPE_INT: return ESteamUserStatType::k_ESteamUserStatTypeINT;
    case StatInfo::STAT_TYPE_FLOAT: return ESteamUserStatType::k_ESteamUserStatTypeFLOAT;
    case StatInfo::STAT_TYPE_AVGRATE: return ESteamUserStatType::k_ESteamUserStatTypeAVGRATE;
    
    default: PRINT_DEBUG("[X] unhandled type %i", (int)stat_info->second.type); break;
    }
    
    return ESteamUserStatType::k_ESteamUserStatTypeINVALID;
}

uint32 Steam_User_Stats::GetNumAchievements( CGameID nGameID )
{
    PRINT_DEBUG("old %llu", nGameID.ToUint64());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return 0;
    }
    
    return GetNumAchievements();
}

const char *Steam_User_Stats::GetAchievementName( CGameID nGameID, uint32 iAchievement )
{
    PRINT_DEBUG("old %llu [%u]", nGameID.ToUint64(), iAchievement);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return "";
    }
    
    return GetAchievementName(iAchievement);
}

uint32 Steam_User_Stats::GetNumGroupAchievements( CGameID nGameID )
{
    PRINT_DEBUG("old %llu // TODO", nGameID.ToUint64());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return 0;
    }
    
    return 0;
}

const char *Steam_User_Stats::GetGroupAchievementName( CGameID nGameID, uint32 iAchievement )
{
    PRINT_DEBUG("old %llu [%u] // TODO", nGameID.ToUint64(), iAchievement);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return "";
    }
    
    return "";
}

bool Steam_User_Stats::RequestCurrentStats( CGameID nGameID )
{
    PRINT_DEBUG("old %llu", nGameID.ToUint64());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return RequestCurrentStats();
}

bool Steam_User_Stats::GetStat( CGameID nGameID, const char *pchName, int32 *pData )
{
    PRINT_DEBUG("old %llu '%s' %p", nGameID.ToUint64(), pchName, pData);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (pData) *pData = 0;
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return GetStat(pchName, pData);
}

bool Steam_User_Stats::GetStat( CGameID nGameID, const char *pchName, float *pData )
{
    PRINT_DEBUG("old %llu '%s' %p", nGameID.ToUint64(), pchName, pData);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (pData) *pData = 0;
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return GetStat(pchName, pData);
}

bool Steam_User_Stats::SetStat( CGameID nGameID, const char *pchName, int32 nData )
{
    PRINT_DEBUG("old %llu '%s' %i", nGameID.ToUint64(), pchName, nData);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return SetStat(pchName, nData);
}

bool Steam_User_Stats::SetStat( CGameID nGameID, const char *pchName, float fData )
{
    PRINT_DEBUG("old %llu '%s' %f", nGameID.ToUint64(), pchName, fData);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return SetStat(pchName, fData);
}

bool Steam_User_Stats::UpdateAvgRateStat( CGameID nGameID, const char *pchName, float flCountThisSession, double dSessionLength )
{
    PRINT_DEBUG("old %llu '%s' %f %f", nGameID.ToUint64(), pchName, flCountThisSession, dSessionLength);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return UpdateAvgRateStat(pchName, flCountThisSession, dSessionLength);
}

bool Steam_User_Stats::GetAchievement( CGameID nGameID, const char *pchName, bool *pbAchieved )
{
    PRINT_DEBUG("old %llu '%s' %p", nGameID.ToUint64(), pchName, pbAchieved);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (pbAchieved) *pbAchieved = false;
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return GetAchievement(pchName, pbAchieved);
}

bool Steam_User_Stats::GetGroupAchievement( CGameID nGameID, const char *pchName, bool *pbAchieved )
{
    PRINT_DEBUG("old %llu '%s' %p // TODO", nGameID.ToUint64(), pchName, pbAchieved);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    if (pbAchieved) *pbAchieved = false;
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return false;
}

bool Steam_User_Stats::SetAchievement( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s'", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID && settings->achievement_bypass) {
        return false;
    }
    
    return SetAchievement(pchName);
}

bool Steam_User_Stats::SetGroupAchievement( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s' // TODO", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return false;
}

bool Steam_User_Stats::StoreStats( CGameID nGameID )
{
    PRINT_DEBUG("old %llu", nGameID.ToUint64());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return StoreStats();
}

bool Steam_User_Stats::ClearAchievement( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s'", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return ClearAchievement(pchName);
}

bool Steam_User_Stats::ClearGroupAchievement( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s' // TODO", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return 0;
    }
    
    return false;
}

int Steam_User_Stats::GetAchievementIcon( CGameID nGameID, const char *pchName )
{
    PRINT_DEBUG("old %llu '%s'", nGameID.ToUint64(), pchName);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return Settings::INVALID_IMAGE_HANDLE;
    }
    
    return GetAchievementIcon(pchName);
}

const char *Steam_User_Stats::GetAchievementDisplayAttribute( CGameID nGameID, const char *pchName, const char *pchKey )
{
    PRINT_DEBUG("old %llu '%s' ['%s']", nGameID.ToUint64(), pchName, pchKey);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return "";
    }
    
    return GetAchievementDisplayAttribute(pchName, pchKey);
}

bool Steam_User_Stats::IndicateAchievementProgress( CGameID nGameID, const char *pchName, uint32 nCurProgress, uint32 nMaxProgress )
{
    PRINT_DEBUG("old %llu '%s' %u %u", nGameID.ToUint64(), pchName, nCurProgress, nMaxProgress);
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    if (settings->get_local_game_id() != nGameID) {
        return false;
    }
    
    return IndicateAchievementProgress(pchName, nCurProgress, nMaxProgress);
}


// --- steam callbacks

void Steam_User_Stats::steam_run_callback()
{
    send_updated_stats();
    load_achievements_icons();
    send_pending_user_stats_requests();
}



// --- networking callbacks
// only triggered when we have a message

// user connect/disconnect
void Steam_User_Stats::network_callback_low_level(Common_Message *msg)
{
    CSteamID steamid((uint64)msg->source_id());
    // this should never happen, but just in case
    if (steamid == settings->get_local_steam_id()) return;

    switch (msg->low_level().type())
    {
    case Low_Level::CONNECT:
        // nothing
    break;
    
    case Low_Level::DISCONNECT: {
        for (auto &board : cached_leaderboards) {
            board.remove_entries(steamid);
        }
        
        // TODO: need tests on real steam
        bool had_data = false;

        auto it_res_r = received_user_stats_data.find(steamid.ConvertToUint64());
        if (it_res_r != received_user_stats_data.end()) {
            had_data = true;
            it_res_r = received_user_stats_data.erase(it_res_r);
        }
        auto it_res_p = pending_user_stats_requests.find(steamid.ConvertToUint64());
        if (it_res_p != pending_user_stats_requests.end()) {
            trigger_user_stats_received(steamid, it_res_p->second.api_id);
            it_res_p = pending_user_stats_requests.erase(it_res_p);
        }
        else if (had_data) {
            UserStatsUnloaded_t data{};
            data.m_steamIDUser = steamid;
            callbacks->addCBResult(data.k_iCallback, &data, sizeof(data), 0.0);
        }

        // PRINT_DEBUG("removed user %llu", (uint64)steamid.ConvertToUint64());
    }
    break;
    
    default:
        PRINT_DEBUG("unknown type %i", (int)msg->low_level().type());
    break;
    }
}


// Write defined_achievements back to steam_settings/achievements.json
void Steam_User_Stats::save_achievements_db()
{
    std::string full_path = Local_Storage::get_game_settings_path() + achievements_user_file;
    std::ofstream inv_file(std::filesystem::u8path(full_path), std::ios::trunc | std::ios::out);
    if (inv_file) {
        inv_file << std::setw(2) << defined_achievements;
        PRINT_DEBUG("Saved global achievement percentages to '%s'", full_path.c_str());
    } else {
        PRINT_DEBUG("Couldn't open '%s' to save achievements", full_path.c_str());
    }
}


// Curl write callback to accumulate response data into a string
static size_t curl_write_global_percentages(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    ((std::string*)userp)->append((char*)contents, total);
    return total;
}


void Steam_User_Stats::fetch_and_update_global_percentages()
{
    uint32 appid = settings->get_local_game_id().AppID();
    if (!appid) return;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string url = "https://api.steampowered.com/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v2/?gameid="
        + std::to_string(appid) + "&_=" + std::to_string(now_ms);

    PRINT_DEBUG("Fetching global achievement percentages from: %s", url.c_str());

    CURL *curl = curl_easy_init();
    if (!curl) return;

    std::string response{};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_global_percentages);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoldbergEmulator/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        PRINT_DEBUG("Curl request failed: %s", curl_easy_strerror(res));
        return;
    }

    if (response.empty()) {
        PRINT_DEBUG("Empty response from Steam API");
        return;
    }

    // Parse JSON response
    nlohmann::json api_response;
    try {
        api_response = nlohmann::json::parse(response);
    } catch (const std::exception &e) {
        PRINT_DEBUG("Failed to parse API response: %s", e.what());
        return;
    }

    // Extract achievement percentages
    auto ach_pct = api_response["achievementpercentages"]["achievements"];
    if (!ach_pct.is_array() || ach_pct.empty()) {
        PRINT_DEBUG("No achievement percentages in API response");
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    unsigned matches = 0;
    for (const auto &entry : ach_pct) {
        try {
            std::string api_name = entry["name"];
            if (api_name.empty()) continue;

            float percent = 0.0f;
            const auto& percent_val = entry["percent"];
            if (percent_val.is_string()) {
                percent = std::stof(percent_val.get_ref<const std::string&>());
            } else if (percent_val.is_number()) {
                percent = percent_val.get<float>();
            } else {
                continue;
            }

            auto it = defined_achievements_find(api_name);
            if (it != defined_achievements.end()) {
                it.value()["unlock_percentage"] = percent;
                ++matches;
            }
        } catch (...) {
            continue;
        }
    }

    PRINT_DEBUG("Updated unlock_percentage for %u/%zu achievements", matches, ach_pct.size());
    save_achievements_db();
}


// Curl write callback for SteamDB RSS
static size_t curl_write_steamdb_rss(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    ((std::string*)userp)->append((char*)contents, total);
    return total;
}


void Steam_User_Stats::fetch_and_check_game_update()
{
    uint32 appid = settings->get_local_game_id().AppID();
    if (!appid) return;

    std::string url = "https://steamdb.info/api/PatchnotesRSS/?appid=" + std::to_string(appid);

    PRINT_DEBUG("Checking for game updates via SteamDB RSS: %s", url.c_str());

    CURL *curl = curl_easy_init();
    if (!curl) return;

    std::string response{};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_steamdb_rss);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GoldbergEmulator/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        PRINT_DEBUG("Curl request failed: %s", curl_easy_strerror(res));
        return;
    }

    if (response.empty()) {
        PRINT_DEBUG("Empty response from SteamDB RSS");
        return;
    }

    // Parse the latest build ID from the RSS feed
    // Format: <guid>build#<number></guid>
    std::string build_tag = "build#";
    auto build_pos = response.find(build_tag);
    if (build_pos == std::string::npos) {
        PRINT_DEBUG("Could not find build# in SteamDB RSS response");
        return;
    }

    auto build_start = build_pos + build_tag.size();
    auto build_end = response.find_first_not_of("0123456789", build_start);
    if (build_end == std::string::npos) {
        PRINT_DEBUG("Could not parse build number from SteamDB RSS");
        return;
    }

    uint32 latest_build = 0;
    try {
        latest_build = static_cast<uint32>(std::stoul(response.substr(build_start, build_end - build_start)));
    } catch (...) {
        PRINT_DEBUG("Failed to convert build number");
        return;
    }

    // Find the first <item> so we can parse item-level fields
    // (channel-level <description> appears before <item> and would be wrong)
    std::string version_str;
    std::string date_str;
    auto item_start = response.find("<item>");
    if (item_start != std::string::npos) {
        // Parse the <description> within the first <item>
        // Format: <description><![CDATA[Version 1.0.5 [r148188] (SteamDB Build 23559724)]]></description>
        auto desc_start = response.find("<description>", item_start);
        if (desc_start != std::string::npos) {
            auto desc_content_start = desc_start + 13; // length of "<description>"
            auto desc_end = response.find("</description>", desc_content_start);
            if (desc_end != std::string::npos) {
                version_str = response.substr(desc_content_start, desc_end - desc_content_start);
                // Remove CDATA if present
                auto cdata_start = version_str.find("<![CDATA[");
                if (cdata_start != std::string::npos) {
                    cdata_start += 9; // length of "<![CDATA["
                    auto cdata_end = version_str.find("]]>", cdata_start);
                    if (cdata_end != std::string::npos) {
                        version_str = version_str.substr(cdata_start, cdata_end - cdata_start);
                    }
                }
            }
        }

        // Parse the <pubDate> within the first <item>
        auto pubdate_start = response.find("<pubDate>", item_start);
        if (pubdate_start != std::string::npos) {
            auto pubdate_content_start = pubdate_start + 9; // length of "<pubDate>"
            auto pubdate_end = response.find("</pubDate>", pubdate_content_start);
            if (pubdate_end != std::string::npos) {
                date_str = response.substr(pubdate_content_start, pubdate_end - pubdate_content_start);
            }
        }
    }

    PRINT_DEBUG("SteamDB latest build: %u, version: '%s', date: '%s'", latest_build, version_str.c_str(), date_str.c_str());

    // Get the installed build ID from the active branch (public branch)
    uint32 installed_build = 0;
    for (const auto &branch : settings->branches) {
        if (branch.active) {
            installed_build = branch.build_id;
            break;
        }
    }

    if (installed_build == 0) {
        PRINT_DEBUG("Could not determine installed build from branches");
        return;
    }

    PRINT_DEBUG("Installed build: %u, Latest build: %u", installed_build, latest_build);

    // If latest build > installed build, there's an update available
    if (latest_build > installed_build) {
        PRINT_DEBUG("Game update available: %u -> %u", installed_build, latest_build);
        // Set all data fields first, then the flag last to avoid race with overlay render
        settings->pending_update_latest_build = latest_build;
        settings->pending_update_installed_build = installed_build;
        settings->pending_update_version = version_str;
        settings->pending_update_date = date_str;
        settings->pending_update_available = true;
    } else {
        PRINT_DEBUG("Game is up to date");
    }
}
