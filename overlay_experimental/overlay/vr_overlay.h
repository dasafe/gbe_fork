#ifndef __INCLUDED_GBE_VR_OVERLAY_H__
#define __INCLUDED_GBE_VR_OVERLAY_H__

// VR achievement overlay bridge for gbe_fork.
// Goal: show achievement toasts in-headset (wrist/head/chest) + full list
// in the SteamVR dashboard, instead of only the flat desktop mirror.
//
// Design constraints:
// - No static OpenVR dependency. openvr_api is loaded dynamically at runtime.
//   Builds and runs fine without any SteamVR runtime installed.
// - No haptics (per user request).
// - When VR is active, achievement toasts are HMD-exclusive (no desktop mirror).
// - Sound still plays through the existing desktop path (audible via SteamVR mirroring).

#include "dll/base.h"
#include "overlay/vr_openvr_abi.h"
#include <string>
#include <deque>
#include <chrono>
#include <mutex>

#ifdef EMU_OVERLAY

// Where to pin the momentary toast in-headset.
enum class VRToastAnchor : uint8_t {
    left_wrist = 0,
    right_wrist = 1,
    head = 2,
    chest = 3,          // "knee-chest": HMD-relative, glance-down preset
    dashboard_only = 4, // no scene toast, dashboard history only
};

// Mirrors [overlay::vr] in configs.overlay.ini
struct VROverlayConfig {
    bool enable_vr_overlay = true;
    VRToastAnchor anchor = VRToastAnchor::left_wrist;
    float width_m = 0.16f;          // 0.08 - 0.30
    float offset_x = 0.0f;          // meters, anchor-local nudge
    float offset_y = 0.04f;
    float offset_z = 0.0f;
    float tilt_deg = 45.0f;         // wrist watch-face tilt
    float duration_sec = -1.0f;     // <=0 reuses desktop Notification_Duration_Achievement
    bool suppress_desktop_achievements = true; // HMD-exclusive toasts in VR
    bool fallback_to_head = true;   // if wrist controller not tracked

    static VRToastAnchor anchor_from_string(const std::string &s);
    static std::string anchor_to_string(VRToastAnchor a);
};

// Lightweight toast payload (no GPU handles, safe to queue).
struct VRAchToast {
    std::string name{};
    std::string title{};
    std::string description{};
    uint32 progress{};
    uint32 max_progress{};
    bool achieved{};
    bool for_progress{};
    bool rare{}; // <=10% global unlock rate: gold border in HMD toast
    uint32 unlock_time{};
    std::string icon_rgba{};   // copied RGBA bytes (square), may be empty
    uint32 icon_size{};        // width == height
    std::chrono::milliseconds trigger_time{};
    std::chrono::milliseconds scheduled_show_time{};
};

class VROverlayBridge {
    class Settings *settings = nullptr;

    mutable std::recursive_mutex vr_mutex{};
    bool runtime_probed = false;
    bool runtime_available = false; // real SteamVR / openvr_api present
    bool hmd_present = false;

    void *openvr_module = nullptr; // dynamic handle, never linked statically

    // Native SteamVR handles (resolved via VR_GetGenericInterface, see
    // vr_openvr_abi.h for the pinned vtable layout). Null unless a current
    // SteamVR runtime answered with the exact expected interface versions.
    gbe_vr::IVROverlay_028 *native_overlay = nullptr;
    gbe_vr::IVRSystem_026 *native_system = nullptr;
    bool native_available = false;

    // Scene toast overlay + dashboard tab handles (opaque, only valid if runtime_available).
    // We keep them as uint64 to avoid including openvr headers.
    uint64_t scene_overlay_handle = 0;
    uint64_t dashboard_overlay_handle = 0;
    uint64_t dashboard_thumb_handle = 0;
    bool overlays_created = false;
    // When the scene toast must auto-hide (steady_clock).
    std::chrono::steady_clock::time_point scene_visible_until{};
    bool scene_visible = false;

    std::deque<VRAchToast> vr_queue{};
    std::chrono::milliseconds last_scheduled_show_time{};

    // Dashboard history (mirrors desktop NotificationHistoryEntry, capped).
    struct VRHistoryEntry {
        std::chrono::milliseconds timestamp{};
        std::string title{};
        std::string description{};
        bool achieved{};
    };
    std::deque<VRHistoryEntry> dashboard_history{};
    static constexpr size_t MAX_VR_HISTORY = 20;

    bool probe_runtime();          // dynamic-load openvr_api, detect HMD (cached)
    bool ensure_overlays();        // CreateOverlay + CreateDashboardOverlay (once)
    void apply_anchor_transform(); // SetOverlayTransform* per current anchor/size/offset
    void push_dashboard_history(const VRAchToast &toast);
    // Native SteamVR path (2b): resolve IVROverlay_028/IVRSystem_026 via
    // VR_GetGenericInterface, Find/Create scene + dashboard overlays.
    // Returns true when native toasts can be shown. Lock must be held.
    bool init_native();
    // Hide + destroy native overlays. Never calls VR_Shutdown (game owns VR).
    void shutdown_native();
    // Show the composed toast PNG in-headset at the configured anchor.
    void show_native_toast(const std::string &png_path);
    void hide_native_scene();
    float toast_duration_sec() const;
    // Writes %TEMP%/gbe_vr_achievement_toast.json (+ .png toast image,
    // + gbe_vr_dashboard.json history snapshot) so companion tools
    // (OVR Toolkit/XSOverlay) can display even when native IVROverlay is
    // unavailable. Returns the toast PNG path (empty on failure).
    // Best-effort, never fails the queue.
    std::string write_companion_toast_file(const VRAchToast &toast);

public:
    explicit VROverlayBridge(class Settings *settings);
    ~VROverlayBridge();

    VROverlayBridge(const VROverlayBridge &) = delete;
    VROverlayBridge &operator=(const VROverlayBridge &) = delete;

    void Setup();
    void Shutdown();

    // True when toasts must go HMD-exclusive (suppress desktop mirror).
    bool IsActive();
    // True when openvr_api + HMD were detected (regardless of enable flag).
    bool IsHmdPresent();

    bool ShouldSuppressDesktopToast() const;

    void QueueToast(const VRAchToast &toast);
    void ProcessQueue(); // called from overlay_render_proc()
    void ShowTestToast();

    // Dashboard data access (rendered by Steam_Overlay dashboard hookup or
    // future native overlay texture painter).
    size_t DashboardHistorySize() const;
    bool DashboardHistoryEntry(size_t idx, std::string &title, std::string &desc, bool &achieved) const;

    // Live Toast Setup editing from dashboard UI.
    void SetAnchor(VRToastAnchor a);
    void SetWidth(float w_m);
    void SetOffset(float x, float y, float z);
    void SetTilt(float deg);
    VRToastAnchor GetAnchor() const;
};

#endif // EMU_OVERLAY

#endif // __INCLUDED_GBE_VR_OVERLAY_H__
