/* Direct3D 11 overlay renderer.
 *
 * Runs inside the game's Present call, on the game's own device. Everything
 * it changes on the device context is saved first and put back afterwards,
 * and nothing is kept that refers to the back buffer, so the engine can
 * resize or recreate its swap chain at any time.
 *
 * The font is rendered once with GDI into a one-channel texture: the system's
 * UI font where there is one (Windows), whatever Wine substitutes otherwise.
 * The two small shaders are compiled ahead of time and part of this DLL
 * (menu_shaders.h), so no shader compiler is needed where the game runs.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define COBJMACROS
#include "overlay.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless unions in the SDK header */
#endif
#include <d3d11.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <string.h>

#include "log.h"
#include "menu_shaders.h"

#define MAX_VERTICES 24576u /* 4096 rectangles */
#define ATLAS_WIDTH 1024
#define MIN_LINE 14.0f
#define MAX_LINE 96.0f

static const GUID IID_Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID IID_Device = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};

typedef struct gpu_vertex {
    float x, y, u, v;
    unsigned long color;
} gpu_vertex;

static struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vertices;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;
    ID3D11DepthStencilState *depth;
    ID3D11SamplerState *sampler;
    ID3D11ShaderResourceView *font_view;
    md_font font;
    float font_line;   /* the line height the font texture was built for */
    ID3D11RenderTargetView *target; /* only between begin and draw */
    float width, height;
    int failed;        /* do not try again: something this needs is missing */
} g;

/* ---- device objects -------------------------------------------------------------- */

#define RELEASE(p)              \
    do {                        \
        if (p) {                \
            (p)->lpVtbl->Release(p); \
            (p) = NULL;         \
        }                       \
    } while (0)

static void release_font(void)
{
    RELEASE(g.font_view);
    g.font_line = 0;
}

void overlay_release(void)
{
    RELEASE(g.target);
    release_font();
    RELEASE(g.sampler);
    RELEASE(g.depth);
    RELEASE(g.raster);
    RELEASE(g.blend);
    RELEASE(g.vertices);
    RELEASE(g.layout);
    RELEASE(g.ps);
    RELEASE(g.vs);
    RELEASE(g.context);
    RELEASE(g.device);
}

static int fail(const char *what, HRESULT hr)
{
    if (!g.failed) {
        LOGE("menu: cannot draw, %s (0x%08lX); the in-game menu is unavailable", what, (unsigned long)hr);
    }
    g.failed = 1;
    overlay_release();
    return 0;
}

static int create_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_BUFFER_DESC buffer;
    D3D11_BLEND_DESC blend;
    D3D11_RASTERIZER_DESC raster;
    D3D11_DEPTH_STENCIL_DESC depth;
    D3D11_SAMPLER_DESC sampler;
    HRESULT hr;

    /* The shaders are part of this DLL as compiled code (menu_shaders.h): the
     * game brings no shader compiler along, and under Wine there may be none
     * that loads. */
    hr = ID3D11Device_CreateVertexShader(g.device, MENU_VS_CODE, sizeof(MENU_VS_CODE), NULL, &g.vs);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(g.device, MENU_PS_CODE, sizeof(MENU_PS_CODE), NULL, &g.ps);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateInputLayout(g.device, elements, 3, MENU_VS_CODE, sizeof(MENU_VS_CODE), &g.layout);
    }
    if (FAILED(hr)) {
        return fail("the menu's shaders could not be created", hr);
    }

    memset(&buffer, 0, sizeof(buffer));
    buffer.ByteWidth = MAX_VERTICES * sizeof(gpu_vertex);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g.device, &buffer, NULL, &g.vertices);

    memset(&blend, 0, sizeof(blend));
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateBlendState(g.device, &blend, &g.blend);
    }

    memset(&raster, 0, sizeof(raster));
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRasterizerState(g.device, &raster, &g.raster);
    }

    memset(&depth, 0, sizeof(depth));
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateDepthStencilState(g.device, &depth, &g.depth);
    }

    memset(&sampler, 0, sizeof(sampler));
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateSamplerState(g.device, &sampler, &g.sampler);
    }
    return SUCCEEDED(hr) ? 1 : fail("the menu's Direct3D objects could not be created", hr);
}

/* ---- font ------------------------------------------------------------------------ */

/* Renders ASCII 32..126 with GDI and uploads the coverage as an R8 texture. */
static int create_font(float line_height)
{
    BITMAPINFO info;
    TEXTMETRICW metrics;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA data;
    ID3D11Texture2D *texture = NULL;
    HDC dc;
    HFONT font;
    HBITMAP bitmap;
    HGDIOBJ old_font, old_bitmap;
    void *bits = NULL;
    unsigned char *coverage;
    int cell_h, rows, atlas_h, x, y, c, i;
    int pixel_height = (int)(line_height * 0.80f + 0.5f);
    /* Room around each glyph: letters such as j and f reach beyond their own
     * width, the more the larger the font. */
    int pad = 2 + pixel_height / 5;
    SIZE extent[MD_GLYPHS];
    HRESULT hr;

    release_font();
    dc = CreateCompatibleDC(NULL);
    if (!dc) {
        return fail("no GDI device context for the menu font", E_FAIL);
    }
    font = CreateFontW(-pixel_height, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    old_font = SelectObject(dc, font);
    memset(&metrics, 0, sizeof(metrics));
    GetTextMetricsW(dc, &metrics);
    cell_h = metrics.tmHeight > 0 ? metrics.tmHeight : pixel_height + 4;

    /* First pass: measure, to know how many rows the texture needs. */
    rows = 1;
    x = 8; /* the first cells of the first row are the solid block */
    for (c = 0; c < MD_GLYPHS; ++c) {
        WCHAR ch = (WCHAR)(MD_FIRST_CHAR + c);
        if (!GetTextExtentPoint32W(dc, &ch, 1, &extent[c]) || extent[c].cx <= 0) {
            extent[c].cx = pixel_height / 2;
        }
        if (x + extent[c].cx + 2 * pad > ATLAS_WIDTH) {
            x = 0;
            ++rows;
        }
        x += extent[c].cx + 2 * pad;
    }
    atlas_h = rows * (cell_h + 1);

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = ATLAS_WIDTH;
    info.bmiHeader.biHeight = -atlas_h; /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    coverage = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)ATLAS_WIDTH * (SIZE_T)atlas_h);
    if (!bitmap || !bits || !coverage) {
        if (coverage) {
            HeapFree(GetProcessHeap(), 0, coverage);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return fail("no memory for the menu font", E_OUTOFMEMORY);
    }
    old_bitmap = SelectObject(dc, bitmap);
    memset(bits, 0, (size_t)ATLAS_WIDTH * (size_t)atlas_h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP);

    x = 8;
    y = 0;
    for (c = 0; c < MD_GLYPHS; ++c) {
        WCHAR ch = (WCHAR)(MD_FIRST_CHAR + c);
        md_glyph *glyph = &g.font.glyph[c];
        int cell_w = extent[c].cx + 2 * pad;
        if (x + cell_w > ATLAS_WIDTH) {
            x = 0;
            y += cell_h + 1;
        }
        {
            /* clipped to its cell, so that nothing of it lands in a neighbour's */
            RECT cell;
            cell.left = x;
            cell.top = y;
            cell.right = x + cell_w;
            cell.bottom = y + cell_h;
            ExtTextOutW(dc, x + pad, y, ETO_CLIPPED, &cell, &ch, 1, NULL);
        }
        glyph->u0 = (float)x / ATLAS_WIDTH;
        glyph->v0 = (float)y / (float)atlas_h;
        glyph->u1 = (float)(x + cell_w) / ATLAS_WIDTH;
        glyph->v1 = (float)(y + cell_h) / (float)atlas_h;
        glyph->w = (float)cell_w;
        glyph->h = (float)cell_h;
        glyph->x_offset = -(float)pad;
        glyph->advance = (float)extent[c].cx;
        x += cell_w;
    }
    GdiFlush();
    for (i = 0; i < ATLAS_WIDTH * atlas_h; ++i) {
        coverage[i] = ((const unsigned char *)bits)[i * 4 + 1]; /* green: grey-scale anti-aliasing */
    }
    for (y = 0; y < 4 && y < atlas_h; ++y) { /* an opaque block for plain rectangles */
        memset(coverage + (size_t)y * ATLAS_WIDTH, 0xFF, 4);
    }
    g.font.solid_u = 2.0f / ATLAS_WIDTH;
    g.font.solid_v = 2.0f / (float)atlas_h;
    g.font.line_height = (float)cell_h;

    SelectObject(dc, old_bitmap);
    SelectObject(dc, old_font);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);

    memset(&desc, 0, sizeof(desc));
    desc.Width = ATLAS_WIDTH;
    desc.Height = (UINT)atlas_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = coverage;
    data.SysMemPitch = ATLAS_WIDTH;
    data.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(g.device, &desc, &data, &texture);
    HeapFree(GetProcessHeap(), 0, coverage);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(g.device, (ID3D11Resource *)texture, NULL, &g.font_view);
    }
    RELEASE(texture);
    if (FAILED(hr)) {
        return fail("the menu font texture could not be created", hr);
    }
    g.font_line = line_height;
    return 1;
}

/* ---- per frame ------------------------------------------------------------------- */

static int g_scale_percent = 100;

void overlay_set_scale(int percent)
{
    g_scale_percent = percent < 100 ? 100 : percent > 200 ? 200 : percent;
}

int overlay_begin(void *swap_chain, float *width, float *height, const md_font **font)
{
    IDXGISwapChain *chain = (IDXGISwapChain *)swap_chain;
    ID3D11Device *device = NULL;
    ID3D11Texture2D *back = NULL;
    D3D11_TEXTURE2D_DESC desc;
    float line;
    HRESULT hr;

    if (g.failed || !chain) {
        return 0;
    }
    hr = IDXGISwapChain_GetDevice(chain, &IID_Device, (void **)&device);
    if (FAILED(hr) || !device) {
        return fail("the game is not drawing with Direct3D 11", hr);
    }
    if (device != g.device) { /* first use, or the engine made a new device */
        overlay_release();
        g.device = device;
        ID3D11Device_GetImmediateContext(device, &g.context);
        if (!g.context || !create_pipeline()) {
            return g.failed ? 0 : fail("no Direct3D device context", E_FAIL);
        }
    } else {
        ID3D11Device_Release(device);
    }

    hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_Texture2D, (void **)&back);
    if (FAILED(hr) || !back) {
        return fail("the back buffer is not available", hr);
    }
    ID3D11Texture2D_GetDesc(back, &desc);
    RELEASE(g.target);
    hr = ID3D11Device_CreateRenderTargetView(g.device, (ID3D11Resource *)back, NULL, &g.target);
    ID3D11Texture2D_Release(back);
    if (FAILED(hr) || !g.target) {
        return fail("the back buffer cannot be drawn to", hr);
    }
    g.width = (float)desc.Width;
    g.height = (float)desc.Height;

    line = g.height / 46.0f * (float)g_scale_percent / 100.0f;
    if (line < MIN_LINE) {
        line = MIN_LINE;
    }
    if (line > MAX_LINE) {
        line = MAX_LINE;
    }
    line = (float)(int)(line + 0.5f);
    if ((!g.font_view || line != g.font_line) && !create_font(line)) {
        return 0;
    }
    *width = g.width;
    *height = g.height;
    *font = &g.font;
    return 1;
}

/* What the engine had set, for the parts of the pipeline the overlay uses. */
typedef struct saved_state {
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *depth_view;
    UINT viewport_count, scissor_count;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState *raster;
    ID3D11BlendState *blend;
    FLOAT blend_factor[4];
    UINT sample_mask, stencil_ref;
    ID3D11DepthStencilState *depth;
    ID3D11ShaderResourceView *ps_resource;
    ID3D11SamplerState *ps_sampler;
    ID3D11PixelShader *ps;
    ID3D11VertexShader *vs;
    ID3D11GeometryShader *gs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11ClassInstance *ps_instances[256], *vs_instances[256], *gs_instances[256], *hs_instances[256],
        *ds_instances[256];
    UINT ps_instance_count, vs_instance_count, gs_instance_count, hs_instance_count, ds_instance_count;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11Buffer *index_buffer, *vertex_buffer;
    UINT index_offset, vertex_stride, vertex_offset;
    DXGI_FORMAT index_format;
    ID3D11InputLayout *layout;
} saved_state;

static saved_state g_saved; /* large; the overlay draws from one thread */

static void release_instances(ID3D11ClassInstance **list, UINT count)
{
    UINT i;
    for (i = 0; i < count && i < 256; ++i) {
        if (list[i]) {
            ID3D11ClassInstance_Release(list[i]);
        }
    }
}

static void save_state(ID3D11DeviceContext *c, saved_state *s)
{
    memset(s, 0, sizeof(*s));
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->targets, &s->depth_view);
    s->viewport_count = s->scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(c, &s->viewport_count, s->viewports);
    ID3D11DeviceContext_RSGetScissorRects(c, &s->scissor_count, s->scissors);
    ID3D11DeviceContext_RSGetState(c, &s->raster);
    ID3D11DeviceContext_OMGetBlendState(c, &s->blend, s->blend_factor, &s->sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s->depth, &s->stencil_ref);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &s->ps_resource);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 1, &s->ps_sampler);
    s->ps_instance_count = s->vs_instance_count = s->gs_instance_count = 256;
    s->hs_instance_count = s->ds_instance_count = 256;
    ID3D11DeviceContext_PSGetShader(c, &s->ps, s->ps_instances, &s->ps_instance_count);
    ID3D11DeviceContext_VSGetShader(c, &s->vs, s->vs_instances, &s->vs_instance_count);
    ID3D11DeviceContext_GSGetShader(c, &s->gs, s->gs_instances, &s->gs_instance_count);
    ID3D11DeviceContext_HSGetShader(c, &s->hs, s->hs_instances, &s->hs_instance_count);
    ID3D11DeviceContext_DSGetShader(c, &s->ds, s->ds_instances, &s->ds_instance_count);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s->topology);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &s->index_buffer, &s->index_format, &s->index_offset);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &s->vertex_buffer, &s->vertex_stride, &s->vertex_offset);
    ID3D11DeviceContext_IAGetInputLayout(c, &s->layout);
}

static void restore_state(ID3D11DeviceContext *c, saved_state *s)
{
    UINT i;

    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->targets, s->depth_view);
    ID3D11DeviceContext_RSSetViewports(c, s->viewport_count, s->viewports);
    ID3D11DeviceContext_RSSetScissorRects(c, s->scissor_count, s->scissors);
    ID3D11DeviceContext_RSSetState(c, s->raster);
    ID3D11DeviceContext_OMSetBlendState(c, s->blend, s->blend_factor, s->sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s->depth, s->stencil_ref);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &s->ps_resource);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &s->ps_sampler);
    ID3D11DeviceContext_PSSetShader(c, s->ps, s->ps_instances, s->ps_instance_count);
    ID3D11DeviceContext_VSSetShader(c, s->vs, s->vs_instances, s->vs_instance_count);
    ID3D11DeviceContext_GSSetShader(c, s->gs, s->gs_instances, s->gs_instance_count);
    ID3D11DeviceContext_HSSetShader(c, s->hs, s->hs_instances, s->hs_instance_count);
    ID3D11DeviceContext_DSSetShader(c, s->ds, s->ds_instances, s->ds_instance_count);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s->topology);
    ID3D11DeviceContext_IASetIndexBuffer(c, s->index_buffer, s->index_format, s->index_offset);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &s->vertex_buffer, &s->vertex_stride, &s->vertex_offset);
    ID3D11DeviceContext_IASetInputLayout(c, s->layout);

    /* The getters handed out references. */
    for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        RELEASE(s->targets[i]);
    }
    RELEASE(s->depth_view);
    RELEASE(s->raster);
    RELEASE(s->blend);
    RELEASE(s->depth);
    RELEASE(s->ps_resource);
    RELEASE(s->ps_sampler);
    RELEASE(s->ps);
    RELEASE(s->vs);
    RELEASE(s->gs);
    RELEASE(s->hs);
    RELEASE(s->ds);
    release_instances(s->ps_instances, s->ps_instance_count);
    release_instances(s->vs_instances, s->vs_instance_count);
    release_instances(s->gs_instances, s->gs_instance_count);
    release_instances(s->hs_instances, s->hs_instance_count);
    release_instances(s->ds_instances, s->ds_instance_count);
    RELEASE(s->index_buffer);
    RELEASE(s->vertex_buffer);
    RELEASE(s->layout);
}

void overlay_draw(const md_list *list)
{
    static const FLOAT no_factor[4] = {0, 0, 0, 0};
    ID3D11DeviceContext *c = g.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT viewport;
    D3D11_RECT scissor;
    gpu_vertex *out;
    UINT stride = sizeof(gpu_vertex), offset = 0, count, i;

    if (!c || !g.target || !list || !list->count) {
        RELEASE(g.target);
        return;
    }
    count = list->count < MAX_VERTICES ? list->count : MAX_VERTICES;
    count -= count % 6;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)g.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        RELEASE(g.target);
        return;
    }
    out = (gpu_vertex *)mapped.pData;
    for (i = 0; i < count; ++i) {
        out[i].x = list->v[i].x / g.width * 2.0f - 1.0f;
        out[i].y = 1.0f - list->v[i].y / g.height * 2.0f;
        out[i].u = list->v[i].u;
        out[i].v = list->v[i].v;
        out[i].color = list->v[i].color;
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)g.vertices, 0);

    save_state(c, &g_saved);

    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = g.width;
    viewport.Height = g.height;
    viewport.MaxDepth = 1.0f;
    scissor.left = scissor.top = 0;
    scissor.right = (LONG)g.width;
    scissor.bottom = (LONG)g.height;
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g.target, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
    ID3D11DeviceContext_RSSetScissorRects(c, 1, &scissor);
    ID3D11DeviceContext_RSSetState(c, g.raster);
    ID3D11DeviceContext_OMSetBlendState(c, g.blend, no_factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g.depth, 0);
    ID3D11DeviceContext_IASetInputLayout(c, g.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g.vertices, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(c, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g.ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g.font_view);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &g.sampler);
    ID3D11DeviceContext_Draw(c, count, 0);

    restore_state(c, &g_saved);
    RELEASE(g.target); /* nothing of ours may hold on to the back buffer */
}
