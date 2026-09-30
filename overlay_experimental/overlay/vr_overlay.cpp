#ifdef EMU_OVERLAY

#include "overlay/vr_overlay.h"
#include "dll/dll.h" // PRINT_DEBUG
#include <cstdlib>
#include <fstream>
#include <filesystem>

#ifdef __WINDOWS__
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// VROverlayConfig helpers
// ---------------------------------------------------------------------------

VRToastAnchor VROverlayConfig::anchor_from_string(const std::string &s)
{
    std::string v = s;
    for (auto &c : v) c = (char)::tolower((unsigned char)c);
    if (v == "right_wrist" || v == "right" || v == "r_wrist") return VRToastAnchor::right_wrist;
    if (v == "head" || v == "hmd" || v == "face") return VRToastAnchor::head;
    if (v == "chest" || v == "knee-chest" || v == "knee_chest" || v == "body") return VRToastAnchor::chest;
    if (v == "dashboard_only" || v == "dashboard" || v == "disabled" || v == "none") return VRToastAnchor::dashboard_only;
    return VRToastAnchor::left_wrist;
}

std::string VROverlayConfig::anchor_to_string(VRToastAnchor a)
{
    switch (a) {
        case VRToastAnchor::right_wrist: return "right_wrist";
        case VRToastAnchor::head: return "head";
        case VRToastAnchor::chest: return "chest";
        case VRToastAnchor::dashboard_only: return "dashboard_only";
        case VRToastAnchor::left_wrist:
        default: return "left_wrist";
    }
}

// ---------------------------------------------------------------------------
// VROverlayBridge
// ---------------------------------------------------------------------------

VROverlayBridge::VROverlayBridge(class Settings *settings)
    : settings(settings)
{}

VROverlayBridge::~VROverlayBridge()
{
    Shutdown();
}

void VROverlayBridge::Setup()
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    PRINT_DEBUG_ENTRY();
    // Lazy probe: don't touch openvr_api until first toast or explicit HMD query.
    // This keeps flat games at zero cost.
}

void VROverlayBridge::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    // NOTE: real IVROverlay_DestroyOverlay calls land here once native upload
    // is implemented (Phase 2). For now just drop handles + unload the module.
    scene_overlay_handle = 0;
    dashboard_overlay_handle = 0;
    overlays_created = false;
    vr_queue.clear();

#ifdef __WINDOWS__
    if (openvr_module) {
        FreeLibrary((HMODULE)openvr_module);
        openvr_module = nullptr;
    }
#else
    if (openvr_module) {
        dlclose(openvr_module);
        openvr_module = nullptr;
    }
#endif
    runtime_probed = false;
    runtime_available = false;
    hmd_present = false;
}

static bool env_truthy(const char *name)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return false;
    return !(v[0] == '0' && v[1] == '\0');
}

bool VROverlayBridge::probe_runtime()
{
    if (runtime_probed) return runtime_available;
    runtime_probed = true;
    runtime_available = false;
    hmd_present = false;

    // Explicit opt-outs first (cheap, no DLL touch).
    if (std::getenv("GBE_DISABLE_VR") && env_truthy("GBE_DISABLE_VR")) {
        PRINT_DEBUG("GBE_DISABLE_VR set, VR bridge disabled");
        return false;
    }

    // Try dynamic load of the real SteamVR client library.
    // This is the same DLL VR games themselves load; we never link it.
#ifdef __WINDOWS__
    const char *candidates[] = { "openvr_api.dll", nullptr };
    for (int i = 0; candidates[i]; ++i) {
        HMODULE mod = LoadLibraryA(candidates[i]);
        if (mod) {
            openvr_module = (void *)mod;
            // Minimal presence check: VR_IsHmdPresent exported by openvr_api.
            FARPROC fn = GetProcAddress(mod, "VR_IsHmdPresent");
            if (fn) {
                using fn_t = uint8_t (__cdecl *)(void);
                // NOTE: real signature is bool VR_IsHmdPresent(); call it.
                bool present = ((fn_t)fn)() != 0;
                runtime_available = true;
                hmd_present = present;
                PRINT_DEBUG("openvr_api found, HMD present=%i", (int)present);
            } else {
                // Library exists but unexpected exports: treat as available,
                // HMD unknown until compositor init (Phase 2).
                runtime_available = true;
                PRINT_DEBUG("openvr_api found, VR_IsHmdPresent export missing");
            }
            break;
        }
    }
#else
    const char *candidates[] = { "libopenvr_api.so", "libopenvr_api.so.1", nullptr };
    for (int i = 0; candidates[i]; ++i) {
        void *mod = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (mod) {
            openvr_module = mod;
            void *fn = dlsym(mod, "VR_IsHmdPresent");
            if (fn) {
                using fn_t = uint8_t (*)(void);
                bool present = ((fn_t)fn)() != 0;
                runtime_available = true;
                hmd_present = present;
                PRINT_DEBUG("libopenvr_api found, HMD present=%i", (int)present);
            } else {
                runtime_available = true;
                PRINT_DEBUG("libopenvr_api found, VR_IsHmdPresent symbol missing");
            }
            break;
        }
    }
#endif

    // Env hints used by SteamVR / OpenXR runtimes when the DLL isn't on PATH
    // (e.g. Linux flatpak, portable SteamVR). These mark "likely VR session"
    // so we still route toasts to the companion file + dashboard queue.
    if (!runtime_available) {
        if (env_truthy("STEAMVR_RUNNING") || env_truthy("VR_RUNNING") ||
            std::getenv("XR_RUNTIME_JSON") || std::getenv("STEAM_VR")) {
            runtime_available = true;
            PRINT_DEBUG("VR env hint detected, enabling companion-file VR path");
        }
    }

    return runtime_available;
}

bool VROverlayBridge::IsHmdPresent()
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (!probe_runtime()) return false;
    // If the runtime DLL answered, trust it. Otherwise fall back to env hint
    // (HMD assumed present when a VR session env is set).
    return hmd_present || runtime_available;
}

bool VROverlayBridge::ShouldSuppressDesktopToast() const
{
    if (!settings) return false;
    return settings->vr_overlay_config.suppress_desktop_achievements;
}

bool VROverlayBridge::IsActive()
{
    if (!settings) return false;
    if (!settings->vr_overlay_config.enable_vr_overlay) return false;
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (!probe_runtime()) return false;
    // Active = runtime seen. HMD strictness lives in Phase 2 compositor check;
    // for Phase 1 (companion file + dashboard queue) runtime/env hint is enough
    // to justify suppressing the desktop mirror per user request.
    return true;
}

bool VROverlayBridge::ensure_overlays()
{
    if (overlays_created) return true;
    if (!runtime_available) return false;
    // Phase 2: VROverlay()->CreateOverlay("gbe.scene.toast", ...) +
    // CreateDashboardOverlay("gbe.achievements", "Achievements", ...).
    // Stubbed until OpenVR headers are vendored; companion file covers Phase 1.
    // Mark created so we don't spam probe logs; real handles assigned in Phase 2.
    overlays_created = true;
    return true;
}

void VROverlayBridge::apply_anchor_transform()
{
    // Phase 2: translate vr_overlay_config {anchor,width,offset,tilt} into
    // SetOverlayTransformTrackedDeviceRelative() for wrist anchors
    // (ETrackedControllerRole LeftHand/RightHand + watch-face tilt), or
    // SetOverlayTransformTrackedDeviceRelative(HMD) for head/chest presets:
    //   head:  (0.0, 0.0, -0.6)
    //   chest: (0.0, -0.40, -0.50) tilted up ~-20deg
    // plus SetOverlayWidthInMeters(width_m).
}

void VROverlayBridge::push_dashboard_history(const VRAchToast &toast)
{
    VRHistoryEntry e{};
    e.timestamp = toast.trigger_time;
    e.title = toast.title;
    e.description = toast.description;
    e.achieved = toast.achieved;
    dashboard_history.push_front(e);
    while (dashboard_history.size() > MAX_VR_HISTORY) dashboard_history.pop_back();
}

static std::string json_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c; break;
        }
    }
    return o;
}

void VROverlayBridge::write_companion_toast_file(const VRAchToast &toast)
{
    try {
        auto tmp = std::filesystem::temp_directory_path();
        auto path = tmp / "gbe_vr_achievement_toast.json";
        std::string anchor = VROverlayConfig::anchor_to_string(settings->vr_overlay_config.anchor);
        float width = settings->vr_overlay_config.width_m;
        std::ofstream f(path, std::ios::trunc);
        if (!f) return;
        f << "{"
          << "\"name\":\"" << json_escape(toast.name) << "\","
          << "\"title\":\"" << json_escape(toast.title) << "\","
          << "\"description\":\"" << json_escape(toast.description) << "\","
          << "\"achieved\":" << (toast.achieved ? "true" : "false") << ","
          << "\"for_progress\":" << (toast.for_progress ? "true" : "false") << ","
          << "\"progress\":" << toast.progress << ","
          << "\"max_progress\":" << toast.max_progress << ","
          << "\"anchor\":\"" << anchor << "\","
          << "\"width_m\":" << width
          << "}";
    } catch (...) {
        // best-effort only
    }
}

void VROverlayBridge::QueueToast(const VRAchToast &toast_in)
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (!settings || !settings->vr_overlay_config.enable_vr_overlay) return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    VRAchToast t = toast_in;
    t.trigger_time = now;

    int delay_ms = settings->achievement_notification_delay_ms;
    if (delay_ms <= 0) {
        t.scheduled_show_time = now;
    } else {
        t.scheduled_show_time = std::max(now, last_scheduled_show_time + std::chrono::milliseconds(delay_ms));
    }
    last_scheduled_show_time = t.scheduled_show_time;
    vr_queue.push_back(t);
    PRINT_DEBUG("VR toast queued '%s' anchor=%s", t.name.c_str(),
        VROverlayConfig::anchor_to_string(settings->vr_overlay_config.anchor).c_str());
}

void VROverlayBridge::ProcessQueue()
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (vr_queue.empty()) return;
    if (!settings || !settings->vr_overlay_config.enable_vr_overlay) {
        vr_queue.clear();
        return;
    }

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    while (!vr_queue.empty()) {
        const auto &t = vr_queue.front();
        if (t.scheduled_show_time > now) break;

        // Dashboard history always records (dashboard_only mode included).
        push_dashboard_history(t);

        if (settings->vr_overlay_config.anchor != VRToastAnchor::dashboard_only) {
            ensure_overlays();
            apply_anchor_transform();
            // Phase 2: paint title+description+icon into overlay texture and
            // ShowOverlay() for duration_sec. Phase 1: companion file.
            write_companion_toast_file(t);
        } else {
            // dashboard_only: still write companion file so external overlays
            // (XSOverlay/OVR Toolkit) can mirror if the user wants.
            write_companion_toast_file(t);
        }

        PRINT_DEBUG("VR toast shown '%s'", t.name.c_str());
        vr_queue.pop_front();
    }
}

void VROverlayBridge::ShowTestToast()
{
    VRAchToast t{};
    t.name = "gbe_test_achievement";
    t.title = "Test achievement";
    t.description = "~~~ Test achievement ~~~";
    t.achieved = true;
    t.for_progress = false;
    QueueToast(t);
}

size_t VROverlayBridge::DashboardHistorySize() const
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    return dashboard_history.size();
}

bool VROverlayBridge::DashboardHistoryEntry(size_t idx, std::string &title, std::string &desc, bool &achieved) const
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (idx >= dashboard_history.size()) return false;
    const auto &e = dashboard_history[idx];
    title = e.title;
    desc = e.description;
    achieved = e.achieved;
    return true;
}

void VROverlayBridge::SetAnchor(VRToastAnchor a)
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (settings) settings->vr_overlay_config.anchor = a;
    apply_anchor_transform();
}

void VROverlayBridge::SetWidth(float w_m)
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (!settings) return;
    if (w_m < 0.08f) w_m = 0.08f;
    if (w_m > 0.30f) w_m = 0.30f;
    settings->vr_overlay_config.width_m = w_m;
    apply_anchor_transform();
}

void VROverlayBridge::SetOffset(float x, float y, float z)
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (!settings) return;
    settings->vr_overlay_config.offset_x = x;
    settings->vr_overlay_config.offset_y = y;
    settings->vr_overlay_config.offset_z = z;
    apply_anchor_transform();
}

void VROverlayBridge::SetTilt(float deg)
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    if (settings) settings->vr_overlay_config.tilt_deg = deg;
    apply_anchor_transform();
}

VRToastAnchor VROverlayBridge::GetAnchor() const
{
    if (!settings) return VRToastAnchor::left_wrist;
    return settings->vr_overlay_config.anchor;
}

#endif // EMU_OVERLAY
