/**
 * @file        ui/overlay/quit_overlay.h
 *
 * @brief       "Quit the game?" confirmation dialog, usable with a gamepad.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <functional>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// Small centered confirmation that lets the player leave the game from the
// couch: guest input is gated while it is open, so it reads the pad through
// the poll callback (raw UI state), and also takes keyboard and mouse input.
class QuitDialog final : public ImGuiDialog {
 public:
  struct Gamepad {
    bool connected = false;
    uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits
    int16_t thumb_ly = 0;
  };
  using PollGamepadCallback = std::function<Gamepad()>;
  using ActionCallback = std::function<void()>;

  QuitDialog(ImGuiDrawer* imgui_drawer, PollGamepadCallback poll_gamepad, ActionCallback on_quit,
             ActionCallback on_cancel);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum Choice : int { kCancel = 0, kQuit = 1 };

  PollGamepadCallback poll_gamepad_;
  ActionCallback on_quit_;
  ActionCallback on_cancel_;
  int selection_ = kCancel;
  // The buttons that opened the dialog (a chord including Start, or A on a
  // menu) are still held on the first frames; ignore them until released.
  uint16_t previous_buttons_ = 0xFFFF;
  int previous_stick_direction_ = 0;
  // Same for the key that opened it (Escape is queued before the first draw).
  int frames_drawn_ = 0;
  bool acted_ = false;
};

}  // namespace rex::ui
