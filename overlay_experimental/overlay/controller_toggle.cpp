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

#ifdef EMU_OVERLAY

#include "overlay/controller_toggle.h"

#ifdef __WINDOWS__

#include <xinput.h>

namespace {

using XInputGetStateFn = DWORD (WINAPI *)(DWORD dwUserIndex, XINPUT_STATE *pState);

constexpr DWORD XI_MAX_COUNT = 4;
constexpr std::chrono::milliseconds TOGGLE_COOLDOWN{ 400 };

// XINPUT_GAMEPAD_* button bits. Guide is defined here explicitly: not all
// SDK headers ship XINPUT_GAMEPAD_GUIDE, and it's the whole point of this class.
constexpr unsigned short XI_GAMEPAD_GUIDE = 0x0400;
constexpr unsigned short XI_GAMEPAD_START = 0x0010;
constexpr unsigned short XI_GAMEPAD_BACK  = 0x0020;

// The undocumented ordinal-100 XInput export ("XInputGetStateEx") is the only
// one that reports the Guide button.
constexpr WORD XI_GET_STATE_EX_ORDINAL = 100;

// try the newest runtime first; all of these may be present on a given machine
const wchar_t *const XI_DLL_CANDIDATES[] = {
    L"xinput1_4.dll",
    L"xinput1_3.dll",
    L"xinput9_1_0.dll",
};

} // namespace

#endif // __WINDOWS__

ControllerToggle::~ControllerToggle()
{
#ifdef __WINDOWS__
    if (input_lib_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(input_lib_));
        input_lib_ = nullptr;
    }
#endif
}

void ControllerToggle::Configure(bool enabled, Button button)
{
    enabled_ = enabled;
    button_ = button;

#ifdef __WINDOWS__
    if (!enabled_) return;

    if (!LoadInput()) {
        PRINT_DEBUG("controller toggle: XInput is not available, feature disabled");
        enabled_ = false;
        return;
    }

    if (button_ == Button::Guide && !guide_available_) {
        PRINT_DEBUG("controller toggle: the Guide button is unavailable on this runtime (no XInputGetStateEx), use 'start' or 'back' instead");
    }
    PRINT_DEBUG("controller toggle: enabled (button=%i, guide_available=%i)", static_cast<int>(button_), static_cast<int>(guide_available_));
#endif
}

bool ControllerToggle::Poll()
{
#ifdef __WINDOWS__
    if (!enabled_ || get_state_fn_ == nullptr) return false;

    auto get_state = reinterpret_cast<XInputGetStateFn>(get_state_fn_);
    const unsigned short mask = ButtonMask();

    bool down[XI_MAX_COUNT] = { false, false, false, false };
    for (DWORD i = 0; i < XI_MAX_COUNT; i++) {
        XINPUT_STATE state{};
        if (get_state(i, &state) != ERROR_SUCCESS) continue;
        down[i] = (state.Gamepad.wButtons & mask) != 0;
    }

    // rising edge on any slot (a disconnected slot reports false, so a
    // hot-swapped controller can't leave a stale "held" state behind)
    bool rising = false;
    for (DWORD i = 0; i < XI_MAX_COUNT; i++) {
        if (down[i] && !prev_down_[i]) rising = true;
        prev_down_[i] = down[i];
    }

    // ignore the first sample: a button already held at startup must not toggle
    if (!primed_) {
        primed_ = true;
        return false;
    }

    if (!rising) return false;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_toggle_ < TOGGLE_COOLDOWN) return false;
    last_toggle_ = now;
    return true;
#else
    (void)enabled_;
    (void)button_;
    return false;
#endif
}

#ifdef __WINDOWS__

bool ControllerToggle::LoadInput()
{
    if (get_state_fn_ != nullptr) return true;

    for (const auto dll_name : XI_DLL_CANDIDATES) {
        HMODULE mod = LoadLibraryW(dll_name);
        if (mod == nullptr) continue;

        auto get_state_ex = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, MAKEINTRESOURCEA(XI_GET_STATE_EX_ORDINAL)));
        auto get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));

        // prefer the Guide-capable export; fall back to the public one so that
        // start/back still work on runtimes without the undocumented export
        if (get_state_ex != nullptr) {
            input_lib_ = mod;
            get_state_fn_ = reinterpret_cast<void *>(get_state_ex);
            guide_available_ = true;
            return true;
        }
        if (get_state != nullptr) {
            input_lib_ = mod;
            get_state_fn_ = reinterpret_cast<void *>(get_state);
            guide_available_ = false;
            return true;
        }

        FreeLibrary(mod);
    }

    return false;
}

unsigned short ControllerToggle::ButtonMask() const
{
    switch (button_) {
        case Button::Guide: return XI_GAMEPAD_GUIDE;
        case Button::Start: return XI_GAMEPAD_START;
        case Button::Back:  return XI_GAMEPAD_BACK;
    }
    return 0;
}

#endif // __WINDOWS__

#endif // EMU_OVERLAY
