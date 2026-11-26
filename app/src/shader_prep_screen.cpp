// S4ep2-XBLA-nx (github.com/FurryLilManChild/S4ep2-XBLA-nx, GPL-3.0, 0.5.0; its ca03ac30 and 8aff941b): the "Preparing
// shaders" screen. The work is banjo::renut_gpu::deko3d::PrepareListedVariants (banjo_renut_deko3d.cpp, with
// s4ep2_prepare_shaders_list), run on the game's thread from Direct3D's CreateDevice hook before the game draws; while
// it runs the game has not presented. The dialog asks for no ImGui frame unless the preparation is running (WantsFrame).
// Switch builds only (banjo_renut_deko3d.cpp is); elsewhere RegisterShaderPrepScreen does nothing.
#include "shader_prep_screen.h"

#if defined(__SWITCH__)
#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "renut_engine/gpu/banjo_renut_deko3d.h"

namespace {

class ShaderPrepDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ShaderPrepDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  bool WantsFrame() const override {
    return banjo::renut_gpu::deko3d::ShaderPrep().active.load(std::memory_order_relaxed);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& p = banjo::renut_gpu::deko3d::ShaderPrep();
    if (!p.active.load(std::memory_order_relaxed)) return;
    const unsigned done = p.done.load(), total = std::max(1u, p.total.load()), compiled = p.compiled.load();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("##shaderprep", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoInputs)) {
      ImGui::SetCursorPos(ImVec2(io.DisplaySize.x * 0.1f, io.DisplaySize.y * 0.4f));
      ImGui::BeginGroup();
      ImGui::Text("Preparing shaders: %u of %u", done, total);
      char label[64];
      std::snprintf(label, sizeof(label), "%u%%", done * 100 / total);
      ImGui::ProgressBar(float(done) / float(total), ImVec2(io.DisplaySize.x * 0.8f, 0), label);
      ImGui::TextDisabled("Compiled on this console so far: %u. Later starts skip this.", compiled);
      ImGui::EndGroup();
    }
    ImGui::End();
    ImGui::PopFont();
  }
};

}  // namespace

void RegisterShaderPrepScreen(rex::ui::ImGuiDrawer* drawer) {
  drawer->AddDialog(new ShaderPrepDialog(drawer));
}
#else
void RegisterShaderPrepScreen(rex::ui::ImGuiDrawer*) {}
#endif
