#pragma once

#include "imgui.h"      // IMGUI_API
#ifndef IMGUI_DISABLE

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

IMGUI_API void     ImGui_ImplBlur_Init(IDXGISwapChain* swap_chain);
IMGUI_API void     ImGui_ImplBlur_Shutdown();

IMGUI_API void     ImGui_ImplBlur_Apply(ImDrawList* draw_list, int iterations, float offset);
IMGUI_API void     ImGui_ImplBlur_Rect(ImVec2 min, ImVec2 max, ImDrawList* draw_list, int iterations, float offset, float rounding = 0.0f, ImDrawFlags draw_flags = 0);

#endif // #ifndef IMGUI_DISABLE
