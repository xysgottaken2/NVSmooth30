// nvs30_probe: minimal D3D11 swapchain harness for the RTX 2060 smoke test.
//
// Run it with the freshly built version.dll in the same directory. It
// exercises exactly the pipeline NVSmooth30 touches -- swapchain creation,
// the shared DXGI vtable Present path, and (with NVPI configured) the
// NvPresent64 attach -- for a fixed frame budget, then exits with code 0 on
// a clean run.
//
//   nvs30_probe [--frames N] [--width W] [--height H] [--vsync 0|1]
//
// No NVIDIA-specific APIs are used here; all validation results are read
// from nvsmooth30.log written next to the executable by version.dll.

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

namespace {
struct Options {
    long frames = 600;
    long width = 1280;
    long height = 720;
    long vsync = 1;
};

HWND window{};
bool running = true;

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) {
        running = false;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool parse(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (i + 1 >= argc) return false;
        long* dest = nullptr;
        if (a == L"--frames") dest = &o.frames;
        else if (a == L"--width") dest = &o.width;
        else if (a == L"--height") dest = &o.height;
        else if (a == L"--vsync") dest = &o.vsync;
        else return false;
        *dest = _wtoi(argv[++i]);
        if (*dest <= 0 && a != L"--vsync") return false;
    }
    return true;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    Options opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool parsed = argc <= 1 || parse(argc, argv, opt);
    LocalFree(argv);
    if (!parsed) {
        wprintf(L"usage: nvs30_probe [--frames N] [--width W] [--height H] [--vsync 0|1]\n");
        return 2;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"NVSmooth30Probe";
    RegisterClassW(&wc);
    window = CreateWindowW(wc.lpszClassName, L"NVSmooth30 probe (close to stop)",
                           WS_OVERLAPPEDWINDOW, 80, 80,
                           static_cast<int>(opt.width) + 16,
                           static_cast<int>(opt.height) + 39,
                           nullptr, nullptr, instance, nullptr);
    if (!window) {
        wprintf(L"CreateWindow failed: %lu\n", GetLastError());
        return 3;
    }
    ShowWindow(window, SW_SHOW);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE driver : drivers) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = static_cast<UINT>(opt.width);
        desc.BufferDesc.Height = static_cast<UINT>(opt.height);
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, nullptr, 0,
                                           D3D11_SDK_VERSION, &desc, &swapchain, &device,
                                           nullptr, &context);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr)) {
        wprintf(L"D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 4;
    }
    wprintf(L"nvs30_probe: device+swapchain OK, running %ld frames at %ldx%ld vsync=%ld\n",
            opt.frames, opt.width, opt.height, opt.vsync);

    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&backbuffer));
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    if (FAILED(hr)) {
        wprintf(L"render target setup failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 5;
    }

    LARGE_INTEGER freq{}, start{}, tick{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    long presented = 0;
    while (presented < opt.frames && running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const float t = static_cast<float>(presented) * 0.01f;
        const float clear[4] = {0.25f + 0.25f * t, 0.45f, 0.65f - 0.25f * t, 1.0f};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        const HRESULT present_hr = swapchain->Present(static_cast<UINT>(opt.vsync), 0);
        if (FAILED(present_hr)) {
            wprintf(L"Present failed at frame %ld: 0x%08X\n", presented,
                    static_cast<unsigned>(present_hr));
            return 6;
        }
        ++presented;
        QueryPerformanceCounter(&tick);
        if ((presented & 127) == 0) {
            const double seconds = double(tick.QuadPart - start.QuadPart) / double(freq.QuadPart);
            wprintf(L"  frame %ld/%ld, %.1f fps\n", presented, opt.frames, presented / seconds);
        }
    }

    const double seconds = double(tick.QuadPart - start.QuadPart) / double(freq.QuadPart);
    wprintf(L"nvs30_probe: %ld frames presented in %.2f s (%.1f fps), closing\n",
            presented, seconds, presented / (seconds > 0 ? seconds : 1));
    if (rtv) rtv->Release();
    if (backbuffer) backbuffer->Release();
    if (swapchain) swapchain->Release();
    if (context) context->Release();
    if (device) device->Release();
    return presented > 0 ? 0 : 7;
}
