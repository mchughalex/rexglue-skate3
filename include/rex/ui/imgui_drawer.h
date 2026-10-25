/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#ifndef REX_UI_IMGUI_DRAWER_H_
#define REX_UI_IMGUI_DRAWER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/style.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace rex {
namespace ui {

class ImGuiDialog;
class Window;

class ImGuiDrawer : public WindowInputListener, public UIDrawer {
 public:
  using FontSetupCallback = std::function<void(ImFontAtlas*)>;
  using StyleSetupCallback = std::function<void(ImGuiStyle&, Style&)>;
  ImGuiDrawer(Window* window, size_t z_order, FontSetupCallback font_setup = nullptr,
              StyleSetupCallback style_setup = nullptr);
  ~ImGuiDrawer();

  ImGuiIO& GetIO();
  bool HasDialogs() const { return !dialogs_.empty(); }

  // Proportional system UI font for styled overlays (and a heavier weight for
  // headings), loaded in SetupFonts from per-platform system font paths.
  // Nullptr when no suitable system font was found - callers fall back to the
  // default font (PushFont(nullptr, size)).
  ImFont* ui_font() const { return ui_font_; }
  ImFont* ui_font_semibold() const { return ui_font_semibold_; }
  ImFont* ui_font_bold() const { return ui_font_bold_; }
  // Coverage-thinned variants for DARK text on LIGHT backgrounds (see
  // SetupFonts); same metrics as the plain fonts.
  ImFont* ui_font_on_light() const { return ui_font_on_light_; }
  ImFont* ui_font_semibold_on_light() const { return ui_font_semibold_on_light_; }

  // Per-overlay styling, patched by the consumer in OnConfigureStyle.
  Style& style() { return style_; }
  const Style& style() const { return style_; }

  void AddDialog(ImGuiDialog* dialog);
  void RemoveDialog(ImGuiDialog* dialog);

  Presenter* presenter() const { return presenter_; }

  // Whether Draw would render anything, so a caller that has to marshal to the
  // UI thread can skip the round trip entirely.
  // SetPresenter may be called from the destructor.
  void SetPresenter(Presenter* new_presenter);
  void SetImmediateDrawer(ImmediateDrawer* new_immediate_drawer);
  void SetPresenterAndImmediateDrawer(Presenter* new_presenter,
                                      ImmediateDrawer* new_immediate_drawer) {
    SetPresenter(new_presenter);
    SetImmediateDrawer(new_immediate_drawer);
  }

  void Draw(UIDrawContext& ui_draw_context) override;

 protected:
  void OnKeyDown(KeyEvent& e) override;
  void OnKeyUp(KeyEvent& e) override;
  void OnKeyChar(KeyEvent& e) override;
  void OnMouseDown(MouseEvent& e) override;
  void OnMouseMove(MouseEvent& e) override;
  void OnMouseUp(MouseEvent& e) override;
  void OnMouseWheel(MouseEvent& e) override;
  void OnTouchEvent(TouchEvent& e) override;
  // For now, no need for OnDpiChanged because redrawing is done continuously.

 private:
  void Initialize();

  void SetupFontTexture();

  void SetupFonts();

  // Services ImGui's texture create/update/destroy requests when
  // ImGuiBackendFlags_RendererHasTextures is active.
  void ProcessImGuiTextureRequests(ImDrawData* data);

  void RenderDrawLists(ImDrawData* data, UIDrawContext& ui_draw_context);

  void AddDialogImpl(ImGuiDialog* dialog);
  void RemoveDialogImpl(ImGuiDialog* dialog);

  void ClearInput();
  void OnKey(KeyEvent& e, bool is_down);
  static int MouseEventButtonToImGui(const MouseEvent& e);
  void UpdateMousePosition(float x, float y);
  void SwitchToPhysicalMouseAndUpdateMousePosition(const MouseEvent& e);

  bool IsDrawingDialogs() const { return dialog_loop_next_index_ != SIZE_MAX; }
  void DetachIfLastDialogRemoved();

  std::optional<ImGuiKey> VirtualKeyToImGuiKey(VirtualKey vkey);

  Window* window_;
  size_t z_order_;
  FontSetupCallback font_setup_;
  StyleSetupCallback style_setup_;
  Style style_;

  ImGuiContext* internal_state_ = nullptr;

  // System UI fonts (see ui_font()). Owned by the ImGui font atlas.
  ImFont* ui_font_ = nullptr;
  ImFont* ui_font_semibold_ = nullptr;
  ImFont* ui_font_bold_ = nullptr;
  ImFont* ui_font_on_light_ = nullptr;
  ImFont* ui_font_semibold_on_light_ = nullptr;

  // All currently-attached dialogs that get drawn.
  std::vector<ImGuiDialog*> dialogs_;
  // Using an index, not an iterator, because after the erasure, the adjustment
  // must be done for the vector element indices that would be in the iterator
  // range that would be invalidated.
  // SIZE_MAX if not currently in the dialog loop.
  size_t dialog_loop_next_index_ = SIZE_MAX;

  Presenter* presenter_ = nullptr;

  ImmediateDrawer* immediate_drawer_ = nullptr;
  // Resources specific to an immediate drawer - must be destroyed before
  // detaching the presenter.
  std::unique_ptr<ImmediateTexture> font_texture_;
  // Textures created on ImGui's request when ImGuiBackendFlags_RendererHasTextures
  // is active (dynamic glyph rasterization); keyed back to ImGui via TexID.
  std::vector<std::unique_ptr<ImmediateTexture>> imgui_managed_textures_;

  // Bit mask of ImGui mouse buttons the drawer has seen pressed, used for
  // window mouse capture bookkeeping (io.MouseDown can't be used - input is
  // queued via the ImGui event API and only reflected at NewFrame).
  uint32_t mouse_buttons_down_ = 0;

  // If there's an active pointer, the ImGui mouse is controlled by this touch.
  // If it's TouchEvent::kPointerIDNone, the ImGui mouse is controlled by the
  // mouse.
  uint32_t touch_pointer_id_ = TouchEvent::kPointerIDNone;
  // Whether after the next frame (since the mouse up event needs to be handled
  // with the correct mouse position still), the ImGui mouse position should be
  // reset (for instance, after releasing a touch), so it's not hovering over
  // anything.
  bool reset_mouse_position_after_next_frame_ = false;

  double frame_time_tick_frequency_;
  uint64_t last_frame_time_ticks_;

  // ImGui's IME hook, called from EndFrame with the frame being ended.
  // ImGuiIO::WantTextInput would be a frame behind.
  static void PlatformSetImeData(ImGuiContext* context, ImGuiViewport* viewport,
                                 ImGuiPlatformImeData* data);
  void SetWindowTextInputActive(bool active);

  // Last value handed to the window, so detaching can clear it without a frame.
  bool text_input_active_ = false;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_UI_IMGUI_DRAWER_H_
