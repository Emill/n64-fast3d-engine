#ifdef ENABLE_DX11

#if defined(_WIN32) || defined(_WIN64)

#include <stdio.h>
#include <vector>

#include <windows.h>
#include <wrl/client.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"

#include "gfx_screen_config.h"

#define WINCLASS_NAME L"SUPERMARIO64"
#define DEBUG_D3D 0

using namespace Microsoft::WRL; // For ComPtr

struct PerFrameCB {
    uint32_t frame_count;
    uint32_t window_height;
    uint32_t padding[2];
};

struct TextureData {
    ComPtr<ID3D11ShaderResourceView> resource_view;
    ComPtr<ID3D11SamplerState> sampler_state;
};

struct ShaderProgram {
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11BlendState> blend_state;

    uint32_t shader_id;
    uint8_t num_inputs;
    uint8_t num_floats;
    bool used_textures[2];
};

static struct {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> backbuffer_view;
    ComPtr<ID3D11DepthStencilView> depth_stencil_view;
    ComPtr<ID3D11RasterizerState> rasterizer_state;
    ComPtr<ID3D11DepthStencilState> depth_stencil_state;
    ComPtr<ID3D11Buffer> vertex_buffer;
    ComPtr<ID3D11Buffer> per_frame_cb;

#if DEBUG_D3D
    ComPtr<ID3D11Debug> debug;
#endif

    DXGI_SAMPLE_DESC sample_description;

    PerFrameCB per_frame_cb_data;

    struct ShaderProgram shader_program_pool[64];
    uint8_t shader_program_pool_size;

    std::vector<struct TextureData> textures;
    int current_tile;
    uint32_t current_texture_ids[2];

    // Current state

    struct ShaderProgram *shader_program;
    
    uint32_t current_width, current_height;
    
    int8_t depth_test;
    int8_t depth_mask;
    int8_t zmode_decal;

    // Previous states (to prevent setting states needlessly)

    struct ShaderProgram *last_shader_program = nullptr;
    uint32_t last_vertex_buffer_stride = 0;
    ComPtr<ID3D11BlendState> last_blend_state = nullptr;
    ComPtr<ID3D11ShaderResourceView> last_resource_views[2] = { nullptr, nullptr };
    ComPtr<ID3D11SamplerState> last_sampler_states[2] = { nullptr, nullptr };
    int8_t last_depth_test = -1;
    int8_t last_depth_mask = -1;
    int8_t last_zmode_decal = -1;
    D3D_PRIMITIVE_TOPOLOGY last_primitive_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    // Game loop callback
    
    void (*run_one_game_iter)(void);
} d3d;

HWND h_wnd;
LARGE_INTEGER last_time, frequency;

static void ThrowIfFailed(HRESULT res) {
    if (FAILED(res)) {
        throw res;
    }
}

static void SetDebugNameFormatted(ID3D11DeviceChild *device_child, char *format, uint32_t value) {
#if DEBUG_D3D
    char debug_name[128];
    int length = sprintf(debug_name, format, value);
    ThrowIfFailed(device_child->SetPrivateData(WKPDID_D3DDebugObjectName, length, debug_name));
#endif
}

void create_render_target_views(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    if (d3d.current_width == width && d3d.current_height == height) {
        return;
    }

    // Release previous stuff (if any)

    d3d.backbuffer_view.Reset();
    d3d.depth_stencil_view.Reset();

    // Resize swap chain

    ThrowIfFailed(d3d.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));

    // Create back buffer

    ComPtr<ID3D11Texture2D> backbuffer_texture;
    ThrowIfFailed(d3d.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backbuffer_texture));
    ThrowIfFailed(d3d.device->CreateRenderTargetView(backbuffer_texture.Get(), NULL, d3d.backbuffer_view.GetAddressOf()));

    // Create depth buffer

    D3D11_TEXTURE2D_DESC depth_stencil_texture_desc;
    ZeroMemory(&depth_stencil_texture_desc, sizeof(D3D11_TEXTURE2D_DESC));

    depth_stencil_texture_desc.Width = width;
    depth_stencil_texture_desc.Height = height;
    depth_stencil_texture_desc.MipLevels = 1;
    depth_stencil_texture_desc.ArraySize = 1;
    depth_stencil_texture_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_stencil_texture_desc.SampleDesc = d3d.sample_description;
    depth_stencil_texture_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_stencil_texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depth_stencil_texture_desc.CPUAccessFlags = 0;
    depth_stencil_texture_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> depth_stencil_texture;
    ThrowIfFailed(d3d.device->CreateTexture2D(&depth_stencil_texture_desc, NULL, depth_stencil_texture.GetAddressOf()));
    ThrowIfFailed(d3d.device->CreateDepthStencilView(depth_stencil_texture.Get(), NULL, d3d.depth_stencil_view.GetAddressOf()));

    // Set render targets

    d3d.context->OMSetRenderTargets(1, d3d.backbuffer_view.GetAddressOf(), d3d.depth_stencil_view.Get());

    // Save resolution

    d3d.current_width = width;
    d3d.current_height = height;
}

LRESULT CALLBACK gfx_d3d11_dxgi_wnd_proc(HWND h_wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_SIZE: {
            RECT rect;
            GetClientRect(h_wnd, &rect);
            create_render_target_views(rect.right - rect.left, rect.bottom - rect.top);
            break;
        }
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO lpMMI = (LPMINMAXINFO) l_param;
            lpMMI->ptMinTrackSize.x = 320;
            lpMMI->ptMinTrackSize.y = 240;
            break;
        }
        case WM_DESTROY: {
#if DEBUG_D3D
            d3d.debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
#endif
            exit(0);
        }
        case WM_ACTIVATEAPP: {
            keyboard_on_all_keys_up();
            break;
        }
        case WM_KEYDOWN: {
            keyboard_on_key_down((l_param >> 16) & 0x1ff);
            break;
        }
        case WM_KEYUP: {
            keyboard_on_key_up((l_param >> 16) & 0x1ff);
            break;
        }
        default: {
            return DefWindowProcW(h_wnd, message, w_param, l_param);
        }
    }
    return 0;
}

static void gfx_d3d11_dxgi_init(void) {

    // Create window
    WNDCLASSEXW wcex;
    ZeroMemory(&wcex, sizeof(WNDCLASSEX));

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = gfx_d3d11_dxgi_wnd_proc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = nullptr;
    wcex.hIcon = nullptr;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = WINCLASS_NAME;
    wcex.hIconSm = nullptr;

    RegisterClassExW(&wcex);

    RECT wr = { 0, 0, DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    h_wnd = CreateWindowW(WINCLASS_NAME, L"Super Mario 64 PC-Port (D3D11)", WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, 0, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr,
                          nullptr, nullptr);

    // Center window

    int xPos = (GetSystemMetrics(SM_CXSCREEN) - wr.right) * 0.5;
    int yPos = (GetSystemMetrics(SM_CYSCREEN) - wr.bottom) * 0.5;
    SetWindowPos(h_wnd, 0, xPos, yPos, 0, 0, SWP_NOZORDER | SWP_NOSIZE);

    // Sample description to be used in back buffer and depth buffer
    
    d3d.sample_description.Count = 1;
    d3d.sample_description.Quality = 0;

    // Create swap chain description

    DXGI_SWAP_CHAIN_DESC swap_chain_description;
    ZeroMemory(&swap_chain_description, sizeof(DXGI_SWAP_CHAIN_DESC));

    swap_chain_description.BufferCount = 1;
    swap_chain_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_description.BufferDesc.Height = DESIRED_SCREEN_HEIGHT;
    swap_chain_description.BufferDesc.Width = DESIRED_SCREEN_WIDTH;
    swap_chain_description.BufferDesc.RefreshRate.Numerator = 60; // 60Hz, maybe not force it?
    swap_chain_description.BufferDesc.RefreshRate.Denominator = 1;
    swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_description.OutputWindow = h_wnd;
    swap_chain_description.SampleDesc = d3d.sample_description;
    swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_description.Windowed = TRUE;

    // Create device and swap chain

#if DEBUG_D3D
    UINT device_creation_flags = D3D11_CREATE_DEVICE_DEBUG;
#else
    UINT device_creation_flags = 0;
#endif

    ThrowIfFailed(D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        device_creation_flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &swap_chain_description,
        d3d.swap_chain.GetAddressOf(),
        d3d.device.GetAddressOf(),
        nullptr,
        d3d.context.GetAddressOf()));

#if DEBUG_D3D
    ThrowIfFailed(d3d.device->QueryInterface(__uuidof(ID3D11Debug), (void **) (d3d.debug.GetAddressOf())));
#endif

    // Create views

    create_render_target_views(DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT);

    // Create main vertex buffer

    D3D11_BUFFER_DESC vertex_buffer_desc;
    ZeroMemory(&vertex_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    vertex_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    vertex_buffer_desc.ByteWidth = 256 * 26 * 3 * sizeof(float); // Same as buf_vbo size in gfx_pc
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertex_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertex_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(d3d.device->CreateBuffer(&vertex_buffer_desc, NULL, d3d.vertex_buffer.GetAddressOf()));

    // Create constant buffer

    D3D11_BUFFER_DESC constant_buffer_desc;
    ZeroMemory(&constant_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.ByteWidth = sizeof(PerFrameCB);
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(d3d.device->CreateBuffer(&constant_buffer_desc, NULL, d3d.per_frame_cb.GetAddressOf()));

    // Show the window

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last_time);

    ShowWindow(h_wnd, SW_SHOW);
    UpdateWindow(h_wnd);
}

static void gfx_d3d11_dxgi_main_loop(void (*run_one_game_iter)(void)) {
    MSG msg = { 0 };

    if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    } else {
        if (IsIconic(h_wnd)) {
            Sleep(1);
            return;
        }

        LARGE_INTEGER current_time, elapsed_time_microseconds;
        QueryPerformanceCounter(&current_time);
        elapsed_time_microseconds.QuadPart = current_time.QuadPart - last_time.QuadPart;
        elapsed_time_microseconds.QuadPart *= 1000000;
        elapsed_time_microseconds.QuadPart /= frequency.QuadPart;

        if (elapsed_time_microseconds.QuadPart < 33333.33333333) {
            return;
        }

        last_time = current_time;

        d3d.run_one_game_iter = run_one_game_iter;
        if (d3d.run_one_game_iter != nullptr) {
            d3d.run_one_game_iter();
        }
    }
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\r';
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "input.input1" : "input.input1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "input.input2" : "input.input2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "input.input3" : "input.input3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "input.input4" : "input.input4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texelVal0.a, texelVal0.a, texelVal0.a, texelVal0.a)" : "float3(texelVal0.a, texelVal0.a, texelVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1:
                return "input.input1.a";
            case SHADER_INPUT_2:
                return "input.input2.a";
            case SHADER_INPUT_3:
                return "input.input3.a";
            case SHADER_INPUT_4:
                return "input.input4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
        }
    }
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "lerp(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static void gfx_d3d11_dxgi_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = d3d.current_width;
    *height = d3d.current_height;
}

static void gfx_d3d11_dxgi_handle_events(void) {
    
}

static bool gfx_d3d11_dxgi_start_frame(void) {
    return true;
}

static void gfx_d3d11_dxgi_swap_buffers_begin(void) {
    d3d.swap_chain->Present(1, 0);
}

static void gfx_d3d11_dxgi_swap_buffers_end(void) {
}

double gfx_d3d11_dxgi_get_time(void) {
    return 0.0;
}

static bool gfx_d3d11_z_is_from_0_to_1(void) {
    return true;
}

static void gfx_d3d11_unload_shader(struct ShaderProgram *old_prg) {
}

static void gfx_d3d11_load_shader(struct ShaderProgram *new_prg) {
    d3d.shader_program = new_prg;
}

static struct ShaderProgram *gfx_d3d11_create_and_load_new_shader(uint32_t shader_id) {
    uint8_t c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }

    bool opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    bool opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    bool opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    bool opt_noise = (shader_id & SHADER_OPT_NOISE) != 0;
    
    bool used_textures[2] = {0, 0};
    int num_inputs = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > num_inputs) {
                    num_inputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                used_textures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                used_textures[1] = true;
            }
        }
    }
    bool do_single[2] = {c[0][2] == 0, c[1][2] == 0};
    bool do_multiply[2] = {c[0][1] == 0 && c[0][3] == 0, c[1][1] == 0 && c[1][3] == 0};
    bool do_mix[2] = {c[0][1] == c[0][3], c[1][1] == c[1][3]};
    bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);
    
    char buf[2048];
    size_t len = 0;
    size_t num_floats = 4;

    // Pixel shader input struct

    append_line(buf, &len, "struct PSInput {");
    append_line(buf, &len, "    float4 position : SV_POSITION;");

    if (used_textures[0] || used_textures[1]) {
        append_line(buf, &len, "    float2 uv : TEXCOORD;");
        num_floats += 2;
    }

    if (opt_fog) {
        append_line(buf, &len, "    float4 fog : FOG;");
        num_floats += 4;
    }
    for (int i = 0; i < num_inputs; i++) {
        len += sprintf(buf + len, "    float%d input%d : INPUT%d;\r\n", opt_alpha ? 4 : 3, i + 1, i);
        num_floats += opt_alpha ? 4 : 3;
    }
    append_line(buf, &len, "};");

    // Textures and samplers

    if (used_textures[0]) {
        append_line(buf, &len, "Texture2D g_texture0 : register(t0);");
        append_line(buf, &len, "SamplerState g_sampler0 : register(s0);");
    }
    if (used_textures[1]) {
        append_line(buf, &len, "Texture2D g_texture1 : register(t1);");
        append_line(buf, &len, "SamplerState g_sampler1 : register(s1);");
    }

    // Constant buffer and random function

    if (opt_alpha && opt_noise) {
        append_line(buf, &len, "cbuffer PerFrameCB : register(b0) {");
        append_line(buf, &len, "    uint frame_count;");
        append_line(buf, &len, "    uint window_height;");
        append_line(buf, &len, "}");

        append_line(buf, &len, "float random(in float3 value) {");
        append_line(buf, &len, "    float random = dot(sin(value), float3(12.9898, 78.233, 37.719));");
        append_line(buf, &len, "    return frac(sin(random) * 143758.5453);");
        append_line(buf, &len, "}");
    }

    // Vertex shader

    append_str(buf, &len, "PSInput VSMain(float4 position : POSITION");
    if (used_textures[0] || used_textures[1]) {
        append_str(buf, &len, ", float2 uv : TEXCOORD");
    }
    if (opt_fog) {
        append_str(buf, &len, ", float4 fog : FOG");
    }
    for (int i = 0; i < num_inputs; i++) {
        len += sprintf(buf + len, ", float%d input%d : INPUT%d", opt_alpha ? 4 : 3, i + 1, i);
    }
    append_line(buf, &len, ") {");
    append_line(buf, &len, "    PSInput result;");
    append_line(buf, &len, "    result.position = position;");
    if (used_textures[0] || used_textures[1]) {
        append_line(buf, &len, "    result.uv = uv;");
    }
    if (opt_fog) {
        append_line(buf, &len, "    result.fog = fog;");
    }
    for (int i = 0; i < num_inputs; i++) {
        len += sprintf(buf + len, "    result.input%d = input%d;\r\n", i + 1, i + 1);
    }
    append_line(buf, &len, "    return result;");
    append_line(buf, &len, "}");

    // Pixel shader

    append_line(buf, &len, "float4 PSMain(PSInput input, float4 screenSpace : SV_Position) : SV_TARGET {");
    if (used_textures[0]) {
        append_line(buf, &len, "    float4 texVal0 = g_texture0.Sample(g_sampler0, input.uv);");
    }
    if (used_textures[1]) {
        append_line(buf, &len, "    float4 texVal1 = g_texture1.Sample(g_sampler1, input.uv);");
    }
    
    append_str(buf, &len, opt_alpha ? "    float4 texel = " : "    float3 texel = ");
    if (!color_alpha_same && opt_alpha) {
        append_str(buf, &len, "float4(");
        append_formula(buf, &len, c, do_single[0], do_multiply[0], do_mix[0], false, false, true);
        append_str(buf, &len, ", ");
        append_formula(buf, &len, c, do_single[1], do_multiply[1], do_mix[1], true, true, true);
        append_str(buf, &len, ")");
    } else {
        append_formula(buf, &len, c, do_single[0], do_multiply[0], do_mix[0], opt_alpha, false, opt_alpha);
    }
    append_line(buf, &len, ";");
    
    if (opt_texture_edge && opt_alpha) {
        append_line(buf, &len, "    if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    // TODO discard if alpha is 0?
    if (opt_fog) {
        if (opt_alpha) {
            append_line(buf, &len, "    texel = float4(lerp(texel.rgb, input.fog.rgb, input.fog.a), texel.a);");
        } else {
            append_line(buf, &len, "    texel = lerp(texel, input.fog.rgb, input.fog.a);");
        }
    }

    if(opt_alpha && opt_noise) {
        append_line(buf, &len, "    texel.a *= round(random(float3(floor(screenSpace.xy * (240.0 / window_height)), frame_count)));");
    }
    
    if (opt_alpha) {
        append_line(buf, &len, "    return texel;");
    } else {
        append_line(buf, &len, "    return float4(texel, 1.0);");
    }
    append_line(buf, &len, "}");

    ComPtr<ID3DBlob> vs, ps;

#if DEBUG_D3D
    ThrowIfFailed(D3DCompile(buf, len, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_DEBUG, 0, vs.GetAddressOf(), nullptr));
    ThrowIfFailed(D3DCompile(buf, len, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", D3DCOMPILE_DEBUG, 0, ps.GetAddressOf(), nullptr));
#else
    ThrowIfFailed(D3DCompile(buf, len, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, vs.GetAddressOf(), nullptr));
    ThrowIfFailed(D3DCompile(buf, len, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, ps.GetAddressOf(), nullptr));
#endif

    struct ShaderProgram *prg = &d3d.shader_program_pool[d3d.shader_program_pool_size++];

    ThrowIfFailed(d3d.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), NULL, prg->vertex_shader.GetAddressOf()));
    ThrowIfFailed(d3d.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), NULL, prg->pixel_shader.GetAddressOf()));

    // Input Layout

    D3D11_INPUT_ELEMENT_DESC ied[7];
    uint8_t ied_index = 0;
    ied[ied_index++] = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (used_textures[0] || used_textures[1]) {
        ied[ied_index++] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }
    if (opt_fog) {
        ied[ied_index++] = { "FOG", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }
    for (int i = 0; i < num_inputs; i++) {
        DXGI_FORMAT format = opt_alpha ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
        ied[ied_index++] = { "INPUT", i, format, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }

    ThrowIfFailed(d3d.device->CreateInputLayout(ied, ied_index, vs->GetBufferPointer(), vs->GetBufferSize(), prg->input_layout.GetAddressOf()));

    // Blend state

    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(D3D11_BLEND_DESC));

    if (opt_alpha) {
        blend_desc.RenderTarget[0].BlendEnable = true;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    } else {
        blend_desc.RenderTarget[0].BlendEnable = false;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }

    ThrowIfFailed(d3d.device->CreateBlendState(&blend_desc, prg->blend_state.GetAddressOf()));

    // Save some values

    prg->shader_id = shader_id;
    prg->num_inputs = num_inputs;
    prg->num_floats = num_floats;
    prg->used_textures[0] = used_textures[0];
    prg->used_textures[1] = used_textures[1];

    return d3d.shader_program = prg;
}

static struct ShaderProgram *gfx_d3d11_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < d3d.shader_program_pool_size; i++) {
        if (d3d.shader_program_pool[i].shader_id == shader_id) {
            return &d3d.shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_d3d11_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gfx_d3d11_new_texture(void) {
    d3d.textures.resize(d3d.textures.size() + 1);
    return (uint32_t)(d3d.textures.size() - 1);
}

static void gfx_d3d11_select_texture(int tile, uint32_t texture_id) {
    d3d.current_tile = tile;
    d3d.current_texture_ids[tile] = texture_id;
}

static D3D11_TEXTURE_ADDRESS_MODE gfx_cm_to_d3d11(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return (val & G_TX_MIRROR) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
}

static void gfx_d3d11_upload_texture(uint8_t *rgba32_buf, int width, int height) {
    // Create texture
    
    D3D11_TEXTURE2D_DESC texture_desc;
    ZeroMemory(&texture_desc, sizeof(D3D11_TEXTURE2D_DESC));

    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0; // D3D11_RESOURCE_MISC_GENERATE_MIPS ?
    texture_desc.ArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;

    D3D11_SUBRESOURCE_DATA resource_data;
    resource_data.pSysMem = rgba32_buf;
    resource_data.SysMemPitch = width * 4;
    resource_data.SysMemSlicePitch = resource_data.SysMemPitch * height;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(d3d.device->CreateTexture2D(&texture_desc, &resource_data, texture.GetAddressOf()));

    // Create shader resource view from texture

    D3D11_SHADER_RESOURCE_VIEW_DESC resource_view_desc;
    ZeroMemory(&resource_view_desc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));

    resource_view_desc.Format = texture_desc.Format;
    resource_view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    resource_view_desc.Texture2D.MostDetailedMip = 0;
    resource_view_desc.Texture2D.MipLevels = -1;

    TextureData *texture_data = &d3d.textures[d3d.current_texture_ids[d3d.current_tile]];
    ThrowIfFailed(d3d.device->CreateShaderResourceView(texture.Get(), &resource_view_desc, texture_data->resource_view.GetAddressOf()));
}

static void gfx_d3d11_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    D3D11_SAMPLER_DESC sampler_desc;
    ZeroMemory(&sampler_desc, sizeof(D3D11_SAMPLER_DESC));

    sampler_desc.Filter = linear_filter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = gfx_cm_to_d3d11(cms);
    sampler_desc.AddressV = gfx_cm_to_d3d11(cmt);
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

    TextureData *texture_data = &d3d.textures[d3d.current_texture_ids[tile]];

    // This function is called twice per texture, the first one only to set default values.
    // Maybe that could be skipped? Anyway, make sure to release the first default sampler
    // state before setting the actual one.
    texture_data->sampler_state.Reset();

    ThrowIfFailed(d3d.device->CreateSamplerState(&sampler_desc, texture_data->sampler_state.GetAddressOf()));
}

static void gfx_d3d11_set_depth_test(bool depth_test) {
    d3d.depth_test = depth_test;
}

static void gfx_d3d11_set_depth_mask(bool depth_mask) {
    d3d.depth_mask = depth_mask;
}

static void gfx_d3d11_set_zmode_decal(bool zmode_decal) {
    d3d.zmode_decal = zmode_decal;
}

static void gfx_d3d11_set_viewport(int x, int y, int width, int height) {
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = x;
    viewport.TopLeftY = d3d.current_height - y - height;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    d3d.context->RSSetViewports(1, &viewport);
}

static void gfx_d3d11_set_scissor(int x, int y, int width, int height) {
    D3D11_RECT rect;
    rect.left = x;
    rect.top = d3d.current_height - y - height;
    rect.right = x + width;
    rect.bottom = d3d.current_height - y;

    d3d.context->RSSetScissorRects(1, &rect);
}

static void gfx_d3d11_set_use_alpha(bool use_alpha) {
    // Already part of the pipeline state from shader info
}

static void gfx_d3d11_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {

    if (d3d.last_depth_test != d3d.depth_test || d3d.last_depth_mask != d3d.depth_mask) {
        d3d.last_depth_test = d3d.depth_test;
        d3d.last_depth_mask = d3d.depth_mask;

        d3d.depth_stencil_state.Reset();

        D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
        ZeroMemory(&depth_stencil_desc, sizeof(D3D11_DEPTH_STENCIL_DESC));

        depth_stencil_desc.DepthEnable = d3d.depth_test;
        depth_stencil_desc.DepthWriteMask = d3d.depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_stencil_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        depth_stencil_desc.StencilEnable = false;

        ThrowIfFailed(d3d.device->CreateDepthStencilState(&depth_stencil_desc, d3d.depth_stencil_state.GetAddressOf()));
        d3d.context->OMSetDepthStencilState(d3d.depth_stencil_state.Get(), 0);
    }

    if (d3d.last_zmode_decal != d3d.zmode_decal) {
        d3d.last_zmode_decal = d3d.zmode_decal;

        d3d.rasterizer_state.Reset();

        D3D11_RASTERIZER_DESC rasterizer_desc;
        ZeroMemory(&rasterizer_desc, sizeof(D3D11_RASTERIZER_DESC));

        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.FrontCounterClockwise = true;
        rasterizer_desc.DepthBias = 0;
        rasterizer_desc.SlopeScaledDepthBias = d3d.zmode_decal ? -2.0f : 0.0f;
        rasterizer_desc.DepthBiasClamp = 0.0f;
        rasterizer_desc.DepthClipEnable = true;
        rasterizer_desc.ScissorEnable = true;
        rasterizer_desc.MultisampleEnable = false;
        rasterizer_desc.AntialiasedLineEnable = false;

        ThrowIfFailed(d3d.device->CreateRasterizerState(&rasterizer_desc, d3d.rasterizer_state.GetAddressOf()));
        d3d.context->RSSetState(d3d.rasterizer_state.Get());
    }

    for (int i = 0; i < 2; i++) {
        if (d3d.shader_program->used_textures[i]) {
            if (d3d.last_resource_views[i].Get() != d3d.textures[d3d.current_texture_ids[i]].resource_view.Get()) {
                d3d.last_resource_views[i] = d3d.textures[d3d.current_texture_ids[i]].resource_view.Get();
                d3d.context->PSSetShaderResources(i, 1, d3d.textures[d3d.current_texture_ids[i]].resource_view.GetAddressOf());

                if (d3d.last_sampler_states[i].Get() != d3d.textures[d3d.current_texture_ids[i]].sampler_state.Get()) {
                    d3d.last_sampler_states[i] = d3d.textures[d3d.current_texture_ids[i]].sampler_state.Get();
                    d3d.context->PSSetSamplers(i, 1, d3d.textures[d3d.current_texture_ids[i]].sampler_state.GetAddressOf());
                }
            }
        }
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    d3d.context->Map(d3d.vertex_buffer.Get(), NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms);
    memcpy(ms.pData, buf_vbo, buf_vbo_len * sizeof(float));
    d3d.context->Unmap(d3d.vertex_buffer.Get(), NULL);

    uint32_t stride = d3d.shader_program->num_floats * sizeof(float);
    uint32_t offset = 0;

    if (d3d.last_vertex_buffer_stride != stride) {
        d3d.last_vertex_buffer_stride = stride;
        d3d.context->IASetVertexBuffers(0, 1, d3d.vertex_buffer.GetAddressOf(), &stride, &offset);
    }

    if (d3d.last_shader_program != d3d.shader_program) {
        d3d.last_shader_program = d3d.shader_program;
        d3d.context->IASetInputLayout(d3d.shader_program->input_layout.Get());
        d3d.context->VSSetShader(d3d.shader_program->vertex_shader.Get(), 0, 0);
        d3d.context->PSSetShader(d3d.shader_program->pixel_shader.Get(), 0, 0);

        if (d3d.last_blend_state.Get() != d3d.shader_program->blend_state.Get()) {
            d3d.last_blend_state = d3d.shader_program->blend_state.Get();
            d3d.context->OMSetBlendState(d3d.shader_program->blend_state.Get(), 0, 0xFFFFFFFF);
        }
    }

    if (d3d.last_primitive_topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        d3d.last_primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        d3d.context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    d3d.context->Draw(buf_vbo_num_tris * 3, 0);
}

static void gfx_d3d11_init(void) {
}

static void gfx_d3d11_start_frame(void) {
    // Clear render targets

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    d3d.context->ClearRenderTargetView(d3d.backbuffer_view.Get(), clearColor);
    d3d.context->ClearDepthStencilView(d3d.depth_stencil_view.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Set per-frame constant buffer

    d3d.per_frame_cb_data.frame_count++;
    d3d.per_frame_cb_data.window_height = d3d.current_height;

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    d3d.context->Map(d3d.per_frame_cb.Get(), NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms);
    memcpy(ms.pData, &d3d.per_frame_cb_data, sizeof(PerFrameCB));
    d3d.context->Unmap(d3d.per_frame_cb.Get(), NULL);

    d3d.context->PSSetConstantBuffers(0, 1, d3d.per_frame_cb.GetAddressOf());
}

struct GfxRenderingAPI gfx_direct3d11_api = {
    gfx_d3d11_z_is_from_0_to_1,
    gfx_d3d11_unload_shader,
    gfx_d3d11_load_shader,
    gfx_d3d11_create_and_load_new_shader,
    gfx_d3d11_lookup_shader,
    gfx_d3d11_shader_get_info,
    gfx_d3d11_new_texture,
    gfx_d3d11_select_texture,
    gfx_d3d11_upload_texture,
    gfx_d3d11_set_sampler_parameters,
    gfx_d3d11_set_depth_test,
    gfx_d3d11_set_depth_mask,
    gfx_d3d11_set_zmode_decal,
    gfx_d3d11_set_viewport,
    gfx_d3d11_set_scissor,
    gfx_d3d11_set_use_alpha,
    gfx_d3d11_draw_triangles,
    gfx_d3d11_init,
    gfx_d3d11_start_frame
};

struct GfxWindowManagerAPI gfx_d3d11_dxgi_api = {
    gfx_d3d11_dxgi_init,
    gfx_d3d11_dxgi_main_loop,
    gfx_d3d11_dxgi_get_dimensions,
    gfx_d3d11_dxgi_handle_events,
    gfx_d3d11_dxgi_start_frame,
    gfx_d3d11_dxgi_swap_buffers_begin,
    gfx_d3d11_dxgi_swap_buffers_end,
    gfx_d3d11_dxgi_get_time,
};

#endif

#endif