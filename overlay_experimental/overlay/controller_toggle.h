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

#ifndef __INCLUDED_CONTROLLER_TOGGLE_H__
#define __INCLUDED_CONTROLLER_TOGGLE_H__

#ifdef EMU_OVERLAY

#include "dll/base.h"

#include <chrono>

// Polls a game controller button and reports a single debounced "pressed"
// edge, so a controller can open/close the in-game overlay just like the
// keyboard hotkey.
//
// Windows: uses XInput. The Xbox/Guide ("Home") bit is intentionally hidden by
// the public XInputGetState() export, so we resolve the undocumented ordinal 100
// export (XInputGetStateEx), which does report it, once during Configure().
// Non-Windows: currently a no-op stub (Linux evdev BTN_MODE is deferred).
class ControllerToggle
{
public:
    enum class Button
    {
        Guide,
        Start,
        Back,
    };

    ControllerToggle() = default;
    ~ControllerToggle();

    ControllerToggle(ControllerToggle const&) = delete;
    ControllerToggle(ControllerToggle&&) = delete;
    ControllerToggle& operator=(ControllerToggle const&) = delete;
    ControllerToggle& operator=(ControllerToggle&&) = delete;

    // Resolves the input library (once) and arms the poller for `button`.
    // Does nothing when `enabled` is false, so the feature stays off by default.
    void Configure(bool enabled, Button button);

    // Returns true exactly once per physical press (rising edge + short cooldown).
    bool Poll();

private:
    [[maybe_unused]] bool enabled_ = false;
    [[maybe_unused]] Button button_ = Button::Guide;

    // Per-slot previous state, so a controller hot-swap / reconnect can't fake an edge.
    [[maybe_unused]] bool prev_down_[4] = { false, false, false, false };
    [[maybe_unused]] std::chrono::steady_clock::time_point last_toggle_{};
    // Skip the very first sample, so a button already held at startup
    // doesn't immediately toggle the overlay.
    [[maybe_unused]] bool primed_ = false;

#ifdef __WINDOWS__
    bool LoadInput();
    unsigned short ButtonMask() const;

    void* input_lib_ = nullptr;
    void* get_state_fn_ = nullptr; // XInputGetState (or ordinal-100 XInputGetStateEx, when available)
    bool guide_available_ = false;
#endif
};

#endif // EMU_OVERLAY

#endif // __INCLUDED_CONTROLLER_TOGGLE_H__
