/**
 * @file        rex/ui/overlay/install_wizard_overlay.h
 *
 * @brief       Generic pre-runtime installer dialog.
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class InstallWizardDialog final : public ImGuiDialog {
 public:
  using PickSourceCallback = std::function<std::filesystem::path()>;
  using InstallCallback = std::function<bool(const std::filesystem::path& source,
                                            std::atomic<uint64_t>& copied_bytes,
                                            std::atomic<uint64_t>& total_bytes,
                                            std::string& error)>;
  using CompleteCallback = std::function<void()>;

  InstallWizardDialog(ImGuiDrawer* drawer, std::string title, std::string intro,
                      std::string install_directory, PickSourceCallback pick_source,
                      InstallCallback install, CompleteCallback complete);

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State {
    kWaitingForSource,
    kInstalling,
    kInstalled,
    kFailed,
  };

  void PickSourceAndInstall();
  void StartInstall(std::filesystem::path source_path);
  void FinishInstallIfNeeded();

  std::string title_;
  std::string intro_;
  std::string install_directory_;
  PickSourceCallback pick_source_;
  InstallCallback install_;
  CompleteCallback complete_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  std::atomic<bool> install_ok_{false};
  std::atomic<uint64_t> copied_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
  State state_ = State::kWaitingForSource;
  std::filesystem::path source_path_;
  std::string status_;
  std::string error_;
};

}  // namespace rex::ui
