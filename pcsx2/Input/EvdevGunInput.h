// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

// Reads dedicated light gun devices directly via Linux evdev and feeds them to
// the emulator as extra pointers, so both players can aim with their own gun
// independently of the system cursor (which stays pointer index 0).
//
// Gun slot 0 (Player 1) -> pointer index 1, gun slot 1 (Player 2) -> pointer
// index 2. A gun with no evdev device falls back to the system mouse (pointer 0).
//
// Both absolute (ABS_X/ABS_Y, typical for real light guns) and relative
// (REL_X/REL_Y) aiming are supported. Mouse buttons map to pointer buttons 0-2
// and the gun's extra buttons (BTN_1..BTN_8) to pointer buttons 3-10, so they
// can be bound to Trigger/Start/Coin/etc. in the GUI.
//
// On non-Linux platforms every function is a no-op stub: EnumerateDevices()
// returns empty, Start() returns false, and PointerIndexForGun() returns 0.
namespace EvdevGun
{
	// Number of independent gun slots (0 = Player 1, 1 = Player 2).
	static constexpr u32 NUM_GUNS = 2;

	// Devices tagged ID_INPUT_GUN by udev, as {evdev node path, display name}.
	std::vector<std::pair<std::string, std::string>> EnumerateDevices();

	// Pointer index a gun should read from: its dedicated evdev pointer when one is
	// running, otherwise 0 (the system mouse). gun: 0 = P1, 1 = P2.
	u32 PointerIndexForGun(u32 gun);

	// Start all gun slots, selecting devices Batocera-style from the sorted
	// ID_INPUT_GUN list. numdevice[gun] < 0 auto-assigns by order (gun 0 = first
	// gun, gun 1 = second, ...); numdevice[gun] >= 0 forces that list index.
	// exclusive (default) grabs each device with EVIOCGRAB so it stops driving the
	// desktop cursor (in-game). Pass false for GUI button-binding: the device is read
	// shared so presses still reach InputManager, but the desktop mouse stays usable.
	void StartGuns(const std::array<int, NUM_GUNS>& numdevice, bool exclusive = true);

	// Start reading device_path for the given gun slot on a background thread.
	// exclusive grabs it with EVIOCGRAB (in-game); false reads it shared (GUI binding).
	// Restarts the slot if the device changed; an empty path stops the slot.
	// Returns false if open failed.
	bool Start(u32 gun, const std::string& device_path, bool exclusive = true);

	// Stop a single gun slot's reader, or all of them.
	void Stop(u32 gun);
	void StopAll();

	bool IsRunning(u32 gun);
} // namespace EvdevGun
