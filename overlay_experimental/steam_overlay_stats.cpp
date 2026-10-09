#include "overlay/steam_overlay_stats.h"
// translation
#include "overlay/steam_overlay_translations.h"
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif


// Returns true if the game window is the currently focused foreground window.
// On non-Windows platforms, always returns true (no focus-based pausing).
static bool is_game_focused()
{
#if defined(_WIN32)
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
#else
    return true;
#endif
}


Steam_Overlay_Stats::Steam_Overlay_Stats(class Settings* settings):
    settings(settings)
{
    show_fps = settings->overlay_always_show_fps;
    show_frametime = settings->overlay_always_show_frametime;
    show_playtime = settings->overlay_always_show_playtime;
}

bool Steam_Overlay_Stats::show_any_stats() const
{
    return show_fps || show_frametime || show_playtime;
}

void Steam_Overlay_Stats::update_frametime(const std::chrono::steady_clock::time_point &now)
{
    // ms-based sliding average, updated every frame (smooth, framerate-independent).
    // ns precision: FPS = 1000 / avg(frametime).
    const auto delta_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_frame_timepoint).count()
    );
    last_frame_timepoint = now;

    // Drop huge gaps (overlay init, ALT+TAB, hitch) so they don't poison the average.
    constexpr uint64_t kMaxDeltaNs = 500ULL * 1000ULL * 1000ULL; // 500ms
    if (delta_ns > kMaxDeltaNs || delta_ns == 0) {
        if (delta_ns > kMaxDeltaNs) {
            recent_deltas_ns.clear();
            running_sum_ns = 0;
        }
        return;
    }

    unsigned window_ms = settings->overlay_fps_avg_window;
    if (window_ms < 100) window_ms = 100;
    if (window_ms > 2000) window_ms = 2000;
    const uint64_t window_ns = static_cast<uint64_t>(window_ms) * 1000000ULL;

    recent_deltas_ns.push_back(delta_ns);
    running_sum_ns += delta_ns;
    // Pop oldest while we cover more than the window, but always keep >= 2 frames
    // so a single slow frame can't spike the display.
    while (recent_deltas_ns.size() > 2 && running_sum_ns > window_ns) {
        running_sum_ns -= recent_deltas_ns.front();
        recent_deltas_ns.pop_front();
    }
    // Safety cap: ~2000 frames max even if window is huge / fps is extreme.
    while (recent_deltas_ns.size() > 2000) {
        running_sum_ns -= recent_deltas_ns.front();
        recent_deltas_ns.pop_front();
    }

    const auto count = recent_deltas_ns.size();
    if (count > 0 && running_sum_ns > 0) {
        active_frametime_ms = static_cast<double>(running_sum_ns) / count / 1000000.0;
        if (active_frametime_ms > 0.0) {
            active_fps = 1000.0 / active_frametime_ms;
        }
    }
}

void Steam_Overlay_Stats::update_playtime(const std::chrono::steady_clock::time_point &now)
{
    const auto update_duration_sec = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_playtime
    ).count();
    if (update_duration_sec < 1) return;

    // Pause accumulation when the game window loses focus (ALT+TAB)
    if (!is_game_focused() && settings->pause_session_when_unfocused) {
        if (!playtime_pause_start.has_value()) {
            playtime_pause_start = now;
        }
        return; // freeze the display values
    }

    // Accumulate paused duration when focus returns
    if (playtime_pause_start.has_value()) {
        total_playtime_paused += now - *playtime_pause_start;
        playtime_pause_start.reset();
    }

    last_playtime = now;

    const auto effective_elapsed = (now - initial_time) - total_playtime_paused;
    const auto time_duration_sec = (unsigned long long)std::chrono::duration_cast<std::chrono::seconds>(
        effective_elapsed
    ).count();
    active_playtime_sec = static_cast<unsigned>(time_duration_sec % 60);

    const auto time_duration_min = time_duration_sec / 60;
    active_playtime_min = static_cast<unsigned>(time_duration_min % 60);

    const auto time_duration_hr = time_duration_min / 60;
    active_playtime_hr = static_cast<unsigned>(time_duration_hr % 24);
}

void Steam_Overlay_Stats::render_stats(int current_language)
{
    auto now = std::chrono::steady_clock::now();
    if (show_fps || show_frametime) {
        update_frametime(now);
    }
    if (show_playtime) {
        update_playtime(now);
    }

    ImGui::PushFont(font);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, settings->overlay_appearance.notification_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(
        settings->overlay_appearance.stats_background_r,
        settings->overlay_appearance.stats_background_g,
        settings->overlay_appearance.stats_background_b,
        settings->overlay_appearance.stats_background_a
    ));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(
        settings->overlay_appearance.stats_text_r,
        settings->overlay_appearance.stats_text_g,
        settings->overlay_appearance.stats_text_b,
        settings->overlay_appearance.stats_text_a
    ));

    std::stringstream stats_txt_buff{};
    if (show_fps) {
        stats_txt_buff << translationFpsDisplay[current_language]
                       << std::left << std::setw(3)
                       << static_cast<unsigned>(active_fps + 0.5)
                       << std::right << std::setw(0);
    }
    if (show_frametime) {
        if (stats_txt_buff.tellp() > 0) {
            stats_txt_buff << " | ";
        }
        stats_txt_buff << translationFrametimeDisplay[current_language]
                       << std::left << std::setw(4) << std::fixed << std::setprecision(2)
                       << active_frametime_ms
                       << std::defaultfloat << std::right << std::setw(0)
                       << translationFrametimeUnitDisplay[current_language];
    }
    if (show_playtime) {
        if (stats_txt_buff.tellp() > 0) {
            stats_txt_buff << " | ";
        }
        const auto org_fill = stats_txt_buff.fill();
        stats_txt_buff << translationPlaytimeDisplay[current_language]
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_hr << ':'
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_min << ':'
                       << std::setw(2) << std::setfill('0')
                       << active_playtime_sec
                       << std::setw(0) << std::setfill(org_fill);
    }
    const auto stats_txt = stats_txt_buff.str();

    // set FPS box width/height based on text size
    const auto msg_box = ImGui::CalcTextSize(
        stats_txt.c_str(),
        stats_txt.c_str() + stats_txt.size()
    );
    auto &global_style = ImGui::GetStyle();
    const float padding_all_sides = global_style.WindowPadding.y + global_style.WindowPadding.x;
    const auto stats_box = ImVec2(msg_box.x + padding_all_sides, msg_box.y + padding_all_sides);
    ImGui::SetNextWindowSize(stats_box);

    auto &io = ImGui::GetIO();
    const auto anchor_point_x = stats_box.x * settings->overlay_stats_pos_x;
    const auto anchor_point_y = stats_box.y * settings->overlay_stats_pos_y;
    ImGui::SetNextWindowPos({
        io.DisplaySize.x * settings->overlay_stats_pos_x - anchor_point_x,
        io.DisplaySize.y * settings->overlay_stats_pos_y - anchor_point_y
    });

    if (ImGui::Begin("wnd_fps_frametime", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::TextWrapped("%s", stats_txt.c_str());
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}
