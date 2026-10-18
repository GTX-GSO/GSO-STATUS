/* Controlled ETW validation target. Not a game or a production component. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

int main(void) {
    WNDCLASSW klass = {0};
    klass.hInstance = GetModuleHandleW(NULL);
    klass.lpfnWndProc = window_proc;
    klass.lpszClassName = L"GSOStatusPresentFixture";
    if (!RegisterClassW(&klass)) return 1;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        klass.lpszClassName, L"GSO STATUS · D3D test", WS_POPUP,
        10, 10, 128, 72, NULL, NULL, klass.hInstance, NULL);
    if (!window) return 2;
    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferDesc.Width = 128;
    desc.BufferDesc.Height = 72;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swap = NULL;
    ID3D11Texture2D *buffer = NULL;
    ID3D11RenderTargetView *view = NULL;
    int result = 3;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        0, NULL, 0, D3D11_SDK_VERSION, &desc, &swap, &device, NULL, &context);
    if (FAILED(hr)) goto cleanup;
    hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&buffer);
    if (FAILED(hr)) goto cleanup;
    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)buffer, NULL, &view);
    if (FAILED(hr)) goto cleanup;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ULONGLONG started = GetTickCount64();
    unsigned presents = 0;
    while (GetTickCount64() - started < 8000) {
        MSG message;
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const FLOAT color[4] = {0.03f, 0.08f, 0.15f, 1.0f};
        ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
        hr = IDXGISwapChain_Present(swap, 1, 0);
        if (FAILED(hr)) goto cleanup;
        if (hr == S_OK) ++presents;
        Sleep(15);
    }
    result = presents ? 0 : 4;
cleanup:
    if (view) ID3D11RenderTargetView_Release(view);
    if (buffer) ID3D11Texture2D_Release(buffer);
    if (swap) IDXGISwapChain_Release(swap);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, klass.hInstance);
    return result;
}
