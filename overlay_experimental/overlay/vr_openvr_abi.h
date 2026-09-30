#ifndef __INCLUDED_GBE_VR_OPENVR_ABI_H__
#define __INCLUDED_GBE_VR_OPENVR_ABI_H__

// Minimal OpenVR ABI for the GBE VR achievement path.
// Lets the emulator drive SteamVR overlays WITHOUT linking openvr_api or
// vendoring the full SDK: openvr_api is loaded dynamically at runtime and the
// interfaces below are resolved via VR_GetGenericInterface().
//
// VTABLE LAYOUT is pinned to:
//   IVROverlay_028  (OpenVR SDK master, ValveSoftware/openvr)
//   IVRSystem_026
// Slot indices were counted directly from openvr.h and double-checked
// against neighboring methods. Only the slots actually used carry real
// signatures; every other slot is a padding virtual so offsets stay exact.
// Calling an unknown/wrong slot would crash SteamVR, so if Valve ever bumps
// these versions you MUST re-verify the slots (see VR_GetGenericInterface
// version strings below) -- a version mismatch fails closed to the
// companion-file path, never to a blind call.
//
// OpenVR SDK license: BSD 3-Clause, (c) Valve Corporation. This file reuses
// only interface shapes/slots needed for interop (fair-use interop header).

#include <cstdint>

#ifdef EMU_OVERLAY

namespace gbe_vr {

// --- Plain types (layout-identical to openvr.h) ---------------------------

typedef uint32_t TrackedDeviceIndex_t;
static const TrackedDeviceIndex_t k_HmdIndex = 0;
static const TrackedDeviceIndex_t k_InvalidDeviceIndex = 0xFFFFFFFFu;

typedef uint64_t VROverlayHandle_t;
static const VROverlayHandle_t k_InvalidOverlayHandle = 0;

struct HmdMatrix34_t { float m[3][4]; };
struct HmdVector2_t { float v[2]; };

enum EVROverlayInputMethod {
    VROverlayInputMethod_None = 0,
    VROverlayInputMethod_Mouse = 1, // tracked controllers get mouse events automatically
};

// Overlay mouse event ids (data is VREvent_Mouse_Compat_t).
static const uint32_t k_VREvent_MouseMove = 300;
static const uint32_t k_VREvent_MouseButtonDown = 301;
static const uint32_t k_VREvent_MouseButtonUp = 302;
static const uint32_t k_VRMouseButton_Left = 0x0001;

// Minimal VREvent_t: header (12 bytes) + mouse payload at data offset.
// Full VREvent_t is exactly 60 bytes (pack 8); only the mouse prefix is
// ever read, other events are ignored by type id.
struct VREvent_Mouse_Compat_t {
    float x, y; // GL space: bottom-left of the texture is 0,0
    uint32_t button; // EVRMouseButton bitmask
    uint32_t cursor_index;
};
struct VREvent_Compat_t {
    uint32_t event_type;
    TrackedDeviceIndex_t tracked_device_index;
    float event_age_seconds;
    VREvent_Mouse_Compat_t mouse; // valid only for mouse events
    uint8_t reserved[32]; // pad to the real 60-byte VREvent_t
};
static_assert(sizeof(VREvent_Compat_t) == 60, "VREvent_t size must match openvr.h");

enum EVRInitError {
    VRInitError_None = 0,
};

enum EVRApplicationType {
    VRApplication_Overlay = 2,
};

enum ETrackedControllerRole {
    ControllerRole_Invalid = 0,
    ControllerRole_LeftHand = 1,
    ControllerRole_RightHand = 2,
};

enum EVROverlayError {
    VROverlayError_None = 0,
};

static const char * const k_IVROverlay_Version = "IVROverlay_028";
static const char * const k_IVRSystem_Version = "IVRSystem_026";

// --- IVRSystem_026 (only slot 18 is real) ---------------------------------

class IVRSystem_026 {
public:
    virtual void _pad00() = 0;
    virtual void _pad01() = 0;
    virtual void _pad02() = 0;
    virtual void _pad03() = 0;
    virtual void _pad04() = 0;
    virtual void _pad05() = 0;
    virtual void _pad06() = 0;
    virtual void _pad07() = 0;
    virtual void _pad08() = 0;
    virtual void _pad09() = 0;
    virtual void _pad10() = 0;
    virtual void _pad11() = 0;
    virtual void _pad12() = 0;
    virtual void _pad13() = 0;
    virtual void _pad14() = 0;
    virtual void _pad15() = 0;
    virtual void _pad16() = 0;
    virtual void _pad17() = 0;
    // slot 18: Returns the device index for a controller role, or
    // k_InvalidDeviceIndex when not tracked.
    virtual TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole role) = 0;
};

// --- IVROverlay_028 (real slots: 0,1,3,20,22,35,43,44,45,48,50,52,63,67) ----

class IVROverlay_028 {
public:
    // slot 0
    virtual EVROverlayError FindOverlay(const char *pchOverlayKey, VROverlayHandle_t *pOverlayHandle) = 0;
    // slot 1
    virtual EVROverlayError CreateOverlay(const char *pchOverlayKey, const char *pchOverlayName, VROverlayHandle_t *pOverlayHandle) = 0;
    virtual void _pad02() = 0; // slot 2: CreateSubviewOverlay
    // slot 3
    virtual EVROverlayError DestroyOverlay(VROverlayHandle_t ulOverlayHandle) = 0;
    virtual void _pad04() = 0;
    virtual void _pad05() = 0;
    virtual void _pad06() = 0;
    virtual void _pad07() = 0;
    virtual void _pad08() = 0;
    virtual void _pad09() = 0;
    virtual void _pad10() = 0;
    virtual void _pad11() = 0;
    virtual void _pad12() = 0;
    virtual void _pad13() = 0;
    virtual void _pad14() = 0;
    virtual void _pad15() = 0;
    virtual void _pad16() = 0;
    virtual void _pad17() = 0;
    virtual void _pad18() = 0;
    virtual void _pad19() = 0;
    // slot 20
    virtual EVROverlayError SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder) = 0;
    virtual void _pad21() = 0;
    // slot 22
    virtual EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters) = 0;
    virtual void _pad23() = 0;
    virtual void _pad24() = 0;
    virtual void _pad25() = 0;
    virtual void _pad26() = 0;
    virtual void _pad27() = 0;
    virtual void _pad28() = 0;
    virtual void _pad29() = 0;
    virtual void _pad30() = 0;
    virtual void _pad31() = 0;
    virtual void _pad32() = 0;
    virtual void _pad33() = 0;
    virtual void _pad34() = 0;
    // slot 35
    virtual EVROverlayError SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform) = 0;
    virtual void _pad36() = 0;
    virtual void _pad37() = 0;
    virtual void _pad38() = 0;
    virtual void _pad39() = 0;
    virtual void _pad40() = 0;
    virtual void _pad41() = 0;
    virtual void _pad42() = 0;
    // slot 43
    virtual EVROverlayError ShowOverlay(VROverlayHandle_t ulOverlayHandle) = 0;
    // slot 44
    virtual EVROverlayError HideOverlay(VROverlayHandle_t ulOverlayHandle) = 0;
    // slot 45
    virtual bool IsOverlayVisible(VROverlayHandle_t ulOverlayHandle) = 0;
    virtual void _pad46() = 0;
    virtual void _pad47() = 0;
    // slot 48: fills *pEvent when the overlay has a queued event.
    // uncbVREvent must be sizeof(VREvent_t) == 60.
    virtual bool PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_Compat_t *pEvent, uint32_t uncbVREvent) = 0;
    virtual void _pad49() = 0;
    // slot 50: dashboard overlays need Mouse so the laser becomes mouse events.
    virtual EVROverlayError SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, EVROverlayInputMethod eInputMethod) = 0;
    virtual void _pad51() = 0;
    // slot 52: mouse coords are reported in these units (set to texture px).
    virtual EVROverlayError SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t *pvecMouseScale) = 0;
    virtual void _pad53() = 0;
    virtual void _pad54() = 0;
    virtual void _pad55() = 0;
    virtual void _pad56() = 0;
    virtual void _pad57() = 0;
    virtual void _pad58() = 0;
    virtual void _pad59() = 0;
    virtual void _pad60() = 0;
    virtual void _pad61() = 0;
    virtual void _pad62() = 0;
    // slot 63
    virtual EVROverlayError SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char *pchFilePath) = 0;
    virtual void _pad64() = 0;
    virtual void _pad65() = 0;
    virtual void _pad66() = 0;
    // slot 67
    virtual EVROverlayError CreateDashboardOverlay(const char *pchOverlayKey, const char *pchOverlayFriendlyName, VROverlayHandle_t *pMainHandle, VROverlayHandle_t *pThumbnailHandle) = 0;
};

// C exports resolved via GetProcAddress/dlsym.
typedef void *(*VR_Init_Fn)(EVRInitError *peError, EVRApplicationType eApplicationType);
typedef void *(*VR_GetGenericInterface_Fn)(const char *pchInterfaceVersion, EVRInitError *peError);
typedef const char *(*VR_GetVRInitErrorAsEnglishDescription_Fn)(EVRInitError error);

} // namespace gbe_vr

#endif // EMU_OVERLAY

#endif // __INCLUDED_GBE_VR_OPENVR_ABI_H__
