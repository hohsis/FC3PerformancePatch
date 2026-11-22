/* Draws the in-game menu on a real Direct3D 11 device and checks the result.
 *
 *     fc3pp_overlay_shot [picture.bmp] [width height]
 *
 * It creates a device and a swap chain of its own (hardware, or the software
 * rasteriser where there is no graphics card), fills the back buffer with a
 * plain colour, sets a recognisable pipeline state, lets the menu draw as it
 * does inside the game's Present, and then checks that
 *   - the menu is there (dark panel, bright text) and the rest is untouched,
 *   - every piece of Direct3D state it had to change was put back.
 * The picture can be saved for a look. Exit code 77: no Direct3D 11 here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define COBJMACROS
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless unions in the SDK header */
#endif
#include <d3d11.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "config.h"
#include "menu.h"

static const GUID IID_Tex2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

static int g_checks, g_failures;

#define CHECK(cond, ...)             \
    do {                             \
        ++g_checks;                  \
        if (!(cond)) {               \
            ++g_failures;            \
            printf("  FAIL: ");      \
            printf(__VA_ARGS__);     \
            printf("\n");            \
        }                            \
    } while (0)

static void save_bmp(const char *path, const unsigned char *bgra, unsigned width, unsigned height, unsigned pitch)
{
    BITMAPFILEHEADER file;
    BITMAPINFOHEADER info;
    FILE *f = fopen(path, "wb");
    unsigned y;

    if (!f) {
        return;
    }
    memset(&file, 0, sizeof(file));
    memset(&info, 0, sizeof(info));
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = (LONG)width;
    info.biHeight = -(LONG)height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    fwrite(&file, sizeof(file), 1, f);
    fwrite(&info, sizeof(info), 1, f);
    for (y = 0; y < height; ++y) {
        fwrite(bgra + (size_t)y * pitch, 4, width, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static const FLOAT background[4] = {0.10f, 0.45f, 0.30f, 1.0f}; /* jungle green */
    const char *picture = argc > 1 ? argv[1] : NULL;
    UINT width = argc > 3 ? (UINT)atoi(argv[2]) : 1280, height = argc > 3 ? (UINT)atoi(argv[3]) : 720;
    DXGI_SWAP_CHAIN_DESC desc;
    IDXGISwapChain *chain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *back = NULL, *copy = NULL;
    ID3D11RenderTargetView *target = NULL, *target_after = NULL;
    ID3D11BlendState *blend = NULL, *blend_after = NULL;
    ID3D11RasterizerState *raster = NULL, *raster_after = NULL;
    D3D11_BLEND_DESC blend_desc;
    D3D11_RASTERIZER_DESC raster_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT viewport, viewport_after;
    D3D11_PRIMITIVE_TOPOLOGY topology_after = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    FLOAT factor_after[4];
    UINT mask_after = 0, viewports = 1;
    D3D_FEATURE_LEVEL level;
    fc3pp_config cfg;
    HWND window;
    HRESULT hr;
    int frame;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&mapped, 0, sizeof(mapped));
    window = CreateWindowExA(0, "STATIC", "fc3pp overlay test", WS_POPUP, 0, 0, (int)width, (int)height, NULL, NULL,
                             NULL, NULL);
    if (!window) {
        printf("SKIPPED: no window can be created here\n");
        return 77;
    }
    memset(&desc, 0, sizeof(desc));
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* what the game uses */
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &desc,
                                       &chain, &device, &level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &desc,
                                           &chain, &device, &level, &context);
    }
    if (FAILED(hr) || !chain) {
        printf("SKIPPED: no Direct3D 11 device (0x%08lX)\n", (unsigned long)hr);
        return 77;
    }

    /* The "game": a cleared back buffer and some state of its own. */
    IDXGISwapChain_GetBuffer(chain, 0, &IID_Tex2D, (void **)&back);
    ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &target);
    ID3D11DeviceContext_ClearRenderTargetView(context, target, background);
    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(device, &blend_desc, &blend);
    memset(&raster_desc, 0, sizeof(raster_desc));
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_FRONT;
    ID3D11Device_CreateRasterizerState(device, &raster_desc, &raster);
    memset(&viewport, 0, sizeof(viewport));
    viewport.TopLeftX = 11;
    viewport.TopLeftY = 13;
    viewport.Width = 320;
    viewport.Height = 200;
    viewport.MaxDepth = 1;
    {
        const FLOAT factor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &target, NULL);
        ID3D11DeviceContext_OMSetBlendState(context, blend, factor, 0x0F0F0F0Fu);
        ID3D11DeviceContext_RSSetState(context, raster);
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    }

    /* The menu, as it runs in the game: built from the configuration, opened, drawn at Present. */
    config_defaults(&cfg);
    cfg.menu_open_at_start = 1;
    menu_init(&cfg, NULL);
    menu_set_status(MENU_STATUS_PROCESSOR, "6 of 8 cores, 6 threads");
    menu_set_status(MENU_STATUS_MEMORY, "4 GB address space");
    CHECK(menu_is_open(), "the menu did not open");
    for (frame = 0; frame < 3; ++frame) {
        menu_on_present(chain);
    }
    CHECK(menu_is_open(), "the menu closed itself: it could not draw");

    /* State: exactly as the "game" left it. */
    ID3D11DeviceContext_OMGetRenderTargets(context, 1, &target_after, NULL);
    ID3D11DeviceContext_OMGetBlendState(context, &blend_after, factor_after, &mask_after);
    ID3D11DeviceContext_RSGetState(context, &raster_after);
    ID3D11DeviceContext_RSGetViewports(context, &viewports, &viewport_after);
    ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology_after);
    CHECK(target_after == target, "the render target was not put back");
    CHECK(blend_after == blend && mask_after == 0x0F0F0F0Fu && factor_after[1] == 0.5f,
          "the blend state was not put back");
    CHECK(raster_after == raster, "the rasteriser state was not put back");
    CHECK(viewports == 1 && viewport_after.TopLeftX == 11 && viewport_after.Width == 320,
          "the viewport was not put back");
    CHECK(topology_after == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "the primitive topology was not put back");

    /* Picture: read the back buffer. */
    ID3D11Texture2D_GetDesc(back, &texture_desc);
    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.BindFlags = 0;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(device, &texture_desc, NULL, &copy);
    CHECK(SUCCEEDED(hr), "no staging texture");
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)copy, (ID3D11Resource *)back);
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)copy, 0, D3D11_MAP_READ, 0, &mapped);
        CHECK(SUCCEEDED(hr), "the picture could not be read back");
    }
    if (SUCCEEDED(hr)) {
        const unsigned char *px = (const unsigned char *)mapped.pData;
        unsigned x, y, dark = 0, bright = 0, amber = 0, untouched = 0, right_half_changed = 0;
        const unsigned bg_b = 76, bg_g = 115, bg_r = 26; /* the clear colour as B8G8R8A8 */

        for (y = 0; y < height; ++y) {
            for (x = 0; x < width; ++x) {
                const unsigned char *p = px + (size_t)y * mapped.RowPitch + (size_t)x * 4;
                int is_bg = abs((int)p[0] - (int)bg_b) <= 2 && abs((int)p[1] - (int)bg_g) <= 2 &&
                            abs((int)p[2] - (int)bg_r) <= 2;
                untouched += is_bg;
                if (!is_bg && x > width * 3 / 4) {
                    ++right_half_changed;
                }
                if (p[0] < 50 && p[1] < 50 && p[2] < 50) {
                    ++dark;
                }
                if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
                    ++bright;
                }
                if (p[2] > 200 && p[1] > 130 && p[1] < 215 && p[0] < 120) {
                    ++amber;
                }
            }
        }
        printf("  %ux%u, feature level 0x%X: panel pixels %u, text pixels %u, accent pixels %u, untouched %u%%\n",
               width, height, (unsigned)level, dark, bright, amber, untouched * 100 / (width * height));
        CHECK(dark > width * height / 12, "no dark panel in the picture");
        CHECK(bright > 500, "no text in the picture");
        CHECK(amber > 100, "no accent colour (title, edge) in the picture");
        CHECK(untouched > width * height / 3, "the menu covers the game's picture");
        CHECK(right_half_changed == 0, "something was drawn in the right quarter of the screen");
        if (picture) {
            save_bmp(picture, px, width, height, mapped.RowPitch);
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)copy, 0);
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
