// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Input/EvdevGunInput.h"

#if defined(__linux__)

#include "Input/InputManager.h"
#include "ImGui/ImGuiManager.h"
#include "Host.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <tuple>

#include <fcntl.h>
#include <libudev.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace EvdevGun
{
	namespace
	{
		constexpr long BITS_PER_LONG = sizeof(long) * 8;

		bool TestBit(int nr, const unsigned long* addr)
		{
			return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1UL;
		}

		// Whether the device reports absolute axes (ABS_X/ABS_Y), as real light guns do.
		bool HasAbsoluteAxes(int fd)
		{
			unsigned long ev_bits[(EV_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {};
			unsigned long abs_bits[(ABS_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {};

			if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !TestBit(EV_ABS, ev_bits))
				return false;
			if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
				return false;
			return TestBit(ABS_X, abs_bits) && TestBit(ABS_Y, abs_bits);
		}

		// Maps a Linux BTN_* code to the pointer button index used by InputManager:
		// the standard mouse buttons (left=0, right=1, middle=2) plus the light gun's
		// extra buttons BTN_1..BTN_8 -> 3..10. Returns -1 if unmapped.
		int PointerButtonForCode(u16 code)
		{
			switch (code)
			{
				case BTN_LEFT: return 0;
				case BTN_RIGHT: return 1;
				case BTN_MIDDLE: return 2;
				default: break;
			}
			if (code >= BTN_1 && code <= BTN_8)
				return 3 + (code - BTN_1);
			return -1;
		}

		// Per-gun reader state. Slot 0 = P1 (pointer 1), slot 1 = P2 (pointer 2).
		struct GunSlot
		{
			std::thread thread;
			std::atomic<bool> stop{false};
			std::atomic<bool> running{false};
			std::string device_path;
		};
		std::mutex s_mutex;
		GunSlot s_slots[NUM_GUNS];

		// Gun slot N feeds pointer index N + 1 (pointer 0 is reserved for the system mouse).
		u32 PointerIndexFor(u32 gun) { return gun + 1; }

		void ReaderThread(u32 gun, std::string device_path, bool exclusive)
		{
			GunSlot& slot = s_slots[gun];
			const u32 pointer_index = PointerIndexFor(gun);
			const int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0)
			{
				Console.Error("EvdevGun: failed to open '%s': %s. Ensure the device is readable "
							  "(add your user to the 'input' group or use a udev rule).",
					device_path.c_str(), std::strerror(errno));
				slot.running.store(false, std::memory_order_release);
				return;
			}

			// In-game we grab exclusively so the gun no longer drives the desktop cursor.
			// For GUI button-binding we read the device shared (no grab): the press still
			// reaches InputManager for binding, but the desktop mouse keeps working in Qt.
			if (exclusive && ioctl(fd, EVIOCGRAB, 1) < 0)
				Console.Warning("EvdevGun: could not grab '%s': %s", device_path.c_str(), std::strerror(errno));

			// Real light guns usually report absolute axes; fall back to relative (mouse-like).
			const bool absolute = HasAbsoluteAxes(fd);
			input_absinfo abs_x{}, abs_y{};
			if (absolute)
			{
				ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
				ioctl(fd, EVIOCGABS(ABS_Y), &abs_y);
			}
			Console.WriteLn("EvdevGun: P%u gun reading '%s' (%s aiming) -> pointer %u",
				gun + 1, device_path.c_str(), absolute ? "absolute" : "relative", pointer_index);

			auto window_dims = []() -> std::pair<float, float> {
				float w = ImGuiManager::GetWindowWidth();
				float h = ImGuiManager::GetWindowHeight();
				if (w <= 0.0f) w = 1920.0f;
				if (h <= 0.0f) h = 1080.0f;
				return {w, h};
			};
			// Map a raw absolute axis value to a 0..1 fraction of its reported range.
			auto abs_fraction = [](s32 value, const input_absinfo& info) -> float {
				const float range = static_cast<float>(info.maximum - info.minimum);
				if (range <= 0.0f)
					return 0.5f;
				return std::clamp((static_cast<float>(value - info.minimum)) / range, 0.0f, 1.0f);
			};

			float win_w, win_h;
			std::tie(win_w, win_h) = window_dims();
			// Window-space absolute position; start centred (overwritten on first absolute report).
			float pos_x = win_w * 0.5f;
			float pos_y = win_h * 0.5f;
			float frac_x = 0.5f, frac_y = 0.5f; // latest absolute fractions
			bool dirty = false;

			while (!slot.stop.load(std::memory_order_acquire))
			{
				pollfd pfd{fd, POLLIN, 0};
				const int pr = poll(&pfd, 1, 100);
				if (pr <= 0)
					continue; // timeout or EINTR; re-check stop flag
				if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
					break;

				input_event ev;
				while (read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev)))
				{
					switch (ev.type)
					{
						case EV_ABS:
							if (ev.code == ABS_X)
							{
								frac_x = abs_fraction(ev.value, abs_x);
								dirty = true;
							}
							else if (ev.code == ABS_Y)
							{
								frac_y = abs_fraction(ev.value, abs_y);
								dirty = true;
							}
							break;

						case EV_REL:
							if (ev.code == REL_X)
							{
								pos_x += static_cast<float>(ev.value);
								dirty = true;
							}
							else if (ev.code == REL_Y)
							{
								pos_y += static_cast<float>(ev.value);
								dirty = true;
							}
							break;

						case EV_KEY:
						{
							const int button = PointerButtonForCode(ev.code);
							if (button >= 0 && (ev.value == 0 || ev.value == 1))
							{
								Host::RunOnCPUThread([pointer_index, button, pressed = (ev.value == 1)]() {
									InputManager::InvokeEvents(
										InputManager::MakePointerButtonKey(pointer_index, button),
										static_cast<float>(pressed));
								});
							}
							break;
						}

						case EV_SYN:
							if (ev.code == SYN_REPORT && dirty)
							{
								std::tie(win_w, win_h) = window_dims();
								if (absolute)
								{
									pos_x = frac_x * win_w;
									pos_y = frac_y * win_h;
								}
								pos_x = std::clamp(pos_x, 0.0f, win_w);
								pos_y = std::clamp(pos_y, 0.0f, win_h);
								InputManager::UpdatePointerAbsolutePosition(pointer_index, pos_x, pos_y);
								dirty = false;
							}
							break;

						default:
							break;
					}
				}
			}

			if (exclusive)
				ioctl(fd, EVIOCGRAB, 0);
			close(fd);
			Console.WriteLn("EvdevGun: P%u gun stopped reading '%s'", gun + 1, device_path.c_str());
			slot.running.store(false, std::memory_order_release);
		}

		// Signal a slot's reader thread to exit and join it. Caller must hold s_mutex.
		void StopLocked(u32 gun)
		{
			GunSlot& slot = s_slots[gun];
			slot.stop.store(true, std::memory_order_release);
			if (slot.thread.joinable())
				slot.thread.join();
			slot.device_path.clear();
		}
	} // namespace

	std::vector<std::pair<std::string, std::string>> EnumerateDevices()
	{
		using DeviceList = std::vector<std::pair<std::string, std::string>>;
		DeviceList devices;

		udev* const ctx = udev_new();
		if (!ctx)
			return devices;

		// Collect every input event node tagged with the given udev property into `out`.
		const auto scan = [&](const char* id_property, DeviceList& out) {
			udev_enumerate* const en = udev_enumerate_new(ctx);
			if (!en)
				return;
			udev_enumerate_add_match_subsystem(en, "input");
			udev_enumerate_add_match_property(en, id_property, "1");
			udev_enumerate_scan_devices(en);

			udev_list_entry* dev_entry;
			udev_list_entry_foreach(dev_entry, udev_enumerate_get_list_entry(en))
			{
				const char* syspath = udev_list_entry_get_name(dev_entry);
				udev_device* const dev = udev_device_new_from_syspath(ctx, syspath);
				if (!dev)
					continue;

				const char* devnode = udev_device_get_devnode(dev);
				if (devnode && std::strncmp(devnode, "/dev/input/event", 16) == 0)
				{
					// Prefer the parent input device's friendly name.
					const char* name = nullptr;
					if (udev_device* parent = udev_device_get_parent(dev))
						name = udev_device_get_sysattr_value(parent, "name");
					out.emplace_back(devnode, name && name[0] ? name : "Light Gun");
				}

				udev_device_unref(dev);
			}
			udev_enumerate_unref(en);
		};

		// Batocera tags dedicated light guns ID_INPUT_GUN=1. Wii/IR setups instead
		// expose the pointer as a virtual mouse (the wii "mouse bar"), with no gun tag.
		// Gather both so mixed rigs work (e.g. a real gun on P1 + a wiimote on P2),
		// mirroring the RPCS3 light gun handler. The reader auto-detects abs vs rel.
		DeviceList guns, mice;
		scan("ID_INPUT_GUN", guns);
		scan("ID_INPUT_MOUSE", mice);
		udev_unref(ctx);

		// Sort by event-node number (event2 before event10), matching Batocera, so
		// autodetect-by-order is stable and intuitive.
		const auto event_number = [](const std::string& path) -> long {
			const size_t pos = path.find_last_not_of("0123456789");
			if (pos == std::string::npos || pos + 1 >= path.size())
				return -1;
			return std::strtol(path.c_str() + pos + 1, nullptr, 10);
		};
		const auto sort_unique = [&](DeviceList& v) {
			std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
				const long na = event_number(a.first), nb = event_number(b.first);
				return (na != nb) ? (na < nb) : (a.first < b.first);
			});
			v.erase(std::unique(v.begin(), v.end(),
						[](const auto& a, const auto& b) { return a.first == b.first; }),
				v.end());
		};
		sort_unique(guns);
		sort_unique(mice);

		// Guns take the low player slots (P1 first); mice fill the rest. A device
		// tagged as both a gun and a mouse stays a gun (skip its mouse duplicate).
		devices = std::move(guns);
		for (auto& m : mice)
		{
			if (std::none_of(devices.begin(), devices.end(),
					[&](const auto& d) { return d.first == m.first; }))
				devices.push_back(std::move(m));
		}
		return devices;
	}

	void StartGuns(const std::array<int, NUM_GUNS>& numdevice, bool exclusive)
	{
		const std::vector<std::pair<std::string, std::string>> devices = EnumerateDevices();

		// Batocera selection: gun N defaults to the N-th sorted gun; a numdevice >= 0
		// overrides that with an explicit index into the list.
		for (u32 gun = 0; gun < NUM_GUNS; gun++)
		{
			const int index = (numdevice[gun] >= 0) ? numdevice[gun] : static_cast<int>(gun);
			const std::string path =
				(index >= 0 && static_cast<size_t>(index) < devices.size()) ? devices[index].first : std::string();
			Start(gun, path, exclusive);
		}
	}

	u32 PointerIndexForGun(u32 gun)
	{
		if (gun < NUM_GUNS && s_slots[gun].running.load(std::memory_order_acquire))
			return PointerIndexFor(gun);
		return 0; // fall back to the system mouse
	}

	bool Start(u32 gun, const std::string& device_path, bool exclusive)
	{
		if (gun >= NUM_GUNS)
			return false;

		std::lock_guard<std::mutex> lock(s_mutex);
		GunSlot& slot = s_slots[gun];

		if (device_path.empty())
		{
			StopLocked(gun);
			return false;
		}

		if (slot.running.load(std::memory_order_acquire) && slot.device_path == device_path)
			return true; // already reading this device

		StopLocked(gun);

		slot.device_path = device_path;
		slot.stop.store(false, std::memory_order_release);
		slot.running.store(true, std::memory_order_release);
		slot.thread = std::thread(ReaderThread, gun, device_path, exclusive);
		return true;
	}

	void Stop(u32 gun)
	{
		if (gun >= NUM_GUNS)
			return;
		std::lock_guard<std::mutex> lock(s_mutex);
		StopLocked(gun);
	}

	void StopAll()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		for (u32 gun = 0; gun < NUM_GUNS; gun++)
			StopLocked(gun);
	}

	bool IsRunning(u32 gun)
	{
		return gun < NUM_GUNS && s_slots[gun].running.load(std::memory_order_acquire);
	}
} // namespace EvdevGun

#else // !__linux__

namespace EvdevGun
{
	std::vector<std::pair<std::string, std::string>> EnumerateDevices() { return {}; }
	u32 PointerIndexForGun(u32) { return 0; }
	void StartGuns(const std::array<int, NUM_GUNS>&, bool) {}
	bool Start(u32, const std::string&, bool) { return false; }
	void Stop(u32) {}
	void StopAll() {}
	bool IsRunning(u32) { return false; }
} // namespace EvdevGun

#endif
