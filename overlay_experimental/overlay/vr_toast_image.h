#ifndef __INCLUDED_GBE_VR_TOAST_IMAGE_H__
#define __INCLUDED_GBE_VR_TOAST_IMAGE_H__

// Software toast composer for the VR achievement path.
// Renders title + description (+ icon + progress bar) into an RGBA buffer
// and encodes PNG bytes. No GPU, no font files, no extra dependencies:
// text uses a small embedded 5x7 bitmap font, icons are blitted from the
// emulator's RGBA Image_Data. Output PNGs feed SetOverlayFromFile (Phase 2b)
// and companion tools (OVR Toolkit / XSOverlay) today.

#include <string>
#include <vector>
#include <cstdint>

#ifdef EMU_OVERLAY

struct VRToastImageRequest {
    std::string title{};
    std::string description{};
    std::string icon_rgba{};   // raw RGBA bytes, square; empty = no icon
    uint32_t icon_size = 0;    // width == height when icon present
    uint32_t progress = 0;
    uint32_t max_progress = 0;
    bool for_progress = false;
    bool rare = false;         // <=10% unlock rate: gold border
    bool flip_y = false;       // vertically flip output (SteamVR file-loader orientation fix)
};

// Canvas is 2048x576: exact desktop-toast replica at 2x, readable at arm's
// length on a ~0.22m wrist overlay.
struct VRToastImage {
    static constexpr uint32_t WIDTH = 2048;
    static constexpr uint32_t HEIGHT = 576;
    std::vector<uint8_t> rgba{}; // WIDTH*HEIGHT*4
};

// Returns false only on empty title+description (nothing to draw).
bool ComposeVRToastImage(const VRToastImageRequest &req, VRToastImage &out);

// Dashboard achievements list (mirrors the flat achievements window:
// unlocked gold first, then locked gray). 1024x1024 for the SteamVR tab.
struct VRDashboardEntry {
    std::string title{};
    std::string description{};
    std::string detail{}; // e.g. "12.5%" global unlock rate, may be empty
    bool achieved = false;
    bool rare = false;    // achieved + <=10%: gold marker
};

struct VRDashboardImage {
    static constexpr uint32_t WIDTH = 1024;
    static constexpr uint32_t HEIGHT = 1024;
    std::vector<uint8_t> rgba{}; // WIDTH*HEIGHT*4
};

// header example: "GSE Achievements (3/25)". Empty entries => "no data" panel.
// When hits != nullptr, tab-switch rects are appended (canvas px, top-left origin).
bool ComposeVRDashboardImage(const std::vector<VRDashboardEntry> &entries,
                             const std::string &header,
                             VRDashboardImage &out,
                             bool flip_y = false,
                             std::vector<struct VRDashHitRect> *hits = nullptr);

// Interactive dashboard: widget ids for hit-testing the setup view.
enum class VRDashWidget : uint8_t {
    none = 0,
    tab_achievements,
    tab_setup,
    anchor_cycle,
    slider_size,
    slider_ox,
    slider_oy,
    slider_oz,
    slider_tilt,
    slider_duration,
    check_suppress,
    check_fallback,
    check_flip,
    test_toast,
    save,
};

struct VRDashHitRect {
    VRDashWidget id = VRDashWidget::none;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // canvas px, top-left origin
};

// Live settings snapshot rendered by the Toast Setup view.
struct VRDashboardSetup {
    int anchor = 0; // 0..4 (VRToastAnchor order)
    float width_m = 0.22f;
    float offset_x = 0.0f, offset_y = 0.06f, offset_z = 0.0f;
    float tilt_deg = 45.0f;
    float duration_sec = -1.0f; // <=0 shows "auto"
    bool suppress_desktop = true;
    bool fallback_to_head = true;
    bool flip_y = false;
};

// Toast Setup view (1024x1024). Records one hit rect per widget into hits.
bool ComposeVRDashboardSetupImage(const VRDashboardSetup &setup,
                                  const std::string &header,
                                  VRDashboardImage &out,
                                  std::vector<VRDashHitRect> &hits,
                                  bool flip_y = false);

// PNG-encode an RGBA image. Returns false on failure.
bool EncodeVRToastPNG(const VRToastImage &img, std::vector<uint8_t> &out_png);
bool EncodeVRDashboardPNG(const VRDashboardImage &img, std::vector<uint8_t> &out_png);

#endif // EMU_OVERLAY

#endif // __INCLUDED_GBE_VR_TOAST_IMAGE_H__
