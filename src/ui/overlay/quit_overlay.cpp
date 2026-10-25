/**
 * @file        ui/overlay/quit_overlay.cpp
 *
 * @brief       "Quit the game?" confirmation dialog. See quit_overlay.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/quit_overlay.h>

#include <utility>

#include <imgui.h>

namespace rex::ui {

namespace {

// X_INPUT_GAMEPAD_* bits, kept local so the UI layer does not depend on input.
constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr int16_t kStickThreshold = 16000;

}  // namespace

QuitDialog::QuitDialog(ImGuiDrawer* imgui_drawer, PollGamepadCallback poll_gamepad,
                       ActionCallback on_quit, ActionCallback on_cancel)
    : ImGuiDialog(imgui_drawer),
      poll_gamepad_(std::move(poll_gamepad)),
      on_quit_(std::move(on_quit)),
      on_cancel_(std::move(on_cancel)) {}

void QuitDialog::OnDraw(ImGuiIO& io) {
  if (acted_) {
    return;
  }

  // Gamepad: edge-triggered, so a held button or stick moves only once.
  bool confirm = false;
  bool cancel = false;
  int move = 0;
  if (poll_gamepad_) {
    Gamepad pad = poll_gamepad_();
    uint16_t buttons = pad.connected ? pad.buttons : 0;
    uint16_t pressed = buttons & ~previous_buttons_;
    previous_buttons_ = buttons;
    if (pressed & (kPadDpadUp | kPadDpadLeft)) {
      move = -1;
    } else if (pressed & (kPadDpadDown | kPadDpadRight)) {
      move = 1;
    }
    int stick_direction = 0;
    if (pad.connected && pad.thumb_ly > kStickThreshold) {
      stick_direction = -1;
    } else if (pad.connected && pad.thumb_ly < -kStickThreshold) {
      stick_direction = 1;
    }
    if (stick_direction != 0 && stick_direction != previous_stick_direction_) {
      move = stick_direction;
    }
    previous_stick_direction_ = stick_direction;
    confirm |= (pressed & kPadA) != 0;
    cancel |= (pressed & kPadB) != 0;
  }

  // Keyboard, once the opening key press has been consumed.
  if (++frames_drawn_ > 2) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
      move = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
               ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
      move = 1;
    }
    confirm |= ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Space, false);
    cancel |= ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  }

  if (move != 0) {
    selection_ = selection_ == kCancel ? kQuit : kCancel;
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushFont(nullptr, 26.0f);
  if (ImGui::Begin("##quit_dialog", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Quit the game?");
    ImGui::PushFont(nullptr, 18.0f);
    ImGui::TextDisabled("Unsaved progress is lost.");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const ImVec2 button_size(180.0f, 0.0f);
    const char* labels[] = {"Cancel", "Quit"};
    for (int i = kCancel; i <= kQuit; ++i) {
      if (i != kCancel) {
        ImGui::SameLine();
      }
      // The selection must read at a glance from the couch: a bright, outlined
      // button against a dim one.
      const bool selected = selection_ == i;
      const ImVec4 fill = selected ? ImVec4(0.20f, 0.62f, 0.25f, 1.0f)
                                   : ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, fill);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
      ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                    : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, selected ? 1.0f : 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
      if (ImGui::Button(labels[i], button_size)) {
        selection_ = i;
        confirm = true;
      }
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(4);
    }
    ImGui::PushFont(nullptr, 16.0f);
    ImGui::TextDisabled("A / Enter: select    B / Esc: back");
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopFont();

  if (cancel || (confirm && selection_ == kCancel)) {
    acted_ = true;
    if (on_cancel_) {
      on_cancel_();
    }
  } else if (confirm && selection_ == kQuit) {
    acted_ = true;
    if (on_quit_) {
      on_quit_();
    }
  }
}

}  // namespace rex::ui
