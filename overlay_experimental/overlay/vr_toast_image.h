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
};

// Canvas is fixed 1024x288 (readable at wrist width ~0.16m).
struct VRToastImage {
    static constexpr uint32_t WIDTH = 1024;
    static constexpr uint32_t HEIGHT = 288;
    std::vector<uint8_t> rgba{}; // WIDTH*HEIGHT*4
};

// Returns false only on empty title+description (nothing to draw).
bool ComposeVRToastImage(const VRToastImageRequest &req, VRToastImage &out);

// PNG-encode an RGBA image. Returns false on failure.
bool EncodeVRToastPNG(const VRToastImage &img, std::vector<uint8_t> &out_png);

#endif // EMU_OVERLAY

#endif // __INCLUDED_GBE_VR_TOAST_IMAGE_H__
