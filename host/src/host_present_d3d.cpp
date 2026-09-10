// ---------------------------------------------------------------------------
// The picture, presented through a flip-model swap chain.
//
// WHY THIS EXISTS, when StretchDIBits already worked.
//
// A GDI window is composited by DWM: the blit lands in a redirection surface and
// the desktop compositor shows it at ITS next refresh. That costs one to two
// whole frames before anything reaches the glass, and nothing in the game can
// see it -- every frame timer read flat while the build felt a third of a second
// behind the board. The device writes its panel directly and has none of it.
//
// A flip-model swap chain hands the buffer to the display path instead of to a
// redirection surface. With the frame latency pinned to one, that is about one
// frame of delay rather than three.
//
// It is also the pacer now. Present(1) returns when the display has taken the
// frame, which is the one clock that matters and the one the spin loop in
// host_window.cpp could never quite hit -- it aimed at 16667 us against a panel
// running 16666, and the two beat once a second.
//
// EVERY FAILURE FALLS BACK. A machine with no D3D11, a driver that will not make
// a flip chain, a lost device: host_window.cpp keeps the GDI path and uses it
// whenever ready() is false. The game is playable either way; this is the
// difference between playable and pleasant.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <stdio.h>

#include "host_present_d3d.h"


static ID3D11Device*           s_dev   = nullptr;
static ID3D11DeviceContext*    s_ctx   = nullptr;
static IDXGISwapChain1*        s_chain = nullptr;
static ID3D11RenderTargetView* s_rtv   = nullptr;
static ID3D11Texture2D*        s_tex   = nullptr;   // the panel, uploaded each frame
static ID3D11ShaderResourceView* s_srv = nullptr;
static ID3D11VertexShader*     s_vs    = nullptr;
static ID3D11PixelShader*      s_ps    = nullptr;
static ID3D11SamplerState*     s_samp  = nullptr;
static HWND                    s_hwnd  = nullptr;
static int                     s_bw = 0, s_bh = 0;   // current back buffer size
static bool                    s_ready = false;

// A full-screen triangle and a texture read. The triangle is generated from the
// vertex id, so there is no vertex buffer and no input layout: the viewport does
// the letterboxing and the clear paints the bars.
//
// POINT SAMPLED, deliberately. The picture is 480x480 of one-pixel lines drawn
// for a panel; smoothing it turns the wireframe into grey mush. This is the same
// decision as SetStretchBltMode(COLORONCOLOR) in the GDI path.
static const char* HLSL =
    "Texture2D tx : register(t0);\n"
    "SamplerState sm : register(s0);\n"
    "struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VO vsmain(uint id : SV_VertexID) {\n"
    "  VO o;\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = p;\n"
    "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o;\n"
    "}\n"
    "float4 psmain(VO i) : SV_TARGET { return tx.Sample(sm, i.uv); }\n";

static void release_targets(void) {
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
}

static bool make_targets(void) {
    release_targets();
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    const HRESULT hr = s_dev->CreateRenderTargetView(back, nullptr, &s_rtv);
    D3D11_TEXTURE2D_DESC d = {};
    back->GetDesc(&d);
    s_bw = (int)d.Width;
    s_bh = (int)d.Height;
    back->Release();
    return SUCCEEDED(hr);
}

bool host_d3d_init(HWND hwnd, int src_w, int src_h) {
    if (!hwnd) return false;
    s_hwnd = hwnd;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                       D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 want, (UINT)(sizeof(want) / sizeof(want[0])),
                                 D3D11_SDK_VERSION, &s_dev, &got, &s_ctx))) {
        printf("d3d: no device, falling back to GDI\n");
        return false;
    }

    IDXGIDevice1* dxdev = nullptr;
    IDXGIAdapter* adapt = nullptr;
    IDXGIFactory2* fact = nullptr;
    if (SUCCEEDED(s_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev))) {
        // ONE FRAME IN FLIGHT. The default is three, and each one is a frame of
        // delay between the stick moving and the picture saying so.
        dxdev->SetMaximumFrameLatency(1);
        if (SUCCEEDED(dxdev->GetAdapter(&adapt)))
            adapt->GetParent(__uuidof(IDXGIFactory2), (void**)&fact);
    }
    if (!fact) {
        printf("d3d: no DXGI 1.2 factory, falling back to GDI\n");
        goto fail;
    }

    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width  = 0;                                  // take the client size
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;         // the panel's own order
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling     = DXGI_SCALING_NONE;             // the viewport letterboxes
        sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD; // the whole point
        sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(fact->CreateSwapChainForHwnd(s_dev, s_hwnd, &sd, nullptr, nullptr,
                                                &s_chain))) {
            printf("d3d: no flip swap chain, falling back to GDI\n");
            goto fail;
        }
        // The game answers its own keys; DXGI must not eat alt+enter.
        fact->MakeWindowAssociation(s_hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    {
        ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(HLSL, strlen(HLSL), nullptr, nullptr, nullptr,
                              "vsmain", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(HLSL, strlen(HLSL), nullptr, nullptr, nullptr,
                              "psmain", "ps_4_0", 0, 0, &psb, &err))) {
            if (err) { printf("d3d: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
            if (vsb) vsb->Release();
            if (psb) psb->Release();
            goto fail;
        }
        const bool ok =
            SUCCEEDED(s_dev->CreateVertexShader(vsb->GetBufferPointer(),
                                                vsb->GetBufferSize(), nullptr, &s_vs)) &&
            SUCCEEDED(s_dev->CreatePixelShader(psb->GetBufferPointer(),
                                               psb->GetBufferSize(), nullptr, &s_ps));
        vsb->Release();
        psb->Release();
        if (!ok) goto fail;
    }

    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width  = (UINT)src_w;
        td.Height = (UINT)src_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;                 // written every frame
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_dev->CreateTexture2D(&td, nullptr, &s_tex))) goto fail;
        if (FAILED(s_dev->CreateShaderResourceView(s_tex, nullptr, &s_srv))) goto fail;

        D3D11_SAMPLER_DESC sm = {};
        sm.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;   // no smoothing: 1 px lines
        sm.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(s_dev->CreateSamplerState(&sm, &s_samp))) goto fail;
    }

    if (!make_targets()) goto fail;

    if (adapt) adapt->Release();
    if (dxdev) dxdev->Release();
    if (fact)  fact->Release();
    s_ready = true;
    printf("d3d: flip-model swap chain, frame latency 1\n");
    return true;

fail:
    if (adapt) adapt->Release();
    if (dxdev) dxdev->Release();
    if (fact)  fact->Release();
    host_d3d_shutdown();
    return false;
}

bool host_d3d_ready(void) { return s_ready; }

bool host_d3d_present(const uint32_t* bgra, int src_w, int src_h,
                      int vx, int vy, int vw, int vh) {
    if (!s_ready || !bgra) return false;

    // The window changed size: give the chain new buffers before drawing into it.
    RECT cr;
    if (GetClientRect(s_hwnd, &cr)) {
        const int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw > 0 && ch > 0 && (cw != s_bw || ch != s_bh)) {
            release_targets();
            if (FAILED(s_chain->ResizeBuffers(0, (UINT)cw, (UINT)ch,
                                              DXGI_FORMAT_UNKNOWN, 0))) return false;
            if (!make_targets()) return false;
        }
    }
    if (!s_rtv) return false;

    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(s_ctx->Map(s_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        // ROW BY ROW. The driver picks its own pitch and it is rarely the width.
        const uint8_t* src = (const uint8_t*)bgra;
        uint8_t* dst = (uint8_t*)m.pData;
        const size_t row = (size_t)src_w * 4;
        for (int y = 0; y < src_h; y++) {
            memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * row, row);
        }
        s_ctx->Unmap(s_tex, 0);
    }

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
    s_ctx->ClearRenderTargetView(s_rtv, black);        // this is the letterbox

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (float)vx;
    vp.TopLeftY = (float)vy;
    vp.Width    = (float)vw;
    vp.Height   = (float)vh;
    vp.MaxDepth = 1.0f;
    s_ctx->RSSetViewports(1, &vp);

    s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_ctx->VSSetShader(s_vs, nullptr, 0);
    s_ctx->PSSetShader(s_ps, nullptr, 0);
    s_ctx->PSSetShaderResources(0, 1, &s_srv);
    s_ctx->PSSetSamplers(0, 1, &s_samp);
    s_ctx->Draw(3, 0);

    // ONE, NOT ZERO. This blocks until the display has taken the frame, which is
    // what makes it the pacer: see the note at the top and the spin loop in
    // host_window.cpp that this replaces.
    const HRESULT hr = s_chain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        printf("d3d: device lost, falling back to GDI\n");
        host_d3d_shutdown();
        return false;
    }
    return SUCCEEDED(hr);
}

void host_d3d_shutdown(void) {
    s_ready = false;
    if (s_samp)  { s_samp->Release();  s_samp  = nullptr; }
    if (s_srv)   { s_srv->Release();   s_srv   = nullptr; }
    if (s_tex)   { s_tex->Release();   s_tex   = nullptr; }
    if (s_ps)    { s_ps->Release();    s_ps    = nullptr; }
    if (s_vs)    { s_vs->Release();    s_vs    = nullptr; }
    release_targets();
    if (s_chain) { s_chain->Release(); s_chain = nullptr; }
    if (s_ctx)   { s_ctx->ClearState(); s_ctx->Release(); s_ctx = nullptr; }
    if (s_dev)   { s_dev->Release();   s_dev   = nullptr; }
}
