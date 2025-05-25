#include "imgui_impl_blur.h"
#ifndef IMGUI_DISABLE
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3dcompiler")
#endif

struct ImGui_ImplBlur_Data
{
    ID3D11Texture2D* screen_texture;
    ID3D11ShaderResourceView* screen_srv;
    ID3D11Texture2D* blur_texture;
	ID3D11RenderTargetView* blur_rtv;
    ID3D11VertexShader* vertex_shader;
	ID3D11PixelShader* pixel_shader;
    ID3D11InputLayout* input_layout;
    ID3D11SamplerState* sampler_state;
    ID3D11Buffer* quad_buffer;
	ID3D11Buffer* buffer;

    struct Uniforms {
        float half_pixel[2];
        float offset;
        float _padding;
    } uniforms;

    int iterations = 0;

    ImGui_ImplBlur_Data() { memset((void*)this, 0, sizeof(*this)); }
};

static ImGui_ImplBlur_Data* ImGui_ImplBlur_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplBlur_Data*)ImGui::GetIO().BackendPProcessUserData : nullptr;
}

static ID3DBlob* ImGui_ImplBlur_CompileShader(const char* src, const char* entry, const char* target, ID3D11Device* device)
{
	ID3DBlob* blob = nullptr;
	ID3DBlob* error_blob = nullptr;
	if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &error_blob)))
	{
        if (error_blob)
        {
            ImGui::ErrorLog((const char*)error_blob->GetBufferPointer());
            IM_ASSERT(0 && "Shader compilation error");
            error_blob->Release();
        }
        else
        {
            IM_ASSERT(false && "Unknown shader compilation error");
        }
        return nullptr;
	}
	return blob;
}

static void ImGui_ImplBlur_CreateShaders(ID3D11Device* device)
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    static const char* vertex_shader = R"(
        struct VSInput {
            float2 pos : POSITION;
            float2 uv : TEXCOORD;
        };

        struct VSOutput {
            float4 pos : SV_POSITION;
            float2 uv : TEXCOORD;
        };

        VSOutput main(VSInput input) {
            VSOutput output;
            output.pos = float4(input.pos, 0.0, 1.0);
            output.uv = input.uv;
            return output;
        }
    )";

    static const char* kawase_upsample = R"(
        Texture2D input_texture : register(t0);
        SamplerState sampler_linear : register(s0);

        cbuffer uniforms : register(b0) {
            float2 half_pixel;
            float offset;
        }

        float2 hash22(float2 p) {
            float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
            p3 += dot(p3, p3.yzx + 33.33);
            return frac((p3.xx + p3.yz) * p3.zy);
        }

        float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
            float2 scatter = offset + hash22(pos.xy) * offset * 2.0;

            float4 sum = input_texture.Sample(sampler_linear, uv) * 4.0;
            sum += input_texture.Sample(sampler_linear, uv - float2(half_pixel.x, half_pixel.y) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(half_pixel.x, half_pixel.y) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(half_pixel.x, -half_pixel.y) * scatter);
            sum += input_texture.Sample(sampler_linear, uv - float2(half_pixel.x, -half_pixel.y) * scatter);

            sum += input_texture.Sample(sampler_linear, uv + float2(-half_pixel.x * 2.0, 0.0) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(-half_pixel.x, half_pixel.y) * scatter) * 2.0;
            sum += input_texture.Sample(sampler_linear, uv + float2(0.0, half_pixel.y * 2.0) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(half_pixel.x, half_pixel.y) * scatter) * 2.0;
            sum += input_texture.Sample(sampler_linear, uv + float2(half_pixel.x * 2.0, 0.0) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(half_pixel.x, -half_pixel.y) * scatter) * 2.0;
            sum += input_texture.Sample(sampler_linear, uv + float2(0.0, -half_pixel.y * 2.0) * scatter);
            sum += input_texture.Sample(sampler_linear, uv + float2(-half_pixel.x, -half_pixel.y) * scatter) * 2.0;

            return sum / 20.0;
        }
    )";

    ID3DBlob* vertex_shader_blob = ImGui_ImplBlur_CompileShader(vertex_shader, "main", "vs_5_0", device);
	ID3DBlob* pixel_blob = ImGui_ImplBlur_CompileShader(kawase_upsample, "main", "ps_5_0", device);
	IM_ASSERT(vertex_shader_blob != nullptr && pixel_blob != nullptr && "Failed to compile shaders!");
    
    device->CreateVertexShader(vertex_shader_blob->GetBufferPointer(), vertex_shader_blob->GetBufferSize(), nullptr, &bd->vertex_shader);
    device->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr, &bd->pixel_shader);

    D3D11_INPUT_ELEMENT_DESC layout_desc[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout_desc, 2, vertex_shader_blob->GetBufferPointer(), vertex_shader_blob->GetBufferSize(), &bd->input_layout);

	D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampler_desc, &bd->sampler_state);

	struct QuadVertex
	{
		float position[2];
		float uv[2];
	};

    QuadVertex vertices[] =
    {
        { -1, -1, 0, 1 }, { -1,  1, 0, 0 }, {  1, -1, 1, 1 },
        {  1, -1, 1, 1 }, { -1,  1, 0, 0 }, {  1,  1, 1, 0 }
    };

	D3D11_BUFFER_DESC buffer_desc = {};
	buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
	buffer_desc.ByteWidth = sizeof(vertices);
	buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

	D3D11_SUBRESOURCE_DATA buffer_data = {};
    buffer_data.pSysMem = vertices;

	device->CreateBuffer(&buffer_desc, &buffer_data, &bd->quad_buffer);

    D3D11_BUFFER_DESC pixel_buffer_desc = {};
    pixel_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    pixel_buffer_desc.ByteWidth = sizeof(ImGui_ImplBlur_Data::Uniforms);
    pixel_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    pixel_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    device->CreateBuffer(&pixel_buffer_desc, nullptr, & bd->buffer);

    if (vertex_shader_blob) vertex_shader_blob->Release();
	if (pixel_blob) pixel_blob->Release();
}

static void ImGui_ImplBlur_DestroyShaders()
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    if (bd->vertex_shader) bd->vertex_shader->Release();
	if (bd->pixel_shader) bd->pixel_shader->Release();
	if (bd->input_layout) bd->input_layout->Release();
    if (bd->quad_buffer) { bd->quad_buffer->Release(); }
    if (bd->sampler_state) { bd->sampler_state->Release(); }
    if (bd->blur_rtv) { bd->blur_rtv->Release(); }
    if (bd->blur_texture) { bd->blur_texture->Release(); }
    if (bd->buffer) { bd->buffer->Release(); }
}

static void ImGui_ImplBlur_Begin(const ImDrawList* draw_list, const ImDrawCmd* draw_cmd)
{
	ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    ImGui_ImplDX11_RenderState* render_state = (ImGui_ImplDX11_RenderState*)platform_io.Renderer_RenderState;
    ID3D11DeviceContext* device_context = render_state->DeviceContext;

    ID3D11RenderTargetView* render_target = nullptr;
    device_context->OMGetRenderTargets(1, &render_target, nullptr);
    if (!render_target) return;

    ID3D11Resource* rtv_resource = nullptr;
    render_target->GetResource(&rtv_resource);
    render_target->Release();
    if (!rtv_resource) return;

    ID3D11Texture2D* back_buffer = nullptr;
    rtv_resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&back_buffer);
    rtv_resource->Release();
    if (!back_buffer) return;

    device_context->CopyResource(bd->screen_texture, back_buffer);
    back_buffer->Release();
}

static void ImGui_ImplBlur_End(const ImDrawList* draw_list, const ImDrawCmd* draw_cmd)
{
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    
    ImGui_ImplDX11_RenderState* render_state = (ImGui_ImplDX11_RenderState*)platform_io.Renderer_RenderState;
    ID3D11DeviceContext* device_context = render_state->DeviceContext;

    device_context->VSSetShader(nullptr, nullptr, 0);
    device_context->PSSetShader(nullptr, nullptr, 0);
}

static void ImGui_ImplBlur_Pass(const ImDrawList* draw_list, const ImDrawCmd* draw_cmd)
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    ImGui_ImplDX11_RenderState* render_state = (ImGui_ImplDX11_RenderState*)platform_io.Renderer_RenderState;
    ID3D11DeviceContext* device_context = render_state->DeviceContext;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(device_context->Map(bd->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &bd->uniforms, sizeof(bd->uniforms));
        device_context->Unmap(bd->buffer, 0);
    }

    static float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11RenderTargetView* prev_rtv = nullptr;
    ID3D11DepthStencilView* prev_dsv = nullptr;
    device_context->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);
    device_context->OMSetRenderTargets(1, &bd->blur_rtv, nullptr);
    device_context->ClearRenderTargetView(bd->blur_rtv, clear_color);
    device_context->IASetInputLayout(bd->input_layout);

    UINT stride = sizeof(float) * 4, offset = 0;
    device_context->IASetVertexBuffers(0, 1, &bd->quad_buffer, &stride, &offset);
    device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    device_context->VSSetShader(bd->vertex_shader, nullptr, 0);
    device_context->PSSetShader(bd->pixel_shader, nullptr, 0);
    device_context->PSSetShaderResources(0, 1, &bd->screen_srv);
    device_context->PSSetSamplers(0, 1, &bd->sampler_state);
    device_context->PSSetConstantBuffers(0, 1, &bd->buffer);

    for (int i = 0; i < bd->iterations; ++i)
    {
        device_context->Draw(6, 0);
        device_context->CopyResource(bd->screen_texture, bd->blur_texture);
    }

    device_context->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();
}

void ImGui_ImplBlur_Init(IDXGISwapChain* swap_chain)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPProcessUserData == nullptr && "Already initialized a post processing backend!");

    ImGui_ImplBlur_Data* bd = IM_NEW(ImGui_ImplBlur_Data)();
    io.BackendPProcessUserData = (void*)bd;

    ID3D11Device* device = nullptr;
    swap_chain->GetDevice(__uuidof(ID3D11Device), (void**)&device);

    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (!back_buffer) return;

    D3D11_TEXTURE2D_DESC texture_desc;
    back_buffer->GetDesc(&texture_desc);
    back_buffer->Release();

    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;

    device->CreateTexture2D(&texture_desc, nullptr, &bd->screen_texture);
    device->CreateShaderResourceView(bd->screen_texture, nullptr, &bd->screen_srv);
    device->CreateTexture2D(&texture_desc, nullptr, &bd->blur_texture);
    device->CreateRenderTargetView(bd->blur_texture, nullptr, &bd->blur_rtv);

    ImGui_ImplBlur_CreateShaders(device);
    device->Release();
}

void ImGui_ImplBlur_Shutdown()
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplBlur_DestroyShaders();
    if (bd->screen_texture) { bd->screen_texture->Release(); }
    if (bd->screen_srv) { bd->screen_srv->Release(); }

    io.BackendPProcessUserData = nullptr;
    IM_DELETE(bd);
}

void ImGui_ImplBlur_Apply(ImDrawList* draw_list, int iterations, float offset)
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplBlur_Init()?");

    ImGuiIO& io = ImGui::GetIO();
    bd->uniforms = { 1.0f / io.DisplaySize.x, 1.0f / io.DisplaySize.y, offset, 0 };
    bd->iterations = iterations;

    draw_list->AddDrawCmd();
    draw_list->AddCallback(ImGui_ImplBlur_Begin, nullptr);
    draw_list->AddCallback(ImGui_ImplBlur_Pass, nullptr);
    draw_list->AddCallback(ImGui_ImplBlur_End, nullptr);

    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    draw_list->AddImage((ImTextureID)bd->screen_srv, { 0.0f, 0.0f }, io.DisplaySize);
}

IMGUI_API void ImGui_ImplBlur_Rect(ImVec2 min, ImVec2 max, ImDrawList* draw_list, int iterations, float offset, float rounding, ImDrawFlags draw_flags)
{
    ImGui_ImplBlur_Data* bd = ImGui_ImplBlur_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplBlur_Init()?");

    ImGuiIO& io = ImGui::GetIO();
    bd->uniforms = { 1.0f / io.DisplaySize.x, 1.0f / io.DisplaySize.y, offset, 0 };
    bd->iterations = iterations;

    draw_list->AddDrawCmd();
    draw_list->AddCallback(ImGui_ImplBlur_Begin, nullptr);
    draw_list->AddCallback(ImGui_ImplBlur_Pass, nullptr);
    draw_list->AddCallback(ImGui_ImplBlur_End, nullptr);

    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    draw_list->AddImageRounded((ImTextureID)bd->screen_srv, min, max, { min.x / io.DisplaySize.x, min.y / io.DisplaySize.y }, { max.x / io.DisplaySize.x, max.y / io.DisplaySize.y }, IM_COL32_WHITE, rounding, draw_flags);
}

//-----------------------------------------------------------------------------

#endif // #ifndef IMGUI_DISABLE
