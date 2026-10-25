/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window_listener.h>

namespace {

constexpr const char* kDefaultInputBackend =
#if REX_PLATFORM_WIN32
    "xinput";
#else
    "sdl";
#endif

}  // namespace

REXCVAR_DEFINE_STRING(input_backend, kDefaultInputBackend, "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
// Back+Start is deliberately not the default: Steam Input uses View+Menu
// (Back+Start) as its Guide-button chord on pads without a Guide button, so
// opening our settings would also pop the Steam overlay.
REXCVAR_DEFINE_STRING(menu_chord, "rb+start", "Input",
                      "Controller chord that toggles the settings menu. Buttons joined by '+': "
                      "a, b, x, y, lb, rb, l3, r3, back, start, dpad_up, dpad_down, dpad_left, "
                      "dpad_right. Empty disables the chord.");
REXCVAR_DEFINE_BOOL(hid_rumble_enabled, true, "Input", "Enable controller vibration");
REXCVAR_DEFINE_UINT32(hid_rumble_min_motor_speed, 0x1000, "Input",
                      "Minimum XInput motor speed forwarded to host controllers")
    .range(0, 0xFFFF);
namespace rex::input {

namespace {

uint16_t ChordButtonFromToken(std::string_view token) {
  if (token == "a") return X_INPUT_GAMEPAD_A;
  if (token == "b") return X_INPUT_GAMEPAD_B;
  if (token == "x") return X_INPUT_GAMEPAD_X;
  if (token == "y") return X_INPUT_GAMEPAD_Y;
  if (token == "lb") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (token == "rb") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (token == "l3") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (token == "r3") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (token == "back" || token == "select" || token == "view") return X_INPUT_GAMEPAD_BACK;
  if (token == "start" || token == "menu") return X_INPUT_GAMEPAD_START;
  if (token == "dpad_up") return X_INPUT_GAMEPAD_DPAD_UP;
  if (token == "dpad_down") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (token == "dpad_left") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (token == "dpad_right") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  return 0;
}

// "lb+rb+start" -> button mask; 0 when empty or no token parses (chord off).
uint16_t ChordMaskFromSpec(std::string_view spec) {
  uint16_t mask = 0;
  std::string token;
  for (size_t i = 0; i <= spec.size(); ++i) {
    char c = i < spec.size() ? spec[i] : '+';
    if (c == '+' || c == ',' || c == ' ') {
      if (!token.empty()) {
        mask |= ChordButtonFromToken(token);
        token.clear();
      }
      continue;
    }
    token.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  return mask;
}

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  if (window_) {
    rex::ui::UIEvent closing_event(window_);
    for (auto& driver : drivers_) {
      if (auto* window_listener = dynamic_cast<rex::ui::WindowListener*>(driver.get())) {
        window_listener->OnClosing(closing_event);
      }
    }
    window_ = nullptr;
  }
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  active_callback_ = callback;
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetMenuChordCallback(std::function<void()> callback) {
  menu_chord_callback_ = std::move(callback);
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const uint16_t menu_chord_buttons = ChordMaskFromSpec(REXCVAR_GET(menu_chord));
  const bool menu_chord_down =
      menu_chord_buttons != 0 &&
      (static_cast<uint16_t>(merged.gamepad.buttons) & menu_chord_buttons) == menu_chord_buttons;
  if (menu_chord_down && !menu_chord_down_ && menu_chord_callback_) {
    menu_chord_callback_();
  }
  menu_chord_down_ = menu_chord_down;

  if (active_callback_ && !active_callback_()) {
    std::memset(&merged.gamepad, 0, sizeof(merged.gamepad));
  }

  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

bool InputSystem::GetUiGamepadState(X_INPUT_GAMEPAD* out_gamepad) {
  X_INPUT_GAMEPAD merged = {};
  bool any = false;

  auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
    return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
  };

  // Every device of every driver, regardless of guest user assignment.
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (const DeviceInfo& info : enumerated) {
      X_INPUT_STATE state = {};
      if (driver->GetDeviceStateUi(info.id, &state) != X_ERROR_SUCCESS) {
        continue;
      }
      any = true;
      merged.buttons = static_cast<uint16_t>(merged.buttons) |
                       static_cast<uint16_t>(state.gamepad.buttons);
      merged.left_trigger = std::max(merged.left_trigger, state.gamepad.left_trigger);
      merged.right_trigger = std::max(merged.right_trigger, state.gamepad.right_trigger);
      merged.thumb_lx = merge_axis(merged.thumb_lx, state.gamepad.thumb_lx);
      merged.thumb_ly = merge_axis(merged.thumb_ly, state.gamepad.thumb_ly);
      merged.thumb_rx = merge_axis(merged.thumb_rx, state.gamepad.thumb_rx);
      merged.thumb_ry = merge_axis(merged.thumb_ry, state.gamepad.thumb_ry);
    }
  }

  if (out_gamepad) {
    *out_gamepad = merged;
  }
  return any;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_INPUT_VIBRATION filtered_vibration = {};
  if (REXCVAR_GET(hid_rumble_enabled)) {
    const uint32_t min_motor_speed = REXCVAR_GET(hid_rumble_min_motor_speed);
    const auto filter_motor = [min_motor_speed](uint16_t speed) -> uint16_t {
      return speed >= min_motor_speed ? speed : 0;
    };
    filtered_vibration.left_motor_speed =
        filter_motor(static_cast<uint16_t>(vibration->left_motor_speed));
    filtered_vibration.right_motor_speed =
        filter_motor(static_cast<uint16_t>(vibration->right_motor_speed));
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, &filtered_vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
