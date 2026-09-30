#ifdef EMU_OVERLAY

#include "overlay/vr_overlay.h"
#include "overlay/vr_toast_image.h"
#include "dll/dll.h" // PRINT_DEBUG
#include <cstdlib>
#include <cmath>
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
    // Hide + destroy native overlays first (needs the interfaces + module).
    // Never calls VR_Shutdown: the game owns the VR lifecycle.
    shutdown_native();
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

static gbe_vr::VR_GetGenericInterface_Fn native_get_interface_proc(void *module)
{
    if (!module) return nullptr;
#ifdef __WINDOWS__
    FARPROC p = GetProcAddress((HMODULE)module, "VR_GetGenericInterface");
    return (gbe_vr::VR_GetGenericInterface_Fn)p;
#else
    void *p = dlsym(module, "VR_GetGenericInterface");
    return (gbe_vr::VR_GetGenericInterface_Fn)p;
#endif
}

static gbe_vr::VR_Init_Fn native_init_proc(void *module)
{
    if (!module) return nullptr;
#ifdef __WINDOWS__
    FARPROC p = GetProcAddress((HMODULE)module, "VR_Init");
    return (gbe_vr::VR_Init_Fn)p;
#else
    void *p = dlsym(module, "VR_Init");
    return (gbe_vr::VR_Init_Fn)p;
#endif
}

bool VROverlayBridge::init_native()
{
    // Lock is held by callers (ensure_overlays / show path).
    if (native_available && native_overlay) return true;
    native_available = false;
    native_overlay = nullptr;
    native_system = nullptr;
    if (!openvr_module) return false;

    auto get_iface = native_get_interface_proc(openvr_module);
    if (!get_iface) {
        PRINT_DEBUG("openvr_api has no VR_GetGenericInterface export");
        return false;
    }

    gbe_vr::EVRInitError err = gbe_vr::VRInitError_None;
    void *ov = get_iface(gbe_vr::k_IVROverlay_Version, &err);
    if (!ov || err != gbe_vr::VRInitError_None) {
        // Nobody in this process inited VR yet (e.g. flat game with SteamVR
        // running): init overlay-mode ourselves so toasts still reach the HMD.
        auto vr_init = native_init_proc(openvr_module);
        if (vr_init) {
            vr_init(&err, gbe_vr::VRApplication_Overlay);
            if (err == gbe_vr::VRInitError_None)
                ov = get_iface(gbe_vr::k_IVROverlay_Version, &err);
        }
    }
    if (!ov || err != gbe_vr::VRInitError_None) {
        PRINT_DEBUG("IVROverlay_028 unavailable (err=%i), companion-file fallback", (int)err);
        return false;
    }
    native_overlay = (gbe_vr::IVROverlay_028 *)ov;

    // IVRSystem is optional: only needed for wrist controller role lookup.
    // Head/chest anchors use HMD index 0 without it.
    gbe_vr::EVRInitError serr = gbe_vr::VRInitError_None;
    void *sys = get_iface(gbe_vr::k_IVRSystem_Version, &serr);
    if (sys && serr == gbe_vr::VRInitError_None)
        native_system = (gbe_vr::IVRSystem_026 *)sys;

    // Scene toast overlay (Find first: keys survive across Setup cycles).
    gbe_vr::VROverlayHandle_t scene = 0;
    if (native_overlay->FindOverlay("gbe.scene.toast", &scene) != gbe_vr::VROverlayError_None || !scene) {
        if (native_overlay->CreateOverlay("gbe.scene.toast", "GBE Achievement Toast", &scene) != gbe_vr::VROverlayError_None || !scene) {
            PRINT_DEBUG("CreateOverlay(scene) failed, companion-file fallback");
            native_overlay = nullptr;
            native_system = nullptr;
            return false;
        }
    }
    scene_overlay_handle = (uint64_t)scene;
    native_overlay->SetOverlaySortOrder(scene, 10);
    native_overlay->SetOverlayWidthInMeters(scene, settings->vr_overlay_config.width_m);

    // Dashboard tab overlay.
    gbe_vr::VROverlayHandle_t dash_main = 0, dash_thumb = 0;
    if (native_overlay->FindOverlay("gbe.achievements", &dash_main) != gbe_vr::VROverlayError_None || !dash_main) {
        if (native_overlay->CreateDashboardOverlay("gbe.achievements", "GBE Achievements", &dash_main, &dash_thumb) == gbe_vr::VROverlayError_None && dash_main) {
            dashboard_overlay_handle = (uint64_t)dash_main;
            dashboard_thumb_handle = (uint64_t)dash_thumb;
        } else {
            PRINT_DEBUG("CreateDashboardOverlay failed (non-fatal)");
        }
    } else {
        dashboard_overlay_handle = (uint64_t)dash_main;
    }

    native_available = true;
    PRINT_DEBUG("native IVROverlay ready (scene=%llu dash=%llu sys=%p)",
        (unsigned long long)scene_overlay_handle,
        (unsigned long long)dashboard_overlay_handle, native_system);
    return true;
}

void VROverlayBridge::shutdown_native()
{
    // Lock is held by Shutdown(). Never calls VR_Shutdown (game owns VR).
    if (native_overlay) {
        if (scene_visible && scene_overlay_handle) {
            native_overlay->HideOverlay((gbe_vr::VROverlayHandle_t)scene_overlay_handle);
            scene_visible = false;
        }
        if (scene_overlay_handle) {
            native_overlay->DestroyOverlay((gbe_vr::VROverlayHandle_t)scene_overlay_handle);
            scene_overlay_handle = 0;
        }
        if (dashboard_overlay_handle) {
            native_overlay->DestroyOverlay((gbe_vr::VROverlayHandle_t)dashboard_overlay_handle);
            dashboard_overlay_handle = 0;
            dashboard_thumb_handle = 0;
        }
    }
    native_overlay = nullptr;
    native_system = nullptr;
    native_available = false;
    overlays_created = false;
}

bool VROverlayBridge::ensure_overlays()
{
    if (overlays_created) return true;
    if (!runtime_available) return false;
    if (!init_native()) {
        // Native path unavailable (old SteamVR, no HMD session): the
        // companion PNG+JSON files still carry the toast to overlay tools.
        // Mark created to avoid spamming init attempts every toast; a fresh
        // probe happens on next Setup() cycle.
        overlays_created = true;
        return false;
    }
    overlays_created = true;
    return true;
}

// Build the device-relative transform for the current anchor preset.
// OpenVR: +y up, overlay plane faces +z. Wrist watch-face tips up toward the
// eyes by tilt_deg; head floats ahead; chest sits low and tips up.
static gbe_vr::HmdMatrix34_t build_anchor_matrix(VRToastAnchor anchor, float ox, float oy, float oz, float tilt_deg)
{
    const float pi = 3.14159265358979323846f;
    float tilt = -tilt_deg * pi / 180.0f; // tip normal from +z toward +y
    float tx = ox, ty = oy, tz = oz;

    switch (anchor) {
        case VRToastAnchor::head:
            tx += 0.0f; ty += -0.05f; tz += -0.60f;
            tilt = 0.0f;
            break;
        case VRToastAnchor::chest:
            tx += 0.0f; ty += -0.40f; tz += -0.55f;
            tilt = -20.0f * pi / 180.0f;
            break;
        case VRToastAnchor::left_wrist:
        case VRToastAnchor::right_wrist:
        case VRToastAnchor::dashboard_only:
        default:
            break; // wrist: user offset + tilt as configured
    }

    float c = cosf(tilt), s = sinf(tilt);
    gbe_vr::HmdMatrix34_t m{};
    m.m[0][0] = 1.0f; m.m[0][1] = 0.0f; m.m[0][2] = 0.0f; m.m[0][3] = tx;
    m.m[1][0] = 0.0f; m.m[1][1] = c;    m.m[1][2] = -s;   m.m[1][3] = ty;
    m.m[2][0] = 0.0f; m.m[2][1] = s;    m.m[2][2] = c;    m.m[2][3] = tz;
    return m;
}

float VROverlayBridge::toast_duration_sec() const
{
    float d = settings ? settings->vr_overlay_config.duration_sec : -1.0f;
    if (d > 0.0f) return d;
    if (settings) return settings->overlay_appearance.notification_duration_achievement / 1000.0f;
    return 7.0f;
}

void VROverlayBridge::apply_anchor_transform()
{
    if (!native_available || !native_overlay || !scene_overlay_handle) return;
    auto cfg = settings->vr_overlay_config;
    auto anchor = (VRToastAnchor)cfg.anchor;

    gbe_vr::TrackedDeviceIndex_t dev = gbe_vr::k_HmdIndex;
    if ((anchor == VRToastAnchor::left_wrist || anchor == VRToastAnchor::right_wrist) && native_system) {
        auto role = (anchor == VRToastAnchor::left_wrist)
            ? gbe_vr::ControllerRole_LeftHand : gbe_vr::ControllerRole_RightHand;
        gbe_vr::TrackedDeviceIndex_t idx = native_system->GetTrackedDeviceIndexForControllerRole(role);
        if (idx != gbe_vr::k_InvalidDeviceIndex) {
            dev = idx;
        } else if (!cfg.fallback_to_head) {
            PRINT_DEBUG("wrist controller untracked and no fallback, skipping native toast");
            return;
        }
        // else: fall through with HMD device (head fallback)
    }

    auto mat = build_anchor_matrix(anchor, cfg.offset_x, cfg.offset_y, cfg.offset_z, cfg.tilt_deg);
    auto scene = (gbe_vr::VROverlayHandle_t)scene_overlay_handle;
    native_overlay->SetOverlayTransformTrackedDeviceRelative(scene, dev, &mat);
    native_overlay->SetOverlayWidthInMeters(scene, cfg.width_m);
}

void VROverlayBridge::show_native_toast(const std::string &png_path)
{
    if (png_path.empty()) return;
    if (!native_available && !init_native()) return;
    if (!native_overlay || !scene_overlay_handle) return;
    if ((VRToastAnchor)settings->vr_overlay_config.anchor == VRToastAnchor::dashboard_only) {
        // Dashboard-only: refresh the tab thumbnail so the menu shows latest.
        if (dashboard_thumb_handle)
            native_overlay->SetOverlayFromFile((gbe_vr::VROverlayHandle_t)dashboard_thumb_handle, png_path.c_str());
        return;
    }
    auto scene = (gbe_vr::VROverlayHandle_t)scene_overlay_handle;
    if (native_overlay->SetOverlayFromFile(scene, png_path.c_str()) != gbe_vr::VROverlayError_None) {
        PRINT_DEBUG("SetOverlayFromFile failed");
        return;
    }
    apply_anchor_transform();
    if (native_overlay->ShowOverlay(scene) != gbe_vr::VROverlayError_None) {
        PRINT_DEBUG("ShowOverlay failed");
        return;
    }
    // Dashboard thumbnail mirrors the latest toast.
    if (dashboard_thumb_handle)
        native_overlay->SetOverlayFromFile((gbe_vr::VROverlayHandle_t)dashboard_thumb_handle, png_path.c_str());
    scene_visible = true;
    auto ms = (int)(toast_duration_sec() * 1000.0f);
    scene_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    PRINT_DEBUG("native toast shown (%s)", png_path.c_str());
}

void VROverlayBridge::hide_native_scene()
{
    if (native_overlay && scene_visible && scene_overlay_handle) {
        native_overlay->HideOverlay((gbe_vr::VROverlayHandle_t)scene_overlay_handle);
    }
    scene_visible = false;
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

std::string VROverlayBridge::write_companion_toast_file(const VRAchToast &toast)
{
    std::string png_str;
    try {
        auto tmp = std::filesystem::temp_directory_path();
        auto json_path = tmp / "gbe_vr_achievement_toast.json";
        auto png_path = tmp / "gbe_vr_achievement_toast.png";
        auto dash_path = tmp / "gbe_vr_dashboard.json";
        std::string anchor = VROverlayConfig::anchor_to_string(static_cast<VRToastAnchor>(settings->vr_overlay_config.anchor));
        float width = settings->vr_overlay_config.width_m;

        // Compose + write the toast PNG (icon + title + description + progress).
        // Native path shows this file via SetOverlayFromFile; companion tools
        // (OVR Toolkit / XSOverlay) can display it too.
        png_str = png_path.string();
        {
            VRToastImageRequest req{};
            req.title = toast.title;
            req.description = toast.description;
            req.icon_rgba = toast.icon_rgba;
            req.icon_size = toast.icon_size;
            req.progress = toast.progress;
            req.max_progress = toast.max_progress;
            req.for_progress = toast.for_progress;
            req.rare = toast.rare;
            VRToastImage img{};
            std::vector<uint8_t> png{};
            if (ComposeVRToastImage(req, img) && EncodeVRToastPNG(img, png) && !png.empty()) {
                std::ofstream pf(png_path, std::ios::binary | std::ios::trunc);
                if (pf) pf.write(reinterpret_cast<const char *>(png.data()), (std::streamsize)png.size());
                else png_str.clear();
            } else {
                png_str.clear();
            }
        }

        std::ofstream f(json_path, std::ios::trunc);
        if (!f) return png_str;
        f << "{"
          << "\"name\":\"" << json_escape(toast.name) << "\","
          << "\"title\":\"" << json_escape(toast.title) << "\","
          << "\"description\":\"" << json_escape(toast.description) << "\","
          << "\"achieved\":" << (toast.achieved ? "true" : "false") << ","
          << "\"for_progress\":" << (toast.for_progress ? "true" : "false") << ","
          << "\"rare\":" << (toast.rare ? "true" : "false") << ","
          << "\"progress\":" << toast.progress << ","
          << "\"max_progress\":" << toast.max_progress << ","
          << "\"anchor\":\"" << anchor << "\","
          << "\"width_m\":" << width << ","
          << "\"image\":\"" << json_escape(png_str) << "\""
          << "}";

        // Dashboard history snapshot for the future native tab + tools.
        std::ofstream d(dash_path, std::ios::trunc);
        if (d) {
            d << "{\"history\":[";
            bool first = true;
            for (const auto &e : dashboard_history) {
                if (!first) d << ",";
                first = false;
                d << "{\"title\":\"" << json_escape(e.title) << "\","
                  << "\"description\":\"" << json_escape(e.description) << "\","
                  << "\"achieved\":" << (e.achieved ? "true" : "false") << "}";
            }
            d << "]}";
        }
    } catch (...) {
        // best-effort only
    }
    return png_str;
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
        VROverlayConfig::anchor_to_string(static_cast<VRToastAnchor>(settings->vr_overlay_config.anchor)).c_str());
}

void VROverlayBridge::ProcessQueue()
{
    std::lock_guard<std::recursive_mutex> lock(vr_mutex);
    // Auto-hide the native scene toast when its duration elapses, even with
    // nothing else queued.
    if (scene_visible && std::chrono::steady_clock::now() >= scene_visible_until)
        hide_native_scene();
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

        // Companion PNG+JSON always written (native path + external tools).
        std::string png = write_companion_toast_file(t);
        // Native path: show the PNG in-headset, or refresh the dashboard
        // thumbnail in dashboard_only mode (no-op without SteamVR).
        if (!png.empty()) {
            ensure_overlays();
            show_native_toast(png);
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
    if (settings) settings->vr_overlay_config.anchor = static_cast<int>(a);
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
    return static_cast<VRToastAnchor>(settings->vr_overlay_config.anchor);
}

#endif // EMU_OVERLAY
