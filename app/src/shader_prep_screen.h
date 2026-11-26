// S4ep2-XBLA-nx (2026-11-19): the "Preparing shaders" screen (shader_prep_screen.cpp; s4ep2_prepare_shaders_list).
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

// From RenutApp::OnCreateDialogs: a full-screen progress screen, drawn only while the shaders are being prepared.
void RegisterShaderPrepScreen(rex::ui::ImGuiDrawer* drawer);
