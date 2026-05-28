/**
 * @file        ui/overlay/simple_settings_overlay.cpp
 *
 * @brief       Curated user-facing settings overlay.
 */
#include <rex/ui/overlay/simple_settings_overlay.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <toml++/toml.hpp>

namespace rex::ui {
namespace {

constexpr std::array<int32_t, 3> kResolutionScales = {1, 2, 3};
constexpr std::array<const char*, 3> kResolutionLabels = {"720p", "1440p", "2160p"};
constexpr std::array<double, 6> kFrameCapRates = {60.0, 90.0, 120.0, 144.0, 165.0, 240.0};
constexpr std::array<const char*, 7> kFrameCapLabels = {"Unlimited", "60 FPS", "90 FPS",
                                                        "120 FPS", "144 FPS", "165 FPS",
                                                        "240 FPS"};
constexpr std::array<std::string_view, 10> kSimpleSettingsCvars = {
    "resolution_scale",
    "draw_resolution_scale_x",
    "draw_resolution_scale_y",
    "d3d12_present_frame_limiter",
    "d3d12_present_frame_limiter_fps",
    "fullscreen",
    "vsync",
    "d3d12_allow_variable_refresh_rate_and_tearing",
    "mnk_mode",
    "mnk_capture_mouse"};

int ResolutionIndexFromCvar() {
  int32_t current = rex::cvar::Query<int32_t>("resolution_scale");
  int best = 0;
  int32_t best_delta = 1000;
  for (int i = 0; i < static_cast<int>(kResolutionScales.size()); ++i) {
    int32_t delta = kResolutionScales[i] > current ? kResolutionScales[i] - current
                                                   : current - kResolutionScales[i];
    if (delta < best_delta) {
      best = i;
      best_delta = delta;
    }
  }
  return best;
}

int FrameCapIndexFromCvar() {
  if (!rex::cvar::Query<bool>("d3d12_present_frame_limiter")) {
    return 0;
  }
  double current = rex::cvar::Query<double>("d3d12_present_frame_limiter_fps");
  int best = 1;
  double best_delta = 1000.0;
  for (int i = 0; i < static_cast<int>(kFrameCapRates.size()); ++i) {
    double delta = std::abs(kFrameCapRates[i] - current);
    if (delta < best_delta) {
      best = i + 1;
      best_delta = delta;
    }
  }
  return best;
}

void CopyToBuffer(char* buffer, size_t buffer_size, const std::string& value) {
  size_t count = std::min(value.size(), buffer_size - 1);
  std::copy_n(value.data(), count, buffer);
  buffer[count] = '\0';
}

void SetBoolCvar(std::string_view name, bool value) {
  rex::cvar::SetFlagByName(name, value ? "true" : "false");
}

void SectionTitle(const char* label) {
  ImGui::PushFont(nullptr, 20.0f);
  ImGui::TextUnformatted(label);
  ImGui::PopFont();
  ImGui::Spacing();
}

void BeginFieldRow(const char* label) {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine(260.0f);
  ImGui::SetNextItemWidth(-1.0f);
}

void EndFieldRow() {
  ImGui::PopID();
  ImGui::Spacing();
}

bool PrimaryButton(const char* label) {
  return ImGui::Button(label, ImVec2(170.0f, 42.0f));
}

}  // namespace

void SaveSimpleSettingsConfig(const std::filesystem::path& config_path) {
  rex::cvar::SaveConfigValues(config_path,
                              {"resolution_scale",
                               "draw_resolution_scale_x",
                               "draw_resolution_scale_y",
                               "d3d12_present_frame_limiter",
                               "d3d12_present_frame_limiter_fps",
                               "fullscreen",
                               "vsync",
                               "d3d12_allow_variable_refresh_rate_and_tearing",
                               "mnk_mode",
                               "mnk_capture_mouse"});
}

void EnsureSimpleSettingsConfig(const std::filesystem::path& config_path) {
  bool should_save = !std::filesystem::exists(config_path);
  if (!should_save) {
    try {
      auto config = toml::parse_file(config_path.string());
      for (std::string_view name : kSimpleSettingsCvars) {
        if (!config.contains(name)) {
          should_save = true;
          break;
        }
      }
    } catch (const toml::parse_error&) {
      should_save = true;
    }
  }

  if (should_save) {
    SaveSimpleSettingsConfig(config_path);
  }
}

SimpleSettingsDialog::SimpleSettingsDialog(ImGuiDrawer* drawer, std::filesystem::path config_path,
                                           LoadProfilesCallback load_profiles,
                                           SaveProfileCallback save_profile,
                                           CloseSettingsCallback close_settings,
                                           CloseGameCallback close_game,
                                           RestartGameCallback restart_game)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      load_profiles_(std::move(load_profiles)),
      save_profile_(std::move(save_profile)),
      close_settings_(std::move(close_settings)),
      close_game_(std::move(close_game)),
      restart_game_(std::move(restart_game)) {
  ReloadProfiles();
  LoadSettingsFromCvars();
}

SimpleSettingsDialog::~SimpleSettingsDialog() = default;

void SimpleSettingsDialog::Show() {
  visible_ = true;
  ReloadProfiles();
  LoadSettingsFromCvars();
}

void SimpleSettingsDialog::LoadSettingsFromCvars() {
  resolution_scale_index_ = ResolutionIndexFromCvar();
  frame_cap_index_ = FrameCapIndexFromCvar();
  fullscreen_ = rex::cvar::Query<bool>("fullscreen");
  vsync_ = rex::cvar::Query<bool>("vsync");
  tearing_ = rex::cvar::Query<bool>("d3d12_allow_variable_refresh_rate_and_tearing");
  mnk_mode_ = rex::cvar::Query<bool>("mnk_mode");
  mnk_capture_mouse_ = rex::cvar::Query<bool>("mnk_capture_mouse");
}

bool SimpleSettingsDialog::HasSettingsChanges() const {
  return resolution_scale_index_ != ResolutionIndexFromCvar() ||
         frame_cap_index_ != FrameCapIndexFromCvar() ||
         fullscreen_ != rex::cvar::Query<bool>("fullscreen") ||
         vsync_ != rex::cvar::Query<bool>("vsync") ||
         tearing_ != rex::cvar::Query<bool>("d3d12_allow_variable_refresh_rate_and_tearing") ||
         mnk_mode_ != rex::cvar::Query<bool>("mnk_mode") ||
         mnk_capture_mouse_ != rex::cvar::Query<bool>("mnk_capture_mouse");
}

void SimpleSettingsDialog::Toggle() {
  if (visible_) {
    Hide();
    return;
  }
  Show();
}

void SimpleSettingsDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  if (close_settings_) {
    close_settings_();
  }
}

void SimpleSettingsDialog::ReloadProfiles() {
  profiles_ = load_profiles_ ? load_profiles_() : SimpleProfileState{};
  if (profiles_.profiles.empty()) {
    profiles_.profiles.push_back({"default", "Player"});
    profiles_.selected_index = 0;
  }
  profiles_.selected_index =
      std::clamp(profiles_.selected_index, 0, static_cast<int>(profiles_.profiles.size()) - 1);
  CopyToBuffer(gamertag_buf_, sizeof(gamertag_buf_),
               profiles_.profiles[profiles_.selected_index].gamertag);
  profile_signed_in_ = profiles_.profiles[profiles_.selected_index].signed_in;
}

void SimpleSettingsDialog::SaveVideo() {
  resolution_scale_index_ =
      std::clamp(resolution_scale_index_, 0, static_cast<int>(kResolutionScales.size()) - 1);
  frame_cap_index_ =
      std::clamp(frame_cap_index_, 0, static_cast<int>(kFrameCapLabels.size()) - 1);
  const auto scale = std::to_string(kResolutionScales[resolution_scale_index_]);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  SetBoolCvar("d3d12_present_frame_limiter", frame_cap_index_ != 0);
  if (frame_cap_index_ != 0) {
    rex::cvar::SetFlagByName("d3d12_present_frame_limiter_fps",
                             std::to_string(kFrameCapRates[frame_cap_index_ - 1]));
  }
  SetBoolCvar("fullscreen", fullscreen_);
  SetBoolCvar("vsync", vsync_);
  SetBoolCvar("d3d12_allow_variable_refresh_rate_and_tearing", tearing_);
  SetBoolCvar("mnk_mode", mnk_mode_);
  SetBoolCvar("mnk_capture_mouse", mnk_capture_mouse_);
  SaveSimpleSettingsConfig(config_path_);
}

void SimpleSettingsDialog::SaveProfile() {
  if (save_profile_) {
    save_profile_(profiles_.selected_index, gamertag_buf_, profile_signed_in_);
  }
  ReloadProfiles();
}

void SimpleSettingsDialog::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    return;
  }

  const float window_width =
      std::max(420.0f, std::min(820.0f, std::max(0.0f, io.DisplaySize.x - 48.0f)));
  const float window_height =
      std::max(560.0f, std::min(900.0f, std::max(0.0f, io.DisplaySize.y - 16.0f)));

  ImGui::PushFont(nullptr, 18.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.055f, 0.065f, 0.96f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.070f, 0.082f, 0.095f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.32f, 0.38f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.060f, 0.070f, 0.080f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.075f, 0.095f, 0.110f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.17f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.22f, 0.25f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.22f, 0.28f, 0.31f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.38f, 0.42f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.49f, 0.54f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.32f, 0.36f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.38f, 0.42f, 0.72f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.15f, 0.49f, 0.54f, 0.88f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.17f, 0.56f, 0.62f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.52f, 0.88f, 0.82f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.52f, 0.88f, 0.82f, 0.86f));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.66f, 1.00f, 0.92f, 1.00f));

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(window_width, window_height), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.96f);
  if (!ImGui::Begin("Game Settings", nullptr,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
    ImGui::End();
    ImGui::PopStyleColor(17);
    ImGui::PopStyleVar(7);
    ImGui::PopFont();
    return;
  }

  ImGui::PushFont(nullptr, 22.0f);
  ImGui::TextUnformatted("Settings");
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::Spacing();

  if (ImGui::Button("Close Settings", ImVec2(170.0f, 40.0f))) {
    Hide();
    ImGui::End();
    ImGui::PopStyleColor(17);
    ImGui::PopStyleVar(7);
    ImGui::PopFont();
    return;
  }
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.12f, 0.10f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.16f, 0.13f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.38f, 0.08f, 0.07f, 1.00f));
  if (ImGui::Button("Close Game", ImVec2(170.0f, 40.0f))) {
    if (close_game_) {
      close_game_();
    }
  }
  ImGui::PopStyleColor(3);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::BeginChild("##simple_settings_nav", ImVec2(180.0f, 0.0f), true);
  const char* tabs[] = {"Video", "Controls", "Profile"};
  for (int i = 0; i < 3; ++i) {
    if (ImGui::Selectable(tabs[i], selected_tab_ == i, 0, ImVec2(0.0f, 42.0f))) {
      selected_tab_ = i;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##simple_settings_body", ImVec2(0.0f, 0.0f), true,
                    ImGuiWindowFlags_NoScrollbar);
  const bool has_settings_changes = HasSettingsChanges();
  if (selected_tab_ == 0) {
    SectionTitle("Video");

    BeginFieldRow("Resolution scale");
    ImGui::Combo("##resolution_scale", &resolution_scale_index_, kResolutionLabels.data(),
                 static_cast<int>(kResolutionLabels.size()));
    EndFieldRow();

    BeginFieldRow("Framerate Cap");
    ImGui::Combo("##frame_cap", &frame_cap_index_, kFrameCapLabels.data(),
                 static_cast<int>(kFrameCapLabels.size()));
    EndFieldRow();

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Checkbox("Fullscreen", &fullscreen_);
    ImGui::Checkbox("VSync", &vsync_);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.28f, 0.24f, 1.0f), "not recommended");
    ImGui::Checkbox("Variable refresh / tearing", &tearing_);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (has_settings_changes) {
      ImGui::TextColored(ImVec4(0.95f, 0.76f, 0.30f, 1.0f),
                         "Restart required to apply these changes.");
      if (PrimaryButton("Apply & Restart")) {
        SaveVideo();
        if (restart_game_) {
          restart_game_();
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel Changes", ImVec2(170.0f, 42.0f))) {
        LoadSettingsFromCvars();
      }
    } else {
      ImGui::TextDisabled("No pending changes");
    }
  } else if (selected_tab_ == 1) {
    SectionTitle("Controls");

    ImGui::Checkbox("Mouse and keyboard mode", &mnk_mode_);
    ImGui::Checkbox("Capture mouse", &mnk_capture_mouse_);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (has_settings_changes) {
      ImGui::TextColored(ImVec4(0.95f, 0.76f, 0.30f, 1.0f),
                         "Restart required to apply these changes.");
      if (PrimaryButton("Apply & Restart")) {
        SaveVideo();
        if (restart_game_) {
          restart_game_();
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel Changes", ImVec2(170.0f, 42.0f))) {
        LoadSettingsFromCvars();
      }
    } else {
      ImGui::TextDisabled("No pending changes");
    }
  } else {
    SectionTitle("Profile");

    std::vector<const char*> names;
    names.reserve(profiles_.profiles.size());
    for (const auto& profile : profiles_.profiles) {
      names.push_back(profile.gamertag.c_str());
    }

    BeginFieldRow("Local profile");
    if (ImGui::Combo("##profile", &profiles_.selected_index, names.data(),
                     static_cast<int>(names.size()))) {
      CopyToBuffer(gamertag_buf_, sizeof(gamertag_buf_),
                   profiles_.profiles[profiles_.selected_index].gamertag);
      profile_signed_in_ = profiles_.profiles[profiles_.selected_index].signed_in;
    }
    EndFieldRow();

    BeginFieldRow("Gamertag");
    ImGui::InputText("##gamertag", gamertag_buf_, sizeof(gamertag_buf_));
    EndFieldRow();

    BeginFieldRow("Local sign-in");
    const char* sign_in_labels[] = {"Signed out", "Signed in"};
    int sign_in_index = profile_signed_in_ ? 1 : 0;
    if (ImGui::Combo("##signed_in", &sign_in_index, sign_in_labels, 2)) {
      profile_signed_in_ = sign_in_index != 0;
    }
    EndFieldRow();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (PrimaryButton("Save Profile")) {
      SaveProfile();
    }
  }
  ImGui::EndChild();

  ImGui::End();
  ImGui::PopStyleColor(17);
  ImGui::PopStyleVar(7);
  ImGui::PopFont();
}

}  // namespace rex::ui
