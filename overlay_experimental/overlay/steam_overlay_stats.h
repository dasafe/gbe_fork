#ifndef _STEAM_OVERLAY_STATS_H_
#define _STEAM_OVERLAY_STATS_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "dll/settings.h"
#include "InGameOverlay/ImGui/imgui.h"


class Steam_Overlay_Stats {
private:
    class Settings* settings{};
    
    // ms-based sliding average, ns precision, monotonic clock.
    // fps_averaging_window = time window in milliseconds (default 500).
    // FPS = 1000 / avg(frametime), like MangoHud fps_sampling_period /
    // CapFrameX PresentMon MsBetweenPresents.
    std::chrono::steady_clock::time_point last_frame_timepoint =
        std::chrono::steady_clock::now();
    std::deque<uint64_t> recent_deltas_ns{};
    uint64_t running_sum_ns = 0; // sum of recent_deltas_ns
    double active_frametime_ms = 0; // final averaged frametime
    double active_fps = 0; // final averaged FPS (= 1000 / frametime)


    std::chrono::steady_clock::time_point initial_time =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_playtime =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration total_playtime_paused{0};
    std::optional<std::chrono::steady_clock::time_point> playtime_pause_start;
    unsigned active_playtime_hr = 0;
    unsigned active_playtime_min = 0;
    unsigned active_playtime_sec = 0;

    void update_frametime(const std::chrono::steady_clock::time_point &now);
    void update_playtime(const std::chrono::steady_clock::time_point &now);

public:
    ImFont *font = nullptr;
    bool show_fps = false;
    bool show_frametime = false;
    bool show_playtime = false;

    Steam_Overlay_Stats(class Settings* settings);

    bool show_any_stats() const;
    void render_stats(int current_language);
};


#endif // _STEAM_OVERLAY_STATS_H_
