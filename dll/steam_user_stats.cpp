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

    // discard achievements without a "name"
    auto x = defined_achievements.begin();
    while (x != defined_achievements.end()) {
        if (!x->contains("name")) {
            x = defined_achievements.erase(x);
        } else {
            ++x;
        }
    }

    for (auto & it : defined_achievements) {
        try {
            std::string name = static_cast<std::string const&>(it["name"]);
            sorted_achievement_names.push_back(name);

            achievement_trigger trig{};
            try {
                trig.name = name;
                trig.value_operation = static_cast<std::string const&>(it["progress"]["value"]["operation"]);
                std::string stat_name = common_helpers::to_lower(static_cast<std::string const&>(it["progress"]["value"]["operand1"]));
                const auto &min_val_obj = it["progress"]["min_val"];
                std::string min_val = min_val_obj.is_number()
                    ? std::to_string(static_cast<double>(min_val_obj))
                    : static_cast<std::string const&>(min_val_obj);
                const auto &max_val_obj = it["progress"]["max_val"];
                std::string max_val = max_val_obj.is_number()
                    ? std::to_string(static_cast<double>(max_val_obj))
                    : static_cast<std::string const&>(max_val_obj);
                trig.min_value = min_val;
                trig.max_value = max_val;
                achievement_stat_trigger[stat_name].push_back(trig);
            } catch(...) {}
            
            // default initial values, will only be added if they don't exist already
            auto &user_ach = user_achievements[name]; // this will create a new json entry if the key didn't exist already
            user_ach.emplace("earned", false);
            user_ach.emplace("earned_time", static_cast<uint32>(0));
            // they will throw an exception for achievements with no progress
            try {
                uint32 progress_min = std::stoul(trig.min_value);
                uint32 progress_max = std::stoul(trig.max_value);
                // if the above lines didn't throw exception then add the values
                user_ach.emplace("progress", progress_min);
                user_ach.emplace("max_progress", progress_max);
            } catch(...) {}
        } catch(...) {}

        try {
            it["hidden"] = std::to_string(it["hidden"].get<int>());
        } catch(...) {}

        it["displayName"] = get_value_for_language(it, "displayName", settings->get_language());
        it["description"] = get_value_for_language(it, "description", settings->get_language());

        it["icon_handle"] = Settings::UNLOADED_IMAGE_HANDLE;
        it["icon_gray_handle"] = Settings::UNLOADED_IMAGE_HANDLE;
    }

    //TODO: not sure if the sort is actually case insensitive, ach names seem to be treated by steam as case insensitive so I assume they are.
    //need to find a game with achievements of different case names to confirm
    std::sort(sorted_achievement_names.begin(), sorted_achievement_names.end(), [](const std::string lhs, const std::string rhs){
        const auto result = std::mismatch(lhs.cbegin(), lhs.cend(), rhs.cbegin(), rhs.cend(), [](const unsigned char lhs, const unsigned char rhs){return std::tolower(lhs) == std::tolower(rhs);});
        return result.second != rhs.cend() && (result.first == lhs.cend() || std::tolower(*result.first) < std::tolower(*result.second));}
    );
    
    // Auto-fetch global achievement percentages from Steam API
    // Requires disable_lan_only=true in config to bypass LAN-only Winsock hook
    if (!settings->disable_networking && settings_disable_lan_only() && !defined_achievements.empty()) {
        fetch_thread = std::thread(&Steam_User_Stats::fetch_and_update_global_percentages, this);
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
