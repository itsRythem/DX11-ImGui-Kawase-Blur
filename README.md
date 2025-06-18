# DX11-ImGui-Kawase-Blur
Efficient single pass kawase blur implementation for dx11 imgui

### Blur Tinting (`imgui_impl_blur.cpp`, line 389)

To tint the blur, define `ImGuiCol_Blur` in your style and use:

```cpp
ImGui::GetColorU32(ImGuiCol_Blur)
```

Otherwise, replace it with:

```cpp
IM_COL32_WHITE
```

to disable tinting.
